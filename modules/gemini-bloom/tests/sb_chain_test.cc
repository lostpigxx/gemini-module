#include <gtest/gtest.h>
#include "sb_chain.h"

#include <string>
#include <vector>

static std::span<const std::byte> ToSpan(const std::string& s) {
  return AsBytes(s.data(), s.size());
}

static auto DefaultFlags() {
  return BloomFlags::Use64Bit | BloomFlags::NoRound;
}

TEST(ScalingBloomTest, ConstructAndDestruct) {
  auto* mem = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  new (mem) ScalingBloomFilter(1000, 0.01, DefaultFlags(), 2);
  EXPECT_TRUE(mem->IsValid());
  EXPECT_EQ(mem->NumLayers(), 1u);
  EXPECT_EQ(mem->TotalItems(), 0u);
  EXPECT_EQ(mem->ExpansionFactor(), 2u);
  EXPECT_GT(mem->TotalCapacity(), 0u);
  mem->~ScalingBloomFilter();
  free(mem);
}

TEST(ScalingBloomTest, PutAndContains) {
  auto* mem = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  new (mem) ScalingBloomFilter(1000, 0.01, DefaultFlags(), 2);

  auto r1 = mem->Put(AsBytes("hello", 5));
  ASSERT_TRUE(r1.has_value());
  EXPECT_TRUE(*r1);

  auto r2 = mem->Put(AsBytes("hello", 5));
  ASSERT_TRUE(r2.has_value());
  EXPECT_FALSE(*r2);

  EXPECT_TRUE(mem->Contains(AsBytes("hello", 5)));
  EXPECT_EQ(mem->TotalItems(), 1u);

  mem->~ScalingBloomFilter();
  free(mem);
}

TEST(ScalingBloomTest, Uses32BitHashWhenFlagIsAbsent) {
  auto flags = BloomFlags::NoRound;
  auto* mem = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  new (mem) ScalingBloomFilter(1000, 0.01, flags, 2);

  ASSERT_TRUE(mem->IsValid());
  EXPECT_EQ(mem->Flags(), flags);
  auto r1 = mem->Put(AsBytes("hash32", 6));
  ASSERT_TRUE(r1.has_value());
  EXPECT_TRUE(*r1);
  EXPECT_TRUE(mem->Contains(AsBytes("hash32", 6)));
  EXPECT_FALSE(mem->Contains(AsBytes("hash64", 6)));

  mem->~ScalingBloomFilter();
  free(mem);
}

TEST(ScalingBloomTest, NoFalseNegatives) {
  auto* mem = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  new (mem) ScalingBloomFilter(5000, 0.01, DefaultFlags(), 2);

  std::vector<std::string> items;
  for (int i = 0; i < 5000; i++) {
    items.push_back("item_" + std::to_string(i));
  }
  for (const auto& item : items) mem->Put(ToSpan(item));
  for (const auto& item : items) {
    EXPECT_TRUE(mem->Contains(ToSpan(item))) << "False negative for " << item;
  }

  mem->~ScalingBloomFilter();
  free(mem);
}

TEST(ScalingBloomTest, AutoExpansion) {
  auto* mem = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  new (mem) ScalingBloomFilter(100, 0.01, DefaultFlags(), 2);

  for (int i = 0; i < 500; i++) {
    auto item = "expand_" + std::to_string(i);
    mem->Put(ToSpan(item));
  }
  EXPECT_GT(mem->NumLayers(), 1u);

  for (int i = 0; i < 500; i++) {
    auto item = "expand_" + std::to_string(i);
    EXPECT_TRUE(mem->Contains(ToSpan(item)));
  }

  mem->~ScalingBloomFilter();
  free(mem);
}

TEST(ScalingBloomTest, FixedSizeRejectsOverflow) {
  auto flg = DefaultFlags() | BloomFlags::FixedSize;
  auto* mem = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  new (mem) ScalingBloomFilter(100, 0.01, flg, 2);

  int inserted = 0;
  for (int i = 0; i < 200; i++) {
    auto item = "fixed_" + std::to_string(i);
    auto result = mem->Put(ToSpan(item));
    if (!result.has_value()) break;
    if (*result) inserted++;
  }
  EXPECT_EQ(mem->NumLayers(), 1u);
  EXPECT_LE(inserted, 100);

  mem->~ScalingBloomFilter();
  free(mem);
}

TEST(ScalingBloomTest, OptionalReturnSemantics) {
  auto flg = DefaultFlags() | BloomFlags::FixedSize;
  auto* mem = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  new (mem) ScalingBloomFilter(2, 0.01, flg, 2);

  auto r1 = mem->Put(AsBytes("a", 1));
  EXPECT_TRUE(r1.has_value());
  EXPECT_TRUE(*r1);

  auto r2 = mem->Put(AsBytes("a", 1));
  EXPECT_TRUE(r2.has_value());
  EXPECT_FALSE(*r2);

  mem->Put(AsBytes("b", 1));
  auto r3 = mem->Put(AsBytes("c", 1));
  EXPECT_FALSE(r3.has_value());

  mem->~ScalingBloomFilter();
  free(mem);
}

TEST(ScalingBloomTest, FixedSizeDuplicateDoesNotConsumeCapacity) {
  auto flg = DefaultFlags() | BloomFlags::FixedSize;
  auto* mem = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  new (mem) ScalingBloomFilter(2, 0.01, flg, 2);

  ASSERT_TRUE(mem->Put(AsBytes("a", 1)).has_value());
  for (int i = 0; i < 5; i++) {
    auto duplicate = mem->Put(AsBytes("a", 1));
    ASSERT_TRUE(duplicate.has_value());
    EXPECT_FALSE(*duplicate);
  }

  auto b = mem->Put(AsBytes("b", 1));
  ASSERT_TRUE(b.has_value());
  EXPECT_TRUE(*b);
  EXPECT_EQ(mem->TotalItems(), 2u);
  EXPECT_FALSE(mem->Put(AsBytes("c", 1)).has_value());

  mem->~ScalingBloomFilter();
  free(mem);
}

TEST(ScalingBloomTest, TotalCapacity) {
  auto* mem = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  new (mem) ScalingBloomFilter(1000, 0.01, DefaultFlags(), 2);
  EXPECT_EQ(mem->TotalCapacity(), 1000u);
  mem->~ScalingBloomFilter();
  free(mem);
}

TEST(ScalingBloomTest, BytesUsed) {
  auto* mem = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  new (mem) ScalingBloomFilter(1000, 0.01, DefaultFlags(), 2);
  EXPECT_GT(mem->BytesUsed(), sizeof(ScalingBloomFilter));
  mem->~ScalingBloomFilter();
  free(mem);
}

TEST(ScalingBloomTest, MoveAssignmentTransfersOwnership) {
  auto* leftMem = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  auto* rightMem = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  new (leftMem) ScalingBloomFilter(10, 0.01, DefaultFlags(), 2);
  new (rightMem) ScalingBloomFilter(20, 0.001, DefaultFlags(), 4);

  rightMem->Put(AsBytes("moved", 5));
  *leftMem = std::move(*rightMem);

  EXPECT_TRUE(leftMem->Contains(AsBytes("moved", 5)));
  EXPECT_EQ(leftMem->ExpansionFactor(), 4u);
  EXPECT_EQ(leftMem->TotalItems(), 1u);
  EXPECT_FALSE(rightMem->IsValid());

  leftMem->~ScalingBloomFilter();
  rightMem->~ScalingBloomFilter();
  free(leftMem);
  free(rightMem);
}

TEST(ScalingBloomTest, ExpansionOneAddsSameSizedLayers) {
  auto* mem = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  new (mem) ScalingBloomFilter(10, 0.01, DefaultFlags(), 1);

  for (int i = 0; i < 35; i++) {
    auto item = "exp1_" + std::to_string(i);
    mem->Put(ToSpan(item));
  }

  ASSERT_GT(mem->NumLayers(), 1u);
  for (const auto& layer : mem->Layers()) {
    EXPECT_EQ(layer.bloom.GetCapacity(), 10u);
  }
  EXPECT_EQ(mem->TotalCapacity(), mem->NumLayers() * 10u);

  mem->~ScalingBloomFilter();
  free(mem);
}

// Bug regression: SetLayer must use placement new on calloc'd memory.
// FromRdbShell allocates with calloc (zero-filled), then SetLayer
// assigns into those slots. Without placement new, this is UB.
TEST(ScalingBloomTest, FromRdbShellSetLayer) {
  // Build a filter with data, then reconstruct via shell
  auto* orig = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  new (orig) ScalingBloomFilter(100, 0.01, DefaultFlags(), 2);

  std::vector<std::string> items;
  for (int i = 0; i < 300; i++) {
    items.push_back("shell_" + std::to_string(i));
    orig->Put(ToSpan(items.back()));
  }
  EXPECT_GT(orig->NumLayers(), 1u);

  // Reconstruct via FromRdbShell + SetLayer
  ScalingBloomFilter::RdbShell shell{
    .totalItems = orig->TotalItems(),
    .numLayers = orig->NumLayers(),
    .flags = orig->Flags(),
    .expansionFactor = orig->ExpansionFactor(),
  };
  auto* rebuilt = ScalingBloomFilter::FromRdbShell(shell);
  ASSERT_NE(rebuilt, nullptr);

  for (size_t i = 0; i < orig->NumLayers(); i++) {
    auto& src = orig->Layers()[i];
    auto layer = BloomLayer::Create(
      src.bloom.GetCapacity(), src.bloom.GetFpRate(), orig->Flags());
    ASSERT_TRUE(layer.has_value());
    std::memcpy(layer->GetBitArray(), src.bloom.GetBitArray(),
                src.bloom.GetDataSize());
    rebuilt->SetLayer(i, {std::move(*layer), src.itemCount});
  }

  for (const auto& item : items) {
    EXPECT_TRUE(rebuilt->Contains(ToSpan(item)))
      << "False negative after FromRdbShell+SetLayer: " << item;
  }

  orig->~ScalingBloomFilter();
  free(orig);
  rebuilt->~ScalingBloomFilter();
  free(rebuilt);
}

// Bug regression: AppendLayer must safely move FilterLayer objects
// during array growth instead of using realloc on non-trivial types.
// This test forces multiple internal array expansions by inserting
// far more items than the initial capacity with a small starting
// layer array (initial layerCapacity_ = 4).
TEST(ScalingBloomTest, AppendLayerSafeRelocation) {
  auto* mem = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  new (mem) ScalingBloomFilter(10, 0.01, DefaultFlags(), 2);

  std::vector<std::string> items;
  for (int i = 0; i < 500; i++) {
    auto item = "reloc_" + std::to_string(i);
    auto result = mem->Put(ToSpan(item));
    if (!result.has_value()) break;
    items.push_back(std::move(item));
  }
  EXPECT_GT(mem->NumLayers(), 4u);

  for (const auto& item : items) {
    EXPECT_TRUE(mem->Contains(ToSpan(item)))
      << "False negative after layer relocation: " << item;
  }

  mem->~ScalingBloomFilter();
  free(mem);
}

// Bug regression: extreme capacity/fpRate should be rejected
// instead of causing float-to-int overflow UB.
TEST(ScalingBloomTest, ExtremeParamsRejected) {
  auto layer = BloomLayer::Create(UINT64_MAX, 1e-300, DefaultFlags());
  EXPECT_FALSE(layer.has_value())
    << "Should reject extreme capacity * bitsPerEntry overflow";

  auto layer2 = BloomLayer::Create(1ULL << 50, 1e-100, DefaultFlags());
  EXPECT_FALSE(layer2.has_value())
    << "Should reject large capacity with tiny fpRate";
}

TEST(ScalingBloomTest, TotalDataSizeAccumulates) {
  auto* mem = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  new (mem) ScalingBloomFilter(100, 0.01, DefaultFlags(), 2);

  uint64_t initialDataSize = mem->TotalDataSize();
  EXPECT_GT(initialDataSize, 0u);
  EXPECT_EQ(mem->NumLayers(), 1u);

  for (int i = 0; i < 500; i++) {
    auto item = "grow_" + std::to_string(i);
    mem->Put(ToSpan(item));
  }
  EXPECT_GT(mem->NumLayers(), 1u);
  EXPECT_GT(mem->TotalDataSize(), initialDataSize);

  uint64_t summed = 0;
  for (const auto& layer : mem->Layers()) {
    summed += layer.bloom.GetDataSize();
  }
  EXPECT_EQ(mem->TotalDataSize(), summed);

  mem->~ScalingBloomFilter();
  free(mem);
}

TEST(ScalingBloomTest, MaxBitsPerEntryConstant) {
  EXPECT_EQ(kMaxBitsPerEntry, 1000.0);
}

TEST(ScalingBloomTest, MaxTotalDataSizeConstant) {
  EXPECT_EQ(kMaxTotalDataSize, 4ULL * 1024 * 1024 * 1024);
}

TEST(ScalingBloomTest, LoadingStateLifecycle) {
  auto* mem = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  new (mem) ScalingBloomFilter(100, 0.01, DefaultFlags(), 2);

  EXPECT_FALSE(mem->IsLoading());
  mem->SetLoading();
  EXPECT_TRUE(mem->IsLoading());
  EXPECT_TRUE(HasFlag(mem->Flags(), BloomFlags::Loading));
  mem->ClearLoading();
  EXPECT_FALSE(mem->IsLoading());
  EXPECT_FALSE(HasFlag(mem->Flags(), BloomFlags::Loading));

  mem->~ScalingBloomFilter();
  free(mem);
}

TEST(ScalingBloomTest, LoadingFlagNotInSupportedFlags) {
  EXPECT_FALSE(ValidateFlags(ToUnderlying(BloomFlags::Loading)));
}

// SerializeDeserializeHeader test is covered by TCL integration tests
// (SCANDUMP/LOADCHUNK round-trip) since the serialization code depends
// on the Redis Module API.

TEST(ScalingBloomTest, SpanInterface) {
  auto* mem = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  new (mem) ScalingBloomFilter(1000, 0.01, DefaultFlags(), 2);

  std::string data = "test_span";
  auto result = mem->Put(ToSpan(data));
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(*result);
  EXPECT_TRUE(mem->Contains(ToSpan(data)));

  mem->~ScalingBloomFilter();
  free(mem);
}
