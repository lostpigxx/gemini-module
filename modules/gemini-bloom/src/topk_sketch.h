#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// --- Top-K: approximate heavy-hitter tracking via HeavyKeeper ---
//
// Reference: Yang, Yan, Yu, Bruschi, Xie, Zhao, Cui, "HeavyKeeper: An
// Accurate Algorithm for Finding Top-k Elephant Flows" (USENIX ATC 2018).
// This implementation is an independent, clean-room design derived only
// from the paper's publicly described algorithm (probabilistic counter
// decay to bias against light-weight collisions) and the public TOPK.*
// command documentation on redis.io; no RedisBloom source was consulted.
//
// A width x depth table of {fingerprint, count} cells estimates each
// item's frequency: on a fingerprint mismatch, the existing counter is
// decayed with probability decay^count rather than always overwritten,
// which makes it exponentially harder to evict an already-heavy item. A
// small sorted array of up to `k` (name, count) entries tracks the current
// top-k list; membership changes only when an item's estimated count
// exceeds the current minimum entry.

constexpr uint32_t kTopkMaxK = 1u << 20;
constexpr uint32_t kTopkMaxWidth = 1u << 26;
constexpr uint32_t kTopkMaxDepth = 256;
// Bounds total cell allocation to ~1 GiB (8 bytes/cell).
constexpr uint64_t kTopkMaxCells = 1ULL << 27;
// Safety cap on a single item's name length, independent of any Redis
// proto-max-bulk-len setting.
constexpr uint32_t kTopkMaxNameLen = 1u << 20;

class TopkSketch {
public:
  struct Cell {
    uint32_t fingerprint = 0;
    uint32_t count = 0;
  };

  struct Entry {
    char* name = nullptr;
    uint32_t nameLen = 0;
    uint64_t count = 0;
  };

  TopkSketch() = default;
  ~TopkSketch();

  TopkSketch(const TopkSketch&) = delete;
  TopkSketch& operator=(const TopkSketch&) = delete;

  TopkSketch(TopkSketch&& other) noexcept;
  TopkSketch& operator=(TopkSketch&& other) noexcept;

  // decay must be in (0, 1) exclusive.
  static std::optional<TopkSketch> Create(uint32_t k, uint32_t width, uint32_t depth, double decay);

  // Processes one occurrence of `item`. If this causes another item to be
  // evicted from the top-k list, returns the evicted item's name.
  std::optional<std::string> Add(std::span<const std::byte> item);
  // Processes `increment` occurrences of `item` (increment >= 1). Returns
  // the name of whichever item was most recently evicted from the top-k
  // list during this call, if any.
  std::optional<std::string> IncrBy(std::span<const std::byte> item, uint32_t increment);

  // True if `item` is currently one of the top-k entries.
  bool Query(std::span<const std::byte> item) const;
  // Best-effort frequency estimate: the entry's own count if `item` is in
  // the top-k list, otherwise the max matching-fingerprint cell count
  // across rows (0 if no row's fingerprint matches). Per the public
  // TOPK.COUNT documentation, this is a deprecated, unreliable estimate.
  uint64_t Count(std::span<const std::byte> item) const;
  // Current top-k entries, sorted by descending estimated count.
  std::vector<std::pair<std::string, uint64_t>> List() const;

  std::optional<TopkSketch> Clone() const;

  uint32_t K() const { return k_; }
  uint32_t Width() const { return width_; }
  uint32_t Depth() const { return depth_; }
  double Decay() const { return decay_; }
  uint32_t NumActive() const { return numActive_; }

  const Cell* GetCellArray() const { return cells_; }
  Cell* GetCellArray() { return cells_; }
  uint64_t GetCellDataSize() const { return static_cast<uint64_t>(width_) * depth_ * sizeof(Cell); }
  // For defrag: adopt a relocated cell-table buffer. Caller owns the
  // relocation (e.g. via RedisModule_DefragAlloc) and transfers ownership.
  void AdoptCellArray(Cell* relocated) { cells_ = relocated; }

  const Entry* GetEntries() const { return entries_; }
  Entry* GetEntries() { return entries_; }
  // For defrag: adopt a relocated entry-table buffer (holds k_ slots).
  void AdoptEntryArray(Entry* relocated) { entries_ = relocated; }

  // For RDB/wire load: append an owned copy of `name` with `count` as the
  // next active entry. Caller must append in descending-count order (as
  // this class always persists them) and never exceed k_ entries.
  void AppendEntryForLoad(std::span<const std::byte> name, uint64_t count);

private:
  uint32_t FingerprintOf(std::span<const std::byte> item) const;
  uint32_t BucketOf(std::span<const std::byte> item, uint32_t row) const;
  double NextRandom();

  uint64_t ApplyOneIncrement(std::span<const std::byte> item);
  std::optional<std::string> UpdateTopK(std::span<const std::byte> item, uint64_t newCount);
  int FindEntryIndex(std::span<const std::byte> item) const;
  void SetEntryName(uint32_t idx, std::span<const std::byte> name);
  void FreeEntryName(uint32_t idx);

  uint32_t k_ = 0;
  uint32_t width_ = 0;
  uint32_t depth_ = 0;
  double decay_ = 0.0;
  Cell* cells_ = nullptr;
  Entry* entries_ = nullptr;
  uint32_t numActive_ = 0;
  uint64_t rngState_ = 0x9e3779b97f4a7c15ULL;
};
