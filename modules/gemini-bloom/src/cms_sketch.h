#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// --- Count-Min Sketch: width x depth counter table ---
//
// Reference: Cormode, Muthukrishnan, "An Improved Data Stream Summary: The
// Count-Min Sketch and its Applications" (2005). This implementation is an
// independent, clean-room design derived only from the paper's publicly
// described algorithm and the public CMS.* command documentation on
// redis.io; no RedisBloom source was consulted.
//
// Sizing follows the paper's standard formulas: width = ceil(e / error),
// depth = ceil(ln(1 / probability)), where e is Euler's number.
//
// Counters are stored as a flat depth*width array of uint32_t (row r,
// column c at index r*width + c). Row r's column for an item is derived by
// hashing the item with a row-specific seed (MurmurHash2 with distinct
// seeds per row, following the same "one hash function, many seeds"
// technique documented for bloom filters — see doc/redis_bloom_survey.md
// §3.2).

constexpr uint32_t kCmsMaxWidth = 1u << 26;
constexpr uint32_t kCmsMaxDepth = 256;
// Bounds total allocation (counters_ array) to 1 GiB (4 bytes/counter).
constexpr uint64_t kCmsMaxCounters = 1ULL << 28;

class CmsSketch {
public:
  CmsSketch() = default;
  ~CmsSketch();

  CmsSketch(const CmsSketch&) = delete;
  CmsSketch& operator=(const CmsSketch&) = delete;

  CmsSketch(CmsSketch&& other) noexcept;
  CmsSketch& operator=(CmsSketch&& other) noexcept;

  static std::optional<CmsSketch> Create(uint32_t width, uint32_t depth);
  // Returns nullopt if error/probability are out of (0, 1) or the derived
  // width/depth would exceed kCmsMaxWidth/kCmsMaxDepth.
  static std::optional<CmsSketch> CreateByProb(double error, double probability);

  // Increments every row's counter for `item` by `delta` (may be negative,
  // per the public CMS.INCRBY "increment" argument being a signed integer).
  // Validates first that every row's resulting counter stays within
  // [0, UINT32_MAX] and only then applies the change, so a call that would
  // overflow/underflow leaves the sketch untouched. Returns the new
  // min-count estimate on success, nullopt on overflow.
  std::optional<uint64_t> IncrBy(std::span<const std::byte> item, int64_t delta);
  uint64_t Query(std::span<const std::byte> item) const;

  // Merges `sources` (each weighted by the corresponding entry in
  // `weights`) into *this*, replacing this sketch's current counters and
  // totalCount with the weighted sum. All sources must share this sketch's
  // width/depth. Returns false (leaving *this* unmodified) on a dimension
  // mismatch.
  bool Merge(std::span<const CmsSketch* const> sources, std::span<const double> weights);

  std::optional<CmsSketch> Clone() const;

  // For defrag: adopt a relocated counters buffer. Caller owns the
  // relocation (e.g. via RedisModule_DefragAlloc) and transfers ownership.
  void AdoptCounterArray(uint32_t* relocated) { counters_ = relocated; }

  uint32_t Width() const { return width_; }
  uint32_t Depth() const { return depth_; }
  uint64_t TotalCount() const { return totalCount_; }
  // For RDB load: populate totalCount_ after the counters array has been
  // filled in directly (bypassing IncrBy).
  void SetTotalCount(uint64_t v) { totalCount_ = v; }

  const uint32_t* GetCounterArray() const { return counters_; }
  uint32_t* GetCounterArray() { return counters_; }
  uint64_t GetDataSize() const { return static_cast<uint64_t>(width_) * depth_ * sizeof(uint32_t); }

private:
  uint32_t& CounterAt(uint32_t row, uint32_t col) { return counters_[row * width_ + col]; }
  const uint32_t& CounterAt(uint32_t row, uint32_t col) const { return counters_[row * width_ + col]; }
  uint32_t ColumnOf(std::span<const std::byte> item, uint32_t row) const;

  uint32_t width_ = 0;
  uint32_t depth_ = 0;
  uint32_t* counters_ = nullptr;
  uint64_t totalCount_ = 0;
};
