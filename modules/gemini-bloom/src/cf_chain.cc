#include "cf_chain.h"
#include "rm_alloc.h"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace {

// Rounds capacity/bucketSize up to the next power of two, matching
// BloomLayer's std::bit_ceil usage for bit-array sizing.
uint64_t BucketsForCapacity(uint64_t capacity, uint32_t bucketSize) {
  uint64_t buckets = (capacity + bucketSize - 1) / bucketSize;
  if (buckets == 0) buckets = 1;
  return std::bit_ceil(buckets);
}

}  // namespace

// --- Lifecycle ---

CuckooChain::CuckooChain(uint64_t initialCapacity, uint32_t bucketSize, uint32_t maxIterations,
                          uint32_t expansion, uint32_t maxExpansions)
    : bucketSize_(bucketSize), maxIterations_(maxIterations), expansion_(expansion),
      maxExpansions_(maxExpansions) {
  uint64_t numBuckets = BucketsForCapacity(initialCapacity, bucketSize);
  if (numBuckets > kCfMaxBuckets || !AppendLayer(numBuckets)) {
    if (layers_) RMFree(layers_);
    layers_ = nullptr;
    numLayers_ = 0;
    layerCapacity_ = 0;
  }
}

CuckooChain::~CuckooChain() {
  for (size_t i = 0; i < numLayers_; i++) {
    layers_[i].~CfFilterLayer();
  }
  if (layers_) RMFree(layers_);
}

CuckooChain::CuckooChain(CuckooChain&& other) noexcept
    : layers_(other.layers_),
      numLayers_(other.numLayers_),
      layerCapacity_(other.layerCapacity_),
      totalItems_(other.totalItems_),
      totalDeleted_(other.totalDeleted_),
      bucketSize_(other.bucketSize_),
      maxIterations_(other.maxIterations_),
      expansion_(other.expansion_),
      maxExpansions_(other.maxExpansions_),
      loading_(other.loading_),
      chunksLoaded_(other.chunksLoaded_) {
  other.layers_ = nullptr;
  other.numLayers_ = 0;
  other.layerCapacity_ = 0;
}

CuckooChain& CuckooChain::operator=(CuckooChain&& other) noexcept {
  if (this != &other) {
    for (size_t i = 0; i < numLayers_; i++)
      layers_[i].~CfFilterLayer();
    if (layers_) RMFree(layers_);
    layers_ = other.layers_;
    numLayers_ = other.numLayers_;
    layerCapacity_ = other.layerCapacity_;
    totalItems_ = other.totalItems_;
    totalDeleted_ = other.totalDeleted_;
    bucketSize_ = other.bucketSize_;
    maxIterations_ = other.maxIterations_;
    expansion_ = other.expansion_;
    maxExpansions_ = other.maxExpansions_;
    loading_ = other.loading_;
    chunksLoaded_ = other.chunksLoaded_;
    other.layers_ = nullptr;
    other.numLayers_ = 0;
    other.layerCapacity_ = 0;
  }
  return *this;
}

// --- Internal helpers ---

bool CuckooChain::AppendLayer(uint64_t numBuckets) {
  if (numLayers_ >= layerCapacity_) {
    size_t newCap = std::max(layerCapacity_ * 2, size_t{4});
    if (newCap > SIZE_MAX / sizeof(CfFilterLayer)) return false;
    auto* expanded = static_cast<CfFilterLayer*>(RMAlloc(newCap * sizeof(CfFilterLayer)));
    if (!expanded) return false;
    for (size_t i = 0; i < numLayers_; i++) {
      new (&expanded[i]) CfFilterLayer{std::move(layers_[i].cuckoo)};
      layers_[i].~CfFilterLayer();
    }
    if (layers_) RMFree(layers_);
    layers_ = expanded;
    layerCapacity_ = newCap;
  }

  auto maybeFilter = CuckooFilter::Create(numBuckets, bucketSize_, maxIterations_);
  if (!maybeFilter) return false;

  auto* slot = &layers_[numLayers_];
  new (slot) CfFilterLayer{std::move(*maybeFilter)};
  numLayers_++;
  return true;
}

// --- Public API ---

std::optional<bool> CuckooChain::Add(std::span<const std::byte> item) {
  auto result = layers_[numLayers_ - 1].cuckoo.Insert(item);
  if (result.has_value()) {
    totalItems_++;
    return true;
  }

  if (expansion_ == 0 || numLayers_ >= maxExpansions_) return std::nullopt;

  uint64_t prevBuckets = layers_[numLayers_ - 1].cuckoo.NumBuckets();
  uint64_t nextBuckets = prevBuckets;
  if (expansion_ > 1) {
    if (prevBuckets > kCfMaxBuckets / expansion_) return std::nullopt;
    nextBuckets = prevBuckets * expansion_;
  }
  if (nextBuckets > kCfMaxBuckets || !AppendLayer(nextBuckets)) return std::nullopt;

  result = layers_[numLayers_ - 1].cuckoo.Insert(item);
  if (!result.has_value()) return std::nullopt;
  totalItems_++;
  return true;
}

bool CuckooChain::Contains(std::span<const std::byte> item) const {
  for (const auto& layer : Layers()) {
    if (layer.cuckoo.Contains(item)) return true;
  }
  return false;
}

bool CuckooChain::Delete(std::span<const std::byte> item) {
  for (auto& layer : Layers()) {
    if (layer.cuckoo.Delete(item)) {
      totalDeleted_++;
      return true;
    }
  }
  return false;
}

uint64_t CuckooChain::Count(std::span<const std::byte> item) const {
  uint64_t total = 0;
  for (const auto& layer : Layers()) {
    total += layer.cuckoo.Count(item);
  }
  return total;
}

uint64_t CuckooChain::TotalBuckets() const {
  uint64_t total = 0;
  for (const auto& layer : Layers()) {
    total += layer.cuckoo.NumBuckets();
  }
  return total;
}

uint64_t CuckooChain::BytesUsed() const {
  uint64_t base = sizeof(CuckooChain) + layerCapacity_ * sizeof(CfFilterLayer);
  for (const auto& layer : Layers()) {
    base += layer.cuckoo.GetDataSize();
  }
  return base;
}

// --- Shell construction for deserialization ---

CuckooChain* CuckooChain::FromRdbShell(const RdbShell& shell) {
  auto* chain = static_cast<CuckooChain*>(RMAlloc(sizeof(CuckooChain)));
  if (!chain) return nullptr;

  new (chain) CuckooChain(EmptyShellTag{});
  chain->layers_ = static_cast<CfFilterLayer*>(RMCalloc(shell.numLayers, sizeof(CfFilterLayer)));
  if (!chain->layers_) {
    chain->~CuckooChain();
    RMFree(chain);
    return nullptr;
  }
  chain->numLayers_ = 0;
  chain->layerCapacity_ = shell.numLayers;
  chain->totalItems_ = shell.totalItems;
  chain->totalDeleted_ = shell.totalDeleted;
  chain->bucketSize_ = shell.bucketSize;
  chain->maxIterations_ = shell.maxIterations;
  chain->expansion_ = shell.expansion;
  chain->maxExpansions_ = shell.maxExpansions;
  return chain;
}

void CuckooChain::SetLayer(size_t index, CfFilterLayer&& layer) {
  if (index < layerCapacity_) {
    new (&layers_[index]) CfFilterLayer{std::move(layer.cuckoo)};
    if (index >= numLayers_) numLayers_ = index + 1;
  }
}

CuckooChain* CuckooChain::Clone() const {
  auto* clone = FromRdbShell({totalItems_, totalDeleted_, static_cast<uint32_t>(numLayers_),
                               bucketSize_, maxIterations_, expansion_, maxExpansions_});
  if (!clone) return nullptr;

  for (size_t i = 0; i < numLayers_; i++) {
    auto clonedFilter = layers_[i].cuckoo.Clone();
    if (!clonedFilter) {
      clone->~CuckooChain();
      RMFree(clone);
      return nullptr;
    }
    clone->SetLayer(i, CfFilterLayer{std::move(*clonedFilter)});
  }
  return clone;
}
