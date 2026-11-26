#define REDISMODULE_API
extern "C" {
#include "redismodule.h"
}

#include "mock_redismodule_io.h"

#include <gtest/gtest.h>
#include "bloom_rdb.h"
#include "cf_chain.h"
#include "cf_rdb.h"
#include "cms_rdb.h"
#include "cms_sketch.h"
#include "sb_chain.h"
#include "tdigest_rdb.h"
#include "tdigest_sketch.h"
#include "topk_rdb.h"
#include "topk_sketch.h"

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

class BloomFuzzEnv : public ::testing::Environment {
public:
  void SetUp() override { InstallMockRedisModuleIO(); }
};
static auto* const gFuzzEnv [[maybe_unused]] =
  ::testing::AddGlobalTestEnvironment(new BloomFuzzEnv);

std::span<const std::byte> ToSpan(const std::string& s) {
  return AsBytes(s.data(), s.size());
}

TEST(BloomFuzzTest, WireHeaderAndChunkTruncationsAndBitFlips) {
  std::mt19937_64 rng(0x5eed1234ULL);

  // 1. Bloom wire header
  {
    ScalingBloomFilter bf(200, 0.01, BloomFlags::Use64Bit | BloomFlags::NoRound, 2);
    for (int i = 0; i < 300; i++) (void)bf.Put(ToSpan("bf_" + std::to_string(i)));
    std::vector<uint8_t> valid(ComputeHeaderSize(bf));
    SerializeHeader(bf, valid.data());

    for (size_t cut = 0; cut < valid.size(); cut++) {
      EXPECT_EQ(DeserializeHeader(valid.data(), cut), nullptr);
    }
    for (int iter = 0; iter < 500; iter++) {
      auto mutated = valid;
      size_t idx = rng() % mutated.size();
      mutated[idx] ^= static_cast<uint8_t>(1u << (rng() % 8));
      auto* res = DeserializeHeader(mutated.data(), mutated.size());
      if (res) FreeBloom(res);
    }
  }

  // 2. Cuckoo wire header
  {
    CuckooChain cf(128, 4, 20, 2, 16);
    for (int i = 0; i < 200; i++) (void)cf.Add(ToSpan("cf_" + std::to_string(i)));
    std::vector<uint8_t> valid(CfComputeHeaderSize(cf));
    CfSerializeHeader(cf, valid.data());

    for (size_t cut = 0; cut < valid.size(); cut++) {
      EXPECT_EQ(CfDeserializeHeader(valid.data(), cut), nullptr);
    }
    for (int iter = 0; iter < 500; iter++) {
      auto mutated = valid;
      size_t idx = rng() % mutated.size();
      mutated[idx] ^= static_cast<uint8_t>(1u << (rng() % 8));
      auto* res = CfDeserializeHeader(mutated.data(), mutated.size());
      if (res) FreeCuckoo(res);
    }
  }

  // 3. CMS wire chunk
  {
    auto cms = *CmsSketch::Create(128, 4);
    for (int i = 0; i < 50; i++) (void)cms.IncrBy(ToSpan("cms_" + std::to_string(i)), i + 1);
    std::vector<uint8_t> valid(CmsComputeChunkSize(cms));
    CmsSerializeChunk(cms, valid.data());

    for (size_t cut = 0; cut < valid.size(); cut += 7) {
      EXPECT_EQ(CmsDeserializeChunk(valid.data(), cut), nullptr);
    }
    for (int iter = 0; iter < 500; iter++) {
      auto mutated = valid;
      size_t idx = rng() % mutated.size();
      mutated[idx] ^= static_cast<uint8_t>(1u << (rng() % 8));
      auto* res = CmsDeserializeChunk(mutated.data(), mutated.size());
      if (res) FreeCms(res);
    }
  }

  // 4. Top-K wire chunk
  {
    auto tk = *TopkSketch::Create(10, 64, 4, 0.9);
    for (int i = 0; i < 100; i++) (void)tk.IncrBy(ToSpan("tk_" + std::to_string(i)), (i % 5) + 1);
    std::vector<uint8_t> valid(TopkComputeChunkSize(tk));
    TopkSerializeChunk(tk, valid.data());

    for (size_t cut = 0; cut < valid.size(); cut += 7) {
      EXPECT_EQ(TopkDeserializeChunk(valid.data(), cut), nullptr);
    }
    for (int iter = 0; iter < 500; iter++) {
      auto mutated = valid;
      size_t idx = rng() % mutated.size();
      mutated[idx] ^= static_cast<uint8_t>(1u << (rng() % 8));
      auto* res = TopkDeserializeChunk(mutated.data(), mutated.size());
      if (res) FreeTopk(res);
    }
  }

  // 5. T-Digest wire chunk
  {
    auto td = *TdigestSketch::Create(50.0);
    for (int i = 0; i < 500; i++) (void)td.Add(static_cast<double>(i) * 0.5);
    td.Compress();
    (void)td.Add(999.0);
    std::vector<uint8_t> valid(TdigestComputeChunkSize(td));
    TdigestSerializeChunk(td, valid.data());

    for (size_t cut = 0; cut < valid.size(); cut += 5) {
      EXPECT_EQ(TdigestDeserializeChunk(valid.data(), cut), nullptr);
    }
    for (int iter = 0; iter < 500; iter++) {
      auto mutated = valid;
      size_t idx = rng() % mutated.size();
      mutated[idx] ^= static_cast<uint8_t>(1u << (rng() % 8));
      auto* res = TdigestDeserializeChunk(mutated.data(), mutated.size());
      if (res) FreeTdigest(res);
    }
  }
}

TEST(BloomFuzzTest, RandomGarbageInputsNeverCrashOrLeak) {
  std::mt19937_64 rng(0xabcdef01ULL);
  std::vector<uint8_t> buf;
  for (int iter = 0; iter < 1000; iter++) {
    size_t len = rng() % 256;
    buf.resize(len);
    for (size_t i = 0; i < len; i++) buf[i] = static_cast<uint8_t>(rng() & 0xFFu);
    if (auto* p = DeserializeHeader(buf.data(), len)) FreeBloom(p);
    if (auto* p = CfDeserializeHeader(buf.data(), len)) FreeCuckoo(p);
    if (auto* p = CmsDeserializeChunk(buf.data(), len)) FreeCms(p);
    if (auto* p = TopkDeserializeChunk(buf.data(), len)) FreeTopk(p);
    if (auto* p = TdigestDeserializeChunk(buf.data(), len)) FreeTdigest(p);
  }
}

TEST(BloomFuzzTest, TdigestRejectsUnsortedCentroidsAndWeightMismatch) {
  auto td = *TdigestSketch::Create(100.0);
  ASSERT_TRUE(td.AllocateForLoad(2, 0));
  // Craft unsorted centroids (20.0 before 10.0) and serialize.
  td.AppendCentroidForLoad(20.0, 5.0);
  td.AppendCentroidForLoad(10.0, 5.0);
  td.SetBookkeepingForLoad(10.0, 0.0, 10.0, 20.0, 1);

  std::vector<uint8_t> chunk(TdigestComputeChunkSize(td));
  TdigestSerializeChunk(td, chunk.data());
  EXPECT_EQ(TdigestDeserializeChunk(chunk.data(), chunk.size()), nullptr);

  // Craft sorted centroids with mismatched mergedWeight (header says 100, sum is 10).
  auto td2 = *TdigestSketch::Create(100.0);
  ASSERT_TRUE(td2.AllocateForLoad(2, 0));
  td2.AppendCentroidForLoad(10.0, 5.0);
  td2.AppendCentroidForLoad(20.0, 5.0);
  td2.SetBookkeepingForLoad(100.0, 0.0, 10.0, 20.0, 1);

  std::vector<uint8_t> chunk2(TdigestComputeChunkSize(td2));
  TdigestSerializeChunk(td2, chunk2.data());
  EXPECT_EQ(TdigestDeserializeChunk(chunk2.data(), chunk2.size()), nullptr);
}

TEST(BloomFuzzTest, TopkRejectsDuplicateEntryNamesInRdb) {
  MockRdbStream stream;
  auto* io = stream.IO();
  RedisModule_SaveUnsigned(io, 5);    // k
  RedisModule_SaveUnsigned(io, 16);   // width
  RedisModule_SaveUnsigned(io, 2);    // depth
  RedisModule_SaveDouble(io, 0.9);    // decay
  RedisModule_SaveUnsigned(io, 2);    // numActive
  std::vector<char> cells(16 * 2 * sizeof(TopkSketch::Cell), 0);
  RedisModule_SaveStringBuffer(io, cells.data(), cells.size());
  // Two entries with identical name "dup"
  RedisModule_SaveUnsigned(io, 10);
  RedisModule_SaveStringBuffer(io, "dup", 3);
  RedisModule_SaveUnsigned(io, 10);
  RedisModule_SaveStringBuffer(io, "dup", 3);
  stream.Rewind();

  EXPECT_EQ(RdbLoadTopk(io, kTopkEncVerCurrent), nullptr);
}

}  // namespace
