// QuorumTracker (LLD §5.5): counts successful replies for one request and lets the coordinator wait
// for `need` of them until a deadline. Shared via shared_ptr because RPC callbacks may outlive the
// request that created it.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dkv {

template <typename T>
class QuorumTracker {
 public:
  // need: successes required; expected: replies that can possibly arrive.
  QuorumTracker(uint32_t need, uint32_t expected) : need_(need), expected_(expected) {}

  void add(bool ok, const std::string& from, T reply) {
    {
      std::lock_guard<std::mutex> l(mu_);
      ++responded_;
      if (ok) {
        ++ok_;
        replies_.emplace_back(from, std::move(reply));
      } else {
        failed_.push_back(from);
      }
    }
    cv_.notify_all();
  }

  // Returns true once `need` successes arrived; false at the deadline or when it became impossible.
  bool wait_until(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> l(mu_);
    cv_.wait_until(l, deadline, [this] { return ok_ >= need_ || responded_ >= expected_; });
    return ok_ >= need_;
  }

  uint32_t ok_count() const {
    std::lock_guard<std::mutex> l(mu_);
    return ok_;
  }
  std::vector<std::pair<std::string, T>> replies() const {
    std::lock_guard<std::mutex> l(mu_);
    return replies_;
  }
  std::vector<std::string> failed() const {
    std::lock_guard<std::mutex> l(mu_);
    return failed_;
  }

 private:
  const uint32_t need_;
  const uint32_t expected_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  uint32_t ok_ = 0;
  uint32_t responded_ = 0;
  std::vector<std::pair<std::string, T>> replies_;
  std::vector<std::string> failed_;
};

}  // namespace dkv
