// Auto-Admin cluster health view (LLD §5.8, HLD state machine state-admin-health) + repair tracking.
#pragma once

#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "admin.pb.h"
#include "common/clock.h"
#include "common/event_log.h"

namespace dkv {

const char* HealthName(wire::NodeHealth h);

class HealthTracker {
 public:
  struct Options {
    uint32_t interval_ms = 1000;
    uint32_t suspect_after = 1;
    uint32_t down_after = 3;
    uint32_t tick_ms = 100;
  };
  // nodes: id -> address
  HealthTracker(std::map<std::string, std::string> nodes, Options opts, Clock& clock, EventLog* events,
                const std::string& out_dir);
  ~HealthTracker();
  void start();
  void stop();

  void on_heartbeat(const wire::HeartbeatMsg& m);
  void on_register(const wire::RegisterRequest& r);
  void on_repair_report(const wire::RepairReport& r);

  wire::HealthSnapshot snapshot() const;
  wire::ClusterStatus status() const;
  // Blocks until the snapshot version differs from `after_version` or the timeout passes.
  wire::HealthSnapshot wait_for_change(uint64_t after_version, int64_t timeout_ms);
  void tick();  // public for tests

 private:
  struct NodeRecord {
    wire::NodeHealth health = wire::HEALTH_UNKNOWN;
    uint64_t incarnation = 0;
    int64_t last_hb_mono_ms = 0;
    int64_t last_hb_wall_ms = 0;
    std::set<std::string> reports_suspect;  // this node's latest suspect list
    std::string address;
  };
  void transition_locked(const std::string& node, NodeRecord& rec, wire::NodeHealth to, const std::string& why);
  bool alive(wire::NodeHealth h) const {
    return h == wire::UP || h == wire::SUSPECT || h == wire::RECOVERING || h == wire::UNREACHABLE_FROM_ADMIN;
  }

  const Options opts_;
  Clock& clock_;
  EventLog* events_;
  mutable std::mutex mu_;
  std::condition_variable changed_;
  std::map<std::string, NodeRecord> nodes_;
  std::map<std::string, wire::RepairReport> repairs_;  // latest DONE (or STARTED) per node
  uint64_t version_ = 1;
  FILE* health_csv_ = nullptr;
  FILE* repairs_csv_ = nullptr;

  std::mutex stop_mu_;
  std::condition_variable stop_cv_;
  bool stopping_ = false;
  std::thread ticker_;
};

}  // namespace dkv
