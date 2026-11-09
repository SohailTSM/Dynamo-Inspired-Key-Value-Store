// Append-only, segmented version log with group commit (LLD §5.3, §6.3, §7.4).
// A caller's future resolves only after the batch containing its record was fdatasync'ed.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>

#include "common.pb.h"
#include "common/status.h"
#include "storage/segment.h"
#include "version/version.h"

namespace dkv {

struct RecoveryStats {
  uint64_t segments = 0;
  uint64_t records = 0;
  uint64_t truncated_bytes = 0;
  bool header_rewritten = false;
};

class VersionLog {
 public:
  using ReplayFn = std::function<void(const wire::LogRecord&, const Location&)>;

  VersionLog(std::string dir, uint64_t segment_bytes);
  ~VersionLog();

  // Replays every valid record (in log order), truncates a torn tail in the last segment, fails on
  // corruption in a sealed segment, then starts the committer thread.
  Status open(const ReplayFn& replay, RecoveryStats* stats);
  void close();  // drains the queue, syncs, joins

  std::future<StatusOr<Location>> append(const wire::LogRecord& rec);
  StatusOr<wire::LogRecord> read(const Location& loc) const;

  uint64_t fsync_count() const { return fsyncs_.load(); }
  uint64_t records_appended() const { return appended_.load(); }
  uint64_t bytes_appended() const { return bytes_.load(); }

 private:
  struct Pending {
    std::string frame;
    std::promise<StatusOr<Location>> done;
  };
  void commit_loop();
  Status rollover_locked();  // requires seg_mu_ exclusive

  const std::string dir_;
  const uint64_t segment_bytes_;

  mutable std::shared_mutex seg_mu_;  // guards segments_ map structure
  std::map<uint32_t, std::unique_ptr<Segment>> segments_;
  Segment* active_ = nullptr;

  std::mutex q_mu_;
  std::condition_variable q_cv_;
  std::deque<Pending> queue_;
  bool stopping_ = false;
  bool open_ = false;
  std::thread committer_;

  std::atomic<uint64_t> fsyncs_{0}, appended_{0}, bytes_{0};
};

}  // namespace dkv
