// Fixed-size worker pool used for RPC fan-out (LLD §9).
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace dkv {

class ThreadPool {
 public:
  explicit ThreadPool(size_t threads);
  ~ThreadPool();
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Returns false if the pool is stopping (task not queued).
  bool submit(std::function<void()> task);
  void stop();  // finishes queued tasks, joins workers
  size_t queued() const;

 private:
  void worker();
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  std::vector<std::thread> workers_;
  bool stopping_ = false;
};

}  // namespace dkv
