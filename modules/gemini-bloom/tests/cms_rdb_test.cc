// RDB and wire-format serialization round-trip tests for the gemini-bloom
// Count-Min Sketch (CMS) data type. Verifies RDB encver 1, LOADCHUNK wire
// format, field validation, and module type callbacks.

#define REDISMODULE_API
extern "C" {
#include "redismodule.h"
}

#include "mock_redismodule_io.h"

#include <gtest/gtest.h>
#include "cms_rdb.h"
#include "cms_sketch.h"
#include "rm_alloc.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Install mock IO before any tests.
class CmsRdbTestEnv : public ::testing::Environment {
public:
  void SetUp() override { InstallMockRedisModuleIO(); }
};
static auto* const gEnv [[maybe_unused]] =
  ::testing::AddGlobalTestEnvironment(new CmsRdbTestEnv);

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

static CmsSketch* CreateSketch(uint32_t width, uint32_t depth) {
  auto maybe = CmsSketch::Create(width, depth);
  if (!maybe) return nullptr;
  auto* sketch = static_cast<CmsSketch*>(RMAlloc(sizeof(CmsSketch)));
  new (sketch) CmsSketch(std::move(*maybe));
  return sketch;
}

static void DestroySketch(CmsSketch* sketch) {
  if (sketch) { sketch->~CmsSketch(); RMFree(sketch); }
}

static CmsSketch* CmsRdbRoundTrip(CmsSketch* src, int load_encver) {
  MockRdbStream stream;
  RdbSaveCms(stream.IO(), src);
  stream.Rewind();
  return static_cast<CmsSketch*>(RdbLoadCms(stream.IO(), load_encver));
}

static std::span<const std::byte> ToSpan(const std::string& s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

// ==================================================================
// RDB round-trip: empty sketch
// ==================================================================

TEST(CmsRdb, EmptySketchRoundTrip) {
  auto* sketch = CreateSketch(500, 4);
  ASSERT_NE(sketch, nullptr);

  auto* loaded = CmsRdbRoundTrip(sketch, kCmsEncVerCurrent);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->Width(), 500u);
  EXPECT_EQ(loaded->Depth(), 4u);
  EXPECT_EQ(loaded->TotalCount(), 0u);

  DestroySketch(sketch);
  DestroySketch(loaded);
}

// ==================================================================
// RDB round-trip: populated sketch preserves counts
// ==================================================================

TEST(CmsRdb, PopulatedSketchRoundTrip) {
  auto* sketch = CreateSketch(1000, 5);
  std::vector<std::string> items;
  for (int i = 0; i < 200; i++) {
    auto item = "item_" + std::to_string(i);
    sketch->IncrBy(ToSpan(item), i + 1);
    items.push_back(item);
  }

  auto* loaded = CmsRdbRoundTrip(sketch, kCmsEncVerCurrent);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->TotalCount(), sketch->TotalCount());

  for (int i = 0; i < 200; i++) {
    EXPECT_EQ(loaded->Query(ToSpan(items[static_cast<size_t>(i)])),
              sketch->Query(ToSpan(items[static_cast<size_t>(i)])));
  }

  DestroySketch(sketch);
  DestroySketch(loaded);
}

// ==================================================================
// RDB: counter array exact binary match
// ==================================================================

TEST(CmsRdb, CounterArrayExactMatch) {
  auto* sketch = CreateSketch(300, 3);
  for (int i = 0; i < 100; i++) {
    sketch->IncrBy(ToSpan("bits_" + std::to_string(i)), i);
  }

  auto* loaded = CmsRdbRoundTrip(sketch, kCmsEncVerCurrent);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->GetDataSize(), sketch->GetDataSize());
  EXPECT_EQ(std::memcmp(loaded->GetCounterArray(), sketch->GetCounterArray(),
                        sketch->GetDataSize()), 0);

  DestroySketch(sketch);
  DestroySketch(loaded);
}

// ==================================================================
// RDB: rejected unknown encver
// ==================================================================

TEST(CmsRdb, RejectsUnknownEncver) {
  MockRdbStream stream;
  auto* sketch = CreateSketch(200, 4);
  RdbSaveCms(stream.IO(), sketch);
  stream.Rewind();

  auto* loaded = static_cast<CmsSketch*>(RdbLoadCms(stream.IO(), 99));
  EXPECT_EQ(loaded, nullptr);

  DestroySketch(sketch);
}

// ==================================================================
// LOADCHUNK wire format round-trip
// ==================================================================

TEST(CmsWire, LoadChunkRoundTrip) {
  auto* sketch = CreateSketch(400, 4);
  std::vector<std::string> items;
  for (int i = 0; i < 100; i++) {
    auto item = "chunk_" + std::to_string(i);
    sketch->IncrBy(ToSpan(item), i + 1);
    items.push_back(item);
  }

  size_t chunkSize = CmsComputeChunkSize(*sketch);
  std::vector<uint8_t> buf(chunkSize);
  size_t written = CmsSerializeChunk(*sketch, buf.data());
  EXPECT_EQ(written, chunkSize);

  auto* loaded = CmsDeserializeChunk(buf.data(), buf.size());
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->Width(), sketch->Width());
  EXPECT_EQ(loaded->Depth(), sketch->Depth());
  EXPECT_EQ(loaded->TotalCount(), sketch->TotalCount());

  for (const auto& item : items) {
    EXPECT_EQ(loaded->Query(ToSpan(item)), sketch->Query(ToSpan(item)));
  }

  DestroySketch(sketch);
  DestroySketch(loaded);
}

TEST(CmsWire, RejectsTruncatedChunk) {
  auto* result = CmsDeserializeChunk("x", 1);
  EXPECT_EQ(result, nullptr);
}

TEST(CmsWire, RejectsChunkWithDataSizeMismatch) {
  auto* sketch = CreateSketch(100, 2);
  size_t chunkSize = CmsComputeChunkSize(*sketch);
  std::vector<uint8_t> buf(chunkSize);
  CmsSerializeChunk(*sketch, buf.data());
  buf.pop_back();  // truncate the data blob by one byte

  auto* loaded = CmsDeserializeChunk(buf.data(), buf.size());
  EXPECT_EQ(loaded, nullptr);

  DestroySketch(sketch);
  if (loaded) DestroySketch(loaded);
}

// ==================================================================
// RDB: field validation on the raw stream
// ==================================================================

struct CustomCmsFields {
  uint64_t width = 100;
  uint64_t depth = 4;
  uint64_t totalCount = 0;
};

static CmsSketch* LoadCustomRdb(const CustomCmsFields& f,
                                 size_t blobSizeOverride = static_cast<size_t>(-1)) {
  MockRdbStream stream;
  auto* io = stream.IO();

  Mock_SaveUnsigned(io, f.width);
  Mock_SaveUnsigned(io, f.depth);
  Mock_SaveUnsigned(io, f.totalCount);

  uint64_t dataSize = f.width * f.depth * sizeof(uint32_t);
  size_t blobSize = blobSizeOverride == static_cast<size_t>(-1)
    ? static_cast<size_t>(dataSize) : blobSizeOverride;
  std::vector<char> bytes(blobSize, 0);
  Mock_SaveStringBuffer(io, bytes.data(), bytes.size());

  stream.Rewind();
  return static_cast<CmsSketch*>(RdbLoadCms(stream.IO(), kCmsEncVerCurrent));
}

TEST(CmsRdb, RejectsWidthZero) {
  CustomCmsFields f;
  f.width = 0;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(CmsRdb, RejectsWidthTooLarge) {
  CustomCmsFields f;
  f.width = kCmsMaxWidth + 1;
  // Field validation rejects width before the blob length matters, so keep
  // the written blob tiny to avoid allocating a multi-GB buffer for a test
  // that's expected to fail fast.
  auto* loaded = LoadCustomRdb(f, 0);
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(CmsRdb, RejectsDepthZero) {
  CustomCmsFields f;
  f.depth = 0;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(CmsRdb, RejectsDepthTooLarge) {
  CustomCmsFields f;
  f.depth = kCmsMaxDepth + 1;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(CmsRdb, RejectsShortBlob) {
  CustomCmsFields f;
  uint64_t expected = f.width * f.depth * sizeof(uint32_t);
  auto* loaded = LoadCustomRdb(f, static_cast<size_t>(expected - 1));
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(CmsRdb, RejectsLongBlob) {
  CustomCmsFields f;
  uint64_t expected = f.width * f.depth * sizeof(uint32_t);
  auto* loaded = LoadCustomRdb(f, static_cast<size_t>(expected + 1));
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(CmsRdb, AcceptsConsistentEmptySketch) {
  CustomCmsFields f;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_NE(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

// Narrowing-cast bypass: a malicious/corrupt RDB writes (2^32 + N) for a
// uint32 field. ReadCmsSketch must reject values exceeding UINT32_MAX
// outright rather than silently truncating via static_cast<uint32_t>.
TEST(CmsRdb, RejectsWidthHighBitBypass) {
  MockRdbStream stream;
  auto* io = stream.IO();

  Mock_SaveUnsigned(io, (1ULL << 32) + 1);  // width with high bits set
  Mock_SaveUnsigned(io, 4);
  Mock_SaveUnsigned(io, 0);

  stream.Rewind();
  auto* loaded = static_cast<CmsSketch*>(RdbLoadCms(stream.IO(), kCmsEncVerCurrent));
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

// ==================================================================
// Module type callbacks: CopyCms2 / DigestCms / FreeEffortCms2 / DefragCms
// ==================================================================

TEST(CmsModuleType, CopyCms2ProducesIndependentEqualClone) {
  auto* sketch = CreateSketch(500, 4);
  std::vector<std::string> items;
  for (int i = 0; i < 100; i++) {
    auto item = "copy2_" + std::to_string(i);
    sketch->IncrBy(ToSpan(item), i + 1);
    items.push_back(item);
  }

  auto* clone = static_cast<CmsSketch*>(CopyCms2(nullptr, sketch));
  ASSERT_NE(clone, nullptr);
  EXPECT_NE(clone, sketch);
  EXPECT_EQ(clone->TotalCount(), sketch->TotalCount());

  for (const auto& item : items) {
    EXPECT_EQ(clone->Query(ToSpan(item)), sketch->Query(ToSpan(item)));
  }

  auto extra = std::string("post_copy");
  sketch->IncrBy(ToSpan(extra), 5);
  EXPECT_EQ(clone->Query(ToSpan(extra)), 0u);

  DestroySketch(sketch);
  DestroySketch(clone);
}

TEST(CmsModuleType, DigestCmsDeterministicAndContentSensitive) {
  auto* sketch = CreateSketch(200, 4);
  for (int i = 0; i < 50; i++) {
    sketch->IncrBy(ToSpan(std::string("digest_" + std::to_string(i))), i + 1);
  }

  MockDigest d1;
  DigestCms(d1.Handle(), sketch);
  EXPECT_TRUE(d1.ended);
  EXPECT_FALSE(d1.bytes.empty());

  MockDigest d2;
  DigestCms(d2.Handle(), sketch);
  EXPECT_EQ(d1.bytes, d2.bytes);

  sketch->IncrBy(ToSpan(std::string("extra_item")), 1);
  MockDigest d3;
  DigestCms(d3.Handle(), sketch);
  EXPECT_NE(d1.bytes, d3.bytes);

  DestroySketch(sketch);
}

TEST(CmsModuleType, FreeEffortCms2ReturnsConstant) {
  auto* sketch = CreateSketch(200, 4);
  EXPECT_EQ(FreeEffortCms2(nullptr, sketch), 2u);
  DestroySketch(sketch);
}

TEST(CmsModuleType, DefragCmsRelocatesBuffersAndPreservesData) {
  auto* sketch = CreateSketch(300, 4);
  std::vector<std::string> items;
  for (int i = 0; i < 100; i++) {
    auto item = "defrag_" + std::to_string(i);
    sketch->IncrBy(ToSpan(item), i + 1);
    items.push_back(item);
  }

  const uint32_t* origCounters = sketch->GetCounterArray();

  void* value = sketch;
  int rc = DefragCms(nullptr, nullptr, &value);
  EXPECT_EQ(rc, 0);

  auto* relocated = static_cast<CmsSketch*>(value);
  ASSERT_NE(relocated, nullptr);
  EXPECT_NE(relocated->GetCounterArray(), origCounters);

  for (const auto& item : items) {
    EXPECT_GE(relocated->Query(ToSpan(item)), static_cast<uint64_t>(1));
  }

  DestroySketch(relocated);
}
