#include <gtest/gtest.h>
#include "query_parser.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

// =============================================================
// Set operations
// =============================================================

TEST(SetOpsTest, IntersectBasic) {
  auto r = SetIntersect({"a", "b", "c"}, {"b", "c", "d"});
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0], "b");
  EXPECT_EQ(r[1], "c");
}

TEST(SetOpsTest, IntersectDisjoint) {
  auto r = SetIntersect({"a", "b"}, {"c", "d"});
  EXPECT_TRUE(r.empty());
}

TEST(SetOpsTest, IntersectEmpty) {
  auto r = SetIntersect({}, {"a", "b"});
  EXPECT_TRUE(r.empty());
}

TEST(SetOpsTest, UnionBasic) {
  auto r = SetUnion({"a", "c"}, {"b", "c"});
  ASSERT_EQ(r.size(), 3u);
  EXPECT_EQ(r[0], "a");
  EXPECT_EQ(r[1], "b");
  EXPECT_EQ(r[2], "c");
}

TEST(SetOpsTest, UnionEmpty) {
  auto r = SetUnion({}, {"a"});
  ASSERT_EQ(r.size(), 1u);
}

TEST(SetOpsTest, DifferenceBasic) {
  auto r = SetDifference({"a", "b", "c"}, {"b"});
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0], "a");
  EXPECT_EQ(r[1], "c");
}

TEST(SetOpsTest, DifferenceEmpty) {
  auto r = SetDifference({"a", "b"}, {});
  ASSERT_EQ(r.size(), 2u);
}

TEST(SetOpsTest, DifferenceAll) {
  auto r = SetDifference({"a", "b"}, {"a", "b"});
  EXPECT_TRUE(r.empty());
}

// =============================================================
// Parser: leaf nodes
// =============================================================

TEST(QueryParserTest, MatchAll) {
  ParsedQuery q;
  std::string err;
  ASSERT_TRUE(ParseQuery("*", q, err)) << err;
  EXPECT_EQ(q.root.type, QueryNode::Type::kMatchAll);
  EXPECT_FALSE(q.has_knn);
}

TEST(QueryParserTest, MatchAllWhitespace) {
  ParsedQuery q;
  std::string err;
  ASSERT_TRUE(ParseQuery("  *  ", q, err));
  EXPECT_EQ(q.root.type, QueryNode::Type::kMatchAll);
}

TEST(QueryParserTest, TagSingleValue) {
  ParsedQuery q;
  std::string err;
  ASSERT_TRUE(ParseQuery("@status:{active}", q, err)) << err;
  EXPECT_EQ(q.root.type, QueryNode::Type::kTagMatch);
  EXPECT_EQ(q.root.field_name, "status");
  ASSERT_EQ(q.root.tag_values.size(), 1u);
  EXPECT_EQ(q.root.tag_values[0], "active");
}

TEST(QueryParserTest, TagMultiValue) {
  ParsedQuery q;
  std::string err;
  ASSERT_TRUE(ParseQuery("@color:{red|blue|green}", q, err));
  EXPECT_EQ(q.root.type, QueryNode::Type::kTagMatch);
  ASSERT_EQ(q.root.tag_values.size(), 3u);
}

TEST(QueryParserTest, NumericRange) {
  ParsedQuery q;
  std::string err;
  ASSERT_TRUE(ParseQuery("@price:[100 500]", q, err));
  EXPECT_EQ(q.root.type, QueryNode::Type::kNumericRange);
  EXPECT_EQ(q.root.field_name, "price");
  EXPECT_DOUBLE_EQ(q.root.range_min, 100.0);
  EXPECT_DOUBLE_EQ(q.root.range_max, 500.0);
  EXPECT_FALSE(q.root.min_exclusive);
  EXPECT_FALSE(q.root.max_exclusive);
}

TEST(QueryParserTest, NumericExclusive) {
  ParsedQuery q;
  std::string err;
  ASSERT_TRUE(ParseQuery("@price:[(100 (500]", q, err));
  EXPECT_TRUE(q.root.min_exclusive);
  EXPECT_TRUE(q.root.max_exclusive);
}

TEST(QueryParserTest, NumericInf) {
  ParsedQuery q;
  std::string err;
  ASSERT_TRUE(ParseQuery("@price:[-inf +inf]", q, err));
  EXPECT_EQ(q.root.range_min, -std::numeric_limits<double>::infinity());
  EXPECT_EQ(q.root.range_max, std::numeric_limits<double>::infinity());
}

// =============================================================
// Parser: boolean combinations
// =============================================================

TEST(QueryParserTest, AndTwoConditions) {
  ParsedQuery q;
  std::string err;
  ASSERT_TRUE(ParseQuery("@a:{x} @b:{y}", q, err)) << err;
  EXPECT_EQ(q.root.type, QueryNode::Type::kAnd);
  ASSERT_EQ(q.root.children.size(), 2u);
  EXPECT_EQ(q.root.children[0].type, QueryNode::Type::kTagMatch);
  EXPECT_EQ(q.root.children[0].field_name, "a");
  EXPECT_EQ(q.root.children[1].type, QueryNode::Type::kTagMatch);
  EXPECT_EQ(q.root.children[1].field_name, "b");
}

TEST(QueryParserTest, AndThreeConditions) {
  ParsedQuery q;
  std::string err;
  ASSERT_TRUE(ParseQuery("@a:{x} @b:{y} @c:{z}", q, err));
  EXPECT_EQ(q.root.type, QueryNode::Type::kAnd);
  EXPECT_EQ(q.root.children[0].type, QueryNode::Type::kAnd);
  EXPECT_EQ(q.root.children[1].field_name, "c");
}

TEST(QueryParserTest, OrTwoConditions) {
  ParsedQuery q;
  std::string err;
  ASSERT_TRUE(ParseQuery("@a:{x} | @b:{y}", q, err)) << err;
  EXPECT_EQ(q.root.type, QueryNode::Type::kOr);
  ASSERT_EQ(q.root.children.size(), 2u);
}

TEST(QueryParserTest, NotCondition) {
  ParsedQuery q;
  std::string err;
  ASSERT_TRUE(ParseQuery("-@a:{x}", q, err)) << err;
  EXPECT_EQ(q.root.type, QueryNode::Type::kNot);
  ASSERT_EQ(q.root.children.size(), 1u);
  EXPECT_EQ(q.root.children[0].type, QueryNode::Type::kTagMatch);
  EXPECT_EQ(q.root.children[0].field_name, "a");
}

TEST(QueryParserTest, PrecedenceAndOverOr) {
  // @a:{x} @b:{y} | @c:{z} → OR(AND(a,b), c)
  ParsedQuery q;
  std::string err;
  ASSERT_TRUE(ParseQuery("@a:{x} @b:{y} | @c:{z}", q, err)) << err;
  EXPECT_EQ(q.root.type, QueryNode::Type::kOr);
  EXPECT_EQ(q.root.children[0].type, QueryNode::Type::kAnd);
  EXPECT_EQ(q.root.children[1].field_name, "c");
}

TEST(QueryParserTest, ParenthesizedGrouping) {
  // (@a:{x} | @b:{y}) @c:[0 100]
  ParsedQuery q;
  std::string err;
  ASSERT_TRUE(ParseQuery("(@a:{x} | @b:{y}) @c:[0 100]", q, err)) << err;
  EXPECT_EQ(q.root.type, QueryNode::Type::kAnd);
  EXPECT_EQ(q.root.children[0].type, QueryNode::Type::kOr);
  EXPECT_EQ(q.root.children[1].type, QueryNode::Type::kNumericRange);
}

TEST(QueryParserTest, NotWithAnd) {
  // -@a:{x} @b:{y} → AND(NOT(a), b)
  ParsedQuery q;
  std::string err;
  ASSERT_TRUE(ParseQuery("-@a:{x} @b:{y}", q, err)) << err;
  EXPECT_EQ(q.root.type, QueryNode::Type::kAnd);
  EXPECT_EQ(q.root.children[0].type, QueryNode::Type::kNot);
  EXPECT_EQ(q.root.children[1].type, QueryNode::Type::kTagMatch);
}

TEST(QueryParserTest, MixedTagAndNumeric) {
  ParsedQuery q;
  std::string err;
  ASSERT_TRUE(ParseQuery("@status:{active} @price:[100 500]", q, err));
  EXPECT_EQ(q.root.type, QueryNode::Type::kAnd);
  EXPECT_EQ(q.root.children[0].type, QueryNode::Type::kTagMatch);
  EXPECT_EQ(q.root.children[1].type, QueryNode::Type::kNumericRange);
}

// =============================================================
// Parser: KNN
// =============================================================

TEST(QueryParserTest, KnnBasic) {
  ParsedQuery q;
  std::string err;
  ASSERT_TRUE(ParseQuery("*=>[KNN 5 @embedding $blob]", q, err)) << err;
  EXPECT_TRUE(q.has_knn);
  EXPECT_EQ(q.knn_k, 5u);
  EXPECT_EQ(q.knn_field, "embedding");
  EXPECT_EQ(q.knn_param_name, "blob");
  EXPECT_EQ(q.root.type, QueryNode::Type::kMatchAll);
}

TEST(QueryParserTest, KnnWithSpaces) {
  ParsedQuery q;
  std::string err;
  ASSERT_TRUE(ParseQuery("  * => [KNN 10 @vec $query_vec]  ", q, err));
  EXPECT_TRUE(q.has_knn);
  EXPECT_EQ(q.knn_k, 10u);
}

// =============================================================
// Parser: error cases
// =============================================================

TEST(QueryParserTest, ErrorEmpty) {
  ParsedQuery q;
  std::string err;
  EXPECT_FALSE(ParseQuery("", q, err));
}

TEST(QueryParserTest, ErrorWhitespace) {
  ParsedQuery q;
  std::string err;
  EXPECT_FALSE(ParseQuery("   ", q, err));
}

TEST(QueryParserTest, ErrorUnmatchedParen) {
  ParsedQuery q;
  std::string err;
  EXPECT_FALSE(ParseQuery("(@a:{x}", q, err));
}

TEST(QueryParserTest, ErrorExtraParen) {
  ParsedQuery q;
  std::string err;
  EXPECT_FALSE(ParseQuery("@a:{x})", q, err));
}

TEST(QueryParserTest, ErrorEmptyTagValue) {
  ParsedQuery q;
  std::string err;
  EXPECT_FALSE(ParseQuery("@a:{}", q, err));
}

TEST(QueryParserTest, ErrorTrailingPipe) {
  ParsedQuery q;
  std::string err;
  EXPECT_FALSE(ParseQuery("@a:{x|}", q, err));
}

TEST(QueryParserTest, ErrorMissingClosingBrace) {
  ParsedQuery q;
  std::string err;
  EXPECT_FALSE(ParseQuery("@a:{x", q, err));
}

TEST(QueryParserTest, ErrorKnnZeroK) {
  ParsedQuery q;
  std::string err;
  EXPECT_FALSE(ParseQuery("*=>[KNN 0 @emb $blob]", q, err));
}

TEST(QueryParserTest, ErrorKnnMissingAt) {
  ParsedQuery q;
  std::string err;
  EXPECT_FALSE(ParseQuery("*=>[KNN 5 emb $blob]", q, err));
}

TEST(QueryParserTest, ErrorKnnMissingDollar) {
  ParsedQuery q;
  std::string err;
  EXPECT_FALSE(ParseQuery("*=>[KNN 5 @emb blob]", q, err));
}

// =============================================================
// Evaluation (integration with real indices)
// =============================================================

class QueryEvalTest : public ::testing::Test {
 protected:
  IndexSpec spec;
  DocumentStore doc_store;
  TagFieldIndices tag_indices;
  NumericFieldIndices numeric_indices;
  TextFieldIndices text_indices;

  void SetUp() override {
    spec.name = "test";
    spec.fields = {
        {"category", FieldType::kTag, {}},
        {"status", FieldType::kTag, {}},
        {"price", FieldType::kNumeric, {}},
    };

    doc_store.Add("d1", {{"category", "shoes"}, {"status", "active"}, {"price", "100"}});
    doc_store.Add("d2", {{"category", "hat"}, {"status", "active"}, {"price", "50"}});
    doc_store.Add("d3", {{"category", "shoes"}, {"status", "inactive"}, {"price", "200"}});
    doc_store.Add("d4", {{"category", "boots"}, {"status", "active"}, {"price", "300"}});

    tag_indices.GetOrCreate("category").Add("shoes", "d1");
    tag_indices.GetOrCreate("category").Add("hat", "d2");
    tag_indices.GetOrCreate("category").Add("shoes", "d3");
    tag_indices.GetOrCreate("category").Add("boots", "d4");

    tag_indices.GetOrCreate("status").Add("active", "d1");
    tag_indices.GetOrCreate("status").Add("active", "d2");
    tag_indices.GetOrCreate("status").Add("inactive", "d3");
    tag_indices.GetOrCreate("status").Add("active", "d4");

    numeric_indices.GetOrCreate("price").Add(100, "d1");
    numeric_indices.GetOrCreate("price").Add(50, "d2");
    numeric_indices.GetOrCreate("price").Add(200, "d3");
    numeric_indices.GetOrCreate("price").Add(300, "d4");
  }

  std::vector<std::string> Eval(const std::string& query_str) {
    ParsedQuery pq;
    std::string parse_err;
    if (!ParseQuery(query_str, pq, parse_err)) return {};
    std::string eval_err;
    return EvaluateQuery(pq.root, spec, doc_store, tag_indices,
                          numeric_indices, text_indices, eval_err);
  }
};

TEST_F(QueryEvalTest, MatchAll) {
  auto r = Eval("*");
  EXPECT_EQ(r.size(), 4u);
}

TEST_F(QueryEvalTest, SingleTag) {
  auto r = Eval("@category:{shoes}");
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0], "d1");
  EXPECT_EQ(r[1], "d3");
}

TEST_F(QueryEvalTest, SingleNumeric) {
  auto r = Eval("@price:[50 150]");
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0], "d1");
  EXPECT_EQ(r[1], "d2");
}

TEST_F(QueryEvalTest, AndTagTag) {
  auto r = Eval("@category:{shoes} @status:{active}");
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0], "d1");
}

TEST_F(QueryEvalTest, AndTagNumeric) {
  auto r = Eval("@status:{active} @price:[0 100]");
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0], "d1");
  EXPECT_EQ(r[1], "d2");
}

TEST_F(QueryEvalTest, OrTags) {
  auto r = Eval("@category:{shoes} | @category:{boots}");
  ASSERT_EQ(r.size(), 3u);
}

TEST_F(QueryEvalTest, NotTag) {
  auto r = Eval("-@category:{shoes}");
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0], "d2");
  EXPECT_EQ(r[1], "d4");
}

TEST_F(QueryEvalTest, GroupedOrAndNumeric) {
  // (@category:{shoes} | @category:{boots}) @price:[100 300]
  auto r = Eval("(@category:{shoes} | @category:{boots}) @price:[100 300]");
  ASSERT_EQ(r.size(), 3u);
}

TEST_F(QueryEvalTest, NotAndAnd) {
  // -@status:{inactive} @price:[0 200]
  auto r = Eval("-@status:{inactive} @price:[0 200]");
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0], "d1");
  EXPECT_EQ(r[1], "d2");
}

TEST_F(QueryEvalTest, EmptyResult) {
  auto r = Eval("@category:{nonexistent}");
  EXPECT_TRUE(r.empty());
}
