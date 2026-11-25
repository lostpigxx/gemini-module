#include <gtest/gtest.h>
#include "vector_index.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// =============================================================
// Distance functions
// =============================================================

TEST(DistanceTest, L2IdenticalVectorsZero) {
  float a[] = {1, 2, 3};
  EXPECT_FLOAT_EQ(L2Distance(a, a, 3), 0.0f);
}

TEST(DistanceTest, L2KnownValues) {
  float a[] = {0, 0, 0};
  float b[] = {3, 4, 0};
  EXPECT_FLOAT_EQ(L2Distance(a, b, 3), 25.0f);
}

TEST(DistanceTest, L2SingleDim) {
  float a[] = {5};
  float b[] = {8};
  EXPECT_FLOAT_EQ(L2Distance(a, b, 1), 9.0f);
}

TEST(DistanceTest, CosineParallelVectors) {
  float a[] = {1, 0, 0};
  float b[] = {2, 0, 0};
  EXPECT_NEAR(CosineDistance(a, b, 3), 0.0f, 1e-6f);
}

TEST(DistanceTest, CosineOrthogonalVectors) {
  float a[] = {1, 0, 0};
  float b[] = {0, 1, 0};
  EXPECT_NEAR(CosineDistance(a, b, 3), 1.0f, 1e-6f);
}

TEST(DistanceTest, CosineOppositeVectors) {
  float a[] = {1, 0};
  float b[] = {-1, 0};
  EXPECT_NEAR(CosineDistance(a, b, 2), 2.0f, 1e-6f);
}

TEST(DistanceTest, CosineZeroVectorReturnsOne) {
  float a[] = {0, 0, 0};
  float b[] = {1, 2, 3};
  EXPECT_FLOAT_EQ(CosineDistance(a, b, 3), 1.0f);
}

TEST(DistanceTest, IPKnownValues) {
  float a[] = {1, 2, 3};
  float b[] = {4, 5, 6};
  // dot = 1*4 + 2*5 + 3*6 = 32, IP distance = -32
  EXPECT_FLOAT_EQ(InnerProductDistance(a, b, 3), -32.0f);
}

TEST(DistanceTest, IPOrthogonal) {
  float a[] = {1, 0};
  float b[] = {0, 1};
  EXPECT_FLOAT_EQ(InnerProductDistance(a, b, 2), 0.0f);
}

// =============================================================
// FlatVectorIndex
// =============================================================

TEST(FlatVectorIndexTest, AddAndKnnL2) {
  FlatVectorIndex idx(3, DistanceMetric::kL2);
  float v1[] = {0, 0, 0};
  float v2[] = {1, 0, 0};
  float v3[] = {10, 0, 0};
  idx.Add("close", v1);
  idx.Add("mid", v2);
  idx.Add("far", v3);

  float query[] = {0, 0, 0};
  auto results = idx.KnnQuery(query, 2);
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].doc_id, "close");
  EXPECT_EQ(results[1].doc_id, "mid");
  EXPECT_FLOAT_EQ(results[0].score, 0.0f);
  EXPECT_FLOAT_EQ(results[1].score, 1.0f);
}

TEST(FlatVectorIndexTest, KnnCosine) {
  FlatVectorIndex idx(2, DistanceMetric::kCosine);
  float v1[] = {1, 0};
  float v2[] = {0, 1};
  float v3[] = {0.7f, 0.7f};
  idx.Add("x_axis", v1);
  idx.Add("y_axis", v2);
  idx.Add("diagonal", v3);

  float query[] = {1, 0};
  auto results = idx.KnnQuery(query, 3);
  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(results[0].doc_id, "x_axis");
  EXPECT_NEAR(results[0].score, 0.0f, 1e-5f);
}

TEST(FlatVectorIndexTest, KnnIP) {
  FlatVectorIndex idx(2, DistanceMetric::kIP);
  float v1[] = {1, 0};
  float v2[] = {0, 1};
  float v3[] = {1, 1};
  idx.Add("a", v1);
  idx.Add("b", v2);
  idx.Add("c", v3);

  float query[] = {1, 1};
  auto results = idx.KnnQuery(query, 3);
  ASSERT_EQ(results.size(), 3u);
  // c: dot=2, score=-2 (lowest = best)
  EXPECT_EQ(results[0].doc_id, "c");
  EXPECT_FLOAT_EQ(results[0].score, -2.0f);
}

TEST(FlatVectorIndexTest, KnnKGreaterThanSize) {
  FlatVectorIndex idx(2, DistanceMetric::kL2);
  float v[] = {1, 1};
  idx.Add("only", v);

  float query[] = {0, 0};
  auto results = idx.KnnQuery(query, 100);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].doc_id, "only");
}

TEST(FlatVectorIndexTest, EmptyIndexReturnsEmpty) {
  FlatVectorIndex idx(3, DistanceMetric::kL2);
  float query[] = {1, 2, 3};
  auto results = idx.KnnQuery(query, 5);
  EXPECT_TRUE(results.empty());
}

TEST(FlatVectorIndexTest, RemoveDoc) {
  FlatVectorIndex idx(2, DistanceMetric::kL2);
  float v1[] = {0, 0};
  float v2[] = {1, 1};
  idx.Add("a", v1);
  idx.Add("b", v2);
  EXPECT_TRUE(idx.Remove("a"));
  EXPECT_FALSE(idx.Remove("a"));

  float query[] = {0, 0};
  auto results = idx.KnnQuery(query, 10);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].doc_id, "b");
}

TEST(FlatVectorIndexTest, ReplaceDoc) {
  FlatVectorIndex idx(2, DistanceMetric::kL2);
  float v1[] = {0, 0};
  float v2[] = {10, 10};
  idx.Add("doc", v1);
  idx.Add("doc", v2);
  EXPECT_EQ(idx.Size(), 1u);

  float query[] = {0, 0};
  auto results = idx.KnnQuery(query, 1);
  EXPECT_FLOAT_EQ(results[0].score, 200.0f);
}

TEST(FlatVectorIndexTest, SizeTracking) {
  FlatVectorIndex idx(2, DistanceMetric::kL2);
  EXPECT_EQ(idx.Size(), 0u);
  float v[] = {1, 2};
  idx.Add("a", v);
  EXPECT_EQ(idx.Size(), 1u);
  idx.Add("b", v);
  EXPECT_EQ(idx.Size(), 2u);
  idx.Remove("a");
  EXPECT_EQ(idx.Size(), 1u);
}

TEST(FlatVectorIndexTest, DimAndMetric) {
  FlatVectorIndex idx(128, DistanceMetric::kCosine);
  EXPECT_EQ(idx.Dim(), 128u);
  EXPECT_EQ(idx.Metric(), DistanceMetric::kCosine);
}

TEST(FlatVectorIndexTest, Clear) {
  FlatVectorIndex idx(2, DistanceMetric::kL2);
  float v[] = {1, 2};
  idx.Add("a", v);
  idx.Add("b", v);
  idx.Clear();
  EXPECT_EQ(idx.Size(), 0u);
}

// =============================================================
// VectorFieldIndices
// =============================================================

TEST(VectorFieldIndicesTest, GetOrCreateAndGet) {
  VectorFieldIndices indices;
  VectorFieldParams params;
  params.dim = 3;
  params.metric = DistanceMetric::kL2;
  auto& idx = indices.GetOrCreate("emb", params);
  float v[] = {1, 2, 3};
  idx.Add("doc1", v);

  const auto* ptr = indices.Get("emb");
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(ptr->Size(), 1u);
}

TEST(VectorFieldIndicesTest, GetNonexistent) {
  VectorFieldIndices indices;
  EXPECT_EQ(indices.Get("nope"), nullptr);
}

TEST(VectorFieldIndicesTest, Clear) {
  VectorFieldIndices indices;
  VectorFieldParams p1;
  p1.dim = 2;
  p1.metric = DistanceMetric::kL2;
  VectorFieldParams p2;
  p2.dim = 4;
  p2.metric = DistanceMetric::kCosine;
  indices.GetOrCreate("a", p1);
  indices.GetOrCreate("b", p2);
  indices.Clear();
  EXPECT_EQ(indices.Get("a"), nullptr);
  EXPECT_EQ(indices.Get("b"), nullptr);
}

TEST(VectorFieldIndicesTest, GetOrCreateHnsw) {
  VectorFieldIndices indices;
  VectorFieldParams params;
  params.algorithm = VectorAlgorithm::kHnsw;
  params.dim = 3;
  params.metric = DistanceMetric::kL2;
  params.m = 8;
  params.ef_construction = 100;
  auto& idx = indices.GetOrCreate("emb", params);
  float v[] = {1, 2, 3};
  idx.Add("doc1", v);
  EXPECT_EQ(idx.Size(), 1u);
}

TEST(VectorPrecisionTest, Float16RoundTrip) {
  float vals[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 3.140625f, -42.5f, 0.0001f};
  for (float v : vals) {
    uint16_t h = Float32ToFloat16(v);
    float back = Float16ToFloat32(h);
    EXPECT_NEAR(back, v, std::max(1e-3f, std::fabs(v) * 2e-3f));
  }
}

TEST(VectorPrecisionTest, BFloat16RoundTrip) {
  float vals[] = {0.0f, 1.0f, -1.0f, 3.14159f, -123.5f, 1e-6f, 1e10f};
  for (float v : vals) {
    uint16_t bf = Float32ToBFloat16(v);
    float back = BFloat16ToFloat32(bf);
    EXPECT_NEAR(back, v, std::max(1e-7f, std::fabs(v) * 1e-2f));
  }
}

TEST(VectorPrecisionTest, DecodeVectorBlobFloat16AndBFloat16) {
  uint16_t fp16[3] = {Float32ToFloat16(1.0f), Float32ToFloat16(-2.5f),
                      Float32ToFloat16(0.25f)};
  std::string blob16(reinterpret_cast<const char*>(fp16), sizeof(fp16));
  std::vector<float> out;
  ASSERT_TRUE(DecodeVectorBlob(blob16, 3, VectorDataType::kFloat16, out));
  ASSERT_EQ(out.size(), 3u);
  EXPECT_NEAR(out[0], 1.0f, 1e-3f);
  EXPECT_NEAR(out[1], -2.5f, 1e-3f);
  EXPECT_NEAR(out[2], 0.25f, 1e-3f);
  // Wrong size rejected
  EXPECT_FALSE(DecodeVectorBlob(blob16, 4, VectorDataType::kFloat16, out));

  uint16_t bf16[2] = {Float32ToBFloat16(2.0f), Float32ToBFloat16(-4.0f)};
  std::string blob_bf(reinterpret_cast<const char*>(bf16), sizeof(bf16));
  ASSERT_TRUE(DecodeVectorBlob(blob_bf, 2, VectorDataType::kBFloat16, out));
  EXPECT_NEAR(out[0], 2.0f, 1e-2f);
  EXPECT_NEAR(out[1], -4.0f, 1e-2f);
  EXPECT_FALSE(DecodeVectorBlob(blob_bf, 3, VectorDataType::kBFloat16, out));
}

TEST(VectorSq8Test, FlatSq8L2CosineIpAccuracyAndRemove) {
  for (auto metric : {DistanceMetric::kL2, DistanceMetric::kCosine, DistanceMetric::kIP}) {
    FlatVectorIndex idx(4, metric, VectorQuantization::kSq8);
    float v1[] = {1.0f, 0.0f, 0.0f, 0.0f};
    float v2[] = {0.0f, 1.0f, 0.0f, 0.0f};
    float v3[] = {0.9f, 0.1f, 0.0f, 0.0f};
    idx.Add("d1", v1);
    idx.Add("d2", v2);
    idx.Add("d3", v3);

    auto res = idx.KnnQuery(v1, 2);
    ASSERT_EQ(res.size(), 2u);
    EXPECT_EQ(res[0].doc_id, "d1");
    EXPECT_EQ(res[1].doc_id, "d3");

    EXPECT_TRUE(idx.Remove("d1"));
    EXPECT_EQ(idx.Size(), 2u);
    auto res2 = idx.KnnQuery(v1, 1);
    ASSERT_EQ(res2.size(), 1u);
    EXPECT_EQ(res2[0].doc_id, "d3");
  }
}

