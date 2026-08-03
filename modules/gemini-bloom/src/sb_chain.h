#pragma once

#include "bloom_filter.h"

#include <algorithm>
#include <cstddef>
#include <numeric>

// Error tightening ratio from Almeida et al. (2007) §3:
// each successive layer halves the per-layer FP rate so the
// overall rate converges to the user-specified target.
constexpr double kTighteningRatio = 0.5;

// One layer in a scaling bloom filter.
struct FilterLayer {
  BloomLayer bloom;
  size_t itemCount = 0;
};

enum class PutResult {
  Inserted,
  Duplicate,
  Full,
};

// Scaling bloom filter with RAII lifetime management.
// Layers grow automatically when capacity is exceeded.
// Based on "Scalable Bloom Filters" by Almeida, Baquero et al. (2007).
class ScalingBloomFilter {
public:
  ScalingBloomFilter(uint64_t initialCapacity, double errorRate,
                      BloomFlags flags, unsigned expansion);
  ~ScalingBloomFilter();

  ScalingBloomFilter(const ScalingBloomFilter&) = delete;
  ScalingBloomFilter& operator=(const ScalingBloomFilter&) = delete;
  ScalingBloomFilter(ScalingBloomFilter&&) noexcept;
  ScalingBloomFilter& operator=(ScalingBloomFilter&&) noexcept;

  bool IsValid() const { return layers_ != nullptr; }

  PutResult Put(const void* data, size_t len);
  bool Contains(const void* data, size_t len) const;

  uint64_t TotalCapacity() const;
  size_t BytesUsed() const;

  // Layer access for RDB / SCANDUMP
  FilterLayer* Layers() { return layers_; }
  const FilterLayer* Layers() const { return layers_; }
  size_t NumLayers() const { return numLayers_; }
  size_t TotalItems() const { return totalItems_; }
  BloomFlags Flags() const { return flags_; }
  unsigned ExpansionFactor() const { return expansionFactor_; }
  bool IsLoading() const { return HasFlag(flags_, BloomFlags::Loading); }
  void SetLoading() { flags_ = flags_ | BloomFlags::Loading; chunksLoaded_ = 0; }
  void ClearLoading() { flags_ = FromUnderlying(ToUnderlying(flags_) & ~ToUnderlying(BloomFlags::Loading)); }
  size_t ChunksLoaded() const { return chunksLoaded_; }
  void IncrementChunksLoaded() { ++chunksLoaded_; }
  uint64_t TotalDataSize() const;

  // RDB serialization: the filter writes/reads its own structure
  void WriteTo(RdbWriter& w) const;
  static ScalingBloomFilter* ReadFrom(RdbReader& r, int encver);

  // For SCANDUMP deserialization: construct empty shell, then populate layers
  struct RdbShell {
    size_t totalItems;
    size_t numLayers;
    BloomFlags flags;
    unsigned expansionFactor;
  };
  static ScalingBloomFilter* FromRdbShell(RdbShell shell);
  void SetLayer(size_t index, FilterLayer&& layer);

  // Deep copy: the copy constructor is deleted (move-only), so cloning
  // needs an explicit method. Used by the COPY command's copy2 callback.
  ScalingBloomFilter* Clone() const;

  // For defrag: adopt a relocated layers array. Caller owns the
  // relocation (e.g. via RedisModule_DefragAlloc) and transfers ownership.
  void AdoptLayersArray(FilterLayer* newLayers) { layers_ = newLayers; }

private:
  struct EmptyShellTag {};
  explicit ScalingBloomFilter(EmptyShellTag) {}

  HashPair ComputeHash(const void* data, size_t len) const;
  bool IsDuplicate(const HashPair& hp) const;
  bool GrowIfNeeded();
  bool AppendLayer(uint64_t cap, double rate);

  FilterLayer* layers_ = nullptr;
  size_t totalItems_ = 0;
  size_t numLayers_ = 0;
  size_t layerCapacity_ = 0;
  BloomFlags flags_ = BloomFlags::None;
  unsigned expansionFactor_ = 2;
  size_t chunksLoaded_ = 0;
};

// Wire-format structures for BF.SCANDUMP / BF.LOADCHUNK.
// gemini uses a private layer-index cursor protocol; these are NOT
// interoperable with RedisBloom's byte-offset SCANDUMP/LOADCHUNK.
// RDB-level field order matches RedisBloom v2.4.20 for DUMP/RESTORE
// and RDB file compatibility only.
#pragma pack(push, 1)
struct WireLayerMeta {
  uint64_t dataSize;
  uint64_t totalBits;
  uint64_t itemCount;
  double fpRate;
  double bitsPerEntry;
  uint32_t hashCount;
  uint64_t capacity;
  uint8_t log2Bits;
};

struct WireFilterHeader {
  uint64_t totalItems;
  uint32_t numLayers;
  uint32_t flags;
  uint32_t expansionFactor;
};
#pragma pack(pop)

size_t ComputeHeaderSize(const ScalingBloomFilter& filter);
size_t SerializeHeader(const ScalingBloomFilter& filter, void* output);
ScalingBloomFilter* DeserializeHeader(const void* data, size_t length);
