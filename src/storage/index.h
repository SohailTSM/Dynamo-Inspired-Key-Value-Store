// In-memory index: key -> current (non-dominated) versions (LLD §5.3).
// apply() is order-independent: whatever order versions arrive in (live or during log replay), the
// resulting current set is the same, so replicas converge and Merkle hashes match.
#pragma once

#include <array>
#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "version/version.h"

namespace dkv {

struct KeyEntry {
  std::vector<VersionMeta> current;
  uint32_t versions_on_disk = 0;
};

class Index {
 public:
  using OnChange = std::function<void(const std::string& key, const std::vector<VersionMeta>& current)>;

  // Applies a durable version. Returns true if the current set changed. `on_change` (optional) is
  // called with the new current set while the shard lock is held, so callers see changes in order.
  bool apply(const std::string& key, const VersionMeta& v, const OnChange& on_change = nullptr);

  std::vector<VersionMeta> current(const std::string& key) const;
  // True if the version (by digest) is already current or an existing version strictly dominates
  // the clock -> storing it again would change nothing.
  bool covers(const std::string& key, const VectorClock& clock, const Digest160& digest) const;
  size_t key_count() const;
  void for_each(const OnChange& visit) const;

 private:
  static constexpr size_t kShards = 64;
  struct Shard {
    mutable std::shared_mutex mu;
    std::unordered_map<std::string, KeyEntry> map;
  };
  Shard& shard(const std::string& key);
  const Shard& shard(const std::string& key) const;
  std::array<Shard, kShards> shards_;
};

}  // namespace dkv
