#define REDISMODULE_API
extern "C" {
#include "redismodule.h"
}

#include "mock_redismodule_io.h"

#include <gtest/gtest.h>
#include "search_rdb.h"
#include "search_commands.h"
#include "index_spec.h"
#include "document_store.h"
#include "query_parser.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

class SearchRdbTestEnv : public ::testing::Environment {
 public:
  void SetUp() override { InstallMockRedisModuleIO(); }
};
static auto* const gEnv [[maybe_unused]] =
    ::testing::AddGlobalTestEnvironment(new SearchRdbTestEnv);

static void SaveIndex(MockRdbStream& stream, const std::string& name) {
  auto* name_ptr = new std::string(name);
  RdbSaveSearch(stream.IO(), name_ptr);
  delete name_ptr;
}

static void ClearGlobalIndices() {
  // Erase all entries
  std::vector<std::string> names;
  // Use GetIndexEntry to check existence iteratively... or just use CreateIndexFromRdb pattern
  // Actually we need to clear g_indices. We can do this by erasing known names.
  // For tests, we'll track names manually.
}

// =============================================================
// Round-trip tests
// =============================================================

TEST(SearchRdbTest, RoundTripTagAndNumeric) {
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

  auto* entry = GetIndexEntry("testidx");
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->doc_store.Size(), 3u);

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

  // Verify TAG index rebuilt
  auto* tag_idx = loaded->tag_indices.Get("name");
  ASSERT_NE(tag_idx, nullptr);
  auto shoes = tag_idx->Lookup("shoes");
  EXPECT_EQ(shoes.size(), 2u);

  // Verify NUMERIC index rebuilt
  auto* num_idx = loaded->numeric_indices.Get("price");
  ASSERT_NE(num_idx, nullptr);
  auto range = num_idx->RangeQuery(0, false, 150, false);
  EXPECT_EQ(range.size(), 2u);

  EraseIndexEntry("testidx");
  delete loaded_name;
}

TEST(SearchRdbTest, RoundTripVector) {
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
  EXPECT_EQ(loaded->spec.fields.size(), 2u);
  EXPECT_EQ(loaded->spec.fields[1].type, FieldType::kVector);
  EXPECT_EQ(loaded->spec.fields[1].vector_params.dim, 3u);
  EXPECT_EQ(loaded->spec.fields[1].vector_params.metric, DistanceMetric::kL2);
  EXPECT_EQ(loaded->doc_store.Size(), 2u);

  // Verify vector index rebuilt
  auto* vidx = loaded->vector_indices.Get("embedding");
  ASSERT_NE(vidx, nullptr);
  EXPECT_EQ(vidx->Size(), 2u);

  float query[] = {1.0f, 0.0f, 0.0f};
  auto results = vidx->KnnQuery(query, 1);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].doc_id, "d1");

  EraseIndexEntry("vecidx");
  delete loaded_name;
}

TEST(SearchRdbTest, RoundTripEmptyIndex) {
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

  EraseIndexEntry("emptyidx");
  delete loaded_name;
}

TEST(SearchRdbTest, RoundTripMultipleFieldTypes) {
  IndexSpec spec;
  spec.name = "mixidx";
  VectorFieldParams vp;
  vp.algorithm = VectorAlgorithm::kFlat;
  vp.dim = 2;
  vp.metric = DistanceMetric::kCosine;
  spec.fields = {
      {"tag", FieldType::kTag, {}},
      {"num", FieldType::kNumeric, {}},
      {"vec", FieldType::kVector, vp},
  };

  float vec[] = {0.5f, 0.5f};
  std::string blob(reinterpret_cast<const char*>(vec), sizeof(vec));

  std::vector<std::pair<std::string, std::unordered_map<std::string, std::string>>>
      docs = {{"d1", {{"tag", "a"}, {"num", "42"}, {"vec", blob}}}};

  CreateIndexFromRdb("mixidx", spec, docs);

  MockRdbStream stream;
  SaveIndex(stream, "mixidx");
  stream.Rewind();
  EraseIndexEntry("mixidx");

  auto* loaded_name = static_cast<std::string*>(
      RdbLoadSearch(stream.IO(), kSearchEncVer));
  ASSERT_NE(loaded_name, nullptr);

  auto* loaded = GetIndexEntry("mixidx");
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->spec.fields.size(), 3u);
  EXPECT_EQ(loaded->spec.fields[0].type, FieldType::kTag);
  EXPECT_EQ(loaded->spec.fields[1].type, FieldType::kNumeric);
  EXPECT_EQ(loaded->spec.fields[2].type, FieldType::kVector);
  EXPECT_EQ(loaded->spec.fields[2].vector_params.metric,
            DistanceMetric::kCosine);

  auto* d1 = loaded->doc_store.Get("d1");
  ASSERT_NE(d1, nullptr);
  EXPECT_EQ(d1->fields.at("tag"), "a");
  EXPECT_EQ(d1->fields.at("num"), "42");
  EXPECT_EQ(d1->fields.at("vec").size(), sizeof(vec));

  EraseIndexEntry("mixidx");
  delete loaded_name;
}

TEST(SearchRdbTest, WrongEncverReturnsNull) {
  IndexSpec spec;
  spec.name = "badver";
  spec.fields = {{"f", FieldType::kTag, {}}};
  CreateIndexFromRdb("badver", spec, {});

  MockRdbStream stream;
  SaveIndex(stream, "badver");
  stream.Rewind();

  auto* result = RdbLoadSearch(stream.IO(), 999);
  EXPECT_EQ(result, nullptr);

  EraseIndexEntry("badver");
}
