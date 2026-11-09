#include "ring/hash_ring.h"

#include <algorithm>

#include "common/config_loader.h"
#include "common/hash.h"

namespace dkv {

uint64_t HashRing::token(std::string_view s) {
  Digest128 d = Md5(s);
  uint64_t t = 0;
  for (int i = 0; i < 8; ++i) t = (t << 8) | d[i];
  return t;
}

HashRing HashRing::Build(const std::vector<RingNode>& nodes, uint32_t vnodes, uint32_t n,
                         uint64_t epoch) {
  HashRing r;
  r.nodes_ = nodes;
  r.vnodes_ = vnodes;
  r.n_ = std::min<uint32_t>(n, uint32_t(nodes.size()));
  r.epoch_ = epoch;
  r.node_tokens_.resize(nodes.size());
  for (size_t i = 0; i < nodes.size(); ++i)
    for (uint32_t v = 0; v < vnodes; ++v)
      r.node_tokens_[i].push_back(token(nodes[i].id + "#" + std::to_string(v)));
  r.finalize();
  return r;
}

HashRing HashRing::FromConfig(const wire::ClusterConfig& cfg, uint64_t epoch) {
  std::vector<RingNode> nodes;
  for (const auto& e : cfg.storage_nodes()) nodes.push_back({e.id(), EndpointAddress(e)});
  return Build(nodes, cfg.vnodes_per_node(), cfg.quorum().n(), epoch);
}

HashRing HashRing::FromRingInfo(const wire::RingInfo& info) {
  HashRing r;
  r.vnodes_ = info.vnodes_per_node();
  r.n_ = std::min<uint32_t>(info.quorum().n(), uint32_t(info.nodes_size()));
  r.epoch_ = info.epoch();
  for (const auto& ni : info.nodes()) {
    r.nodes_.push_back({ni.id(), ni.address()});
    r.node_tokens_.emplace_back(ni.tokens().begin(), ni.tokens().end());
  }
  r.finalize();
  return r;
}

wire::RingInfo HashRing::ToRingInfo() const {
  wire::RingInfo info;
  info.set_epoch(epoch_);
  info.set_vnodes_per_node(vnodes_);
  info.mutable_quorum()->set_n(n_);
  for (size_t i = 0; i < nodes_.size(); ++i) {
    auto* ni = info.add_nodes();
    ni->set_id(nodes_[i].id);
    ni->set_address(nodes_[i].address);
    for (uint64_t t : node_tokens_[i]) ni->add_tokens(t);
  }
  return info;
}

void HashRing::finalize() {
  tokens_.clear();
  for (size_t i = 0; i < node_tokens_.size(); ++i)
    for (uint64_t t : node_tokens_[i]) tokens_.emplace_back(t, uint32_t(i));
  std::sort(tokens_.begin(), tokens_.end());
  range_replicas_.assign(tokens_.size(), {});
  for (size_t r = 0; r < tokens_.size(); ++r) {
    auto& reps = range_replicas_[r];
    for (size_t k = 0; k < tokens_.size() && reps.size() < n_; ++k) {
      const std::string& id = nodes_[tokens_[(r + k) % tokens_.size()].second].id;
      if (std::find(reps.begin(), reps.end(), id) == reps.end()) reps.push_back(id);
    }
  }
}

uint32_t HashRing::range_of(std::string_view key) const {
  if (tokens_.empty()) return 0;
  uint64_t t = token(key);
  auto it = std::lower_bound(tokens_.begin(), tokens_.end(), std::make_pair(t, uint32_t(0)));
  if (it == tokens_.end()) return 0;  // wraps to range 0
  return uint32_t(it - tokens_.begin());
}

const std::vector<std::string>& HashRing::replicas_of_range(uint32_t range_id) const {
  static const std::vector<std::string> kEmpty;
  return range_id < range_replicas_.size() ? range_replicas_[range_id] : kEmpty;
}

std::vector<std::string> HashRing::preference_list(std::string_view key) const {
  return replicas_of_range(range_of(key));
}

std::vector<uint32_t> HashRing::ranges_replicated_by(const std::string& node) const {
  std::vector<uint32_t> out;
  for (uint32_t r = 0; r < range_replicas_.size(); ++r)
    if (is_replica(node, r)) out.push_back(r);
  return out;
}

bool HashRing::is_replica(const std::string& node, uint32_t range_id) const {
  const auto& reps = replicas_of_range(range_id);
  return std::find(reps.begin(), reps.end(), node) != reps.end();
}

std::string HashRing::address_of(const std::string& id) const {
  for (const auto& n : nodes_)
    if (n.id == id) return n.address;
  return "";
}

}  // namespace dkv
