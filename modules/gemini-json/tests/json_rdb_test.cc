// RDB serialization round-trip tests for gemini-json.
// Verifies binary format (encver 2), text format (encver 1/3),
// cross-format compatibility, and compat-mode switching.

#define REDISMODULE_API
extern "C" {
#include "redismodule.h"
}

#include "mock_redismodule_io.h"

#include <gtest/gtest.h>
#include "json_type.h"
#include "json_value.h"
#include "json_parse.h"
#include "json_serialize.h"

#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

// Install the mock IO once, before any tests run.
class JsonRdbTestEnv : public ::testing::Environment {
public:
  void SetUp() override { InstallMockRedisModuleIO(); }
};
static auto* const gEnv [[maybe_unused]] =
  ::testing::AddGlobalTestEnvironment(new JsonRdbTestEnv);

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

static JsonValue* Parse(const char* json) {
  return JsonParse(json).value;
}

// Save then load using the given format modes.
// save_compat = true  → TextSave (JSON text)
// save_compat = false → BinarySave
// load_encver selects the deserialization path in RdbLoadJson.
static JsonValue* RoundTrip(const JsonValue* src, bool save_compat, int load_encver) {
  MockRdbStream stream;

  bool prev = JsonCompatMode;
  JsonCompatMode = save_compat;
  RdbSaveJson(stream.IO(), const_cast<void*>(static_cast<const void*>(src)));
  JsonCompatMode = prev;

  stream.Rewind();
  return static_cast<JsonValue*>(RdbLoadJson(stream.IO(), load_encver));
}

static JsonValue* BinaryRoundTrip(const JsonValue* src) {
  return RoundTrip(src, /*save_compat=*/false, /*load_encver=*/2);
}

static JsonValue* TextRoundTrip(const JsonValue* src) {
  return RoundTrip(src, /*save_compat=*/true, /*load_encver=*/3);
}

// ==================================================================
// Binary format (encver 2) round-trip
// ==================================================================

TEST(JsonRdbBinary, Null) {
  auto* v = JsonValue::CreateNull();
  auto* loaded = BinaryRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(loaded->IsNull());
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbBinary, BoolTrue) {
  auto* v = JsonValue::CreateBool(true);
  auto* loaded = BinaryRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(loaded->IsBool());
  EXPECT_TRUE(loaded->GetBool());
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbBinary, BoolFalse) {
  auto* v = JsonValue::CreateBool(false);
  auto* loaded = BinaryRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_FALSE(loaded->GetBool());
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbBinary, Integer) {
  auto* v = JsonValue::CreateInteger(9223372036854775807LL); // INT64_MAX
  auto* loaded = BinaryRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(loaded->IsInteger());
  EXPECT_EQ(loaded->GetInteger(), 9223372036854775807LL);
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbBinary, NegativeInteger) {
  auto* v = JsonValue::CreateInteger(-42);
  auto* loaded = BinaryRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->GetInteger(), -42);
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbBinary, Double) {
  auto* v = JsonValue::CreateNumber(3.141592653589793);
  auto* loaded = BinaryRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(loaded->Type() == JsonType::kNumber);
  EXPECT_DOUBLE_EQ(loaded->GetNumber(), 3.141592653589793);
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbBinary, DoubleSpecialValues) {
  for (double d : {0.0, -0.0, 1e308, -1e308, 5e-324}) {
    auto* v = JsonValue::CreateNumber(d);
    auto* loaded = BinaryRoundTrip(v);
    ASSERT_NE(loaded, nullptr);
    double got = loaded->GetNumber();
    EXPECT_EQ(std::memcmp(&d, &got, sizeof(double)), 0)
      << "Mismatch for value " << d;
    JsonValue::Destroy(v);
    JsonValue::Destroy(loaded);
  }
}

TEST(JsonRdbBinary, EmptyString) {
  auto* v = JsonValue::CreateString("", 0);
  auto* loaded = BinaryRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(loaded->IsString());
  EXPECT_EQ(loaded->GetString(), "");
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbBinary, StringWithEscapes) {
  const char data[] = "hello\n\t\"world\"\x01\xff";
  auto* v = JsonValue::CreateString(data, sizeof(data) - 1);
  auto* loaded = BinaryRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->GetString().size(), sizeof(data) - 1);
  EXPECT_EQ(std::memcmp(loaded->GetString().data(), data, sizeof(data) - 1), 0);
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbBinary, LongString) {
  std::string s(100000, 'x');
  auto* v = JsonValue::CreateString(s.c_str(), static_cast<uint32_t>(s.size()));
  auto* loaded = BinaryRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->GetString().size(), 100000u);
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbBinary, EmptyArray) {
  auto* v = JsonValue::CreateArray();
  auto* loaded = BinaryRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(loaded->IsArray());
  EXPECT_EQ(loaded->ArrayLen(), 0u);
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbBinary, EmptyObject) {
  auto* v = JsonValue::CreateObject();
  auto* loaded = BinaryRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(loaded->IsObject());
  EXPECT_EQ(loaded->ObjectLen(), 0u);
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbBinary, ComplexDocument) {
  auto* v = Parse(R"({
    "store": {
      "book": [
        {"category": "reference", "author": "Nigel", "price": 8.95},
        {"category": "fiction", "author": "Tolkien", "price": 22.99}
      ],
      "bicycle": {"color": "red", "price": 19.95}
    },
    "expensive": 10,
    "active": true,
    "deleted": null
  })");
  ASSERT_NE(v, nullptr);

  auto* loaded = BinaryRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(v->DeepEqual(loaded));

  // Spot-check nested access
  auto* book = loaded->ObjectGet("store")->ObjectGet("book");
  EXPECT_EQ(book->ArrayLen(), 2u);
  EXPECT_EQ(book->ArrayGet(1)->ObjectGet("author")->GetString(), "Tolkien");
  EXPECT_DOUBLE_EQ(book->ArrayGet(0)->ObjectGet("price")->GetNumber(), 8.95);
  EXPECT_TRUE(loaded->ObjectGet("active")->GetBool());
  EXPECT_TRUE(loaded->ObjectGet("deleted")->IsNull());

  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbBinary, DeeplyNested) {
  std::string json;
  constexpr int depth = 100;
  for (int i = 0; i < depth; i++) json += "{\"n\":";
  json += "42";
  for (int i = 0; i < depth; i++) json += "}";

  auto* v = Parse(json.c_str());
  ASSERT_NE(v, nullptr);
  auto* loaded = BinaryRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(v->DeepEqual(loaded));
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbBinary, LargeArray) {
  auto* arr = JsonValue::CreateArray();
  for (int i = 0; i < 1000; i++) arr->ArrayAppend(JsonValue::CreateInteger(i));
  auto* loaded = BinaryRoundTrip(arr);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->ArrayLen(), 1000u);
  EXPECT_EQ(loaded->ArrayGet(0)->GetInteger(), 0);
  EXPECT_EQ(loaded->ArrayGet(999)->GetInteger(), 999);
  JsonValue::Destroy(arr);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbBinary, LargeObject) {
  auto* obj = JsonValue::CreateObject();
  for (int i = 0; i < 500; i++) {
    auto key = "key_" + std::to_string(i);
    obj->ObjectSet(key.c_str(), static_cast<uint32_t>(key.size()),
                   JsonValue::CreateInteger(i));
  }
  auto* loaded = BinaryRoundTrip(obj);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->ObjectLen(), 500u);
  EXPECT_EQ(loaded->ObjectGet("key_0")->GetInteger(), 0);
  EXPECT_EQ(loaded->ObjectGet("key_499")->GetInteger(), 499);
  JsonValue::Destroy(obj);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbBinary, MixedTypesArray) {
  auto* v = Parse(R"([null, true, false, 42, -1, 3.14, "hello", [], {}])");
  ASSERT_NE(v, nullptr);
  auto* loaded = BinaryRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(v->DeepEqual(loaded));
  EXPECT_EQ(loaded->ArrayLen(), 9u);
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

// ==================================================================
// Text format (encver 1 & 3) round-trip
// ==================================================================

TEST(JsonRdbText, Null) {
  auto* v = JsonValue::CreateNull();
  auto* loaded = TextRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(loaded->IsNull());
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbText, Bool) {
  for (bool b : {true, false}) {
    auto* v = JsonValue::CreateBool(b);
    auto* loaded = TextRoundTrip(v);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetBool(), b);
    JsonValue::Destroy(v);
    JsonValue::Destroy(loaded);
  }
}

TEST(JsonRdbText, Integer) {
  auto* v = JsonValue::CreateInteger(-9223372036854775807LL);
  auto* loaded = TextRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(loaded->IsInteger());
  EXPECT_EQ(loaded->GetInteger(), -9223372036854775807LL);
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbText, Double) {
  auto* v = JsonValue::CreateNumber(2.718281828459045);
  auto* loaded = TextRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_DOUBLE_EQ(loaded->GetNumber(), 2.718281828459045);
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbText, StringWithUnicode) {
  auto* v = Parse(R"("hello 世界")"); // hello 世界
  ASSERT_NE(v, nullptr);
  auto* loaded = TextRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->GetString(), v->GetString());
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbText, ComplexDocument) {
  auto* v = Parse(R"({
    "users": [
      {"name": "Alice", "age": 30, "active": true},
      {"name": "Bob", "age": 25, "active": false}
    ],
    "count": 2,
    "meta": null
  })");
  ASSERT_NE(v, nullptr);
  auto* loaded = TextRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(v->DeepEqual(loaded));
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbText, Encver1Load) {
  // encver 1 uses the same text format — verify it loads correctly
  auto* v = Parse(R"({"key": [1, 2, 3]})");
  ASSERT_NE(v, nullptr);
  auto* loaded = RoundTrip(v, /*save_compat=*/true, /*load_encver=*/1);
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(v->DeepEqual(loaded));
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

// ==================================================================
// Cross-format: save as binary, verify text load of same data fails
// gracefully; save as text, verify binary load of same data fails
// gracefully — the two formats are NOT interchangeable.
// ==================================================================

TEST(JsonRdbCrossFormat, BinarySaveTextLoadAreIncompatible) {
  auto* v = JsonValue::CreateInteger(42);
  MockRdbStream stream;

  JsonCompatMode = false;
  RdbSaveJson(stream.IO(), v);

  stream.Rewind();
  // Try loading binary data with text path (encver 1) — should fail or
  // produce a different/null result because binary data isn't valid JSON.
  auto* result = static_cast<JsonValue*>(RdbLoadJson(stream.IO(), 1));
  // Binary data is not valid JSON text, so parse should fail
  EXPECT_EQ(result, nullptr);

  JsonValue::Destroy(v);
  JsonValue::Destroy(result);
}

// ==================================================================
// Compat mode flag behavior
// ==================================================================

TEST(JsonRdbCompat, DefaultModeWritesBinary) {
  auto* v = JsonValue::CreateInteger(42);
  MockRdbStream stream;

  JsonCompatMode = false;
  RdbSaveJson(stream.IO(), v);

  // Binary format: first 8 bytes are the type tag (kTagInteger = 3)
  ASSERT_GE(stream.buf.size(), 8u);
  uint64_t tag;
  std::memcpy(&tag, stream.buf.data(), sizeof(tag));
  EXPECT_EQ(tag, 3u); // kTagInteger

  JsonValue::Destroy(v);
}

TEST(JsonRdbCompat, CompatModeWritesText) {
  auto* v = JsonValue::CreateInteger(42);
  MockRdbStream stream;

  JsonCompatMode = true;
  RdbSaveJson(stream.IO(), v);
  JsonCompatMode = false;

  // Text format: length-prefixed string "42"
  // First 8 bytes = string length (2), then "42"
  ASSERT_GE(stream.buf.size(), 10u);
  uint64_t len;
  std::memcpy(&len, stream.buf.data(), sizeof(len));
  EXPECT_EQ(len, 2u);
  EXPECT_EQ(stream.buf[8], '4');
  EXPECT_EQ(stream.buf[9], '2');

  JsonValue::Destroy(v);
}

TEST(JsonRdbCompat, SwitchModesRoundTrip) {
  // Save in compat mode, load in compat mode
  auto* v = Parse(R"({"a": 1, "b": [true, null]})");
  ASSERT_NE(v, nullptr);

  auto* loaded_text = RoundTrip(v, true, 3);
  ASSERT_NE(loaded_text, nullptr);
  EXPECT_TRUE(v->DeepEqual(loaded_text));

  // Save in native mode, load in native mode
  auto* loaded_bin = RoundTrip(v, false, 2);
  ASSERT_NE(loaded_bin, nullptr);
  EXPECT_TRUE(v->DeepEqual(loaded_bin));

  // Both loaded versions should be equal
  EXPECT_TRUE(loaded_text->DeepEqual(loaded_bin));

  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded_text);
  JsonValue::Destroy(loaded_bin);
}

// ==================================================================
// Invalid encver rejection
// ==================================================================

TEST(JsonRdbVersion, RejectsUnknownEncver) {
  MockRdbStream stream;
  auto* v = JsonValue::CreateNull();
  JsonCompatMode = false;
  RdbSaveJson(stream.IO(), v);
  stream.Rewind();

  auto* result = static_cast<JsonValue*>(RdbLoadJson(stream.IO(), 99));
  EXPECT_EQ(result, nullptr);

  JsonValue::Destroy(v);
}

TEST(JsonRdbVersion, RejectsEncver0) {
  MockRdbStream stream;
  auto* v = JsonValue::CreateNull();
  JsonCompatMode = true;
  RdbSaveJson(stream.IO(), v);
  stream.Rewind();

  auto* result = static_cast<JsonValue*>(RdbLoadJson(stream.IO(), 0));
  EXPECT_EQ(result, nullptr);

  JsonValue::Destroy(v);
}

// ==================================================================
// Stress: repeated save/load cycles
// ==================================================================

TEST(JsonRdbStress, RepeatedBinaryRoundTrips) {
  auto* v = Parse(R"({"data": [1, "two", 3.0, true, null, {"nested": []}]})");
  ASSERT_NE(v, nullptr);

  for (int i = 0; i < 100; i++) {
    auto* loaded = BinaryRoundTrip(v);
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(v->DeepEqual(loaded));
    JsonValue::Destroy(loaded);
  }
  JsonValue::Destroy(v);
}

TEST(JsonRdbStress, RepeatedTextRoundTrips) {
  auto* v = Parse(R"({"data": [1, "two", 3.0, true, null, {"nested": []}]})");
  ASSERT_NE(v, nullptr);

  for (int i = 0; i < 100; i++) {
    auto* loaded = TextRoundTrip(v);
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(v->DeepEqual(loaded));
    JsonValue::Destroy(loaded);
  }
  JsonValue::Destroy(v);
}

// ==================================================================
// Integer precision: text format may lose integer type distinction
// ==================================================================

TEST(JsonRdbPrecision, BinaryPreservesIntegerType) {
  auto* v = JsonValue::CreateInteger(42);
  auto* loaded = BinaryRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  // Binary format preserves the exact type tag
  EXPECT_EQ(loaded->Type(), JsonType::kInteger);
  EXPECT_EQ(loaded->GetInteger(), 42);
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbPrecision, TextPreservesIntegerType) {
  auto* v = JsonValue::CreateInteger(42);
  auto* loaded = TextRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  // Text format serializes as "42" which the parser reads back as integer
  EXPECT_TRUE(loaded->IsInteger());
  EXPECT_EQ(loaded->GetInteger(), 42);
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

TEST(JsonRdbPrecision, TextLargeDoubleRoundTrip) {
  // Doubles near the edge of precision
  auto* v = JsonValue::CreateNumber(1.7976931348623157e+308); // near DBL_MAX
  auto* loaded = TextRoundTrip(v);
  ASSERT_NE(loaded, nullptr);
  EXPECT_DOUBLE_EQ(loaded->GetNumber(), 1.7976931348623157e+308);
  JsonValue::Destroy(v);
  JsonValue::Destroy(loaded);
}

// ==================================================================
// Object key ordering preserved through RDB
// ==================================================================

TEST(JsonRdbOrder, ObjectKeyOrderPreserved) {
  auto* v = Parse(R"({"z": 1, "a": 2, "m": 3})");
  ASSERT_NE(v, nullptr);

  for (bool compat : {false, true}) {
    int encver = compat ? 3 : 2;
    auto* loaded = RoundTrip(v, compat, encver);
    ASSERT_NE(loaded, nullptr);

    auto* entries = loaded->ObjectEntries();
    EXPECT_EQ(entries[0].KeyView(), "z");
    EXPECT_EQ(entries[1].KeyView(), "a");
    EXPECT_EQ(entries[2].KeyView(), "m");

    JsonValue::Destroy(loaded);
  }
  JsonValue::Destroy(v);
}

// ==================================================================
// Hardened RDB security & Redis 7 ModuleType callbacks
// ==================================================================

TEST(JsonRdbHardening, TruncatedBinaryStreamRejected) {
  MockRdbStream stream;
  auto* v = Parse(R"({"a":[1,2,3],"b":"hello"})");
  ASSERT_NE(v, nullptr);
  JsonCompatMode = false;
  RdbSaveJson(stream.IO(), v);
  JsonValue::Destroy(v);

  size_t half = stream.buf.size() / 2;
  ASSERT_GT(half, 0u);
  stream.buf.resize(half);
  stream.Rewind();

  auto* loaded = static_cast<JsonValue*>(RdbLoadJson(stream.IO(), 2));
  EXPECT_EQ(loaded, nullptr);
}

TEST(JsonRdbHardening, ExcessiveDepthRejected) {
  MockRdbStream stream;
  for (int i = 0; i < 150; i++) {
    RedisModule_SaveUnsigned(stream.IO(), 6);  // kTagArray
    RedisModule_SaveUnsigned(stream.IO(), 1);
  }
  RedisModule_SaveUnsigned(stream.IO(), 0);  // kTagNull
  stream.Rewind();

  auto* loaded = static_cast<JsonValue*>(RdbLoadJson(stream.IO(), 2));
  EXPECT_EQ(loaded, nullptr);
}

TEST(JsonRdbHardening, ExcessiveCollectionCountRejected) {
  MockRdbStream stream;
  RedisModule_SaveUnsigned(stream.IO(), 6);  // kTagArray
  RedisModule_SaveUnsigned(stream.IO(), 999999999ULL);
  stream.Rewind();

  auto* loaded = static_cast<JsonValue*>(RdbLoadJson(stream.IO(), 2));
  EXPECT_EQ(loaded, nullptr);
}

TEST(JsonModuleTypeTest, CopyFreeEffortDigestAndDefrag) {
  auto* v1 = Parse(R"({"b":2,"a":[1,true,"x"]})");
  auto* v2 = Parse(R"({"a":[1,true,"x"],"b":2})");
  auto* v3 = Parse(R"({"a":[1,false,"x"],"b":2})");
  ASSERT_NE(v1, nullptr);
  ASSERT_NE(v2, nullptr);
  ASSERT_NE(v3, nullptr);

  auto* cloned = static_cast<JsonValue*>(CopyJson2(nullptr, v1));
  ASSERT_NE(cloned, nullptr);
  EXPECT_TRUE(v1->DeepEqual(cloned));
  cloned->ObjectSet("c", 1, JsonValue::CreateInteger(99));
  EXPECT_FALSE(v1->DeepEqual(cloned));

  EXPECT_GE(FreeEffortJson2(nullptr, v1), 6u);
  EXPECT_EQ(FreeEffortJson2(nullptr, nullptr), 1u);

  MockDigest d1, d2, d3;
  DigestJson(d1.Handle(), v1);
  DigestJson(d2.Handle(), v2);
  DigestJson(d3.Handle(), v3);
  EXPECT_TRUE(d1.ended);
  EXPECT_EQ(d1.bytes, d2.bytes);
  EXPECT_NE(d1.bytes, d3.bytes);

  void* ptr = v1;
  EXPECT_EQ(DefragJson(nullptr, nullptr, &ptr), 0);
  v1 = static_cast<JsonValue*>(ptr);
  EXPECT_TRUE(v1->DeepEqual(v2));

  JsonValue::Destroy(cloned);
  JsonValue::Destroy(v1);
  JsonValue::Destroy(v2);
  JsonValue::Destroy(v3);
}

TEST(JsonModuleTypeTest, FreeEffortBoundedCutoffAndDefragLargeTree) {
  auto* root = JsonValue::CreateObject();
  auto* arr = JsonValue::CreateArray();
  for (int i = 0; i < 500; i++) {
    auto* item = JsonValue::CreateObject();
    item->ObjectSet("short", 5, JsonValue::CreateString("sso", 3));
    item->ObjectSet("long_field", 10,
                    JsonValue::CreateString("this_string_exceeds_21_bytes_for_heap", 37));
    item->ObjectSet("id", 2, JsonValue::CreateInteger(i));
    arr->ArrayAppend(item);
  }
  root->ObjectSet("items", 5, arr);
  for (int i = 0; i < 30; i++) {
    std::string k = "extra_" + std::to_string(i);
    root->ObjectSet(k.data(), static_cast<uint32_t>(k.size()),
                    JsonValue::CreateInteger(i));
  }

  // FreeEffort must cut off at 64 instead of traversing all ~2000 nodes
  EXPECT_EQ(FreeEffortJson2(nullptr, root), 64u);

  auto* snapshot = root->Clone();
  void* ptr = root;
  EXPECT_EQ(DefragJson(nullptr, nullptr, &ptr), 0);
  root = static_cast<JsonValue*>(ptr);
  EXPECT_TRUE(root->DeepEqual(snapshot));

  JsonValue::Destroy(snapshot);
  JsonValue::Destroy(root);
}

TEST(JsonRdbBinary, VeryLargeArrayAndObjectReserveRoundTrip) {
  auto* arr = JsonValue::CreateArray();
  ASSERT_TRUE(arr->ArrayReserve(10000));
  for (int i = 0; i < 10000; i++) {
    arr->ArrayAppend(JsonValue::CreateInteger(i));
  }
  auto* loaded_arr = BinaryRoundTrip(arr);
  ASSERT_NE(loaded_arr, nullptr);
  EXPECT_EQ(loaded_arr->ArrayLen(), 10000u);
  EXPECT_EQ(loaded_arr->ArrayGet(9999)->GetInteger(), 9999);
  JsonValue::Destroy(arr);
  JsonValue::Destroy(loaded_arr);

  auto* obj = JsonValue::CreateObject();
  ASSERT_TRUE(obj->ObjectReserve(5000));
  for (int i = 0; i < 5000; i++) {
    std::string k = "field_" + std::to_string(i);
    obj->ObjectSet(k.data(), static_cast<uint32_t>(k.size()),
                   JsonValue::CreateString("short", 5));
  }
  auto* loaded_obj = BinaryRoundTrip(obj);
  ASSERT_NE(loaded_obj, nullptr);
  EXPECT_EQ(loaded_obj->ObjectLen(), 5000u);
  EXPECT_EQ(loaded_obj->ObjectGet("field_4999")->GetString(), "short");
  JsonValue::Destroy(obj);
  JsonValue::Destroy(loaded_obj);
}
