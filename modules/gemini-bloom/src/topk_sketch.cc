#include "topk_sketch.h"
#include "murmur2.h"
#include "rm_alloc.h"

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>

namespace {
// Distinct from the bloom/cuckoo/CMS seeds — this is a private wire format
// with no compatibility requirement with any external sketch's hashing.
constexpr uint32_t kTopkRowSeedBase = 0x2f5a9d31u;
constexpr uint32_t kTopkFpSeed = 0x7ed2b6a3u;
constexpr uint32_t kTopkNameHashSeed = 0x517cc1b7u;

inline uint64_t ComputeFastMod32M(uint32_t d) {
  return (d <= 1) ? 0 : (UINT64_MAX / d + 1);
}

inline uint32_t FastMod32(uint32_t a, uint64_t M, uint32_t d) {
  if (d <= 1) return 0;
  __extension__ using u128 = unsigned __int128;
  uint64_t lowbits = M * a;
  return static_cast<uint32_t>((static_cast<u128>(lowbits) * d) >> 64);
}

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
  if (indexTable_) {
    RMFree(indexTable_);
    indexTable_ = nullptr;
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
      widthFastModM_(other.widthFastModM_),
      decay_(other.decay_),
      maxDecayCount_(other.maxDecayCount_),
      cells_(other.cells_),
      entries_(other.entries_),
      indexTable_(other.indexTable_),
      indexCap_(other.indexCap_),
      numActive_(other.numActive_),
      rngState_(other.rngState_) {
  std::memcpy(decayTable_, other.decayTable_, sizeof(decayTable_));
  other.cells_ = nullptr;
  other.entries_ = nullptr;
  other.indexTable_ = nullptr;
  other.indexCap_ = 0;
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
    if (indexTable_) RMFree(indexTable_);
    if (cells_) RMFree(cells_);

    k_ = other.k_;
    width_ = other.width_;
    depth_ = other.depth_;
    widthFastModM_ = other.widthFastModM_;
    decay_ = other.decay_;
    maxDecayCount_ = other.maxDecayCount_;
    std::memcpy(decayTable_, other.decayTable_, sizeof(decayTable_));
    cells_ = other.cells_;
    entries_ = other.entries_;
    indexTable_ = other.indexTable_;
    indexCap_ = other.indexCap_;
    numActive_ = other.numActive_;
    rngState_ = other.rngState_;

    other.cells_ = nullptr;
    other.entries_ = nullptr;
    other.indexTable_ = nullptr;
    other.indexCap_ = 0;
    other.numActive_ = 0;
    other.k_ = 0;
    other.width_ = 0;
    other.depth_ = 0;
  }
  return *this;
}

void TopkSketch::InitTables() {
  widthFastModM_ = ComputeFastMod32M(width_);
  double p = 1.0;
  for (uint32_t c = 0; c < kDecayLutSize; c++) {
    decayTable_[c] = p;
    p *= decay_;
  }
  // Beyond maxDecayCount_, decay_^c < 2^-53 (below NextRandom() granularity).
  double logDecay = std::log(decay_);
  if (logDecay < 0.0) {
    double cutoff = std::ceil(std::log(1e-16) / logDecay);
    if (!std::isfinite(cutoff) || cutoff >= static_cast<double>(UINT32_MAX)) {
      maxDecayCount_ = UINT32_MAX;
    } else {
      maxDecayCount_ = static_cast<uint32_t>(std::max(1.0, cutoff));
    }
  } else {
    maxDecayCount_ = UINT32_MAX;
  }
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
  sketch.InitTables();

  sketch.cells_ = static_cast<Cell*>(RMCalloc(static_cast<size_t>(width) * depth, sizeof(Cell)));
  if (!sketch.cells_) return std::nullopt;
  sketch.entries_ = static_cast<Entry*>(RMCalloc(k, sizeof(Entry)));
  if (!sketch.entries_) return std::nullopt;

  uint32_t idxCap = std::bit_ceil(std::max<uint32_t>(16u, k * 2u));
  sketch.indexTable_ = static_cast<uint32_t*>(RMCalloc(idxCap, sizeof(uint32_t)));
  if (!sketch.indexTable_) return std::nullopt;
  sketch.indexCap_ = idxCap;
  return sketch;
}

std::optional<TopkSketch> TopkSketch::FromLoadedBuffer(uint32_t k, uint32_t width, uint32_t depth,
                                                       double decay, Cell* adoptedCells) {
  if (!adoptedCells) return std::nullopt;
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
  sketch.InitTables();
  sketch.cells_ = adoptedCells;

  sketch.entries_ = static_cast<Entry*>(RMCalloc(k, sizeof(Entry)));
  if (!sketch.entries_) {
    sketch.cells_ = nullptr;  // Caller frees adoptedCells on failure
    return std::nullopt;
  }
  uint32_t idxCap = std::bit_ceil(std::max<uint32_t>(16u, k * 2u));
  sketch.indexTable_ = static_cast<uint32_t*>(RMCalloc(idxCap, sizeof(uint32_t)));
  if (!sketch.indexTable_) {
    sketch.cells_ = nullptr;  // Caller frees adoptedCells on failure
    return std::nullopt;
  }
  sketch.indexCap_ = idxCap;
  return sketch;
}

uint32_t TopkSketch::FingerprintOf(std::span<const std::byte> item) const {
  auto* ptr = reinterpret_cast<const void*>(item.data());
  auto len = static_cast<int>(std::min(item.size(), static_cast<size_t>(INT_MAX)));
  uint32_t h = MurmurHash2(ptr, len, kTopkFpSeed);
  return h == 0 ? 1 : h;
}

uint32_t TopkSketch::NameHashOf(std::span<const std::byte> item) const {
  auto* ptr = reinterpret_cast<const void*>(item.data());
  auto len = static_cast<int>(std::min(item.size(), static_cast<size_t>(INT_MAX)));
  return MurmurHash2(ptr, len, kTopkNameHashSeed);
}

uint32_t TopkSketch::BucketOf(std::span<const std::byte> item, uint32_t row) const {
  auto* ptr = reinterpret_cast<const void*>(item.data());
  auto len = static_cast<int>(std::min(item.size(), static_cast<size_t>(INT_MAX)));
  uint32_t h = MurmurHash2(ptr, len, kTopkRowSeedBase + row);
  return FastMod32(h, widthFastModM_, width_);
}

double TopkSketch::DecayProb(uint32_t count) const {
  if (count < kDecayLutSize) return decayTable_[count];
  if (count >= maxDecayCount_) return 0.0;
  return std::pow(decay_, static_cast<double>(count));
}

double TopkSketch::NextRandom() {
  rngState_ = Splitmix64(rngState_);
  // Top 53 bits give a double uniformly distributed in [0, 1).
  return static_cast<double>(rngState_ >> 11) * (1.0 / 9007199254740992.0);
}

int TopkSketch::FindEntryIndex(std::span<const std::byte> item, uint32_t nameHash) const {
  if (!indexTable_ || numActive_ == 0) return -1;
  uint32_t mask = indexCap_ - 1;
  uint32_t slot = nameHash & mask;
  while (true) {
    uint32_t val = indexTable_[slot];
    if (val == 0) return -1;
    uint32_t idx = val - 1;
    if (entries_[idx].nameHash == nameHash &&
        entries_[idx].nameLen == item.size() &&
        (item.size() == 0 || std::memcmp(entries_[idx].name, item.data(), item.size()) == 0)) {
      return static_cast<int>(idx);
    }
    slot = (slot + 1) & mask;
  }
}

void TopkSketch::IndexInsert(uint32_t entryIdx) {
  uint32_t mask = indexCap_ - 1;
  uint32_t slot = entries_[entryIdx].nameHash & mask;
  while (indexTable_[slot] != 0) slot = (slot + 1) & mask;
  indexTable_[slot] = entryIdx + 1;
}

void TopkSketch::IndexRemove(uint32_t entryIdx, uint32_t nameHash) {
  uint32_t mask = indexCap_ - 1;
  uint32_t slot = nameHash & mask;
  while (indexTable_[slot] != 0) {
    if (indexTable_[slot] == entryIdx + 1) break;
    slot = (slot + 1) & mask;
  }
  if (indexTable_[slot] == 0) return;

  // Backward-shift deletion preserves probe chains without tombstones.
  uint32_t hole = slot;
  uint32_t cur = (hole + 1) & mask;
  while (indexTable_[cur] != 0) {
    uint32_t curIdx = indexTable_[cur] - 1;
    uint32_t home = entries_[curIdx].nameHash & mask;
    if (((hole - home) & mask) < ((cur - home) & mask)) {
      indexTable_[hole] = indexTable_[cur];
      hole = cur;
    }
    cur = (cur + 1) & mask;
  }
  indexTable_[hole] = 0;
}

void TopkSketch::SwapEntries(uint32_t a, uint32_t b) {
  uint32_t mask = indexCap_ - 1;
  uint32_t slotA = entries_[a].nameHash & mask;
  while (indexTable_[slotA] != a + 1) slotA = (slotA + 1) & mask;
  uint32_t slotB = entries_[b].nameHash & mask;
  while (indexTable_[slotB] != b + 1) slotB = (slotB + 1) & mask;

  std::swap(entries_[a], entries_[b]);
  indexTable_[slotA] = b + 1;
  indexTable_[slotB] = a + 1;
}

void TopkSketch::FreeEntryName(uint32_t idx) {
  if (entries_[idx].name) {
    RMFree(entries_[idx].name);
    entries_[idx].name = nullptr;
    entries_[idx].nameLen = 0;
  }
}

bool TopkSketch::SetEntryName(uint32_t idx, std::span<const std::byte> name, uint32_t nameHash) {
  auto* buf = static_cast<char*>(RMAlloc(name.size() == 0 ? 1 : name.size()));
  if (!buf) return false;
  if (name.size() > 0) std::memcpy(buf, name.data(), name.size());
  FreeEntryName(idx);
  entries_[idx].name = buf;
  entries_[idx].nameLen = static_cast<uint32_t>(name.size());
  entries_[idx].nameHash = nameHash;
  return true;
}

// Batched HeavyKeeper update: hashes item once per row, applies full increment
// directly on empty/matching cells, and uses decayTable_ LUT on mismatches.
uint64_t TopkSketch::ApplyIncrements(std::span<const std::byte> item, uint32_t increment, uint32_t fp) {
  uint64_t fpMatchCount = 0;

  for (uint32_t row = 0; row < depth_; row++) {
    uint32_t col = BucketOf(item, row);
    Cell& cell = cells_[row * width_ + col];

    if (cell.count == 0) {
      cell.fingerprint = fp;
      cell.count = increment;
      fpMatchCount = std::max<uint64_t>(fpMatchCount, cell.count);
    } else if (cell.fingerprint == fp) {
      uint64_t sum = static_cast<uint64_t>(cell.count) + increment;
      cell.count = (sum > UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(sum);
      fpMatchCount = std::max<uint64_t>(fpMatchCount, cell.count);
    } else {
      for (uint32_t rem = increment; rem > 0; rem--) {
        double p = DecayProb(cell.count);
        if (p == 0.0) break;  // Saturated occupant; decay probability underflows to 0.
        if (NextRandom() < p) {
          cell.count--;
          if (cell.count == 0) {
            cell.fingerprint = fp;
            cell.count = rem;
            fpMatchCount = std::max<uint64_t>(fpMatchCount, cell.count);
            break;
          }
        }
      }
    }
  }
  return fpMatchCount;
}

std::optional<std::string> TopkSketch::UpdateTopK(std::span<const std::byte> item, uint64_t newCount,
                                                   uint32_t increment, uint32_t nameHash) {
  int idx = FindEntryIndex(item, nameHash);
  if (idx >= 0) {
    auto i = static_cast<uint32_t>(idx);
    uint64_t inc = increment;
    entries_[i].count = (inc > UINT64_MAX - entries_[i].count) ? UINT64_MAX : entries_[i].count + inc;
    while (i > 0 && entries_[i - 1].count < entries_[i].count) {
      SwapEntries(i - 1, i);
      i--;
    }
    return std::nullopt;
  }

  uint64_t candidateCount = std::max<uint64_t>(newCount, 1);

  if (numActive_ < k_) {
    uint32_t i = numActive_;
    if (!SetEntryName(i, item, nameHash)) return std::nullopt;
    entries_[i].count = candidateCount;
    numActive_++;
    IndexInsert(i);
    while (i > 0 && entries_[i - 1].count < entries_[i].count) {
      SwapEntries(i - 1, i);
      i--;
    }
    return std::nullopt;
  }

  uint32_t minIdx = numActive_ - 1;
  if (candidateCount <= entries_[minIdx].count) return std::nullopt;

  std::string evicted(entries_[minIdx].name, entries_[minIdx].nameLen);
  uint32_t oldHash = entries_[minIdx].nameHash;
  if (!SetEntryName(minIdx, item, nameHash)) return std::nullopt;
  IndexRemove(minIdx, oldHash);
  entries_[minIdx].count = candidateCount;
  IndexInsert(minIdx);

  uint32_t i = minIdx;
  while (i > 0 && entries_[i - 1].count < entries_[i].count) {
    SwapEntries(i - 1, i);
    i--;
  }
  return evicted;
}

std::optional<std::string> TopkSketch::Add(std::span<const std::byte> item) {
  return IncrBy(item, 1);
}

std::optional<std::string> TopkSketch::IncrBy(std::span<const std::byte> item, uint32_t increment) {
  if (increment == 0) return std::nullopt;
  uint32_t fp = FingerprintOf(item);
  uint32_t nameHash = NameHashOf(item);
  uint64_t fpMatchCount = ApplyIncrements(item, increment, fp);
  return UpdateTopK(item, fpMatchCount, increment, nameHash);
}

bool TopkSketch::Query(std::span<const std::byte> item) const {
  return FindEntryIndex(item, NameHashOf(item)) >= 0;
}

uint64_t TopkSketch::Count(std::span<const std::byte> item) const {
  int idx = FindEntryIndex(item, NameHashOf(item));
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

bool TopkSketch::AppendEntryForLoad(std::span<const std::byte> name, uint64_t count) {
  if (numActive_ >= k_) return false;
  uint32_t i = numActive_;
  uint32_t nameHash = NameHashOf(name);
  if (!SetEntryName(i, name, nameHash)) return false;
  entries_[i].count = count;
  numActive_++;
  IndexInsert(i);
  return true;
}

std::optional<TopkSketch> TopkSketch::Clone() const {
  TopkSketch copy;
  copy.k_ = k_;
  copy.width_ = width_;
  copy.depth_ = depth_;
  copy.decay_ = decay_;
  copy.rngState_ = rngState_;
  copy.InitTables();

  uint64_t cellDataSize = GetCellDataSize();
  copy.cells_ = static_cast<Cell*>(RMAlloc(cellDataSize));
  if (!copy.cells_) return std::nullopt;
  if (cellDataSize > 0) std::memcpy(copy.cells_, cells_, cellDataSize);

  copy.entries_ = static_cast<Entry*>(RMCalloc(k_, sizeof(Entry)));
  if (!copy.entries_) return std::nullopt;

  copy.indexTable_ = static_cast<uint32_t*>(RMCalloc(indexCap_, sizeof(uint32_t)));
  if (!copy.indexTable_) return std::nullopt;
  copy.indexCap_ = indexCap_;

  for (uint32_t i = 0; i < numActive_; i++) {
    if (!copy.AppendEntryForLoad(std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(entries_[i].name), entries_[i].nameLen), entries_[i].count)) {
      return std::nullopt;
    }
  }
  return copy;
}
