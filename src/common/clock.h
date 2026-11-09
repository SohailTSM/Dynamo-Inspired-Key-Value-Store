// Injectable clock (LLD §1): wall time for version timestamps and logs, monotonic time for durations.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace dkv {

class Clock {
 public:
  virtual ~Clock() = default;
  virtual int64_t now_wall_ms() const = 0;
  virtual int64_t now_mono_ms() const = 0;
  virtual int64_t now_mono_us() const = 0;
};

class SystemClock final : public Clock {
 public:
  int64_t now_wall_ms() const override {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
  }
  int64_t now_mono_ms() const override {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  }
  int64_t now_mono_us() const override {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
  }
  static SystemClock& Instance() {
    static SystemClock c;
    return c;
  }
};

// Manually advanced clock for unit tests.
class FakeClock final : public Clock {
 public:
  explicit FakeClock(int64_t start_ms = 1'000'000) : wall_(start_ms), mono_(start_ms) {}
  int64_t now_wall_ms() const override { return wall_.load(); }
  int64_t now_mono_ms() const override { return mono_.load(); }
  int64_t now_mono_us() const override { return mono_.load() * 1000; }
  void advance_ms(int64_t ms) {
    wall_ += ms;
    mono_ += ms;
  }

 private:
  std::atomic<int64_t> wall_;
  std::atomic<int64_t> mono_;
};

}  // namespace dkv
