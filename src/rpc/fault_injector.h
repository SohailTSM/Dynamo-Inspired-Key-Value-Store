// Per-peer fault rules: drop outgoing, drop incoming, delay (LLD §5.5). Used by tests now and by the
// Phase 2 partition experiment.
#pragma once

#include <map>
#include <mutex>
#include <string>

#include "node.pb.h"

namespace dkv {

class FaultInjector {
 public:
  void set(const wire::FaultConfig& cfg);  // replaces all rules
  bool allow_outgoing(const std::string& peer) const;
  bool allow_incoming(const std::string& peer) const;
  uint32_t delay_ms(const std::string& peer) const;
  wire::FaultConfig get() const;

 private:
  mutable std::mutex mu_;
  std::map<std::string, wire::FaultRule> rules_;
};

}  // namespace dkv
