#include <gtest/gtest.h>
#include "index_spec.h"

#include <string>
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
