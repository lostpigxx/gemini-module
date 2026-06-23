// RDB and wire-format serialization round-trip tests for gemini-bloom.
// Verifies RDB encver 2/4, wire-format (SCANDUMP/LOADCHUNK) headers,
// layer metadata, and data integrity after serialization.

#define REDISMODULE_API
extern "C" {
#include "redismodule.h"
}

#include "mock_redismodule_io.h"

#include <gtest/gtest.h>
#include "bloom_rdb.h"
#include "sb_chain.h"
#include "bloom_filter.h"
#include "rm_alloc.h"

#include <cstring>
#include <string>
#include <vector>

// Install mock IO before any tests.
class BloomRdbTestEnv : public ::testing::Environment {
public:
  void SetUp() override { InstallMockRedisModuleIO(); }
};
static auto* const gEnv [[maybe_unused]] =
  ::testing::AddGlobalTestEnvironment(new BloomRdbTestEnv);

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

static auto DefaultFlags() {
  return BloomFlags::Use64Bit | BloomFlags::NoRound;
}

static ScalingBloomFilter* CreateFilter(uint64_t cap, double fp,
                                         BloomFlags flags, unsigned exp) {
  auto* mem = static_cast<ScalingBloomFilter*>(malloc(sizeof(ScalingBloomFilter)));
  new (mem) ScalingBloomFilter(cap, fp, flags, exp);
  return mem;
}

static void DestroyFilter(ScalingBloomFilter* f) {
  if (f) { f->~ScalingBloomFilter(); free(f); }
}

static ScalingBloomFilter* RdbRoundTrip(ScalingBloomFilter* src, int load_encver) {
  MockRdbStream stream;
  RdbSaveBloom(stream.IO(), src);
  stream.Rewind();
  return static_cast<ScalingBloomFilter*>(RdbLoadBloom(stream.IO(), load_encver));
}

static std::span<const std::byte> ToSpan(const std::string& s) {
  return AsBytes(s.data(), s.size());
}

// ==================================================================
// RDB round-trip: empty filter
// ==================================================================

TEST(BloomRdb, EmptyFilterRoundTrip) {
  auto* filter = CreateFilter(1000, 0.01, DefaultFlags(), 2);
  ASSERT_TRUE(filter->IsValid());
  EXPECT_EQ(filter->TotalItems(), 0u);
  EXPECT_EQ(filter->NumLayers(), 1u);

  auto* loaded = RdbRoundTrip(filter, kCurrentEncVer);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->TotalItems(), 0u);
  EXPECT_EQ(loaded->NumLayers(), 1u);
  EXPECT_EQ(loaded->ExpansionFactor(), 2u);
  EXPECT_EQ(loaded->Flags(), DefaultFlags());

  DestroyFilter(filter);
  DestroyFilter(loaded);
}

// ==================================================================
// RDB round-trip: filter with data — no false negatives
// ==================================================================

TEST(BloomRdb, PopulatedFilterRoundTrip) {
  auto* filter = CreateFilter(1000, 0.01, DefaultFlags(), 2);

  std::vector<std::string> items;
  for (int i = 0; i < 500; i++) {
    items.push_back("item_" + std::to_string(i));
    filter->Put(ToSpan(items.back()));
  }
  EXPECT_EQ(filter->TotalItems(), 500u);

  auto* loaded = RdbRoundTrip(filter, kCurrentEncVer);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->TotalItems(), 500u);

  // Every inserted item MUST be found — zero false negatives
  for (const auto& item : items) {
    EXPECT_TRUE(loaded->Contains(ToSpan(item)))
      << "False negative after RDB round-trip: " << item;
  }

  DestroyFilter(filter);
  DestroyFilter(loaded);
}

// ==================================================================
// RDB round-trip: multi-layer filter (auto-expanded)
// ==================================================================

TEST(BloomRdb, MultiLayerRoundTrip) {
  auto* filter = CreateFilter(100, 0.01, DefaultFlags(), 2);

  std::vector<std::string> items;
  for (int i = 0; i < 500; i++) {
    items.push_back("multi_" + std::to_string(i));
    filter->Put(ToSpan(items.back()));
  }
  EXPECT_GT(filter->NumLayers(), 1u);
  size_t orig_layers = filter->NumLayers();
  size_t orig_items = filter->TotalItems();

  auto* loaded = RdbRoundTrip(filter, kCurrentEncVer);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->NumLayers(), orig_layers);
  EXPECT_EQ(loaded->TotalItems(), orig_items);

  for (const auto& item : items) {
    EXPECT_TRUE(loaded->Contains(ToSpan(item)));
  }

  DestroyFilter(filter);
  DestroyFilter(loaded);
}

// ==================================================================
// RDB round-trip: metadata preserved
// ==================================================================

TEST(BloomRdb, MetadataPreserved) {
  auto flags = BloomFlags::Use64Bit | BloomFlags::NoRound;
  auto* filter = CreateFilter(2000, 0.001, flags, 4);
  filter->Put(AsBytes("x", 1));

  auto* loaded = RdbRoundTrip(filter, kCurrentEncVer);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->Flags(), flags);
  EXPECT_EQ(loaded->ExpansionFactor(), 4u);
  EXPECT_EQ(loaded->NumLayers(), 1u);

  // Layer-level metadata
  auto orig_layers = filter->Layers();
  auto load_layers = loaded->Layers();
  EXPECT_EQ(orig_layers[0].bloom.GetCapacity(), load_layers[0].bloom.GetCapacity());
  EXPECT_DOUBLE_EQ(orig_layers[0].bloom.GetFpRate(), load_layers[0].bloom.GetFpRate());
  EXPECT_EQ(orig_layers[0].bloom.GetHashCount(), load_layers[0].bloom.GetHashCount());
  EXPECT_EQ(orig_layers[0].bloom.GetTotalBits(), load_layers[0].bloom.GetTotalBits());
  EXPECT_EQ(orig_layers[0].bloom.GetDataSize(), load_layers[0].bloom.GetDataSize());
  EXPECT_EQ(orig_layers[0].bloom.GetLog2Bits(), load_layers[0].bloom.GetLog2Bits());
  EXPECT_DOUBLE_EQ(orig_layers[0].bloom.GetBitsPerEntry(),
                   load_layers[0].bloom.GetBitsPerEntry());

  DestroyFilter(filter);
  DestroyFilter(loaded);
}

// ==================================================================
// RDB round-trip: bit array exact binary match
// ==================================================================

TEST(BloomRdb, BitArrayExactMatch) {
  auto* filter = CreateFilter(1000, 0.01, DefaultFlags(), 2);
  for (int i = 0; i < 200; i++) {
    auto s = "bits_" + std::to_string(i);
    filter->Put(ToSpan(s));
  }

  // Save original bit arrays
  std::vector<std::vector<uint8_t>> orig_bits;
  for (const auto& layer : filter->Layers()) {
    orig_bits.emplace_back(layer.bloom.GetBitArray(),
                           layer.bloom.GetBitArray() + layer.bloom.GetDataSize());
  }

  auto* loaded = RdbRoundTrip(filter, kCurrentEncVer);
  ASSERT_NE(loaded, nullptr);
  ASSERT_EQ(loaded->NumLayers(), orig_bits.size());

  for (size_t i = 0; i < orig_bits.size(); i++) {
    auto& layer = loaded->Layers()[i];
    ASSERT_EQ(layer.bloom.GetDataSize(), orig_bits[i].size());
    EXPECT_EQ(std::memcmp(layer.bloom.GetBitArray(), orig_bits[i].data(),
                          orig_bits[i].size()), 0)
      << "Bit array mismatch in layer " << i;
  }

  DestroyFilter(filter);
  DestroyFilter(loaded);
}

// ==================================================================
// RDB round-trip: FixedSize flag preserved, no extra layers
// ==================================================================

TEST(BloomRdb, FixedSizeRoundTrip) {
  auto flags = DefaultFlags() | BloomFlags::FixedSize;
  auto* filter = CreateFilter(100, 0.01, flags, 2);

  for (int i = 0; i < 50; i++) {
    auto s = "fixed_" + std::to_string(i);
    filter->Put(ToSpan(s));
  }
  EXPECT_EQ(filter->NumLayers(), 1u);
  EXPECT_TRUE(HasFlag(filter->Flags(), BloomFlags::FixedSize));

  auto* loaded = RdbRoundTrip(filter, kCurrentEncVer);
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(HasFlag(loaded->Flags(), BloomFlags::FixedSize));
  EXPECT_EQ(loaded->NumLayers(), 1u);

  for (int i = 0; i < 50; i++) {
    auto s = "fixed_" + std::to_string(i);
    EXPECT_TRUE(loaded->Contains(ToSpan(s)));
  }

  DestroyFilter(filter);
  DestroyFilter(loaded);
}

// ==================================================================
// RDB encver backward compat: load encver 2 (no expansion field)
// ==================================================================

TEST(BloomRdb, EncVer2BackwardCompat) {
  // Manually serialize in encver-2 format (no expansion factor written)
  auto* filter = CreateFilter(500, 0.01, DefaultFlags(), 2);
  for (int i = 0; i < 100; i++) {
    auto s = "v2_" + std::to_string(i);
    filter->Put(ToSpan(s));
  }

  // Serialize using current format
  MockRdbStream stream;
  RdbSaveBloom(stream.IO(), filter);

  // Now load it pretending encver=2 won't work directly (the format
  // saved includes expansion factor). Instead, manually build an
  // encver-2 stream: totalItems, numLayers, flags (no expansion), layers.
  MockRdbStream v2stream;
  auto* io = v2stream.IO();
  Mock_SaveUnsigned(io, filter->TotalItems());
  Mock_SaveUnsigned(io, filter->NumLayers());
  Mock_SaveUnsigned(io, ToUnderlying(filter->Flags()));
  // NOTE: no expansion factor in encver 2!

  for (const auto& layer : filter->Layers()) {
    Mock_SaveUnsigned(io, layer.bloom.GetCapacity());
    Mock_SaveDouble(io, layer.bloom.GetFpRate());
    Mock_SaveUnsigned(io, layer.bloom.GetHashCount());
    Mock_SaveDouble(io, layer.bloom.GetBitsPerEntry());
    Mock_SaveUnsigned(io, layer.bloom.GetTotalBits());
    Mock_SaveUnsigned(io, layer.bloom.GetLog2Bits());
    Mock_SaveStringBuffer(io,
      reinterpret_cast<const char*>(layer.bloom.GetBitArray()),
      layer.bloom.GetDataSize());
    Mock_SaveUnsigned(io, layer.itemCount);
  }

  v2stream.Rewind();
  auto* loaded = static_cast<ScalingBloomFilter*>(RdbLoadBloom(v2stream.IO(), 2));
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->TotalItems(), filter->TotalItems());
  EXPECT_EQ(loaded->NumLayers(), filter->NumLayers());
  // encver 2 defaults expansion to 2
  EXPECT_EQ(loaded->ExpansionFactor(), 2u);

  for (int i = 0; i < 100; i++) {
    auto s = "v2_" + std::to_string(i);
    EXPECT_TRUE(loaded->Contains(ToSpan(s)));
  }

  DestroyFilter(filter);
  DestroyFilter(loaded);
}

// ==================================================================
// RDB: rejected unknown encver
// ==================================================================

TEST(BloomRdb, RejectsUnknownEncver) {
  MockRdbStream stream;
  auto* filter = CreateFilter(100, 0.01, DefaultFlags(), 2);
  RdbSaveBloom(stream.IO(), filter);
  stream.Rewind();

  auto* loaded = static_cast<ScalingBloomFilter*>(RdbLoadBloom(stream.IO(), 99));
  EXPECT_EQ(loaded, nullptr);

  DestroyFilter(filter);
}

// ==================================================================
// Wire format: SerializeHeader / DeserializeHeader round-trip
// ==================================================================

TEST(BloomWire, EmptyFilterHeaderRoundTrip) {
  auto* filter = CreateFilter(1000, 0.01, DefaultFlags(), 2);

  size_t hdr_size = ComputeHeaderSize(*filter);
  EXPECT_GT(hdr_size, sizeof(WireFilterHeader));

  std::vector<uint8_t> buf(hdr_size);
  size_t written = SerializeHeader(*filter, buf.data());
  EXPECT_EQ(written, hdr_size);

  auto* loaded = DeserializeHeader(buf.data(), buf.size());
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->NumLayers(), filter->NumLayers());
  EXPECT_EQ(loaded->TotalItems(), filter->TotalItems());
  EXPECT_EQ(loaded->Flags(), filter->Flags());
  EXPECT_EQ(loaded->ExpansionFactor(), filter->ExpansionFactor());

  DestroyFilter(filter);
  DestroyFilter(loaded);
}

TEST(BloomWire, PopulatedFilterHeaderRoundTrip) {
  auto* filter = CreateFilter(100, 0.01, DefaultFlags(), 2);
  for (int i = 0; i < 300; i++) {
    auto s = "wire_" + std::to_string(i);
    filter->Put(ToSpan(s));
  }
  EXPECT_GT(filter->NumLayers(), 1u);

  size_t hdr_size = ComputeHeaderSize(*filter);
  std::vector<uint8_t> buf(hdr_size);
  SerializeHeader(*filter, buf.data());

  auto* loaded = DeserializeHeader(buf.data(), buf.size());
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->NumLayers(), filter->NumLayers());
  EXPECT_EQ(loaded->TotalItems(), filter->TotalItems());

  DestroyFilter(filter);
  DestroyFilter(loaded);
}

// ==================================================================
// Wire format: full SCANDUMP/LOADCHUNK simulation
// ==================================================================

TEST(BloomWire, FullScanDumpLoadChunkRoundTrip) {
  auto* filter = CreateFilter(200, 0.01, DefaultFlags(), 2);
  std::vector<std::string> items;
  for (int i = 0; i < 400; i++) {
    items.push_back("scandump_" + std::to_string(i));
    filter->Put(ToSpan(items.back()));
  }
  EXPECT_GT(filter->NumLayers(), 1u);

  // Phase 1: serialize header
  size_t hdr_size = ComputeHeaderSize(*filter);
  std::vector<uint8_t> hdr_buf(hdr_size);
  SerializeHeader(*filter, hdr_buf.data());

  // Phase 2: save per-layer bit arrays
  std::vector<std::vector<uint8_t>> layer_data;
  for (const auto& layer : filter->Layers()) {
    layer_data.emplace_back(layer.bloom.GetBitArray(),
                            layer.bloom.GetBitArray() + layer.bloom.GetDataSize());
  }

  // Phase 3: reconstruct from header
  auto* rebuilt = DeserializeHeader(hdr_buf.data(), hdr_buf.size());
  ASSERT_NE(rebuilt, nullptr);
  ASSERT_EQ(rebuilt->NumLayers(), layer_data.size());

  // Phase 4: restore bit arrays
  for (size_t i = 0; i < layer_data.size(); i++) {
    auto& dest = rebuilt->Layers()[i].bloom;
    ASSERT_EQ(dest.GetDataSize(), layer_data[i].size());
    std::memcpy(dest.GetBitArray(), layer_data[i].data(), layer_data[i].size());
  }

  // Verify: every item must be found
  for (const auto& item : items) {
    EXPECT_TRUE(rebuilt->Contains(ToSpan(item)))
      << "False negative after wire-format round-trip: " << item;
  }

  DestroyFilter(filter);
  DestroyFilter(rebuilt);
}

// ==================================================================
// Wire format: layer metadata round-trip
// ==================================================================

TEST(BloomWire, LayerMetaRoundTrip) {
  auto layer = BloomLayer::Create(1000, 0.01, DefaultFlags());
  ASSERT_TRUE(layer.has_value());

  // Insert some data so the bit array is non-zero
  auto hp = Hash64Policy::Compute(AsBytes("test", 4));
  layer->Insert(hp);

  WireLayerMeta meta = layer->ToWireMeta(42);
  EXPECT_EQ(meta.itemCount, 42u);
  EXPECT_EQ(meta.capacity, layer->GetCapacity());
  EXPECT_DOUBLE_EQ(meta.fpRate, layer->GetFpRate());
  EXPECT_EQ(meta.hashCount, layer->GetHashCount());
  EXPECT_EQ(meta.totalBits, layer->GetTotalBits());
  EXPECT_EQ(meta.dataSize, layer->GetDataSize());
  EXPECT_EQ(meta.log2Bits, layer->GetLog2Bits());
  EXPECT_DOUBLE_EQ(meta.bitsPerEntry, layer->GetBitsPerEntry());

  auto restored = BloomLayer::FromWireMeta(meta, DefaultFlags());
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(restored->GetCapacity(), layer->GetCapacity());
  EXPECT_DOUBLE_EQ(restored->GetFpRate(), layer->GetFpRate());
  EXPECT_EQ(restored->GetHashCount(), layer->GetHashCount());
  EXPECT_EQ(restored->GetTotalBits(), layer->GetTotalBits());
  EXPECT_EQ(restored->GetDataSize(), layer->GetDataSize());
}

// ==================================================================
// Wire format: invalid data rejected
// ==================================================================

TEST(BloomWire, RejectsTruncatedHeader) {
  auto* result = DeserializeHeader("x", 1);
  EXPECT_EQ(result, nullptr);
}

TEST(BloomWire, RejectsZeroLayers) {
  WireFilterHeader hdr = {};
  hdr.totalItems = 10;
  hdr.numLayers = 0;
  hdr.flags = ToUnderlying(DefaultFlags());
  hdr.expansionFactor = 2;

  auto* result = DeserializeHeader(&hdr, sizeof(hdr));
  EXPECT_EQ(result, nullptr);
}

TEST(BloomWire, RejectsTooManyLayers) {
  WireFilterHeader hdr = {};
  hdr.numLayers = 9999;
  hdr.flags = ToUnderlying(DefaultFlags());

  auto* result = DeserializeHeader(&hdr, sizeof(hdr));
  EXPECT_EQ(result, nullptr);
}

// ==================================================================
// RDB stress: repeated round-trips
// ==================================================================

TEST(BloomRdbStress, RepeatedRoundTrips) {
  auto* filter = CreateFilter(500, 0.01, DefaultFlags(), 2);
  for (int i = 0; i < 200; i++) {
    auto s = "stress_" + std::to_string(i);
    filter->Put(ToSpan(s));
  }

  for (int round = 0; round < 50; round++) {
    auto* loaded = RdbRoundTrip(filter, kCurrentEncVer);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->TotalItems(), filter->TotalItems());
    EXPECT_EQ(loaded->NumLayers(), filter->NumLayers());

    // Spot-check a few items
    EXPECT_TRUE(loaded->Contains(ToSpan(std::string("stress_0"))));
    EXPECT_TRUE(loaded->Contains(ToSpan(std::string("stress_199"))));

    DestroyFilter(loaded);
  }

  DestroyFilter(filter);
}

// ==================================================================
// RDB: different expansion factors
// ==================================================================

TEST(BloomRdb, ExpansionFactors) {
  for (unsigned exp : {1u, 2u, 4u, 8u}) {
    auto* filter = CreateFilter(100, 0.01, DefaultFlags(), exp);
    for (int i = 0; i < 50; i++) {
      auto s = "exp_" + std::to_string(exp) + "_" + std::to_string(i);
      filter->Put(ToSpan(s));
    }

    auto* loaded = RdbRoundTrip(filter, kCurrentEncVer);
    ASSERT_NE(loaded, nullptr) << "Failed for expansion=" << exp;
    EXPECT_EQ(loaded->ExpansionFactor(), exp);

    for (int i = 0; i < 50; i++) {
      auto s = "exp_" + std::to_string(exp) + "_" + std::to_string(i);
      EXPECT_TRUE(loaded->Contains(ToSpan(s)));
    }

    DestroyFilter(filter);
    DestroyFilter(loaded);
  }
}

// ==================================================================
// RDB: different FP rates
// ==================================================================

TEST(BloomRdb, DifferentFpRates) {
  for (double fp : {0.1, 0.01, 0.001, 0.0001}) {
    auto* filter = CreateFilter(500, fp, DefaultFlags(), 2);
    for (int i = 0; i < 100; i++) {
      auto s = "fp_" + std::to_string(i);
      filter->Put(ToSpan(s));
    }

    auto* loaded = RdbRoundTrip(filter, kCurrentEncVer);
    ASSERT_NE(loaded, nullptr) << "Failed for fp=" << fp;

    // Verify layer FP rate is preserved
    EXPECT_DOUBLE_EQ(loaded->Layers()[0].bloom.GetFpRate(),
                     filter->Layers()[0].bloom.GetFpRate());

    for (int i = 0; i < 100; i++) {
      auto s = "fp_" + std::to_string(i);
      EXPECT_TRUE(loaded->Contains(ToSpan(s)));
    }

    DestroyFilter(filter);
    DestroyFilter(loaded);
  }
}

// ==================================================================
// Wire format: header size computation
// ==================================================================

// ==================================================================
// Bug regression: SetLayer on calloc'd memory must use placement new,
// not move-assignment on an unconstructed FilterLayer.
// This test verifies that a wire-format deserialized filter survives
// SetLayer and correctly answers membership queries.
// ==================================================================

TEST(BloomWire, SetLayerOnUninitializedMemory) {
  auto* filter = CreateFilter(100, 0.01, DefaultFlags(), 2);
  std::vector<std::string> items;
  for (int i = 0; i < 200; i++) {
    items.push_back("setlayer_" + std::to_string(i));
    filter->Put(ToSpan(items.back()));
  }
  EXPECT_GT(filter->NumLayers(), 1u);

  size_t hdr_size = ComputeHeaderSize(*filter);
  std::vector<uint8_t> hdr_buf(hdr_size);
  SerializeHeader(*filter, hdr_buf.data());

  std::vector<std::vector<uint8_t>> layer_data;
  for (const auto& layer : filter->Layers()) {
    layer_data.emplace_back(layer.bloom.GetBitArray(),
                            layer.bloom.GetBitArray() + layer.bloom.GetDataSize());
  }

  auto* rebuilt = DeserializeHeader(hdr_buf.data(), hdr_buf.size());
  ASSERT_NE(rebuilt, nullptr);

  for (size_t i = 0; i < layer_data.size(); i++) {
    auto& dest = rebuilt->Layers()[i].bloom;
    ASSERT_EQ(dest.GetDataSize(), layer_data[i].size());
    std::memcpy(dest.GetBitArray(), layer_data[i].data(), layer_data[i].size());
  }

  for (const auto& item : items) {
    EXPECT_TRUE(rebuilt->Contains(ToSpan(item)))
      << "False negative after SetLayer: " << item;
  }

  DestroyFilter(filter);
  DestroyFilter(rebuilt);
}

// ==================================================================
// Bug regression: RDB load must check r.Ok() after reading per-layer
// item count. A truncated stream missing the item count should not
// produce a filter with wrong data.
// ==================================================================

TEST(BloomRdb, TruncatedItemCountReturnsNull) {
  auto* filter = CreateFilter(500, 0.01, DefaultFlags(), 2);
  for (int i = 0; i < 100; i++) {
    auto s = "trunc_" + std::to_string(i);
    filter->Put(ToSpan(s));
  }
  ASSERT_EQ(filter->NumLayers(), 1u);

  // Build a manually truncated RDB stream: write the filter header
  // and the first layer's BloomLayer data, but omit the itemCount
  // uint64 that follows it.
  MockRdbStream truncStream;
  auto* io = truncStream.IO();

  // Filter-level header
  Mock_SaveUnsigned(io, filter->TotalItems());
  Mock_SaveUnsigned(io, 1);  // numLayers = 1
  Mock_SaveUnsigned(io, ToUnderlying(filter->Flags()));
  Mock_SaveUnsigned(io, filter->ExpansionFactor());

  // Layer 0 BloomLayer fields (complete)
  const auto& layer0 = filter->Layers()[0];
  Mock_SaveUnsigned(io, layer0.bloom.GetCapacity());
  Mock_SaveDouble(io, layer0.bloom.GetFpRate());
  Mock_SaveUnsigned(io, layer0.bloom.GetHashCount());
  Mock_SaveDouble(io, layer0.bloom.GetBitsPerEntry());
  Mock_SaveUnsigned(io, layer0.bloom.GetTotalBits());
  Mock_SaveUnsigned(io, layer0.bloom.GetLog2Bits());
  Mock_SaveStringBuffer(io,
    reinterpret_cast<const char*>(layer0.bloom.GetBitArray()),
    layer0.bloom.GetDataSize());
  // DELIBERATELY omit: Mock_SaveUnsigned(io, layer0.itemCount);

  truncStream.Rewind();
  auto* loaded = static_cast<ScalingBloomFilter*>(
    RdbLoadBloom(truncStream.IO(), kCurrentEncVer));

  // With the fix: the loader detects the truncated itemCount via
  // r.Ok() and returns nullptr.
  // Before the fix: itemCount silently reads 0 (default from
  // GetUint's error path) and loading "succeeds" with wrong data.
  EXPECT_EQ(loaded, nullptr)
    << "Expected nullptr for stream truncated before itemCount";

  if (loaded) DestroyFilter(loaded);
  DestroyFilter(filter);
}

// ==================================================================
// Bug regression: RDB with log2Bits >= 64 must be rejected.
// Without the fix, ComputeModuloMask() does 1ULL << log2Bits
// which is undefined behavior for values >= 64.
// ==================================================================

TEST(BloomRdb, RejectsInvalidLog2Bits) {
  auto* filter = CreateFilter(500, 0.01, DefaultFlags(), 2);
  filter->Put(AsBytes("x", 1));

  // Build a stream with log2Bits = 64 (invalid)
  MockRdbStream badStream;
  auto* io = badStream.IO();

  Mock_SaveUnsigned(io, filter->TotalItems());
  Mock_SaveUnsigned(io, 1);  // numLayers
  Mock_SaveUnsigned(io, ToUnderlying(filter->Flags()));
  Mock_SaveUnsigned(io, filter->ExpansionFactor());

  const auto& layer0 = filter->Layers()[0];
  Mock_SaveUnsigned(io, layer0.bloom.GetCapacity());
  Mock_SaveDouble(io, layer0.bloom.GetFpRate());
  Mock_SaveUnsigned(io, layer0.bloom.GetHashCount());
  Mock_SaveDouble(io, layer0.bloom.GetBitsPerEntry());
  Mock_SaveUnsigned(io, layer0.bloom.GetTotalBits());
  Mock_SaveUnsigned(io, 64);  // log2Bits = 64 (INVALID)
  Mock_SaveStringBuffer(io,
    reinterpret_cast<const char*>(layer0.bloom.GetBitArray()),
    layer0.bloom.GetDataSize());
  Mock_SaveUnsigned(io, layer0.itemCount);

  badStream.Rewind();
  auto* loaded = static_cast<ScalingBloomFilter*>(
    RdbLoadBloom(badStream.IO(), kCurrentEncVer));

  EXPECT_EQ(loaded, nullptr)
    << "Should reject RDB with log2Bits >= 64";

  if (loaded) DestroyFilter(loaded);
  DestroyFilter(filter);
}

// ==================================================================
// Bug regression: LOADCHUNK header with log2Bits >= 64 must be
// rejected by ValidateLayerMeta.
// ==================================================================

TEST(BloomWire, RejectsHeaderWithBadLog2Bits) {
  auto* filter = CreateFilter(100, 0.01, DefaultFlags(), 2);
  filter->Put(AsBytes("x", 1));

  size_t hdr_size = ComputeHeaderSize(*filter);
  std::vector<uint8_t> buf(hdr_size);
  SerializeHeader(*filter, buf.data());

  // Corrupt the first layer's log2Bits to 65
  auto* meta = reinterpret_cast<WireLayerMeta*>(buf.data() + sizeof(WireFilterHeader));
  meta[0].log2Bits = 65;

  auto* loaded = DeserializeHeader(buf.data(), buf.size());
  EXPECT_EQ(loaded, nullptr)
    << "Should reject header with log2Bits >= 64";

  if (loaded) DestroyFilter(loaded);
  DestroyFilter(filter);
}

// ==================================================================
// Bug regression: RDB numLayers must be capped at kMaxLayers.
// ==================================================================

TEST(BloomRdb, RejectsExcessiveNumLayers) {
  MockRdbStream bigStream;
  auto* io = bigStream.IO();

  Mock_SaveUnsigned(io, 0);      // totalItems
  Mock_SaveUnsigned(io, 9999);   // numLayers (way over kMaxLayers=1024)
  Mock_SaveUnsigned(io, ToUnderlying(DefaultFlags()));
  Mock_SaveUnsigned(io, 2);      // expansion

  bigStream.Rewind();
  auto* loaded = static_cast<ScalingBloomFilter*>(
    RdbLoadBloom(bigStream.IO(), kCurrentEncVer));

  EXPECT_EQ(loaded, nullptr)
    << "Should reject RDB with numLayers > kMaxLayers";

  if (loaded) DestroyFilter(loaded);
}

TEST(BloomWire, HeaderSizeScalesWithLayers) {
  auto* f1 = CreateFilter(100, 0.01, DefaultFlags(), 2);
  size_t s1 = ComputeHeaderSize(*f1);
  EXPECT_EQ(s1, sizeof(WireFilterHeader) + 1 * sizeof(WireLayerMeta));

  // Force multiple layers
  auto* f3 = CreateFilter(10, 0.01, DefaultFlags(), 2);
  for (int i = 0; i < 200; i++) {
    auto s = "grow_" + std::to_string(i);
    f3->Put(ToSpan(s));
  }
  EXPECT_GT(f3->NumLayers(), 1u);
  size_t s3 = ComputeHeaderSize(*f3);
  EXPECT_EQ(s3, sizeof(WireFilterHeader) + f3->NumLayers() * sizeof(WireLayerMeta));

  DestroyFilter(f1);
  DestroyFilter(f3);
}
