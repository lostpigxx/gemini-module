#include "hybrid_fusion.h"

#include <gtest/gtest.h>
#include <cmath>
#include <limits>

TEST(HybridFusionTest, EmptyInputs) {
  HybridConfig cfg;
  auto rrf = FuseHybridResults({}, {}, cfg);
  EXPECT_TRUE(rrf.empty());

  cfg.method = FusionMethod::kLinear;
  auto lin = FuseHybridResults({}, {}, cfg);
  EXPECT_TRUE(lin.empty());
}

TEST(HybridFusionTest, RrfOverlapBoostsSharedDoc) {
  std::vector<ScoredResult> text = {{"d1", 5.0}, {"d2", 3.0}, {"d3", 1.0}};
  std::vector<KnnResult> vec = {{"d4", 0.05f}, {"d2", 0.10f}, {"d5", 0.20f}};

  HybridConfig cfg;
  cfg.method = FusionMethod::kRrf;
  cfg.rrf_k = 60.0;

  auto res = FuseHybridResults(text, vec, cfg);
  ASSERT_EQ(res.size(), 5u);
  // d2 is rank 2 in both branches -> 2/62 = 0.03226, beating d1 (1/61 = 0.01639) and d4 (1/61)
  EXPECT_EQ(res[0].doc_id, "d2");
  EXPECT_EQ(res[0].text_rank, 2);
  EXPECT_EQ(res[0].vector_rank, 2);
  EXPECT_NEAR(res[0].score, 2.0 / 62.0, 1e-9);
}

TEST(HybridFusionTest, RrfCustomConstantAndWeights) {
  std::vector<ScoredResult> text = {{"d1", 10.0}, {"d2", 5.0}};
  std::vector<KnnResult> vec = {{"d2", 0.01f}, {"d3", 0.02f}};

  HybridConfig cfg;
  cfg.method = FusionMethod::kRrf;
  cfg.rrf_k = 10.0;
  cfg.text_weight = 3.0;
  cfg.vector_weight = 1.0;

  auto res = FuseHybridResults(text, vec, cfg);
  ASSERT_EQ(res.size(), 3u);
  // d2: 3/12 + 1/11 = 0.25 + 0.0909 = 0.3409
  // d1: 3/11 = 0.2727
  // d3: 1/12 = 0.0833
  EXPECT_EQ(res[0].doc_id, "d2");
  EXPECT_EQ(res[1].doc_id, "d1");
  EXPECT_EQ(res[2].doc_id, "d3");
  EXPECT_NEAR(res[0].score, 3.0 / 12.0 + 1.0 / 11.0, 1e-9);
}

TEST(HybridFusionTest, RrfTieBreakByDocId) {
  std::vector<ScoredResult> text = {{"b", 5.0}};
  std::vector<KnnResult> vec = {{"a", 0.1f}};

  HybridConfig cfg;
  auto res = FuseHybridResults(text, vec, cfg);
  ASSERT_EQ(res.size(), 2u);
  EXPECT_EQ(res[0].doc_id, "a");
  EXPECT_EQ(res[1].doc_id, "b");
  EXPECT_DOUBLE_EQ(res[0].score, res[1].score);
}

TEST(HybridFusionTest, RrfTextOnlyBranch) {
  std::vector<ScoredResult> text = {{"d2", 1.0}, {"d1", 4.0}};
  HybridConfig cfg;
  auto res = FuseHybridResults(text, {}, cfg);
  ASSERT_EQ(res.size(), 2u);
  EXPECT_EQ(res[0].doc_id, "d1");
  EXPECT_EQ(res[0].text_rank, 1);
  EXPECT_EQ(res[0].vector_rank, 0);
  EXPECT_EQ(res[1].doc_id, "d2");
}

TEST(HybridFusionTest, RrfVectorOnlyBranch) {
  std::vector<KnnResult> vec = {{"d2", 0.9f}, {"d1", 0.1f}};
  HybridConfig cfg;
  auto res = FuseHybridResults({}, vec, cfg);
  ASSERT_EQ(res.size(), 2u);
  EXPECT_EQ(res[0].doc_id, "d1");
  EXPECT_EQ(res[0].vector_rank, 1);
  EXPECT_EQ(res[0].text_rank, 0);
  EXPECT_EQ(res[1].doc_id, "d2");
}

TEST(HybridFusionTest, LinearEqualWeight) {
  // text: d1=10 (norm 1.0), d2=0 (norm 0.0)
  // vec:  d2=0.0 (norm 1.0), d1=1.0 (norm 0.0)
  std::vector<ScoredResult> text = {{"d1", 10.0}, {"d2", 0.0}};
  std::vector<KnnResult> vec = {{"d2", 0.0f}, {"d1", 1.0f}};

  HybridConfig cfg;
  cfg.method = FusionMethod::kLinear;
  cfg.alpha = 0.5;
  auto res = FuseHybridResults(text, vec, cfg);
  ASSERT_EQ(res.size(), 2u);
  EXPECT_NEAR(res[0].score, 0.5, 1e-9);
  EXPECT_NEAR(res[1].score, 0.5, 1e-9);
  EXPECT_EQ(res[0].doc_id, "d1");  // tie-break
}

TEST(HybridFusionTest, LinearAlphaFavorsTextOrVector) {
  std::vector<ScoredResult> text = {{"t_best", 10.0}, {"v_best", 1.0}};
  std::vector<KnnResult> vec = {{"v_best", 0.1f}, {"t_best", 0.9f}};

  HybridConfig cfg;
  cfg.method = FusionMethod::kLinear;
  cfg.alpha = 0.8;
  auto res_text = FuseHybridResults(text, vec, cfg);
  ASSERT_EQ(res_text.size(), 2u);
  EXPECT_EQ(res_text[0].doc_id, "t_best");
  EXPECT_NEAR(res_text[0].score, 0.8, 1e-9);

  cfg.alpha = 0.2;
  auto res_vec = FuseHybridResults(text, vec, cfg);
  ASSERT_EQ(res_vec.size(), 2u);
  EXPECT_EQ(res_vec[0].doc_id, "v_best");
  EXPECT_NEAR(res_vec[0].score, 0.8, 1e-9);
}

TEST(HybridFusionTest, LinearAlphaExtremeZeroAndOne) {
  std::vector<ScoredResult> text = {{"d1", 5.0}, {"d2", 1.0}};
  std::vector<KnnResult> vec = {{"d2", 0.0f}, {"d1", 2.0f}};

  HybridConfig cfg;
  cfg.method = FusionMethod::kLinear;
  cfg.alpha = 1.0;
  auto r1 = FuseHybridResults(text, vec, cfg);
  EXPECT_EQ(r1[0].doc_id, "d1");
  EXPECT_NEAR(r1[0].score, 1.0, 1e-9);
  EXPECT_NEAR(r1[1].score, 0.0, 1e-9);

  cfg.alpha = 0.0;
  auto r0 = FuseHybridResults(text, vec, cfg);
  EXPECT_EQ(r0[0].doc_id, "d2");
  EXPECT_NEAR(r0[0].score, 1.0, 1e-9);
  EXPECT_NEAR(r0[1].score, 0.0, 1e-9);
}

TEST(HybridFusionTest, LinearSingleHitPerBranchNormalizesToOne) {
  std::vector<ScoredResult> text = {{"d1", 3.5}};
  std::vector<KnnResult> vec = {{"d1", 0.25f}};

  HybridConfig cfg;
  cfg.method = FusionMethod::kLinear;
  cfg.alpha = 0.6;
  auto res = FuseHybridResults(text, vec, cfg);
  ASSERT_EQ(res.size(), 1u);
  EXPECT_NEAR(res[0].score, 1.0, 1e-9);
}

TEST(HybridFusionTest, LinearIdenticalScoresNormalizeToOne) {
  std::vector<ScoredResult> text = {{"d1", 2.0}, {"d2", 2.0}};
  std::vector<KnnResult> vec = {{"d2", 0.5f}, {"d3", 0.5f}};

  HybridConfig cfg;
  cfg.method = FusionMethod::kLinear;
  cfg.alpha = 0.5;
  auto res = FuseHybridResults(text, vec, cfg);
  ASSERT_EQ(res.size(), 3u);
  EXPECT_EQ(res[0].doc_id, "d2");
  EXPECT_NEAR(res[0].score, 1.0, 1e-9);
  EXPECT_NEAR(res[1].score, 0.5, 1e-9);
  EXPECT_NEAR(res[2].score, 0.5, 1e-9);
}

TEST(HybridFusionTest, InvalidConfigValuesFallBackSafely) {
  std::vector<ScoredResult> text = {{"d1", 1.0}};
  std::vector<KnnResult> vec = {{"d1", 0.1f}};

  HybridConfig cfg;
  cfg.method = FusionMethod::kRrf;
  cfg.rrf_k = -5.0;
  cfg.text_weight = std::numeric_limits<double>::quiet_NaN();
  cfg.vector_weight = -1.0;
  auto r1 = FuseHybridResults(text, vec, cfg);
  ASSERT_EQ(r1.size(), 1u);
  EXPECT_TRUE(std::isfinite(r1[0].score));
  EXPECT_NEAR(r1[0].score, 2.0 / 61.0, 1e-9);

  cfg.method = FusionMethod::kLinear;
  cfg.alpha = 5.0;  // clamped to 1.0
  auto r2 = FuseHybridResults(text, vec, cfg);
  ASSERT_EQ(r2.size(), 1u);
  EXPECT_NEAR(r2[0].score, 1.0, 1e-9);
}
