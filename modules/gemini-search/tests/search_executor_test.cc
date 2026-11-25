#include "search_executor.h"
#include "document_store.h"
#include "hnsw_index.h"
#include "text_index.h"
#include "vector_index.h"

#include <gtest/gtest.h>
#include <atomic>
#include <cmath>
#include <shared_mutex>
#include <string>
#include <vector>

TEST(SearchExecutorTest, ExecutesAllSubmittedTasks) {
  SearchExecutor exec(4);
  std::atomic<int> counter{0};
  constexpr int kTasks = 200;

  for (int i = 0; i < kTasks; i++) {
    exec.Submit([&]() { counter.fetch_add(1, std::memory_order_relaxed); });
  }
  exec.Shutdown();
  EXPECT_EQ(counter.load(), kTasks);
}

TEST(SearchExecutorTest, ConcurrentReadersAndWriterUnderSharedMutex) {
  SearchExecutor exec(4);
  std::shared_mutex rwlock;
  TextIndex text_idx;
  HnswIndex vec_idx(8, DistanceMetric::kL2, 8, 50);

  // Seed initial data
  for (int i = 0; i < 100; i++) {
    float v[8];
    for (int d = 0; d < 8; d++) v[d] = std::sin(static_cast<float>(i + d));
    std::string id = "d" + std::to_string(i);
    text_idx.Add(id, "redis search vector concurrent test " + std::to_string(i % 10));
    vec_idx.Add(id, v);
  }

  std::atomic<int> reads_done{0};

  // Launch 40 concurrent read tasks on the executor
  for (int r = 0; r < 40; r++) {
    exec.Submit([&, r]() {
      float q[8];
      for (int d = 0; d < 8; d++) q[d] = std::cos(static_cast<float>(r + d));
      for (int iter = 0; iter < 25; iter++) {
        std::shared_lock<std::shared_mutex> lk(rwlock);
        auto tr = text_idx.Search({"redis", "vector"});
        auto vr = vec_idx.KnnQuery(q, 5);
        EXPECT_FALSE(tr.empty());
        EXPECT_FALSE(vr.empty());
        reads_done.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // Concurrent writer on main thread while readers run
  for (int w = 0; w < 50; w++) {
    float v[8];
    for (int d = 0; d < 8; d++) v[d] = static_cast<float>(w + d) * 0.01f;
    std::string id = "w" + std::to_string(w);
    std::unique_lock<std::shared_mutex> lk(rwlock);
    text_idx.Add(id, "redis vector writer update");
    vec_idx.Add(id, v);
  }

  exec.Shutdown();
  EXPECT_EQ(reads_done.load(), 40 * 25);
  EXPECT_EQ(vec_idx.Size(), 150u);
}
