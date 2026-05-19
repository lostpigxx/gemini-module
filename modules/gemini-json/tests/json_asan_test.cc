#include <gtest/gtest.h>
#include "json_value.h"
#include "json_parse.h"
#include "json_serialize.h"
#include "json_path.h"

#include <string>
#include <vector>

// =============================================================
// Memory leak detection tests
// ASAN will report leaks if any JsonValue is not properly freed.
// =============================================================

TEST(AsanLeakTest, ParseAndDestroyAllTypes) {
  const char* docs[] = {
    "null", "true", "false", "0", "-1", "3.14", "1e100",
    "\"\"", "\"hello\"", "\"esc\\n\\t\\u0041\"",
    "[]", "[1,2,3]", "[[[]]]",
    "{}", "{\"a\":1}", "{\"a\":{\"b\":{\"c\":[1,2,3]}}}",
  };
  for (auto* doc : docs) {
    auto r = JsonParse(doc);
    ASSERT_NE(r.value, nullptr) << "Failed to parse: " << doc;
    JsonValue::Destroy(r.value);
  }
}

TEST(AsanLeakTest, ParseErrorCleansUpPartialTree) {
  const char* bad_docs[] = {
    "[1, 2,",
    "{\"key\": [1, 2, {\"nested\":",
    "{\"a\": 1, \"b\": [true, false, ",
    "[[[[[",
    "{\"a\": {\"b\": {\"c\": {\"d\":",
    "[1, 2, 3, \"unterminated",
    "{\"key\": \"val\", bad}",
  };
  for (auto* doc : bad_docs) {
    auto r = JsonParse(doc);
    EXPECT_EQ(r.value, nullptr) << "Should fail: " << doc;
    // ASAN will catch leaks if partial trees aren't cleaned up
  }
}

TEST(AsanLeakTest, SerializeAndDiscard) {
  auto r = JsonParse(R"({"arr":[1,2,3],"nested":{"x":"hello","y":null}})");
  ASSERT_NE(r.value, nullptr);

  for (int i = 0; i < 100; i++) {
    auto s = JsonSerialize(r.value);
    EXPECT_FALSE(s.empty());
  }

  SerializeOptions opts;
  opts.indent = "  ";
  opts.newline = "\n";
  opts.space = " ";
  for (int i = 0; i < 100; i++) {
    auto s = JsonSerialize(r.value, opts);
    EXPECT_FALSE(s.empty());
  }

  JsonValue::Destroy(r.value);
}

TEST(AsanLeakTest, CloneAndDestroy) {
  auto r = JsonParse(R"({
    "users": [
      {"name": "Alice", "scores": [10, 20, 30], "active": true},
      {"name": "Bob", "scores": [40, 50], "active": false}
    ],
    "meta": {"version": 1, "tags": ["a", "b", "c"]}
  })");
  ASSERT_NE(r.value, nullptr);

  for (int i = 0; i < 100; i++) {
    auto* clone = r.value->Clone();
    ASSERT_NE(clone, nullptr);
    JsonValue::Destroy(clone);
  }

  JsonValue::Destroy(r.value);
}

TEST(AsanLeakTest, ParseRoundTripStress) {
  auto r = JsonParse(R"({"a":[1,{"b":true},null,"str",[[]]]})");
  ASSERT_NE(r.value, nullptr);

  for (int i = 0; i < 50; i++) {
    auto json = JsonSerialize(r.value);
    auto r2 = JsonParse(json);
    ASSERT_NE(r2.value, nullptr);
    EXPECT_TRUE(r.value->DeepEqual(r2.value));
    JsonValue::Destroy(r2.value);
  }

  JsonValue::Destroy(r.value);
}

// =============================================================
// Ownership transfer tests
// Verify no double-free or use-after-free.
// =============================================================

TEST(AsanOwnershipTest, ArrayPopOwnership) {
  auto* arr = JsonValue::CreateArray();
  for (int i = 0; i < 10; i++) {
    arr->ArrayAppend(JsonValue::CreateString(
      std::to_string(i).c_str(), static_cast<uint32_t>(std::to_string(i).size())));
  }

  std::vector<JsonValue*> popped;
  while (arr->ArrayLen() > 0) {
    auto* p = arr->ArrayPop(-1);
    ASSERT_NE(p, nullptr);
    popped.push_back(p);
  }
  EXPECT_EQ(arr->ArrayLen(), 0u);

  for (auto* p : popped) JsonValue::Destroy(p);
  JsonValue::Destroy(arr);
}

TEST(AsanOwnershipTest, ArrayPopFromFront) {
  auto* arr = JsonValue::CreateArray();
  for (int i = 0; i < 20; i++) {
    arr->ArrayAppend(JsonValue::CreateInteger(i));
  }

  for (int i = 0; i < 20; i++) {
    auto* p = arr->ArrayPop(0);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->GetInteger(), i);
    JsonValue::Destroy(p);
  }

  JsonValue::Destroy(arr);
}

TEST(AsanOwnershipTest, ObjectUpsertFreesOldValue) {
  auto* obj = JsonValue::CreateObject();

  for (int i = 0; i < 100; i++) {
    auto* nested = JsonValue::CreateObject();
    nested->ObjectSet("idx", 3, JsonValue::CreateInteger(i));
    nested->ObjectSet("data", 4, JsonValue::CreateString("payload", 7));
    obj->ObjectSet("key", 3, nested);
  }
  EXPECT_EQ(obj->ObjectLen(), 1u);
  EXPECT_EQ(obj->ObjectGet("key")->ObjectGet("idx")->GetInteger(), 99);

  JsonValue::Destroy(obj);
}

TEST(AsanOwnershipTest, ObjectDeleteFreesValue) {
  auto* obj = JsonValue::CreateObject();
  for (int i = 0; i < 50; i++) {
    auto key = std::to_string(i);
    auto* arr = JsonValue::CreateArray();
    for (int j = 0; j < 10; j++) {
      arr->ArrayAppend(JsonValue::CreateInteger(j));
    }
    obj->ObjectSet(key.c_str(), static_cast<uint32_t>(key.size()), arr);
  }
  EXPECT_EQ(obj->ObjectLen(), 50u);

  for (int i = 0; i < 50; i++) {
    auto key = std::to_string(i);
    EXPECT_TRUE(obj->ObjectDelete(key));
  }
  EXPECT_EQ(obj->ObjectLen(), 0u);

  JsonValue::Destroy(obj);
}

TEST(AsanOwnershipTest, ArrayTrimFreesRemoved) {
  auto* arr = JsonValue::CreateArray();
  for (int i = 0; i < 100; i++) {
    auto* obj = JsonValue::CreateObject();
    obj->ObjectSet("i", 1, JsonValue::CreateInteger(i));
    obj->ObjectSet("s", 1, JsonValue::CreateString("hello", 5));
    arr->ArrayAppend(obj);
  }

  arr->ArrayTrim(10, 19);
  EXPECT_EQ(arr->ArrayLen(), 10u);
  EXPECT_EQ(arr->ArrayGet(0)->ObjectGet("i")->GetInteger(), 10);

  JsonValue::Destroy(arr);
}

TEST(AsanOwnershipTest, ClearFreesChildren) {
  auto* obj = JsonValue::CreateObject();
  for (int i = 0; i < 20; i++) {
    auto key = "key" + std::to_string(i);
    auto* arr = JsonValue::CreateArray();
    arr->ArrayAppend(JsonValue::CreateString("data", 4));
    arr->ArrayAppend(JsonValue::CreateObject());
    obj->ObjectSet(key.c_str(), static_cast<uint32_t>(key.size()), arr);
  }

  obj->Clear();
  EXPECT_EQ(obj->ObjectLen(), 0u);

  JsonValue::Destroy(obj);
}

// =============================================================
// Buffer overflow / boundary tests
// =============================================================

TEST(AsanBoundaryTest, VeryLongString) {
  std::string long_str(100000, 'x');
  auto* v = JsonValue::CreateString(long_str.c_str(), static_cast<uint32_t>(long_str.size()));
  EXPECT_EQ(v->GetString().size(), 100000u);
  auto* clone = v->Clone();
  EXPECT_TRUE(v->DeepEqual(clone));
  JsonValue::Destroy(v);
  JsonValue::Destroy(clone);
}

TEST(AsanBoundaryTest, DeeplyNestedParse) {
  std::string json;
  constexpr int depth = 200;
  for (int i = 0; i < depth; i++) json += "{\"n\":";
  json += "null";
  for (int i = 0; i < depth; i++) json += "}";

  auto r = JsonParse(json);
  ASSERT_NE(r.value, nullptr);

  auto s = JsonSerialize(r.value);
  auto r2 = JsonParse(s);
  ASSERT_NE(r2.value, nullptr);
  EXPECT_TRUE(r.value->DeepEqual(r2.value));

  JsonValue::Destroy(r.value);
  JsonValue::Destroy(r2.value);
}

TEST(AsanBoundaryTest, DeeplyNestedArrayParse) {
  std::string json;
  constexpr int depth = 200;
  for (int i = 0; i < depth; i++) json += "[";
  json += "1";
  for (int i = 0; i < depth; i++) json += "]";

  auto r = JsonParse(json);
  ASSERT_NE(r.value, nullptr);
  JsonValue::Destroy(r.value);
}

TEST(AsanBoundaryTest, LargeArrayGrowth) {
  auto* arr = JsonValue::CreateArray();
  for (int i = 0; i < 10000; i++) {
    arr->ArrayAppend(JsonValue::CreateInteger(i));
  }
  EXPECT_EQ(arr->ArrayLen(), 10000u);

  for (int i = 0; i < 5000; i++) {
    auto* p = arr->ArrayPop(-1);
    JsonValue::Destroy(p);
  }
  EXPECT_EQ(arr->ArrayLen(), 5000u);

  for (int i = 0; i < 5000; i++) {
    arr->ArrayAppend(JsonValue::CreateString("refill", 6));
  }
  EXPECT_EQ(arr->ArrayLen(), 10000u);

  JsonValue::Destroy(arr);
}

TEST(AsanBoundaryTest, LargeObjectGrowth) {
  auto* obj = JsonValue::CreateObject();
  for (int i = 0; i < 5000; i++) {
    auto key = "k" + std::to_string(i);
    obj->ObjectSet(key.c_str(), static_cast<uint32_t>(key.size()),
                   JsonValue::CreateInteger(i));
  }
  EXPECT_EQ(obj->ObjectLen(), 5000u);
  JsonValue::Destroy(obj);
}

TEST(AsanBoundaryTest, ParseStringWithAllEscapes) {
  auto r = JsonParse(R"("tab\tnl\ncr\rbs\bff\fquote\"slash\\fwdslash\/end")");
  ASSERT_NE(r.value, nullptr);
  auto s = JsonSerialize(r.value);
  auto r2 = JsonParse(s);
  ASSERT_NE(r2.value, nullptr);
  JsonValue::Destroy(r.value);
  JsonValue::Destroy(r2.value);
}

// =============================================================
// JSONPath evaluation + mutation memory safety
// =============================================================

TEST(AsanPathTest, EvaluateAndMutateObject) {
  auto r = JsonParse(R"({"a":{"x":1},"b":{"x":2},"c":{"x":3}})");
  ASSERT_NE(r.value, nullptr);

  auto pr = ParsePath("$.*.x");
  auto matches = EvaluatePath(pr.path, r.value);
  EXPECT_EQ(matches.size(), 3u);

  // Replace matched values with new objects
  for (auto& m : matches) {
    auto* replacement = JsonValue::CreateObject();
    replacement->ObjectSet("replaced", 8, JsonValue::CreateBool(true));
    if (m.parent && m.parent->IsObject() && !m.object_key.empty()) {
      m.parent->ObjectSet(m.object_key.c_str(),
                          static_cast<uint32_t>(m.object_key.size()), replacement);
    } else {
      JsonValue::Destroy(replacement);
    }
  }

  JsonValue::Destroy(r.value);
}

TEST(AsanPathTest, RecursiveDescentLargeTree) {
  auto* root = JsonValue::CreateObject();
  auto* current = root;
  for (int i = 0; i < 50; i++) {
    auto* child = JsonValue::CreateObject();
    child->ObjectSet("val", 3, JsonValue::CreateInteger(i));
    current->ObjectSet("child", 5, child);
    current = child;
  }

  auto pr = ParsePath("$..val");
  auto matches = EvaluatePath(pr.path, root);
  EXPECT_EQ(matches.size(), 50u);

  JsonValue::Destroy(root);
}

TEST(AsanPathTest, WildcardOnLargeArray) {
  auto* arr = JsonValue::CreateArray();
  for (int i = 0; i < 1000; i++) {
    auto* obj = JsonValue::CreateObject();
    obj->ObjectSet("id", 2, JsonValue::CreateInteger(i));
    arr->ArrayAppend(obj);
  }

  auto pr = ParsePath("$[*].id");
  auto matches = EvaluatePath(pr.path, arr);
  EXPECT_EQ(matches.size(), 1000u);

  JsonValue::Destroy(arr);
}

TEST(AsanPathTest, SliceEvaluation) {
  auto* arr = JsonValue::CreateArray();
  for (int i = 0; i < 100; i++) {
    arr->ArrayAppend(JsonValue::CreateInteger(i));
  }

  auto pr1 = ParsePath("$[10:20]");
  auto m1 = EvaluatePath(pr1.path, arr);
  EXPECT_EQ(m1.size(), 10u);

  auto pr2 = ParsePath("$[-5:]");
  auto m2 = EvaluatePath(pr2.path, arr);
  EXPECT_EQ(m2.size(), 5u);

  auto pr3 = ParsePath("$[0:50:5]");
  auto m3 = EvaluatePath(pr3.path, arr);
  EXPECT_EQ(m3.size(), 10u);

  JsonValue::Destroy(arr);
}

TEST(AsanPathTest, DeleteMultipleFromArray) {
  auto r = JsonParse("[0,1,2,3,4,5,6,7,8,9]");
  ASSERT_NE(r.value, nullptr);

  auto pr = ParsePath("$[*]");
  auto matches = EvaluatePath(pr.path, r.value);
  EXPECT_EQ(matches.size(), 10u);

  // Delete even-indexed elements in reverse order
  for (int i = 8; i >= 0; i -= 2) {
    auto* popped = r.value->ArrayPop(i);
    JsonValue::Destroy(popped);
  }
  EXPECT_EQ(r.value->ArrayLen(), 5u);

  JsonValue::Destroy(r.value);
}

// =============================================================
// Stress test: repeated create/destroy cycles
// =============================================================

TEST(AsanStressTest, RepeatedCreateDestroy) {
  for (int i = 0; i < 1000; i++) {
    auto* obj = JsonValue::CreateObject();
    obj->ObjectSet("str", 3, JsonValue::CreateString("value", 5));
    obj->ObjectSet("arr", 3, JsonValue::CreateArray());
    obj->ObjectGet("arr")->ArrayAppend(JsonValue::CreateInteger(i));
    obj->ObjectGet("arr")->ArrayAppend(JsonValue::CreateNull());
    obj->ObjectGet("arr")->ArrayAppend(JsonValue::CreateBool(true));
    JsonValue::Destroy(obj);
  }
}

TEST(AsanStressTest, RepeatedParseDestroy) {
  const char* json = R"({"users":[{"name":"A","scores":[1,2,3]},{"name":"B","scores":[4,5]}]})";
  for (int i = 0; i < 1000; i++) {
    auto r = JsonParse(json);
    ASSERT_NE(r.value, nullptr);
    JsonValue::Destroy(r.value);
  }
}

TEST(AsanStressTest, RepeatedCloneAndMutate) {
  auto r = JsonParse(R"({"a":[1,2,3],"b":{"c":"d"}})");
  ASSERT_NE(r.value, nullptr);

  for (int i = 0; i < 500; i++) {
    auto* clone = r.value->Clone();
    clone->ObjectGet("a")->ArrayAppend(JsonValue::CreateInteger(i));
    clone->ObjectGet("b")->ObjectSet("new", 3, JsonValue::CreateBool(true));
    JsonValue::Destroy(clone);
  }

  JsonValue::Destroy(r.value);
}

TEST(AsanStressTest, ArrayInsertRemoveCycle) {
  auto* arr = JsonValue::CreateArray();

  for (int round = 0; round < 100; round++) {
    for (int i = 0; i < 20; i++) {
      arr->ArrayInsert(0, JsonValue::CreateInteger(round * 20 + i));
    }
    for (int i = 0; i < 10; i++) {
      auto* p = arr->ArrayPop(0);
      JsonValue::Destroy(p);
    }
  }

  JsonValue::Destroy(arr);
}

TEST(AsanStressTest, PathEvalOnChangingDocument) {
  auto r = JsonParse(R"({"items":[{"v":1},{"v":2},{"v":3}]})");
  ASSERT_NE(r.value, nullptr);

  for (int i = 0; i < 100; i++) {
    auto pr = ParsePath("$.items[*].v");
    auto matches = EvaluatePath(pr.path, r.value);

    auto* items = r.value->ObjectGet("items");
    auto* new_item = JsonValue::CreateObject();
    new_item->ObjectSet("v", 1, JsonValue::CreateInteger(100 + i));
    items->ArrayAppend(new_item);
  }

  EXPECT_EQ(r.value->ObjectGet("items")->ArrayLen(), 103u);
  JsonValue::Destroy(r.value);
}

// =============================================================
// Merge (RFC 7396) memory safety
// =============================================================

TEST(AsanMergeTest, MergePatchMemorySafety) {
  auto r = JsonParse(R"({"a":1,"b":{"c":2,"d":3},"e":[1,2]})");
  ASSERT_NE(r.value, nullptr);

  // Merge that deletes, overwrites, and adds
  auto patch = JsonParse(R"({"a":null,"b":{"c":99,"new_key":"hello"},"f":true})");
  ASSERT_NE(patch.value, nullptr);

  // Manual merge simulation (RFC 7396)
  auto* target = r.value;
  auto* p = patch.value;
  auto* p_entries = p->ObjectEntries();
  for (uint32_t i = 0; i < p->ObjectLen(); i++) {
    auto pk = std::string_view(p_entries[i].key, p_entries[i].key_len);
    if (p_entries[i].value->IsNull()) {
      target->ObjectDelete(pk);
    } else if (p_entries[i].value->IsObject()) {
      auto* existing = target->ObjectGet(pk);
      if (existing && existing->IsObject()) {
        auto* sub_entries = p_entries[i].value->ObjectEntries();
        for (uint32_t j = 0; j < p_entries[i].value->ObjectLen(); j++) {
          existing->ObjectSet(sub_entries[j].key, sub_entries[j].key_len,
                              sub_entries[j].value->Clone());
        }
      } else {
        target->ObjectSet(p_entries[i].key, p_entries[i].key_len,
                          p_entries[i].value->Clone());
      }
    } else {
      target->ObjectSet(p_entries[i].key, p_entries[i].key_len,
                        p_entries[i].value->Clone());
    }
  }

  EXPECT_EQ(target->ObjectGet("a"), nullptr);
  EXPECT_EQ(target->ObjectGet("b")->ObjectGet("c")->GetInteger(), 99);
  EXPECT_TRUE(target->ObjectGet("f")->GetBool());

  JsonValue::Destroy(r.value);
  JsonValue::Destroy(patch.value);
}
