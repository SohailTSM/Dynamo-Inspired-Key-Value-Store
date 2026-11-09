// Consistent-hash ring with virtual nodes (LLD §5.2, HLD §5).
//  - token(x) = first 8 bytes of MD5(x), big-endian
//  - node i owns tokens MD5(id + "#" + v) for v in [0, vnodes)
//  - range r = (token[r-1], token[r]] ; range 0 wraps around
//  - replicas of range r = first N distinct physical nodes walking clockwise from token r
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config.pb.h"
#include "metadata.pb.h"

namespace dkv {

struct RingNode {
  std::string id;
  std::string address;
};

class HashRing {
 public:
  static uint64_t token(std::string_view s);

  // Computes vnode tokens deterministically.
  static HashRing Build(const std::vector<RingNode>& nodes, uint32_t vnodes, uint32_t n,
                        uint64_t epoch);
  static HashRing FromConfig(const wire::ClusterConfig& cfg, uint64_t epoch = 0);
  // Uses the tokens carried in RingInfo (Metadata Service is the source of truth).
  static HashRing FromRingInfo(const wire::RingInfo& info);
  wire::RingInfo ToRingInfo() const;

  std::vector<std::string> preference_list(std::string_view key) const;  // N distinct nodes
  uint32_t range_of(std::string_view key) const;
  const std::vector<std::string>& replicas_of_range(uint32_t range_id) const;
  std::vector<uint32_t> ranges_replicated_by(const std::string& node) const;
  bool is_replica(const std::string& node, uint32_t range_id) const;

  uint32_t range_count() const { return uint32_t(tokens_.size()); }
  uint32_t n() const { return n_; }
  uint64_t epoch() const { return epoch_; }
  const std::vector<RingNode>& nodes() const { return nodes_; }
  std::string address_of(const std::string& id) const;  // "" if unknown

 private:
  void finalize();  // sort tokens, precompute replicas per range
  std::vector<RingNode> nodes_;
  std::vector<std::vector<uint64_t>> node_tokens_;     // per node index
  std::vector<std::pair<uint64_t, uint32_t>> tokens_;  // (token, node index), sorted
  std::vector<std::vector<std::string>> range_replicas_;
  uint32_t vnodes_ = 0;
  uint32_t n_ = 3;
  uint64_t epoch_ = 0;
};

}  // namespace dkv
