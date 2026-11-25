#include "search_executor.h"

SearchExecutor::SearchExecutor(size_t num_threads)
    : num_threads_(num_threads > 0 ? num_threads : 2) {}

SearchExecutor::~SearchExecutor() { Shutdown(); }

void SearchExecutor::EnsureStartedLocked() {
  if (started_ || stopping_) return;
  started_ = true;
  workers_.reserve(num_threads_);
  for (size_t i = 0; i < num_threads_; i++) {
    workers_.emplace_back([this]() { WorkerLoop(); });
  }
}

void SearchExecutor::WorkerLoop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this]() { return stopping_ || !queue_.empty(); });
      if (stopping_ && queue_.empty()) return;
      task = std::move(queue_.front());
      queue_.pop();
    }
    if (task) task();
  }
}

void SearchExecutor::Submit(std::function<void()> task) {
  {
    std::unique_lock<std::mutex> lk(mu_);
    if (stopping_) return;
    EnsureStartedLocked();
    queue_.push(std::move(task));
  }
  cv_.notify_one();
}

void SearchExecutor::Shutdown() {
  {
    std::unique_lock<std::mutex> lk(mu_);
    if (stopping_) return;
    stopping_ = true;
  }
  cv_.notify_all();
  for (auto& t : workers_) {
    if (t.joinable()) t.join();
  }
  workers_.clear();
}
