// RDB and wire-format serialization round-trip tests for the gemini-bloom
// Cuckoo Filter (CF) data type. Verifies RDB encver 1, wire-format
// (SCANDUMP/LOADCHUNK) headers, layer metadata, and data integrity after
// serialization.

#define REDISMODULE_API
extern "C" {
#include "redismodule.h"
}

#include "mock_redismodule_io.h"

#include <gtest/gtest.h>
#include "cf_rdb.h"
#include "cf_chain.h"
#include "cuckoo_filter.h"
#include "rm_alloc.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

// Install mock IO before any tests.
class CfRdbTestEnv : public ::testing::Environment {
public:
  void SetUp() override { InstallMockRedisModuleIO(); }
};
static auto* const gEnv =
  ::testing::AddGlobalTestEnvironment(new CfRdbTestEnv);

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

static CuckooChain* CreateChain(uint64_t capacity, uint32_t bucketSize, uint32_t maxIterations,
                                 uint32_t expansion, uint32_t maxExpansions) {
  auto* mem = static_cast<CuckooChain*>(malloc(sizeof(CuckooChain)));
  new (mem) CuckooChain(capacity, bucketSize, maxIterations, expansion, maxExpansions);
  return mem;
}

static void DestroyChain(CuckooChain* chain) {
  if (chain) { chain->~CuckooChain(); free(chain); }
}

static CuckooChain* CfRdbRoundTrip(CuckooChain* src, int load_encver) {
  MockRdbStream stream;
  RdbSaveCuckoo(stream.IO(), src);
  stream.Rewind();
  return static_cast<CuckooChain*>(RdbLoadCuckoo(stream.IO(), load_encver));
}

static std::span<const std::byte> ToSpan(const std::string& s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

// ==================================================================
// RDB round-trip: empty chain
// ==================================================================

TEST(CfRdb, EmptyChainRoundTrip) {
  auto* chain = CreateChain(1024, 2, 20, 2, 32);
  ASSERT_TRUE(chain->IsValid());
  EXPECT_EQ(chain->TotalItems(), 0u);
  EXPECT_EQ(chain->NumLayers(), 1u);

  auto* loaded = CfRdbRoundTrip(chain, kCfEncVerCurrent);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->TotalItems(), 0u);
  EXPECT_EQ(loaded->NumLayers(), 1u);
  EXPECT_EQ(loaded->Expansion(), 2u);
  EXPECT_EQ(loaded->BucketSize(), 2u);

  DestroyChain(chain);
  DestroyChain(loaded);
}

// ==================================================================
// RDB round-trip: chain with data — no false negatives
// ==================================================================

TEST(CfRdb, PopulatedChainRoundTrip) {
  auto* chain = CreateChain(1024, 2, 20, 2, 32);

  std::vector<std::string> items;
  for (int i = 0; i < 500; i++) {
    auto item = "item_" + std::to_string(i);
    if (chain->Add(ToSpan(item)).has_value()) items.push_back(item);
  }
  EXPECT_GT(items.size(), 0u);

  auto* loaded = CfRdbRoundTrip(chain, kCfEncVerCurrent);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->TotalItems(), chain->TotalItems());

  for (const auto& item : items) {
    EXPECT_TRUE(loaded->Contains(ToSpan(item))) << "False negative after RDB round-trip: " << item;
  }

  DestroyChain(chain);
  DestroyChain(loaded);
}

// ==================================================================
// RDB round-trip: multi-layer chain (auto-expanded)
// ==================================================================

TEST(CfRdb, MultiLayerRoundTrip) {
  auto* chain = CreateChain(64, 2, 20, 2, 32);

  std::vector<std::string> items;
  for (int i = 0; i < 500; i++) {
    auto item = "multi_" + std::to_string(i);
    if (chain->Add(ToSpan(item)).has_value()) items.push_back(item);
  }
  ASSERT_GT(chain->NumLayers(), 1u);
  size_t origLayers = chain->NumLayers();
  uint64_t origItems = chain->TotalItems();

  auto* loaded = CfRdbRoundTrip(chain, kCfEncVerCurrent);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->NumLayers(), origLayers);
  EXPECT_EQ(loaded->TotalItems(), origItems);

  for (const auto& item : items) {
    EXPECT_TRUE(loaded->Contains(ToSpan(item)));
  }

  DestroyChain(chain);
  DestroyChain(loaded);
}

// ==================================================================
// RDB round-trip: metadata + bucket array exact binary match
// ==================================================================

TEST(CfRdb, MetadataAndBucketArrayExactMatch) {
  auto* chain = CreateChain(1024, 4, 25, 4, 16);
  for (int i = 0; i < 200; i++) {
    auto item = "bits_" + std::to_string(i);
    chain->Add(ToSpan(item));
  }

  std::vector<std::vector<uint8_t>> origBuckets;
  for (const auto& layer : chain->Layers()) {
    origBuckets.emplace_back(layer.cuckoo.GetBucketArray(),
                              layer.cuckoo.GetBucketArray() + layer.cuckoo.GetDataSize());
  }

  auto* loaded = CfRdbRoundTrip(chain, kCfEncVerCurrent);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->BucketSize(), chain->BucketSize());
  EXPECT_EQ(loaded->MaxIterations(), chain->MaxIterations());
  EXPECT_EQ(loaded->Expansion(), chain->Expansion());
  EXPECT_EQ(loaded->MaxExpansions(), chain->MaxExpansions());
  ASSERT_EQ(loaded->NumLayers(), origBuckets.size());

  for (size_t i = 0; i < origBuckets.size(); i++) {
    auto& layer = loaded->Layers()[i];
    ASSERT_EQ(layer.cuckoo.GetDataSize(), origBuckets[i].size());
    EXPECT_EQ(std::memcmp(layer.cuckoo.GetBucketArray(), origBuckets[i].data(),
                          origBuckets[i].size()), 0)
      << "Bucket array mismatch in layer " << i;
  }

  DestroyChain(chain);
  DestroyChain(loaded);
}

// ==================================================================
// RDB: rejected unknown encver
// ==================================================================

TEST(CfRdb, RejectsUnknownEncver) {
  MockRdbStream stream;
  auto* chain = CreateChain(64, 2, 20, 2, 32);
  RdbSaveCuckoo(stream.IO(), chain);
  stream.Rewind();

  auto* loaded = static_cast<CuckooChain*>(RdbLoadCuckoo(stream.IO(), 99));
  EXPECT_EQ(loaded, nullptr);

  DestroyChain(chain);
}

// ==================================================================
// Wire format: SerializeHeader / DeserializeHeader round-trip
// ==================================================================

TEST(CfWire, EmptyChainHeaderRoundTrip) {
  auto* chain = CreateChain(1024, 2, 20, 2, 32);

  size_t hdrSize = CfComputeHeaderSize(*chain);
  EXPECT_GT(hdrSize, sizeof(CfWireHeader));

  std::vector<uint8_t> buf(hdrSize);
  size_t written = CfSerializeHeader(*chain, buf.data());
  EXPECT_EQ(written, hdrSize);

  auto* loaded = CfDeserializeHeader(buf.data(), buf.size());
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->NumLayers(), chain->NumLayers());
  EXPECT_EQ(loaded->TotalItems(), chain->TotalItems());
  EXPECT_EQ(loaded->BucketSize(), chain->BucketSize());
  EXPECT_EQ(loaded->Expansion(), chain->Expansion());

  DestroyChain(chain);
  DestroyChain(loaded);
}

TEST(CfWire, FullScanDumpLoadChunkRoundTrip) {
  auto* chain = CreateChain(64, 2, 20, 2, 32);
  std::vector<std::string> items;
  for (int i = 0; i < 400; i++) {
    auto item = "scandump_" + std::to_string(i);
    if (chain->Add(ToSpan(item)).has_value()) items.push_back(item);
  }
  ASSERT_GT(chain->NumLayers(), 1u);

  // Phase 1: serialize header
  size_t hdrSize = CfComputeHeaderSize(*chain);
  std::vector<uint8_t> hdrBuf(hdrSize);
  CfSerializeHeader(*chain, hdrBuf.data());

  // Phase 2: save per-layer bucket arrays
  std::vector<std::vector<uint8_t>> layerData;
  for (const auto& layer : chain->Layers()) {
    layerData.emplace_back(layer.cuckoo.GetBucketArray(),
                            layer.cuckoo.GetBucketArray() + layer.cuckoo.GetDataSize());
  }

  // Phase 3: reconstruct from header
  auto* rebuilt = CfDeserializeHeader(hdrBuf.data(), hdrBuf.size());
  ASSERT_NE(rebuilt, nullptr);
  ASSERT_EQ(rebuilt->NumLayers(), layerData.size());

  // Phase 4: restore bucket arrays + recount
  for (size_t i = 0; i < layerData.size(); i++) {
    auto& dest = rebuilt->Layers()[i].cuckoo;
    ASSERT_EQ(dest.GetDataSize(), layerData[i].size());
    std::memcpy(dest.GetBucketArray(), layerData[i].data(), layerData[i].size());
    dest.RecountItems();
  }

  for (const auto& item : items) {
    EXPECT_TRUE(rebuilt->Contains(ToSpan(item)))
      << "False negative after wire-format round-trip: " << item;
  }

  DestroyChain(chain);
  DestroyChain(rebuilt);
}

// ==================================================================
// Wire format: invalid data rejected
// ==================================================================

TEST(CfWire, RejectsTruncatedHeader) {
  auto* result = CfDeserializeHeader("x", 1);
  EXPECT_EQ(result, nullptr);
}

TEST(CfWire, RejectsZeroLayers) {
  CfWireHeader hdr = {};
  hdr.totalItems = 0;
  hdr.numLayers = 0;
  hdr.bucketSize = 2;
  hdr.maxIterations = 20;
  hdr.expansion = 2;
  hdr.maxExpansions = 32;

  auto* result = CfDeserializeHeader(&hdr, sizeof(hdr));
  EXPECT_EQ(result, nullptr);
}

TEST(CfWire, RejectsTooManyLayers) {
  CfWireHeader hdr = {};
  hdr.numLayers = 99999;
  hdr.bucketSize = 2;
  hdr.maxIterations = 20;
  hdr.expansion = 2;
  hdr.maxExpansions = 32;

  auto* result = CfDeserializeHeader(&hdr, sizeof(hdr));
  EXPECT_EQ(result, nullptr);
}

TEST(CfWire, RejectsBucketSizeZero) {
  CfWireHeader hdr = {};
  hdr.numLayers = 1;
  hdr.bucketSize = 0;
  hdr.maxIterations = 20;
  hdr.expansion = 2;
  hdr.maxExpansions = 32;

  auto* result = CfDeserializeHeader(&hdr, sizeof(hdr));
  EXPECT_EQ(result, nullptr);
}

TEST(CfWire, RejectsMaxIterationsZero) {
  CfWireHeader hdr = {};
  hdr.numLayers = 1;
  hdr.bucketSize = 2;
  hdr.maxIterations = 0;
  hdr.expansion = 2;
  hdr.maxExpansions = 32;

  auto* result = CfDeserializeHeader(&hdr, sizeof(hdr));
  EXPECT_EQ(result, nullptr);
}

TEST(CfWire, RejectsNonPowerOfTwoBuckets) {
  auto* chain = CreateChain(1024, 2, 20, 2, 32);
  size_t hdrSize = CfComputeHeaderSize(*chain);
  std::vector<uint8_t> buf(hdrSize);
  CfSerializeHeader(*chain, buf.data());

  auto* meta = reinterpret_cast<CfWireLayerMeta*>(buf.data() + sizeof(CfWireHeader));
  meta[0].numBuckets = 100;  // not a power of two
  meta[0].dataSize = 100 * chain->BucketSize();

  auto* loaded = CfDeserializeHeader(buf.data(), buf.size());
  EXPECT_EQ(loaded, nullptr) << "Should reject non-power-of-two numBuckets";

  DestroyChain(chain);
  if (loaded) DestroyChain(loaded);
}

TEST(CfWire, RejectsDataSizeMismatch) {
  auto* chain = CreateChain(1024, 2, 20, 2, 32);
  size_t hdrSize = CfComputeHeaderSize(*chain);
  std::vector<uint8_t> buf(hdrSize);
  CfSerializeHeader(*chain, buf.data());

  auto* meta = reinterpret_cast<CfWireLayerMeta*>(buf.data() + sizeof(CfWireHeader));
  meta[0].dataSize -= 1;

  auto* loaded = CfDeserializeHeader(buf.data(), buf.size());
  EXPECT_EQ(loaded, nullptr) << "Should reject dataSize != numBuckets * bucketSize";

  DestroyChain(chain);
  if (loaded) DestroyChain(loaded);
}

// ==================================================================
// RDB: field validation on the raw stream
// ==================================================================

struct CustomChainFields {
  uint64_t totalItems = 0;
  uint64_t totalDeleted = 0;
  uint32_t numLayers = 1;
  uint32_t bucketSize = 2;
  uint32_t maxIterations = 20;
  uint32_t expansion = 2;
  uint32_t maxExpansions = 32;
  uint64_t loading = 0;
  uint64_t chunksLoaded = 0;
  uint64_t numBuckets = 128;
};

static CuckooChain* LoadCustomRdb(const CustomChainFields& f,
                                   size_t blobSizeOverride = static_cast<size_t>(-1)) {
  MockRdbStream stream;
  auto* io = stream.IO();

  Mock_SaveUnsigned(io, f.totalItems);
  Mock_SaveUnsigned(io, f.totalDeleted);
  Mock_SaveUnsigned(io, f.numLayers);
  Mock_SaveUnsigned(io, f.bucketSize);
  Mock_SaveUnsigned(io, f.maxIterations);
  Mock_SaveUnsigned(io, f.expansion);
  Mock_SaveUnsigned(io, f.maxExpansions);
  Mock_SaveUnsigned(io, f.loading);
  Mock_SaveUnsigned(io, f.chunksLoaded);

  for (uint32_t i = 0; i < f.numLayers; i++) {
    Mock_SaveUnsigned(io, f.numBuckets);
    uint64_t dataSize = f.numBuckets * f.bucketSize;
    size_t blobSize = blobSizeOverride == static_cast<size_t>(-1)
      ? static_cast<size_t>(dataSize) : blobSizeOverride;
    std::vector<char> bytes(blobSize, 0);
    Mock_SaveStringBuffer(io, bytes.data(), bytes.size());
  }

  stream.Rewind();
  return static_cast<CuckooChain*>(RdbLoadCuckoo(stream.IO(), kCfEncVerCurrent));
}

TEST(CfRdb, RejectsNonPowerOfTwoBucketsInRdb) {
  CustomChainFields f;
  f.numBuckets = 100;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr) << "Should reject non-power-of-two numBuckets";
  if (loaded) DestroyChain(loaded);
}

TEST(CfRdb, RejectsBucketSizeZero) {
  CustomChainFields f;
  f.bucketSize = 0;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr) << "Should reject bucketSize==0";
  if (loaded) DestroyChain(loaded);
}

TEST(CfRdb, RejectsBucketSizeTooLarge) {
  CustomChainFields f;
  f.bucketSize = kCfMaxBucketSize + 1;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr) << "Should reject bucketSize > kCfMaxBucketSize";
  if (loaded) DestroyChain(loaded);
}

TEST(CfRdb, RejectsMaxIterationsZero) {
  CustomChainFields f;
  f.maxIterations = 0;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr) << "Should reject maxIterations==0";
  if (loaded) DestroyChain(loaded);
}

TEST(CfRdb, RejectsMaxIterationsTooLarge) {
  CustomChainFields f;
  f.maxIterations = kCfMaxIterations + 1;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr) << "Should reject maxIterations > kCfMaxIterations";
  if (loaded) DestroyChain(loaded);
}

TEST(CfRdb, RejectsTotalItemsMismatch) {
  CustomChainFields f;
  f.totalItems = 999;  // blob is all-zero, so real itemSum is 0
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr) << "Should reject when totalItems != sum(layer item counts)";
  if (loaded) DestroyChain(loaded);
}

TEST(CfRdb, AcceptsConsistentEmptyChain) {
  CustomChainFields f;
  f.totalItems = 0;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_NE(loaded, nullptr) << "Should accept consistent all-zero chain";
  if (loaded) DestroyChain(loaded);
}

TEST(CfRdb, RejectsShortLayerBlob) {
  CustomChainFields f;
  uint64_t expected = f.numBuckets * f.bucketSize;
  auto* loaded = LoadCustomRdb(f, static_cast<size_t>(expected - 1));
  EXPECT_EQ(loaded, nullptr) << "Should reject layer blob shorter than numBuckets*bucketSize";
  if (loaded) DestroyChain(loaded);
}

TEST(CfRdb, RejectsLongLayerBlob) {
  CustomChainFields f;
  uint64_t expected = f.numBuckets * f.bucketSize;
  auto* loaded = LoadCustomRdb(f, static_cast<size_t>(expected + 1));
  EXPECT_EQ(loaded, nullptr) << "Should reject layer blob longer than numBuckets*bucketSize";
  if (loaded) DestroyChain(loaded);
}

TEST(CfRdb, RejectsExcessiveNumLayers) {
  MockRdbStream stream;
  auto* io = stream.IO();

  Mock_SaveUnsigned(io, 0);      // totalItems
  Mock_SaveUnsigned(io, 0);      // totalDeleted
  Mock_SaveUnsigned(io, 9999);   // numLayers (way over the 1024 cap)
  Mock_SaveUnsigned(io, 2);      // bucketSize
  Mock_SaveUnsigned(io, 20);     // maxIterations
  Mock_SaveUnsigned(io, 2);      // expansion
  Mock_SaveUnsigned(io, 32);     // maxExpansions
  Mock_SaveUnsigned(io, 0);      // loading
  Mock_SaveUnsigned(io, 0);      // chunksLoaded

  stream.Rewind();
  auto* loaded = static_cast<CuckooChain*>(RdbLoadCuckoo(stream.IO(), kCfEncVerCurrent));
  EXPECT_EQ(loaded, nullptr) << "Should reject RDB with numLayers > kCfMaxLayers";
  if (loaded) DestroyChain(loaded);
}

// Narrowing-cast bypass: a malicious/corrupt RDB writes (2^32 + N) for a
// uint32 field. ReadCfChain must reject values exceeding UINT32_MAX outright
// rather than silently truncating via static_cast<uint32_t>.
TEST(CfRdb, RejectsNumLayersHighBitBypass) {
  MockRdbStream stream;
  auto* io = stream.IO();

  Mock_SaveUnsigned(io, 0);
  Mock_SaveUnsigned(io, 0);
  Mock_SaveUnsigned(io, (1ULL << 32) + 1);  // numLayers with high bits set
  Mock_SaveUnsigned(io, 2);
  Mock_SaveUnsigned(io, 20);
  Mock_SaveUnsigned(io, 2);
  Mock_SaveUnsigned(io, 32);
  Mock_SaveUnsigned(io, 0);      // loading
  Mock_SaveUnsigned(io, 0);      // chunksLoaded

  stream.Rewind();
  auto* loaded = static_cast<CuckooChain*>(RdbLoadCuckoo(stream.IO(), kCfEncVerCurrent));
  EXPECT_EQ(loaded, nullptr) << "Should reject numLayers with high bits set (narrowing bypass)";
  if (loaded) DestroyChain(loaded);
}

// ==================================================================
// Module type callbacks: CopyCuckoo2 / DigestCuckoo / FreeEffortCuckoo2 /
// DefragCuckoo
// ==================================================================

TEST(CuckooModuleType, CopyCuckoo2ProducesIndependentEqualClone) {
  auto* chain = CreateChain(64, 2, 20, 2, 32);
  std::vector<std::string> items;
  for (int i = 0; i < 300; i++) {
    auto item = "copy2_" + std::to_string(i);
    if (chain->Add(ToSpan(item)).has_value()) items.push_back(item);
  }
  ASSERT_GT(chain->NumLayers(), 1u);

  auto* clone = static_cast<CuckooChain*>(CopyCuckoo2(nullptr, chain));
  ASSERT_NE(clone, nullptr);
  EXPECT_NE(clone, chain);
  EXPECT_EQ(clone->NumLayers(), chain->NumLayers());
  EXPECT_EQ(clone->TotalItems(), chain->TotalItems());

  for (const auto& item : items) {
    EXPECT_TRUE(clone->Contains(ToSpan(item)));
  }

  auto extra = std::string("post_copy");
  chain->Add(ToSpan(extra));
  EXPECT_FALSE(clone->Contains(ToSpan(extra)));

  DestroyChain(chain);
  DestroyChain(clone);
}

TEST(CuckooModuleType, DigestCuckooDeterministicAndContentSensitive) {
  auto* chain = CreateChain(64, 2, 20, 2, 32);
  for (int i = 0; i < 50; i++) {
    chain->Add(ToSpan(std::string("digest_" + std::to_string(i))));
  }

  MockDigest d1;
  DigestCuckoo(d1.Handle(), chain);
  EXPECT_TRUE(d1.ended);
  EXPECT_FALSE(d1.bytes.empty());

  MockDigest d2;
  DigestCuckoo(d2.Handle(), chain);
  EXPECT_EQ(d1.bytes, d2.bytes);

  chain->Add(ToSpan(std::string("extra_item")));
  MockDigest d3;
  DigestCuckoo(d3.Handle(), chain);
  EXPECT_NE(d1.bytes, d3.bytes);

  DestroyChain(chain);
}

TEST(CuckooModuleType, FreeEffortCuckoo2ReturnsAllocationCount) {
  auto* chain = CreateChain(16, 2, 20, 2, 32);
  EXPECT_EQ(FreeEffortCuckoo2(nullptr, chain), chain->NumLayers() + 1);

  for (int i = 0; i < 500; i++) {
    chain->Add(ToSpan(std::string("effort_" + std::to_string(i))));
  }
  ASSERT_GT(chain->NumLayers(), 1u);
  EXPECT_EQ(FreeEffortCuckoo2(nullptr, chain), chain->NumLayers() + 1);

  DestroyChain(chain);
}

TEST(CuckooModuleType, DefragCuckooRelocatesBuffersAndPreservesData) {
  auto* chain = CreateChain(32, 2, 20, 2, 32);
  std::vector<std::string> items;
  for (int i = 0; i < 300; i++) {
    auto item = "defrag_" + std::to_string(i);
    if (chain->Add(ToSpan(item)).has_value()) items.push_back(item);
  }
  ASSERT_GT(chain->NumLayers(), 1u);

  auto* origLayersPtr = chain->Layers().data();
  std::vector<const uint8_t*> origBucketArrays;
  for (auto& layer : chain->Layers()) {
    origBucketArrays.push_back(layer.cuckoo.GetBucketArray());
  }

  void* value = chain;
  int rc = DefragCuckoo(nullptr, nullptr, &value);
  EXPECT_EQ(rc, 0);

  auto* relocated = static_cast<CuckooChain*>(value);
  ASSERT_NE(relocated, nullptr);
  EXPECT_NE(relocated->Layers().data(), origLayersPtr);
  ASSERT_EQ(relocated->NumLayers(), origBucketArrays.size());
  for (size_t i = 0; i < origBucketArrays.size(); i++) {
    EXPECT_NE(relocated->Layers()[i].cuckoo.GetBucketArray(), origBucketArrays[i]);
  }

  for (const auto& item : items) {
    EXPECT_TRUE(relocated->Contains(ToSpan(item))) << "False negative after defrag for " << item;
  }

  DestroyChain(relocated);
}

// ==================================================================
// Loading state must survive an RDB round-trip: a chain that is still
// mid-LOADCHUNK when the server saves (e.g. DEBUG RELOAD) has layers whose
// bucket data isn't populated yet, so recomputing itemSum on load would
// disagree with the stale totalItems_ carried over from the source filter.
// Persisting loading/chunksLoaded_ lets ReadCfChain skip that mismatch and
// keep rejecting reads on the reloaded key until the rest of LOADCHUNK
// completes, instead of failing the whole RDB load.
// ==================================================================

TEST(CfRdb, NotLoadingChainRoundTripsWithoutLoadingFlag) {
  auto* chain = CreateChain(64, 2, 20, 2, 32);
  chain->Add(ToSpan(std::string("x")));

  auto* loaded = CfRdbRoundTrip(chain, kCfEncVerCurrent);
  ASSERT_NE(loaded, nullptr);
  EXPECT_FALSE(loaded->IsLoading());
  EXPECT_TRUE(loaded->Contains(ToSpan(std::string("x"))));

  DestroyChain(chain);
  DestroyChain(loaded);
}

TEST(CfRdb, LoadingChainSurvivesRoundTripStillLoading) {
  auto* chain = CreateChain(64, 2, 20, 2, 32);
  chain->Add(ToSpan(std::string("x")));
  chain->SetLoading();
  chain->IncrementChunksLoaded();
  EXPECT_TRUE(chain->IsLoading());

  auto* loaded = CfRdbRoundTrip(chain, kCfEncVerCurrent);
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(loaded->IsLoading());
  EXPECT_EQ(loaded->ChunksLoaded(), 1u);

  DestroyChain(chain);
  DestroyChain(loaded);
}

// TotalItems()/TotalDeleted() are cumulative counters that never decrease
// (see cf_chain.h), so after a delete the real item count recoverable from
// the bucket arrays is totalItems_ - totalDeleted_, not totalItems_ alone.
// ReadCfChain must check against that difference, or any chain with a net
// deletion fails to round-trip through RDB (e.g. DEBUG RELOAD).
TEST(CfRdb, ChainWithDeletedItemRoundTrips) {
  auto* chain = CreateChain(64, 2, 20, 2, 32);
  chain->Add(ToSpan(std::string("x")));
  EXPECT_TRUE(chain->Delete(ToSpan(std::string("x"))));

  auto* loaded = CfRdbRoundTrip(chain, kCfEncVerCurrent);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->TotalItems(), 1u);
  EXPECT_EQ(loaded->TotalDeleted(), 1u);
  EXPECT_FALSE(loaded->Contains(ToSpan(std::string("x"))));

  DestroyChain(chain);
  DestroyChain(loaded);
}
