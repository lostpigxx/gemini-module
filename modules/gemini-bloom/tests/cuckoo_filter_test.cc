#include <gtest/gtest.h>
#include "cuckoo_filter.h"

#include <cstring>
#include <string>
#include <vector>

static std::span<const std::byte> ToSpan(const std::string& s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

// --- Fingerprint / candidate-bucket derivation ---

TEST(CfHashTest, FingerprintNeverZero) {
  for (int i = 0; i < 10000; i++) {
    auto item = "fp_" + std::to_string(i);
    EXPECT_NE(CfFingerprint(ToSpan(item)), 0u);
  }
}

TEST(CfHashTest, FingerprintDeterministic) {
  auto item = std::string("deterministic");
  EXPECT_EQ(CfFingerprint(ToSpan(item)), CfFingerprint(ToSpan(item)));
}

TEST(CfHashTest, AltIndexIsInvolutive) {
  constexpr uint64_t numBuckets = 1024;
  for (int i = 0; i < 10000; i++) {
    auto item = "alt_" + std::to_string(i);
    uint8_t fp = CfFingerprint(ToSpan(item));
    uint64_t i1 = CfIndexOf(ToSpan(item), numBuckets);
    uint64_t i2 = CfAltIndex(i1, fp, numBuckets);
    EXPECT_EQ(CfAltIndex(i2, fp, numBuckets), i1)
      << "AltIndex must be involutive for item " << item;
    EXPECT_LT(i1, numBuckets);
    EXPECT_LT(i2, numBuckets);
  }
}

// --- Create ---

TEST(CuckooFilterTest, CreateRejectsNonPowerOfTwoBuckets) {
  EXPECT_FALSE(CuckooFilter::Create(100, 2, 20).has_value());
  EXPECT_TRUE(CuckooFilter::Create(128, 2, 20).has_value());
}

TEST(CuckooFilterTest, CreateRejectsInvalidParameters) {
  EXPECT_FALSE(CuckooFilter::Create(0, 2, 20).has_value());
  EXPECT_FALSE(CuckooFilter::Create(128, 0, 20).has_value());
  EXPECT_FALSE(CuckooFilter::Create(128, 2, 0).has_value());
  EXPECT_FALSE(CuckooFilter::Create(1ULL << 40, 2, 20).has_value());
  EXPECT_FALSE(CuckooFilter::Create(128, kCfMaxBucketSize + 1, 20).has_value());
  EXPECT_FALSE(CuckooFilter::Create(128, 2, kCfMaxIterations + 1).has_value());
}

TEST(CuckooFilterTest, CreateRAII) {
  auto filter = CuckooFilter::Create(128, 2, 20);
  ASSERT_TRUE(filter.has_value());
  EXPECT_EQ(filter->NumBuckets(), 128u);
  EXPECT_EQ(filter->BucketSize(), 2u);
  EXPECT_EQ(filter->MaxIterations(), 20u);
  EXPECT_EQ(filter->NumItems(), 0u);
  EXPECT_NE(filter->GetBucketArray(), nullptr);
  EXPECT_EQ(filter->GetDataSize(), 256u);
}

TEST(CuckooFilterTest, MoveSemantics) {
  auto filter = CuckooFilter::Create(128, 2, 20);
  ASSERT_TRUE(filter.has_value());

  CuckooFilter moved = std::move(*filter);
  EXPECT_NE(moved.GetBucketArray(), nullptr);
  EXPECT_EQ(filter->GetBucketArray(), nullptr);
}

// --- Insert / Contains ---

TEST(CuckooFilterTest, InsertAndContains) {
  auto filter = CuckooFilter::Create(128, 2, 20);
  ASSERT_TRUE(filter.has_value());

  auto item = std::string("hello");
  auto r1 = filter->Insert(ToSpan(item));
  ASSERT_TRUE(r1.has_value());
  EXPECT_TRUE(*r1);
  EXPECT_TRUE(filter->Contains(ToSpan(item)));
  EXPECT_EQ(filter->NumItems(), 1u);
}

TEST(CuckooFilterTest, ContainsFalseForAbsentItem) {
  auto filter = CuckooFilter::Create(128, 2, 20);
  ASSERT_TRUE(filter.has_value());
  filter->Insert(ToSpan(std::string("present")));
  EXPECT_FALSE(filter->Contains(ToSpan(std::string("absent"))));
}

TEST(CuckooFilterTest, NoFalseNegatives) {
  auto filter = CuckooFilter::Create(1024, 4, 500);
  ASSERT_TRUE(filter.has_value());

  std::vector<std::string> items;
  for (int i = 0; i < 2000; i++) {
    auto item = "item_" + std::to_string(i);
    auto result = filter->Insert(ToSpan(item));
    if (!result.has_value()) break;
    items.push_back(item);
  }
  EXPECT_GT(items.size(), 0u);

  for (const auto& item : items) {
    EXPECT_TRUE(filter->Contains(ToSpan(item))) << "False negative for " << item;
  }
}

TEST(CuckooFilterTest, FullFilterReturnsNullopt) {
  auto filter = CuckooFilter::Create(4, 1, 10);
  ASSERT_TRUE(filter.has_value());

  int inserted = 0;
  bool sawFull = false;
  for (int i = 0; i < 100; i++) {
    auto item = "full_" + std::to_string(i);
    auto result = filter->Insert(ToSpan(item));
    if (!result.has_value()) {
      sawFull = true;
      break;
    }
    inserted++;
  }
  EXPECT_TRUE(sawFull) << "Expected filter to eventually report full";
  EXPECT_LE(inserted, 4 * 1);
}

// --- Delete ---

TEST(CuckooFilterTest, DeleteThenContainsIsFalse) {
  auto filter = CuckooFilter::Create(128, 2, 20);
  ASSERT_TRUE(filter.has_value());

  auto item = std::string("deleteme");
  ASSERT_TRUE(filter->Insert(ToSpan(item)).has_value());
  EXPECT_TRUE(filter->Contains(ToSpan(item)));

  EXPECT_TRUE(filter->Delete(ToSpan(item)));
  EXPECT_FALSE(filter->Contains(ToSpan(item)));
  EXPECT_EQ(filter->NumItems(), 0u);
}

TEST(CuckooFilterTest, DeleteAbsentItemReturnsFalse) {
  auto filter = CuckooFilter::Create(128, 2, 20);
  ASSERT_TRUE(filter.has_value());
  EXPECT_FALSE(filter->Delete(ToSpan(std::string("never_inserted"))));
}

TEST(CuckooFilterTest, DeleteOneOfDuplicatesKeepsOthers) {
  auto filter = CuckooFilter::Create(128, 4, 20);
  ASSERT_TRUE(filter.has_value());

  auto item = std::string("dup");
  ASSERT_TRUE(filter->Insert(ToSpan(item)).has_value());
  ASSERT_TRUE(filter->Insert(ToSpan(item)).has_value());
  EXPECT_EQ(filter->Count(ToSpan(item)), 2u);

  EXPECT_TRUE(filter->Delete(ToSpan(item)));
  EXPECT_EQ(filter->Count(ToSpan(item)), 1u);
  EXPECT_TRUE(filter->Contains(ToSpan(item)));
}

// --- Count ---

TEST(CuckooFilterTest, CountReflectsInsertions) {
  auto filter = CuckooFilter::Create(128, 4, 20);
  ASSERT_TRUE(filter.has_value());

  auto item = std::string("counted");
  EXPECT_EQ(filter->Count(ToSpan(item)), 0u);
  filter->Insert(ToSpan(item));
  EXPECT_EQ(filter->Count(ToSpan(item)), 1u);
  filter->Insert(ToSpan(item));
  EXPECT_EQ(filter->Count(ToSpan(item)), 2u);
}

// --- RecountItems ---

TEST(CuckooFilterTest, RecountItemsMatchesNonEmptySlots) {
  auto filter = CuckooFilter::Create(128, 2, 20);
  ASSERT_TRUE(filter.has_value());

  for (int i = 0; i < 50; i++) {
    filter->Insert(ToSpan("recount_" + std::to_string(i)));
  }
  uint64_t before = filter->NumItems();

  // Simulate a raw memcpy-based restore that bypasses Insert() bookkeeping.
  filter->RecountItems();
  EXPECT_EQ(filter->NumItems(), before);
}

// --- Clone ---

TEST(CuckooFilterTest, CloneDeepCopiesBucketArray) {
  auto filter = CuckooFilter::Create(128, 2, 20);
  ASSERT_TRUE(filter.has_value());

  std::vector<std::string> items;
  for (int i = 0; i < 50; i++) {
    auto item = "clone_" + std::to_string(i);
    if (filter->Insert(ToSpan(item)).has_value()) items.push_back(item);
  }

  auto clone = filter->Clone();
  ASSERT_TRUE(clone.has_value());

  EXPECT_NE(clone->GetBucketArray(), filter->GetBucketArray());
  EXPECT_EQ(clone->GetDataSize(), filter->GetDataSize());
  EXPECT_EQ(std::memcmp(clone->GetBucketArray(), filter->GetBucketArray(),
                        filter->GetDataSize()), 0);
  EXPECT_EQ(clone->NumItems(), filter->NumItems());

  for (const auto& item : items) {
    EXPECT_TRUE(clone->Contains(ToSpan(item)));
  }
}

TEST(CuckooFilterTest, CloneIsIndependentOfOriginal) {
  auto filter = CuckooFilter::Create(128, 2, 20);
  ASSERT_TRUE(filter.has_value());

  auto clone = filter->Clone();
  ASSERT_TRUE(clone.has_value());

  auto item = std::string("after_clone");
  filter->Insert(ToSpan(item));

  EXPECT_TRUE(filter->Contains(ToSpan(item)));
  EXPECT_FALSE(clone->Contains(ToSpan(item)));
}
