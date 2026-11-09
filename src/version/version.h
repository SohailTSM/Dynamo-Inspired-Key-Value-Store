// Version metadata + digest + declared conflict-resolution rule (LLD §5.1, HLD §6).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common.pb.h"
#include "common/hash.h"
#include "version/vector_clock.h"

namespace dkv {

// Where a version's record lives in the version log.
struct Location {
  uint32_t segment_id = 0;
  uint64_t offset = 0;  // start of the record frame
  uint32_t length = 0;  // whole frame length (header + payload)
};

// A version without its value (what the index keeps).
struct VersionMeta {
  VectorClock clock;
  int64_t timestamp_ms = 0;
  std::string coordinator;
  Digest160 digest{};
  Location loc;
  bool tombstone = false;
};

// SHA-1(canonical clock | timestamp | coordinator | SHA-1(value)). Computed once by the coordinator
// and stored with the version, so every replica holding the version has identical bytes.
Digest160 ComputeVersionDigest(const VectorClock& clock, int64_t timestamp_ms,
                               const std::string& coordinator, std::string_view value);

VersionMeta MetaFromProto(const wire::Version& v, Location loc = {});
// Fills everything except the value.
void MetaToProto(const VersionMeta& m, wire::Version* out);

enum class ResolutionPolicy { kLwwOnConcurrent, kReturnSiblings /* [P2] */ };

class VersionResolver {
 public:
  explicit VersionResolver(ResolutionPolicy p = ResolutionPolicy::kLwwOnConcurrent) : policy_(p) {}

  // Removes versions dominated by another one, and exact duplicates (same digest).
  // Survivors are pairwise concurrent ("siblings").
  static std::vector<VersionMeta> prune(std::vector<VersionMeta> versions);

  // Phase 1 (kLwwOnConcurrent): exactly one winner (empty input -> empty output).
  // [P2] kReturnSiblings: all survivors.
  std::vector<VersionMeta> resolve(std::vector<VersionMeta> versions) const;

  // Declared tie rule for concurrent versions: higher timestamp wins; equal timestamps ->
  // lexicographically lower coordinator id wins; still equal -> lower digest bytes (total order).
  static bool wins_tie(const VersionMeta& a, const VersionMeta& b);

  // Index of the winning element among `versions` per prune + tie rule (-1 if empty).
  static int winner_index(const std::vector<VersionMeta>& versions);

  ResolutionPolicy policy() const { return policy_; }

 private:
  ResolutionPolicy policy_;
};

}  // namespace dkv
