#include "cms_sketch.h"
#include "murmur2.h"
#include "rm_alloc.h"

#include <climits>
#include <cmath>
#include <cstring>
#include <numbers>

namespace {
// Distinct from the bloom/cuckoo filter seeds (see bloom_filter.cc,
// cuckoo_filter.cc) — this is a private wire format with no compatibility
// requirement with any external sketch's internal hashing.
constexpr uint32_t kCmsRowSeedBase = 0xc3157a5eu;
}  // namespace

CmsSketch::~CmsSketch() {
  if (counters_) {
    RMFree(counters_);
    counters_ = nullptr;
  }
}

CmsSketch::CmsSketch(CmsSketch&& other) noexcept
    : width_(other.width_),
      depth_(other.depth_),
      counters_(other.counters_),
      totalCount_(other.totalCount_) {
  other.counters_ = nullptr;
  other.width_ = 0;
  other.depth_ = 0;
  other.totalCount_ = 0;
}

CmsSketch& CmsSketch::operator=(CmsSketch&& other) noexcept {
  if (this != &other) {
    if (counters_) RMFree(counters_);
    width_ = other.width_;
    depth_ = other.depth_;
    counters_ = other.counters_;
    totalCount_ = other.totalCount_;
    other.counters_ = nullptr;
    other.width_ = 0;
    other.depth_ = 0;
    other.totalCount_ = 0;
  }
  return *this;
}

std::optional<CmsSketch> CmsSketch::Create(uint32_t width, uint32_t depth) {
  if (width == 0 || width > kCmsMaxWidth) return std::nullopt;
  if (depth == 0 || depth > kCmsMaxDepth) return std::nullopt;
  if (static_cast<uint64_t>(width) * depth > kCmsMaxCounters) return std::nullopt;

  CmsSketch sketch;
  sketch.width_ = width;
  sketch.depth_ = depth;
  sketch.counters_ = static_cast<uint32_t*>(RMCalloc(static_cast<size_t>(width) * depth, sizeof(uint32_t)));
  if (!sketch.counters_) return std::nullopt;
  return sketch;
}

std::optional<CmsSketch> CmsSketch::CreateByProb(double error, double probability) {
  if (!std::isfinite(error) || error <= 0.0 || error >= 1.0) return std::nullopt;
  if (!std::isfinite(probability) || probability <= 0.0 || probability >= 1.0) return std::nullopt;

  double rawWidth = std::ceil(std::numbers::e / error);
  double rawDepth = std::ceil(std::log(1.0 / probability));
  if (!std::isfinite(rawWidth) || !std::isfinite(rawDepth)) return std::nullopt;
  if (rawWidth < 1.0 || rawWidth > static_cast<double>(kCmsMaxWidth)) return std::nullopt;
  if (rawDepth < 1.0 || rawDepth > static_cast<double>(kCmsMaxDepth)) return std::nullopt;

  return Create(static_cast<uint32_t>(rawWidth), static_cast<uint32_t>(rawDepth));
}

uint32_t CmsSketch::ColumnOf(std::span<const std::byte> item, uint32_t row) const {
  auto* ptr = reinterpret_cast<const void*>(item.data());
  auto len = static_cast<int>(std::min(item.size(), static_cast<size_t>(INT_MAX)));
  uint32_t h = MurmurHash2(ptr, len, kCmsRowSeedBase + row);
  return h % width_;
}

std::optional<uint64_t> CmsSketch::IncrBy(std::span<const std::byte> item, int64_t delta) {
  // Pass 1: validate every row's resulting counter fits in [0, UINT32_MAX]
  // without signed integer overflow UB (even for INT64_MAX / INT64_MIN).
  uint32_t cols[kCmsMaxDepth];
  for (uint32_t row = 0; row < depth_; row++) {
    uint32_t col = ColumnOf(item, row);
    cols[row] = col;
    uint32_t cur = CounterAt(row, col);
    if (delta >= 0) {
      uint64_t inc = static_cast<uint64_t>(delta);
      if (inc > static_cast<uint64_t>(UINT32_MAX) - cur) return std::nullopt;
    } else {
      uint64_t dec = static_cast<uint64_t>(-(delta + 1)) + 1ULL;
      if (dec > cur) return std::nullopt;
    }
  }

  uint64_t minCount = UINT64_MAX;
  for (uint32_t row = 0; row < depth_; row++) {
    uint32_t col = cols[row];
    uint32_t cur = CounterAt(row, col);
    uint32_t updated;
    if (delta >= 0) {
      updated = cur + static_cast<uint32_t>(delta);
    } else {
      uint64_t dec = static_cast<uint64_t>(-(delta + 1)) + 1ULL;
      updated = cur - static_cast<uint32_t>(dec);
    }
    CounterAt(row, col) = updated;
    minCount = std::min<uint64_t>(minCount, updated);
  }
  if (delta > 0) {
    uint64_t inc = static_cast<uint64_t>(delta);
    totalCount_ = (inc > UINT64_MAX - totalCount_) ? UINT64_MAX : totalCount_ + inc;
  } else if (delta < 0) {
    uint64_t dec = static_cast<uint64_t>(-(delta + 1)) + 1ULL;
    totalCount_ = (dec > totalCount_) ? 0 : totalCount_ - dec;
  }
  return minCount;
}

bool CmsSketch::IncrByBatch(std::span<const std::span<const std::byte>> items,
                            std::span<const int64_t> deltas,
                            std::span<uint64_t> outResults) {
  size_t n = items.size();
  if (n != deltas.size() || n != outResults.size()) return false;
  if (n == 0) return true;
  if (n == 1) {
    auto res = IncrBy(items[0], deltas[0]);
    if (!res.has_value()) return false;
    outResults[0] = *res;
    return true;
  }

  // Record each applied item's delta so we can roll back in reverse order on
  // failure without allocating a full copy of the counter table.
  uint64_t savedTotal = totalCount_;
  for (size_t i = 0; i < n; i++) {
    auto res = IncrBy(items[i], deltas[i]);
    if (!res.has_value()) {
      for (size_t j = i; j-- > 0;) {
        // Since deltas[j] fit in [-UINT32_MAX, UINT32_MAX] (validated by IncrBy),
        // -deltas[j] is guaranteed not to overflow int64_t.
        (void)IncrBy(items[j], -deltas[j]);
      }
      totalCount_ = savedTotal;
      return false;
    }
    outResults[i] = *res;
  }
  return true;
}

uint64_t CmsSketch::Query(std::span<const std::byte> item) const {
  uint64_t minCount = UINT64_MAX;
  for (uint32_t row = 0; row < depth_; row++) {
    uint32_t col = ColumnOf(item, row);
    minCount = std::min<uint64_t>(minCount, CounterAt(row, col));
  }
  return minCount;
}

bool CmsSketch::Merge(std::span<const CmsSketch* const> sources, std::span<const double> weights) {
  if (sources.size() != weights.size()) return false;
  bool selfAliased = false;
  for (size_t i = 0; i < sources.size(); i++) {
    const auto* src = sources[i];
    if (!src || src->width_ != width_ || src->depth_ != depth_) return false;
    if (!std::isfinite(weights[i]) || weights[i] < 0.0) return false;
    if (src == this) selfAliased = true;
  }

  uint64_t numCounters = static_cast<uint64_t>(width_) * depth_;
  uint32_t* target = counters_;
  if (selfAliased) {
    target = static_cast<uint32_t*>(RMCalloc(static_cast<size_t>(numCounters), sizeof(uint32_t)));
    if (!target) return false;
  } else {
    std::memset(target, 0, numCounters * sizeof(uint32_t));
  }

  uint64_t newTotal = 0;
  constexpr double kMaxU32 = static_cast<double>(UINT32_MAX);
  constexpr double kMaxU64 = 18446744073709551616.0;  // 2^64 (exact in double)

  for (size_t i = 0; i < sources.size(); i++) {
    const auto* src = sources[i];
    double weight = weights[i];
    for (uint64_t idx = 0; idx < numCounters; idx++) {
      double sum = static_cast<double>(target[idx]) + static_cast<double>(src->counters_[idx]) * weight;
      if (!std::isfinite(sum) || sum >= kMaxU32) {
        target[idx] = UINT32_MAX;
      } else if (sum <= 0.0) {
        target[idx] = 0;
      } else {
        target[idx] = static_cast<uint32_t>(sum);
      }
    }
    double weightedTotal = static_cast<double>(src->totalCount_) * weight;
    if (!std::isfinite(weightedTotal) || weightedTotal >= kMaxU64) {
      newTotal = UINT64_MAX;
    } else if (weightedTotal > 0.0) {
      uint64_t add = static_cast<uint64_t>(weightedTotal);
      newTotal = (add > UINT64_MAX - newTotal) ? UINT64_MAX : newTotal + add;
    }
  }

  if (selfAliased) {
    RMFree(counters_);
    counters_ = target;
  }
  totalCount_ = newTotal;
  return true;
}

std::optional<CmsSketch> CmsSketch::Clone() const {
  CmsSketch copy;
  copy.width_ = width_;
  copy.depth_ = depth_;
  copy.totalCount_ = totalCount_;

  uint64_t dataSize = GetDataSize();
  copy.counters_ = static_cast<uint32_t*>(RMAlloc(dataSize));
  if (!copy.counters_) return std::nullopt;
  std::memcpy(copy.counters_, counters_, dataSize);
  return copy;
}
