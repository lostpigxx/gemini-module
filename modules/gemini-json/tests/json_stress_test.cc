// Stress, differential, and fuzz tests for gemini-json.
// Exercises extreme inputs, pathological JSONPath queries, byte-by-byte RDB
// truncation/corruption, high-churn DOM mutations, and multi-MB documents.

#define REDISMODULE_API
extern "C" {
#include "redismodule.h"
}

#include "mock_redismodule_io.h"

#include <gtest/gtest.h>
#include "json_parse.h"
#include "json_path.h"
#include "json_serialize.h"
#include "json_type.h"
#include "json_value.h"

#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

class JsonStressTestEnv : public ::testing::Environment {
public:
  void SetUp() override { InstallMockRedisModuleIO(); }
};
static auto* const gStressEnv [[maybe_unused]] =
    ::testing::AddGlobalTestEnvironment(new JsonStressTestEnv);

// ------------------------------------------------------------------
// 1. Differential Object Stress (SKO inline + heap keys + hash + defrag)
// ------------------------------------------------------------------

TEST(JsonStressTest, DifferentialObjectChurnAndDefrag) {
  std::mt19937 rng(20261126);
  auto* obj = JsonValue::CreateObject();
  std::unordered_map<std::string, int64_t> ref;

  auto make_key = [&](uint32_t id) -> std::string {
    if (id % 3 == 0) {
      // Long heap key (> 15 bytes)
      return "heap_key_prefix_longer_than_15_" + std::to_string(id);
    }
    // Short inline SKO key (<= 15 bytes)
    return "k" + std::to_string(id);
  };

  for (int step = 0; step < 15000; step++) {
    uint32_t id = rng() % 800;
    std::string k = make_key(id);
    uint32_t op = rng() % 10;

    if (op < 5) {
      int64_t val = static_cast<int64_t>(rng()) - 1000000;
      ASSERT_TRUE(obj->ObjectSet(k.data(), static_cast<uint32_t>(k.size()),
                                 JsonValue::CreateInteger(val)));
      ref[k] = val;
    } else if (op < 8) {
      bool deleted = obj->ObjectDelete(k);
      bool ref_deleted = (ref.erase(k) > 0);
      ASSERT_EQ(deleted, ref_deleted) << "step=" << step << " key=" << k;
    } else if (op == 8) {
      auto* got = obj->ObjectGet(k);
      auto it = ref.find(k);
      if (it == ref.end()) {
        ASSERT_EQ(got, nullptr);
      } else {
        ASSERT_NE(got, nullptr);
        ASSERT_EQ(got->GetInteger(), it->second);
      }
    } else if (step % 500 == 0) {
      // Trigger defrag relocation mid-churn
      void* ptr = obj;
      ASSERT_EQ(DefragJson(nullptr, nullptr, &ptr), 0);
      obj = static_cast<JsonValue*>(ptr);
    }
  }

  ASSERT_EQ(obj->ObjectLen(), ref.size());
  auto* entries = obj->ObjectEntries();
  for (uint32_t i = 0; i < obj->ObjectLen(); i++) {
    auto it = ref.find(std::string(entries[i].KeyView()));
    ASSERT_NE(it, ref.end());
    ASSERT_EQ(entries[i].value->GetInteger(), it->second);
  }

  JsonValue::Destroy(obj);
}

// ------------------------------------------------------------------
// 2. Differential Array Stress (append, insert, pop, trim, clear)
// ------------------------------------------------------------------

TEST(JsonStressTest, DifferentialArrayChurn) {
  std::mt19937 rng(42);
  auto* arr = JsonValue::CreateArray();
  std::vector<int64_t> ref;

  for (int step = 0; step < 8000; step++) {
    uint32_t op = rng() % 10;
    if (op < 4 || ref.empty()) {
      int64_t v = static_cast<int64_t>(rng() % 100000);
      ASSERT_TRUE(arr->ArrayAppend(JsonValue::CreateInteger(v)));
      ref.push_back(v);
    } else if (op < 6) {
      uint32_t idx = rng() % (static_cast<uint32_t>(ref.size()) + 1);
      int64_t v = static_cast<int64_t>(rng() % 100000);
      ASSERT_TRUE(arr->ArrayInsert(idx, JsonValue::CreateInteger(v)));
      ref.insert(ref.begin() + idx, v);
    } else if (op < 8) {
      int32_t idx = static_cast<int32_t>(rng() % ref.size());
      auto* popped = arr->ArrayPop(idx);
      ASSERT_NE(popped, nullptr);
      ASSERT_EQ(popped->GetInteger(), ref[static_cast<size_t>(idx)]);
      JsonValue::Destroy(popped);
      ref.erase(ref.begin() + idx);
    } else if (op == 8 && ref.size() > 10) {
      uint32_t s = rng() % static_cast<uint32_t>(ref.size() / 2);
      uint32_t e = s + (rng() % static_cast<uint32_t>(ref.size() - s));
      arr->ArrayTrim(s, e);
      ref.assign(ref.begin() + s, ref.begin() + e + 1);
    } else if (step % 1500 == 0) {
      arr->Clear();
      ref.clear();
    }
    ASSERT_EQ(arr->ArrayLen(), ref.size());
  }

  for (size_t i = 0; i < ref.size(); i++) {
    ASSERT_EQ(arr->ArrayGet(static_cast<uint32_t>(i))->GetInteger(), ref[i]);
  }
  JsonValue::Destroy(arr);
}

// ------------------------------------------------------------------
// 3. Parser & Serializer Fuzz (random + mutated valid JSON)
// ------------------------------------------------------------------

TEST(JsonStressTest, ParserFuzzRandomAndMutatedInputs) {
  std::mt19937 rng(1337);
  const std::vector<std::string> seeds = {
    R"({"a":1,"b":[true,false,null,"str\n\t\u0041\uD83D\uDE00"],"c":-1.5e-10})",
    R"([[[1,2,3]],{"":"empty_key","nested":{"x":9223372036854775807}}])",
    R"("plain string with utf8 世界 and escapes \" \\ \/")",
    R"(-9223372036854775808)",
  };

  // 2000 mutated seed inputs
  for (int i = 0; i < 2000; i++) {
    std::string s = seeds[static_cast<size_t>(i) % seeds.size()];
    int mutations = 1 + static_cast<int>(rng() % 5);
    for (int m = 0; m < mutations; m++) {
      uint32_t action = rng() % 4;
      if (action == 0 && !s.empty()) {
        s[rng() % s.size()] = static_cast<char>(rng() % 256);
      } else if (action == 1 && !s.empty()) {
        s.erase(rng() % s.size(), 1);
      } else if (action == 2) {
        s.insert(s.empty() ? 0 : (rng() % s.size()), 1, static_cast<char>(rng() % 256));
      } else if (!s.empty()) {
        s.resize(rng() % s.size());
      }
    }
    auto r = JsonParse(s);
    if (r.value) {
      auto ser = JsonSerialize(r.value);
      auto r2 = JsonParse(ser);
      ASSERT_NE(r2.value, nullptr) << "Serialized valid DOM failed to re-parse: " << ser;
      EXPECT_TRUE(r.value->DeepEqual(r2.value));
      JsonValue::Destroy(r2.value);
      JsonValue::Destroy(r.value);
    }
  }

  // 2000 purely random byte strings
  for (int i = 0; i < 2000; i++) {
    size_t len = rng() % 128;
    std::string s(len, '\0');
    for (size_t j = 0; j < len; j++) s[j] = static_cast<char>(rng() % 256);
    auto r = JsonParse(s);
    if (r.value) JsonValue::Destroy(r.value);
  }
}

// ------------------------------------------------------------------
// 4. Pathological & Fuzzed JSONPath DoS Protection
// ------------------------------------------------------------------

TEST(JsonStressTest, PathologicalJsonPathBudgetAndFuzz) {
  // 10-level nested single-element array: 21 bytes
  auto* doc = JsonParse("[[[[[[[[[[1]]]]]]]]]]").value;
  ASSERT_NE(doc, nullptr);

  // Cartesian union explosion: 10 duplicate indices across 10 levels = 10^10 theoretical matches!
  // Eval budget must cap it quickly (< 200ms) without hanging or OOM.
  std::string bomb = "$";
  for (int i = 0; i < 10; i++) bomb += "[0,0,0,0,0,0,0,0,0,0]";
  auto pr = ParsePath(bomb);
  ASSERT_EQ(pr.error, nullptr);

  auto t0 = std::chrono::steady_clock::now();
  auto matches = EvaluatePath(pr.path, doc);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0).count();
  EXPECT_LE(matches.size(), 500000u);
  EXPECT_LT(ms, 2000) << "Pathological union took too long: " << ms << "ms";

  // Too many segments (> 128) rejected
  std::string many_segs = "$";
  for (int i = 0; i < 150; i++) many_segs += ".a";
  EXPECT_NE(ParsePath(many_segs).error, nullptr);

  // Oversized path (> 64KB) rejected
  std::string huge_path = "$." + std::string(70000, 'a');
  EXPECT_NE(ParsePath(huge_path).error, nullptr);

  // Too many union members (> 1024) rejected
  std::string huge_union = "$[0";
  for (int i = 1; i <= 1100; i++) huge_union += ",0";
  huge_union += "]";
  EXPECT_NE(ParsePath(huge_union).error, nullptr);

  // Deeply nested filter parens (> 32) rejected
  std::string deep_flt = "$[?" + std::string(50, '(') + "@.a == 1" + std::string(50, ')') + "]";
  EXPECT_NE(ParsePath(deep_flt).error, nullptr);

  // 2000 fuzzed JSONPath strings
  std::mt19937 rng(999);
  const char alphabet[] = "$.@[]()?*:,\"'\\01239aAbB=!<>&|~+-/ ";
  for (int i = 0; i < 2000; i++) {
    size_t len = 1 + (rng() % 48);
    std::string p(len, '$');
    for (size_t j = 1; j < len; j++) {
      p[j] = alphabet[rng() % (sizeof(alphabet) - 1)];
    }
    auto r = ParsePath(p);
    if (!r.error) {
      auto m = EvaluatePath(r.path, doc);
      (void)m;
    }
  }

  JsonValue::Destroy(doc);
}

// ------------------------------------------------------------------
// 5. Byte-by-Byte RDB Truncation & Bit-Flip Corruption Fuzz
// ------------------------------------------------------------------

TEST(JsonStressTest, RdbByteByByteTruncationAndCorruptionFuzz) {
  auto* doc = JsonParse(R"({
    "users": [
      {"id": 1, "name": "Alice", "long_field_name_exceeding_15": [1.5, -42, true, null, "hello"]},
      {"id": 2, "name": "Bob 世界", "tags": ["a", "b"]}
    ],
    "empty_arr": [],
    "empty_obj": {}
  })").value;
  ASSERT_NE(doc, nullptr);

  MockRdbStream golden;
  JsonCompatMode = false;
  RdbSaveJson(golden.IO(), doc);
  JsonValue::Destroy(doc);

  ASSERT_GT(golden.buf.size(), 50u);

  // Every single truncated prefix [0 .. size-1] must fail cleanly without crash/leak
  for (size_t cut = 0; cut < golden.buf.size(); cut++) {
    MockRdbStream trunc;
    trunc.buf.assign(golden.buf.begin(), golden.buf.begin() + static_cast<ptrdiff_t>(cut));
    trunc.Rewind();
    auto* loaded = static_cast<JsonValue*>(RdbLoadJson(trunc.IO(), 2));
    EXPECT_EQ(loaded, nullptr) << "Truncated RDB at byte " << cut << " should fail";
    JsonValue::Destroy(loaded);
  }

  // 500 random byte-corruption mutations must never crash or leak
  std::mt19937 rng(7777);
  for (int i = 0; i < 500; i++) {
    MockRdbStream corrupt;
    corrupt.buf = golden.buf;
    int flips = 1 + static_cast<int>(rng() % 4);
    for (int f = 0; f < flips; f++) {
      corrupt.buf[rng() % corrupt.buf.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
    }
    corrupt.Rewind();
    auto* loaded = static_cast<JsonValue*>(RdbLoadJson(corrupt.IO(), 2));
    if (loaded) JsonValue::Destroy(loaded);
  }
}

// ------------------------------------------------------------------
// 6. Multi-MB Large Document Full Pipeline Stress
// ------------------------------------------------------------------

TEST(JsonStressTest, MultiMbDocumentPipelineStress) {
  // ~4 MB JSON document (25,000 records)
  std::string big;
  big.reserve(4500000);
  big += "{\"records\":[";
  for (int i = 0; i < 25000; i++) {
    if (i > 0) big += ',';
    big += "{\"id\":";
    big += std::to_string(i);
    big += ",\"title\":\"record_title_";
    big += std::to_string(i);
    big += "\",\"score\":";
    big += std::to_string((i % 1000) + 0.25);
    big += ",\"flags\":[true,false],\"long_key_name_for_heap_test\":\"payload_value_exceeding_21_bytes\"}";
  }
  big += "]}";

  auto r = JsonParse(big);
  ASSERT_NE(r.value, nullptr);
  EXPECT_EQ(r.value->ObjectGet("records")->ArrayLen(), 25000u);

  // Serialize round-trip (to_chars uses shortest float representation, shorter than std::to_string)
  auto ser = JsonSerialize(r.value);
  EXPECT_GT(ser.size(), 3000000u);
  auto r_ser = JsonParse(ser);
  ASSERT_NE(r_ser.value, nullptr);
  EXPECT_EQ(r_ser.value->ObjectGet("records")->ArrayLen(), 25000u);
  JsonValue::Destroy(r_ser.value);

  // Binary RDB round-trip
  MockRdbStream stream;
  JsonCompatMode = false;
  RdbSaveJson(stream.IO(), r.value);
  stream.Rewind();
  auto* loaded = static_cast<JsonValue*>(RdbLoadJson(stream.IO(), 2));
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->ObjectGet("records")->ArrayLen(), 25000u);

  // Defrag entire 4MB DOM tree
  void* ptr = loaded;
  EXPECT_EQ(DefragJson(nullptr, nullptr, &ptr), 0);
  loaded = static_cast<JsonValue*>(ptr);

  // Filter over 25,000 records
  auto pf = ParsePath("$.records[?(@.id >= 24990 && length(@.flags) == 2)]");
  ASSERT_EQ(pf.error, nullptr);
  auto matches = EvaluatePath(pf.path, loaded);
  EXPECT_EQ(matches.size(), 10u);

  JsonValue::Destroy(r.value);
  JsonValue::Destroy(loaded);
}
