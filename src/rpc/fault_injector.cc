#include "rpc/fault_injector.h"

namespace dkv {

void FaultInjector::set(const wire::FaultConfig& cfg) {
  std::lock_guard<std::mutex> l(mu_);
  rules_.clear();
  for (const auto& r : cfg.rules()) rules_[r.peer()] = r;
}

bool FaultInjector::allow_outgoing(const std::string& peer) const {
  std::lock_guard<std::mutex> l(mu_);
  auto it = rules_.find(peer);
  return it == rules_.end() || !it->second.drop_outgoing();
}

bool FaultInjector::allow_incoming(const std::string& peer) const {
  if (peer.empty()) return true;
  std::lock_guard<std::mutex> l(mu_);
  auto it = rules_.find(peer);
  return it == rules_.end() || !it->second.drop_incoming();
}

uint32_t FaultInjector::delay_ms(const std::string& peer) const {
  std::lock_guard<std::mutex> l(mu_);
  auto it = rules_.find(peer);
  return it == rules_.end() ? 0 : it->second.delay_ms();
}

wire::FaultConfig FaultInjector::get() const {
  std::lock_guard<std::mutex> l(mu_);
  wire::FaultConfig cfg;
  for (const auto& kv : rules_) *cfg.add_rules() = kv.second;
  return cfg;
}

}  // namespace dkv
