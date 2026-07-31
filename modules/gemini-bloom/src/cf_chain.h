#pragma once

#include "cuckoo_filter.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// One layer in a cuckoo filter chain.
struct CfFilterLayer {
  CuckooFilter cuckoo;
};

// Scaling cuckoo filter chain with RAII lifetime management.
// Mirrors ScalingBloomFilter (sb_chain.h): a chain of independent
// CuckooFilter sub-filters, growing automatically when the newest layer
// fills up. Based on the scaling-filter growth strategy from "Scalable
// Bloom Filters" (Almeida, Baquero et al., 2007), adapted here for cuckoo
// sub-filters instead of bloom sub-filters.
class CuckooChain {
public:
  CuckooChain(uint64_t initialCapacity, uint32_t bucketSize, uint32_t maxIterations,
              uint32_t expansion, uint32_t maxExpansions);
  ~CuckooChain();

  CuckooChain(const CuckooChain&) = delete;
  CuckooChain& operator=(const CuckooChain&) = delete;
  CuckooChain(CuckooChain&&) noexcept;
  CuckooChain& operator=(CuckooChain&&) noexcept;

  bool IsValid() const { return layers_ != nullptr; }

  // No value = every layer is full and maxExpansions_ has been reached
  // (or expansion_ == 0, i.e. fixed-size mode) — insert failed entirely.
  std::optional<bool> Add(std::span<const std::byte> item);
  bool Contains(std::span<const std::byte> item) const;
  // Deletes one matching copy from the first layer that has it.
  bool Delete(std::span<const std::byte> item);
  // Sum of matching-fingerprint counts across all layers.
  uint64_t Count(std::span<const std::byte> item) const;

  CuckooChain* Clone() const;

  // For defrag: adopt a relocated layers array. Caller owns the
  // relocation (e.g. via RedisModule_DefragAlloc) and transfers ownership.
  void AdoptLayersArray(CfFilterLayer* relocated) { layers_ = relocated; }

  std::span<CfFilterLayer> Layers() { return {layers_, numLayers_}; }
  std::span<const CfFilterLayer> Layers() const { return {layers_, numLayers_}; }
  size_t NumLayers() const { return numLayers_; }
  // Cumulative counters (never decrease): total items ever inserted /
  // deleted, matching CF.INFO's "Number of items inserted/deleted" fields.
  uint64_t TotalItems() const { return totalItems_; }
  uint64_t TotalDeleted() const { return totalDeleted_; }
  uint32_t BucketSize() const { return bucketSize_; }
  uint32_t Expansion() const { return expansion_; }
  uint32_t MaxIterations() const { return maxIterations_; }
  uint32_t MaxExpansions() const { return maxExpansions_; }
  uint64_t TotalBuckets() const;
  uint64_t BytesUsed() const;

  // Two-phase construction for RDB / SCANDUMP deserialization, mirroring
  // ScalingBloomFilter::RdbShell/FromRdbShell/SetLayer.
  struct RdbShell {
    uint64_t totalItems;
    uint64_t totalDeleted;
    uint32_t numLayers;
    uint32_t bucketSize;
    uint32_t maxIterations;
    uint32_t expansion;
    uint32_t maxExpansions;
  };
  static CuckooChain* FromRdbShell(const RdbShell& shell);
  void SetLayer(size_t index, CfFilterLayer&& layer);

  // LOADCHUNK half-restore tracking, mirroring ScalingBloomFilter's
  // Loading/chunksLoaded_ (see sb_chain.h) — a chain built via
  // FromRdbShell is not safe to query until every layer's chunk has
  // arrived via CF.LOADCHUNK.
  bool IsLoading() const { return loading_; }
  void SetLoading() { loading_ = true; chunksLoaded_ = 0; }
  void ClearLoading() { loading_ = false; }
  size_t ChunksLoaded() const { return chunksLoaded_; }
  void IncrementChunksLoaded() { ++chunksLoaded_; }
  // Restores loading/chunksLoaded_ read back from RDB (see cf_rdb.cc's
  // ReadCfChain) so a chain that was mid-LOADCHUNK when the server saved
  // stays correctly marked loading after a DEBUG RELOAD / restart, instead
  // of being treated as a complete chain with a stale totalItems_ count.
  void RestoreLoadingState(bool loading, size_t chunksLoaded) {
    loading_ = loading;
    chunksLoaded_ = chunksLoaded;
  }

private:
  struct EmptyShellTag {};
  explicit CuckooChain(EmptyShellTag) {}

  bool AppendLayer(uint64_t numBuckets);

  CfFilterLayer* layers_ = nullptr;
  size_t numLayers_ = 0;
  size_t layerCapacity_ = 0;
  uint64_t totalItems_ = 0;
  uint64_t totalDeleted_ = 0;
  uint32_t bucketSize_ = 0;
  uint32_t maxIterations_ = 0;
  uint32_t expansion_ = 0;
  uint32_t maxExpansions_ = 0;
  bool loading_ = false;
  size_t chunksLoaded_ = 0;
};
