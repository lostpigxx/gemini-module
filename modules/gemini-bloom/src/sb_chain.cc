#include "sb_chain.h"
#include "rm_alloc.h"

#include <algorithm>
#include <cstring>

// --- Lifecycle ---

ScalingBloomFilter::ScalingBloomFilter(uint64_t initialCapacity, double errorRate,
                                        BloomFlags flg, unsigned expansion)
    : flags_(flg), expansionFactor_(expansion) {
  double firstRate = HasFlag(flg, BloomFlags::FixedSize)
    ? errorRate
    : errorRate * kTighteningRatio;

  if (!AppendLayer(initialCapacity, firstRate)) {
    if (layers_) RMFree(layers_);
    layers_ = nullptr;
    numLayers_ = 0;
    layerCapacity_ = 0;
  }
}

ScalingBloomFilter::~ScalingBloomFilter() {
  for (size_t i = 0; i < numLayers_; i++) {
    layers_[i].~FilterLayer();
  }
  if (layers_) RMFree(layers_);
}

ScalingBloomFilter::ScalingBloomFilter(ScalingBloomFilter&& other) noexcept
    : layers_(other.layers_),
      totalItems_(other.totalItems_),
      numLayers_(other.numLayers_),
      layerCapacity_(other.layerCapacity_),
      flags_(other.flags_),
      expansionFactor_(other.expansionFactor_) {
  other.layers_ = nullptr;
  other.numLayers_ = 0;
  other.layerCapacity_ = 0;
}

ScalingBloomFilter& ScalingBloomFilter::operator=(ScalingBloomFilter&& other) noexcept {
  if (this != &other) {
    for (size_t i = 0; i < numLayers_; i++)
      layers_[i].~FilterLayer();
    if (layers_) RMFree(layers_);
    layers_ = other.layers_;
    totalItems_ = other.totalItems_;
    numLayers_ = other.numLayers_;
    layerCapacity_ = other.layerCapacity_;
    flags_ = other.flags_;
    expansionFactor_ = other.expansionFactor_;
    other.layers_ = nullptr;
    other.numLayers_ = 0;
    other.layerCapacity_ = 0;
  }
  return *this;
}

// --- Internal helpers ---

bool ScalingBloomFilter::AppendLayer(uint64_t cap, double rate) {
  if (numLayers_ >= layerCapacity_) {
    size_t newCap = std::max(layerCapacity_ * 2, size_t{4});
    if (newCap > SIZE_MAX / sizeof(FilterLayer)) return false;
    auto* expanded = static_cast<FilterLayer*>(RMAlloc(newCap * sizeof(FilterLayer)));
    if (!expanded) return false;
    for (size_t i = 0; i < numLayers_; i++) {
      new (&expanded[i]) FilterLayer{std::move(layers_[i].bloom), layers_[i].itemCount};
      layers_[i].~FilterLayer();
    }
    if (layers_) RMFree(layers_);
    layers_ = expanded;
    layerCapacity_ = newCap;
  }

  BloomLayer layer;
  if (!BloomLayer::Create(cap, rate, flags_, &layer)) return false;

  uint64_t newDataSize = layer.GetDataSize();
  if (newDataSize > kMaxTotalDataSize || TotalDataSize() > kMaxTotalDataSize - newDataSize)
    return false;

  auto* slot = &layers_[numLayers_];
  new (slot) FilterLayer{std::move(layer), 0};
  numLayers_++;
  return true;
}

HashPair ScalingBloomFilter::ComputeHash(const void* data, size_t len) const {
  return HasFlag(flags_, BloomFlags::Use64Bit)
    ? Hash64Policy::Compute(data, len)
    : Hash32Policy::Compute(data, len);
}

bool ScalingBloomFilter::IsDuplicate(const HashPair& hp) const {
  for (size_t i = numLayers_; i > 0; --i) {
    if (layers_[i - 1].bloom.Test(hp)) return true;
  }
  return false;
}

bool ScalingBloomFilter::GrowIfNeeded() {
  auto& top = layers_[numLayers_ - 1];
  if (top.itemCount < top.bloom.GetCapacity()) return true;
  if (HasFlag(flags_, BloomFlags::FixedSize)) return false;

  uint64_t prevCap = top.bloom.GetCapacity();
  if (prevCap > UINT64_MAX / expansionFactor_) return false;
  uint64_t nextCap = prevCap * expansionFactor_;
  constexpr double kMinFpRate = 1e-15;
  double nextRate = top.bloom.GetFpRate() * kTighteningRatio;
  return (nextRate >= kMinFpRate) && AppendLayer(nextCap, nextRate);
}

// --- Public API ---

PutResult ScalingBloomFilter::Put(const void* data, size_t len) {
  auto hp = ComputeHash(data, len);

  if (IsDuplicate(hp)) return PutResult::Duplicate;
  if (!GrowIfNeeded()) return PutResult::Full;

  auto& target = layers_[numLayers_ - 1];
  target.bloom.Insert(hp);
  target.itemCount++;
  totalItems_++;
  return PutResult::Inserted;
}

bool ScalingBloomFilter::Contains(const void* data, size_t len) const {
  return IsDuplicate(ComputeHash(data, len));
}

uint64_t ScalingBloomFilter::TotalCapacity() const {
  uint64_t total = 0;
  for (size_t i = 0; i < numLayers_; ++i) {
    total += layers_[i].bloom.GetCapacity();
  }
  return total;
}

uint64_t ScalingBloomFilter::TotalDataSize() const {
  uint64_t total = 0;
  for (size_t i = 0; i < numLayers_; ++i) {
    total += layers_[i].bloom.GetDataSize();
  }
  return total;
}

size_t ScalingBloomFilter::BytesUsed() const {
  size_t base = sizeof(ScalingBloomFilter) + layerCapacity_ * sizeof(FilterLayer);
  for (size_t i = 0; i < numLayers_; ++i) {
    base += static_cast<size_t>(layers_[i].bloom.GetDataSize());
  }
  return base;
}

// --- Shell construction for deserialization ---

ScalingBloomFilter* ScalingBloomFilter::FromRdbShell(RdbShell shell) {
  auto* filter = static_cast<ScalingBloomFilter*>(RMAlloc(sizeof(ScalingBloomFilter)));
  if (!filter) return nullptr;

  new (filter) ScalingBloomFilter(EmptyShellTag{});
  filter->layers_ = static_cast<FilterLayer*>(RMCalloc(shell.numLayers, sizeof(FilterLayer)));
  if (!filter->layers_) {
    filter->~ScalingBloomFilter();
    RMFree(filter);
    return nullptr;
  }
  filter->totalItems_ = shell.totalItems;
  filter->numLayers_ = 0;
  filter->layerCapacity_ = shell.numLayers;
  filter->flags_ = shell.flags;
  filter->expansionFactor_ = shell.expansionFactor;
  return filter;
}

void ScalingBloomFilter::SetLayer(size_t index, FilterLayer&& layer) {
  if (index < layerCapacity_) {
    new (&layers_[index]) FilterLayer{std::move(layer.bloom), layer.itemCount};
    if (index >= numLayers_) numLayers_ = index + 1;
  }
}

ScalingBloomFilter* ScalingBloomFilter::Clone() const {
  auto* clone = FromRdbShell({totalItems_, numLayers_, flags_, expansionFactor_});
  if (!clone) return nullptr;

  for (size_t i = 0; i < numLayers_; i++) {
    BloomLayer clonedLayer;
    if (!layers_[i].bloom.Clone(&clonedLayer)) {
      clone->~ScalingBloomFilter();
      RMFree(clone);
      return nullptr;
    }
    clone->SetLayer(i, {std::move(clonedLayer), layers_[i].itemCount});
  }
  return clone;
}

// WriteTo, ReadFrom, SerializeHeader, DeserializeHeader live in bloom_rdb.cc
// to keep Redis Module API dependencies out of test builds.
