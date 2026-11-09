// Merkle trees for anti-entropy (LLD §5.4, §6.4).
// One fixed-shape tree per ring range: root (heap 0) -> 16 inner (1..16) -> 256 leaves (17..272).
// Leaf = SHA1(key)[0]. Hashes are recomputed lazily on query (dirty bits).
#pragma once

#include <array>
#include <bitset>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/hash.h"
#include "version/version.h"

namespace dkv {

class HashRing;
class StorageEngine;

class MerkleTree {
 public:
  static constexpr uint32_t kFanout = 16;
  static constexpr uint32_t kInner = 16;
  static constexpr uint32_t kLeaves = 256;
  static constexpr uint32_t kFirstLeaf = 1 + kInner;           // 17
  static constexpr uint32_t kNodes = 1 + kInner + kLeaves;     // 273

  using KeyDigests = std::pair<std::string, std::vector<Digest160>>;

  MerkleTree();

  // Sets the digests of a key's current versions (sorted internally). Empty vector removes the key.
  void upsert(const std::string& key, std::vector<Digest160> digests);
  Digest160 node_hash(uint32_t heap_index);
  std::vector<KeyDigests> leaf_contents(uint32_t leaf) const;  // leaf in [0, 256)
  size_t key_count() const;

  static uint32_t leaf_of(std::string_view key);
  static bool is_leaf(uint32_t heap_index) { return heap_index >= kFirstLeaf; }
  static uint32_t heap_to_leaf(uint32_t heap_index) { return heap_index - kFirstLeaf; }
  static std::vector<uint32_t> children(uint32_t heap_index);  // empty for leaves

 private:
  Digest160 compute_locked(uint32_t idx);
  mutable std::mutex mu_;
  std::array<std::map<std::string, std::vector<Digest160>>, kLeaves> leaves_;
  std::array<Digest160, kNodes> hashes_;
  std::bitset<kNodes> dirty_;
};

// All trees of one node: one per ring range it replicates.
class MerkleForest {
 public:
  // (Re)builds trees for every range `self` replicates, from the engine's current versions.
  void rebuild(std::shared_ptr<const HashRing> ring, const std::string& self,
               const StorageEngine& engine);
  // Storage change listener target.
  void on_current_changed(const std::string& key, const std::vector<VersionMeta>& current);

  // nullopt if this node does not replicate the range.
  std::optional<Digest160> node_hash(uint32_t range_id, uint32_t heap_index);
  std::optional<std::vector<MerkleTree::KeyDigests>> leaf_contents(uint32_t range_id, uint32_t leaf);
  std::vector<uint32_t> ranges() const;
  std::shared_ptr<const HashRing> ring() const;

 private:
  MerkleTree* tree(uint32_t range_id);
  mutable std::shared_mutex mu_;  // guards the map structure and ring_
  std::unordered_map<uint32_t, std::unique_ptr<MerkleTree>> trees_;
  std::shared_ptr<const HashRing> ring_;
};

std::vector<Digest160> DigestsOf(const std::vector<VersionMeta>& versions);

}  // namespace dkv
