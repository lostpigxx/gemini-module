#define REDISMODULE_API
extern "C" {
#include "redismodule.h"
}

#include "mock_redismodule_io.h"

#include <gtest/gtest.h>
#include "document_store.h"
#include "index_spec.h"
#include "query_parser.h"
#include "search_commands.h"
#include "search_internal.h"
#include "search_rdb.h"

#include <cmath>
#include <cstdarg>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

// =============================================================
// Minimal local mocks for RedisModuleString + EmitAOF ("v") + CopySearch2
// =============================================================

struct MockRmString {
  std::string data;
};

static std::vector<std::pair<std::string, std::vector<std::string>>> g_emitted_aof;
static MockRmString g_mock_to_key;

static RedisModuleString* Mock_CreateString(RedisModuleCtx* /*ctx*/,
                                            const char* ptr, size_t len) {
  auto* s = new MockRmString{std::string(ptr, len)};
  return reinterpret_cast<RedisModuleString*>(s);
}

static void Mock_FreeString(RedisModuleCtx* /*ctx*/, RedisModuleString* str) {
  delete reinterpret_cast<MockRmString*>(str);
}

static const char* Mock_StringPtrLen(const RedisModuleString* str, size_t* len) {
  const auto* s = reinterpret_cast<const MockRmString*>(str);
  if (len) *len = s->data.size();
  return s->data.c_str();
}

static const RedisModuleString* Mock_GetToKeyNameFromOptCtx(
    RedisModuleKeyOptCtx* /*ctx*/) {
  return reinterpret_cast<const RedisModuleString*>(&g_mock_to_key);
}

static void Mock_EmitAOF(RedisModuleIO* /*io*/, const char* cmdname,
                         const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vector<std::string> args;
  for (const char* p = fmt; *p != '\0'; p++) {
    if (*p == 'v') {
      auto** vec = va_arg(ap, RedisModuleString**);
      size_t len = va_arg(ap, size_t);
      for (size_t i = 0; i < len; i++) {
        size_t slen = 0;
        const char* sp = Mock_StringPtrLen(vec[i], &slen);
        args.emplace_back(sp, slen);
      }
    }
  }
  va_end(ap);
  g_emitted_aof.emplace_back(cmdname, std::move(args));
}

class SearchRdbTestEnv : public ::testing::Environment {
 public:
  void SetUp() override {
    InstallMockRedisModuleIO();
    RedisModule_CreateString = Mock_CreateString;
    RedisModule_FreeString = Mock_FreeString;
    RedisModule_StringPtrLen = Mock_StringPtrLen;
    RedisModule_GetToKeyNameFromOptCtx = Mock_GetToKeyNameFromOptCtx;
    RedisModule_EmitAOF = Mock_EmitAOF;
  }
};
static auto* const gEnv [[maybe_unused]] =
    ::testing::AddGlobalTestEnvironment(new SearchRdbTestEnv);

class SearchRdbTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ClearAllGlobalState();
    g_emitted_aof.clear();
  }
  void TearDown() override {
    ClearAllGlobalState();
    g_emitted_aof.clear();
  }
};

static void SaveIndex(MockRdbStream& stream, const std::string& name) {
  auto* name_ptr = new std::string(name);
  RdbSaveSearch(stream.IO(), name_ptr);
  delete name_ptr;
}

// =============================================================
// Round-trip tests
// =============================================================

TEST_F(SearchRdbTest, RoundTripTagAndNumeric) {
  IndexSpec spec;
  spec.name = "testidx";
  spec.fields = {
      {"name", FieldType::kTag, {}},
      {"price", FieldType::kNumeric, {}},
  };

  std::vector<std::pair<std::string, std::unordered_map<std::string, std::string>>>
      docs = {
          {"doc1", {{"name", "shoes"}, {"price", "100"}}},
          {"doc2", {{"name", "hat"}, {"price", "50"}}},
          {"doc3", {{"name", "shoes"}, {"price", "200"}}},
      };

  CreateIndexFromRdb("testidx", spec, docs);

  MockRdbStream stream;
  SaveIndex(stream, "testidx");
  stream.Rewind();

  EraseIndexEntry("testidx");
  EXPECT_EQ(GetIndexEntry("testidx"), nullptr);

  auto* loaded_name = static_cast<std::string*>(
      RdbLoadSearch(stream.IO(), kSearchEncVer));
  ASSERT_NE(loaded_name, nullptr);
  EXPECT_EQ(*loaded_name, "testidx");

  auto* loaded = GetIndexEntry("testidx");
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->spec.name, "testidx");
  EXPECT_EQ(loaded->spec.fields.size(), 2u);
  EXPECT_EQ(loaded->doc_store.Size(), 3u);

  auto* d1 = loaded->doc_store.Get("doc1");
  ASSERT_NE(d1, nullptr);
  EXPECT_EQ(d1->fields.at("name"), "shoes");
  EXPECT_EQ(d1->fields.at("price"), "100");

  auto* tag_idx = loaded->tag_indices.Get("name");
  ASSERT_NE(tag_idx, nullptr);
  EXPECT_EQ(tag_idx->Lookup("shoes").size(), 2u);

  auto* num_idx = loaded->numeric_indices.Get("price");
  ASSERT_NE(num_idx, nullptr);
  EXPECT_EQ(num_idx->RangeQuery(0, false, 150, false).size(), 2u);

  delete loaded_name;
}

TEST_F(SearchRdbTest, RoundTripVector) {
  IndexSpec spec;
  spec.name = "vecidx";
  VectorFieldParams vp;
  vp.algorithm = VectorAlgorithm::kFlat;
  vp.dim = 3;
  vp.metric = DistanceMetric::kL2;
  spec.fields = {
      {"label", FieldType::kTag, {}},
      {"embedding", FieldType::kVector, vp},
  };

  float v1[] = {1.0f, 0.0f, 0.0f};
  float v2[] = {0.0f, 1.0f, 0.0f};
  std::string blob1(reinterpret_cast<const char*>(v1), sizeof(v1));
  std::string blob2(reinterpret_cast<const char*>(v2), sizeof(v2));

  std::vector<std::pair<std::string, std::unordered_map<std::string, std::string>>>
      docs = {
          {"d1", {{"label", "x"}, {"embedding", blob1}}},
          {"d2", {{"label", "y"}, {"embedding", blob2}}},
      };

  CreateIndexFromRdb("vecidx", spec, docs);

  MockRdbStream stream;
  SaveIndex(stream, "vecidx");
  stream.Rewind();
  EraseIndexEntry("vecidx");

  auto* loaded_name = static_cast<std::string*>(
      RdbLoadSearch(stream.IO(), kSearchEncVer));
  ASSERT_NE(loaded_name, nullptr);

  auto* loaded = GetIndexEntry("vecidx");
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->spec.fields[1].type, FieldType::kVector);
  EXPECT_EQ(loaded->spec.fields[1].vector_params.dim, 3u);
  EXPECT_EQ(loaded->spec.fields[1].vector_params.metric, DistanceMetric::kL2);
  EXPECT_EQ(loaded->doc_store.Size(), 2u);

  auto* vidx = loaded->vector_indices.Get("embedding");
  ASSERT_NE(vidx, nullptr);
  float query[] = {1.0f, 0.0f, 0.0f};
  auto results = vidx->KnnQuery(query, 1);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].doc_id, "d1");

  delete loaded_name;
}

TEST_F(SearchRdbTest, RoundTripEmptyIndex) {
  IndexSpec spec;
  spec.name = "emptyidx";
  spec.fields = {{"tag", FieldType::kTag, {}}};
  CreateIndexFromRdb("emptyidx", spec, {});

  MockRdbStream stream;
  SaveIndex(stream, "emptyidx");
  stream.Rewind();
  EraseIndexEntry("emptyidx");

  auto* loaded_name = static_cast<std::string*>(
      RdbLoadSearch(stream.IO(), kSearchEncVer));
  ASSERT_NE(loaded_name, nullptr);

  auto* loaded = GetIndexEntry("emptyidx");
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->doc_store.Size(), 0u);
  EXPECT_EQ(loaded->spec.fields.size(), 1u);

  delete loaded_name;
}

TEST_F(SearchRdbTest, RoundTripEncver7AllOptionsAndSynonyms) {
  IndexSpec spec;
  spec.name = "fullidx";
  spec.prefixes = {"item:", "prod:"};
  spec.language = "french";
  spec.source_type = IndexSourceType::kJson;
  spec.has_custom_stopwords = true;
  spec.custom_stopwords = {"le", "la"};
  spec.nofreqs = true;
  spec.nooffsets = true;
  spec.nohl = true;
  spec.temporary_ttl = 300;

  FieldSpec f1;
  f1.name = "title";
  f1.type = FieldType::kText;
  f1.json_path = "$.title";
  f1.nostem = true;
  f1.phonetic = "dm:en";
  f1.sortable = true;
  f1.noindex = false;
  f1.weight = 2.5;

  FieldSpec f2;
  f2.name = "notes";
  f2.type = FieldType::kText;
  f2.json_path = "$.notes";
  f2.noindex = true;

  VectorFieldParams vp;
  vp.algorithm = VectorAlgorithm::kHnsw;
  vp.dim = 4;
  vp.metric = DistanceMetric::kCosine;
  vp.m = 24;
  vp.ef_construction = 128;
  FieldSpec f3;
  f3.name = "vec";
  f3.type = FieldType::kVector;
  f3.vector_params = vp;

  spec.fields = {f1, f2, f3};

  CreateIndexFromRdb("fullidx", spec, {
      {"item:1", {{"title", "Jon Smith"}, {"notes", "internal"}}},
  });
  auto* entry = GetIndexEntry("fullidx");
  ASSERT_NE(entry, nullptr);
  entry->synonyms.Update("g1", {"fast", "quick", "rapid"});
  entry->synonyms.Update("g2", {"car", "auto"});

  MockRdbStream stream;
  SaveIndex(stream, "fullidx");
  stream.Rewind();
  EraseIndexEntry("fullidx");

  auto* loaded_name = static_cast<std::string*>(
      RdbLoadSearch(stream.IO(), kSearchEncVer));
  ASSERT_NE(loaded_name, nullptr);
  EXPECT_EQ(*loaded_name, "fullidx");

  auto* loaded = GetIndexEntry("fullidx");
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->spec.prefixes, (std::vector<std::string>{"item:", "prod:"}));
  EXPECT_EQ(loaded->spec.language, "french");
  EXPECT_EQ(loaded->spec.source_type, IndexSourceType::kJson);
  EXPECT_TRUE(loaded->spec.has_custom_stopwords);
  EXPECT_EQ(loaded->spec.custom_stopwords, (std::vector<std::string>{"le", "la"}));
  EXPECT_TRUE(loaded->spec.nofreqs);
  EXPECT_TRUE(loaded->spec.nooffsets);
  EXPECT_TRUE(loaded->spec.nohl);
  EXPECT_EQ(loaded->spec.temporary_ttl, 300);

  ASSERT_EQ(loaded->spec.fields.size(), 3u);
  EXPECT_TRUE(loaded->spec.fields[0].nostem);
  EXPECT_EQ(loaded->spec.fields[0].phonetic, "dm:en");
  EXPECT_TRUE(loaded->spec.fields[0].sortable);
  EXPECT_DOUBLE_EQ(loaded->spec.fields[0].weight, 2.5);
  EXPECT_EQ(loaded->spec.fields[0].json_path, "$.title");
  EXPECT_TRUE(loaded->spec.fields[1].noindex);
  EXPECT_EQ(loaded->spec.fields[2].vector_params.algorithm, VectorAlgorithm::kHnsw);
  EXPECT_EQ(loaded->spec.fields[2].vector_params.m, 24u);
  EXPECT_EQ(loaded->spec.fields[2].vector_params.ef_construction, 128u);

  // Verify phonetic index rebuilt after RDB load
  auto* txt = loaded->text_indices.Get("title");
  ASSERT_NE(txt, nullptr);
  EXPECT_EQ(txt->PhoneticLookup("John").size(), 1u);

  // Verify synonyms preserved
  auto syn_dump = loaded->synonyms.Dump();
  ASSERT_EQ(syn_dump.size(), 2u);
  EXPECT_EQ(syn_dump[0].first, "g1");
  EXPECT_EQ(syn_dump[1].first, "g2");
  EXPECT_EQ(loaded->synonyms.Expand("fast").size(), 2u);

  delete loaded_name;
}

// =============================================================
// Malformed / corrupted RDB rejection tests
// =============================================================

TEST_F(SearchRdbTest, WrongEncverReturnsNull) {
  MockRdbStream stream;
  EXPECT_EQ(RdbLoadSearch(stream.IO(), 0), nullptr);
  EXPECT_EQ(RdbLoadSearch(stream.IO(), 999), nullptr);
}

TEST_F(SearchRdbTest, TruncatedStreamRejected) {
  IndexSpec spec;
  spec.name = "trunc";
  spec.fields = {{"a", FieldType::kTag, {}}};
  CreateIndexFromRdb("trunc", spec, {{"d1", {{"a", "v1"}}}});

  MockRdbStream full;
  SaveIndex(full, "trunc");
  EraseIndexEntry("trunc");

  // Truncate at multiple byte boundaries; every truncated buffer must return nullptr
  for (size_t cut : {size_t{0}, size_t{4}, full.buf.size() / 4,
                     full.buf.size() / 2, full.buf.size() - 1}) {
    MockRdbStream partial;
    partial.buf.assign(full.buf.begin(), full.buf.begin() + static_cast<ptrdiff_t>(cut));
    partial.Rewind();
    EXPECT_EQ(RdbLoadSearch(partial.IO(), kSearchEncVer), nullptr)
        << "Expected nullptr at cut=" << cut;
  }
}

TEST_F(SearchRdbTest, EmptyIndexNameRejected) {
  MockRdbStream stream;
  RedisModule_SaveStringBuffer(stream.IO(), "", 0);
  stream.Rewind();
  EXPECT_EQ(RdbLoadSearch(stream.IO(), kSearchEncVer), nullptr);
}

TEST_F(SearchRdbTest, InvalidEnumValuesRejected) {
  // Invalid source_type = 99
  {
    MockRdbStream s;
    RedisModule_SaveStringBuffer(s.IO(), "badsrc", 6);
    RedisModule_SaveUnsigned(s.IO(), 0);   // prefixes
    RedisModule_SaveUnsigned(s.IO(), 99);  // invalid source_type
    s.Rewind();
    EXPECT_EQ(RdbLoadSearch(s.IO(), kSearchEncVer), nullptr);
  }
  // Invalid FieldType = 42
  {
    MockRdbStream s;
    RedisModule_SaveStringBuffer(s.IO(), "badft", 5);
    RedisModule_SaveUnsigned(s.IO(), 0);   // prefixes
    RedisModule_SaveUnsigned(s.IO(), 0);   // source_type
    RedisModule_SaveUnsigned(s.IO(), 1);   // 1 field
    RedisModule_SaveStringBuffer(s.IO(), "f1", 2);
    RedisModule_SaveUnsigned(s.IO(), 42);  // invalid FieldType
    s.Rewind();
    EXPECT_EQ(RdbLoadSearch(s.IO(), kSearchEncVer), nullptr);
  }
  // Invalid vector dim = 0
  {
    MockRdbStream s;
    RedisModule_SaveStringBuffer(s.IO(), "baddim", 6);
    RedisModule_SaveUnsigned(s.IO(), 0);  // prefixes
    RedisModule_SaveUnsigned(s.IO(), 0);  // source_type
    RedisModule_SaveUnsigned(s.IO(), 1);  // 1 field
    RedisModule_SaveStringBuffer(s.IO(), "v", 1);
    RedisModule_SaveUnsigned(s.IO(), static_cast<uint64_t>(FieldType::kVector));
    RedisModule_SaveUnsigned(s.IO(), static_cast<uint64_t>(VectorAlgorithm::kFlat));
    RedisModule_SaveUnsigned(s.IO(), 0);  // dim = 0 invalid
    RedisModule_SaveUnsigned(s.IO(), static_cast<uint64_t>(DistanceMetric::kL2));
    s.Rewind();
    EXPECT_EQ(RdbLoadSearch(s.IO(), kSearchEncVer), nullptr);
  }
}

TEST_F(SearchRdbTest, NegativeOrNaNWeightRejected) {
  for (double bad_w : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity()}) {
    MockRdbStream s;
    RedisModule_SaveStringBuffer(s.IO(), "badw", 4);
    RedisModule_SaveUnsigned(s.IO(), 0);  // prefixes
    RedisModule_SaveUnsigned(s.IO(), 0);  // source_type
    RedisModule_SaveUnsigned(s.IO(), 1);  // 1 field
    RedisModule_SaveStringBuffer(s.IO(), "t", 1);
    RedisModule_SaveUnsigned(s.IO(), static_cast<uint64_t>(FieldType::kText));
    RedisModule_SaveStringBuffer(s.IO(), "", 0);  // json_path
    RedisModule_SaveUnsigned(s.IO(), 0);          // sortable
    RedisModule_SaveUnsigned(s.IO(), 0);          // noindex
    RedisModule_SaveDouble(s.IO(), bad_w);
    s.Rewind();
    EXPECT_EQ(RdbLoadSearch(s.IO(), kSearchEncVer), nullptr);
  }
}

TEST_F(SearchRdbTest, ExcessivePrefixCountRejected) {
  MockRdbStream s;
  RedisModule_SaveStringBuffer(s.IO(), "bigpref", 7);
  RedisModule_SaveUnsigned(s.IO(), 999999);  // exceeds kMaxRdbPrefixes
  s.Rewind();
  EXPECT_EQ(RdbLoadSearch(s.IO(), kSearchEncVer), nullptr);
}

// =============================================================
// Aux RDB (global state) tests
// =============================================================

TEST_F(SearchRdbTest, AuxRoundTripGlobalState) {
  GetAliases()["prod_alias"] = "idx_prod";
  GetAliases()["dev_alias"] = "idx_dev";

  GetSuggestDicts()["ac"].Add("redis", 10.5, false, "payload1");
  GetSuggestDicts()["ac"].Add("redpanda", 3.0, false, "");

  GetTermDicts()["custom_dict"].Add({"alpha", "beta", "gamma"});

  GetConfig()["DEFAULT_DIALECT"] = "1";
  GetConfig()["TIMEOUT"] = "1500";

  MockRdbStream stream;
  AuxSaveSearch(stream.IO(), REDISMODULE_AUX_AFTER_RDB);
  stream.Rewind();

  ClearAllGlobalState();
  EXPECT_TRUE(GetAliases().empty());
  EXPECT_TRUE(GetSuggestDicts().empty());
  EXPECT_TRUE(GetTermDicts().empty());
  EXPECT_EQ(GetConfig()["DEFAULT_DIALECT"], "2");

  EXPECT_EQ(AuxLoadSearch(stream.IO(), kSearchEncVer, REDISMODULE_AUX_AFTER_RDB),
            REDISMODULE_OK);

  EXPECT_EQ(GetAliases().size(), 2u);
  EXPECT_EQ(GetAliases()["prod_alias"], "idx_prod");
  EXPECT_EQ(GetAliases()["dev_alias"], "idx_dev");

  ASSERT_EQ(GetSuggestDicts().count("ac"), 1u);
  EXPECT_EQ(GetSuggestDicts()["ac"].Len(), 2u);
  auto sugs = GetSuggestDicts()["ac"].Get("red", false, 5);
  ASSERT_EQ(sugs.size(), 2u);
  EXPECT_EQ(sugs[0].str, "redis");
  EXPECT_DOUBLE_EQ(sugs[0].score, 10.5);
  EXPECT_EQ(sugs[0].payload, "payload1");

  ASSERT_EQ(GetTermDicts().count("custom_dict"), 1u);
  EXPECT_TRUE(GetTermDicts()["custom_dict"].Contains("beta"));
  EXPECT_EQ(GetTermDicts()["custom_dict"].Size(), 3u);

  EXPECT_EQ(GetConfig()["DEFAULT_DIALECT"], "1");
  EXPECT_EQ(GetConfig()["TIMEOUT"], "1500");
}

TEST_F(SearchRdbTest, AuxCorruptedStreamIsAtomicAndReturnsErr) {
  GetAliases()["keep_me"] = "idx1";

  MockRdbStream bad;
  RedisModule_SaveUnsigned(bad.IO(), kSearchAuxEncVer);
  RedisModule_SaveUnsigned(bad.IO(), 5);  // claims 5 aliases, but truncated
  bad.Rewind();

  EXPECT_EQ(AuxLoadSearch(bad.IO(), kSearchEncVer, REDISMODULE_AUX_AFTER_RDB),
            REDISMODULE_ERR);
  // Existing global state untouched
  EXPECT_EQ(GetAliases().size(), 1u);
  EXPECT_EQ(GetAliases()["keep_me"], "idx1");
}

TEST_F(SearchRdbTest, AuxWrongVersionRejected) {
  MockRdbStream bad;
  RedisModule_SaveUnsigned(bad.IO(), 999);
  bad.Rewind();
  EXPECT_EQ(AuxLoadSearch(bad.IO(), kSearchEncVer, REDISMODULE_AUX_AFTER_RDB),
            REDISMODULE_ERR);
}

// =============================================================
// AOF rewrite tests (unlimited fields + binary values + synonyms)
// =============================================================

TEST_F(SearchRdbTest, AofRewriteHandlesMoreThanTenFieldsAndBinaryVectors) {
  IndexSpec spec;
  spec.name = "wideidx";
  spec.language = "english";
  VectorFieldParams vp;
  vp.algorithm = VectorAlgorithm::kFlat;
  vp.dim = 2;
  vp.metric = DistanceMetric::kL2;

  for (int i = 0; i < 14; i++) {
    spec.fields.push_back({"f" + std::to_string(i), FieldType::kTag, {}});
  }
  spec.fields.push_back({"vec", FieldType::kVector, vp});

  std::unordered_map<std::string, std::string> doc_fields;
  for (int i = 0; i < 14; i++) {
    doc_fields["f" + std::to_string(i)] = "val" + std::to_string(i);
  }
  float v[] = {0.0f, 1.5f};  // contains NUL bytes
  std::string vblob(reinterpret_cast<const char*>(v), sizeof(v));
  doc_fields["vec"] = vblob;

  CreateIndexFromRdb("wideidx", spec, {{"doc_wide", doc_fields}});
  GetIndexEntry("wideidx")->synonyms.Update("syn1", {"big", "large"});

  MockRmString key{"wideidx"};
  std::string val("wideidx");
  MockRdbStream dummy_aof;
  AofRewriteSearch(dummy_aof.IO(),
                   reinterpret_cast<RedisModuleString*>(&key), &val);

  // Expect FT.CREATE + FT.SYNUPDATE + FT.ADD
  ASSERT_EQ(g_emitted_aof.size(), 3u);
  EXPECT_EQ(g_emitted_aof[0].first, "FT.CREATE");
  EXPECT_EQ(g_emitted_aof[1].first, "FT.SYNUPDATE");
  EXPECT_EQ(g_emitted_aof[2].first, "FT.ADD");

  // FT.ADD args: [wideidx, doc_wide, FIELDS, 15 * (k, v)] = 3 + 30 = 33 args
  const auto& add_args = g_emitted_aof[2].second;
  EXPECT_EQ(add_args.size(), 33u);
  EXPECT_EQ(add_args[0], "wideidx");
  EXPECT_EQ(add_args[1], "doc_wide");
  EXPECT_EQ(add_args[2], "FIELDS");

  // Verify binary vector value preserved intact (including NUL bytes)
  bool found_vec = false;
  for (size_t i = 3; i + 1 < add_args.size(); i += 2) {
    if (add_args[i] == "vec") {
      found_vec = true;
      EXPECT_EQ(add_args[i + 1].size(), sizeof(v));
      EXPECT_EQ(add_args[i + 1], vblob);
    }
  }
  EXPECT_TRUE(found_vec);
}

// =============================================================
// ModuleType callback tests (digest, copy2, free_effort2, defrag, mem_usage)
// =============================================================

TEST_F(SearchRdbTest, DigestDeterministicAndContentSensitive) {
  IndexSpec spec;
  spec.name = "digidx";
  spec.fields = {{"a", FieldType::kTag, {}}};
  CreateIndexFromRdb("digidx", spec, {{"d1", {{"a", "x"}}}});

  std::string key("digidx");
  MockDigest d1, d2, d3;
  DigestSearch(d1.Handle(), &key);
  DigestSearch(d2.Handle(), &key);
  EXPECT_TRUE(d1.ended);
  EXPECT_EQ(d1.bytes, d2.bytes);

  // Mutate doc and verify digest changes
  GetIndexEntry("digidx")->doc_store.Add("d2", {{"a", "y"}});
  DigestSearch(d3.Handle(), &key);
  EXPECT_NE(d1.bytes, d3.bytes);
}

TEST_F(SearchRdbTest, CopySearch2ClonesIndependently) {
  IndexSpec spec;
  spec.name = "src_idx";
  spec.fields = {{"tag", FieldType::kTag, {}}, {"num", FieldType::kNumeric, {}}};
  CreateIndexFromRdb("src_idx", spec, {{"d1", {{"tag", "red"}, {"num", "10"}}}});
  GetIndexEntry("src_idx")->synonyms.Update("g1", {"red", "crimson"});

  g_mock_to_key.data = "dst_idx";
  std::string src_val("src_idx");
  auto* dst_val = static_cast<std::string*>(
      CopySearch2(reinterpret_cast<RedisModuleKeyOptCtx*>(1), &src_val));
  ASSERT_NE(dst_val, nullptr);
  EXPECT_EQ(*dst_val, "dst_idx");

  auto* dst = GetIndexEntry("dst_idx");
  ASSERT_NE(dst, nullptr);
  EXPECT_EQ(dst->spec.name, "dst_idx");
  EXPECT_EQ(dst->doc_store.Size(), 1u);
  EXPECT_EQ(dst->tag_indices.Get("tag")->Lookup("red").size(), 1u);
  EXPECT_EQ(dst->synonyms.Expand("red").size(), 1u);

  // Mutating src does not affect dst
  EraseIndexEntry("src_idx");
  EXPECT_EQ(GetIndexEntry("dst_idx")->doc_store.Size(), 1u);

  delete dst_val;
}

TEST_F(SearchRdbTest, FreeEffortDefragAndMemUsage) {
  IndexSpec spec;
  spec.name = "memidx";
  spec.fields = {{"body", FieldType::kText, {}}};
  CreateIndexFromRdb("memidx", spec, {});

  std::string key("memidx");
  size_t empty_mem = SearchMemUsage(&key);
  EXPECT_GT(empty_mem, 0u);
  EXPECT_EQ(FreeEffortSearch2(nullptr, &key), 1u);

  void* val_ptr = &key;
  EXPECT_EQ(DefragSearch(nullptr, nullptr, &val_ptr), 0);

  // Add documents with sizable payload and confirm mem_usage + free_effort grow
  std::string big_text(4096, 'a');
  auto* entry = GetIndexEntry("memidx");
  entry->doc_store.Add("d1", {{"body", big_text}});
  entry->doc_store.Add("d2", {{"body", big_text}});

  EXPECT_GT(SearchMemUsage(&key), empty_mem + 8192u);
  EXPECT_EQ(SearchMemUsage2(nullptr, &key, 0), SearchMemUsage(&key));
  EXPECT_EQ(FreeEffortSearch2(nullptr, &key), 3u);
}

TEST_F(SearchRdbTest, RoundTripVectorBqAndCustomDictEncver13) {
  IndexSpec spec;
  spec.name = "bqidx";
  spec.language = "english";
  spec.language_field = "lang";
  spec.custom_dict = "my_dict";

  VectorFieldParams vp;
  vp.algorithm = VectorAlgorithm::kHnsw;
  vp.dim = 64;
  vp.metric = DistanceMetric::kCosine;
  vp.m = 16;
  vp.ef_construction = 64;
  vp.quantization = VectorQuantization::kBq;
  spec.fields = {
      {"title", FieldType::kText, {}},
      {"vec", FieldType::kVector, vp},
  };

  std::vector<float> v1(64, 1.0f);
  std::vector<float> v2(64, -1.0f);
  std::string b1(reinterpret_cast<const char*>(v1.data()), v1.size() * sizeof(float));
  std::string b2(reinterpret_cast<const char*>(v2.data()), v2.size() * sizeof(float));

  CreateIndexFromRdb("bqidx", spec, {
      {"d1", {{"title", "positive"}, {"vec", b1}}},
      {"d2", {{"title", "negative"}, {"vec", b2}}},
  });

  // Verify AOF rewrite includes DICT, LANGUAGE_FIELD, and QUANTIZATION BQ
  MockRmString key{"bqidx"};
  std::string val("bqidx");
  MockRdbStream dummy_aof;
  AofRewriteSearch(dummy_aof.IO(), reinterpret_cast<RedisModuleString*>(&key), &val);
  ASSERT_GE(g_emitted_aof.size(), 1u);
  const auto& create_args = g_emitted_aof[0].second;
  auto has_pair = [&](const std::string& k, const std::string& v) {
    for (size_t i = 0; i + 1 < create_args.size(); i++) {
      if (create_args[i] == k && create_args[i + 1] == v) return true;
    }
    return false;
  };
  EXPECT_TRUE(has_pair("DICT", "my_dict"));
  EXPECT_TRUE(has_pair("LANGUAGE_FIELD", "lang"));
  EXPECT_TRUE(has_pair("QUANTIZATION", "BQ"));

  // Save and reload via RDB encver 13
  MockRdbStream stream;
  SaveIndex(stream, "bqidx");
  stream.Rewind();
  EraseIndexEntry("bqidx");

  auto* loaded_name = static_cast<std::string*>(RdbLoadSearch(stream.IO(), kSearchEncVer));
  ASSERT_NE(loaded_name, nullptr);
  EXPECT_EQ(*loaded_name, "bqidx");

  auto* loaded = GetIndexEntry("bqidx");
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->spec.custom_dict, "my_dict");
  EXPECT_EQ(loaded->spec.language_field, "lang");
  EXPECT_EQ(loaded->spec.fields[1].vector_params.quantization, VectorQuantization::kBq);

  auto* vidx = loaded->vector_indices.Get("vec");
  ASSERT_NE(vidx, nullptr);
  auto res = vidx->KnnQuery(v1.data(), 1);
  ASSERT_EQ(res.size(), 1u);
  EXPECT_EQ(res[0].doc_id, "d1");

  // MemUsage2 for BQ should be strictly smaller than an equivalent FP32 index
  size_t bq_mem = SearchMemUsage2(nullptr, loaded_name, 0);
  EXPECT_GT(bq_mem, 0u);
  delete loaded_name;

  spec.name = "fp32idx";
  spec.fields[1].vector_params.quantization = VectorQuantization::kNone;
  CreateIndexFromRdb("fp32idx", spec, {
      {"d1", {{"title", "positive"}, {"vec", b1}}},
      {"d2", {{"title", "negative"}, {"vec", b2}}},
  });
  std::string fp32_key("fp32idx");
  size_t fp32_mem = SearchMemUsage2(nullptr, &fp32_key, 0);
  EXPECT_LT(bq_mem, fp32_mem);
}

TEST_F(SearchRdbTest, RdbByteByByteTruncationAndBitFlipFuzz) {
  IndexSpec spec;
  spec.name = "fuzzidx";
  spec.prefixes = {"f:"};
  spec.language = "english";
  spec.language_field = "lang";
  spec.custom_dict = "dict1";
  spec.has_custom_stopwords = true;
  spec.custom_stopwords = {"the"};

  VectorFieldParams vp;
  vp.algorithm = VectorAlgorithm::kHnsw;
  vp.dim = 8;
  vp.metric = DistanceMetric::kL2;
  vp.quantization = VectorQuantization::kBq;

  spec.fields = {
      {"title", FieldType::kText, {}},
      {"tag", FieldType::kTag, {}},
      {"price", FieldType::kNumeric, {}},
      {"vec", FieldType::kVector, vp},
  };

  float v[8] = {1, -1, 2, -2, 3, -3, 4, -4};
  std::string vblob(reinterpret_cast<const char*>(v), sizeof(v));
  CreateIndexFromRdb("fuzzidx", spec, {
      {"f:1", {{"title", "hello world"}, {"tag", "a,b"}, {"price", "42.5"}, {"vec", vblob}}},
  });
  GetIndexEntry("fuzzidx")->doc_store.SetPayload("f:1", "payload_bytes");
  GetIndexEntry("fuzzidx")->synonyms.Update("g1", {"hello", "hi"});

  MockRdbStream full;
  SaveIndex(full, "fuzzidx");
  EraseIndexEntry("fuzzidx");

  // Every single byte truncation offset must fail cleanly with nullptr
  for (size_t cut = 0; cut < full.buf.size(); cut++) {
    MockRdbStream partial;
    partial.buf.assign(full.buf.begin(), full.buf.begin() + static_cast<ptrdiff_t>(cut));
    partial.Rewind();
    void* res = RdbLoadSearch(partial.IO(), kSearchEncVer);
    if (res) {
      auto* s = static_cast<std::string*>(res);
      EraseIndexEntry(*s);
      delete s;
      FAIL() << "Expected nullptr for truncated stream at cut=" << cut;
    }
  }

  // Deterministic single-byte bit-flip fuzzing across the buffer
  for (size_t pos = 0; pos < full.buf.size(); pos += 3) {
    MockRdbStream corrupted;
    corrupted.buf = full.buf;
    corrupted.buf[pos] ^= 0xFF;
    corrupted.Rewind();
    void* res = RdbLoadSearch(corrupted.IO(), kSearchEncVer);
    if (res) {
      auto* s = static_cast<std::string*>(res);
      EraseIndexEntry(*s);
      delete s;
    }
  }
}

TEST_F(SearchRdbTest, MultiValueNumericIndexingAndRdbRoundTrip) {
  IndexSpec spec;
  spec.name = "mvnum";
  FieldSpec f;
  f.name = "prices";
  f.type = FieldType::kNumeric;
  f.sortable = true;
  spec.fields = {f};

  CreateIndexFromRdb("mvnum", spec, {
      {"d1", {{"prices", "10,50,100"}}},
      {"d2", {{"prices", "25"}}},
  });

  auto* entry = GetIndexEntry("mvnum");
  ASSERT_NE(entry, nullptr);
  auto* nidx = entry->numeric_indices.Get("prices");
  ASSERT_NE(nidx, nullptr);
  EXPECT_EQ(nidx->RangeQuery(9, false, 11, false), (std::vector<std::string>{"d1"}));
  EXPECT_EQ(nidx->RangeQuery(49, false, 51, false), (std::vector<std::string>{"d1"}));
  EXPECT_EQ(nidx->RangeQuery(99, false, 101, false), (std::vector<std::string>{"d1"}));
  EXPECT_EQ(nidx->RangeQuery(0, false, 200, false), (std::vector<std::string>{"d1", "d2"}));

  // RDB round-trip preserves all multi-value numeric entries
  MockRdbStream stream;
  SaveIndex(stream, "mvnum");
  stream.Rewind();
  EraseIndexEntry("mvnum");

  auto* loaded_name = static_cast<std::string*>(RdbLoadSearch(stream.IO(), kSearchEncVer));
  ASSERT_NE(loaded_name, nullptr);
  auto* loaded = GetIndexEntry("mvnum");
  ASSERT_NE(loaded, nullptr);
  auto* nidx2 = loaded->numeric_indices.Get("prices");
  ASSERT_NE(nidx2, nullptr);
  EXPECT_EQ(nidx2->RangeQuery(99, false, 101, false), (std::vector<std::string>{"d1"}));

  // Removing doc cleans up all multi-value numeric entries
  RemoveDocFromIndices(*loaded, "d1");
  loaded->doc_store.Remove("d1");
  EXPECT_TRUE(nidx2->RangeQuery(9, false, 11, false).empty());
  EXPECT_TRUE(nidx2->RangeQuery(49, false, 51, false).empty());
  EXPECT_TRUE(nidx2->RangeQuery(99, false, 101, false).empty());
  EXPECT_EQ(nidx2->RangeQuery(0, false, 200, false), (std::vector<std::string>{"d2"}));

  delete loaded_name;
}

TEST_F(SearchRdbTest, RoundTripEncver14HnswDirectPersistence) {
  constexpr size_t kDim = 16;
  constexpr size_t kDocs = 60;

  IndexSpec spec;
  spec.name = "hnsw14";
  spec.prefixes = {"h:"};

  VectorFieldParams vp_fp32;
  vp_fp32.algorithm = VectorAlgorithm::kHnsw;
  vp_fp32.dim = kDim;
  vp_fp32.metric = DistanceMetric::kL2;
  vp_fp32.m = 12;
  vp_fp32.ef_construction = 80;
  vp_fp32.quantization = VectorQuantization::kNone;

  VectorFieldParams vp_sq8 = vp_fp32;
  vp_sq8.metric = DistanceMetric::kCosine;
  vp_sq8.quantization = VectorQuantization::kSq8;

  VectorFieldParams vp_bq = vp_fp32;
  vp_bq.metric = DistanceMetric::kCosine;
  vp_bq.quantization = VectorQuantization::kBq;

  spec.fields = {
      {"tag", FieldType::kTag, {}},
      {"v_fp32", FieldType::kVector, vp_fp32},
      {"v_sq8", FieldType::kVector, vp_sq8},
      {"v_bq", FieldType::kVector, vp_bq},
  };

  std::vector<std::pair<std::string, std::unordered_map<std::string, std::string>>> docs;
  std::vector<float> vec(kDim);
  for (size_t i = 0; i < kDocs; i++) {
    for (size_t d = 0; d < kDim; d++) {
      vec[d] = std::sin(static_cast<float>(i * 7 + d * 3)) * 2.0f;
    }
    std::string blob(reinterpret_cast<const char*>(vec.data()), kDim * sizeof(float));
    docs.push_back({"h:" + std::to_string(i),
                    {{"tag", i % 2 == 0 ? "even" : "odd"},
                     {"v_fp32", blob},
                     {"v_sq8", blob},
                     {"v_bq", blob}}});
  }

  ASSERT_TRUE(CreateIndexFromRdb("hnsw14", spec, docs));
  auto* orig = GetIndexEntry("hnsw14");
  ASSERT_NE(orig, nullptr);

  // Remove a few docs before save to test tombstone compaction
  for (size_t i = 0; i < kDocs; i += 10) {
    std::string id = "h:" + std::to_string(i);
    RemoveDocFromIndices(*orig, id);
    orig->doc_store.Remove(id);
  }

  std::vector<float> query(kDim);
  for (size_t d = 0; d < kDim; d++) query[d] = std::cos(static_cast<float>(d + 1));

  auto orig_fp32 = orig->vector_indices.Get("v_fp32")->KnnQueryEf(query.data(), 10, 32);
  auto orig_sq8 = orig->vector_indices.Get("v_sq8")->KnnQueryEf(query.data(), 10, 32);
  auto orig_bq = orig->vector_indices.Get("v_bq")->KnnQueryEf(query.data(), 10, 32);
  ASSERT_EQ(orig_fp32.size(), 10u);

  MockRdbStream stream;
  SaveIndex(stream, "hnsw14");
  stream.Rewind();
  EraseIndexEntry("hnsw14");

  auto* loaded_name = static_cast<std::string*>(RdbLoadSearch(stream.IO(), kSearchEncVer));
  ASSERT_NE(loaded_name, nullptr);
  EXPECT_EQ(*loaded_name, "hnsw14");

  auto* loaded = GetIndexEntry("hnsw14");
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->doc_store.Size(), kDocs - kDocs / 10);

  auto new_fp32 = loaded->vector_indices.Get("v_fp32")->KnnQueryEf(query.data(), 10, 32);
  auto new_sq8 = loaded->vector_indices.Get("v_sq8")->KnnQueryEf(query.data(), 10, 32);
  auto new_bq = loaded->vector_indices.Get("v_bq")->KnnQueryEf(query.data(), 10, 32);

  ASSERT_EQ(new_fp32.size(), orig_fp32.size());
  ASSERT_EQ(new_sq8.size(), orig_sq8.size());
  ASSERT_EQ(new_bq.size(), orig_bq.size());
  for (size_t i = 0; i < 10; i++) {
    EXPECT_EQ(new_fp32[i].doc_id, orig_fp32[i].doc_id);
    EXPECT_FLOAT_EQ(new_fp32[i].score, orig_fp32[i].score);
    EXPECT_EQ(new_sq8[i].doc_id, orig_sq8[i].doc_id);
    EXPECT_FLOAT_EQ(new_sq8[i].score, orig_sq8[i].score);
    EXPECT_EQ(new_bq[i].doc_id, orig_bq[i].doc_id);
    EXPECT_FLOAT_EQ(new_bq[i].score, orig_bq[i].score);
  }

  delete loaded_name;
}
