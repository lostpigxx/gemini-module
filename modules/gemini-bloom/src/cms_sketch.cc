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
  // Pass 1: validate every row's resulting counter fits in uint32_t before
  // mutating anything, so a rejected call leaves the sketch untouched.
  for (uint32_t row = 0; row < depth_; row++) {
    uint32_t col = ColumnOf(item, row);
    int64_t result = static_cast<int64_t>(CounterAt(row, col)) + delta;
    if (result < 0 || result > static_cast<int64_t>(UINT32_MAX)) return std::nullopt;
  }

  uint64_t minCount = UINT64_MAX;
  for (uint32_t row = 0; row < depth_; row++) {
    uint32_t col = ColumnOf(item, row);
    uint32_t updated = static_cast<uint32_t>(static_cast<int64_t>(CounterAt(row, col)) + delta);
    CounterAt(row, col) = updated;
    minCount = std::min<uint64_t>(minCount, updated);
  }
  if (delta > 0) {
    totalCount_ += static_cast<uint64_t>(delta);
  } else if (delta < 0) {
    uint64_t dec = static_cast<uint64_t>(-delta);
    totalCount_ = (dec > totalCount_) ? 0 : totalCount_ - dec;
  }
  return minCount;
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
  for (const auto* src : sources) {
    if (!src || src->width_ != width_ || src->depth_ != depth_) return false;
  }

  uint64_t numCounters = static_cast<uint64_t>(width_) * depth_;
  std::memset(counters_, 0, numCounters * sizeof(uint32_t));
  totalCount_ = 0;

  for (size_t i = 0; i < sources.size(); i++) {
    const auto* src = sources[i];
    double weight = weights[i];
    for (uint64_t idx = 0; idx < numCounters; idx++) {
      double weighted = static_cast<double>(src->counters_[idx]) * weight;
      double sum = static_cast<double>(counters_[idx]) + weighted;
      if (sum < 0.0) sum = 0.0;
      if (sum > static_cast<double>(UINT32_MAX)) sum = static_cast<double>(UINT32_MAX);
      counters_[idx] = static_cast<uint32_t>(sum);
    }
    double weightedTotal = static_cast<double>(src->totalCount_) * weight;
    if (weightedTotal > 0.0) totalCount_ += static_cast<uint64_t>(weightedTotal);
  }
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
