// Structured event log: one JSON object per line (LLD §10). These events drive experiment metrics.
//   events.emit(Ev("fd.transition").str("peer", "C").str("to", "DOWN").i64("missed", 3));
#pragma once

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "common/clock.h"
#include "common/status.h"

namespace dkv {

class Ev {
 public:
  explicit Ev(const std::string& name);
  Ev& str(const std::string& k, const std::string& v);
  Ev& i64(const std::string& k, int64_t v);
  Ev& u64(const std::string& k, uint64_t v);
  Ev& dbl(const std::string& k, double v);
  Ev& boolean(const std::string& k, bool v);
  const std::string& name() const { return name_; }
  const std::string& fields() const { return fields_; }

 private:
  std::string name_;
  std::string fields_;  // ,"k":v ...
};

std::string JsonEscape(const std::string& s);

class EventLog {
 public:
  EventLog() = default;
  ~EventLog();
  // proc: e.g. "node-A". path empty → events are dropped (tests).
  Status open(const std::string& path, const std::string& proc, Clock* clock);
  void emit(const Ev& ev);
  void flush();
  void close();

 private:
  void flusher();
  std::mutex mu_;
  std::condition_variable cv_;
  std::string buffer_;
  FILE* file_ = nullptr;
  std::string proc_;
  Clock* clock_ = nullptr;
  std::thread thread_;
  bool stopping_ = false;
};

}  // namespace dkv
