// RDB and wire-format serialization round-trip tests for the gemini-bloom
// T-Digest data type. Verifies RDB encver 1, LOADCHUNK wire format, field
// validation, and module type callbacks.

#define REDISMODULE_API
extern "C" {
#include "redismodule.h"
}

#include "mock_redismodule_io.h"

#include <gtest/gtest.h>
#include "tdigest_rdb.h"
#include "tdigest_sketch.h"
#include "rm_alloc.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// Install mock IO before any tests.
class TdigestRdbTestEnv : public ::testing::Environment {
public:
  void SetUp() override { InstallMockRedisModuleIO(); }
};
static auto* const gEnv [[maybe_unused]] =
  ::testing::AddGlobalTestEnvironment(new TdigestRdbTestEnv);

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

static TdigestSketch* CreateSketch(double compression) {
  auto maybe = TdigestSketch::Create(compression);
  if (!maybe) return nullptr;
  auto* sketch = static_cast<TdigestSketch*>(RMAlloc(sizeof(TdigestSketch)));
  new (sketch) TdigestSketch(std::move(*maybe));
  return sketch;
}

static void DestroySketch(TdigestSketch* sketch) {
  if (sketch) { sketch->~TdigestSketch(); RMFree(sketch); }
}

static TdigestSketch* TdigestRdbRoundTrip(TdigestSketch* src, int load_encver) {
  MockRdbStream stream;
  RdbSaveTdigest(stream.IO(), src);
  stream.Rewind();
  return static_cast<TdigestSketch*>(RdbLoadTdigest(stream.IO(), load_encver));
}

// ==================================================================
// RDB round-trip: empty sketch
// ==================================================================

TEST(TdigestRdb, EmptySketchRoundTrip) {
  auto* sketch = CreateSketch(100.0);
  ASSERT_NE(sketch, nullptr);

  auto* loaded = TdigestRdbRoundTrip(sketch, kTdigestEncVerCurrent);
  ASSERT_NE(loaded, nullptr);
  EXPECT_DOUBLE_EQ(loaded->Compression(), 100.0);
  EXPECT_EQ(loaded->NumCentroids(), 0u);
  EXPECT_EQ(loaded->NumBuffered(), 0u);
  EXPECT_TRUE(loaded->Empty());

  DestroySketch(sketch);
  DestroySketch(loaded);
}

// ==================================================================
// RDB round-trip: populated sketch preserves centroids/buffer/bookkeeping
// ==================================================================

TEST(TdigestRdb, PopulatedSketchRoundTrip) {
  auto* sketch = CreateSketch(100.0);
  for (int i = 1; i <= 500; i++) sketch->Add(static_cast<double>(i));
  sketch->Compress();

  auto* loaded = TdigestRdbRoundTrip(sketch, kTdigestEncVerCurrent);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->NumCentroids(), sketch->NumCentroids());
  EXPECT_EQ(loaded->NumBuffered(), sketch->NumBuffered());
  EXPECT_DOUBLE_EQ(loaded->Min(), sketch->Min());
  EXPECT_DOUBLE_EQ(loaded->Max(), sketch->Max());
  EXPECT_DOUBLE_EQ(loaded->TotalWeight(), sketch->TotalWeight());
  EXPECT_EQ(loaded->NumCompressions(), sketch->NumCompressions());

  DestroySketch(sketch);
  DestroySketch(loaded);
}

TEST(TdigestRdb, CentroidArrayExactMatch) {
  auto* sketch = CreateSketch(50.0);
  for (int i = 1; i <= 200; i++) sketch->Add(static_cast<double>(i));
  sketch->Compress();

  auto* loaded = TdigestRdbRoundTrip(sketch, kTdigestEncVerCurrent);
  ASSERT_NE(loaded, nullptr);
  ASSERT_EQ(loaded->NumCentroids(), sketch->NumCentroids());
  EXPECT_EQ(std::memcmp(loaded->GetCentroidArray(), sketch->GetCentroidArray(),
                        sketch->NumCentroids() * sizeof(TdigestSketch::Centroid)), 0);

  DestroySketch(sketch);
  DestroySketch(loaded);
}

TEST(TdigestRdb, UncompressedBufferRoundTrip) {
  auto* sketch = CreateSketch(1000.0);
  // Add just a handful of values -- stays well below the auto-flush
  // threshold, so it should round-trip while still sitting in the buffer.
  sketch->Add(1.0);
  sketch->Add(2.0);
  sketch->Add(3.0);
  ASSERT_GT(sketch->NumBuffered(), 0u);

  auto* loaded = TdigestRdbRoundTrip(sketch, kTdigestEncVerCurrent);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->NumBuffered(), sketch->NumBuffered());
  EXPECT_DOUBLE_EQ(loaded->Min(), 1.0);
  EXPECT_DOUBLE_EQ(loaded->Max(), 3.0);

  DestroySketch(sketch);
  DestroySketch(loaded);
}

// ==================================================================
// RDB: rejected unknown encver
// ==================================================================

TEST(TdigestRdb, RejectsUnknownEncver) {
  MockRdbStream stream;
  auto* sketch = CreateSketch(100.0);
  RdbSaveTdigest(stream.IO(), sketch);
  stream.Rewind();

  auto* loaded = static_cast<TdigestSketch*>(RdbLoadTdigest(stream.IO(), 99));
  EXPECT_EQ(loaded, nullptr);

  DestroySketch(sketch);
}

// ==================================================================
// LOADCHUNK wire format round-trip
// ==================================================================

TEST(TdigestWire, LoadChunkRoundTrip) {
  auto* sketch = CreateSketch(100.0);
  for (int i = 1; i <= 300; i++) sketch->Add(static_cast<double>(i));
  sketch->Compress();

  size_t chunkSize = TdigestComputeChunkSize(*sketch);
  std::vector<uint8_t> buf(chunkSize);
  size_t written = TdigestSerializeChunk(*sketch, buf.data());
  EXPECT_EQ(written, chunkSize);

  auto* loaded = TdigestDeserializeChunk(buf.data(), buf.size());
  ASSERT_NE(loaded, nullptr);
  EXPECT_DOUBLE_EQ(loaded->Compression(), sketch->Compression());
  EXPECT_EQ(loaded->NumCentroids(), sketch->NumCentroids());
  EXPECT_DOUBLE_EQ(loaded->Min(), sketch->Min());
  EXPECT_DOUBLE_EQ(loaded->Max(), sketch->Max());

  DestroySketch(sketch);
  DestroySketch(loaded);
}

TEST(TdigestWire, RejectsTruncatedChunk) {
  auto* result = TdigestDeserializeChunk("x", 1);
  EXPECT_EQ(result, nullptr);
}

TEST(TdigestWire, RejectsChunkWithTrailingGarbage) {
  auto* sketch = CreateSketch(50.0);
  for (int i = 1; i <= 20; i++) sketch->Add(static_cast<double>(i));
  sketch->Compress();

  size_t chunkSize = TdigestComputeChunkSize(*sketch);
  std::vector<uint8_t> buf(chunkSize + 1, 0);
  TdigestSerializeChunk(*sketch, buf.data());

  auto* loaded = TdigestDeserializeChunk(buf.data(), buf.size());
  EXPECT_EQ(loaded, nullptr);

  DestroySketch(sketch);
  if (loaded) DestroySketch(loaded);
}

// ==================================================================
// RDB: field validation on the raw stream
// ==================================================================

struct CustomTdigestFields {
  double compression = 100.0;
  double mergedWeight = 0.0;
  double unmergedWeight = 0.0;
  double rawMin = 0.0;
  double rawMax = 0.0;
  uint64_t numCompressions = 0;
  uint64_t numCentroids = 0;
  uint64_t numBuffered = 0;
};

static TdigestSketch* LoadCustomRdb(const CustomTdigestFields& f,
                                     size_t centroidBlobSizeOverride = static_cast<size_t>(-1),
                                     size_t bufferBlobSizeOverride = static_cast<size_t>(-1)) {
  MockRdbStream stream;
  auto* io = stream.IO();

  Mock_SaveDouble(io, f.compression);
  Mock_SaveDouble(io, f.mergedWeight);
  Mock_SaveDouble(io, f.unmergedWeight);
  Mock_SaveDouble(io, f.rawMin);
  Mock_SaveDouble(io, f.rawMax);
  Mock_SaveUnsigned(io, f.numCompressions);
  Mock_SaveUnsigned(io, f.numCentroids);

  uint64_t centroidBytes = f.numCentroids * sizeof(TdigestSketch::Centroid);
  size_t centroidBlobSize = centroidBlobSizeOverride == static_cast<size_t>(-1)
    ? static_cast<size_t>(centroidBytes) : centroidBlobSizeOverride;
  std::vector<TdigestSketch::Centroid> centroids(f.numCentroids, TdigestSketch::Centroid{1.0, 1.0});
  std::vector<char> centroidBuf(centroidBlobSize, 0);
  if (!centroids.empty() && centroidBlobSize >= centroidBytes) {
    std::memcpy(centroidBuf.data(), centroids.data(), centroidBytes);
  }
  Mock_SaveStringBuffer(io, centroidBuf.data(), centroidBuf.size());

  Mock_SaveUnsigned(io, f.numBuffered);
  uint64_t bufferBytes = f.numBuffered * sizeof(TdigestSketch::Centroid);
  size_t bufferBlobSize = bufferBlobSizeOverride == static_cast<size_t>(-1)
    ? static_cast<size_t>(bufferBytes) : bufferBlobSizeOverride;
  std::vector<TdigestSketch::Centroid> buffered(f.numBuffered, TdigestSketch::Centroid{1.0, 1.0});
  std::vector<char> bufferBuf(bufferBlobSize, 0);
  if (!buffered.empty() && bufferBlobSize >= bufferBytes) {
    std::memcpy(bufferBuf.data(), buffered.data(), bufferBytes);
  }
  Mock_SaveStringBuffer(io, bufferBuf.data(), bufferBuf.size());

  stream.Rewind();
  return static_cast<TdigestSketch*>(RdbLoadTdigest(stream.IO(), kTdigestEncVerCurrent));
}

TEST(TdigestRdb, RejectsCompressionTooLow) {
  CustomTdigestFields f;
  f.compression = kTdigestMinCompression - 1.0;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TdigestRdb, RejectsCompressionTooHigh) {
  CustomTdigestFields f;
  f.compression = kTdigestMaxCompression + 1.0;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TdigestRdb, RejectsNonFiniteCompression) {
  CustomTdigestFields f;
  f.compression = std::nan("");
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TdigestRdb, RejectsNegativeMergedWeight) {
  CustomTdigestFields f;
  f.mergedWeight = -1.0;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TdigestRdb, RejectsNonEmptyCentroidsWithZeroWeight) {
  CustomTdigestFields f;
  f.numCentroids = 3;
  // mergedWeight/unmergedWeight both left at 0 -> "empty" but numCentroids != 0.
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TdigestRdb, RejectsNonFiniteMinMaxWhenNonEmpty) {
  CustomTdigestFields f;
  f.mergedWeight = 5.0;
  f.numCentroids = 1;
  f.rawMin = std::nan("");
  f.rawMax = 10.0;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TdigestRdb, RejectsMinGreaterThanMax) {
  CustomTdigestFields f;
  f.mergedWeight = 5.0;
  f.numCentroids = 1;
  f.rawMin = 10.0;
  f.rawMax = 1.0;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TdigestRdb, RejectsShortCentroidBlob) {
  CustomTdigestFields f;
  f.mergedWeight = 5.0;
  f.numCentroids = 2;
  f.rawMin = 1.0;
  f.rawMax = 2.0;
  uint64_t expected = f.numCentroids * sizeof(TdigestSketch::Centroid);
  auto* loaded = LoadCustomRdb(f, static_cast<size_t>(expected - 1));
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TdigestRdb, RejectsLongCentroidBlob) {
  CustomTdigestFields f;
  f.mergedWeight = 5.0;
  f.numCentroids = 2;
  f.rawMin = 1.0;
  f.rawMax = 2.0;
  uint64_t expected = f.numCentroids * sizeof(TdigestSketch::Centroid);
  auto* loaded = LoadCustomRdb(f, static_cast<size_t>(expected + 1));
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TdigestRdb, RejectsShortBufferBlob) {
  CustomTdigestFields f;
  f.unmergedWeight = 3.0;
  f.numBuffered = 2;
  f.rawMin = 1.0;
  f.rawMax = 2.0;
  uint64_t expected = f.numBuffered * sizeof(TdigestSketch::Centroid);
  auto* loaded = LoadCustomRdb(f, static_cast<size_t>(-1), static_cast<size_t>(expected - 1));
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TdigestRdb, AcceptsConsistentEmptySketch) {
  CustomTdigestFields f;
  auto* loaded = LoadCustomRdb(f);
  EXPECT_NE(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TdigestRdb, AcceptsConsistentPopulatedSketch) {
  CustomTdigestFields f;
  f.mergedWeight = 4.0;
  f.numCentroids = 4;
  f.rawMin = 1.0;
  f.rawMax = 2.0;
  auto* loaded = LoadCustomRdb(f);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->NumCentroids(), 4u);
  DestroySketch(loaded);
}

// Narrowing-cast bypass: a malicious/corrupt RDB writes (2^32 + N) for a
// uint32 field. ReadTdigestSketch must reject values exceeding UINT32_MAX
// outright rather than silently truncating via static_cast<uint32_t>.
TEST(TdigestRdb, RejectsNumCentroidsHighBitBypass) {
  MockRdbStream stream;
  auto* io = stream.IO();

  Mock_SaveDouble(io, 100.0);
  Mock_SaveDouble(io, 0.0);
  Mock_SaveDouble(io, 0.0);
  Mock_SaveDouble(io, 0.0);
  Mock_SaveDouble(io, 0.0);
  Mock_SaveUnsigned(io, 0);
  Mock_SaveUnsigned(io, (1ULL << 32) + 1);  // numCentroids with high bits set

  stream.Rewind();
  auto* loaded = static_cast<TdigestSketch*>(RdbLoadTdigest(stream.IO(), kTdigestEncVerCurrent));
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

TEST(TdigestRdb, RejectsCentroidWithNonPositiveWeight) {
  MockRdbStream stream;
  auto* io = stream.IO();

  Mock_SaveDouble(io, 100.0);
  Mock_SaveDouble(io, 1.0);
  Mock_SaveDouble(io, 0.0);
  Mock_SaveDouble(io, 5.0);
  Mock_SaveDouble(io, 5.0);
  Mock_SaveUnsigned(io, 0);
  Mock_SaveUnsigned(io, 1);

  TdigestSketch::Centroid bad{5.0, 0.0};  // weight <= 0
  Mock_SaveStringBuffer(io, reinterpret_cast<const char*>(&bad), sizeof(bad));
  Mock_SaveUnsigned(io, 0);
  Mock_SaveStringBuffer(io, nullptr, 0);

  stream.Rewind();
  auto* loaded = static_cast<TdigestSketch*>(RdbLoadTdigest(stream.IO(), kTdigestEncVerCurrent));
  EXPECT_EQ(loaded, nullptr);
  if (loaded) DestroySketch(loaded);
}

// ==================================================================
// Module type callbacks: CopyTdigest2 / DigestTdigest / FreeEffortTdigest2 /
// DefragTdigest
// ==================================================================

TEST(TdigestModuleType, CopyTdigest2ProducesIndependentEqualClone) {
  auto* sketch = CreateSketch(100.0);
  for (int i = 1; i <= 200; i++) sketch->Add(static_cast<double>(i));
  sketch->Compress();

  auto* clone = static_cast<TdigestSketch*>(CopyTdigest2(nullptr, sketch));
  ASSERT_NE(clone, nullptr);
  EXPECT_NE(clone, sketch);
  EXPECT_EQ(clone->NumCentroids(), sketch->NumCentroids());
  EXPECT_DOUBLE_EQ(clone->Min(), sketch->Min());
  EXPECT_DOUBLE_EQ(clone->Max(), sketch->Max());

  sketch->Add(10000.0);
  sketch->Compress();
  EXPECT_NE(clone->Max(), sketch->Max());

  DestroySketch(sketch);
  DestroySketch(clone);
}

TEST(TdigestModuleType, DigestTdigestDeterministicAndContentSensitive) {
  auto* sketch = CreateSketch(100.0);
  for (int i = 1; i <= 100; i++) sketch->Add(static_cast<double>(i));
  sketch->Compress();

  MockDigest d1;
  DigestTdigest(d1.Handle(), sketch);
  EXPECT_TRUE(d1.ended);
  EXPECT_FALSE(d1.bytes.empty());

  MockDigest d2;
  DigestTdigest(d2.Handle(), sketch);
  EXPECT_EQ(d1.bytes, d2.bytes);

  for (int i = 0; i < 50; i++) sketch->Add(5000.0);
  sketch->Compress();
  MockDigest d3;
  DigestTdigest(d3.Handle(), sketch);
  EXPECT_NE(d1.bytes, d3.bytes);

  DestroySketch(sketch);
}

TEST(TdigestModuleType, FreeEffortTdigest2ReturnsConstant) {
  auto* sketch = CreateSketch(100.0);
  EXPECT_EQ(FreeEffortTdigest2(nullptr, sketch), 3u);
  DestroySketch(sketch);
}

TEST(TdigestModuleType, DefragTdigestRelocatesBuffersAndPreservesData) {
  auto* sketch = CreateSketch(100.0);
  for (int i = 1; i <= 100; i++) sketch->Add(static_cast<double>(i));
  sketch->Compress();
  sketch->Add(101.0);

  const TdigestSketch::Centroid* origCentroids = sketch->GetCentroidArray();
  const TdigestSketch::Centroid* origBuffer = sketch->GetBufferArray();

  void* value = sketch;
  int rc = DefragTdigest(nullptr, nullptr, &value);
  EXPECT_EQ(rc, 0);

  auto* relocated = static_cast<TdigestSketch*>(value);
  ASSERT_NE(relocated, nullptr);
  EXPECT_NE(relocated->GetCentroidArray(), origCentroids);
  EXPECT_NE(relocated->GetBufferArray(), origBuffer);
  EXPECT_EQ(relocated->NumCentroids(), 100u);
  EXPECT_EQ(relocated->NumBuffered(), 1u);

  DestroySketch(relocated);
}
