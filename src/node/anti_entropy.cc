#include "node/anti_entropy.h"

#include <algorithm>
#include <set>

#include "common/logging.h"
#include "ring/hash_ring.h"

namespace dkv {

void RepairStats::add(const RepairStats& o) {
  ranges_compared += o.ranges_compared;
  ranges_differing += o.ranges_differing;
  keys_fetched += o.keys_fetched;
  versions_stored += o.versions_stored;
  bytes_fetched += o.bytes_fetched;
  rpcs += o.rpcs;
  ok = ok && o.ok;
}

AntiEntropy::AntiEntropy(std::string self, uint64_t incarnation, int64_t process_start_wall_ms, Options opts,
                         MerkleForest& forest, StorageEngine& engine, PeerClientPool& peers,
                         FailureDetector& fd, AdminReporter* admin, Clock& clock, EventLog* events)
    : self_(std::move(self)), incarnation_(incarnation), start_wall_ms_(process_start_wall_ms), opts_(opts),
      forest_(forest), engine_(engine), peers_(peers), fd_(fd), admin_(admin), clock_(clock),
      events_(events) {}

AntiEntropy::~AntiEntropy() { stop(); }

void AntiEntropy::start() { thread_ = std::thread([this] { run(); }); }

void AntiEntropy::stop() {
  {
    std::lock_guard<std::mutex> l(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

bool AntiEntropy::wait_or_stop(int64_t ms) {
  std::unique_lock<std::mutex> l(mu_);
  return cv_.wait_for(l, std::chrono::milliseconds(ms), [this] { return stopping_; });
}

std::map<std::string, std::vector<uint32_t>> AntiEntropy::shared_ranges() const {
  std::map<std::string, std::vector<uint32_t>> out;
  auto ring = forest_.ring();
  if (!ring) return out;
  for (uint32_t r : forest_.ranges())
    for (const auto& p : ring->replicas_of_range(r))
      if (p != self_) out[p].push_back(r);
  return out;
}

void AntiEntropy::run() {
  startup_pass();
  // Periodic pass: one peer per interval, round-robin, all shared ranges (cheap when in sync).
  size_t next = 0;
  while (!wait_or_stop(int64_t(opts_.interval_s) * 1000)) {
    auto shared = shared_ranges();
    std::vector<std::string> live;
    for (const auto& kv : shared)
      if (!fd_.is_down(kv.first)) live.push_back(kv.first);
    if (live.empty()) continue;
    const std::string& peer = live[next++ % live.size()];
    RepairStats s = sync_with(peer, shared[peer]);
    if (s.keys_fetched > 0 || !s.ok) {
      DKV_LOG(INFO) << "periodic anti-entropy with " << peer << ": " << s.ranges_differing
                    << " ranges differ, " << s.versions_stored << " versions stored";
      if (events_)
        events_->emit(Ev("ae.periodic")
                          .str("peer", peer)
                          .u64("ranges_differing", s.ranges_differing)
                          .u64("keys_fetched", s.keys_fetched)
                          .u64("versions_stored", s.versions_stored)
                          .boolean("ok", s.ok));
    }
  }
}

void AntiEntropy::startup_pass() {
  auto shared = shared_ranges();
  // 1. Wait until every reachable peer has seen this incarnation, so every new write includes us;
  //    then let in-flight writes that skipped us finish (coord timeout). LLD §7.6.
  const int64_t wait_start = clock_.now_mono_ms();
  for (;;) {
    bool all = true;
    for (const auto& kv : shared)
      if (!fd_.is_down(kv.first) && !fd_.peer_acked_incarnation(kv.first)) all = false;
    if (all || clock_.now_mono_ms() - wait_start > opts_.wait_peers_max_ms) break;
    if (wait_or_stop(50)) return;
  }
  if (wait_or_stop(opts_.coord_timeout_ms)) return;

  const int64_t repair_start = clock_.now_wall_ms();
  DKV_LOG(INFO) << "anti-entropy startup pass: " << forest_.ranges().size() << " ranges, "
                << shared.size() << " peers";
  if (events_)
    events_->emit(Ev("repair.started")
                      .u64("incarnation", incarnation_)
                      .u64("ranges", forest_.ranges().size())
                      .i64("process_start_wall_ms", start_wall_ms_)
                      .i64("repair_start_wall_ms", repair_start));
  if (admin_) {
    wire::RepairReport r;
    r.set_node(self_);
    r.set_incarnation(incarnation_);
    r.set_phase(wire::RepairReport::STARTED);
    r.set_process_start_wall_ms(start_wall_ms_);
    r.set_repair_start_wall_ms(repair_start);
    r.set_report_wall_ms(repair_start);
    r.set_ranges_total(forest_.ranges().size());
    admin_->report_repair(r);
  }

  // 2. One full pull pass against every healthy peer.
  RepairStats total;
  uint32_t peers_synced = 0;
  for (const auto& kv : shared) {
    {
      std::lock_guard<std::mutex> l(mu_);
      if (stopping_) return;
    }
    if (fd_.is_down(kv.first)) continue;
    RepairStats s = sync_with(kv.first, kv.second);
    total.add(s);
    if (s.ok) ++peers_synced;
  }
  const int64_t done = clock_.now_wall_ms();
  startup_done_ = true;
  DKV_LOG(INFO) << "anti-entropy startup pass done in " << (done - repair_start) << " ms (process up "
                << (done - start_wall_ms_) << " ms): " << total.ranges_differing << " ranges differed, "
                << total.versions_stored << " versions stored from " << peers_synced << " peers";
  if (events_)
    events_->emit(Ev("repair.done")
                      .u64("incarnation", incarnation_)
                      .i64("process_start_wall_ms", start_wall_ms_)
                      .i64("repair_start_wall_ms", repair_start)
                      .i64("done_wall_ms", done)
                      .i64("repair_ms_from_start", done - start_wall_ms_)
                      .i64("repair_ms_pass", done - repair_start)
                      .u64("ranges_compared", total.ranges_compared)
                      .u64("ranges_differing", total.ranges_differing)
                      .u64("keys_fetched", total.keys_fetched)
                      .u64("versions_stored", total.versions_stored)
                      .u64("bytes_fetched", total.bytes_fetched)
                      .u64("rpcs", total.rpcs)
                      .u64("peers_synced", peers_synced)
                      .boolean("initial_sync", incarnation_ == 1));
  if (admin_) {
    wire::RepairReport r;
    r.set_node(self_);
    r.set_incarnation(incarnation_);
    r.set_phase(wire::RepairReport::DONE);
    r.set_process_start_wall_ms(start_wall_ms_);
    r.set_repair_start_wall_ms(repair_start);
    r.set_report_wall_ms(done);
    r.set_ranges_total(forest_.ranges().size());
    r.set_ranges_done(forest_.ranges().size());
    r.set_keys_fetched(total.keys_fetched);
    r.set_bytes_fetched(total.bytes_fetched);
    admin_->report_repair(r);
  }
}

RepairStats AntiEntropy::sync_with(const std::string& peer, const std::vector<uint32_t>& ranges) {
  RepairStats s;
  constexpr size_t kBatch = 512;
  // Breadth-first descent: compare roots of all ranges, then children of differing inner nodes.
  std::vector<wire::MerkleNodeRef> todo;
  for (uint32_t r : ranges) {
    wire::MerkleNodeRef ref;
    ref.set_range_id(r);
    ref.set_node_index(0);
    todo.push_back(ref);
  }
  s.ranges_compared = ranges.size();
  std::map<uint32_t, std::vector<uint32_t>> diff_leaves;  // range -> leaf indices
  std::set<uint32_t> differing_ranges;
  while (!todo.empty()) {
    std::vector<wire::MerkleNodeRef> next;
    for (size_t i = 0; i < todo.size(); i += kBatch) {
      wire::MerkleRequest req;
      req.set_from(self_);
      size_t end = std::min(todo.size(), i + kBatch);
      for (size_t j = i; j < end; ++j) *req.add_nodes() = todo[j];
      wire::MerkleResponse resp;
      ++s.rpcs;
      grpc::Status st = peers_.call(peer, &wire::ReplicaService::Stub::GetMerkleNodes, req, &resp,
                                    opts_.rpc_timeout_ms);
      if (!st.ok() || resp.hashes_size() != req.nodes_size()) {
        DKV_LOG(WARN) << "anti-entropy: GetMerkleNodes from " << peer << " failed: " << st.error_message();
        s.ok = false;
        return s;
      }
      for (int j = 0; j < req.nodes_size(); ++j) {
        const auto& ref = req.nodes(j);
        const std::string& theirs = resp.hashes(j);
        if (theirs.empty()) continue;  // peer does not replicate this range
        auto mine = forest_.node_hash(ref.range_id(), ref.node_index());
        if (!mine || DigestToBytes(*mine) == theirs) continue;
        differing_ranges.insert(ref.range_id());
        if (MerkleTree::is_leaf(ref.node_index())) {
          diff_leaves[ref.range_id()].push_back(MerkleTree::heap_to_leaf(ref.node_index()));
        } else {
          for (uint32_t c : MerkleTree::children(ref.node_index())) {
            wire::MerkleNodeRef child;
            child.set_range_id(ref.range_id());
            child.set_node_index(c);
            next.push_back(child);
          }
        }
      }
    }
    todo.swap(next);
  }
  s.ranges_differing = differing_ranges.size();

  // Leaf contents -> keys where the peer has a version digest we do not have.
  std::vector<std::string> need;
  for (const auto& kv : diff_leaves) {
    wire::BucketRequest req;
    req.set_from(self_);
    req.set_range_id(kv.first);
    for (uint32_t leaf : kv.second) req.add_leaves(leaf);
    wire::BucketResponse resp;
    ++s.rpcs;
    grpc::Status st = peers_.call(peer, &wire::ReplicaService::Stub::GetBucketDigests, req, &resp,
                                  opts_.rpc_timeout_ms);
    if (!st.ok()) {
      s.ok = false;
      return s;
    }
    for (const auto& kd : resp.keys()) {
      std::set<std::string> mine;
      for (const auto& m : engine_.current_versions(kd.key())) mine.insert(DigestToBytes(m.digest));
      for (const auto& d : kd.version_digests())
        if (!mine.count(d)) {
          need.push_back(kd.key());
          break;
        }
    }
  }

  // Fetch and store durably (put_version skips versions we already cover).
  constexpr size_t kFetchBatch = 256;
  for (size_t i = 0; i < need.size(); i += kFetchBatch) {
    wire::FetchRequest req;
    req.set_from(self_);
    for (size_t j = i; j < std::min(need.size(), i + kFetchBatch); ++j) req.add_keys(need[j]);
    wire::FetchResponse resp;
    ++s.rpcs;
    grpc::Status st =
        peers_.call(peer, &wire::ReplicaService::Stub::FetchVersions, req, &resp, opts_.rpc_timeout_ms);
    if (!st.ok()) {
      s.ok = false;
      return s;
    }
    for (const auto& item : resp.items()) {
      ++s.keys_fetched;
      for (const auto& v : item.versions()) {
        s.bytes_fetched += uint64_t(v.ByteSizeLong());
        auto r = engine_.put_version(item.key(), v);
        if (r.ok() && *r) ++s.versions_stored;
      }
    }
  }
  return s;
}

}  // namespace dkv
