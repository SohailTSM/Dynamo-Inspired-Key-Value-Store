// Peer heartbeats + local health view (LLD §7.5, HLD state machine state-peer-local).
//   UP --1 missed--> SUSPECT --3 missed--> DOWN ; any heartbeat -> UP
#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "common/clock.h"
#include "common/event_log.h"
#include "node.pb.h"
#include "rpc/peer_client_pool.h"

namespace dkv {

enum class PeerHealth { kUp, kSuspect, kDown };
const char* PeerHealthName(PeerHealth h);

class FailureDetector {
 public:
  struct Options {
    uint32_t interval_ms = 1000;
    uint32_t suspect_after = 1;
    uint32_t down_after = 3;
    uint32_t tick_ms = 100;
    uint32_t rpc_timeout_ms = 500;
  };

  FailureDetector(std::string self, std::vector<std::string> peers, Options opts, PeerClientPool& pool,
                  Clock& clock, EventLog* events);
  ~FailureDetector();

  void set_incarnation(uint64_t inc) { incarnation_ = inc; }
  void start();
  void stop();

  // Incoming heartbeat from a peer.
  void on_heartbeat(const wire::HeartbeatMsg& m);
  uint64_t seen_incarnation(const std::string& peer) const;

  bool is_down(const std::string& peer) const;
  PeerHealth health(const std::string& peer) const;
  std::vector<std::string> suspects() const;  // SUSPECT or DOWN
  // True once `peer` acknowledged a heartbeat carrying our current incarnation.
  bool peer_acked_incarnation(const std::string& peer) const;
  const std::vector<std::string>& peers() const { return peers_; }

  wire::HeartbeatMsg make_heartbeat();

 private:
  struct PeerState {
    int64_t last_seen_mono_ms = 0;
    uint64_t incarnation = 0;
    PeerHealth health = PeerHealth::kUp;
  };
  void sender_loop();
  void ticker_loop();
  void tick();

  const std::string self_;
  const std::vector<std::string> peers_;
  const Options opts_;
  PeerClientPool& pool_;
  Clock& clock_;
  EventLog* events_;
  std::atomic<uint64_t> incarnation_{0};
  std::atomic<uint64_t> seq_{0};

  mutable std::mutex mu_;
  std::map<std::string, PeerState> state_;
  std::set<std::string> acked_;  // peers that have seen our current incarnation

  std::mutex stop_mu_;
  std::condition_variable stop_cv_;
  bool stopping_ = false;
  std::thread sender_, ticker_;
};

}  // namespace dkv
