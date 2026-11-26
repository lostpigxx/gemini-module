#define REDISMODULE_API
extern "C" {
#include "redismodule.h"
}

#include "mock_redismodule_io.h"
#include "json_parse.h"
#include "json_path.h"
#include "json_serialize.h"
#include "json_type.h"
#include "json_value.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

template <typename Fn>
static double BenchUs(const char* name, int iters, Fn&& fn) {
  fn();  // warmup
  auto t0 = Clock::now();
  for (int i = 0; i < iters; i++) {
    fn();
  }
  auto t1 = Clock::now();
  double total_us =
      std::chrono::duration<double, std::micro>(t1 - t0).count();
  double avg_us = total_us / iters;
  std::printf("%-38s  %10.2f us/op  (%d iters)\n", name, avg_us, iters);
  return avg_us;
}

template <typename Fn>
static double BenchThroughput(const char* name, size_t bytes, int iters, Fn&& fn) {
  fn();  // warmup
  auto t0 = Clock::now();
  for (int i = 0; i < iters; i++) {
    fn();
  }
  auto t1 = Clock::now();
  double total_s = std::chrono::duration<double>(t1 - t0).count();
  double mb_s = (static_cast<double>(bytes) * iters / (1024.0 * 1024.0)) / total_s;
  double avg_us = (total_s * 1e6) / iters;
  std::printf("%-38s  %10.2f us/op  %8.2f MB/s  (%zu B)\n",
              name, avg_us, mb_s, bytes);
  return mb_s;
}

static std::string GenerateRecordsJson(int num_records) {
  const char* cats[] = {"books", "electronics", "clothing", "home", "sports"};
  std::string out;
  out.reserve(static_cast<size_t>(num_records) * 180 + 64);
  out += "{\"meta\":{\"version\":2,\"source\":\"bench\",\"count\":";
  out += std::to_string(num_records);
  out += "},\"records\":[";
  for (int i = 0; i < num_records; i++) {
    if (i > 0) out += ',';
    out += "{\"id\":";
    out += std::to_string(i);
    out += ",\"name\":\"item_";
    out += std::to_string(i);
    out += "\",\"category\":\"";
    out += cats[i % 5];
    out += "\",\"price\":";
    out += std::to_string((i % 200) + 0.99);
    out += ",\"active\":";
    out += (i % 3 == 0 ? "false" : "true");
    out += ",\"tags\":[\"t1\",\"t2\",\"cat_";
    out += std::to_string(i % 10);
    out += "\"]}";
  }
  out += "]}";
  return out;
}

static std::string GenerateEscapedJson(int num_items) {
  std::string out = "[";
  for (int i = 0; i < num_items; i++) {
    if (i > 0) out += ',';
    out += "\"line1\\nline2\\t\\\"quoted\\\"\\\\path/to/file_";
    out += std::to_string(i);
    out += "\\u0041\"";
  }
  out += "]";
  return out;
}

int main() {
  InstallMockRedisModuleIO();
  volatile size_t sink = 0;

  std::printf("=== gemini-json Benchmark ===\n\n");
  std::printf("sizeof(JsonValue) = %zu bytes\n\n", sizeof(JsonValue));

  // 1. DOM Memory Footprint
  std::printf("--- 1. DOM Memory Footprint ---\n");
  {
    auto* int_arr = JsonValue::CreateArray();
    for (int i = 0; i < 100000; i++) {
      int_arr->ArrayAppend(JsonValue::CreateInteger(i));
    }
    std::printf("%-38s  %10zu KB  (%.1f B/elem)\n",
                "100k int array",
                int_arr->MemoryUsage() / 1024,
                static_cast<double>(int_arr->MemoryUsage()) / 100000.0);
    JsonValue::Destroy(int_arr);

    std::string med_json = GenerateRecordsJson(5000);
    auto r = JsonParse(med_json);
    std::printf("%-38s  %10zu KB  (raw JSON: %zu KB, ratio: %.2fx)\n",
                "5k records doc",
                r.value->MemoryUsage() / 1024,
                med_json.size() / 1024,
                static_cast<double>(r.value->MemoryUsage()) / med_json.size());
    JsonValue::Destroy(r.value);

    auto* wide_obj = JsonValue::CreateObject();
    for (int i = 0; i < 5000; i++) {
      std::string k = "field_" + std::to_string(i);
      wide_obj->ObjectSet(k.data(), static_cast<uint32_t>(k.size()),
                          JsonValue::CreateString("short_val", 9));
    }
    std::printf("%-38s  %10zu KB  (%.1f B/entry)\n\n",
                "5k wide object (short str vals)",
                wide_obj->MemoryUsage() / 1024,
                static_cast<double>(wide_obj->MemoryUsage()) / 5000.0);
    JsonValue::Destroy(wide_obj);
  }

  // 2. Parse & Serialize Throughput
  std::printf("--- 2. Parse & Serialize ---\n");
  std::string small_json =
      R"({"id":12345,"name":"Alice","email":"alice@example.com","active":true,"score":98.5,"roles":["admin","dev"]})";
  std::string med_json = GenerateRecordsJson(300);
  std::string big_json = GenerateRecordsJson(6000);
  std::string esc_json = GenerateEscapedJson(4000);
  std::string longstr_json = "\"" + std::string(262144, 'x') + "\"";  // 256KB clean string

  BenchThroughput("Parse small (~110B)", small_json.size(), 50000, [&]() {
    auto r = JsonParse(small_json);
    sink += r.value ? 1 : 0;
    JsonValue::Destroy(r.value);
  });
  BenchThroughput("Parse medium (~50KB, 300 recs)", med_json.size(), 500, [&]() {
    auto r = JsonParse(med_json);
    sink += r.value ? 1 : 0;
    JsonValue::Destroy(r.value);
  });
  BenchThroughput("Parse large (~1MB, 6k recs)", big_json.size(), 25, [&]() {
    auto r = JsonParse(big_json);
    sink += r.value ? 1 : 0;
    JsonValue::Destroy(r.value);
  });
  BenchThroughput("Parse escaped (~200KB)", esc_json.size(), 100, [&]() {
    auto r = JsonParse(esc_json);
    sink += r.value ? 1 : 0;
    JsonValue::Destroy(r.value);
  });
  BenchThroughput("Parse 256KB clean string (SIMD)", longstr_json.size(), 500, [&]() {
    auto r = JsonParse(longstr_json);
    sink += r.value ? 1 : 0;
    JsonValue::Destroy(r.value);
  });

  auto* small_doc = JsonParse(small_json).value;
  auto* med_doc = JsonParse(med_json).value;
  auto* big_doc = JsonParse(big_json).value;
  auto* esc_doc = JsonParse(esc_json).value;
  auto* longstr_doc = JsonParse(longstr_json).value;

  BenchThroughput("Serialize small (~110B)", small_json.size(), 50000, [&]() {
    auto s = JsonSerialize(small_doc);
    sink += s.size();
  });
  BenchThroughput("Serialize medium (~50KB)", med_json.size(), 500, [&]() {
    auto s = JsonSerialize(med_doc);
    sink += s.size();
  });
  BenchThroughput("Serialize large (~1MB)", big_json.size(), 30, [&]() {
    auto s = JsonSerialize(big_doc);
    sink += s.size();
  });
  BenchThroughput("Serialize escaped (~200KB)", esc_json.size(), 100, [&]() {
    auto s = JsonSerialize(esc_doc);
    sink += s.size();
  });
  BenchThroughput("Serialize 256KB clean string (SIMD)", longstr_json.size(), 500, [&]() {
    auto s = JsonSerialize(longstr_doc);
    sink += s.size();
  });
  JsonValue::Destroy(longstr_doc);
  std::printf("\n");

  // 3. DOM Operations
  std::printf("--- 3. DOM Operations ---\n");
  std::vector<std::string> keys;
  keys.reserve(5000);
  for (int i = 0; i < 5000; i++) keys.push_back("field_" + std::to_string(i));

  BenchUs("Object build (5k keys)", 50, [&]() {
    auto* obj = JsonValue::CreateObject();
    for (int i = 0; i < 5000; i++) {
      obj->ObjectSet(keys[i].data(), static_cast<uint32_t>(keys[i].size()),
                     JsonValue::CreateInteger(i));
    }
    sink += obj->ObjectLen();
    JsonValue::Destroy(obj);
  });

  {
    auto* obj = JsonValue::CreateObject();
    for (int i = 0; i < 5000; i++) {
      obj->ObjectSet(keys[i].data(), static_cast<uint32_t>(keys[i].size()),
                     JsonValue::CreateInteger(i));
    }
    BenchUs("Object lookup (5k hits)", 200, [&]() {
      for (int i = 0; i < 5000; i++) {
        auto* v = obj->ObjectGet(keys[i]);
        sink += v ? 1 : 0;
      }
    });
    JsonValue::Destroy(obj);
  }

  BenchUs("Object delete (1k of 2k keys)", 10, [&]() {
    auto* obj = JsonValue::CreateObject();
    for (int i = 0; i < 2000; i++) {
      obj->ObjectSet(keys[i].data(), static_cast<uint32_t>(keys[i].size()),
                     JsonValue::CreateInteger(i));
    }
    for (int i = 0; i < 1000; i++) {
      obj->ObjectDelete(keys[i * 2]);
    }
    sink += obj->ObjectLen();
    JsonValue::Destroy(obj);
  });

  BenchUs("Clone+Destroy medium (~50KB)", 200, [&]() {
    auto* c = med_doc->Clone();
    sink += c ? 1 : 0;
    JsonValue::Destroy(c);
  });
  std::printf("\n");

  // 4. JSONPath Evaluation
  std::printf("--- 4. JSONPath Evaluation (300 recs) ---\n");
  {
    auto p_dot = ParsePath("$.records[150].name").path;
    auto p_wild = ParsePath("$.records[*].price").path;
    auto p_rec = ParsePath("$..price").path;
    auto p_flt = ParsePath("$.records[?(@.price > 50)]").path;
    auto p_cflt = ParsePath("$.records[?(@.category == 'books' && @.price > 50)]").path;
    auto p_reg = ParsePath("$.records[?(@.name =~ /^item_1[0-9]{2}$/)]").path;
    auto p_fn = ParsePath("$.records[?(length(@.tags) >= 3 && search(@.name, 'item_1'))]").path;

    BenchUs("Path parse + eval $.records[150].name", 50000, [&]() {
      auto pr = ParsePath("$.records[150].name");
      auto m = EvaluatePath(pr.path, med_doc);
      sink += m.size();
    });
    BenchUs("Path cached + eval $.records[150].name", 100000, [&]() {
      const char* err = nullptr;
      const JsonPath* p = ParsePathCached("$.records[150].name", &err);
      auto m = EvaluatePath(*p, med_doc);
      sink += m.size();
    });
    BenchUs("Eval $.records[150].name (preparsed)", 100000, [&]() {
      auto m = EvaluatePath(p_dot, med_doc);
      sink += m.size();
    });
    BenchUs("Eval $.records[*].price", 5000, [&]() {
      auto m = EvaluatePath(p_wild, med_doc);
      sink += m.size();
    });
    BenchUs("Eval $..price (recursive)", 1000, [&]() {
      auto m = EvaluatePath(p_rec, med_doc);
      sink += m.size();
    });
    BenchUs("Eval $.records[?(@.price > 50)]", 5000, [&]() {
      auto m = EvaluatePath(p_flt, med_doc);
      sink += m.size();
    });
    BenchUs("Eval compound filter (&&)", 5000, [&]() {
      auto m = EvaluatePath(p_cflt, med_doc);
      sink += m.size();
    });
    BenchUs("Eval precompiled regex filter", 1000, [&]() {
      auto m = EvaluatePath(p_reg, med_doc);
      sink += m.size();
    });
    BenchUs("Eval RFC9535 length()+search()", 1000, [&]() {
      auto m = EvaluatePath(p_fn, med_doc);
      sink += m.size();
    });
  }
  std::printf("\n");

  // 5. RDB Save & Load
  std::printf("--- 5. RDB Save & Load (~50KB doc) ---\n");
  {
    MockRdbStream bin_stream;
    JsonCompatMode = false;
    BenchUs("RDB Binary Save (encver 2)", 500, [&]() {
      bin_stream.Clear();
      RdbSaveJson(bin_stream.IO(), med_doc);
      sink += bin_stream.buf.size();
    });
    BenchUs("RDB Binary Load (encver 2)", 500, [&]() {
      bin_stream.Rewind();
      auto* v = static_cast<JsonValue*>(RdbLoadJson(bin_stream.IO(), 2));
      sink += v ? 1 : 0;
      JsonValue::Destroy(v);
    });

    MockRdbStream txt_stream;
    JsonCompatMode = true;
    BenchUs("RDB Text Save (encver 3)", 500, [&]() {
      txt_stream.Clear();
      RdbSaveJson(txt_stream.IO(), med_doc);
      sink += txt_stream.buf.size();
    });
    BenchUs("RDB Text Load (encver 3)", 500, [&]() {
      txt_stream.Rewind();
      auto* v = static_cast<JsonValue*>(RdbLoadJson(txt_stream.IO(), 3));
      sink += v ? 1 : 0;
      JsonValue::Destroy(v);
    });
    JsonCompatMode = false;

    // 1MB large doc RDB Binary Save & Load
    MockRdbStream big_bin;
    BenchUs("RDB Binary Save large (~1MB)", 30, [&]() {
      big_bin.Clear();
      RdbSaveJson(big_bin.IO(), big_doc);
      sink += big_bin.buf.size();
    });
    BenchUs("RDB Binary Load large (~1MB)", 30, [&]() {
      big_bin.Rewind();
      auto* v = static_cast<JsonValue*>(RdbLoadJson(big_bin.IO(), 2));
      sink += v ? 1 : 0;
      JsonValue::Destroy(v);
    });
  }

  JsonValue::Destroy(small_doc);
  JsonValue::Destroy(med_doc);
  JsonValue::Destroy(big_doc);
  JsonValue::Destroy(esc_doc);
  (void)sink;
  return 0;
}
