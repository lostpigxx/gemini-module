#include <gtest/gtest.h>
#include "hnsw_index.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

TEST(HnswIndexTest, AddAndKnnL2) {
  HnswIndex idx(3, DistanceMetric::kL2, 4, 50);
  float v1[] = {0, 0, 0};
  float v2[] = {1, 0, 0};
  float v3[] = {10, 0, 0};
  idx.Add("close", v1);
  idx.Add("mid", v2);
  idx.Add("far", v3);

  float query[] = {0, 0, 0};
  idx.SetEfRuntime(10);
  auto results = idx.KnnQuery(query, 2);
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].doc_id, "close");
  EXPECT_EQ(results[1].doc_id, "mid");
  EXPECT_FLOAT_EQ(results[0].score, 0.0f);
  EXPECT_FLOAT_EQ(results[1].score, 1.0f);
}

TEST(HnswIndexTest, KnnCosine) {
  HnswIndex idx(2, DistanceMetric::kCosine, 4, 50);
  float v1[] = {1, 0};
  float v2[] = {0, 1};
  float v3[] = {0.7f, 0.7f};
  idx.Add("x_axis", v1);
  idx.Add("y_axis", v2);
  idx.Add("diagonal", v3);

  float query[] = {1, 0};
  idx.SetEfRuntime(10);
  auto results = idx.KnnQuery(query, 3);
  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(results[0].doc_id, "x_axis");
  EXPECT_NEAR(results[0].score, 0.0f, 1e-5f);
}

TEST(HnswIndexTest, KnnIP) {
  HnswIndex idx(2, DistanceMetric::kIP, 4, 50);
  float v1[] = {1, 0};
  float v2[] = {0, 1};
  float v3[] = {1, 1};
  idx.Add("a", v1);
  idx.Add("b", v2);
  idx.Add("c", v3);

  float query[] = {1, 1};
  idx.SetEfRuntime(10);
  auto results = idx.KnnQuery(query, 3);
  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(results[0].doc_id, "c");
  EXPECT_FLOAT_EQ(results[0].score, -2.0f);
}

TEST(HnswIndexTest, EmptyIndex) {
  HnswIndex idx(3, DistanceMetric::kL2);
  float query[] = {1, 2, 3};
  auto results = idx.KnnQuery(query, 5);
  EXPECT_TRUE(results.empty());
}

TEST(HnswIndexTest, SingleElement) {
  HnswIndex idx(2, DistanceMetric::kL2, 4, 50);
  float v[] = {3, 4};
  idx.Add("only", v);
  EXPECT_EQ(idx.Size(), 1u);

  float query[] = {0, 0};
  auto results = idx.KnnQuery(query, 10);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].doc_id, "only");
  EXPECT_FLOAT_EQ(results[0].score, 25.0f);
}

TEST(HnswIndexTest, KGreaterThanSize) {
  HnswIndex idx(2, DistanceMetric::kL2, 4, 50);
  float v1[] = {0, 0};
  float v2[] = {1, 1};
  idx.Add("a", v1);
  idx.Add("b", v2);

  float query[] = {0, 0};
  auto results = idx.KnnQuery(query, 100);
  ASSERT_EQ(results.size(), 2u);
}

TEST(HnswIndexTest, RemoveDoc) {
  HnswIndex idx(2, DistanceMetric::kL2, 4, 50);
  float v1[] = {0, 0};
  float v2[] = {1, 1};
  float v3[] = {2, 2};
  idx.Add("a", v1);
  idx.Add("b", v2);
  idx.Add("c", v3);

  EXPECT_TRUE(idx.Remove("a"));
  EXPECT_FALSE(idx.Remove("a"));
  EXPECT_EQ(idx.Size(), 2u);

  float query[] = {0, 0};
  idx.SetEfRuntime(10);
  auto results = idx.KnnQuery(query, 10);
  ASSERT_EQ(results.size(), 2u);
  for (auto& r : results) {
    EXPECT_NE(r.doc_id, "a");
  }
}

TEST(HnswIndexTest, ReplaceDoc) {
  HnswIndex idx(2, DistanceMetric::kL2, 4, 50);
  float v1[] = {0, 0};
  idx.Add("doc", v1);
  float v2[] = {10, 10};
  idx.Add("doc", v2);
  EXPECT_EQ(idx.Size(), 1u);

  float query[] = {0, 0};
  auto results = idx.KnnQuery(query, 1);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].doc_id, "doc");
  EXPECT_FLOAT_EQ(results[0].score, 200.0f);
}

TEST(HnswIndexTest, DeleteAndReinsert) {
  HnswIndex idx(2, DistanceMetric::kL2, 4, 50);
  float v1[] = {1, 0};
  float v2[] = {0, 1};
  idx.Add("a", v1);
  idx.Add("b", v2);

  idx.Remove("a");
  EXPECT_EQ(idx.Size(), 1u);

  float v3[] = {0.5f, 0.5f};
  idx.Add("a", v3);
  EXPECT_EQ(idx.Size(), 2u);

  float query[] = {0.5f, 0.5f};
  idx.SetEfRuntime(10);
  auto results = idx.KnnQuery(query, 1);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].doc_id, "a");
}

TEST(HnswIndexTest, SizeTracking) {
  HnswIndex idx(2, DistanceMetric::kL2, 4, 50);
  EXPECT_EQ(idx.Size(), 0u);
  float v[] = {1, 2};
  idx.Add("a", v);
  EXPECT_EQ(idx.Size(), 1u);
  idx.Add("b", v);
  EXPECT_EQ(idx.Size(), 2u);
  idx.Remove("a");
  EXPECT_EQ(idx.Size(), 1u);
}

TEST(HnswIndexTest, Accessors) {
  HnswIndex idx(128, DistanceMetric::kCosine, 32, 400);
  EXPECT_EQ(idx.Dim(), 128u);
  EXPECT_EQ(idx.Metric(), DistanceMetric::kCosine);
  EXPECT_EQ(idx.M(), 32u);
  EXPECT_EQ(idx.EfConstruction(), 400u);
  EXPECT_EQ(idx.EfRuntime(), 10u);
  idx.SetEfRuntime(100);
  EXPECT_EQ(idx.EfRuntime(), 100u);
}

TEST(HnswIndexTest, Clear) {
  HnswIndex idx(2, DistanceMetric::kL2, 4, 50);
  float v[] = {1, 2};
  idx.Add("a", v);
  idx.Add("b", v);
  idx.Clear();
  EXPECT_EQ(idx.Size(), 0u);
  auto results = idx.KnnQuery(v, 5);
  EXPECT_TRUE(results.empty());
}

TEST(HnswIndexTest, KnnQueryFiltered) {
  HnswIndex idx(2, DistanceMetric::kL2, 4, 50);
  float v1[] = {0, 0};
  float v2[] = {1, 0};
  float v3[] = {10, 0};
  float v4[] = {20, 0};
  idx.Add("a", v1);
  idx.Add("b", v2);
  idx.Add("c", v3);
  idx.Add("d", v4);

  float query[] = {0, 0};
  std::vector<std::string> candidates = {"b", "d"};
  auto results = idx.KnnQueryFiltered(query, 2, candidates);
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].doc_id, "b");
  EXPECT_EQ(results[1].doc_id, "d");
}

TEST(HnswIndexTest, RecallMeasurement) {
  constexpr size_t N = 500;
  constexpr size_t DIM = 32;
  constexpr size_t K = 10;

  std::mt19937 rng(42);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  HnswIndex hnsw(DIM, DistanceMetric::kL2, 16, 200);
  FlatVectorIndex flat(DIM, DistanceMetric::kL2);

  std::vector<std::vector<float>> data(N);
  for (size_t i = 0; i < N; i++) {
    data[i].resize(DIM);
    for (size_t d = 0; d < DIM; d++) data[i][d] = dist(rng);
    std::string id = "doc" + std::to_string(i);
    hnsw.Add(id, data[i].data());
    flat.Add(id, data[i].data());
  }

  hnsw.SetEfRuntime(50);

  constexpr size_t NUM_QUERIES = 20;
  double total_recall = 0;
  for (size_t q = 0; q < NUM_QUERIES; q++) {
    std::vector<float> query(DIM);
    for (size_t d = 0; d < DIM; d++) query[d] = dist(rng);

    auto hnsw_results = hnsw.KnnQuery(query.data(), K);
    auto flat_results = flat.KnnQuery(query.data(), K);

    std::unordered_set<std::string> ground_truth;
    for (auto& r : flat_results) ground_truth.insert(r.doc_id);

    size_t hits = 0;
    for (auto& r : hnsw_results) {
      if (ground_truth.count(r.doc_id)) hits++;
    }
    total_recall += static_cast<double>(hits) / static_cast<double>(K);
  }

  double avg_recall = total_recall / static_cast<double>(NUM_QUERIES);
  EXPECT_GT(avg_recall, 0.95) << "Average recall: " << avg_recall;
}

TEST(HnswIndexTest, VaryingMSmall) {
  HnswIndex idx(3, DistanceMetric::kL2, 2, 20);
  float v1[] = {0, 0, 0};
  float v2[] = {1, 0, 0};
  float v3[] = {2, 0, 0};
  float v4[] = {3, 0, 0};
  idx.Add("a", v1);
  idx.Add("b", v2);
  idx.Add("c", v3);
  idx.Add("d", v4);
  EXPECT_EQ(idx.Size(), 4u);
  EXPECT_EQ(idx.M(), 2u);

  float query[] = {0, 0, 0};
  idx.SetEfRuntime(10);
  auto results = idx.KnnQuery(query, 2);
  ASSERT_GE(results.size(), 1u);
  EXPECT_EQ(results[0].doc_id, "a");
}

TEST(HnswIndexTest, ManyInsertDelete) {
  HnswIndex idx(4, DistanceMetric::kL2, 8, 50);
  std::mt19937 rng(123);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  for (int i = 0; i < 100; i++) {
    float v[4];
    for (int d = 0; d < 4; d++) v[d] = dist(rng);
    idx.Add("d" + std::to_string(i), v);
  }
  EXPECT_EQ(idx.Size(), 100u);

  for (int i = 0; i < 50; i++) {
    idx.Remove("d" + std::to_string(i * 2));
  }
  EXPECT_EQ(idx.Size(), 50u);

  float query[] = {0, 0, 0, 0};
  idx.SetEfRuntime(20);
  auto results = idx.KnnQuery(query, 5);
  EXPECT_EQ(results.size(), 5u);
  for (auto& r : results) {
    EXPECT_NE(r.doc_id, "");
  }
}

TEST(HnswIndexTest, DeleteEntryPoint) {
  HnswIndex idx(2, DistanceMetric::kL2, 4, 50);
  float v1[] = {0, 0};
  float v2[] = {1, 1};
  float v3[] = {2, 2};
  idx.Add("a", v1);
  idx.Add("b", v2);
  idx.Add("c", v3);

  std::string first_entry;
  {
    float q[] = {0, 0};
    auto r = idx.KnnQuery(q, 1);
    ASSERT_EQ(r.size(), 1u);
  }

  idx.Remove("a");
  idx.Remove("b");
  EXPECT_EQ(idx.Size(), 1u);

  float query[] = {0, 0};
  auto results = idx.KnnQuery(query, 5);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].doc_id, "c");
}

TEST(HnswIndexTest, DeleteAllThenAdd) {
  HnswIndex idx(2, DistanceMetric::kL2, 4, 50);
  float v1[] = {1, 0};
  float v2[] = {0, 1};
  idx.Add("a", v1);
  idx.Add("b", v2);
  idx.Remove("a");
  idx.Remove("b");
  EXPECT_EQ(idx.Size(), 0u);

  float v3[] = {3, 4};
  idx.Add("c", v3);
  EXPECT_EQ(idx.Size(), 1u);

  float query[] = {0, 0};
  auto results = idx.KnnQuery(query, 1);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].doc_id, "c");
}

TEST(HnswIndexTest, SlotReuseAndSq8QuantizationRecall) {
  constexpr size_t N = 200;
  constexpr size_t DIM = 32;
  std::mt19937 rng(777);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  HnswIndex hnsw(DIM, DistanceMetric::kL2, 12, 100, VectorQuantization::kSq8);
  FlatVectorIndex flat(DIM, DistanceMetric::kL2);

  std::vector<float> buf(DIM);
  for (size_t i = 0; i < N; i++) {
    for (size_t d = 0; d < DIM; d++) buf[d] = dist(rng);
    std::string id = "v" + std::to_string(i);
    hnsw.Add(id, buf.data());
    flat.Add(id, buf.data());
  }
  // Remove half and re-add new vectors to exercise free_slots_ reuse
  for (size_t i = 0; i < N; i += 2) {
    std::string id = "v" + std::to_string(i);
    EXPECT_TRUE(hnsw.Remove(id));
    flat.Remove(id);
  }
  for (size_t i = 0; i < N; i += 2) {
    for (size_t d = 0; d < DIM; d++) buf[d] = dist(rng);
    std::string id = "n" + std::to_string(i);
    hnsw.Add(id, buf.data());
    flat.Add(id, buf.data());
  }
  EXPECT_EQ(hnsw.Size(), N);

  hnsw.SetEfRuntime(50);
  double recall = 0.0;
  for (int q = 0; q < 10; q++) {
    for (size_t d = 0; d < DIM; d++) buf[d] = dist(rng);
    auto hr = hnsw.KnnQuery(buf.data(), 10);
    auto fr = flat.KnnQuery(buf.data(), 10);
    std::unordered_set<std::string> gt;
    for (auto& r : fr) gt.insert(r.doc_id);
    size_t hits = 0;
    for (auto& r : hr) if (gt.count(r.doc_id)) hits++;
    recall += static_cast<double>(hits) / 10.0;
  }
  EXPECT_GT(recall / 10.0, 0.85);
}

TEST(HnswIndexTest, ConcurrentReadSafety) {
  constexpr size_t DIM = 32;
  constexpr size_t N = 400;
  HnswIndex hnsw(DIM, DistanceMetric::kL2, 12, 80);
  std::mt19937 rng(777);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
  std::vector<float> buf(DIM);
  for (size_t i = 0; i < N; i++) {
    for (size_t d = 0; d < DIM; d++) buf[d] = dist(rng);
    hnsw.Add("c" + std::to_string(i), buf.data());
  }
  hnsw.SetEfRuntime(32);

  std::vector<std::thread> threads;
  std::vector<int> ok_counts(4, 0);
  for (int t = 0; t < 4; t++) {
    threads.emplace_back([&, t]() {
      std::mt19937 trng(1000 + t);
      std::vector<float> q(DIM);
      for (int iter = 0; iter < 150; iter++) {
        for (size_t d = 0; d < DIM; d++) q[d] = dist(trng);
        auto res = hnsw.KnnQuery(q.data(), 5);
        if (res.size() == 5) ok_counts[t]++;
      }
    });
  }
  for (auto& th : threads) th.join();
  for (int t = 0; t < 4; t++) {
    EXPECT_EQ(ok_counts[t], 150);
  }
}

TEST(HnswIndexTest, BqQuantizationRecallAndConsistency) {
  constexpr size_t N = 400;
  constexpr size_t DIM = 128;
  std::mt19937 rng(2026);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  HnswIndex hnsw(DIM, DistanceMetric::kCosine, 16, 120, VectorQuantization::kBq);
  FlatVectorIndex flat(DIM, DistanceMetric::kCosine, VectorQuantization::kBq);

  std::vector<float> buf(DIM);
  std::vector<std::string> subset;
  for (size_t i = 0; i < N; i++) {
    for (size_t d = 0; d < DIM; d++) buf[d] = dist(rng);
    std::string id = "bq" + std::to_string(i);
    hnsw.Add(id, buf.data());
    flat.Add(id, buf.data());
    if (i % 5 == 0) subset.push_back(id);
  }

  hnsw.SetEfRuntime(64);
  double total_recall = 0.0;
  for (int q = 0; q < 15; q++) {
    for (size_t d = 0; d < DIM; d++) buf[d] = dist(rng);
    auto hr = hnsw.KnnQuery(buf.data(), 10);
    auto fr = flat.KnnQuery(buf.data(), 10);
    ASSERT_EQ(hr.size(), 10u);
    // Verify ascending distance order
    for (size_t i = 1; i < hr.size(); i++) {
      EXPECT_LE(hr[i - 1].score, hr[i].score);
    }
    // Compare top-10 worst distance against flat BQ top-10 worst distance
    float flat_kth = fr.back().score;
    size_t within_kth = 0;
    for (const auto& r : hr) {
      if (r.score <= flat_kth + 1e-6f) within_kth++;
    }
    total_recall += static_cast<double>(within_kth) / 10.0;

    // RangeQuery and KnnQueryFiltered must match Flat BQ exactly on scores
    auto hrange = hnsw.RangeQuery(buf.data(), 0.45f);
    auto frange = flat.RangeQuery(buf.data(), 0.45f);
    EXPECT_EQ(hrange.size(), frange.size());

    auto hfilt = hnsw.KnnQueryFiltered(buf.data(), 5, subset);
    auto ffilt = flat.KnnQueryFiltered(buf.data(), 5, subset);
    ASSERT_EQ(hfilt.size(), ffilt.size());
    for (size_t i = 0; i < hfilt.size(); i++) {
      EXPECT_FLOAT_EQ(hfilt[i].score, ffilt[i].score);
    }
  }
  EXPECT_GT(total_recall / 15.0, 0.90);
}
