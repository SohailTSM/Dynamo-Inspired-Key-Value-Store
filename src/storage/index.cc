#include "storage/index.h"

#include <mutex>

namespace dkv {

Index::Shard& Index::shard(const std::string& key) {
  return shards_[std::hash<std::string>{}(key) % kShards];
}
const Index::Shard& Index::shard(const std::string& key) const {
  return shards_[std::hash<std::string>{}(key) % kShards];
}

bool Index::apply(const std::string& key, const VersionMeta& v, const OnChange& on_change) {
  Shard& s = shard(key);
  std::unique_lock<std::shared_mutex> l(s.mu);
  KeyEntry& e = s.map[key];
  e.versions_on_disk++;
  auto& cur = e.current;
  for (const auto& c : cur) {
    if (c.digest == v.digest) return false;  // already current
    Order o = v.clock.compare(c.clock);
    if (o == Order::kBefore) return false;  // dominated
    if (o == Order::kEqual && !VersionResolver::wins_tie(v, c)) return false;
  }
  // v survives: remove everything it dominates (or equal-clock versions it beats on the tie rule).
  for (auto it = cur.begin(); it != cur.end();) {
    Order o = v.clock.compare(it->clock);
    if (o == Order::kAfter || o == Order::kEqual)
      it = cur.erase(it);
    else
      ++it;
  }
  cur.push_back(v);
  if (on_change) on_change(key, cur);
  return true;
}

std::vector<VersionMeta> Index::current(const std::string& key) const {
  const Shard& s = shard(key);
  std::shared_lock<std::shared_mutex> l(s.mu);
  auto it = s.map.find(key);
  return it == s.map.end() ? std::vector<VersionMeta>{} : it->second.current;
}

bool Index::covers(const std::string& key, const VectorClock& clock, const Digest160& digest) const {
  const Shard& s = shard(key);
  std::shared_lock<std::shared_mutex> l(s.mu);
  auto it = s.map.find(key);
  if (it == s.map.end()) return false;
  for (const auto& c : it->second.current) {
    if (c.digest == digest) return true;
    if (clock.compare(c.clock) == Order::kBefore) return true;
  }
  return false;
}

size_t Index::key_count() const {
  size_t n = 0;
  for (const auto& s : shards_) {
    std::shared_lock<std::shared_mutex> l(s.mu);
    n += s.map.size();
  }
  return n;
}

void Index::for_each(const OnChange& visit) const {
  for (const auto& s : shards_) {
    std::shared_lock<std::shared_mutex> l(s.mu);
    for (const auto& kv : s.map) visit(kv.first, kv.second.current);
  }
}

}  // namespace dkv
