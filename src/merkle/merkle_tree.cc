#include "merkle/merkle_tree.h"

#include <algorithm>

#include "ring/hash_ring.h"
#include "storage/storage_engine.h"

namespace dkv {
namespace {
void PutU32(Sha1* h, uint32_t v) {
  uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
  h->update(b, 4);
}
}  // namespace

std::vector<Digest160> DigestsOf(const std::vector<VersionMeta>& versions) {
  std::vector<Digest160> d;
  d.reserve(versions.size());
  for (const auto& v : versions) d.push_back(v.digest);
  return d;
}

MerkleTree::MerkleTree() {
  hashes_.fill(Digest160{});
  dirty_.set();
}

uint32_t MerkleTree::leaf_of(std::string_view key) { return Sha1Of(key)[0]; }

std::vector<uint32_t> MerkleTree::children(uint32_t idx) {
  std::vector<uint32_t> c;
  if (is_leaf(idx)) return c;
  for (uint32_t i = 1; i <= kFanout; ++i) c.push_back(idx * kFanout + i);
  return c;
}

void MerkleTree::upsert(const std::string& key, std::vector<Digest160> digests) {
  std::sort(digests.begin(), digests.end());
  uint32_t leaf = leaf_of(key);
  std::lock_guard<std::mutex> l(mu_);
  auto& m = leaves_[leaf];
  if (digests.empty())
    m.erase(key);
  else
    m[key] = std::move(digests);
  uint32_t heap = kFirstLeaf + leaf;
  dirty_.set(heap);
  dirty_.set(1 + leaf / kFanout);  // parent inner node
  dirty_.set(0);
}

Digest160 MerkleTree::compute_locked(uint32_t idx) {
  if (!dirty_.test(idx)) return hashes_[idx];
  Digest160 out{};
  if (is_leaf(idx)) {
    const auto& m = leaves_[heap_to_leaf(idx)];
    if (!m.empty()) {
      Sha1 h;
      for (const auto& kv : m) {
        PutU32(&h, uint32_t(kv.first.size()));
        h.update(kv.first);
        PutU32(&h, uint32_t(kv.second.size()));
        for (const auto& d : kv.second) h.update(d.data(), d.size());
      }
      out = h.finish();
    }
  } else {
    Sha1 h;
    for (uint32_t c : children(idx)) {
      Digest160 ch = compute_locked(c);
      h.update(ch.data(), ch.size());
    }
    out = h.finish();
  }
  hashes_[idx] = out;
  dirty_.reset(idx);
  return out;
}

Digest160 MerkleTree::node_hash(uint32_t heap_index) {
  std::lock_guard<std::mutex> l(mu_);
  if (heap_index >= kNodes) return Digest160{};
  return compute_locked(heap_index);
}

std::vector<MerkleTree::KeyDigests> MerkleTree::leaf_contents(uint32_t leaf) const {
  std::lock_guard<std::mutex> l(mu_);
  std::vector<KeyDigests> out;
  if (leaf >= kLeaves) return out;
  for (const auto& kv : leaves_[leaf]) out.emplace_back(kv.first, kv.second);
  return out;
}

size_t MerkleTree::key_count() const {
  std::lock_guard<std::mutex> l(mu_);
  size_t n = 0;
  for (const auto& m : leaves_) n += m.size();
  return n;
}

// ------------------------------------------------------------------------------------- forest

void MerkleForest::rebuild(std::shared_ptr<const HashRing> ring, const std::string& self,
                           const StorageEngine& engine) {
  std::unordered_map<uint32_t, std::unique_ptr<MerkleTree>> trees;
  for (uint32_t r : ring->ranges_replicated_by(self)) trees[r] = std::make_unique<MerkleTree>();
  engine.for_each_key([&](const std::string& key, const std::vector<VersionMeta>& cur) {
    auto it = trees.find(ring->range_of(key));
    if (it != trees.end()) it->second->upsert(key, DigestsOf(cur));
  });
  std::unique_lock<std::shared_mutex> l(mu_);
  trees_ = std::move(trees);
  ring_ = std::move(ring);
}

void MerkleForest::on_current_changed(const std::string& key, const std::vector<VersionMeta>& current) {
  std::shared_lock<std::shared_mutex> l(mu_);
  if (!ring_) return;
  auto it = trees_.find(ring_->range_of(key));
  if (it != trees_.end()) it->second->upsert(key, DigestsOf(current));
}

MerkleTree* MerkleForest::tree(uint32_t range_id) {
  auto it = trees_.find(range_id);
  return it == trees_.end() ? nullptr : it->second.get();
}

std::optional<Digest160> MerkleForest::node_hash(uint32_t range_id, uint32_t heap_index) {
  std::shared_lock<std::shared_mutex> l(mu_);
  MerkleTree* t = tree(range_id);
  if (!t) return std::nullopt;
  return t->node_hash(heap_index);
}

std::optional<std::vector<MerkleTree::KeyDigests>> MerkleForest::leaf_contents(uint32_t range_id,
                                                                               uint32_t leaf) {
  std::shared_lock<std::shared_mutex> l(mu_);
  MerkleTree* t = tree(range_id);
  if (!t) return std::nullopt;
  return t->leaf_contents(leaf);
}

std::vector<uint32_t> MerkleForest::ranges() const {
  std::shared_lock<std::shared_mutex> l(mu_);
  std::vector<uint32_t> out;
  for (const auto& kv : trees_) out.push_back(kv.first);
  std::sort(out.begin(), out.end());
  return out;
}

std::shared_ptr<const HashRing> MerkleForest::ring() const {
  std::shared_lock<std::shared_mutex> l(mu_);
  return ring_;
}

}  // namespace dkv
