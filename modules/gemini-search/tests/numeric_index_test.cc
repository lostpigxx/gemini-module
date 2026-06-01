#include <gtest/gtest.h>
#include "numeric_index.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

// =============================================================
// NumericIndex
// =============================================================

TEST(NumericIndexTest, AddAndRangeQueryClosed) {
  NumericIndex idx;
  idx.Add(100, "doc1");
  idx.Add(200, "doc2");
  idx.Add(300, "doc3");
  idx.Add(400, "doc4");

  auto results = idx.RangeQuery(150, false, 350, false);
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0], "doc2");
  EXPECT_EQ(results[1], "doc3");
}

TEST(NumericIndexTest, RangeQueryIncludesBoundaries) {
  NumericIndex idx;
  idx.Add(100, "doc1");
  idx.Add(200, "doc2");
  idx.Add(300, "doc3");

  auto results = idx.RangeQuery(100, false, 300, false);
  ASSERT_EQ(results.size(), 3u);
}

TEST(NumericIndexTest, RangeQueryExclusiveMin) {
  NumericIndex idx;
  idx.Add(100, "doc1");
  idx.Add(200, "doc2");

  auto results = idx.RangeQuery(100, true, 200, false);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0], "doc2");
}

TEST(NumericIndexTest, RangeQueryExclusiveMax) {
  NumericIndex idx;
  idx.Add(100, "doc1");
  idx.Add(200, "doc2");

  auto results = idx.RangeQuery(100, false, 200, true);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0], "doc1");
}

TEST(NumericIndexTest, RangeQueryBothExclusive) {
  NumericIndex idx;
  idx.Add(100, "doc1");
  idx.Add(150, "doc2");
  idx.Add(200, "doc3");

  auto results = idx.RangeQuery(100, true, 200, true);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0], "doc2");
}

TEST(NumericIndexTest, RangeQueryUnboundedMin) {
  NumericIndex idx;
  idx.Add(10, "doc1");
  idx.Add(20, "doc2");
  idx.Add(30, "doc3");

  double neg_inf = -std::numeric_limits<double>::infinity();
  auto results = idx.RangeQuery(neg_inf, false, 20, false);
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0], "doc1");
  EXPECT_EQ(results[1], "doc2");
}

TEST(NumericIndexTest, RangeQueryUnboundedMax) {
  NumericIndex idx;
  idx.Add(10, "doc1");
  idx.Add(20, "doc2");
  idx.Add(30, "doc3");

  double pos_inf = std::numeric_limits<double>::infinity();
  auto results = idx.RangeQuery(20, false, pos_inf, false);
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0], "doc2");
  EXPECT_EQ(results[1], "doc3");
}

TEST(NumericIndexTest, RangeQueryUnboundedBoth) {
  NumericIndex idx;
  idx.Add(10, "doc1");
  idx.Add(20, "doc2");

  double neg_inf = -std::numeric_limits<double>::infinity();
  double pos_inf = std::numeric_limits<double>::infinity();
  auto results = idx.RangeQuery(neg_inf, false, pos_inf, false);
  ASSERT_EQ(results.size(), 2u);
}

TEST(NumericIndexTest, EmptyIndexReturnsEmpty) {
  NumericIndex idx;
  auto results = idx.RangeQuery(0, false, 100, false);
  EXPECT_TRUE(results.empty());
}

TEST(NumericIndexTest, NoMatchReturnsEmpty) {
  NumericIndex idx;
  idx.Add(100, "doc1");
  auto results = idx.RangeQuery(200, false, 300, false);
  EXPECT_TRUE(results.empty());
}

TEST(NumericIndexTest, MultipleDocsAtSameValue) {
  NumericIndex idx;
  idx.Add(100, "doc1");
  idx.Add(100, "doc2");
  idx.Add(100, "doc3");

  auto results = idx.RangeQuery(100, false, 100, false);
  ASSERT_EQ(results.size(), 3u);
}

TEST(NumericIndexTest, RemoveEntry) {
  NumericIndex idx;
  idx.Add(100, "doc1");
  idx.Add(100, "doc2");
  EXPECT_TRUE(idx.Remove(100, "doc1"));
  auto results = idx.RangeQuery(100, false, 100, false);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0], "doc2");
}

TEST(NumericIndexTest, RemoveLastAtValue) {
  NumericIndex idx;
  idx.Add(100, "doc1");
  idx.Remove(100, "doc1");
  EXPECT_EQ(idx.Size(), 0u);
}

TEST(NumericIndexTest, RemoveNonexistent) {
  NumericIndex idx;
  idx.Add(100, "doc1");
  EXPECT_FALSE(idx.Remove(100, "doc999"));
  EXPECT_FALSE(idx.Remove(999, "doc1"));
}

TEST(NumericIndexTest, SizeTracking) {
  NumericIndex idx;
  EXPECT_EQ(idx.Size(), 0u);
  idx.Add(10, "a");
  idx.Add(20, "b");
  EXPECT_EQ(idx.Size(), 2u);
  idx.Add(10, "c");
  EXPECT_EQ(idx.Size(), 2u);  // same value, different doc
  idx.Remove(10, "a");
  idx.Remove(10, "c");
  EXPECT_EQ(idx.Size(), 1u);  // value 10 removed entirely
}

TEST(NumericIndexTest, Clear) {
  NumericIndex idx;
  idx.Add(1, "a");
  idx.Add(2, "b");
  idx.Clear();
  EXPECT_EQ(idx.Size(), 0u);
  EXPECT_TRUE(idx.RangeQuery(-1e18, false, 1e18, false).empty());
}

TEST(NumericIndexTest, NegativeValues) {
  NumericIndex idx;
  idx.Add(-50, "doc1");
  idx.Add(-10, "doc2");
  idx.Add(0, "doc3");
  idx.Add(10, "doc4");

  auto results = idx.RangeQuery(-50, false, 0, false);
  ASSERT_EQ(results.size(), 3u);
}

TEST(NumericIndexTest, FloatingPointValues) {
  NumericIndex idx;
  idx.Add(3.14, "doc1");
  idx.Add(2.71, "doc2");
  idx.Add(1.41, "doc3");

  auto results = idx.RangeQuery(2.0, false, 3.5, false);
  ASSERT_EQ(results.size(), 2u);
  // Results are sorted by doc_id (lexicographic), not by numeric value
  EXPECT_EQ(results[0], "doc1");
  EXPECT_EQ(results[1], "doc2");
}

// =============================================================
// NumericFieldIndices
// =============================================================

TEST(NumericFieldIndicesTest, GetOrCreate) {
  NumericFieldIndices indices;
  auto& idx = indices.GetOrCreate("price");
  idx.Add(100, "doc1");
  EXPECT_EQ(idx.RangeQuery(0, false, 200, false).size(), 1u);
}

TEST(NumericFieldIndicesTest, GetNonexistentReturnsNull) {
  NumericFieldIndices indices;
  EXPECT_EQ(indices.Get("nope"), nullptr);
}

TEST(NumericFieldIndicesTest, GetExistingField) {
  NumericFieldIndices indices;
  indices.GetOrCreate("price").Add(100, "doc1");
  const auto* idx = indices.Get("price");
  ASSERT_NE(idx, nullptr);
  EXPECT_EQ(idx->RangeQuery(0, false, 200, false).size(), 1u);
}

TEST(NumericFieldIndicesTest, Clear) {
  NumericFieldIndices indices;
  indices.GetOrCreate("a").Add(1, "doc1");
  indices.GetOrCreate("b").Add(2, "doc2");
  indices.Clear();
  EXPECT_EQ(indices.Get("a"), nullptr);
  EXPECT_EQ(indices.Get("b"), nullptr);
}

