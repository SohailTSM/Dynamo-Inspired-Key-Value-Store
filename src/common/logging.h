// Minimal thread-safe logger: DKV_LOG(INFO) << "text";  Lines go to stderr with wall time + level.
#pragma once

#include <sstream>
#include <string>

namespace dkv {

enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

void SetLogLevel(LogLevel level);
LogLevel GetLogLevel();
void SetLogPrefix(const std::string& prefix);  // e.g. process id "node-A"

class LogLine {
 public:
  LogLine(LogLevel level, const char* file, int line);
  ~LogLine();
  template <typename T>
  LogLine& operator<<(const T& v) {
    if (enabled_) ss_ << v;
    return *this;
  }

 private:
  bool enabled_;
  LogLevel level_;
  std::ostringstream ss_;
};

}  // namespace dkv

#define DKV_LOG(level) ::dkv::LogLine(::dkv::LogLevel::level, __FILE__, __LINE__)
