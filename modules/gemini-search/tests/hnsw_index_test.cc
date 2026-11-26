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

TEST(HnswIndexTest, SerializeDeserializeRoundTripAllQuantizations) {
  constexpr size_t N = 200;
  constexpr size_t DIM = 32;
  std::mt19937 rng(6201);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  // Empty graph round-trip
  {
    HnswIndex empty_src(DIM, DistanceMetric::kL2, 16, 100, VectorQuantization::kNone);
    std::string blob = empty_src.SerializeGraph();
    HnswIndex empty_dst(DIM, DistanceMetric::kL2, 16, 100, VectorQuantization::kNone);
    EXPECT_TRUE(empty_dst.DeserializeGraph(blob.data(), blob.size()));
    EXPECT_EQ(empty_dst.Size(), 0u);
  }

  for (VectorQuantization q :
       {VectorQuantization::kNone, VectorQuantization::kSq8, VectorQuantization::kBq}) {
    for (DistanceMetric metric :
         {DistanceMetric::kL2, DistanceMetric::kCosine, DistanceMetric::kIP}) {
      HnswIndex src(DIM, metric, 12, 80, q);
      src.SetEfRuntime(32);

      std::vector<float> buf(DIM);
      for (size_t i = 0; i < N; i++) {
        for (size_t d = 0; d < DIM; d++) buf[d] = dist(rng);
        src.Add("d" + std::to_string(i), buf.data());
      }
      // Delete every 4th node (including node 0) to exercise tombstone compaction
      for (size_t i = 0; i < N; i += 4) {
        src.Remove("d" + std::to_string(i));
      }
      ASSERT_EQ(src.Size(), N - N / 4);

      std::string blob = src.SerializeGraph();
      HnswIndex dst(DIM, metric, 12, 80, q);
      ASSERT_TRUE(dst.DeserializeGraph(blob.data(), blob.size()));
      EXPECT_EQ(dst.Size(), src.Size());
      EXPECT_EQ(dst.M(), src.M());
      EXPECT_EQ(dst.EfConstruction(), src.EfConstruction());
      EXPECT_EQ(dst.EfRuntime(), src.EfRuntime());

      // Exact KNN-10 match across 10 random queries
      for (int qi = 0; qi < 10; qi++) {
        for (size_t d = 0; d < DIM; d++) buf[d] = dist(rng);
        auto r1 = src.KnnQuery(buf.data(), 10);
        auto r2 = dst.KnnQuery(buf.data(), 10);
        ASSERT_EQ(r1.size(), r2.size());
        for (size_t k = 0; k < r1.size(); k++) {
          EXPECT_EQ(r1[k].doc_id, r2[k].doc_id);
          EXPECT_FLOAT_EQ(r1[k].score, r2[k].score);
        }
      }

      // Post-reload mutations (Add + Remove) must continue to work cleanly
      for (size_t d = 0; d < DIM; d++) buf[d] = dist(rng);
      dst.Add("post_reload_doc", buf.data());
      EXPECT_EQ(dst.Size(), src.Size() + 1);
      EXPECT_TRUE(dst.Remove("post_reload_doc"));
      EXPECT_EQ(dst.Size(), src.Size());
    }
  }
}

TEST(HnswIndexTest, DeserializeGraphCorruptionAndFuzz) {
  constexpr size_t DIM = 8;
  HnswIndex src(DIM, DistanceMetric::kL2, 8, 40, VectorQuantization::kSq8);
  std::mt19937 rng(6202);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
  std::vector<float> buf(DIM);
  for (int i = 0; i < 20; i++) {
    for (size_t d = 0; d < DIM; d++) buf[d] = dist(rng);
    src.Add("n" + std::to_string(i), buf.data());
  }

  std::string valid = src.SerializeGraph();
  HnswIndex dst(DIM, DistanceMetric::kL2, 8, 40, VectorQuantization::kSq8);

  // Mismatched dim / metric / quantization must fail
  HnswIndex wrong_dim(DIM + 1, DistanceMetric::kL2, 8, 40, VectorQuantization::kSq8);
  EXPECT_FALSE(wrong_dim.DeserializeGraph(valid.data(), valid.size()));
  HnswIndex wrong_metric(DIM, DistanceMetric::kCosine, 8, 40, VectorQuantization::kSq8);
  EXPECT_FALSE(wrong_metric.DeserializeGraph(valid.data(), valid.size()));
  HnswIndex wrong_quant(DIM, DistanceMetric::kL2, 8, 40, VectorQuantization::kBq);
  EXPECT_FALSE(wrong_quant.DeserializeGraph(valid.data(), valid.size()));

  // Trailing garbage must fail
  std::string with_trailing = valid + std::string(1, '\0');
  EXPECT_FALSE(dst.DeserializeGraph(with_trailing.data(), with_trailing.size()));

  // Every byte-by-byte truncation must fail cleanly
  for (size_t cut = 0; cut < valid.size(); cut++) {
    EXPECT_FALSE(dst.DeserializeGraph(valid.data(), cut)) << "cut=" << cut;
  }

  // Deterministic bit-flip fuzzing must never crash or leak
  for (size_t pos = 0; pos < valid.size(); pos += 2) {
    std::string bad = valid;
    bad[pos] ^= static_cast<char>(0xFF);
    (void)dst.DeserializeGraph(bad.data(), bad.size());
  }
}

TEST(HnswIndexTest, BqTwoStageOversampleAndRerankBoostsRecall) {
  constexpr size_t N = 1200;
  constexpr size_t DIM = 768;
  constexpr size_t K = 10;
  std::mt19937 rng(6301);
  std::normal_distribution<float> ndist(0.0f, 1.0f);

  FlatVectorIndex gt_fp32(DIM, DistanceMetric::kCosine, VectorQuantization::kNone);
  FlatVectorIndex flat_bq(DIM, DistanceMetric::kCosine, VectorQuantization::kBq);
  FlatVectorIndex flat_bq_fp16(DIM, DistanceMetric::kCosine, VectorQuantization::kBq,
                               VectorRerankType::kFp16, 8);
  FlatVectorIndex flat_bq_sq8(DIM, DistanceMetric::kCosine, VectorQuantization::kBq,
                              VectorRerankType::kSq8, 8);
  HnswIndex hnsw_bq_fp16(DIM, DistanceMetric::kCosine, 16, 150, VectorQuantization::kBq,
                         VectorRerankType::kFp16, 8);
  HnswIndex hnsw_bq_sq8(DIM, DistanceMetric::kCosine, 16, 150, VectorQuantization::kBq,
                        VectorRerankType::kSq8, 8);

  // Generate clustered high-dimensional vectors (realistic embedding structure)
  constexpr size_t kClusters = 24;
  std::vector<std::vector<float>> centers(kClusters, std::vector<float>(DIM));
  for (size_t c = 0; c < kClusters; c++) {
    for (size_t d = 0; d < DIM; d++) centers[c][d] = ndist(rng);
  }
  std::vector<float> buf(DIM);
  for (size_t i = 0; i < N; i++) {
    const auto& ctr = centers[i % kClusters];
    for (size_t d = 0; d < DIM; d++) buf[d] = ctr[d] + 0.35f * ndist(rng);
    std::string id = "e" + std::to_string(i);
    gt_fp32.Add(id, buf.data());
    flat_bq.Add(id, buf.data());
    flat_bq_fp16.Add(id, buf.data());
    flat_bq_sq8.Add(id, buf.data());
    hnsw_bq_fp16.Add(id, buf.data());
    hnsw_bq_sq8.Add(id, buf.data());
  }

  auto measure_recall = [](const std::vector<KnnResult>& gt,
                           const std::vector<KnnResult>& got) {
    std::unordered_set<std::string> gt_set;
    for (const auto& r : gt) gt_set.insert(r.doc_id);
    size_t hits = 0;
    for (const auto& r : got) {
      if (gt_set.count(r.doc_id)) hits++;
    }
    return static_cast<double>(hits) / static_cast<double>(gt.size());
  };

  double r_bq = 0, r_flat_fp16 = 0, r_flat_sq8 = 0, r_hnsw_fp16 = 0, r_hnsw_sq8 = 0;
  constexpr int kQueries = 15;
  for (int q = 0; q < kQueries; q++) {
    const auto& ctr = centers[static_cast<size_t>(q) % kClusters];
    for (size_t d = 0; d < DIM; d++) buf[d] = ctr[d] + 0.35f * ndist(rng);
    auto gt = gt_fp32.KnnQuery(buf.data(), K);
    r_bq += measure_recall(gt, flat_bq.KnnQuery(buf.data(), K));
    r_flat_fp16 += measure_recall(gt, flat_bq_fp16.KnnQuery(buf.data(), K));
    r_flat_sq8 += measure_recall(gt, flat_bq_sq8.KnnQuery(buf.data(), K));
    r_hnsw_fp16 += measure_recall(gt, hnsw_bq_fp16.KnnQueryEf(buf.data(), K, 160, 8));
    r_hnsw_sq8 += measure_recall(gt, hnsw_bq_sq8.KnnQueryEf(buf.data(), K, 160, 8));
  }
  r_bq /= kQueries;
  r_flat_fp16 /= kQueries;
  r_flat_sq8 /= kQueries;
  r_hnsw_fp16 /= kQueries;
  r_hnsw_sq8 /= kQueries;

  EXPECT_GT(r_flat_fp16, r_bq);
  EXPECT_GT(r_flat_sq8, r_bq);
  EXPECT_GT(r_hnsw_fp16, r_bq);
  EXPECT_GT(r_hnsw_sq8, r_bq);
  EXPECT_GE(r_flat_fp16, 0.90) << "flat BQ+FP16 recall=" << r_flat_fp16 << " vs BQ=" << r_bq;
  EXPECT_GE(r_flat_sq8, 0.85) << "flat BQ+SQ8 recall=" << r_flat_sq8 << " vs BQ=" << r_bq;
  EXPECT_GE(r_hnsw_fp16, 0.80) << "hnsw BQ+FP16 recall=" << r_hnsw_fp16 << " vs BQ=" << r_bq;
  EXPECT_GE(r_hnsw_sq8, 0.80) << "hnsw BQ+SQ8 recall=" << r_hnsw_sq8 << " vs BQ=" << r_bq;

  // Serialize & deserialize round-trip for BQ+FP16 and BQ+SQ8 HNSW graphs
  {
    std::string b_fp16 = hnsw_bq_fp16.SerializeGraph();
    HnswIndex restored_fp16(DIM, DistanceMetric::kCosine, 16, 100, VectorQuantization::kBq,
                            VectorRerankType::kFp16, 8);
    ASSERT_TRUE(restored_fp16.DeserializeGraph(b_fp16.data(), b_fp16.size()));
    auto orig = hnsw_bq_fp16.KnnQueryEf(buf.data(), K, 64, 8);
    auto post = restored_fp16.KnnQueryEf(buf.data(), K, 64, 8);
    ASSERT_EQ(orig.size(), post.size());
    for (size_t i = 0; i < orig.size(); i++) {
      EXPECT_EQ(orig[i].doc_id, post[i].doc_id);
      EXPECT_FLOAT_EQ(orig[i].score, post[i].score);
    }
  }
  {
    std::string b_sq8 = hnsw_bq_sq8.SerializeGraph();
    HnswIndex restored_sq8(DIM, DistanceMetric::kCosine, 16, 100, VectorQuantization::kBq,
                           VectorRerankType::kSq8, 8);
    ASSERT_TRUE(restored_sq8.DeserializeGraph(b_sq8.data(), b_sq8.size()));
    auto orig = hnsw_bq_sq8.KnnQueryEf(buf.data(), K, 64, 8);
    auto post = restored_sq8.KnnQueryEf(buf.data(), K, 64, 8);
    ASSERT_EQ(orig.size(), post.size());
    for (size_t i = 0; i < orig.size(); i++) {
      EXPECT_EQ(orig[i].doc_id, post[i].doc_id);
      EXPECT_FLOAT_EQ(orig[i].score, post[i].score);
    }
  }
}

TEST(HnswIndexTest, AdaptiveFilteredHnswSelectivityAndRecall) {
  constexpr size_t N = 1600;
  constexpr size_t DIM = 32;
  constexpr size_t K = 10;
  std::mt19937 rng(6501);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  HnswIndex hnsw(DIM, DistanceMetric::kL2, 16, 150, VectorQuantization::kNone);
  FlatVectorIndex flat(DIM, DistanceMetric::kL2, VectorQuantization::kNone);
  HnswIndex hnsw_bq_fp16(DIM, DistanceMetric::kCosine, 16, 150, VectorQuantization::kBq,
                         VectorRerankType::kFp16, 6);

  std::vector<float> buf(DIM);
  std::vector<std::string> cands_small;   // ~2% (brute-force path)
  std::vector<std::string> cands_medium;  // 35% (Filtered HNSW path)
  std::vector<std::string> cands_large;   // 70% (Filtered HNSW path)
  std::unordered_set<std::string> set_small, set_medium, set_large;

  for (size_t i = 0; i < N; i++) {
    for (size_t d = 0; d < DIM; d++) buf[d] = dist(rng);
    std::string id = "f" + std::to_string(i);
    hnsw.Add(id, buf.data());
    flat.Add(id, buf.data());
    hnsw_bq_fp16.Add(id, buf.data());
    if (i % 50 == 0) {
      cands_small.push_back(id);
      set_small.insert(id);
    }
    if (i % 100 < 35) {
      cands_medium.push_back(id);
      set_medium.insert(id);
    }
    if (i % 10 < 7) {
      cands_large.push_back(id);
      set_large.insert(id);
    }
  }
  // Include duplicate and non-existent IDs to test robustness
  cands_medium.push_back(cands_medium.front());
  cands_medium.push_back("nonexistent_doc");

  auto check_filtered = [&](const std::vector<std::string>& cands,
                            const std::unordered_set<std::string>& allowed,
                            double min_recall) {
    double total_recall = 0.0;
    constexpr int kQueries = 15;
    for (int q = 0; q < kQueries; q++) {
      for (size_t d = 0; d < DIM; d++) buf[d] = dist(rng);
      auto hr = hnsw.KnnQueryFiltered(buf.data(), K, cands, 64);
      auto fr = flat.KnnQueryFiltered(buf.data(), K, cands);
      ASSERT_EQ(hr.size(), K);
      std::unordered_set<std::string> gt;
      for (const auto& r : fr) gt.insert(r.doc_id);
      size_t hits = 0;
      for (size_t i = 0; i < hr.size(); i++) {
        EXPECT_TRUE(allowed.count(hr[i].doc_id) > 0) << "Returned non-matching doc " << hr[i].doc_id;
        if (i > 0) EXPECT_LE(hr[i - 1].score, hr[i].score + 1e-6f);
        if (gt.count(hr[i].doc_id)) hits++;
      }
      total_recall += static_cast<double>(hits) / static_cast<double>(K);
    }
    EXPECT_GE(total_recall / kQueries, min_recall);
  };

  check_filtered(cands_small, set_small, 1.0);
  check_filtered(cands_medium, set_medium, 0.85);
  check_filtered(cands_large, set_large, 0.90);

  // Filtered HNSW with BQ + FP16 Rerank
  for (size_t d = 0; d < DIM; d++) buf[d] = dist(rng);
  auto br = hnsw_bq_fp16.KnnQueryFiltered(buf.data(), K, cands_large, 64, 4);
  ASSERT_EQ(br.size(), K);
  for (const auto& r : br) {
    EXPECT_TRUE(set_large.count(r.doc_id) > 0);
  }
}
