#include <gtest/gtest.h>
#include "bloom_filter.h"
#include "murmur2.h"

#include <cstring>
#include <string>

static std::span<const std::byte> ToSpan(const std::string& s) {
  return AsBytes(s.data(), s.size());
}

TEST(MurmurHash2Test, Deterministic) {
  const char* key = "hello";
  uint32_t h1 = MurmurHash2(key, 5, 0x9747b28c);
  uint32_t h2 = MurmurHash2(key, 5, 0x9747b28c);
  EXPECT_NE(h1, 0u);
  EXPECT_EQ(h1, h2);
  EXPECT_NE(h1, MurmurHash2(key, 5, 0));
}

TEST(MurmurHash64ATest, Deterministic) {
  const char* key = "hello";
  uint64_t h1 = MurmurHash64A(key, 5, 0xc6a4a7935bd1e995ULL);
  uint64_t h2 = MurmurHash64A(key, 5, 0xc6a4a7935bd1e995ULL);
  EXPECT_NE(h1, 0ULL);
  EXPECT_EQ(h1, h2);
}

TEST(MurmurHash2Test, EmptyInput) {
  uint32_t h1 = MurmurHash2("", 0, 0x9747b28c);
  uint32_t h2 = MurmurHash2("", 0, 0x9747b28c);
  EXPECT_EQ(h1, h2);
}

TEST(HashPolicyTest, Hash32Consistency) {
  auto a = Hash32Policy::Compute(AsBytes("test", 4));
  auto b = Hash32Policy::Compute(AsBytes("test", 4));
  EXPECT_EQ(a.primary, b.primary);
  EXPECT_EQ(a.secondary, b.secondary);
  auto c = Hash32Policy::Compute(AsBytes("other", 5));
  EXPECT_NE(a.primary, c.primary);
}

TEST(HashPolicyTest, Hash64Consistency) {
  auto a = Hash64Policy::Compute(AsBytes("test", 4));
  auto b = Hash64Policy::Compute(AsBytes("test", 4));
  EXPECT_EQ(a.primary, b.primary);
  EXPECT_EQ(a.secondary, b.secondary);
}

TEST(BloomLayerTest, CreateRAII) {
  auto layer = BloomLayer::Create(1000, 0.01,
    BloomFlags::Use64Bit | BloomFlags::NoRound);
  ASSERT_TRUE(layer.has_value());
  EXPECT_GT(layer->GetHashCount(), 0u);
  EXPECT_GT(layer->GetTotalBits(), 0u);
  EXPECT_GT(layer->GetDataSize(), 0u);
  EXPECT_NE(layer->GetBitArray(), nullptr);
  EXPECT_GT(layer->GetBitsPerEntry(), 0.0);
}

TEST(BloomLayerTest, MoveSemantics) {
  auto layer = BloomLayer::Create(1000, 0.01, BloomFlags::Use64Bit);
  ASSERT_TRUE(layer.has_value());

  BloomLayer moved = std::move(*layer);
  EXPECT_NE(moved.GetBitArray(), nullptr);
  EXPECT_EQ(layer->GetBitArray(), nullptr);
}

TEST(BloomLayerTest, InsertAndTest) {
  auto layer = BloomLayer::Create(1000, 0.01, BloomFlags::Use64Bit);
  ASSERT_TRUE(layer.has_value());

  auto hp = Hash64Policy::Compute(AsBytes("hello", 5));
  EXPECT_TRUE(layer->Insert(hp));
  EXPECT_FALSE(layer->Insert(hp));
  EXPECT_TRUE(layer->Test(hp));
}

TEST(BloomLayerTest, FalsePositiveRate) {
  auto layer = BloomLayer::Create(10000, 0.01, BloomFlags::Use64Bit);
  ASSERT_TRUE(layer.has_value());

  for (int i = 0; i < 10000; i++) {
    auto item = "item_" + std::to_string(i);
    layer->Insert(Hash64Policy::Compute(ToSpan(item)));
  }

  for (int i = 0; i < 10000; i++) {
    auto item = "item_" + std::to_string(i);
    EXPECT_TRUE(layer->Test(Hash64Policy::Compute(ToSpan(item))))
      << "False negative for " << item;
  }

  int falsePositives = 0;
  constexpr int testCount = 100000;
  for (int i = 10000; i < 10000 + testCount; i++) {
    auto item = "other_" + std::to_string(i);
    if (layer->Test(Hash64Policy::Compute(ToSpan(item)))) falsePositives++;
  }

  double fpRate = static_cast<double>(falsePositives) / testCount;
  EXPECT_LT(fpRate, 0.03) << "FP rate too high: " << fpRate;
}

TEST(BloomLayerTest, PowerOfTwoViaBitCeil) {
  auto layer = BloomLayer::Create(1000, 0.01, BloomFlags::Use64Bit);
  ASSERT_TRUE(layer.has_value());
  EXPECT_GT(layer->GetLog2Bits(), 0);
  EXPECT_EQ(layer->GetTotalBits(), 1ULL << layer->GetLog2Bits());
}

TEST(BloomFlagsTest, EnumClassOperators) {
  auto combined = BloomFlags::Use64Bit | BloomFlags::NoRound;
  EXPECT_TRUE(HasFlag(combined, BloomFlags::Use64Bit));
  EXPECT_TRUE(HasFlag(combined, BloomFlags::NoRound));
  EXPECT_FALSE(HasFlag(combined, BloomFlags::FixedSize));
  EXPECT_EQ(ToUnderlying(combined), 5u);
}

// --- Phase 3: Flags validation tests ---

TEST(BloomFlagsTest, ValidateFlagsAcceptsSupportedCombinations) {
  EXPECT_TRUE(ValidateFlags(0));
  EXPECT_TRUE(ValidateFlags(ToUnderlying(BloomFlags::NoRound)));
  EXPECT_TRUE(ValidateFlags(ToUnderlying(BloomFlags::Use64Bit)));
  EXPECT_TRUE(ValidateFlags(ToUnderlying(BloomFlags::FixedSize)));
  EXPECT_TRUE(ValidateFlags(ToUnderlying(BloomFlags::Use64Bit | BloomFlags::NoRound)));
  EXPECT_TRUE(ValidateFlags(
    ToUnderlying(BloomFlags::Use64Bit | BloomFlags::NoRound | BloomFlags::FixedSize)));
}

TEST(BloomFlagsTest, ValidateFlagsRejectsUnknownBits) {
  EXPECT_FALSE(ValidateFlags(0x80));
  EXPECT_FALSE(ValidateFlags(0x10));
  EXPECT_FALSE(ValidateFlags(0xFF));
}

TEST(BloomFlagsTest, ValidateFlagsRejectsRawBits) {
  EXPECT_FALSE(ValidateFlags(ToUnderlying(BloomFlags::RawBits)));
  EXPECT_FALSE(ValidateFlags(
    ToUnderlying(BloomFlags::Use64Bit | BloomFlags::RawBits)));
}

TEST(BloomLayerTest, RawBitsCreatesZeroHashCount) {
  auto layer = BloomLayer::Create(1024, 0.01, BloomFlags::RawBits);
  ASSERT_TRUE(layer.has_value());
  EXPECT_EQ(layer->GetHashCount(), 0u);
  auto hp = Hash32Policy::Compute(AsBytes("test", 4));
  EXPECT_TRUE(layer->Test(hp)) << "hashCount=0 makes Test() always true";
}

TEST(BloomFlagsTest, ResourceLimitConstants) {
  EXPECT_EQ(kMaxCapacity, 1ULL << 30);
  EXPECT_EQ(kMaxExpansion, 32768u);
}

// --- Phase 6: Hash exact golden vectors ---

TEST(MurmurHash2Test, ExactVectors) {
  EXPECT_EQ(MurmurHash2("", 0, 0x9747b28c), 0x106e08d9u);
  EXPECT_EQ(MurmurHash2("a", 1, 0x9747b28c), 0xa2d0b27cu);
  EXPECT_EQ(MurmurHash2("hello", 5, 0x9747b28c), 0x7f1ddbbdu);

  char bin[] = "hello\0world";
  EXPECT_EQ(MurmurHash2(bin, 11, 0x9747b28c), 0xd8e4f032u);
}

TEST(MurmurHash64ATest, ExactVectors) {
  EXPECT_EQ(MurmurHash64A("", 0, 0xc6a4a7935bd1e995ULL), 0x1ab11ea5a7b2c56eULL);
  EXPECT_EQ(MurmurHash64A("a", 1, 0xc6a4a7935bd1e995ULL), 0x4292cee227b9150aULL);
  EXPECT_EQ(MurmurHash64A("hello", 5, 0xc6a4a7935bd1e995ULL), 0x5ba5b8a59803e699ULL);

  char bin[] = "hello\0world";
  EXPECT_EQ(MurmurHash64A(bin, 11, 0xc6a4a7935bd1e995ULL), 0xdcd0bc9f75315849ULL);
}

TEST(HashPolicyTest, Hash32ExactVectors) {
  auto hp = Hash32Policy::Compute(AsBytes("hello", 5));
  EXPECT_EQ(hp.primary, 0x7f1ddbbdu);
  EXPECT_EQ(hp.secondary, 0xed999d2du);
}

TEST(HashPolicyTest, Hash64ExactVectors) {
  auto hp = Hash64Policy::Compute(AsBytes("hello", 5));
  EXPECT_EQ(hp.primary, 0x5ba5b8a59803e699ULL);
  EXPECT_EQ(hp.secondary, 0xa7d451d588a0c2a4ULL);
}

TEST(HashPolicyTest, Hash32BinaryWithNulls) {
  char bin[] = "hello\0world";
  auto hp = Hash32Policy::Compute(AsBytes(bin, 11));
  EXPECT_EQ(hp.primary, 0xd8e4f032u);
  EXPECT_EQ(hp.secondary, 0x2117a707u);
}

TEST(HashPolicyTest, Hash64BinaryWithNulls) {
  char bin[] = "hello\0world";
  auto hp = Hash64Policy::Compute(AsBytes(bin, 11));
  EXPECT_EQ(hp.primary, 0xdcd0bc9f75315849ULL);
  EXPECT_EQ(hp.secondary, 0xcf5f620f7200160dULL);
}
