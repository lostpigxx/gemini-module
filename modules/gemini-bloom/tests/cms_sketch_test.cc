#include <gtest/gtest.h>
#include "cms_sketch.h"

#include <cmath>
#include <string>
#include <vector>

static std::span<const std::byte> ToSpan(const std::string& s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

// --- Create ---

TEST(CmsSketchTest, CreateRejectsInvalidParameters) {
  EXPECT_FALSE(CmsSketch::Create(0, 4).has_value());
  EXPECT_FALSE(CmsSketch::Create(100, 0).has_value());
  EXPECT_FALSE(CmsSketch::Create(kCmsMaxWidth + 1, 4).has_value());
  EXPECT_FALSE(CmsSketch::Create(100, kCmsMaxDepth + 1).has_value());
}

TEST(CmsSketchTest, CreateSucceedsWithinBounds) {
  auto sketch = CmsSketch::Create(1000, 5);
  ASSERT_TRUE(sketch.has_value());
  EXPECT_EQ(sketch->Width(), 1000u);
  EXPECT_EQ(sketch->Depth(), 5u);
  EXPECT_EQ(sketch->TotalCount(), 0u);
  EXPECT_NE(sketch->GetCounterArray(), nullptr);
  EXPECT_EQ(sketch->GetDataSize(), 1000u * 5u * sizeof(uint32_t));
}

TEST(CmsSketchTest, MoveSemantics) {
  auto sketch = CmsSketch::Create(100, 4);
  ASSERT_TRUE(sketch.has_value());

  CmsSketch moved = std::move(*sketch);
  EXPECT_NE(moved.GetCounterArray(), nullptr);
  EXPECT_EQ(sketch->GetCounterArray(), nullptr);
}

// --- CreateByProb ---

TEST(CmsSketchTest, CreateByProbRejectsInvalidValues) {
  EXPECT_FALSE(CmsSketch::CreateByProb(0.0, 0.01).has_value());
  EXPECT_FALSE(CmsSketch::CreateByProb(0.01, 0.0).has_value());
  EXPECT_FALSE(CmsSketch::CreateByProb(-0.1, 0.01).has_value());
  EXPECT_FALSE(CmsSketch::CreateByProb(0.01, -0.1).has_value());
  EXPECT_FALSE(CmsSketch::CreateByProb(1.5, 0.01).has_value());
  EXPECT_FALSE(CmsSketch::CreateByProb(0.01, 1.5).has_value());
}

TEST(CmsSketchTest, CreateByProbProducesReasonableDimensions) {
  auto sketch = CmsSketch::CreateByProb(0.01, 0.01);
  ASSERT_TRUE(sketch.has_value());
  EXPECT_GT(sketch->Width(), 0u);
  EXPECT_GT(sketch->Depth(), 0u);
  // Smaller error -> larger width.
  auto tighter = CmsSketch::CreateByProb(0.001, 0.01);
  ASSERT_TRUE(tighter.has_value());
  EXPECT_GT(tighter->Width(), sketch->Width());
  // Smaller probability -> larger depth.
  auto moreConfident = CmsSketch::CreateByProb(0.01, 0.001);
  ASSERT_TRUE(moreConfident.has_value());
  EXPECT_GT(moreConfident->Depth(), sketch->Depth());
}

// --- IncrBy / Query ---

TEST(CmsSketchTest, IncrByThenQueryReturnsAtLeastTrueCount) {
  auto sketch = CmsSketch::Create(2000, 5);
  ASSERT_TRUE(sketch.has_value());

  auto item = std::string("hello");
  auto r = sketch->IncrBy(ToSpan(item), 5);
  ASSERT_TRUE(r.has_value());
  EXPECT_GE(*r, 5u);
  EXPECT_GE(sketch->Query(ToSpan(item)), 5u);
}

TEST(CmsSketchTest, QueryOnAbsentItemIsZeroOrOverEstimate) {
  auto sketch = CmsSketch::Create(2000, 5);
  ASSERT_TRUE(sketch.has_value());
  EXPECT_EQ(sketch->Query(ToSpan(std::string("absent"))), 0u);
}

TEST(CmsSketchTest, NoUnderCounting) {
  auto sketch = CmsSketch::Create(50, 4);  // small width to force some collisions
  ASSERT_TRUE(sketch.has_value());

  std::vector<std::string> items;
  for (int i = 0; i < 200; i++) {
    auto item = "item_" + std::to_string(i);
    sketch->IncrBy(ToSpan(item), i + 1);
    items.push_back(item);
  }

  for (int i = 0; i < 200; i++) {
    EXPECT_GE(sketch->Query(ToSpan(items[static_cast<size_t>(i)])), static_cast<uint64_t>(i + 1))
      << "Count-Min Sketch must never under-count for " << items[static_cast<size_t>(i)];
  }
}

TEST(CmsSketchTest, IncrByAccumulatesTotalCount) {
  auto sketch = CmsSketch::Create(500, 4);
  ASSERT_TRUE(sketch.has_value());

  sketch->IncrBy(ToSpan(std::string("a")), 3);
  sketch->IncrBy(ToSpan(std::string("b")), 4);
  EXPECT_EQ(sketch->TotalCount(), 7u);
}

TEST(CmsSketchTest, IncrByNegativeDeltaDecreasesCount) {
  auto sketch = CmsSketch::Create(500, 4);
  ASSERT_TRUE(sketch.has_value());

  auto item = std::string("x");
  sketch->IncrBy(ToSpan(item), 10);
  auto r = sketch->IncrBy(ToSpan(item), -4);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 6u);
  EXPECT_EQ(sketch->TotalCount(), 6u);
}

TEST(CmsSketchTest, IncrByRejectsOverflowWithoutMutating) {
  auto sketch = CmsSketch::Create(10, 2);
  ASSERT_TRUE(sketch.has_value());

  auto item = std::string("overflow");
  auto r1 = sketch->IncrBy(ToSpan(item), static_cast<int64_t>(UINT32_MAX));
  ASSERT_TRUE(r1.has_value());

  auto before = sketch->Query(ToSpan(item));
  auto r2 = sketch->IncrBy(ToSpan(item), static_cast<int64_t>(UINT32_MAX));
  EXPECT_FALSE(r2.has_value());
  // Rejected increment must leave state untouched.
  EXPECT_EQ(sketch->Query(ToSpan(item)), before);
}

// --- Merge ---

TEST(CmsSketchTest, MergeRejectsMismatchedDimensions) {
  auto dest = CmsSketch::Create(100, 4);
  auto src = CmsSketch::Create(200, 4);
  ASSERT_TRUE(dest.has_value());
  ASSERT_TRUE(src.has_value());

  const CmsSketch* sources[] = {&*src};
  double weights[] = {1.0};
  EXPECT_FALSE(dest->Merge(sources, weights));
}

TEST(CmsSketchTest, MergeSumsWeightedCounts) {
  auto dest = CmsSketch::Create(500, 4);
  auto srcA = CmsSketch::Create(500, 4);
  auto srcB = CmsSketch::Create(500, 4);
  ASSERT_TRUE(dest.has_value());
  ASSERT_TRUE(srcA.has_value());
  ASSERT_TRUE(srcB.has_value());

  auto item = std::string("merged");
  srcA->IncrBy(ToSpan(item), 10);
  srcB->IncrBy(ToSpan(item), 20);

  const CmsSketch* sources[] = {&*srcA, &*srcB};
  double weights[] = {1.0, 2.0};
  ASSERT_TRUE(dest->Merge(sources, weights));

  EXPECT_GE(dest->Query(ToSpan(item)), 50u);  // 10*1 + 20*2
  EXPECT_EQ(dest->TotalCount(), 50u);
}

// --- Clone ---

TEST(CmsSketchTest, CloneDeepCopiesCounters) {
  auto sketch = CmsSketch::Create(500, 4);
  ASSERT_TRUE(sketch.has_value());

  auto item = std::string("clone_me");
  sketch->IncrBy(ToSpan(item), 42);

  auto clone = sketch->Clone();
  ASSERT_TRUE(clone.has_value());
  EXPECT_NE(clone->GetCounterArray(), sketch->GetCounterArray());
  EXPECT_EQ(clone->Query(ToSpan(item)), sketch->Query(ToSpan(item)));

  sketch->IncrBy(ToSpan(std::string("after_clone")), 5);
  EXPECT_EQ(clone->Query(ToSpan(std::string("after_clone"))), 0u);
}
