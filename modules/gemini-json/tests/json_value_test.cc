#include <gtest/gtest.h>
#include "json_value.h"

TEST(JsonValueTest, CreateNull) {
  auto* v = JsonValue::CreateNull();
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(v->IsNull());
  EXPECT_EQ(v->Type(), JsonType::kNull);
  EXPECT_STREQ(JsonTypeName(v->Type()), "null");
  JsonValue::Destroy(v);
}

TEST(JsonValueTest, CreateBool) {
  auto* t = JsonValue::CreateBool(true);
  auto* f = JsonValue::CreateBool(false);
  EXPECT_TRUE(t->IsBool());
  EXPECT_TRUE(t->GetBool());
  EXPECT_FALSE(f->GetBool());
  EXPECT_STREQ(JsonTypeName(t->Type()), "boolean");
  JsonValue::Destroy(t);
  JsonValue::Destroy(f);
}

TEST(JsonValueTest, CreateInteger) {
  auto* v = JsonValue::CreateInteger(42);
  EXPECT_TRUE(v->IsInteger());
  EXPECT_TRUE(v->IsNumber());
  EXPECT_EQ(v->GetInteger(), 42);
  EXPECT_DOUBLE_EQ(v->GetNumber(), 42.0);
  EXPECT_STREQ(JsonTypeName(v->Type()), "integer");
  JsonValue::Destroy(v);
}

TEST(JsonValueTest, CreateNumber) {
  auto* v = JsonValue::CreateNumber(3.14);
  EXPECT_FALSE(v->IsInteger());
  EXPECT_TRUE(v->IsNumber());
  EXPECT_DOUBLE_EQ(v->GetNumber(), 3.14);
  EXPECT_STREQ(JsonTypeName(v->Type()), "number");
  JsonValue::Destroy(v);
}

TEST(JsonValueTest, CreateString) {
  auto* v = JsonValue::CreateString("hello", 5);
  EXPECT_TRUE(v->IsString());
  EXPECT_EQ(v->GetString(), "hello");
  EXPECT_STREQ(JsonTypeName(v->Type()), "string");
  JsonValue::Destroy(v);
}

TEST(JsonValueTest, CreateEmptyString) {
  auto* v = JsonValue::CreateString("", 0);
  EXPECT_TRUE(v->IsString());
  EXPECT_EQ(v->GetString(), "");
  EXPECT_EQ(v->GetString().size(), 0u);
  JsonValue::Destroy(v);
}

TEST(JsonValueTest, ArrayBasic) {
  auto* arr = JsonValue::CreateArray();
  EXPECT_TRUE(arr->IsArray());
  EXPECT_EQ(arr->ArrayLen(), 0u);

  EXPECT_TRUE(arr->ArrayAppend(JsonValue::CreateInteger(1)));
  EXPECT_TRUE(arr->ArrayAppend(JsonValue::CreateInteger(2)));
  EXPECT_TRUE(arr->ArrayAppend(JsonValue::CreateInteger(3)));
  EXPECT_EQ(arr->ArrayLen(), 3u);
  EXPECT_EQ(arr->ArrayGet(0)->GetInteger(), 1);
  EXPECT_EQ(arr->ArrayGet(1)->GetInteger(), 2);
  EXPECT_EQ(arr->ArrayGet(2)->GetInteger(), 3);

  JsonValue::Destroy(arr);
}

TEST(JsonValueTest, ArrayInsert) {
  auto* arr = JsonValue::CreateArray();
  arr->ArrayAppend(JsonValue::CreateInteger(1));
  arr->ArrayAppend(JsonValue::CreateInteger(3));

  EXPECT_TRUE(arr->ArrayInsert(1, JsonValue::CreateInteger(2)));
  EXPECT_EQ(arr->ArrayLen(), 3u);
  EXPECT_EQ(arr->ArrayGet(0)->GetInteger(), 1);
  EXPECT_EQ(arr->ArrayGet(1)->GetInteger(), 2);
  EXPECT_EQ(arr->ArrayGet(2)->GetInteger(), 3);

  EXPECT_TRUE(arr->ArrayInsert(0, JsonValue::CreateInteger(0)));
  EXPECT_EQ(arr->ArrayGet(0)->GetInteger(), 0);

  EXPECT_TRUE(arr->ArrayInsert(4, JsonValue::CreateInteger(4)));
  EXPECT_EQ(arr->ArrayGet(4)->GetInteger(), 4);

  JsonValue::Destroy(arr);
}

TEST(JsonValueTest, ArrayPop) {
  auto* arr = JsonValue::CreateArray();
  arr->ArrayAppend(JsonValue::CreateInteger(10));
  arr->ArrayAppend(JsonValue::CreateInteger(20));
  arr->ArrayAppend(JsonValue::CreateInteger(30));

  auto* popped = arr->ArrayPop(-1);
  ASSERT_NE(popped, nullptr);
  EXPECT_EQ(popped->GetInteger(), 30);
  JsonValue::Destroy(popped);
  EXPECT_EQ(arr->ArrayLen(), 2u);

  popped = arr->ArrayPop(0);
  ASSERT_NE(popped, nullptr);
  EXPECT_EQ(popped->GetInteger(), 10);
  JsonValue::Destroy(popped);
  EXPECT_EQ(arr->ArrayLen(), 1u);

  EXPECT_EQ(arr->ArrayPop(5), nullptr);

  JsonValue::Destroy(arr);
}

TEST(JsonValueTest, ArrayPopEmpty) {
  auto* arr = JsonValue::CreateArray();
  EXPECT_EQ(arr->ArrayPop(0), nullptr);
  EXPECT_EQ(arr->ArrayPop(-1), nullptr);
  JsonValue::Destroy(arr);
}

TEST(JsonValueTest, ArrayTrim) {
  auto* arr = JsonValue::CreateArray();
  for (int i = 0; i < 5; i++) arr->ArrayAppend(JsonValue::CreateInteger(i));

  arr->ArrayTrim(1, 3);
  EXPECT_EQ(arr->ArrayLen(), 3u);
  EXPECT_EQ(arr->ArrayGet(0)->GetInteger(), 1);
  EXPECT_EQ(arr->ArrayGet(1)->GetInteger(), 2);
  EXPECT_EQ(arr->ArrayGet(2)->GetInteger(), 3);

  JsonValue::Destroy(arr);
}

TEST(JsonValueTest, ArrayTrimOutOfRange) {
  auto* arr = JsonValue::CreateArray();
  arr->ArrayAppend(JsonValue::CreateInteger(1));
  arr->ArrayAppend(JsonValue::CreateInteger(2));

  arr->ArrayTrim(5, 10);
  EXPECT_EQ(arr->ArrayLen(), 0u);

  JsonValue::Destroy(arr);
}

TEST(JsonValueTest, ObjectBasic) {
  auto* obj = JsonValue::CreateObject();
  EXPECT_TRUE(obj->IsObject());
  EXPECT_EQ(obj->ObjectLen(), 0u);

  EXPECT_TRUE(obj->ObjectSet("name", 4, JsonValue::CreateString("Alice", 5)));
  EXPECT_TRUE(obj->ObjectSet("age", 3, JsonValue::CreateInteger(30)));
  EXPECT_EQ(obj->ObjectLen(), 2u);

  auto* name = obj->ObjectGet("name");
  ASSERT_NE(name, nullptr);
  EXPECT_EQ(name->GetString(), "Alice");

  auto* age = obj->ObjectGet("age");
  ASSERT_NE(age, nullptr);
  EXPECT_EQ(age->GetInteger(), 30);

  EXPECT_EQ(obj->ObjectGet("missing"), nullptr);

  JsonValue::Destroy(obj);
}

TEST(JsonValueTest, ObjectUpsert) {
  auto* obj = JsonValue::CreateObject();
  obj->ObjectSet("key", 3, JsonValue::CreateInteger(1));
  EXPECT_EQ(obj->ObjectGet("key")->GetInteger(), 1);

  obj->ObjectSet("key", 3, JsonValue::CreateInteger(2));
  EXPECT_EQ(obj->ObjectGet("key")->GetInteger(), 2);
  EXPECT_EQ(obj->ObjectLen(), 1u);

  JsonValue::Destroy(obj);
}

TEST(JsonValueTest, ObjectDelete) {
  auto* obj = JsonValue::CreateObject();
  obj->ObjectSet("a", 1, JsonValue::CreateInteger(1));
  obj->ObjectSet("b", 1, JsonValue::CreateInteger(2));
  obj->ObjectSet("c", 1, JsonValue::CreateInteger(3));

  EXPECT_TRUE(obj->ObjectDelete("b"));
  EXPECT_EQ(obj->ObjectLen(), 2u);
  EXPECT_EQ(obj->ObjectGet("a")->GetInteger(), 1);
  EXPECT_EQ(obj->ObjectGet("b"), nullptr);
  EXPECT_EQ(obj->ObjectGet("c")->GetInteger(), 3);

  EXPECT_FALSE(obj->ObjectDelete("missing"));
  EXPECT_EQ(obj->ObjectLen(), 2u);

  JsonValue::Destroy(obj);
}

TEST(JsonValueTest, ObjectPreservesInsertionOrder) {
  auto* obj = JsonValue::CreateObject();
  obj->ObjectSet("z", 1, JsonValue::CreateNull());
  obj->ObjectSet("a", 1, JsonValue::CreateNull());
  obj->ObjectSet("m", 1, JsonValue::CreateNull());

  auto* entries = obj->ObjectEntries();
  EXPECT_EQ(std::string_view(entries[0].key, entries[0].key_len), "z");
  EXPECT_EQ(std::string_view(entries[1].key, entries[1].key_len), "a");
  EXPECT_EQ(std::string_view(entries[2].key, entries[2].key_len), "m");

  JsonValue::Destroy(obj);
}

TEST(JsonValueTest, Clone) {
  auto* obj = JsonValue::CreateObject();
  obj->ObjectSet("arr", 3, JsonValue::CreateArray());
  obj->ObjectGet("arr")->ArrayAppend(JsonValue::CreateInteger(1));
  obj->ObjectGet("arr")->ArrayAppend(JsonValue::CreateString("two", 3));
  obj->ObjectSet("nested", 6, JsonValue::CreateObject());
  obj->ObjectGet("nested")->ObjectSet("x", 1, JsonValue::CreateBool(true));

  auto* clone = obj->Clone();
  ASSERT_NE(clone, nullptr);
  EXPECT_TRUE(clone->DeepEqual(obj));

  // Mutation doesn't affect original
  clone->ObjectGet("arr")->ArrayAppend(JsonValue::CreateInteger(3));
  EXPECT_NE(clone->ObjectGet("arr")->ArrayLen(), obj->ObjectGet("arr")->ArrayLen());

  JsonValue::Destroy(obj);
  JsonValue::Destroy(clone);
}

TEST(JsonValueTest, DeepEqual) {
  auto* a = JsonValue::CreateInteger(42);
  auto* b = JsonValue::CreateNumber(42.0);
  EXPECT_TRUE(a->DeepEqual(b));

  auto* c = JsonValue::CreateNumber(42.5);
  EXPECT_FALSE(a->DeepEqual(c));

  auto* null1 = JsonValue::CreateNull();
  auto* null2 = JsonValue::CreateNull();
  EXPECT_TRUE(null1->DeepEqual(null2));

  JsonValue::Destroy(a);
  JsonValue::Destroy(b);
  JsonValue::Destroy(c);
  JsonValue::Destroy(null1);
  JsonValue::Destroy(null2);
}

TEST(JsonValueTest, Clear) {
  auto* arr = JsonValue::CreateArray();
  arr->ArrayAppend(JsonValue::CreateInteger(1));
  arr->ArrayAppend(JsonValue::CreateInteger(2));
  arr->Clear();
  EXPECT_EQ(arr->ArrayLen(), 0u);

  auto* obj = JsonValue::CreateObject();
  obj->ObjectSet("k", 1, JsonValue::CreateNull());
  obj->Clear();
  EXPECT_EQ(obj->ObjectLen(), 0u);

  auto* num = JsonValue::CreateInteger(42);
  num->Clear();
  EXPECT_EQ(num->GetInteger(), 0);

  auto* fnum = JsonValue::CreateNumber(3.14);
  fnum->Clear();
  EXPECT_DOUBLE_EQ(fnum->GetNumber(), 0.0);

  JsonValue::Destroy(arr);
  JsonValue::Destroy(obj);
  JsonValue::Destroy(num);
  JsonValue::Destroy(fnum);
}

TEST(JsonValueTest, MemoryUsage) {
  auto* v = JsonValue::CreateString("hello world", 11);
  EXPECT_GT(v->MemoryUsage(), sizeof(JsonValue));
  JsonValue::Destroy(v);

  auto* arr = JsonValue::CreateArray();
  arr->ArrayAppend(JsonValue::CreateInteger(1));
  EXPECT_GT(arr->MemoryUsage(), sizeof(JsonValue) * 2);
  JsonValue::Destroy(arr);
}

TEST(JsonValueTest, LargeArray) {
  auto* arr = JsonValue::CreateArray();
  for (int i = 0; i < 1000; i++) {
    EXPECT_TRUE(arr->ArrayAppend(JsonValue::CreateInteger(i)));
  }
  EXPECT_EQ(arr->ArrayLen(), 1000u);
  EXPECT_EQ(arr->ArrayGet(999)->GetInteger(), 999);
  JsonValue::Destroy(arr);
}

TEST(JsonValueTest, NullOperations) {
  JsonValue::Destroy(nullptr); // should not crash
  auto* v = JsonValue::CreateNull();
  EXPECT_FALSE(v->DeepEqual(nullptr));
  JsonValue::Destroy(v);
}
