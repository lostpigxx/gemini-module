#include <gtest/gtest.h>
#include "json_path.h"
#include "json_parse.h"
#include "json_value.h"

#include <string>

static JsonValue* Parse(const char* input) {
  return JsonParse(input).value;
}

static JsonValue* MakeTestDoc() {
  return Parse(R"({
    "store": {
      "book": [
        {"category": "reference", "author": "Nigel", "title": "Sayings", "price": 8.95},
        {"category": "fiction", "author": "Evelyn", "title": "Sword", "price": 12.99},
        {"category": "fiction", "author": "Herman", "title": "Moby", "price": 8.99},
        {"category": "fiction", "author": "Tolkien", "title": "LOTR", "price": 22.99}
      ],
      "bicycle": {"color": "red", "price": 19.95}
    },
    "expensive": 10
  })");
}

// --- Path Parsing ---

TEST(PathParseTest, RootDollar) {
  auto r = ParsePath("$");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_EQ(r.path.segments.size(), 1u);
  EXPECT_EQ(r.path.segments[0].type, PathSegType::kRoot);
  EXPECT_FALSE(r.path.is_legacy);
}

TEST(PathParseTest, RootDot) {
  auto r = ParsePath(".");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_TRUE(r.path.is_legacy);
}

TEST(PathParseTest, EmptyPath) {
  auto r = ParsePath("");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_TRUE(r.path.is_legacy);
}

TEST(PathParseTest, DotKey) {
  auto r = ParsePath("$.store");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_EQ(r.path.segments.size(), 2u);
  EXPECT_EQ(r.path.segments[1].type, PathSegType::kKey);
  EXPECT_EQ(r.path.segments[1].key, "store");
}

TEST(PathParseTest, NestedDotKey) {
  auto r = ParsePath("$.store.book");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_EQ(r.path.segments.size(), 3u);
  EXPECT_EQ(r.path.segments[1].key, "store");
  EXPECT_EQ(r.path.segments[2].key, "book");
}

TEST(PathParseTest, BracketKey) {
  auto r = ParsePath("$[\"store\"]");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_EQ(r.path.segments.size(), 2u);
  EXPECT_EQ(r.path.segments[1].type, PathSegType::kKey);
  EXPECT_EQ(r.path.segments[1].key, "store");
}

TEST(PathParseTest, ArrayIndex) {
  auto r = ParsePath("$.arr[0]");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_EQ(r.path.segments.size(), 3u);
  EXPECT_EQ(r.path.segments[2].type, PathSegType::kIndex);
  EXPECT_EQ(r.path.segments[2].index, 0);
}

TEST(PathParseTest, NegativeIndex) {
  auto r = ParsePath("$.arr[-1]");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_EQ(r.path.segments[2].index, -1);
}

TEST(PathParseTest, Wildcard) {
  auto r = ParsePath("$.store.*");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_EQ(r.path.segments.back().type, PathSegType::kWildcard);
}

TEST(PathParseTest, BracketWildcard) {
  auto r = ParsePath("$.arr[*]");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_EQ(r.path.segments.back().type, PathSegType::kWildcard);
}

TEST(PathParseTest, RecursiveDescent) {
  auto r = ParsePath("$..price");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_GE(r.path.segments.size(), 3u);
  bool found_recursive = false;
  for (auto& seg : r.path.segments) {
    if (seg.type == PathSegType::kRecursive) found_recursive = true;
  }
  EXPECT_TRUE(found_recursive);
}

TEST(PathParseTest, Slice) {
  auto r = ParsePath("$.arr[0:3]");
  EXPECT_EQ(r.error, nullptr);
  auto& seg = r.path.segments.back();
  EXPECT_EQ(seg.type, PathSegType::kSlice);
  EXPECT_EQ(seg.slice.start, 0);
  EXPECT_EQ(seg.slice.stop, 3);
  EXPECT_TRUE(seg.slice.start_set);
  EXPECT_TRUE(seg.slice.stop_set);
}

TEST(PathParseTest, SliceWithStep) {
  auto r = ParsePath("$.arr[0:10:2]");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_EQ(r.path.segments.back().slice.step, 2);
}

TEST(PathParseTest, LegacyPath) {
  auto r = ParsePath(".store.book[0].title");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_TRUE(r.path.is_legacy);
  EXPECT_GE(r.path.segments.size(), 4u);
}

// --- Path Evaluation ---

TEST(PathEvalTest, Root) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$");
  auto matches = EvaluatePath(pr.path, doc);
  EXPECT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches[0].value, doc);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, SimpleKey) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$.expensive");
  auto matches = EvaluatePath(pr.path, doc);
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches[0].value->GetInteger(), 10);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, NestedKey) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$.store.bicycle.color");
  auto matches = EvaluatePath(pr.path, doc);
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches[0].value->GetString(), "red");
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, ArrayIndex) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$.store.book[0].author");
  auto matches = EvaluatePath(pr.path, doc);
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches[0].value->GetString(), "Nigel");
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, NegativeIndex) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$.store.book[-1].author");
  auto matches = EvaluatePath(pr.path, doc);
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches[0].value->GetString(), "Tolkien");
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, Wildcard) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$.store.book[*].author");
  auto matches = EvaluatePath(pr.path, doc);
  EXPECT_EQ(matches.size(), 4u);
  EXPECT_EQ(matches[0].value->GetString(), "Nigel");
  EXPECT_EQ(matches[3].value->GetString(), "Tolkien");
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, RecursiveDescent) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$..price");
  auto matches = EvaluatePath(pr.path, doc);
  EXPECT_EQ(matches.size(), 5u);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, Slice) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$.store.book[0:2]");
  auto matches = EvaluatePath(pr.path, doc);
  EXPECT_EQ(matches.size(), 2u);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, NonexistentKey) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$.missing");
  auto matches = EvaluatePath(pr.path, doc);
  EXPECT_EQ(matches.size(), 0u);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, WildcardOnObject) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$.store.*");
  auto matches = EvaluatePath(pr.path, doc);
  EXPECT_EQ(matches.size(), 2u); // book and bicycle
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, ParentTracking) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$.store.bicycle.color");
  auto matches = EvaluatePath(pr.path, doc);
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_NE(matches[0].parent, nullptr);
  EXPECT_TRUE(matches[0].parent->IsObject());
  EXPECT_EQ(matches[0].object_key, "color");
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, ArrayParentTracking) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$.store.book[0]");
  auto matches = EvaluatePath(pr.path, doc);
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_NE(matches[0].parent, nullptr);
  EXPECT_TRUE(matches[0].parent->IsArray());
  EXPECT_EQ(matches[0].array_index, 0);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, LegacyPathSingleResult) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath(".store.book[0].title");
  auto matches = EvaluatePath(pr.path, doc);
  EXPECT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches[0].value->GetString(), "Sayings");
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, RecursiveWithWildcard) {
  auto* doc = Parse("[1, [2, [3, [4]]]]");
  auto pr = ParsePath("$..[*]");
  auto matches = EvaluatePath(pr.path, doc);
  EXPECT_GE(matches.size(), 4u);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, SliceWithNegativeIndices) {
  auto* doc = Parse("[0,1,2,3,4,5]");
  auto pr = ParsePath("$[-3:-1]");
  auto matches = EvaluatePath(pr.path, doc);
  EXPECT_EQ(matches.size(), 2u);
  EXPECT_EQ(matches[0].value->GetInteger(), 3);
  EXPECT_EQ(matches[1].value->GetInteger(), 4);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, EmptyResult) {
  auto* doc = Parse("{\"a\": 1}");
  auto pr = ParsePath("$.b");
  auto matches = EvaluatePath(pr.path, doc);
  EXPECT_EQ(matches.size(), 0u);
  JsonValue::Destroy(doc);
}
