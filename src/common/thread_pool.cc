#include "common/thread_pool.h"

namespace dkv {

ThreadPool::ThreadPool(size_t threads) {
  workers_.reserve(threads);
  for (size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { worker(); });
}

ThreadPool::~ThreadPool() { stop(); }

bool ThreadPool::submit(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> l(mu_);
    if (stopping_) return false;
    tasks_.push_back(std::move(task));
  }
  cv_.notify_one();
  return true;
}

void ThreadPool::stop() {
  {
    std::lock_guard<std::mutex> l(mu_);
    if (stopping_ && workers_.empty()) return;
    stopping_ = true;
  }
  cv_.notify_all();
  for (auto& t : workers_)
    if (t.joinable()) t.join();
  workers_.clear();
}

size_t ThreadPool::queued() const {
  std::lock_guard<std::mutex> l(mu_);
  return tasks_.size();
}

void ThreadPool::worker() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> l(mu_);
      cv_.wait(l, [this] { return stopping_ || !tasks_.empty(); });
      if (tasks_.empty()) return;  // stopping and drained
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    task();
  }
}

}  // namespace dkv
