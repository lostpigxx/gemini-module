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

// --- Union Parsing ---

TEST(PathParseTest, UnionIndices) {
  auto r = ParsePath("$.arr[0,2]");
  EXPECT_EQ(r.error, nullptr);
  auto& seg = r.path.segments.back();
  EXPECT_EQ(seg.type, PathSegType::kUnion);
  ASSERT_EQ(seg.union_params.members.size(), 2u);
  EXPECT_TRUE(seg.union_params.members[0].is_index);
  EXPECT_EQ(seg.union_params.members[0].index, 0);
  EXPECT_TRUE(seg.union_params.members[1].is_index);
  EXPECT_EQ(seg.union_params.members[1].index, 2);
}

TEST(PathParseTest, UnionKeys) {
  auto r = ParsePath("$['name','age']");
  EXPECT_EQ(r.error, nullptr);
  auto& seg = r.path.segments.back();
  EXPECT_EQ(seg.type, PathSegType::kUnion);
  ASSERT_EQ(seg.union_params.members.size(), 2u);
  EXPECT_FALSE(seg.union_params.members[0].is_index);
  EXPECT_EQ(seg.union_params.members[0].key, "name");
  EXPECT_EQ(seg.union_params.members[1].key, "age");
}

TEST(PathParseTest, UnionMultipleIndices) {
  auto r = ParsePath("$[0,1,3]");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_EQ(r.path.segments.back().union_params.members.size(), 3u);
}

TEST(PathParseTest, UnionNegativeIndex) {
  auto r = ParsePath("$[0,-1]");
  EXPECT_EQ(r.error, nullptr);
  auto& members = r.path.segments.back().union_params.members;
  ASSERT_EQ(members.size(), 2u);
  EXPECT_EQ(members[1].index, -1);
}

// --- Union Evaluation ---

TEST(PathEvalTest, UnionIndices) {
  auto* doc = Parse("[10,20,30,40,50]");
  auto pr = ParsePath("$[0,2,4]");
  auto matches = EvaluatePath(pr.path, doc);
  ASSERT_EQ(matches.size(), 3u);
  EXPECT_EQ(matches[0].value->GetInteger(), 10);
  EXPECT_EQ(matches[1].value->GetInteger(), 30);
  EXPECT_EQ(matches[2].value->GetInteger(), 50);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, UnionKeys) {
  auto* doc = Parse(R"({"a":1,"b":2,"c":3})");
  auto pr = ParsePath("$['a','c']");
  auto matches = EvaluatePath(pr.path, doc);
  ASSERT_EQ(matches.size(), 2u);
  EXPECT_EQ(matches[0].value->GetInteger(), 1);
  EXPECT_EQ(matches[1].value->GetInteger(), 3);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, UnionNegativeIndex) {
  auto* doc = Parse("[10,20,30]");
  auto pr = ParsePath("$[0,-1]");
  auto matches = EvaluatePath(pr.path, doc);
  ASSERT_EQ(matches.size(), 2u);
  EXPECT_EQ(matches[0].value->GetInteger(), 10);
  EXPECT_EQ(matches[1].value->GetInteger(), 30);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, UnionMissingKey) {
  auto* doc = Parse(R"({"a":1,"b":2})");
  auto pr = ParsePath("$['a','z']");
  auto matches = EvaluatePath(pr.path, doc);
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches[0].value->GetInteger(), 1);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, UnionInChain) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$.store.book[0,2].title");
  auto matches = EvaluatePath(pr.path, doc);
  ASSERT_EQ(matches.size(), 2u);
  EXPECT_EQ(matches[0].value->GetString(), "Sayings");
  EXPECT_EQ(matches[1].value->GetString(), "Moby");
  JsonValue::Destroy(doc);
}

// --- Filter Parsing ---

TEST(PathParseTest, FilterExistence) {
  auto r = ParsePath("$.store.book[?(@.isbn)]");
  EXPECT_EQ(r.error, nullptr);
  auto& seg = r.path.segments.back();
  EXPECT_EQ(seg.type, PathSegType::kFilter);
  ASSERT_EQ(seg.filter.field_path.size(), 1u);
  EXPECT_EQ(seg.filter.field_path[0], "isbn");
  EXPECT_EQ(seg.filter.op, FilterOp::kExists);
}

TEST(PathParseTest, FilterEquality) {
  auto r = ParsePath("$.store.book[?(@.category == 'fiction')]");
  EXPECT_EQ(r.error, nullptr);
  auto& seg = r.path.segments.back();
  EXPECT_EQ(seg.type, PathSegType::kFilter);
  EXPECT_EQ(seg.filter.op, FilterOp::kEq);
  EXPECT_EQ(seg.filter.value.kind, FilterValue::Kind::kString);
  EXPECT_EQ(seg.filter.value.str_val, "fiction");
}

TEST(PathParseTest, FilterGreaterThan) {
  auto r = ParsePath("$.store.book[?(@.price > 10)]");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_EQ(r.path.segments.back().filter.op, FilterOp::kGt);
  EXPECT_EQ(r.path.segments.back().filter.value.kind, FilterValue::Kind::kInteger);
  EXPECT_EQ(r.path.segments.back().filter.value.int_val, 10);
}

TEST(PathParseTest, FilterLessThanEqual) {
  auto r = ParsePath("$.store.book[?(@.price <= 8.99)]");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_EQ(r.path.segments.back().filter.op, FilterOp::kLe);
  EXPECT_EQ(r.path.segments.back().filter.value.kind, FilterValue::Kind::kNumber);
  EXPECT_DOUBLE_EQ(r.path.segments.back().filter.value.num_val, 8.99);
}

TEST(PathParseTest, FilterNotEqual) {
  auto r = ParsePath("$.arr[?(@.x != 5)]");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_EQ(r.path.segments.back().filter.op, FilterOp::kNe);
}

TEST(PathParseTest, FilterNestedField) {
  auto r = ParsePath("$[?(@.a.b == 1)]");
  EXPECT_EQ(r.error, nullptr);
  auto& fp = r.path.segments.back().filter.field_path;
  ASSERT_EQ(fp.size(), 2u);
  EXPECT_EQ(fp[0], "a");
  EXPECT_EQ(fp[1], "b");
}

TEST(PathParseTest, FilterBoolValue) {
  auto r = ParsePath("$[?(@.active == true)]");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_EQ(r.path.segments.back().filter.value.kind, FilterValue::Kind::kBool);
  EXPECT_TRUE(r.path.segments.back().filter.value.bool_val);
}

TEST(PathParseTest, FilterNullValue) {
  auto r = ParsePath("$[?(@.x == null)]");
  EXPECT_EQ(r.error, nullptr);
  EXPECT_EQ(r.path.segments.back().filter.value.kind, FilterValue::Kind::kNull);
}

// --- Filter Evaluation ---

TEST(PathEvalTest, FilterExistence) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$.store.book[?(@.category)]");
  auto matches = EvaluatePath(pr.path, doc);
  EXPECT_EQ(matches.size(), 4u);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, FilterEqString) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$.store.book[?(@.category == 'fiction')]");
  auto matches = EvaluatePath(pr.path, doc);
  EXPECT_EQ(matches.size(), 3u);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, FilterGtNumber) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$.store.book[?(@.price > 10)]");
  auto matches = EvaluatePath(pr.path, doc);
  EXPECT_EQ(matches.size(), 2u);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, FilterLeNumber) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$.store.book[?(@.price <= 8.99)]");
  auto matches = EvaluatePath(pr.path, doc);
  EXPECT_EQ(matches.size(), 2u);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, FilterNoMatch) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$.store.book[?(@.price > 100)]");
  auto matches = EvaluatePath(pr.path, doc);
  EXPECT_EQ(matches.size(), 0u);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, FilterWithRecursive) {
  auto* doc = MakeTestDoc();
  auto pr = ParsePath("$..book[?(@.price < 10)]");
  auto matches = EvaluatePath(pr.path, doc);
  EXPECT_EQ(matches.size(), 2u);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, FilterNestedField) {
  auto* doc = Parse(R"([{"a":{"b":1}},{"a":{"b":2}},{"a":{"b":3}}])");
  auto pr = ParsePath("$[?(@.a.b == 2)]");
  auto matches = EvaluatePath(pr.path, doc);
  ASSERT_EQ(matches.size(), 1u);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, FilterBoolValue) {
  auto* doc = Parse(R"([{"active":true},{"active":false},{"active":true}])");
  auto pr = ParsePath("$[?(@.active == true)]");
  auto matches = EvaluatePath(pr.path, doc);
  EXPECT_EQ(matches.size(), 2u);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, FilterExistencePartial) {
  auto* doc = Parse(R"([{"name":"a","stock":5},{"name":"b"},{"name":"c","stock":0}])");
  auto pr = ParsePath("$[?(@.stock)]");
  auto matches = EvaluatePath(pr.path, doc);
  EXPECT_EQ(matches.size(), 2u);
  JsonValue::Destroy(doc);
}

TEST(PathParseTest, FilterNumberNonNullTerminatedBuffer) {
  // Buffer has trailing digits outside string_view; ParseDouble must not read past end.
  std::string raw = "$[?(@.x == 12)]99999999";
  std::string_view sv(raw.data(), 15);  // "$[?(@.x == 12)]"
  auto r = ParsePath(sv);
  ASSERT_EQ(r.error, nullptr);
  EXPECT_EQ(r.path.segments.back().filter.value.kind, FilterValue::Kind::kInteger);
  EXPECT_EQ(r.path.segments.back().filter.value.int_val, 12);
}

TEST(PathEvalTest, EmptyObjectKeyMatch) {
  auto* doc = Parse(R"({"": 42, "a": 1})");
  auto pr = ParsePath(R"($[""])");
  ASSERT_EQ(pr.error, nullptr);
  auto matches = EvaluatePath(pr.path, doc);
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_TRUE(matches[0].has_object_key);
  EXPECT_EQ(matches[0].object_key, "");
  EXPECT_EQ(matches[0].value->GetInteger(), 42);
  JsonValue::Destroy(doc);
}

// --- Phase 4: Rich Filters, Quoted Punctuation, Mixed Union, Slice, Cache ---

TEST(PathEvalTest, QuotedKeyWithCommaAndColon) {
  auto* doc = Parse(R"({"a,b": 1, "c:d": 2, "e\nf": 3})");
  auto r1 = ParsePath(R"($["a,b"])");
  ASSERT_EQ(r1.error, nullptr);
  auto m1 = EvaluatePath(r1.path, doc);
  ASSERT_EQ(m1.size(), 1u);
  EXPECT_EQ(m1[0].value->GetInteger(), 1);

  auto r2 = ParsePath(R"($["c:d"])");
  ASSERT_EQ(r2.error, nullptr);
  auto m2 = EvaluatePath(r2.path, doc);
  ASSERT_EQ(m2.size(), 1u);
  EXPECT_EQ(m2[0].value->GetInteger(), 2);

  auto r3 = ParsePath(R"($["e\nf"])");
  ASSERT_EQ(r3.error, nullptr);
  auto m3 = EvaluatePath(r3.path, doc);
  ASSERT_EQ(m3.size(), 1u);
  EXPECT_EQ(m3[0].value->GetInteger(), 3);

  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, SliceStepZeroAndNegativeStep) {
  auto* doc = Parse("[0,1,2,3,4]");
  auto r0 = ParsePath("$[0:5:0]");
  ASSERT_EQ(r0.error, nullptr);
  EXPECT_EQ(EvaluatePath(r0.path, doc).size(), 0u);

  auto rrev = ParsePath("$[4:0:-2]");
  ASSERT_EQ(rrev.error, nullptr);
  auto m = EvaluatePath(rrev.path, doc);
  ASSERT_EQ(m.size(), 2u);
  EXPECT_EQ(m[0].value->GetInteger(), 4);
  EXPECT_EQ(m[1].value->GetInteger(), 2);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, MixedUnionAndWhitespace) {
  auto* doc = Parse("[10, 20, 30, 40, 50]");
  auto r = ParsePath("$[ 0 , 2:4 , -1 ]");
  ASSERT_EQ(r.error, nullptr);
  auto m = EvaluatePath(r.path, doc);
  ASSERT_EQ(m.size(), 4u);
  EXPECT_EQ(m[0].value->GetInteger(), 10);
  EXPECT_EQ(m[1].value->GetInteger(), 30);
  EXPECT_EQ(m[2].value->GetInteger(), 40);
  EXPECT_EQ(m[3].value->GetInteger(), 50);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, FilterCompoundBooleanAndNot) {
  auto* doc = MakeTestDoc();
  auto r1 = ParsePath("$.store.book[?(@.category == 'fiction' && @.price < 15)]");
  ASSERT_EQ(r1.error, nullptr);
  auto m1 = EvaluatePath(r1.path, doc);
  EXPECT_EQ(m1.size(), 2u);

  auto r2 = ParsePath("$.store.book[?(@.category == 'reference' || @.price > 20)]");
  ASSERT_EQ(r2.error, nullptr);
  auto m2 = EvaluatePath(r2.path, doc);
  EXPECT_EQ(m2.size(), 2u);

  auto r3 = ParsePath("$.store.book[?(!(@.category == 'fiction'))]");
  ASSERT_EQ(r3.error, nullptr);
  auto m3 = EvaluatePath(r3.path, doc);
  ASSERT_EQ(m3.size(), 1u);
  EXPECT_EQ(m3[0].value->ObjectGet("title")->GetString(), "Sayings");

  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, FilterSelfScalarAndNestedIndexAndRootRef) {
  auto* arr = Parse("[1, 5, 10, 15, 20]");
  auto r1 = ParsePath("$[?(@ > 5 && @ <= 15)]");
  ASSERT_EQ(r1.error, nullptr);
  auto m1 = EvaluatePath(r1.path, arr);
  ASSERT_EQ(m1.size(), 2u);
  EXPECT_EQ(m1[0].value->GetInteger(), 10);
  EXPECT_EQ(m1[1].value->GetInteger(), 15);
  JsonValue::Destroy(arr);

  auto* doc = Parse(R"({"limit": 12, "items": [{"a": 5, "b": 10, "tags": ["hot"]}, {"a": 15, "b": 10, "tags": ["cold"]}]})");
  auto r2 = ParsePath("$.items[?(@.a < @.b && @.tags[0] == 'hot')]");
  ASSERT_EQ(r2.error, nullptr);
  auto m2 = EvaluatePath(r2.path, doc);
  ASSERT_EQ(m2.size(), 1u);
  EXPECT_EQ(m2[0].value->ObjectGet("a")->GetInteger(), 5);

  auto r3 = ParsePath("$.items[?(@.a > $.limit)]");
  ASSERT_EQ(r3.error, nullptr);
  auto m3 = EvaluatePath(r3.path, doc);
  ASSERT_EQ(m3.size(), 1u);
  EXPECT_EQ(m3[0].value->ObjectGet("a")->GetInteger(), 15);
  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, FilterRegexAndLargeIntPrecision) {
  auto* doc = MakeTestDoc();
  auto r1 = ParsePath("$.store.book[?(@.author =~ /^tolk/i)]");
  ASSERT_EQ(r1.error, nullptr);
  auto m1 = EvaluatePath(r1.path, doc);
  ASSERT_EQ(m1.size(), 1u);
  EXPECT_EQ(m1[0].value->ObjectGet("author")->GetString(), "Tolkien");

  auto r2 = ParsePath("$.store.book[?(@.title =~ '(?i)moby')]");
  ASSERT_EQ(r2.error, nullptr);
  EXPECT_EQ(EvaluatePath(r2.path, doc).size(), 1u);

  auto rbad = ParsePath("$.store.book[?(@.title =~ /[invalid/)]");
  EXPECT_NE(rbad.error, nullptr);
  JsonValue::Destroy(doc);

  auto* bigs = Parse("[9007199254740992, 9007199254740993]");
  auto rbig = ParsePath("$[?(@ == 9007199254740993)]");
  ASSERT_EQ(rbig.error, nullptr);
  auto mbig = EvaluatePath(rbig.path, bigs);
  ASSERT_EQ(mbig.size(), 1u);
  EXPECT_EQ(mbig[0].value->GetInteger(), 9007199254740993LL);
  JsonValue::Destroy(bigs);
}

TEST(PathEvalTest, MultiRecursiveDedupAndLruCache) {
  auto* doc = Parse(R"({"a":{"a":{"b":1},"b":2}})");
  auto r = ParsePath("$..a..b");
  ASSERT_EQ(r.error, nullptr);
  auto m = EvaluatePath(r.path, doc);
  EXPECT_EQ(m.size(), 2u);
  JsonValue::Destroy(doc);

  // Exercise LRU cache beyond 128 entries
  for (int i = 0; i < 200; i++) {
    std::string p = "$.k_" + std::to_string(i);
    const char* err = nullptr;
    const JsonPath* cached = ParsePathCached(p, &err);
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(err, nullptr);
    EXPECT_TRUE(cached->is_simple);
  }
}

TEST(PathEvalTest, FilterRegexPrecompiledLargeArray) {
  auto* arr = JsonValue::CreateArray();
  for (int i = 0; i < 1000; i++) {
    auto* obj = JsonValue::CreateObject();
    std::string name = (i % 10 == 0) ? ("TargetItem_" + std::to_string(i))
                                     : ("other_" + std::to_string(i));
    obj->ObjectSet("name", 4, JsonValue::CreateString(name.data(), static_cast<uint32_t>(name.size())));
    arr->ArrayAppend(obj);
  }

  const char* err = nullptr;
  const JsonPath* p = ParsePathCached("$[?(@.name =~ /^targetitem_[0-9]+$/i)]", &err);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(err, nullptr);

  auto m1 = EvaluatePath(*p, arr);
  auto m2 = EvaluatePath(*p, arr);
  EXPECT_EQ(m1.size(), 100u);
  EXPECT_EQ(m2.size(), 100u);

  JsonValue::Destroy(arr);
}

TEST(PathEvalTest, Rfc9535FilterFunctionsLengthCountMatchSearch) {
  auto* doc = Parse(R"([
    {"name": "世界", "tags": ["a", "b", "c"], "meta": {"x": 1, "y": 2}},
    {"name": "hello redis", "tags": ["a"], "meta": {}},
    {"name": "redis", "tags": [], "meta": {"x": 1}}
  ])");
  ASSERT_NE(doc, nullptr);

  // length() on UTF-8 string (2 Unicode scalars), array, object
  auto r_len_str = ParsePath("$[?(length(@.name) == 2)]");
  ASSERT_EQ(r_len_str.error, nullptr);
  EXPECT_EQ(EvaluatePath(r_len_str.path, doc).size(), 1u);

  auto r_len_arr = ParsePath("$[?(length(@.tags) >= 2)]");
  ASSERT_EQ(r_len_arr.error, nullptr);
  EXPECT_EQ(EvaluatePath(r_len_arr.path, doc).size(), 1u);

  auto r_len_obj = ParsePath("$[?(length(@.meta) == 0)]");
  ASSERT_EQ(r_len_obj.error, nullptr);
  EXPECT_EQ(EvaluatePath(r_len_obj.path, doc).size(), 1u);

  // count() with wildcard
  auto r_cnt = ParsePath("$[?(count(@.tags[*]) == 3)]");
  ASSERT_EQ(r_cnt.error, nullptr);
  EXPECT_EQ(EvaluatePath(r_cnt.path, doc).size(), 1u);

  // match() full string vs search() substring
  auto r_match = ParsePath("$[?(match(@.name, 'redis'))]");
  ASSERT_EQ(r_match.error, nullptr);
  EXPECT_EQ(EvaluatePath(r_match.path, doc).size(), 1u);

  auto r_search = ParsePath("$[?(search(@.name, 'redis'))]");
  ASSERT_EQ(r_search.error, nullptr);
  EXPECT_EQ(EvaluatePath(r_search.path, doc).size(), 2u);

  // Combined with && and !
  auto r_combo = ParsePath("$[?(search(@.name, '(?i)REDIS') && !match(@.name, 'redis'))]");
  ASSERT_EQ(r_combo.error, nullptr);
  auto mc = EvaluatePath(r_combo.path, doc);
  ASSERT_EQ(mc.size(), 1u);
  EXPECT_EQ(mc[0].value->ObjectGet("name")->GetString(), "hello redis");

  // Unknown function / invalid args rejected
  EXPECT_NE(ParsePath("$[?(unknown_fn(@.a) == 1)]").error, nullptr);
  EXPECT_NE(ParsePath("$[?(count(123) == 1)]").error, nullptr);
  EXPECT_NE(ParsePath("$[?(match(@.name, 123))]").error, nullptr);

  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, FilterGroupedOrBareAndUnterminatedRegex) {
  auto* doc = Parse(R"([{"a":1,"b":0},{"a":0,"b":2},{"a":0,"b":0}])");
  ASSERT_NE(doc, nullptr);

  // Grouped OR without outer parens (RFC 9535)
  auto r1 = ParsePath("$[?(@.a == 1) || (@.b == 2)]");
  ASSERT_EQ(r1.error, nullptr);
  EXPECT_EQ(EvaluatePath(r1.path, doc).size(), 2u);

  // Bare filter without outer parens
  auto r2 = ParsePath("$[?@.a == 1]");
  ASSERT_EQ(r2.error, nullptr);
  EXPECT_EQ(EvaluatePath(r2.path, doc).size(), 1u);

  // Double negation and missing field comparison
  auto r3 = ParsePath("$[?(!!(@.a == 1))]");
  ASSERT_EQ(r3.error, nullptr);
  EXPECT_EQ(EvaluatePath(r3.path, doc).size(), 1u);

  auto r4 = ParsePath("$[?(@.missing != 1)]");
  ASSERT_EQ(r4.error, nullptr);
  EXPECT_EQ(EvaluatePath(r4.path, doc).size(), 0u);

  // Unterminated regex literal must be rejected
  auto rbad = ParsePath("$[?(@.a =~ /unterminated)]");
  EXPECT_NE(rbad.error, nullptr);

  JsonValue::Destroy(doc);
}

TEST(PathEvalTest, MatchDepthTrackingSimpleAndRecursive) {
  auto* doc = Parse(R"({"a":{"b":[{"c":42}]}})");
  ASSERT_NE(doc, nullptr);

  auto rs = ParsePath("$.a.b[0].c");
  ASSERT_EQ(rs.error, nullptr);
  auto ms = EvaluatePath(rs.path, doc);
  ASSERT_EQ(ms.size(), 1u);
  EXPECT_EQ(ms[0].depth, 4);

  auto rr = ParsePath("$..c");
  ASSERT_EQ(rr.error, nullptr);
  auto mr = EvaluatePath(rr.path, doc);
  ASSERT_EQ(mr.size(), 1u);
  EXPECT_EQ(mr[0].depth, 4);

  JsonValue::Destroy(doc);
}
