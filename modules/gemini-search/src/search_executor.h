#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class SearchExecutor {
  std::mutex mu_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> queue_;
  std::vector<std::thread> workers_;
  bool stopping_ = false;
  bool started_ = false;
  size_t num_threads_;

  void EnsureStartedLocked();
  void WorkerLoop();

 public:
  explicit SearchExecutor(size_t num_threads = 2);
  ~SearchExecutor();

  SearchExecutor(const SearchExecutor&) = delete;
  SearchExecutor& operator=(const SearchExecutor&) = delete;

  void Submit(std::function<void()> task);
  void Shutdown();
  size_t NumThreads() const { return num_threads_; }
};
