#include "version/version.h"

#include <algorithm>

namespace dkv {

Digest160 ComputeVersionDigest(const VectorClock& clock, int64_t timestamp_ms,
                               const std::string& coordinator, std::string_view value) {
  Digest160 value_hash = Sha1Of(value);
  Sha1 h;
  h.update(clock.canonical_bytes());
  h.update("|");
  h.update(std::to_string(timestamp_ms));
  h.update("|");
  h.update(coordinator);
  h.update("|");
  h.update(value_hash.data(), value_hash.size());
  return h.finish();
}

VersionMeta MetaFromProto(const wire::Version& v, Location loc) {
  VersionMeta m;
  m.clock = VectorClock::FromProto(v.clock());
  m.timestamp_ms = v.timestamp_ms();
  m.coordinator = v.coordinator_id();
  m.digest = BytesToDigest(v.digest());
  m.loc = loc;
  m.tombstone = v.tombstone();
  return m;
}

void MetaToProto(const VersionMeta& m, wire::Version* out) {
  m.clock.ToProto(out->mutable_clock());
  out->set_timestamp_ms(m.timestamp_ms);
  out->set_coordinator_id(m.coordinator);
  out->set_digest(DigestToBytes(m.digest));
  out->set_tombstone(m.tombstone);
}

std::vector<VersionMeta> VersionResolver::prune(std::vector<VersionMeta> versions) {
  std::vector<VersionMeta> out;
  for (auto& v : versions) {
    bool drop = false;
    for (auto it = out.begin(); it != out.end();) {
      if (it->digest == v.digest) {  // exact duplicate
        drop = true;
        break;
      }
      Order o = v.clock.compare(it->clock);
      if (o == Order::kBefore) {  // v dominated by an existing survivor
        drop = true;
        break;
      }
      if (o == Order::kAfter) {  // v dominates this survivor
        it = out.erase(it);
        continue;
      }
      if (o == Order::kEqual) {
        // Same clock, different digest: should not happen with correct stamping. Keep the tie-rule
        // winner so the result stays deterministic.
        if (wins_tie(v, *it)) {
          it = out.erase(it);
          continue;
        }
        drop = true;
        break;
      }
      ++it;  // concurrent: both survive
    }
    if (!drop) out.push_back(std::move(v));
  }
  return out;
}

bool VersionResolver::wins_tie(const VersionMeta& a, const VersionMeta& b) {
  if (a.timestamp_ms != b.timestamp_ms) return a.timestamp_ms > b.timestamp_ms;
  if (a.coordinator != b.coordinator) return a.coordinator < b.coordinator;
  return a.digest < b.digest;
}

std::vector<VersionMeta> VersionResolver::resolve(std::vector<VersionMeta> versions) const {
  auto survivors = prune(std::move(versions));
  if (survivors.empty() || policy_ == ResolutionPolicy::kReturnSiblings) {
    std::sort(survivors.begin(), survivors.end(),
              [](const VersionMeta& a, const VersionMeta& b) { return wins_tie(a, b); });
    return survivors;
  }
  auto best = std::min_element(survivors.begin(), survivors.end(),
                               [](const VersionMeta& a, const VersionMeta& b) { return wins_tie(a, b); });
  return {*best};
}

int VersionResolver::winner_index(const std::vector<VersionMeta>& versions) {
  if (versions.empty()) return -1;
  auto survivors = prune(versions);
  auto best = std::min_element(survivors.begin(), survivors.end(),
                               [](const VersionMeta& a, const VersionMeta& b) { return wins_tie(a, b); });
  for (size_t i = 0; i < versions.size(); ++i)
    if (versions[i].digest == best->digest) return int(i);
  return 0;
}

}  // namespace dkv
