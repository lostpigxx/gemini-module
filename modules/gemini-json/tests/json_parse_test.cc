#include <gtest/gtest.h>
#include "json_parse.h"
#include "json_serialize.h"
#include "json_value.h"

#include <string>

static JsonValue* Parse(const char* input) {
  auto result = JsonParse(input);
  return result.value;
}

static std::string RoundTrip(const char* input) {
  auto* v = Parse(input);
  if (!v) return "<error>";
  auto s = JsonSerialize(v);
  JsonValue::Destroy(v);
  return s;
}

// --- Primitives ---

TEST(JsonParseTest, Null) {
  auto* v = Parse("null");
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(v->IsNull());
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, True) {
  auto* v = Parse("true");
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(v->IsBool());
  EXPECT_TRUE(v->GetBool());
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, False) {
  auto* v = Parse("false");
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(v->IsBool());
  EXPECT_FALSE(v->GetBool());
  JsonValue::Destroy(v);
}

// --- Numbers ---

TEST(JsonParseTest, IntegerZero) {
  auto* v = Parse("0");
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(v->IsInteger());
  EXPECT_EQ(v->GetInteger(), 0);
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, PositiveInteger) {
  auto* v = Parse("12345");
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(v->IsInteger());
  EXPECT_EQ(v->GetInteger(), 12345);
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, NegativeInteger) {
  auto* v = Parse("-42");
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(v->IsInteger());
  EXPECT_EQ(v->GetInteger(), -42);
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, Float) {
  auto* v = Parse("3.14");
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(v->Type() == JsonType::kNumber);
  EXPECT_DOUBLE_EQ(v->GetNumber(), 3.14);
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, FloatWithExponent) {
  auto* v = Parse("1.5e10");
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(v->Type() == JsonType::kNumber);
  EXPECT_DOUBLE_EQ(v->GetNumber(), 1.5e10);
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, NegativeExponent) {
  auto* v = Parse("1e-3");
  ASSERT_NE(v, nullptr);
  EXPECT_DOUBLE_EQ(v->GetNumber(), 0.001);
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, LeadingZerosRejected) {
  auto result = JsonParse("01");
  EXPECT_EQ(result.value, nullptr);
  EXPECT_NE(result.error, nullptr);
}

// --- Strings ---

TEST(JsonParseTest, EmptyString) {
  auto* v = Parse("\"\"");
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(v->IsString());
  EXPECT_EQ(v->GetString(), "");
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, SimpleString) {
  auto* v = Parse("\"hello world\"");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->GetString(), "hello world");
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, EscapedCharacters) {
  auto* v = Parse("\"a\\\"b\\\\c\\/d\\be\\ff\\ng\\rh\\ti\"");
  ASSERT_NE(v, nullptr);
  auto s = v->GetString();
  EXPECT_EQ(s, std::string_view("a\"b\\c/d\be\ff\ng\rh\ti"));
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, UnicodeEscape) {
  auto* v = Parse("\"\\u0041\""); // 'A'
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->GetString(), "A");
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, UnicodeSurrogatePair) {
  // U+1F600 = 😀 (grinning face)
  auto* v = Parse("\"\\uD83D\\uDE00\"");
  ASSERT_NE(v, nullptr);
  auto s = v->GetString();
  EXPECT_EQ(s.size(), 4u); // 4-byte UTF-8
  EXPECT_EQ(static_cast<unsigned char>(s[0]), 0xF0);
  EXPECT_EQ(static_cast<unsigned char>(s[1]), 0x9F);
  EXPECT_EQ(static_cast<unsigned char>(s[2]), 0x98);
  EXPECT_EQ(static_cast<unsigned char>(s[3]), 0x80);
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, UnterminatedString) {
  auto result = JsonParse("\"hello");
  EXPECT_EQ(result.value, nullptr);
  EXPECT_NE(result.error, nullptr);
}

// --- Arrays ---

TEST(JsonParseTest, EmptyArray) {
  auto* v = Parse("[]");
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(v->IsArray());
  EXPECT_EQ(v->ArrayLen(), 0u);
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, SimpleArray) {
  auto* v = Parse("[1, 2, 3]");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->ArrayLen(), 3u);
  EXPECT_EQ(v->ArrayGet(0)->GetInteger(), 1);
  EXPECT_EQ(v->ArrayGet(1)->GetInteger(), 2);
  EXPECT_EQ(v->ArrayGet(2)->GetInteger(), 3);
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, MixedArray) {
  auto* v = Parse("[1, \"two\", true, null, 3.14]");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->ArrayLen(), 5u);
  EXPECT_TRUE(v->ArrayGet(0)->IsInteger());
  EXPECT_TRUE(v->ArrayGet(1)->IsString());
  EXPECT_TRUE(v->ArrayGet(2)->IsBool());
  EXPECT_TRUE(v->ArrayGet(3)->IsNull());
  EXPECT_TRUE(v->ArrayGet(4)->Type() == JsonType::kNumber);
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, NestedArray) {
  auto* v = Parse("[[1, 2], [3, [4]]]");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->ArrayLen(), 2u);
  EXPECT_EQ(v->ArrayGet(0)->ArrayLen(), 2u);
  EXPECT_EQ(v->ArrayGet(1)->ArrayGet(1)->ArrayGet(0)->GetInteger(), 4);
  JsonValue::Destroy(v);
}

// --- Objects ---

TEST(JsonParseTest, EmptyObject) {
  auto* v = Parse("{}");
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(v->IsObject());
  EXPECT_EQ(v->ObjectLen(), 0u);
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, SimpleObject) {
  auto* v = Parse("{\"name\": \"Alice\", \"age\": 30}");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->ObjectLen(), 2u);
  EXPECT_EQ(v->ObjectGet("name")->GetString(), "Alice");
  EXPECT_EQ(v->ObjectGet("age")->GetInteger(), 30);
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, NestedObject) {
  auto* v = Parse("{\"a\": {\"b\": {\"c\": 42}}}");
  ASSERT_NE(v, nullptr);
  auto* c = v->ObjectGet("a")->ObjectGet("b")->ObjectGet("c");
  EXPECT_EQ(c->GetInteger(), 42);
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, DuplicateKeysLastWins) {
  auto* v = Parse("{\"key\": 1, \"key\": 2}");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->ObjectLen(), 1u);
  EXPECT_EQ(v->ObjectGet("key")->GetInteger(), 2);
  JsonValue::Destroy(v);
}

// --- Whitespace ---

TEST(JsonParseTest, WhitespaceVariants) {
  auto* v = Parse("  { \n \"key\" \t: \r\n \"value\" \n } ");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->ObjectGet("key")->GetString(), "value");
  JsonValue::Destroy(v);
}

// --- Error cases ---

TEST(JsonParseTest, TrailingContent) {
  auto result = JsonParse("123 456");
  EXPECT_EQ(result.value, nullptr);
  EXPECT_NE(result.error, nullptr);
}

TEST(JsonParseTest, EmptyInput) {
  auto result = JsonParse("");
  EXPECT_EQ(result.value, nullptr);
}

TEST(JsonParseTest, TrailingCommaArray) {
  // Our parser allows trailing commas implicitly since it reads the next value
  // after comma, which would be ] and that generates an error.
  auto result = JsonParse("[1,]");
  EXPECT_EQ(result.value, nullptr);
}

TEST(JsonParseTest, InvalidToken) {
  auto result = JsonParse("undefined");
  EXPECT_EQ(result.value, nullptr);
}

// --- Round-trip ---

TEST(JsonParseTest, RoundTripNull) {
  EXPECT_EQ(RoundTrip("null"), "null");
}

TEST(JsonParseTest, RoundTripBool) {
  EXPECT_EQ(RoundTrip("true"), "true");
  EXPECT_EQ(RoundTrip("false"), "false");
}

TEST(JsonParseTest, RoundTripInteger) {
  EXPECT_EQ(RoundTrip("42"), "42");
  EXPECT_EQ(RoundTrip("-100"), "-100");
  EXPECT_EQ(RoundTrip("0"), "0");
}

TEST(JsonParseTest, RoundTripFloat) {
  // Float round-trip may not be character-exact but should parse back to same value
  auto* v = Parse("3.14");
  auto s = JsonSerialize(v);
  auto* v2 = Parse(s.c_str());
  ASSERT_NE(v2, nullptr);
  EXPECT_DOUBLE_EQ(v->GetNumber(), v2->GetNumber());
  JsonValue::Destroy(v);
  JsonValue::Destroy(v2);
}

TEST(JsonParseTest, RoundTripString) {
  EXPECT_EQ(RoundTrip("\"hello\""), "\"hello\"");
  EXPECT_EQ(RoundTrip("\"a\\\"b\""), "\"a\\\"b\"");
}

TEST(JsonParseTest, RoundTripArray) {
  EXPECT_EQ(RoundTrip("[]"), "[]");
  EXPECT_EQ(RoundTrip("[1,2,3]"), "[1,2,3]");
}

TEST(JsonParseTest, RoundTripObject) {
  EXPECT_EQ(RoundTrip("{}"), "{}");
  // Object key order is preserved
  auto* v = Parse("{\"a\":1,\"b\":2}");
  auto s = JsonSerialize(v);
  EXPECT_EQ(s, "{\"a\":1,\"b\":2}");
  JsonValue::Destroy(v);
}

// --- Serializer options ---

TEST(JsonSerializeTest, PrettyPrint) {
  auto* v = Parse("{\"a\":1,\"b\":[2,3]}");
  ASSERT_NE(v, nullptr);
  SerializeOptions opts;
  opts.indent = "  ";
  opts.newline = "\n";
  opts.space = " ";
  auto s = JsonSerialize(v, opts);

  EXPECT_NE(s.find('\n'), std::string::npos);
  EXPECT_NE(s.find("  "), std::string::npos);
  JsonValue::Destroy(v);
}

TEST(JsonSerializeTest, CompactIsDefault) {
  auto* v = Parse("{\"key\": [1, 2]}");
  auto s = JsonSerialize(v);
  EXPECT_EQ(s.find(' '), std::string::npos);
  EXPECT_EQ(s.find('\n'), std::string::npos);
  JsonValue::Destroy(v);
}

TEST(JsonSerializeTest, EscapesControlChars) {
  auto* v = JsonValue::CreateString("a\nb\tc", 5);
  auto s = JsonSerialize(v);
  EXPECT_EQ(s, "\"a\\nb\\tc\"");
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, LargeInteger) {
  auto* v = Parse("9223372036854775807"); // INT64_MAX
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(v->IsInteger());
  EXPECT_EQ(v->GetInteger(), 9223372036854775807LL);
  JsonValue::Destroy(v);
}

TEST(JsonParseTest, ComplexDocument) {
  const char* json = R"({
    "store": {
      "book": [
        {"category": "fiction", "price": 8.95},
        {"category": "fiction", "price": 12.99}
      ],
      "bicycle": {"color": "red", "price": 19.95}
    }
  })";
  auto* v = Parse(json);
  ASSERT_NE(v, nullptr);
  auto* book = v->ObjectGet("store")->ObjectGet("book");
  EXPECT_EQ(book->ArrayLen(), 2u);
  EXPECT_DOUBLE_EQ(book->ArrayGet(0)->ObjectGet("price")->GetNumber(), 8.95);
  JsonValue::Destroy(v);
}
