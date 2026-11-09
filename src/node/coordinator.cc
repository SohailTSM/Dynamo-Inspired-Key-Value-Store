#include "node/coordinator.h"

#include <algorithm>
#include <memory>

#include "common/logging.h"
#include "node/quorum.h"

namespace dkv {

// ------------------------------------------------------------------------------- RequestDeduper

bool RequestDeduper::get(const std::string& id, wire::PutResponse* out) {
  if (id.empty()) return false;
  std::lock_guard<std::mutex> l(mu_);
  auto it = map_.find(id);
  if (it == map_.end()) return false;
  *out = it->second.first;
  return true;
}

void RequestDeduper::put(const std::string& id, const wire::PutResponse& resp) {
  if (id.empty()) return;
  std::lock_guard<std::mutex> l(mu_);
  if (map_.count(id)) return;
  order_.push_front(id);
  map_[id] = {resp, order_.begin()};
  while (map_.size() > capacity_) {
    map_.erase(order_.back());
    order_.pop_back();
  }
}

// ---------------------------------------------------------------------------------- Coordinator

Coordinator::Coordinator(std::string self, const wire::ClusterConfig& cfg, RingCache& ring,
                         FailureDetector& fd, StorageEngine& engine, PeerClientPool& peers,
                         ThreadPool& pool, Clock& clock, EventLog* events)
    : self_(std::move(self)),
      n_(cfg.quorum().n()),
      r_(cfg.quorum().r()),
      w_(cfg.quorum().w()),
      timeout_ms_(cfg.timing().coord_timeout_ms()),
      read_before_write_(cfg.policy().stamping() == wire::Policy::READ_BEFORE_WRITE),
      ring_(ring),
      fd_(fd),
      engine_(engine),
      peers_(peers),
      pool_(pool),
      clock_(clock),
      events_(events),
      resolver_(cfg.policy().resolution() == wire::Policy::RETURN_SIBLINGS ? ResolutionPolicy::kReturnSiblings
                                                                         : ResolutionPolicy::kLwwOnConcurrent) {}

std::vector<std::string> Coordinator::live_peers(const std::vector<std::string>& pref) const {
  std::vector<std::string> out;
  for (const auto& p : pref)
    if (p != self_ && !fd_.is_down(p)) out.push_back(p);
  return out;
}

void Coordinator::put(const wire::PutRequest& req, wire::PutResponse* resp) {
  const int64_t t0 = clock_.now_mono_us();
  if (dedupe_.get(req.request_id(), resp)) return;
  resp->set_coordinator(self_);
  auto ring = ring_.current();
  auto pref = ring->preference_list(req.key());
  if (std::find(pref.begin(), pref.end(), self_) == pref.end()) {
    SetResult(resp->mutable_result(), wire::NOT_A_REPLICA, "coordinator not in preference list");
    return;
  }
  const uint32_t w = effective_w(req);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);
  const auto peers = live_peers(pref);
  if (peers.size() + 1 < w) {  // can never reach W: fail before creating an unackable local write
    SetResult(resp->mutable_result(), wire::QUORUM_NOT_REACHED,
              "only " + std::to_string(peers.size() + 1) + " live replicas < W=" + std::to_string(w));
    return;
  }

  // 1. Stamp, READ_BEFORE_WRITE: read clocks from R-1 peers (outside the key lock; LLD §7.1).
  VectorClock remote;
  int64_t max_ts = 0;
  if (read_before_write_ && r_ > 1) {
    const uint32_t need = r_ - 1;
    if (peers.size() < need) {
      SetResult(resp->mutable_result(), wire::QUORUM_NOT_REACHED,
                "not enough live replicas to read clocks (" + std::to_string(peers.size()) + ")");
      return;
    }
    auto q = std::make_shared<QuorumTracker<wire::ReadLocalResponse>>(need, uint32_t(peers.size()));
    wire::ReadLocalRequest rq;
    rq.set_key(req.key());
    rq.set_metadata_only(true);
    rq.set_from(self_);
    for (const auto& p : peers)
      peers_.async<wire::ReadLocalRequest, wire::ReadLocalResponse>(
          p, &wire::ReplicaService::Stub::ReadLocal, rq, timeout_ms_,
          [q, p](const grpc::Status& st, const wire::ReadLocalResponse& r) {
            q->add(st.ok() && r.result().code() == wire::OK, p, r);
          });
    if (!q->wait_until(deadline)) {
      SetResult(resp->mutable_result(), wire::QUORUM_NOT_REACHED, "clock read: fewer than R-1 replies");
      return;
    }
    for (const auto& kv : q->replies())
      for (const auto& v : kv.second.versions()) {
        remote.merge(VectorClock::FromProto(v.clock()));
        max_ts = std::max(max_ts, v.timestamp_ms());
      }
  }

  // 2. Under the key lock: merge fresh local clocks, increment, durable local write (ack #1).
  wire::Version version;
  {
    std::lock_guard<std::mutex> l(locks_.for_key(req.key()));
    VectorClock clock = remote;
    for (const auto& m : engine_.current_versions(req.key())) {
      clock.merge(m.clock);
      max_ts = std::max(max_ts, m.timestamp_ms);
    }
    clock.increment(self_);
    int64_t ts = std::max(clock_.now_wall_ms(), max_ts + 1);
    version.set_value(req.value());
    clock.ToProto(version.mutable_clock());
    version.set_timestamp_ms(ts);
    version.set_coordinator_id(self_);
    version.set_digest(DigestToBytes(ComputeVersionDigest(clock, ts, self_, req.value())));
    auto st = engine_.put_version(req.key(), version);
    if (!st.ok()) {
      SetResult(resp->mutable_result(), wire::INTERNAL, "local write failed: " + st.status().ToString());
      return;
    }
  }

  // 3. Fan out to the other live replicas; ack the client once W durable copies exist.
  uint32_t acks = 1;
  if (w > 1 || !peers.empty()) {
    const uint32_t need = w > 1 ? w - 1 : 0;
    auto q = std::make_shared<QuorumTracker<wire::ReplicateResponse>>(need, uint32_t(peers.size()));
    wire::ReplicateRequest rr;
    rr.set_key(req.key());
    *rr.mutable_version() = version;
    rr.set_from(self_);
    for (const auto& p : peers)
      peers_.async<wire::ReplicateRequest, wire::ReplicateResponse>(
          p, &wire::ReplicaService::Stub::Replicate, rr, timeout_ms_,
          [q, p](const grpc::Status& st, const wire::ReplicateResponse& r) {
            q->add(st.ok() && r.result().code() == wire::OK, p, r);
          });
    if (need > 0) q->wait_until(deadline);
    acks += q->ok_count();
  }

  wire::Version meta = version;
  meta.clear_value();
  *resp->mutable_version() = meta;
  resp->set_acks(acks);
  if (acks >= w) {
    SetResult(resp->mutable_result(), wire::OK);
  } else {
    SetResult(resp->mutable_result(), wire::QUORUM_NOT_REACHED,
              "only " + std::to_string(acks) + " of W=" + std::to_string(w) + " durable copies");
  }
  dedupe_.put(req.request_id(), *resp);
  const int64_t lat = clock_.now_mono_us() - t0;
  if (events_ && (resp->result().code() != wire::OK || (std::hash<std::string>{}(req.key()) % 100) == 0))
    events_->emit(Ev("coord.put")
                      .str("key", req.key())
                      .str("code", PbCodeName(resp->result().code()))
                      .u64("acks", acks)
                      .i64("latency_us", lat));
}

void Coordinator::get(const wire::GetRequest& req, wire::GetResponse* resp) {
  const int64_t t0 = clock_.now_mono_us();
  resp->set_coordinator(self_);
  auto ring = ring_.current();
  auto pref = ring->preference_list(req.key());
  if (std::find(pref.begin(), pref.end(), self_) == pref.end()) {
    SetResult(resp->mutable_result(), wire::NOT_A_REPLICA, "coordinator not in preference list");
    return;
  }
  const uint32_t r = effective_r(req);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);
  const auto peers = live_peers(pref);
  const uint32_t need = r > 1 ? r - 1 : 0;
  if (peers.size() < need) {
    SetResult(resp->mutable_result(), wire::QUORUM_NOT_REACHED,
              "not enough live replicas (" + std::to_string(peers.size() + 1) + " < R)");
    return;
  }

  auto q = std::make_shared<QuorumTracker<wire::ReadLocalResponse>>(need, uint32_t(peers.size()));
  wire::ReadLocalRequest rq;
  rq.set_key(req.key());
  rq.set_metadata_only(false);
  rq.set_from(self_);
  for (const auto& p : peers)
    peers_.async<wire::ReadLocalRequest, wire::ReadLocalResponse>(
        p, &wire::ReplicaService::Stub::ReadLocal, rq, timeout_ms_,
        [q, p](const grpc::Status& st, const wire::ReadLocalResponse& rr) {
          q->add(st.ok() && rr.result().code() == wire::OK, p, rr);
        });
  // Local read (response #1) while peers are working.
  std::vector<VersionMeta> local = engine_.current_versions(req.key());
  if (need > 0 && !q->wait_until(deadline)) {
    SetResult(resp->mutable_result(), wire::QUORUM_NOT_REACHED, "fewer than R responses");
    return;
  }
  auto replies = q->replies();

  // Candidates: (meta, source index). Source -1 = self, i >= 0 = replies[i].
  std::vector<VersionMeta> metas;
  std::vector<std::pair<int, int>> origin;  // (source, version index within source)
  for (size_t i = 0; i < local.size(); ++i) {
    metas.push_back(local[i]);
    origin.emplace_back(-1, int(i));
  }
  for (size_t s = 0; s < replies.size(); ++s)
    for (int i = 0; i < replies[s].second.versions_size(); ++i) {
      metas.push_back(MetaFromProto(replies[s].second.versions(i)));
      origin.emplace_back(int(s), i);
    }
  resp->set_responses(uint32_t(1 + replies.size()));
  VectorClock context;
  for (const auto& m : metas) context.merge(m.clock);
  context.ToProto(resp->mutable_context());

  int wi = VersionResolver::winner_index(metas);
  if (wi < 0) {
    SetResult(resp->mutable_result(), wire::NOT_FOUND, "");
  } else {
    wire::Version winner;
    if (origin[wi].first < 0) {
      auto full = engine_.read_version(req.key(), metas[wi]);
      if (!full.ok()) {
        SetResult(resp->mutable_result(), wire::INTERNAL, "local read failed: " + full.status().ToString());
        return;
      }
      winner = *full;
    } else {
      winner = replies[origin[wi].first].second.versions(origin[wi].second);
    }
    // Phase 1: one version. [P2] kReturnSiblings would add every survivor of prune().
    *resp->add_versions() = winner;
    SetResult(resp->mutable_result(), wire::OK);

    // Read repair: every responder whose current set neither contains nor dominates the winner.
    auto covers = [&](const std::vector<VersionMeta>& set) {
      for (const auto& m : set)
        if (m.digest == metas[wi].digest || metas[wi].clock.compare(m.clock) == Order::kBefore) return true;
      return false;
    };
    bool repair_self = !covers(local);
    std::vector<std::string> stale;
    for (const auto& rep : replies) {
      std::vector<VersionMeta> set;
      for (const auto& v : rep.second.versions()) set.push_back(MetaFromProto(v));
      if (!covers(set)) stale.push_back(rep.first);
    }
    if (repair_self || !stale.empty()) read_repair(req.key(), winner, stale, repair_self);
  }
  const int64_t lat = clock_.now_mono_us() - t0;
  if (events_ && (resp->result().code() != wire::OK && resp->result().code() != wire::NOT_FOUND))
    events_->emit(Ev("coord.get")
                      .str("key", req.key())
                      .str("code", PbCodeName(resp->result().code()))
                      .u64("responses", resp->responses())
                      .i64("latency_us", lat));
}

void Coordinator::read_repair(const std::string& key, const wire::Version& winner,
                              const std::vector<std::string>& stale_peers, bool repair_self) {
  if (repair_self) {
    pool_.submit([this, key, winner] {
      auto st = engine_.put_version(key, winner);
      if (!st.ok()) DKV_LOG(WARN) << "read repair (self) failed: " << st.status().ToString();
    });
  }
  wire::ReplicateRequest rr;
  rr.set_key(key);
  *rr.mutable_version() = winner;
  rr.set_from(self_);
  rr.set_read_repair(true);
  for (const auto& p : stale_peers)
    peers_.async<wire::ReplicateRequest, wire::ReplicateResponse>(p, &wire::ReplicaService::Stub::Replicate, rr,
                                                              timeout_ms_, nullptr);
  if (events_)
    events_->emit(Ev("read_repair").str("key", key).u64("peers", stale_peers.size()).boolean("self", repair_self));
}

}  // namespace dkv
