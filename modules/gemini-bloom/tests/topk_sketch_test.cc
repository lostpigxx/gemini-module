#include <gtest/gtest.h>
#include "topk_sketch.h"

#include <string>
#include <vector>

static std::span<const std::byte> ToSpan(const std::string& s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

// --- Create ---

TEST(TopkSketchTest, CreateRejectsInvalidParameters) {
  EXPECT_FALSE(TopkSketch::Create(0, 8, 7, 0.9).has_value());
  EXPECT_FALSE(TopkSketch::Create(kTopkMaxK + 1, 8, 7, 0.9).has_value());
  EXPECT_FALSE(TopkSketch::Create(10, 0, 7, 0.9).has_value());
  EXPECT_FALSE(TopkSketch::Create(10, kTopkMaxWidth + 1, 7, 0.9).has_value());
  EXPECT_FALSE(TopkSketch::Create(10, 8, 0, 0.9).has_value());
  EXPECT_FALSE(TopkSketch::Create(10, 8, kTopkMaxDepth + 1, 0.9).has_value());
  EXPECT_FALSE(TopkSketch::Create(10, 8, 7, 0.0).has_value());
  EXPECT_FALSE(TopkSketch::Create(10, 8, 7, 1.0).has_value());
  EXPECT_FALSE(TopkSketch::Create(10, 8, 7, -0.5).has_value());
  EXPECT_FALSE(TopkSketch::Create(10, 8, 7, 1.5).has_value());
}

TEST(TopkSketchTest, CreateSucceedsWithinBounds) {
  auto sketch = TopkSketch::Create(10, 8, 7, 0.9);
  ASSERT_TRUE(sketch.has_value());
  EXPECT_EQ(sketch->K(), 10u);
  EXPECT_EQ(sketch->Width(), 8u);
  EXPECT_EQ(sketch->Depth(), 7u);
  EXPECT_DOUBLE_EQ(sketch->Decay(), 0.9);
  EXPECT_EQ(sketch->NumActive(), 0u);
  EXPECT_NE(sketch->GetCellArray(), nullptr);
  EXPECT_EQ(sketch->GetCellDataSize(), 8u * 7u * sizeof(TopkSketch::Cell));
}

TEST(TopkSketchTest, MoveSemantics) {
  auto sketch = TopkSketch::Create(10, 8, 7, 0.9);
  ASSERT_TRUE(sketch.has_value());

  TopkSketch moved = std::move(*sketch);
  EXPECT_NE(moved.GetCellArray(), nullptr);
  EXPECT_EQ(sketch->GetCellArray(), nullptr);
}

// --- Add / Query / List ---

TEST(TopkSketchTest, AddFillsUpToKWithoutEviction) {
  auto sketch = TopkSketch::Create(3, 32, 4, 0.9);
  ASSERT_TRUE(sketch.has_value());

  EXPECT_FALSE(sketch->Add(ToSpan(std::string("a"))).has_value());
  EXPECT_FALSE(sketch->Add(ToSpan(std::string("b"))).has_value());
  EXPECT_FALSE(sketch->Add(ToSpan(std::string("c"))).has_value());
  EXPECT_EQ(sketch->NumActive(), 3u);

  EXPECT_TRUE(sketch->Query(ToSpan(std::string("a"))));
  EXPECT_TRUE(sketch->Query(ToSpan(std::string("b"))));
  EXPECT_TRUE(sketch->Query(ToSpan(std::string("c"))));
  EXPECT_FALSE(sketch->Query(ToSpan(std::string("d"))));
}

TEST(TopkSketchTest, HeavyItemEventuallyEvictsLightItem) {
  auto sketch = TopkSketch::Create(2, 64, 4, 0.9);
  ASSERT_TRUE(sketch.has_value());

  sketch->Add(ToSpan(std::string("light1")));
  sketch->Add(ToSpan(std::string("light2")));
  EXPECT_EQ(sketch->NumActive(), 2u);

  // Pump a heavy item hundreds of times; it must eventually displace one of
  // the light singleton entries.
  bool evictedSomething = false;
  for (int i = 0; i < 500; i++) {
    auto evicted = sketch->Add(ToSpan(std::string("heavy")));
    if (evicted.has_value()) {
      evictedSomething = true;
      break;
    }
  }
  EXPECT_TRUE(evictedSomething);
  EXPECT_TRUE(sketch->Query(ToSpan(std::string("heavy"))));
}

TEST(TopkSketchTest, RepeatedAddBumpsExistingEntryCount) {
  auto sketch = TopkSketch::Create(3, 64, 4, 0.9);
  ASSERT_TRUE(sketch.has_value());

  auto item = std::string("repeat");
  for (int i = 0; i < 10; i++) sketch->Add(ToSpan(item));

  auto list = sketch->List();
  ASSERT_EQ(list.size(), 1u);
  EXPECT_EQ(list[0].first, item);
  EXPECT_GE(list[0].second, 10u);
}

TEST(TopkSketchTest, ListSortedByDescendingCount) {
  auto sketch = TopkSketch::Create(3, 64, 4, 0.9);
  ASSERT_TRUE(sketch.has_value());

  for (int i = 0; i < 1; i++) sketch->Add(ToSpan(std::string("low")));
  for (int i = 0; i < 5; i++) sketch->Add(ToSpan(std::string("mid")));
  for (int i = 0; i < 20; i++) sketch->Add(ToSpan(std::string("high")));

  auto list = sketch->List();
  ASSERT_EQ(list.size(), 3u);
  EXPECT_EQ(list[0].first, "high");
  EXPECT_EQ(list[1].first, "mid");
  EXPECT_EQ(list[2].first, "low");
  EXPECT_GE(list[0].second, list[1].second);
  EXPECT_GE(list[1].second, list[2].second);
}

// --- IncrBy ---

TEST(TopkSketchTest, IncrByAppliesMultipleOccurrences) {
  auto sketch = TopkSketch::Create(2, 64, 4, 0.9);
  ASSERT_TRUE(sketch.has_value());

  sketch->IncrBy(ToSpan(std::string("a")), 5);
  auto list = sketch->List();
  ASSERT_EQ(list.size(), 1u);
  EXPECT_GE(list[0].second, 5u);
}

TEST(TopkSketchTest, IncrByCanEvict) {
  auto sketch = TopkSketch::Create(2, 64, 4, 0.9);
  ASSERT_TRUE(sketch.has_value());

  sketch->Add(ToSpan(std::string("light1")));
  sketch->Add(ToSpan(std::string("light2")));

  auto evicted = sketch->IncrBy(ToSpan(std::string("heavy")), 500);
  EXPECT_TRUE(evicted.has_value());
  EXPECT_TRUE(sketch->Query(ToSpan(std::string("heavy"))));
}

// --- Count ---

TEST(TopkSketchTest, CountOnTopKEntryReturnsExactCount) {
  auto sketch = TopkSketch::Create(3, 64, 4, 0.9);
  ASSERT_TRUE(sketch.has_value());

  auto item = std::string("counted");
  for (int i = 0; i < 7; i++) sketch->Add(ToSpan(item));
  EXPECT_EQ(sketch->Count(ToSpan(item)), 7u);
}

TEST(TopkSketchTest, CountOnAbsentItemIsZero) {
  auto sketch = TopkSketch::Create(3, 64, 4, 0.9);
  ASSERT_TRUE(sketch.has_value());
  EXPECT_EQ(sketch->Count(ToSpan(std::string("never_added"))), 0u);
}

// --- Clone ---

TEST(TopkSketchTest, CloneDeepCopiesState) {
  auto sketch = TopkSketch::Create(3, 64, 4, 0.9);
  ASSERT_TRUE(sketch.has_value());

  sketch->Add(ToSpan(std::string("a")));
  sketch->Add(ToSpan(std::string("b")));

  auto clone = sketch->Clone();
  ASSERT_TRUE(clone.has_value());
  EXPECT_NE(clone->GetCellArray(), sketch->GetCellArray());
  EXPECT_NE(clone->GetEntries(), sketch->GetEntries());
  EXPECT_EQ(clone->NumActive(), sketch->NumActive());
  EXPECT_TRUE(clone->Query(ToSpan(std::string("a"))));
  EXPECT_TRUE(clone->Query(ToSpan(std::string("b"))));

  sketch->Add(ToSpan(std::string("c")));
  sketch->Add(ToSpan(std::string("c")));
  sketch->Add(ToSpan(std::string("c")));
  EXPECT_FALSE(clone->Query(ToSpan(std::string("c"))));
}

// --- AppendEntryForLoad ---

TEST(TopkSketchTest, AppendEntryForLoadPopulatesEntriesInOrder) {
  auto sketch = TopkSketch::Create(3, 64, 4, 0.9);
  ASSERT_TRUE(sketch.has_value());

  sketch->AppendEntryForLoad(ToSpan(std::string("first")), 100);
  sketch->AppendEntryForLoad(ToSpan(std::string("second")), 50);
  EXPECT_EQ(sketch->NumActive(), 2u);

  auto list = sketch->List();
  ASSERT_EQ(list.size(), 2u);
  EXPECT_EQ(list[0].first, "first");
  EXPECT_EQ(list[0].second, 100u);
  EXPECT_EQ(list[1].first, "second");
  EXPECT_EQ(list[1].second, 50u);
}
