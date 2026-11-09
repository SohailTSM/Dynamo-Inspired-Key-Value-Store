// Talks to the Auto-Admin: Register, heartbeats (with suspects), repair reports (LLD §5.5).
// Never blocks the data path: everything runs on its own thread with retries.
#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "admin.grpc.pb.h"
#include "common/clock.h"
#include "node/failure_detector.h"

namespace dkv {

class AdminReporter {
 public:
  AdminReporter(std::string admin_address, std::string self, uint64_t incarnation,
                int64_t process_start_wall_ms, uint32_t interval_ms, FailureDetector& fd, Clock& clock);
  ~AdminReporter();
  void start();
  void stop();
  void report_repair(const wire::RepairReport& report);  // queued, retried
  bool registered() const;

 private:
  void loop();
  bool try_register();
  const std::string self_;
  const uint64_t incarnation_;
  const int64_t start_wall_ms_;
  const uint32_t interval_ms_;
  FailureDetector& fd_;
  Clock& clock_;
  std::unique_ptr<wire::AdminService::Stub> stub_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<wire::RepairReport> pending_;
  bool registered_ = false;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace dkv
