#include <gtest/gtest.h>
#include "tdigest_sketch.h"

#include <cmath>
#include <vector>

// --- Create ---

TEST(TdigestSketchTest, CreateRejectsInvalidParameters) {
  EXPECT_FALSE(TdigestSketch::Create(0.0).has_value());
  EXPECT_FALSE(TdigestSketch::Create(kTdigestMinCompression - 1.0).has_value());
  EXPECT_FALSE(TdigestSketch::Create(kTdigestMaxCompression + 1.0).has_value());
  EXPECT_FALSE(TdigestSketch::Create(std::nan("")).has_value());
}

TEST(TdigestSketchTest, CreateSucceedsWithinBounds) {
  auto sketch = TdigestSketch::Create(100.0);
  ASSERT_TRUE(sketch.has_value());
  EXPECT_DOUBLE_EQ(sketch->Compression(), 100.0);
  EXPECT_EQ(sketch->NumCentroids(), 0u);
  EXPECT_EQ(sketch->NumBuffered(), 0u);
  EXPECT_TRUE(sketch->Empty());
}

TEST(TdigestSketchTest, MoveSemantics) {
  auto sketch = TdigestSketch::Create(100.0);
  ASSERT_TRUE(sketch.has_value());
  sketch->Add(1.0);

  TdigestSketch moved = std::move(*sketch);
  EXPECT_FALSE(moved.Empty());
  EXPECT_TRUE(sketch->Empty());
  EXPECT_EQ(sketch->GetBufferArray(), nullptr);
}

// --- Empty-sketch sentinel values ---

TEST(TdigestSketchTest, EmptySketchSentinels) {
  auto sketch = TdigestSketch::Create(100.0);
  ASSERT_TRUE(sketch.has_value());

  EXPECT_TRUE(std::isnan(sketch->Quantile(0.5)));
  EXPECT_TRUE(std::isnan(sketch->Cdf(1.0)));
  EXPECT_TRUE(std::isnan(sketch->Min()));
  EXPECT_TRUE(std::isnan(sketch->Max()));
  EXPECT_TRUE(std::isnan(sketch->TrimmedMean(0.1, 0.9)));
  EXPECT_TRUE(std::isnan(sketch->ByRank(0)));
  EXPECT_TRUE(std::isnan(sketch->ByRevRank(0)));
  EXPECT_EQ(sketch->Rank(1.0), -2);
  EXPECT_EQ(sketch->RevRank(1.0), -2);
}

// --- Add / basic quantiles ---

TEST(TdigestSketchTest, AddRejectsInvalidValues) {
  auto sketch = TdigestSketch::Create(100.0);
  ASSERT_TRUE(sketch.has_value());
  EXPECT_FALSE(sketch->Add(std::nan(""), 1.0));
  EXPECT_FALSE(sketch->Add(1.0, 0.0));
  EXPECT_FALSE(sketch->Add(1.0, -1.0));
  EXPECT_FALSE(sketch->Add(1.0, std::nan("")));
}

TEST(TdigestSketchTest, MinMaxTrackedExactly) {
  auto sketch = TdigestSketch::Create(100.0);
  ASSERT_TRUE(sketch.has_value());
  for (int i = 1; i <= 1000; i++) sketch->Add(static_cast<double>(i));

  EXPECT_DOUBLE_EQ(sketch->Min(), 1.0);
  EXPECT_DOUBLE_EQ(sketch->Max(), 1000.0);
  EXPECT_DOUBLE_EQ(sketch->Quantile(0.0), 1.0);
  EXPECT_DOUBLE_EQ(sketch->Quantile(1.0), 1000.0);
}

TEST(TdigestSketchTest, QuantileApproximatesUniformDistribution) {
  auto sketch = TdigestSketch::Create(200.0);
  ASSERT_TRUE(sketch.has_value());
  for (int i = 1; i <= 10000; i++) sketch->Add(static_cast<double>(i));

  double median = sketch->Quantile(0.5);
  EXPECT_NEAR(median, 5000.0, 200.0);

  double p90 = sketch->Quantile(0.9);
  EXPECT_NEAR(p90, 9000.0, 300.0);
}

TEST(TdigestSketchTest, CdfIsMonotonic) {
  auto sketch = TdigestSketch::Create(100.0);
  ASSERT_TRUE(sketch.has_value());
  for (int i = 1; i <= 500; i++) sketch->Add(static_cast<double>(i));

  EXPECT_DOUBLE_EQ(sketch->Cdf(0.0), 0.0);
  EXPECT_DOUBLE_EQ(sketch->Cdf(501.0), 1.0);
  double lastCdf = -1.0;
  for (int v = 0; v <= 500; v += 50) {
    double c = sketch->Cdf(static_cast<double>(v));
    EXPECT_GE(c, lastCdf);
    lastCdf = c;
  }
}

// --- Rank / RevRank sentinels and behavior ---

TEST(TdigestSketchTest, RankSentinelsAtBoundaries) {
  auto sketch = TdigestSketch::Create(100.0);
  ASSERT_TRUE(sketch.has_value());
  for (int i = 1; i <= 100; i++) sketch->Add(static_cast<double>(i));

  EXPECT_EQ(sketch->Rank(0.0), -1);
  EXPECT_EQ(sketch->Rank(1000.0), 100);
  EXPECT_EQ(sketch->RevRank(1000.0), -1);
  EXPECT_EQ(sketch->RevRank(0.0), 100);
}

TEST(TdigestSketchTest, ByRankExactAtBoundaries) {
  auto sketch = TdigestSketch::Create(100.0);
  ASSERT_TRUE(sketch.has_value());
  for (int i = 1; i <= 100; i++) sketch->Add(static_cast<double>(i));

  EXPECT_DOUBLE_EQ(sketch->ByRank(0), sketch->Min());
  EXPECT_DOUBLE_EQ(sketch->ByRank(99), sketch->Max());
  EXPECT_TRUE(std::isinf(sketch->ByRank(100)));
  EXPECT_GT(sketch->ByRank(100), 0.0);
}

TEST(TdigestSketchTest, ByRevRankExactAtBoundaries) {
  auto sketch = TdigestSketch::Create(100.0);
  ASSERT_TRUE(sketch.has_value());
  for (int i = 1; i <= 100; i++) sketch->Add(static_cast<double>(i));

  EXPECT_DOUBLE_EQ(sketch->ByRevRank(0), sketch->Max());
  EXPECT_DOUBLE_EQ(sketch->ByRevRank(99), sketch->Min());
  EXPECT_TRUE(std::isinf(sketch->ByRevRank(100)));
  EXPECT_LT(sketch->ByRevRank(100), 0.0);
}

// --- TrimmedMean ---

TEST(TdigestSketchTest, TrimmedMeanRejectsInvalidQuantiles) {
  auto sketch = TdigestSketch::Create(100.0);
  ASSERT_TRUE(sketch.has_value());
  for (int i = 1; i <= 100; i++) sketch->Add(static_cast<double>(i));

  EXPECT_TRUE(std::isnan(sketch->TrimmedMean(-0.1, 0.9)));
  EXPECT_TRUE(std::isnan(sketch->TrimmedMean(0.1, 1.1)));
  EXPECT_TRUE(std::isnan(sketch->TrimmedMean(0.5, 0.5)));
  EXPECT_TRUE(std::isnan(sketch->TrimmedMean(0.6, 0.4)));
}

TEST(TdigestSketchTest, TrimmedMeanApproximatesUntrimmedMeanOnFullRange) {
  auto sketch = TdigestSketch::Create(200.0);
  ASSERT_TRUE(sketch.has_value());
  for (int i = 1; i <= 1000; i++) sketch->Add(static_cast<double>(i));

  double mean = sketch->TrimmedMean(0.0, 1.0);
  EXPECT_NEAR(mean, 500.5, 20.0);
}

// --- Reset ---

TEST(TdigestSketchTest, ResetClearsState) {
  auto sketch = TdigestSketch::Create(100.0);
  ASSERT_TRUE(sketch.has_value());
  for (int i = 1; i <= 100; i++) sketch->Add(static_cast<double>(i));
  ASSERT_FALSE(sketch->Empty());

  sketch->Reset();
  EXPECT_TRUE(sketch->Empty());
  EXPECT_EQ(sketch->NumCentroids(), 0u);
  EXPECT_EQ(sketch->NumBuffered(), 0u);
  EXPECT_TRUE(std::isnan(sketch->Min()));
}

// --- SetCompression ---

TEST(TdigestSketchTest, SetCompressionValidatesRange) {
  auto sketch = TdigestSketch::Create(100.0);
  ASSERT_TRUE(sketch.has_value());
  EXPECT_TRUE(sketch->SetCompression(50.0));
  EXPECT_DOUBLE_EQ(sketch->Compression(), 50.0);
  EXPECT_FALSE(sketch->SetCompression(0.0));
  EXPECT_DOUBLE_EQ(sketch->Compression(), 50.0);
}

// --- Merge ---

TEST(TdigestSketchTest, MergeCombinesSourcesIntoDestination) {
  auto a = TdigestSketch::Create(100.0);
  auto b = TdigestSketch::Create(100.0);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());

  for (int i = 1; i <= 500; i++) a->Add(static_cast<double>(i));
  for (int i = 501; i <= 1000; i++) b->Add(static_cast<double>(i));

  auto dest = TdigestSketch::Create(100.0);
  ASSERT_TRUE(dest.has_value());

  const TdigestSketch* sources[] = {&*a, &*b};
  ASSERT_TRUE(dest->Merge(sources));

  EXPECT_DOUBLE_EQ(dest->Min(), 1.0);
  EXPECT_DOUBLE_EQ(dest->Max(), 1000.0);
  EXPECT_NEAR(dest->TotalWeight(), 1000.0, 0.001);
  EXPECT_NEAR(dest->Quantile(0.5), 500.0, 200.0);
}

TEST(TdigestSketchTest, MergeWithEmptySourcesLeavesDestinationEmptyIfAllEmpty) {
  auto a = TdigestSketch::Create(100.0);
  auto b = TdigestSketch::Create(100.0);
  auto dest = TdigestSketch::Create(100.0);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  ASSERT_TRUE(dest.has_value());

  const TdigestSketch* sources[] = {&*a, &*b};
  ASSERT_TRUE(dest->Merge(sources));
  EXPECT_TRUE(dest->Empty());
}

// --- Clone ---

TEST(TdigestSketchTest, CloneDeepCopiesState) {
  auto sketch = TdigestSketch::Create(100.0);
  ASSERT_TRUE(sketch.has_value());
  for (int i = 1; i <= 200; i++) sketch->Add(static_cast<double>(i));
  sketch->Compress();

  auto clone = sketch->Clone();
  ASSERT_TRUE(clone.has_value());
  EXPECT_NE(clone->GetCentroidArray(), sketch->GetCentroidArray());
  EXPECT_EQ(clone->NumCentroids(), sketch->NumCentroids());
  EXPECT_DOUBLE_EQ(clone->Min(), sketch->Min());
  EXPECT_DOUBLE_EQ(clone->Max(), sketch->Max());

  sketch->Add(10000.0);
  sketch->Compress();
  EXPECT_NE(clone->Max(), sketch->Max());
}

// --- AllocateForLoad / AppendCentroidForLoad / AppendBufferedForLoad ---

TEST(TdigestSketchTest, LoadHelpersReconstructState) {
  auto sketch = TdigestSketch::Create(100.0);
  ASSERT_TRUE(sketch.has_value());

  ASSERT_TRUE(sketch->AllocateForLoad(2, 1));
  sketch->AppendCentroidForLoad(10.0, 5.0);
  sketch->AppendCentroidForLoad(20.0, 5.0);
  sketch->AppendBufferedForLoad(30.0, 1.0);
  sketch->SetBookkeepingForLoad(10.0, 1.0, 10.0, 30.0, 3);

  EXPECT_EQ(sketch->NumCentroids(), 2u);
  EXPECT_EQ(sketch->NumBuffered(), 1u);
  EXPECT_DOUBLE_EQ(sketch->MergedWeight(), 10.0);
  EXPECT_DOUBLE_EQ(sketch->UnmergedWeight(), 1.0);
  EXPECT_DOUBLE_EQ(sketch->Min(), 10.0);
  EXPECT_DOUBLE_EQ(sketch->Max(), 30.0);
  EXPECT_EQ(sketch->NumCompressions(), 3u);
}
