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
  EXPECT_EQ(entries[0].KeyView(), "z");
  EXPECT_EQ(entries[1].KeyView(), "a");
  EXPECT_EQ(entries[2].KeyView(), "m");

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
  auto* sso = JsonValue::CreateString("hello world", 11);
  EXPECT_EQ(sso->MemoryUsage(), sizeof(JsonValue));
  JsonValue::Destroy(sso);

  auto* v = JsonValue::CreateString("hello world this is a heap string!", 34);
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

TEST(JsonValueTest, DeepEqualLargeIntPrecision) {
  auto* a = JsonValue::CreateInteger(9007199254740992LL);  // 2^53
  auto* b = JsonValue::CreateInteger(9007199254740993LL);  // 2^53 + 1
  auto* c = JsonValue::CreateInteger(9007199254740993LL);
  EXPECT_FALSE(a->DeepEqual(b));
  EXPECT_TRUE(b->DeepEqual(c));

  auto* max_i = JsonValue::CreateInteger(INT64_MAX);
  auto* max_i_minus_1 = JsonValue::CreateInteger(INT64_MAX - 1);
  auto* max_d = JsonValue::CreateNumber(static_cast<double>(INT64_MAX)); // rounds to 2^63
  EXPECT_FALSE(max_i->DeepEqual(max_i_minus_1));
  EXPECT_FALSE(max_i->DeepEqual(max_d));

  auto* min_i = JsonValue::CreateInteger(INT64_MIN);
  auto* min_d = JsonValue::CreateNumber(-9223372036854775808.0); // exact -2^63
  EXPECT_TRUE(min_i->DeepEqual(min_d));

  JsonValue::Destroy(a);
  JsonValue::Destroy(b);
  JsonValue::Destroy(c);
  JsonValue::Destroy(max_i);
  JsonValue::Destroy(max_i_minus_1);
  JsonValue::Destroy(max_d);
  JsonValue::Destroy(min_i);
  JsonValue::Destroy(min_d);
}

TEST(JsonValueTest, EmptyStringObjectKey) {
  auto* obj = JsonValue::CreateObject();
  ASSERT_TRUE(obj->ObjectSet("", 0, JsonValue::CreateInteger(7)));
  EXPECT_EQ(obj->ObjectLen(), 1u);
  auto* got = obj->ObjectGet("");
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->GetInteger(), 7);
  EXPECT_TRUE(obj->ObjectDelete(""));
  EXPECT_EQ(obj->ObjectLen(), 0u);
  JsonValue::Destroy(obj);
}

TEST(JsonValueTest, CompactNodeSizeAndSsoBoundaries) {
  EXPECT_EQ(sizeof(JsonValue), 24u);

  for (uint32_t len : {0u, 1u, 10u, 20u, 21u, 22u, 23u, 100u, 4096u}) {
    std::string s(len, static_cast<char>('a' + (len % 26)));
    auto* v = JsonValue::CreateString(s.data(), len);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->GetString(), s);
    if (len <= JsonValue::kInlineStrMaxLen) {
      EXPECT_TRUE(v->IsInlineString());
      EXPECT_EQ(v->MemoryUsage(), sizeof(JsonValue));
    } else {
      EXPECT_FALSE(v->IsInlineString());
      EXPECT_GT(v->MemoryUsage(), sizeof(JsonValue));
    }
    auto* c = v->Clone();
    EXPECT_TRUE(v->DeepEqual(c));
    EXPECT_EQ(c->IsInlineString(), v->IsInlineString());
    JsonValue::Destroy(c);
    JsonValue::Destroy(v);
  }
}

TEST(JsonValueTest, ObjectDeleteHeavyPreservesOrder) {
  auto* obj = JsonValue::CreateObject();
  ASSERT_TRUE(obj->ObjectReserve(1000));
  for (int i = 0; i < 1000; i++) {
    std::string k = "k_" + std::to_string(i);
    ASSERT_TRUE(obj->ObjectSet(k.data(), static_cast<uint32_t>(k.size()),
                               JsonValue::CreateInteger(i)));
  }
  EXPECT_EQ(obj->ObjectLen(), 1000u);

  // Delete 750 keys (keep multiples of 4)
  for (int i = 0; i < 1000; i++) {
    if (i % 4 != 0) {
      std::string k = "k_" + std::to_string(i);
      EXPECT_TRUE(obj->ObjectDelete(k));
    }
  }
  EXPECT_EQ(obj->ObjectLen(), 250u);

  // Verify remaining lookups and exact insertion order
  auto* entries = obj->ObjectEntries();
  for (uint32_t idx = 0; idx < 250; idx++) {
    std::string expected = "k_" + std::to_string(idx * 4);
    EXPECT_EQ(entries[idx].KeyView(), expected);
    EXPECT_EQ(entries[idx].value->GetInteger(), static_cast<int64_t>(idx * 4));
    auto* got = obj->ObjectGet(expected);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->GetInteger(), static_cast<int64_t>(idx * 4));
  }

  // Re-insert some deleted keys and verify
  for (int i = 1; i < 200; i += 4) {
    std::string k = "k_" + std::to_string(i);
    ASSERT_TRUE(obj->ObjectSet(k.data(), static_cast<uint32_t>(k.size()),
                               JsonValue::CreateInteger(i + 10000)));
  }
  EXPECT_EQ(obj->ObjectLen(), 300u);
  EXPECT_EQ(obj->ObjectGet("k_1")->GetInteger(), 10001);
  EXPECT_EQ(obj->ObjectGet("k_2"), nullptr);

  JsonValue::Destroy(obj);
}

TEST(JsonValueTest, ArrayAndObjectShrinkOnTrimAndClear) {
  auto* arr = JsonValue::CreateArray();
  ASSERT_TRUE(arr->ArrayReserve(1024));
  for (int i = 0; i < 1024; i++) {
    arr->ArrayAppend(JsonValue::CreateInteger(i));
  }
  size_t mem_full = arr->MemoryUsage();
  arr->ArrayTrim(0, 3);
  EXPECT_EQ(arr->ArrayLen(), 4u);
  EXPECT_LT(arr->ArrayCap(), 1024u);
  EXPECT_LT(arr->MemoryUsage(), mem_full / 10);
  JsonValue::Destroy(arr);

  auto* obj = JsonValue::CreateObject();
  ASSERT_TRUE(obj->ObjectReserve(500));
  for (int i = 0; i < 500; i++) {
    std::string k = "key_" + std::to_string(i);
    obj->ObjectSet(k.data(), static_cast<uint32_t>(k.size()),
                   JsonValue::CreateInteger(i));
  }
  size_t obj_full = obj->MemoryUsage();
  obj->Clear();
  EXPECT_EQ(obj->ObjectLen(), 0u);
  EXPECT_LT(obj->MemoryUsage(), obj_full / 10);
  JsonValue::Destroy(obj);
}

TEST(JsonValueTest, StringAppendSsoAndHeapGrowth) {
  auto* s = JsonValue::CreateString("hi", 2);
  EXPECT_TRUE(s->IsInlineString());

  // Append within SSO limit (2 + 5 = 7 <= 21)
  EXPECT_TRUE(s->StringAppend(" there"));
  EXPECT_TRUE(s->IsInlineString());
  EXPECT_EQ(s->GetString(), "hi there");

  // Append crossing SSO boundary -> heap promotion
  EXPECT_TRUE(s->StringAppend(" — crossing the 21-byte SSO boundary!"));
  EXPECT_FALSE(s->IsInlineString());
  EXPECT_EQ(s->GetString(), "hi there — crossing the 21-byte SSO boundary!");

  // Repeated geometric heap appends
  for (int i = 0; i < 200; i++) {
    ASSERT_TRUE(s->StringAppend("0123456789"));
  }
  EXPECT_EQ(s->GetString().size(), 47u + 2000u);
  JsonValue::Destroy(s);
}

TEST(JsonValueTest, SmallKeyOptimizationBoundaries) {
  EXPECT_EQ(sizeof(JsonValue::ObjEntry), 32u);
  auto* obj = JsonValue::CreateObject();
  for (uint32_t len : {0u, 1u, 7u, 14u, 15u, 16u, 17u, 64u}) {
    std::string k(len, static_cast<char>('a' + (len % 26)));
    ASSERT_TRUE(obj->ObjectSet(k.data(), len, JsonValue::CreateInteger(len)));
  }
  auto* entries = obj->ObjectEntries();
  ASSERT_EQ(obj->ObjectLen(), 8u);
  for (uint32_t i = 0; i < 8; i++) {
    uint32_t len = entries[i].KeyLen();
    if (len <= JsonValue::ObjEntry::kInlineKeyMaxLen) {
      EXPECT_TRUE(entries[i].IsInlineKey());
    } else {
      EXPECT_FALSE(entries[i].IsInlineKey());
    }
    EXPECT_EQ(obj->ObjectGet(entries[i].KeyView())->GetInteger(), static_cast<int64_t>(len));
  }
  // Delete both inline and heap keys
  EXPECT_TRUE(obj->ObjectDelete(std::string(15, static_cast<char>('a' + 15))));
  EXPECT_TRUE(obj->ObjectDelete(std::string(16, static_cast<char>('a' + 16))));
  EXPECT_EQ(obj->ObjectLen(), 6u);
  JsonValue::Destroy(obj);
}
