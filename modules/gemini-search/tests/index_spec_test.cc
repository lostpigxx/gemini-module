#include <gtest/gtest.h>
#include "index_spec.h"

#include <algorithm>
#include <string>
#include <vector>

// =============================================================
// FieldTypeName
// =============================================================

TEST(FieldTypeNameTest, ReturnsCorrectStrings) {
  EXPECT_STREQ(FieldTypeName(FieldType::kTag), "TAG");
  EXPECT_STREQ(FieldTypeName(FieldType::kNumeric), "NUMERIC");
}

// =============================================================
// IndexSpec::FindField
// =============================================================

TEST(IndexSpecTest, FindFieldReturnsMatch) {
  IndexSpec spec{"idx", {{"name", FieldType::kTag}, {"price", FieldType::kNumeric}}};
  auto* f = spec.FindField("price");
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->name, "price");
  EXPECT_EQ(f->type, FieldType::kNumeric);
}

TEST(IndexSpecTest, FindFieldReturnsNullForMissing) {
  IndexSpec spec{"idx", {{"name", FieldType::kTag}}};
  EXPECT_EQ(spec.FindField("nonexistent"), nullptr);
}

TEST(IndexSpecTest, FindFieldEmptyFields) {
  IndexSpec spec{"idx", {}};
  EXPECT_EQ(spec.FindField("anything"), nullptr);
}

// =============================================================
// IndexRegistry::Create
// =============================================================

TEST(IndexRegistryTest, CreateSucceeds) {
  IndexRegistry reg;
  EXPECT_TRUE(reg.Create("idx1", {{"name", FieldType::kTag}}));
  EXPECT_EQ(reg.Size(), 1u);
}

TEST(IndexRegistryTest, CreateDuplicateFails) {
  IndexRegistry reg;
  EXPECT_TRUE(reg.Create("idx1", {{"a", FieldType::kTag}}));
  EXPECT_FALSE(reg.Create("idx1", {{"b", FieldType::kNumeric}}));
  EXPECT_EQ(reg.Size(), 1u);
}

TEST(IndexRegistryTest, CreateMultipleIndices) {
  IndexRegistry reg;
  EXPECT_TRUE(reg.Create("idx1", {{"a", FieldType::kTag}}));
  EXPECT_TRUE(reg.Create("idx2", {{"b", FieldType::kNumeric}}));
  EXPECT_TRUE(reg.Create("idx3", {{"c", FieldType::kTag}, {"d", FieldType::kNumeric}}));
  EXPECT_EQ(reg.Size(), 3u);
}

TEST(IndexRegistryTest, CreateEmptySchema) {
  IndexRegistry reg;
  EXPECT_TRUE(reg.Create("empty", {}));
  auto* spec = reg.Get("empty");
  ASSERT_NE(spec, nullptr);
  EXPECT_TRUE(spec->fields.empty());
}

TEST(IndexRegistryTest, CreatePreservesFieldOrder) {
  IndexRegistry reg;
  std::vector<FieldSpec> fields = {
      {"z_field", FieldType::kTag},
      {"a_field", FieldType::kNumeric},
      {"m_field", FieldType::kTag},
  };
  EXPECT_TRUE(reg.Create("idx", fields));
  auto* spec = reg.Get("idx");
  ASSERT_NE(spec, nullptr);
  ASSERT_EQ(spec->fields.size(), 3u);
  EXPECT_EQ(spec->fields[0].name, "z_field");
  EXPECT_EQ(spec->fields[1].name, "a_field");
  EXPECT_EQ(spec->fields[2].name, "m_field");
}

// =============================================================
// IndexRegistry::Get
// =============================================================

TEST(IndexRegistryTest, GetExistingIndex) {
  IndexRegistry reg;
  reg.Create("idx", {{"name", FieldType::kTag}, {"price", FieldType::kNumeric}});
  auto* spec = reg.Get("idx");
  ASSERT_NE(spec, nullptr);
  EXPECT_EQ(spec->name, "idx");
  ASSERT_EQ(spec->fields.size(), 2u);
  EXPECT_EQ(spec->fields[0].name, "name");
  EXPECT_EQ(spec->fields[0].type, FieldType::kTag);
  EXPECT_EQ(spec->fields[1].name, "price");
  EXPECT_EQ(spec->fields[1].type, FieldType::kNumeric);
}

TEST(IndexRegistryTest, GetNonexistentReturnsNull) {
  IndexRegistry reg;
  EXPECT_EQ(reg.Get("nope"), nullptr);
}

TEST(IndexRegistryTest, GetAfterDropReturnsNull) {
  IndexRegistry reg;
  reg.Create("idx", {{"a", FieldType::kTag}});
  reg.Drop("idx");
  EXPECT_EQ(reg.Get("idx"), nullptr);
}

// =============================================================
// IndexRegistry::Drop
// =============================================================

TEST(IndexRegistryTest, DropExistingSucceeds) {
  IndexRegistry reg;
  reg.Create("idx", {{"a", FieldType::kTag}});
  EXPECT_TRUE(reg.Drop("idx"));
  EXPECT_EQ(reg.Size(), 0u);
}

TEST(IndexRegistryTest, DropNonexistentFails) {
  IndexRegistry reg;
  EXPECT_FALSE(reg.Drop("nope"));
}

TEST(IndexRegistryTest, DropOnlyTargetIndex) {
  IndexRegistry reg;
  reg.Create("idx1", {{"a", FieldType::kTag}});
  reg.Create("idx2", {{"b", FieldType::kNumeric}});
  EXPECT_TRUE(reg.Drop("idx1"));
  EXPECT_EQ(reg.Size(), 1u);
  EXPECT_EQ(reg.Get("idx1"), nullptr);
  EXPECT_NE(reg.Get("idx2"), nullptr);
}

TEST(IndexRegistryTest, DropThenRecreate) {
  IndexRegistry reg;
  reg.Create("idx", {{"a", FieldType::kTag}});
  reg.Drop("idx");
  EXPECT_TRUE(reg.Create("idx", {{"b", FieldType::kNumeric}}));
  auto* spec = reg.Get("idx");
  ASSERT_NE(spec, nullptr);
  ASSERT_EQ(spec->fields.size(), 1u);
  EXPECT_EQ(spec->fields[0].name, "b");
  EXPECT_EQ(spec->fields[0].type, FieldType::kNumeric);
}

// =============================================================
// IndexRegistry::List
// =============================================================

TEST(IndexRegistryTest, ListEmpty) {
  IndexRegistry reg;
  EXPECT_TRUE(reg.List().empty());
}

TEST(IndexRegistryTest, ListReturnsSortedNames) {
  IndexRegistry reg;
  reg.Create("charlie", {{"a", FieldType::kTag}});
  reg.Create("alpha", {{"b", FieldType::kTag}});
  reg.Create("bravo", {{"c", FieldType::kTag}});

  auto names = reg.List();
  ASSERT_EQ(names.size(), 3u);
  EXPECT_EQ(names[0], "alpha");
  EXPECT_EQ(names[1], "bravo");
  EXPECT_EQ(names[2], "charlie");
}

TEST(IndexRegistryTest, ListAfterDrops) {
  IndexRegistry reg;
  reg.Create("a", {});
  reg.Create("b", {});
  reg.Create("c", {});
  reg.Drop("b");
  auto names = reg.List();
  ASSERT_EQ(names.size(), 2u);
  EXPECT_EQ(names[0], "a");
  EXPECT_EQ(names[1], "c");
}

// =============================================================
// IndexRegistry::Clear
// =============================================================

TEST(IndexRegistryTest, ClearRemovesAll) {
  IndexRegistry reg;
  reg.Create("a", {});
  reg.Create("b", {});
  reg.Clear();
  EXPECT_EQ(reg.Size(), 0u);
  EXPECT_TRUE(reg.List().empty());
}

// =============================================================
// IndexRegistry::Size
// =============================================================

TEST(IndexRegistryTest, SizeTracksCorrectly) {
  IndexRegistry reg;
  EXPECT_EQ(reg.Size(), 0u);
  reg.Create("a", {});
  EXPECT_EQ(reg.Size(), 1u);
  reg.Create("b", {});
  EXPECT_EQ(reg.Size(), 2u);
  reg.Drop("a");
  EXPECT_EQ(reg.Size(), 1u);
  reg.Drop("b");
  EXPECT_EQ(reg.Size(), 0u);
}
