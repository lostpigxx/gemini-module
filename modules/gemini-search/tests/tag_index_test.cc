#include <gtest/gtest.h>
#include "tag_index.h"
#include "tag_query.h"

#include <string>
#include <vector>

// =============================================================
// TagIndex
// =============================================================

TEST(TagIndexTest, AddAndLookup) {
  TagIndex idx;
  idx.Add("active", "doc1");
  idx.Add("active", "doc2");
  auto results = idx.Lookup("active");
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0], "doc1");
  EXPECT_EQ(results[1], "doc2");
}

TEST(TagIndexTest, LookupMissingTagReturnsEmpty) {
  TagIndex idx;
  idx.Add("active", "doc1");
  EXPECT_TRUE(idx.Lookup("inactive").empty());
}

TEST(TagIndexTest, LookupEmptyIndex) {
  TagIndex idx;
  EXPECT_TRUE(idx.Lookup("anything").empty());
}

TEST(TagIndexTest, RemoveFromPostingList) {
  TagIndex idx;
  idx.Add("active", "doc1");
  idx.Add("active", "doc2");
  EXPECT_TRUE(idx.Remove("active", "doc1"));
  auto results = idx.Lookup("active");
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0], "doc2");
}

TEST(TagIndexTest, RemoveLastEntryRemovesTag) {
  TagIndex idx;
  idx.Add("active", "doc1");
  idx.Remove("active", "doc1");
  EXPECT_EQ(idx.NumTags(), 0u);
}

TEST(TagIndexTest, RemoveNonexistentReturnsFalse) {
  TagIndex idx;
  idx.Add("active", "doc1");
  EXPECT_FALSE(idx.Remove("active", "doc999"));
  EXPECT_FALSE(idx.Remove("nonexistent", "doc1"));
}

TEST(TagIndexTest, LookupOrUnion) {
  TagIndex idx;
  idx.Add("red", "doc1");
  idx.Add("blue", "doc2");
  idx.Add("red", "doc3");
  idx.Add("green", "doc4");

  auto results = idx.LookupOr({"red", "blue"});
  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(results[0], "doc1");
  EXPECT_EQ(results[1], "doc2");
  EXPECT_EQ(results[2], "doc3");
}

TEST(TagIndexTest, LookupOrDeduplicates) {
  TagIndex idx;
  idx.Add("red", "doc1");
  idx.Add("blue", "doc1");
  auto results = idx.LookupOr({"red", "blue"});
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0], "doc1");
}

TEST(TagIndexTest, LookupOrWithMissingValues) {
  TagIndex idx;
  idx.Add("red", "doc1");
  auto results = idx.LookupOr({"red", "nonexistent"});
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0], "doc1");
}

TEST(TagIndexTest, LookupOrAllMissing) {
  TagIndex idx;
  auto results = idx.LookupOr({"a", "b"});
  EXPECT_TRUE(results.empty());
}

TEST(TagIndexTest, NumTags) {
  TagIndex idx;
  idx.Add("a", "doc1");
  idx.Add("b", "doc1");
  idx.Add("c", "doc2");
  EXPECT_EQ(idx.NumTags(), 3u);
}

TEST(TagIndexTest, Clear) {
  TagIndex idx;
  idx.Add("a", "doc1");
  idx.Add("b", "doc2");
  idx.Clear();
  EXPECT_EQ(idx.NumTags(), 0u);
  EXPECT_TRUE(idx.Lookup("a").empty());
}

// =============================================================
// TagFieldIndices
// =============================================================

TEST(TagFieldIndicesTest, GetOrCreateReturnsReference) {
  TagFieldIndices indices;
  auto& idx = indices.GetOrCreate("status");
  idx.Add("active", "doc1");
  EXPECT_EQ(idx.Lookup("active").size(), 1u);
}

TEST(TagFieldIndicesTest, GetExistingField) {
  TagFieldIndices indices;
  indices.GetOrCreate("status").Add("active", "doc1");
  const auto* idx = indices.Get("status");
  ASSERT_NE(idx, nullptr);
  EXPECT_EQ(idx->Lookup("active").size(), 1u);
}

TEST(TagFieldIndicesTest, GetNonexistentReturnsNull) {
  TagFieldIndices indices;
  EXPECT_EQ(indices.Get("nope"), nullptr);
}

TEST(TagFieldIndicesTest, MultipleFields) {
  TagFieldIndices indices;
  indices.GetOrCreate("status").Add("active", "doc1");
  indices.GetOrCreate("region").Add("us", "doc1");
  indices.GetOrCreate("region").Add("eu", "doc2");

  EXPECT_EQ(indices.Get("status")->Lookup("active").size(), 1u);
  EXPECT_EQ(indices.Get("region")->Lookup("us").size(), 1u);
  EXPECT_EQ(indices.Get("region")->Lookup("eu").size(), 1u);
}

TEST(TagFieldIndicesTest, Clear) {
  TagFieldIndices indices;
  indices.GetOrCreate("a").Add("v", "doc1");
  indices.GetOrCreate("b").Add("v", "doc2");
  indices.Clear();
  EXPECT_EQ(indices.Get("a"), nullptr);
  EXPECT_EQ(indices.Get("b"), nullptr);
}

// =============================================================
// TagQuery Parser
// =============================================================

TEST(TagQueryParserTest, MatchAll) {
  TagQuery q;
  std::string err;
  ASSERT_TRUE(ParseTagQuery("*", q, err));
  EXPECT_EQ(q.type, TagQuery::Type::kMatchAll);
}

TEST(TagQueryParserTest, MatchAllWithWhitespace) {
  TagQuery q;
  std::string err;
  ASSERT_TRUE(ParseTagQuery("  *  ", q, err));
  EXPECT_EQ(q.type, TagQuery::Type::kMatchAll);
}

TEST(TagQueryParserTest, SingleValue) {
  TagQuery q;
  std::string err;
  ASSERT_TRUE(ParseTagQuery("@status:{active}", q, err));
  EXPECT_EQ(q.type, TagQuery::Type::kTagMatch);
  EXPECT_EQ(q.field_name, "status");
  ASSERT_EQ(q.tag_values.size(), 1u);
  EXPECT_EQ(q.tag_values[0], "active");
}

TEST(TagQueryParserTest, MultiValueOr) {
  TagQuery q;
  std::string err;
  ASSERT_TRUE(ParseTagQuery("@color:{red|blue|green}", q, err));
  EXPECT_EQ(q.type, TagQuery::Type::kTagMatch);
  EXPECT_EQ(q.field_name, "color");
  ASSERT_EQ(q.tag_values.size(), 3u);
  EXPECT_EQ(q.tag_values[0], "red");
  EXPECT_EQ(q.tag_values[1], "blue");
  EXPECT_EQ(q.tag_values[2], "green");
}

TEST(TagQueryParserTest, WhitespaceAroundQuery) {
  TagQuery q;
  std::string err;
  ASSERT_TRUE(ParseTagQuery("  @name:{Alice}  ", q, err));
  EXPECT_EQ(q.field_name, "name");
  ASSERT_EQ(q.tag_values.size(), 1u);
  EXPECT_EQ(q.tag_values[0], "Alice");
}

TEST(TagQueryParserTest, ErrorEmptyQuery) {
  TagQuery q;
  std::string err;
  EXPECT_FALSE(ParseTagQuery("", q, err));
  EXPECT_FALSE(err.empty());
}

TEST(TagQueryParserTest, ErrorWhitespaceOnly) {
  TagQuery q;
  std::string err;
  EXPECT_FALSE(ParseTagQuery("   ", q, err));
}

TEST(TagQueryParserTest, ErrorNoAtSign) {
  TagQuery q;
  std::string err;
  EXPECT_FALSE(ParseTagQuery("status:{active}", q, err));
}

TEST(TagQueryParserTest, ErrorMissingBrace) {
  TagQuery q;
  std::string err;
  EXPECT_FALSE(ParseTagQuery("@status:active", q, err));
}

TEST(TagQueryParserTest, ErrorMissingClosingBrace) {
  TagQuery q;
  std::string err;
  EXPECT_FALSE(ParseTagQuery("@status:{active", q, err));
}

TEST(TagQueryParserTest, ErrorEmptyFieldName) {
  TagQuery q;
  std::string err;
  EXPECT_FALSE(ParseTagQuery("@:{active}", q, err));
}

TEST(TagQueryParserTest, ErrorEmptyTagValue) {
  TagQuery q;
  std::string err;
  EXPECT_FALSE(ParseTagQuery("@field:{}", q, err));
}

TEST(TagQueryParserTest, ErrorEmptyOrSegment) {
  TagQuery q;
  std::string err;
  EXPECT_FALSE(ParseTagQuery("@field:{a||b}", q, err));
}

TEST(TagQueryParserTest, ErrorTrailingPipe) {
  TagQuery q;
  std::string err;
  EXPECT_FALSE(ParseTagQuery("@field:{a|}", q, err));
}
