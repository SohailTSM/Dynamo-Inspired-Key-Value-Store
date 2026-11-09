#include "common/event_log.h"

#include <cinttypes>
#include <cstdio>

#include "common/fs.h"

namespace dkv {

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += char(c);
        }
    }
  }
  return out;
}

Ev::Ev(const std::string& name) : name_(name) {}
Ev& Ev::str(const std::string& k, const std::string& v) {
  fields_ += ",\"" + JsonEscape(k) + "\":\"" + JsonEscape(v) + "\"";
  return *this;
}
Ev& Ev::i64(const std::string& k, int64_t v) {
  fields_ += ",\"" + JsonEscape(k) + "\":" + std::to_string(v);
  return *this;
}
Ev& Ev::u64(const std::string& k, uint64_t v) {
  fields_ += ",\"" + JsonEscape(k) + "\":" + std::to_string(v);
  return *this;
}
Ev& Ev::dbl(const std::string& k, double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6g", v);
  fields_ += ",\"" + JsonEscape(k) + "\":" + buf;
  return *this;
}
Ev& Ev::boolean(const std::string& k, bool v) {
  fields_ += ",\"" + JsonEscape(k) + "\":" + (v ? "true" : "false");
  return *this;
}

EventLog::~EventLog() { close(); }

Status EventLog::open(const std::string& path, const std::string& proc, Clock* clock) {
  proc_ = proc;
  clock_ = clock;
  if (path.empty()) return Status::OK();
  DKV_RETURN_IF_ERROR(MakeDirs(DirName(path)));
  file_ = std::fopen(path.c_str(), "a");
  if (!file_) return ErrnoStatus("open event log " + path);
  thread_ = std::thread([this] { flusher(); });
  return Status::OK();
}

void EventLog::emit(const Ev& ev) {
  if (!clock_) return;
  std::string line = "{\"t_wall_ms\":" + std::to_string(clock_->now_wall_ms()) +
                     ",\"t_mono_ms\":" + std::to_string(clock_->now_mono_ms()) + ",\"proc\":\"" +
                     JsonEscape(proc_) + "\",\"event\":\"" + JsonEscape(ev.name()) + "\"" +
                     ev.fields() + "}\n";
  std::lock_guard<std::mutex> l(mu_);
  if (file_) buffer_ += line;
}

void EventLog::flush() {
  std::string data;
  {
    std::lock_guard<std::mutex> l(mu_);
    data.swap(buffer_);
    if (!file_) return;
    if (!data.empty()) {
      std::fwrite(data.data(), 1, data.size(), file_);
      std::fflush(file_);
    }
  }
}

void EventLog::flusher() {
  std::unique_lock<std::mutex> l(mu_);
  while (!stopping_) {
    cv_.wait_for(l, std::chrono::milliseconds(200));
    if (!buffer_.empty() && file_) {
      std::fwrite(buffer_.data(), 1, buffer_.size(), file_);
      std::fflush(file_);
      buffer_.clear();
    }
  }
}

void EventLog::close() {
  {
    std::lock_guard<std::mutex> l(mu_);
    if (!file_ && !thread_.joinable()) return;
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> l(mu_);
  if (file_) {
    if (!buffer_.empty()) std::fwrite(buffer_.data(), 1, buffer_.size(), file_);
    std::fclose(file_);
    file_ = nullptr;
    buffer_.clear();
  }
}

}  // namespace dkv
