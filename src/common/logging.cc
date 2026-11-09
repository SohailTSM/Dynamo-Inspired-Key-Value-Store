#include "common/logging.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace dkv {
namespace {
std::atomic<int> g_level{static_cast<int>(LogLevel::INFO)};
std::mutex g_mu;
std::string g_prefix;  // guarded by g_mu

const char* LevelName(LogLevel l) {
  switch (l) {
    case LogLevel::DEBUG: return "D";
    case LogLevel::INFO: return "I";
    case LogLevel::WARN: return "W";
    case LogLevel::ERROR: return "E";
  }
  return "?";
}

const char* Basename(const char* path) {
  const char* b = path;
  for (const char* p = path; *p; ++p)
    if (*p == '/') b = p + 1;
  return b;
}
}  // namespace

void SetLogLevel(LogLevel level) { g_level = static_cast<int>(level); }
LogLevel GetLogLevel() { return static_cast<LogLevel>(g_level.load()); }
void SetLogPrefix(const std::string& prefix) {
  std::lock_guard<std::mutex> l(g_mu);
  g_prefix = prefix;
}

LogLine::LogLine(LogLevel level, const char* file, int line)
    : enabled_(static_cast<int>(level) >= g_level.load()), level_(level) {
  if (enabled_) ss_ << Basename(file) << ":" << line << "] ";
}

LogLine::~LogLine() {
  if (!enabled_) return;
  using namespace std::chrono;
  auto now = system_clock::now();
  std::time_t t = system_clock::to_time_t(now);
  int ms = int(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
  std::tm tm{};
  localtime_r(&t, &tm);
  char ts[32];
  std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
  std::lock_guard<std::mutex> l(g_mu);
  std::fprintf(stderr, "%s.%03d %s %s %s\n", ts, ms, LevelName(level_), g_prefix.c_str(),
               ss_.str().c_str());
}

}  // namespace dkv
