#include <gtest/gtest.h>
#include "document_store.h"
#include "index_spec.h"
#include "numeric_index.h"
#include "tag_index.h"
#include "query_parser.h"
#include "vector_index.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// =============================================================
// Stress tests — ASAN catches leaks, double-free, use-after-free
// =============================================================

TEST(AsanStressTest, RepeatedCreateDrop) {
  IndexRegistry reg;
  for (int i = 0; i < 1000; i++) {
    auto name = "idx_" + std::to_string(i);
    ASSERT_TRUE(reg.Create(name, {{"f", FieldType::kTag}}));
  }
  EXPECT_EQ(reg.Size(), 1000u);

  for (int i = 0; i < 1000; i++) {
    auto name = "idx_" + std::to_string(i);
    ASSERT_TRUE(reg.Drop(name));
  }
  EXPECT_EQ(reg.Size(), 0u);
}

TEST(AsanStressTest, CreateDropCycles) {
  IndexRegistry reg;
  for (int round = 0; round < 500; round++) {
    auto name = "cycle_idx";
    std::vector<FieldSpec> fields;
    for (int f = 0; f < 5; f++) {
      fields.push_back({"field_" + std::to_string(f),
                         f % 2 == 0 ? FieldType::kTag : FieldType::kNumeric});
    }
    ASSERT_TRUE(reg.Create(name, fields));
    auto* spec = reg.Get(name);
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->fields.size(), 5u);
    ASSERT_TRUE(reg.Drop(name));
  }
  EXPECT_EQ(reg.Size(), 0u);
}

TEST(AsanStressTest, ManyFieldsPerIndex) {
  IndexRegistry reg;
  std::vector<FieldSpec> fields;
  for (int i = 0; i < 500; i++) {
    fields.push_back({"field_" + std::to_string(i),
                       i % 2 == 0 ? FieldType::kTag : FieldType::kNumeric});
  }
  ASSERT_TRUE(reg.Create("wide_index", fields));
  auto* spec = reg.Get("wide_index");
  ASSERT_NE(spec, nullptr);
  EXPECT_EQ(spec->fields.size(), 500u);

  for (int i = 0; i < 500; i++) {
    auto* f = spec->FindField("field_" + std::to_string(i));
    ASSERT_NE(f, nullptr);
  }
}

TEST(AsanStressTest, LongNames) {
  IndexRegistry reg;
  std::string long_index_name(10000, 'i');
  std::string long_field_name(10000, 'f');

  ASSERT_TRUE(
      reg.Create(long_index_name, {{long_field_name, FieldType::kTag}}));
  auto* spec = reg.Get(long_index_name);
  ASSERT_NE(spec, nullptr);
  EXPECT_EQ(spec->name.size(), 10000u);
  ASSERT_EQ(spec->fields.size(), 1u);
  EXPECT_EQ(spec->fields[0].name.size(), 10000u);

  ASSERT_TRUE(reg.Drop(long_index_name));
}

TEST(AsanStressTest, InterleavedCreateDropDifferentIndices) {
  IndexRegistry reg;
  for (int i = 0; i < 1000; i++) {
    auto name = "idx_" + std::to_string(i);
    reg.Create(name, {{"a", FieldType::kTag}, {"b", FieldType::kNumeric}});
    if (i >= 100) {
      auto old_name = "idx_" + std::to_string(i - 100);
      ASSERT_TRUE(reg.Drop(old_name));
    }
  }
  EXPECT_EQ(reg.Size(), 100u);
  reg.Clear();
  EXPECT_EQ(reg.Size(), 0u);
}

TEST(AsanStressTest, ListUnderChurn) {
  IndexRegistry reg;
  for (int i = 0; i < 200; i++) {
    reg.Create("idx_" + std::to_string(i), {{"f", FieldType::kTag}});
  }
  for (int i = 0; i < 100; i++) {
    reg.Drop("idx_" + std::to_string(i));
  }
  auto names = reg.List();
  EXPECT_EQ(names.size(), 100u);
  for (auto& n : names) {
    EXPECT_NE(reg.Get(n), nullptr);
  }
}

TEST(AsanStressTest, ClearAndReuse) {
  IndexRegistry reg;
  for (int round = 0; round < 100; round++) {
    for (int i = 0; i < 50; i++) {
      reg.Create("r" + std::to_string(round) + "_i" + std::to_string(i),
                 {{"x", FieldType::kTag}});
    }
    reg.Clear();
    EXPECT_EQ(reg.Size(), 0u);
  }
}

// =============================================================
// Ownership / move semantics
// =============================================================

TEST(AsanOwnershipTest, CreateMovesFields) {
  IndexRegistry reg;
  std::vector<FieldSpec> fields = {{"a", FieldType::kTag},
                                    {"b", FieldType::kNumeric}};
  reg.Create("idx", std::move(fields));
  // fields should be moved-from — don't rely on its contents
  auto* spec = reg.Get("idx");
  ASSERT_NE(spec, nullptr);
  EXPECT_EQ(spec->fields.size(), 2u);
}

TEST(AsanOwnershipTest, DropFreesStrings) {
  IndexRegistry reg;
  for (int i = 0; i < 100; i++) {
    std::string name = "index_with_a_fairly_long_name_" + std::to_string(i);
    std::vector<FieldSpec> fields;
    for (int j = 0; j < 10; j++) {
      fields.push_back(
          {"field_with_long_name_" + std::to_string(j), FieldType::kTag});
    }
    reg.Create(name, std::move(fields));
  }
  for (int i = 0; i < 100; i++) {
    std::string name = "index_with_a_fairly_long_name_" + std::to_string(i);
    ASSERT_TRUE(reg.Drop(name));
  }
  EXPECT_EQ(reg.Size(), 0u);
}

// =============================================================
// Boundary conditions
// =============================================================

TEST(AsanBoundaryTest, EmptyIndexName) {
  IndexRegistry reg;
  EXPECT_TRUE(reg.Create("", {{"a", FieldType::kTag}}));
  auto* spec = reg.Get("");
  ASSERT_NE(spec, nullptr);
  EXPECT_EQ(spec->name, "");
  EXPECT_TRUE(reg.Drop(""));
}

TEST(AsanBoundaryTest, EmptyFieldName) {
  IndexRegistry reg;
  EXPECT_TRUE(reg.Create("idx", {{"", FieldType::kTag}}));
  auto* spec = reg.Get("idx");
  ASSERT_NE(spec, nullptr);
  auto* f = spec->FindField("");
  ASSERT_NE(f, nullptr);
}

TEST(AsanBoundaryTest, FindFieldInLargeSchema) {
  IndexRegistry reg;
  std::vector<FieldSpec> fields;
  for (int i = 0; i < 1000; i++) {
    fields.push_back({"field_" + std::to_string(i), FieldType::kTag});
  }
  reg.Create("big", std::move(fields));
  auto* spec = reg.Get("big");
  ASSERT_NE(spec, nullptr);

  EXPECT_NE(spec->FindField("field_0"), nullptr);
  EXPECT_NE(spec->FindField("field_500"), nullptr);
  EXPECT_NE(spec->FindField("field_999"), nullptr);
  EXPECT_EQ(spec->FindField("field_1000"), nullptr);
}

// =============================================================
// Phase 2: DocumentStore ASAN tests
// =============================================================

TEST(AsanDocStoreStress, RepeatedAddRemove) {
  DocumentStore store;
  for (int i = 0; i < 1000; i++) {
    auto id = "doc_" + std::to_string(i);
    store.Add(id, {{"f1", "val_" + std::to_string(i)}, {"f2", "data"}});
  }
  EXPECT_EQ(store.Size(), 1000u);
  for (int i = 0; i < 1000; i++) {
    store.Remove("doc_" + std::to_string(i));
  }
  EXPECT_EQ(store.Size(), 0u);
}

TEST(AsanDocStoreStress, RepeatedReplace) {
  DocumentStore store;
  for (int i = 0; i < 500; i++) {
    std::unordered_map<std::string, std::string> fields;
    for (int j = 0; j < 10; j++) {
      fields["field_" + std::to_string(j)] = "round_" + std::to_string(i);
    }
    store.Add("doc1", std::move(fields));
  }
  EXPECT_EQ(store.Size(), 1u);
  auto* doc = store.Get("doc1");
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(doc->fields.at("field_0"), "round_499");
}

TEST(AsanDocStoreStress, LargeFieldValues) {
  DocumentStore store;
  std::string big_val(100000, 'x');
  store.Add("doc1", {{"data", big_val}});
  auto* doc = store.Get("doc1");
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(doc->fields.at("data").size(), 100000u);
  store.Remove("doc1");
}

TEST(AsanDocStoreStress, ClearAndReuse) {
  DocumentStore store;
  for (int round = 0; round < 100; round++) {
    for (int i = 0; i < 50; i++) {
      store.Add("doc_" + std::to_string(i), {{"r", std::to_string(round)}});
    }
    store.Clear();
    EXPECT_EQ(store.Size(), 0u);
  }
}

// =============================================================
// Phase 2: TagIndex ASAN tests
// =============================================================

TEST(AsanTagIndexStress, ManyTagValues) {
  TagIndex idx;
  for (int i = 0; i < 1000; i++) {
    idx.Add("tag_" + std::to_string(i), "doc1");
  }
  EXPECT_EQ(idx.NumTags(), 1000u);
  idx.Clear();
  EXPECT_EQ(idx.NumTags(), 0u);
}

TEST(AsanTagIndexStress, ManyDocsPerTag) {
  TagIndex idx;
  for (int i = 0; i < 1000; i++) {
    idx.Add("common_tag", "doc_" + std::to_string(i));
  }
  auto results = idx.Lookup("common_tag");
  EXPECT_EQ(results.size(), 1000u);
}

TEST(AsanTagIndexStress, InterleavedAddRemove) {
  TagIndex idx;
  for (int i = 0; i < 1000; i++) {
    idx.Add("tag", "doc_" + std::to_string(i));
    if (i >= 100) {
      idx.Remove("tag", "doc_" + std::to_string(i - 100));
    }
  }
  auto results = idx.Lookup("tag");
  EXPECT_EQ(results.size(), 100u);
}

TEST(AsanTagIndexStress, LookupOrManyValues) {
  TagIndex idx;
  for (int i = 0; i < 100; i++) {
    idx.Add("tag_" + std::to_string(i), "doc_" + std::to_string(i));
  }
  std::vector<std::string> values;
  for (int i = 0; i < 100; i++) {
    values.push_back("tag_" + std::to_string(i));
  }
  auto results = idx.LookupOr(values);
  EXPECT_EQ(results.size(), 100u);
}

TEST(AsanTagFieldIndicesStress, ManyFieldsClearCycle) {
  TagFieldIndices indices;
  for (int round = 0; round < 50; round++) {
    for (int f = 0; f < 20; f++) {
      auto& idx = indices.GetOrCreate("field_" + std::to_string(f));
      for (int d = 0; d < 10; d++) {
        idx.Add("val_" + std::to_string(d), "doc_" + std::to_string(d));
      }
    }
    indices.Clear();
  }
}

// =============================================================
// Phase 2: TagQuery ASAN tests
// =============================================================

TEST(AsanTagQueryStress, ParseManyPipeValues) {
  std::string query = "@field:{v0";
  for (int i = 1; i < 200; i++) {
    query += "|v" + std::to_string(i);
  }
  query += "}";

  ParsedQuery q;
  std::string err;
  ASSERT_TRUE(ParseQuery(query, q, err));
  EXPECT_EQ(q.root.tag_values.size(), 200u);
}

TEST(AsanTagQueryStress, RepeatedParseCycles) {
  for (int i = 0; i < 1000; i++) {
    ParsedQuery q;
    std::string err;
    ParseQuery("@field:{val_" + std::to_string(i) + "}", q, err);
  }
}

// =============================================================
// Phase 3: NumericIndex ASAN tests
// =============================================================

TEST(AsanNumericIndexStress, RepeatedAddRemove) {
  NumericIndex idx;
  for (int i = 0; i < 1000; i++) {
    idx.Add(static_cast<double>(i), "doc_" + std::to_string(i));
  }
  EXPECT_EQ(idx.Size(), 1000u);
  for (int i = 0; i < 1000; i++) {
    idx.Remove(static_cast<double>(i), "doc_" + std::to_string(i));
  }
  EXPECT_EQ(idx.Size(), 0u);
}

TEST(AsanNumericIndexStress, ManyDocsAtSameValue) {
  NumericIndex idx;
  for (int i = 0; i < 1000; i++) {
    idx.Add(42.0, "doc_" + std::to_string(i));
  }
  auto results = idx.RangeQuery(42.0, false, 42.0, false);
  EXPECT_EQ(results.size(), 1000u);
  idx.Clear();
}

TEST(AsanNumericIndexStress, LargeRangeScan) {
  NumericIndex idx;
  for (int i = 0; i < 10000; i++) {
    idx.Add(static_cast<double>(i), "doc_" + std::to_string(i));
  }
  auto results = idx.RangeQuery(0, false, 9999, false);
  EXPECT_EQ(results.size(), 10000u);
}

TEST(AsanNumericIndexStress, InterleavedAddRemoveWithQuery) {
  NumericIndex idx;
  for (int i = 0; i < 1000; i++) {
    idx.Add(static_cast<double>(i), "doc_" + std::to_string(i));
    if (i >= 100) {
      idx.Remove(static_cast<double>(i - 100),
                 "doc_" + std::to_string(i - 100));
    }
    if (i % 50 == 0) {
      auto r = idx.RangeQuery(0, false, 1e9, false);
      (void)r;
    }
  }
  EXPECT_EQ(idx.Size(), 100u);
}

TEST(AsanNumericFieldIndicesStress, ClearCycle) {
  NumericFieldIndices indices;
  for (int round = 0; round < 50; round++) {
    for (int f = 0; f < 10; f++) {
      auto& idx = indices.GetOrCreate("field_" + std::to_string(f));
      for (int d = 0; d < 20; d++) {
        idx.Add(static_cast<double>(d), "doc_" + std::to_string(d));
      }
    }
    indices.Clear();
  }
}

TEST(AsanNumericQueryStress, RepeatedParseCycles) {
  for (int i = 0; i < 1000; i++) {
    ParsedQuery q;
    std::string err;
    ParseQuery("@price:[" + std::to_string(i) + " " +
                      std::to_string(i + 100) + "]",
                  q, err);
  }
}

// =============================================================
// Phase 4: FlatVectorIndex ASAN tests
// =============================================================

TEST(AsanVectorStress, RepeatedAddRemove) {
  FlatVectorIndex idx(128, DistanceMetric::kL2);
  std::vector<float> vec(128, 0.0f);
  for (int i = 0; i < 1000; i++) {
    vec[0] = static_cast<float>(i);
    idx.Add("doc_" + std::to_string(i), vec.data());
  }
  EXPECT_EQ(idx.Size(), 1000u);
  for (int i = 0; i < 1000; i++) {
    idx.Remove("doc_" + std::to_string(i));
  }
  EXPECT_EQ(idx.Size(), 0u);
}

TEST(AsanVectorStress, LargeDimension) {
  FlatVectorIndex idx(1024, DistanceMetric::kCosine);
  std::vector<float> vec(1024);
  for (int i = 0; i < 100; i++) {
    for (size_t j = 0; j < 1024; j++) {
      vec[j] = static_cast<float>(i * 1024 + j) * 0.001f;
    }
    idx.Add("doc_" + std::to_string(i), vec.data());
  }
  std::vector<float> query(1024, 0.5f);
  auto results = idx.KnnQuery(query.data(), 10);
  EXPECT_EQ(results.size(), 10u);
}

TEST(AsanVectorStress, ManyVectorsKnn) {
  FlatVectorIndex idx(4, DistanceMetric::kL2);
  for (int i = 0; i < 5000; i++) {
    float v[] = {static_cast<float>(i), 0, 0, 0};
    idx.Add("doc_" + std::to_string(i), v);
  }
  float query[] = {2500, 0, 0, 0};
  auto results = idx.KnnQuery(query, 10);
  EXPECT_EQ(results.size(), 10u);
}

TEST(AsanVectorStress, KnnEdgeCases) {
  FlatVectorIndex idx(2, DistanceMetric::kL2);
  float v[] = {1, 1};
  idx.Add("only", v);

  float query[] = {0, 0};
  auto r0 = idx.KnnQuery(query, 0);
  EXPECT_TRUE(r0.empty());

  auto r1 = idx.KnnQuery(query, 1);
  EXPECT_EQ(r1.size(), 1u);

  auto r_big = idx.KnnQuery(query, 1000);
  EXPECT_EQ(r_big.size(), 1u);
}

TEST(AsanVectorStress, ClearAndReuse) {
  FlatVectorIndex idx(3, DistanceMetric::kIP);
  std::vector<float> vec(3);
  for (int round = 0; round < 100; round++) {
    for (int i = 0; i < 50; i++) {
      vec[0] = static_cast<float>(round * 50 + i);
      idx.Add("doc_" + std::to_string(i), vec.data());
    }
    idx.Clear();
    EXPECT_EQ(idx.Size(), 0u);
  }
}

TEST(AsanVectorStress, KnnQueryParseCycles) {
  for (int i = 0; i < 1000; i++) {
    ParsedQuery q;
    std::string err;
    ParseQuery("*=>[KNN " + std::to_string(i + 1) + " @emb $blob]", q,
                  err);
  }
}
