// Shared workload helpers for dkv_loadgen and dkv_verify (LLD §5.9, §7.8).
// Values are deterministic from (key, seq), so the verifier can check the exact bytes returned.
#pragma once

#include <cstdint>
#include <string>

namespace dkv {

std::string WorkloadKey(const std::string& prefix, uint64_t index);
// "<key>#<seq>#" followed by deterministic filler up to `bytes` (at least the header).
std::string WorkloadValue(const std::string& key, uint64_t seq, uint32_t bytes);
// Parses the seq from a workload value; false if the value is not in workload format for `key`.
bool ParseWorkloadSeq(const std::string& key, const std::string& value, uint64_t* seq);

}  // namespace dkv
