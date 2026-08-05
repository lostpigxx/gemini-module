// RDB and wire-format serialization round-trip tests for the gemini-bloom
// Top-K data type. Verifies RDB encver 1, LOADCHUNK wire format, field
// validation, and module type callbacks.

#define REDISMODULE_API
extern "C" {
#include "redismodule.h"
}

#include "mock_redismodule_io.h"

#include <gtest/gtest.h>
#include "topk_rdb.h"
#include "topk_sketch.h"
#include "rm_alloc.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Install mock IO before any tests.
class TopkRdbTestEnv : public ::testing::Environment {
public:
  void SetUp() override { InstallMockRedisModuleIO(); }
};
static auto* const gEnv [[maybe_unused]] =
  ::testing::AddGlobalTestEnvironment(new TopkRdbTestEnv);

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

static TopkSketch* CreateSketch(uint32_t k, uint32_t width, uint32_t depth, double decay) {
  auto maybe = TopkSketch::Create(k, width, depth, decay);
  if (!maybe) return nullptr;
  auto* sketch = static_cast<TopkSketch*>(RMAlloc(sizeof(TopkSketch)));
  new (sketch) TopkSketch(std::move(*maybe));
  return sketch;
}

static void DestroySketch(TopkSketch* sketch) {
  if (sketch) { sketch->~TopkSketch(); RMFree(sketch); }
}

static TopkSketch* TopkRdbRoundTrip(TopkSketch* src, int load_encver) {
  MockRdbStream stream;
  RdbSaveTopk(stream.IO(), src);
  stream.Rewind();
  return static_cast<TopkSketch*>(RdbLoadTopk(stream.IO(), load_encver));
}

static std::span<const std::byte> ToSpan(const std::string& s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

// ==================================================================
// RDB round-trip: empty sketch
// ==================================================================

TEST(TopkRdb, EmptySketchRoundTrip) {
  auto* sketch = CreateSketch(10, 32, 4, 0.9);
  ASSERT_NE(sketch, nullptr);

  auto* loaded = TopkRdbRoundTrip(sketch, kTopkEncVerCurrent);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->K(), 10u);
  EXPECT_EQ(loaded->Width(), 32u);
  EXPECT_EQ(loaded->Depth(), 4u);
  EXPECT_DOUBLE_EQ(loaded->Decay(), 0.9);
  EXPECT_EQ(loaded->NumActive(), 0u);

  DestroySketch(sketch);
  DestroySketch(loaded);
}

// ==================================================================
// RDB round-trip: populated sketch preserves entries and cells
// ==================================================================

TEST(TopkRdb, PopulatedSketchRoundTrip) {
  auto* sketch = CreateSketch(5, 64, 4, 0.9);
  for (int i = 0; i < 50; i++) {
    sketch->Add(ToSpan("item_" + std::to_string(i)));
  }

  auto* loaded = TopkRdbRoundTrip(sketch, kTopkEncVerCurrent);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->NumActive(), sketch->NumActive());

  auto origList = sketch->List();
  auto loadedList = loaded->List();
  ASSERT_EQ(loadedList.size(), origList.size());
  for (size_t i = 0; i < origList.size(); i++) {
    EXPECT_EQ(loadedList[i].first, origList[i].first);
    EXPECT_EQ(loadedList[i].second, origList[i].second);
  }

  DestroySketch(sketch);
  DestroySketch(loaded);
}

// ==================================================================
// RDB: cell array exact binary match
// ==================================================================

TEST(TopkRdb, CellArrayExactMatch) {
  auto* sketch = CreateSketch(3, 32, 3, 0.9);
  for (int i = 0; i < 20; i++) {
    sketch->Add(ToSpan("bits_" + std::to_string(i)));
  }

  auto* loaded = TopkRdbRoundTrip(sketch, kTopkEncVerCurrent);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->GetCellDataSize(), sketch->GetCellDataSize());
  EXPECT_EQ(std::memcmp(loaded->GetCellArray(), sketch->GetCellArray(),
                        sketch->GetCellDataSize()), 0);

  DestroySketch(sketch);
  DestroySketch(loaded);
}

// ==================================================================
// RDB: rejected unknown encver
// ==================================================================

TEST(TopkRdb, RejectsUnknownEncver) {
  MockRdbStream stream;
  auto* sketch = CreateSketch(5, 32, 4, 0.9);
  RdbSaveTopk(stream.IO(), sketch);
  stream.Rewind();

  auto* loaded = static_cast<TopkSketch*>(RdbLoadTopk(stream.IO(), 99));
  EXPECT_EQ(loaded, nullptr);

  DestroySketch(sketch);
}

// ==================================================================
// LOADCHUNK wire format round-trip
// ==================================================================

TEST(TopkWire, LoadChunkRoundTrip) {
  auto* sketch = CreateSketch(5, 64, 4, 0.9);
  for (int i = 0; i < 30; i++) {
    sketch->Add(ToSpan("chunk_" + std::to_string(i)));
  }

  size_t chunkSize = TopkComputeChunkSize(*sketch);
  std::vector<uint8_t> buf(chunkSize);
  size_t written = TopkSerializeChunk(*sketch, buf.data());
  EXPECT_EQ(written, chunkSize);

  auto* loaded = TopkDeserializeChunk(buf.data(), buf.size());
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->K(), sketch->K());
  EXPECT_EQ(loaded->Width(), sketch->Width());
  EXPECT_EQ(loaded->Depth(), sketch->Depth());
  EXPECT_DOUBLE_EQ(loaded->Decay(), sketch->Decay());

  auto origList = sketch->List();
  auto loadedList = loaded->List();
  ASSERT_EQ(loadedList.size(), origList.size());
  for (size_t i = 0; i < origList.size(); i++) {
    EXPECT_EQ(loadedList[i].first, origList[i].first);
    EXPECT_EQ(loadedList[i].second, origList[i].second);
  }

  DestroySketch(sketch);
  DestroySketch(loaded);
}

TEST(TopkWire, RejectsTruncatedChunk) {
  auto* result = TopkDeserializeChunk("x", 1);
  EXPECT_EQ(result, nullptr);
}

TEST(TopkWire, RejectsChunkWithTrailingGarbage) {
  auto* sketch = CreateSketch(3, 16, 2, 0.9);
  size_t chunkSize = TopkComputeChunkSize(*sketch);
  std::vector<uint8_t> buf(chunkSize + 1, 0);
  TopkSerializeChunk(*sketch, buf.data());

  auto* loaded = TopkDeserializeChunk(buf.data(), buf.size());
  EXPECT_EQ(loaded, nullptr);

  DestroySketch(sketch);
  if (loaded) DestroySketch(loaded);
}

// ==================================================================
// RDB: field validation on the raw stream
// ==================================================================

struct CustomTopkFields {
  uint64_t k = 10;
  uint64_t width = 8;
  uint64_t depth = 4;
  double decay = 0.9;
  uint64_t numActive = 0;
};

static TopkSketch* LoadCustomRdb(const CustomTopkFields& f,
                                  size_t cellBlobSizeOverride = static_cast<size_t>(-1)) {
  MockRdbStream stream;
  auto* io = stream.IO();

  Mock_SaveUnsigned(io, f.k);
  Mock_SaveUnsigned(io, f.width);
  Mock_SaveUnsigned(io, f.depth);
  Mock_SaveDouble(io, f.decay);
  Mock_SaveUnsigned(io, f.numActive);

  uint64_t cellDataSize = f.width * f.depth * sizeof(TopkSketch::Cell);
  size_t cellBlobSize = cellBlobSizeOverride == static_cast<size_t>(-1)
    ? static_cast<size_t>(cellDataSize) : cellBlobSizeOverride;
  std::vector<char> cellBytes(cellBlobSize, 0);
  Mock_SaveStringBuffer(io, cellBytes.data(), cellBytes.size());

  for (uint64_t i = 0; i < f.numActive; i++) {
    Mock_SaveUnsigned(io, f.numActive - i);  // descending counts
    std::string name = "entry_" + std::to_string(i);
    Mock_SaveStringBuffer(io, name.data(), name.size());
  }

  stream.Rewind();
  return static_cast<TopkSketch*>(RdbLoadTopk(stream.IO(), kTopkEncVerCurrent));
}

TEST(TopkRdb, RejectsKZero) {
  CustomTopkFields f;
  f.k = 0;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TopkRdb, RejectsKTooLarge) {
  CustomTopkFields f;
  f.k = kTopkMaxK + 1;
  auto* loaded = LoadCustomRdb(f, 0);
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TopkRdb, RejectsWidthZero) {
  CustomTopkFields f;
  f.width = 0;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TopkRdb, RejectsDepthZero) {
  CustomTopkFields f;
  f.depth = 0;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TopkRdb, RejectsDecayOutOfRange) {
  CustomTopkFields f;
  f.decay = 0.0;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);

  CustomTopkFields f2;
  f2.decay = 1.0;
  auto* loaded2 = LoadCustomRdb(f2);
  EXPECT_EQ(loaded2, nullptr);
  if (loaded2) DestroySketch(loaded2);
}

TEST(TopkRdb, RejectsNumActiveGreaterThanK) {
  CustomTopkFields f;
  f.k = 2;
  f.numActive = 3;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TopkRdb, RejectsShortCellBlob) {
  CustomTopkFields f;
  uint64_t expected = f.width * f.depth * sizeof(TopkSketch::Cell);
  auto* loaded = LoadCustomRdb(f, static_cast<size_t>(expected - 1));
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TopkRdb, RejectsLongCellBlob) {
  CustomTopkFields f;
  uint64_t expected = f.width * f.depth * sizeof(TopkSketch::Cell);
  auto* loaded = LoadCustomRdb(f, static_cast<size_t>(expected + 1));
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TopkRdb, AcceptsConsistentEmptySketch) {
  CustomTopkFields f;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_NE(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TopkRdb, AcceptsConsistentPopulatedSketch) {
  CustomTopkFields f;
  f.numActive = 3;
  auto* loaded = LoadCustomRdb(f);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->NumActive(), 3u);
  DestroySketch(loaded);
}

TEST(TopkRdb, RejectsEntriesNotInDescendingOrder) {
  MockRdbStream stream;
  auto* io = stream.IO();

  CustomTopkFields f;
  f.numActive = 2;
  Mock_SaveUnsigned(io, f.k);
  Mock_SaveUnsigned(io, f.width);
  Mock_SaveUnsigned(io, f.depth);
  Mock_SaveDouble(io, f.decay);
  Mock_SaveUnsigned(io, f.numActive);

  uint64_t cellDataSize = f.width * f.depth * sizeof(TopkSketch::Cell);
  std::vector<char> cellBytes(cellDataSize, 0);
  Mock_SaveStringBuffer(io, cellBytes.data(), cellBytes.size());

  // Ascending counts violate the required descending-order invariant.
  Mock_SaveUnsigned(io, 1);
  std::string name1 = "first";
  Mock_SaveStringBuffer(io, name1.data(), name1.size());
  Mock_SaveUnsigned(io, 5);
  std::string name2 = "second";
  Mock_SaveStringBuffer(io, name2.data(), name2.size());

  stream.Rewind();
  auto* loaded = static_cast<TopkSketch*>(RdbLoadTopk(stream.IO(), kTopkEncVerCurrent));
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

// Narrowing-cast bypass: a malicious/corrupt RDB writes (2^32 + N) for a
// uint32 field. ReadTopkSketch must reject values exceeding UINT32_MAX
// outright rather than silently truncating via static_cast<uint32_t>.
TEST(TopkRdb, RejectsWidthHighBitBypass) {
  MockRdbStream stream;
  auto* io = stream.IO();

  Mock_SaveUnsigned(io, 10);
  Mock_SaveUnsigned(io, (1ULL << 32) + 1);  // width with high bits set
  Mock_SaveUnsigned(io, 4);
  Mock_SaveDouble(io, 0.9);
  Mock_SaveUnsigned(io, 0);

  stream.Rewind();
  auto* loaded = static_cast<TopkSketch*>(RdbLoadTopk(stream.IO(), kTopkEncVerCurrent));
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

// ==================================================================
// Module type callbacks: CopyTopk2 / DigestTopk / FreeEffortTopk2 / DefragTopk
// ==================================================================

TEST(TopkModuleType, CopyTopk2ProducesIndependentEqualClone) {
  auto* sketch = CreateSketch(5, 64, 4, 0.9);
  for (int i = 0; i < 30; i++) {
    sketch->Add(ToSpan("copy2_" + std::to_string(i)));
  }

  auto* clone = static_cast<TopkSketch*>(CopyTopk2(nullptr, sketch));
  ASSERT_NE(clone, nullptr);
  EXPECT_NE(clone, sketch);
  EXPECT_EQ(clone->NumActive(), sketch->NumActive());

  auto origList = sketch->List();
  auto cloneList = clone->List();
  ASSERT_EQ(cloneList.size(), origList.size());
  for (size_t i = 0; i < origList.size(); i++) {
    EXPECT_EQ(cloneList[i].first, origList[i].first);
  }

  sketch->Add(ToSpan(std::string("post_copy")));
  sketch->Add(ToSpan(std::string("post_copy")));
  sketch->Add(ToSpan(std::string("post_copy")));
  sketch->Add(ToSpan(std::string("post_copy")));
  sketch->Add(ToSpan(std::string("post_copy")));
  EXPECT_FALSE(clone->Query(ToSpan(std::string("post_copy"))));

  DestroySketch(sketch);
  DestroySketch(clone);
}

TEST(TopkModuleType, DigestTopkDeterministicAndContentSensitive) {
  auto* sketch = CreateSketch(5, 32, 4, 0.9);
  for (int i = 0; i < 20; i++) {
    sketch->Add(ToSpan("digest_" + std::to_string(i)));
  }

  MockDigest d1;
  DigestTopk(d1.Handle(), sketch);
  EXPECT_TRUE(d1.ended);
  EXPECT_FALSE(d1.bytes.empty());

  MockDigest d2;
  DigestTopk(d2.Handle(), sketch);
  EXPECT_EQ(d1.bytes, d2.bytes);

  for (int i = 0; i < 20; i++) sketch->Add(ToSpan(std::string("extra_item")));
  MockDigest d3;
  DigestTopk(d3.Handle(), sketch);
  EXPECT_NE(d1.bytes, d3.bytes);

  DestroySketch(sketch);
}

TEST(TopkModuleType, FreeEffortTopk2ReturnsConstant) {
  auto* sketch = CreateSketch(5, 32, 4, 0.9);
  EXPECT_EQ(FreeEffortTopk2(nullptr, sketch), 3u);
  DestroySketch(sketch);
}

TEST(TopkModuleType, DefragTopkRelocatesBuffersAndPreservesData) {
  auto* sketch = CreateSketch(5, 32, 4, 0.9);
  for (int i = 0; i < 20; i++) {
    sketch->Add(ToSpan("defrag_" + std::to_string(i)));
  }

  const TopkSketch::Cell* origCells = sketch->GetCellArray();
  const TopkSketch::Entry* origEntries = sketch->GetEntries();

  void* value = sketch;
  int rc = DefragTopk(nullptr, nullptr, &value);
  EXPECT_EQ(rc, 0);

  auto* relocated = static_cast<TopkSketch*>(value);
  ASSERT_NE(relocated, nullptr);
  EXPECT_NE(relocated->GetCellArray(), origCells);
  EXPECT_NE(relocated->GetEntries(), origEntries);
  EXPECT_EQ(relocated->NumActive(), 5u);

  DestroySketch(relocated);
}
