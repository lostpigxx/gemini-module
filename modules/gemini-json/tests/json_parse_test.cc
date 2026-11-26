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

TEST(JsonParseTest, MaxDepthAllowedAndRejected) {
  auto make_nested = [](int depth) {
    std::string s;
    s.reserve(static_cast<size_t>(depth) * 2 + 1);
    for (int i = 0; i < depth; i++) s += '[';
    s += '1';
    for (int i = 0; i < depth; i++) s += ']';
    return s;
  };

  auto ok = JsonParse(make_nested(kMaxJsonDepth));
  ASSERT_NE(ok.value, nullptr);
  JsonValue::Destroy(ok.value);

  auto bad1 = JsonParse(make_nested(kMaxJsonDepth + 1));
  EXPECT_EQ(bad1.value, nullptr);
  EXPECT_NE(bad1.error, nullptr);

  auto bad2 = JsonParse(make_nested(5000));
  EXPECT_EQ(bad2.value, nullptr);
  EXPECT_NE(bad2.error, nullptr);
}

TEST(JsonParseTest, FloatOverflowRejected) {
  auto r = JsonParse("1e9999");
  EXPECT_EQ(r.value, nullptr);
  EXPECT_NE(r.error, nullptr);
}

TEST(JsonParseTest, RejectUnescapedControlChars) {
  std::string with_nl = "\"hello\nworld\"";
  std::string with_tab = "\"hello\tworld\"";
  std::string with_ctl = "\"a\x01""b\"";
  std::string with_nul("\"a\0b\"", 5);

  for (const auto& s : {with_nl, with_tab, with_ctl, with_nul}) {
    auto r = JsonParse(s);
    EXPECT_EQ(r.value, nullptr) << "Should reject control char";
    EXPECT_NE(r.error, nullptr);
  }
}

TEST(JsonParseTest, Utf8Validation) {
  // Valid 2, 3, 4-byte UTF-8
  auto ok = JsonParse("\"\xC3\xA9 \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x98\x80\"");
  ASSERT_NE(ok.value, nullptr);
  JsonValue::Destroy(ok.value);

  // Invalid UTF-8 sequences
  const std::string bad_utf8[] = {
    "\"\xFF\"",                   // invalid lead byte
    "\"\xC2\"",                   // truncated 2-byte
    "\"\xC0\xAF\"",               // overlong '/'
    "\"\xED\xA0\x80\"",           // UTF-16 surrogate in UTF-8
    "\"\xF4\x90\x80\x80\"",       // > U+10FFFF
    "\"\xE2\x82\"",               // truncated 3-byte
  };
  for (const auto& s : bad_utf8) {
    auto r = JsonParse(s);
    EXPECT_EQ(r.value, nullptr);
    EXPECT_NE(r.error, nullptr);
  }
}

TEST(JsonParseTest, Int64BoundariesAndPromotion) {
  auto rmax = JsonParse("9223372036854775807");
  ASSERT_NE(rmax.value, nullptr);
  EXPECT_TRUE(rmax.value->IsInteger());
  EXPECT_EQ(rmax.value->GetInteger(), INT64_MAX);
  JsonValue::Destroy(rmax.value);

  auto rmin = JsonParse("-9223372036854775808");
  ASSERT_NE(rmin.value, nullptr);
  EXPECT_TRUE(rmin.value->IsInteger());
  EXPECT_EQ(rmin.value->GetInteger(), INT64_MIN);
  JsonValue::Destroy(rmin.value);

  auto rover = JsonParse("9223372036854775808");
  ASSERT_NE(rover.value, nullptr);
  EXPECT_EQ(rover.value->Type(), JsonType::kNumber);
  EXPECT_DOUBLE_EQ(rover.value->GetNumber(), 9223372036854775808.0);
  JsonValue::Destroy(rover.value);
}

TEST(JsonSerializeTest, IndependentFormattingAndControlHex) {
  auto* v = Parse(R"({"a":1,"b":"x"})");
  ASSERT_NE(v, nullptr);

  SerializeOptions space_only;
  space_only.space = " ";
  EXPECT_EQ(JsonSerialize(v, space_only), R"({"a": 1,"b": "x"})");

  SerializeOptions nl_only;
  nl_only.newline = "\n";
  EXPECT_EQ(JsonSerialize(v, nl_only), "{\n\"a\":1,\n\"b\":\"x\"\n}");

  JsonValue::Destroy(v);

  auto* ctl = JsonValue::CreateString("\x01\x1f", 2);
  EXPECT_EQ(JsonSerialize(ctl), "\"\\u0001\\u001f\"");
  JsonValue::Destroy(ctl);
}

TEST(JsonSerializeTest, ZeroCopyArrayAndObjectOfArrays) {
  auto* doc = Parse(R"({"a":1,"b":[2,3],"c":"hi"})");
  ASSERT_NE(doc, nullptr);

  const JsonValue* items[] = {doc->ObjectGet("a"), doc->ObjectGet("b")};
  EXPECT_EQ(JsonSerializeArray(items, 2), "[1,[2,3]]");

  const JsonValue* c_items[] = {doc->ObjectGet("c")};
  NamedMatchList lists[] = {
    {"$.a", items, 1},
    {"$.c", c_items, 1},
  };
  EXPECT_EQ(JsonSerializeObjectOfArrays(lists, 2), R"({"$.a":[1],"$.c":["hi"]})");
  JsonValue::Destroy(doc);
}

TEST(JsonSimdTest, ExhaustiveLaneOffsetsParseAndSerialize) {
  // Test every offset 0..48 (spanning multiple 16B NEON/SSE2 and 8B SWAR lanes)
  for (size_t pos = 0; pos <= 48; pos++) {
    // 1. Clean ASCII string of length pos
    std::string clean(pos, 'a');
    std::string raw_clean = "\"" + clean + "\"";
    auto rc = JsonParse(raw_clean);
    ASSERT_NE(rc.value, nullptr) << "clean len=" << pos;
    EXPECT_EQ(rc.value->GetString(), clean);
    EXPECT_EQ(JsonSerialize(rc.value), raw_clean);
    JsonValue::Destroy(rc.value);

    // 2. Escaped quote and backslash at offset pos
    std::string with_esc = std::string(pos, 'b') + "\"\\" + std::string(17, 'c');
    auto* v = JsonValue::CreateString(with_esc.data(), static_cast<uint32_t>(with_esc.size()));
    std::string ser = JsonSerialize(v);
    auto r2 = JsonParse(ser);
    ASSERT_NE(r2.value, nullptr) << "esc pos=" << pos;
    EXPECT_EQ(r2.value->GetString(), with_esc);
    JsonValue::Destroy(v);
    JsonValue::Destroy(r2.value);

    // 3. Control char at offset pos rejected in parse, escaped in serialize
    std::string with_ctl = "\"" + std::string(pos, 'd') + std::string(1, '\x07') + "z\"";
    auto rctl = JsonParse(with_ctl);
    EXPECT_EQ(rctl.value, nullptr) << "ctl pos=" << pos;

    // 4. Multi-byte UTF-8 at offset pos accepted & round-trips
    std::string with_utf8 = std::string(pos, 'e') + "\xE4\xB8\x96\xE7\x95\x8C" + std::string(19, 'f');
    std::string raw_utf8 = "\"" + with_utf8 + "\"";
    auto ru = JsonParse(raw_utf8);
    ASSERT_NE(ru.value, nullptr) << "utf8 pos=" << pos;
    EXPECT_EQ(ru.value->GetString(), with_utf8);
    EXPECT_EQ(JsonSerialize(ru.value), raw_utf8);
    JsonValue::Destroy(ru.value);
  }
}
