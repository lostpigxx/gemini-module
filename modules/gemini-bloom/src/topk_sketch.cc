#include "topk_sketch.h"
#include "murmur2.h"
#include "rm_alloc.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace {
// Distinct from the bloom/cuckoo/CMS seeds — this is a private wire format
// with no compatibility requirement with any external sketch's hashing.
constexpr uint32_t kTopkRowSeedBase = 0x2f5a9d31u;
constexpr uint32_t kTopkFpSeed = 0x7ed2b6a3u;

uint64_t Splitmix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  uint64_t z = x;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}
}  // namespace

TopkSketch::~TopkSketch() {
  if (entries_) {
    for (uint32_t i = 0; i < numActive_; i++) FreeEntryName(i);
    RMFree(entries_);
    entries_ = nullptr;
  }
  if (cells_) {
    RMFree(cells_);
    cells_ = nullptr;
  }
}

TopkSketch::TopkSketch(TopkSketch&& other) noexcept
    : k_(other.k_),
      width_(other.width_),
      depth_(other.depth_),
      decay_(other.decay_),
      cells_(other.cells_),
      entries_(other.entries_),
      numActive_(other.numActive_),
      rngState_(other.rngState_) {
  other.cells_ = nullptr;
  other.entries_ = nullptr;
  other.numActive_ = 0;
  other.k_ = 0;
  other.width_ = 0;
  other.depth_ = 0;
}

TopkSketch& TopkSketch::operator=(TopkSketch&& other) noexcept {
  if (this != &other) {
    if (entries_) {
      for (uint32_t i = 0; i < numActive_; i++) FreeEntryName(i);
      RMFree(entries_);
    }
    if (cells_) RMFree(cells_);

    k_ = other.k_;
    width_ = other.width_;
    depth_ = other.depth_;
    decay_ = other.decay_;
    cells_ = other.cells_;
    entries_ = other.entries_;
    numActive_ = other.numActive_;
    rngState_ = other.rngState_;

    other.cells_ = nullptr;
    other.entries_ = nullptr;
    other.numActive_ = 0;
    other.k_ = 0;
    other.width_ = 0;
    other.depth_ = 0;
  }
  return *this;
}

std::optional<TopkSketch> TopkSketch::Create(uint32_t k, uint32_t width, uint32_t depth, double decay) {
  if (k == 0 || k > kTopkMaxK) return std::nullopt;
  if (width == 0 || width > kTopkMaxWidth) return std::nullopt;
  if (depth == 0 || depth > kTopkMaxDepth) return std::nullopt;
  if (static_cast<uint64_t>(width) * depth > kTopkMaxCells) return std::nullopt;
  if (!std::isfinite(decay) || decay <= 0.0 || decay >= 1.0) return std::nullopt;

  TopkSketch sketch;
  sketch.k_ = k;
  sketch.width_ = width;
  sketch.depth_ = depth;
  sketch.decay_ = decay;
  sketch.cells_ = static_cast<Cell*>(RMCalloc(static_cast<size_t>(width) * depth, sizeof(Cell)));
  if (!sketch.cells_) return std::nullopt;
  sketch.entries_ = static_cast<Entry*>(RMCalloc(k, sizeof(Entry)));
  if (!sketch.entries_) {
    RMFree(sketch.cells_);
    return std::nullopt;
  }
  return sketch;
}

uint32_t TopkSketch::FingerprintOf(std::span<const std::byte> item) const {
  auto* ptr = reinterpret_cast<const void*>(item.data());
  auto len = static_cast<int>(std::min(item.size(), static_cast<size_t>(INT_MAX)));
  uint32_t h = MurmurHash2(ptr, len, kTopkFpSeed);
  return h == 0 ? 1 : h;
}

uint32_t TopkSketch::BucketOf(std::span<const std::byte> item, uint32_t row) const {
  auto* ptr = reinterpret_cast<const void*>(item.data());
  auto len = static_cast<int>(std::min(item.size(), static_cast<size_t>(INT_MAX)));
  uint32_t h = MurmurHash2(ptr, len, kTopkRowSeedBase + row);
  return h % width_;
}

double TopkSketch::NextRandom() {
  rngState_ = Splitmix64(rngState_);
  // Top 53 bits give a double uniformly distributed in [0, 1).
  return static_cast<double>(rngState_ >> 11) * (1.0 / 9007199254740992.0);
}

int TopkSketch::FindEntryIndex(std::span<const std::byte> item) const {
  for (uint32_t i = 0; i < numActive_; i++) {
    if (entries_[i].nameLen == item.size() &&
        std::memcmp(entries_[i].name, item.data(), item.size()) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void TopkSketch::FreeEntryName(uint32_t idx) {
  if (entries_[idx].name) {
    RMFree(entries_[idx].name);
    entries_[idx].name = nullptr;
    entries_[idx].nameLen = 0;
  }
}

void TopkSketch::SetEntryName(uint32_t idx, std::span<const std::byte> name) {
  FreeEntryName(idx);
  auto* buf = static_cast<char*>(RMAlloc(name.size() == 0 ? 1 : name.size()));
  if (name.size() > 0) std::memcpy(buf, name.data(), name.size());
  entries_[idx].name = buf;
  entries_[idx].nameLen = static_cast<uint32_t>(name.size());
}

// Per-row HeavyKeeper update: on a fingerprint match (or an empty cell),
// the counter is bumped; on a mismatch, the existing counter is decayed
// with probability decay^count, making heavier existing occupants
// exponentially harder to evict. Returns the max counter value across rows
// where this item's fingerprint ended up occupying the cell (0 if it never
// did, i.e. it collided everywhere without winning a decay).
uint64_t TopkSketch::ApplyOneIncrement(std::span<const std::byte> item) {
  uint32_t fp = FingerprintOf(item);
  uint64_t fpMatchCount = 0;

  for (uint32_t row = 0; row < depth_; row++) {
    uint32_t col = BucketOf(item, row);
    Cell& cell = cells_[row * width_ + col];

    if (cell.count == 0) {
      cell.fingerprint = fp;
      cell.count = 1;
      fpMatchCount = std::max<uint64_t>(fpMatchCount, 1);
    } else if (cell.fingerprint == fp) {
      cell.count++;
      fpMatchCount = std::max<uint64_t>(fpMatchCount, cell.count);
    } else {
      double p = std::pow(decay_, static_cast<double>(cell.count));
      if (NextRandom() < p) {
        cell.count--;
        if (cell.count == 0) {
          cell.fingerprint = fp;
          cell.count = 1;
          fpMatchCount = std::max<uint64_t>(fpMatchCount, 1);
        }
      }
    }
  }
  return fpMatchCount;
}

std::optional<std::string> TopkSketch::UpdateTopK(std::span<const std::byte> item, uint64_t newCount) {
  int idx = FindEntryIndex(item);
  if (idx >= 0) {
    auto i = static_cast<uint32_t>(idx);
    entries_[i].count += 1;
    while (i > 0 && entries_[i - 1].count < entries_[i].count) {
      std::swap(entries_[i - 1], entries_[i]);
      i--;
    }
    return std::nullopt;
  }

  uint64_t candidateCount = std::max<uint64_t>(newCount, 1);

  if (numActive_ < k_) {
    uint32_t i = numActive_;
    SetEntryName(i, item);
    entries_[i].count = candidateCount;
    numActive_++;
    while (i > 0 && entries_[i - 1].count < entries_[i].count) {
      std::swap(entries_[i - 1], entries_[i]);
      i--;
    }
    return std::nullopt;
  }

  uint32_t minIdx = numActive_ - 1;
  if (candidateCount <= entries_[minIdx].count) return std::nullopt;

  std::string evicted(entries_[minIdx].name, entries_[minIdx].nameLen);
  SetEntryName(minIdx, item);
  entries_[minIdx].count = candidateCount;

  uint32_t i = minIdx;
  while (i > 0 && entries_[i - 1].count < entries_[i].count) {
    std::swap(entries_[i - 1], entries_[i]);
    i--;
  }
  return evicted;
}

std::optional<std::string> TopkSketch::Add(std::span<const std::byte> item) {
  uint64_t fpMatchCount = ApplyOneIncrement(item);
  return UpdateTopK(item, fpMatchCount);
}

std::optional<std::string> TopkSketch::IncrBy(std::span<const std::byte> item, uint32_t increment) {
  std::optional<std::string> lastEvicted;
  for (uint32_t i = 0; i < increment; i++) {
    auto evicted = Add(item);
    if (evicted.has_value()) lastEvicted = std::move(evicted);
  }
  return lastEvicted;
}

bool TopkSketch::Query(std::span<const std::byte> item) const {
  return FindEntryIndex(item) >= 0;
}

uint64_t TopkSketch::Count(std::span<const std::byte> item) const {
  int idx = FindEntryIndex(item);
  if (idx >= 0) return entries_[static_cast<uint32_t>(idx)].count;

  uint32_t fp = FingerprintOf(item);
  uint64_t best = 0;
  for (uint32_t row = 0; row < depth_; row++) {
    uint32_t col = BucketOf(item, row);
    const Cell& cell = cells_[row * width_ + col];
    if (cell.fingerprint == fp && cell.count > 0) {
      best = std::max<uint64_t>(best, cell.count);
    }
  }
  return best;
}

std::vector<std::pair<std::string, uint64_t>> TopkSketch::List() const {
  std::vector<std::pair<std::string, uint64_t>> result;
  result.reserve(numActive_);
  for (uint32_t i = 0; i < numActive_; i++) {
    result.emplace_back(std::string(entries_[i].name, entries_[i].nameLen), entries_[i].count);
  }
  return result;
}

void TopkSketch::AppendEntryForLoad(std::span<const std::byte> name, uint64_t count) {
  uint32_t i = numActive_;
  SetEntryName(i, name);
  entries_[i].count = count;
  numActive_++;
}

std::optional<TopkSketch> TopkSketch::Clone() const {
  TopkSketch copy;
  copy.k_ = k_;
  copy.width_ = width_;
  copy.depth_ = depth_;
  copy.decay_ = decay_;
  copy.rngState_ = rngState_;

  uint64_t cellDataSize = GetCellDataSize();
  copy.cells_ = static_cast<Cell*>(RMAlloc(cellDataSize));
  if (!copy.cells_) return std::nullopt;
  std::memcpy(copy.cells_, cells_, cellDataSize);

  copy.entries_ = static_cast<Entry*>(RMCalloc(k_, sizeof(Entry)));
  if (!copy.entries_) {
    RMFree(copy.cells_);
    return std::nullopt;
  }
  for (uint32_t i = 0; i < numActive_; i++) {
    copy.AppendEntryForLoad(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(entries_[i].name), entries_[i].nameLen), entries_[i].count);
  }
  return copy;
}
