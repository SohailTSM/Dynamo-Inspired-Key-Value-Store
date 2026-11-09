#include "tools/workload.h"

#include <algorithm>
#include <cstdlib>

#include "common/hash.h"

namespace dkv {

std::string WorkloadKey(const std::string& prefix, uint64_t index) {
  return prefix + std::to_string(index);
}

std::string WorkloadValue(const std::string& key, uint64_t seq, uint32_t bytes) {
  const std::string header = key + "#" + std::to_string(seq) + "#";
  std::string v = header;
  // Filler: repeated hex of SHA-1(key#seq) — deterministic, cheap, not trivially compressible.
  std::string pad = ToHex(Sha1Of(v));
  while (v.size() < bytes) v += pad;
  if (v.size() > bytes && bytes >= header.size()) v.resize(bytes);  // never cut the header
  return v;
}

bool ParseWorkloadSeq(const std::string& key, const std::string& value, uint64_t* seq) {
  if (value.size() < key.size() + 3 || value.compare(0, key.size(), key) != 0 || value[key.size()] != '#')
    return false;
  const char* p = value.c_str() + key.size() + 1;
  char* end = nullptr;
  unsigned long long s = std::strtoull(p, &end, 10);
  if (end == p || *end != '#') return false;
  *seq = s;
  return true;
}

}  // namespace dkv
