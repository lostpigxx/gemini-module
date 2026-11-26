#include "tdigest_sketch.h"
#include "rm_alloc.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>

namespace {
constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// Dunning-Ertl k1 (arcsin) scale function parameterized so `compression`
// corresponds to ~compression centroids across [0, 1]:
//   k1(q) = (delta / pi) * asin(2q - 1)   in [-delta/2, +delta/2]
//   k1^-1(k) = (sin(pi * k / delta) + 1) / 2
inline double ScaleK1(double q, double delta) {
  double x = 2.0 * q - 1.0;
  if (x <= -1.0) return -delta * 0.5;
  if (x >= 1.0) return delta * 0.5;
  return (delta / std::numbers::pi) * std::asin(x);
}

inline double InvScaleK1(double k, double delta) {
  double lim = delta * 0.5;
  if (k <= -lim) return 0.0;
  if (k >= lim) return 1.0;
  return 0.5 * (std::sin((std::numbers::pi * k) / delta) + 1.0);
}

// Observations are buffered uncompressed until the buffer reaches this many
// entries, at which point Add() triggers an automatic Compress(). Scaled
// with the compression setting so higher-accuracy sketches (which keep more
// centroids) also tolerate a larger buffer between folds.
uint32_t BufferFlushThreshold(double compression) {
  double target = compression * 4.0;
  if (target < 256.0) target = 256.0;
  if (target > 65536.0) target = 65536.0;
  return static_cast<uint32_t>(target);
}
}  // namespace

TdigestSketch::~TdigestSketch() {
  if (centroids_) {
    RMFree(centroids_);
    centroids_ = nullptr;
  }
  if (prefixWeights_) {
    RMFree(prefixWeights_);
    prefixWeights_ = nullptr;
  }
  if (buffer_) {
    RMFree(buffer_);
    buffer_ = nullptr;
  }
}

TdigestSketch::TdigestSketch(TdigestSketch&& other) noexcept
    : compression_(other.compression_),
      centroids_(other.centroids_),
      prefixWeights_(other.prefixWeights_),
      numCentroids_(other.numCentroids_),
      centroidCapacity_(other.centroidCapacity_),
      buffer_(other.buffer_),
      numBuffered_(other.numBuffered_),
      bufferCapacity_(other.bufferCapacity_),
      mergedWeight_(other.mergedWeight_),
      unmergedWeight_(other.unmergedWeight_),
      min_(other.min_),
      max_(other.max_),
      numCompressions_(other.numCompressions_) {
  other.compression_ = 0.0;
  other.centroids_ = nullptr;
  other.prefixWeights_ = nullptr;
  other.numCentroids_ = 0;
  other.centroidCapacity_ = 0;
  other.buffer_ = nullptr;
  other.numBuffered_ = 0;
  other.bufferCapacity_ = 0;
  other.mergedWeight_ = 0.0;
  other.unmergedWeight_ = 0.0;
  other.min_ = 0.0;
  other.max_ = 0.0;
  other.numCompressions_ = 0;
}

TdigestSketch& TdigestSketch::operator=(TdigestSketch&& other) noexcept {
  if (this != &other) {
    if (centroids_) RMFree(centroids_);
    if (prefixWeights_) RMFree(prefixWeights_);
    if (buffer_) RMFree(buffer_);

    compression_ = other.compression_;
    centroids_ = other.centroids_;
    prefixWeights_ = other.prefixWeights_;
    numCentroids_ = other.numCentroids_;
    centroidCapacity_ = other.centroidCapacity_;
    buffer_ = other.buffer_;
    numBuffered_ = other.numBuffered_;
    bufferCapacity_ = other.bufferCapacity_;
    mergedWeight_ = other.mergedWeight_;
    unmergedWeight_ = other.unmergedWeight_;
    min_ = other.min_;
    max_ = other.max_;
    numCompressions_ = other.numCompressions_;

    other.compression_ = 0.0;
    other.centroids_ = nullptr;
    other.prefixWeights_ = nullptr;
    other.numCentroids_ = 0;
    other.centroidCapacity_ = 0;
    other.buffer_ = nullptr;
    other.numBuffered_ = 0;
    other.bufferCapacity_ = 0;
    other.mergedWeight_ = 0.0;
    other.unmergedWeight_ = 0.0;
    other.min_ = 0.0;
    other.max_ = 0.0;
    other.numCompressions_ = 0;
  }
  return *this;
}

std::optional<TdigestSketch> TdigestSketch::Create(double compression) {
  if (!std::isfinite(compression) || compression < kTdigestMinCompression ||
      compression > kTdigestMaxCompression) {
    return std::nullopt;
  }
  TdigestSketch sketch;
  sketch.compression_ = compression;
  if (!sketch.EnsureBufferCapacity(64)) return std::nullopt;
  return sketch;
}

bool TdigestSketch::EnsureBufferCapacity(uint32_t needed) {
  if (needed == 0) return true;
  if (needed <= bufferCapacity_) return true;
  if (needed > kTdigestMaxCentroids) return false;
  uint32_t newCap = bufferCapacity_ == 0 ? 64 : bufferCapacity_;
  while (newCap < needed && newCap <= (kTdigestMaxCentroids >> 1)) newCap *= 2;
  if (newCap < needed) newCap = needed;
  auto* newBuf = static_cast<Centroid*>(RMRealloc(buffer_, sizeof(Centroid) * newCap));
  if (!newBuf) return false;
  buffer_ = newBuf;
  bufferCapacity_ = newCap;
  return true;
}

bool TdigestSketch::EnsureCentroidCapacity(uint32_t needed) {
  if (needed == 0) return true;
  if (needed <= centroidCapacity_) return true;
  if (needed > kTdigestMaxCentroids) return false;
  uint32_t newCap = centroidCapacity_ == 0 ? 64 : centroidCapacity_;
  while (newCap < needed && newCap <= (kTdigestMaxCentroids >> 1)) newCap *= 2;
  if (newCap < needed) newCap = needed;
  auto* newArr = static_cast<Centroid*>(RMRealloc(centroids_, sizeof(Centroid) * newCap));
  if (!newArr) return false;
  centroids_ = newArr;
  auto* newPrefix = static_cast<double*>(RMRealloc(prefixWeights_, sizeof(double) * newCap));
  if (!newPrefix) return false;
  prefixWeights_ = newPrefix;
  centroidCapacity_ = newCap;
  return true;
}

void TdigestSketch::RebuildPrefixWeights() {
  double cum = 0.0;
  for (uint32_t i = 0; i < numCentroids_; i++) {
    double w = centroids_[i].weight;
    prefixWeights_[i] = cum + w * 0.5;
    cum += w;
  }
}

bool TdigestSketch::SetCompression(double compression) {
  if (!std::isfinite(compression) || compression < kTdigestMinCompression ||
      compression > kTdigestMaxCompression) {
    return false;
  }
  compression_ = compression;
  return true;
}

bool TdigestSketch::Add(double value, double weight) {
  if (!std::isfinite(value) || !std::isfinite(weight) || weight <= 0.0) return false;
  if (!EnsureBufferCapacity(numBuffered_ + 1)) return false;

  if (Empty()) {
    min_ = value;
    max_ = value;
  } else {
    min_ = std::min(min_, value);
    max_ = std::max(max_, value);
  }

  buffer_[numBuffered_].mean = value;
  buffer_[numBuffered_].weight = weight;
  numBuffered_++;
  unmergedWeight_ += weight;

  if (numBuffered_ >= BufferFlushThreshold(compression_)) Compress();
  return true;
}

bool TdigestSketch::RebuildFromSorted(const Centroid* points, size_t count, double totalWeight) {
  if (count == 0) {
    numCentroids_ = 0;
    mergedWeight_ = 0.0;
    return true;
  }

  auto* out = static_cast<Centroid*>(RMAlloc(sizeof(Centroid) * count));
  if (!out) return false;

  bool reverseSweep = (numCompressions_ & 1ULL) != 0;
  size_t outCount = 0;
  double cumWeight = 0.0;
  double qLimit = InvScaleK1(ScaleK1(0.0, compression_) + 1.0, compression_);
  double weightLimit = qLimit * totalWeight;

  size_t firstIdx = reverseSweep ? (count - 1) : 0;
  double curMean = points[firstIdx].mean;
  double curWeight = points[firstIdx].weight;

  for (size_t step = 1; step < count; step++) {
    size_t idx = reverseSweep ? (count - 1 - step) : step;
    const Centroid& pt = points[idx];
    double proposedWeight = curWeight + pt.weight;
    if (cumWeight + proposedWeight <= weightLimit || pt.mean == curMean) {
      // Numerically stable incremental weighted mean update.
      curMean += pt.weight * (pt.mean - curMean) / proposedWeight;
      curWeight = proposedWeight;
    } else {
      out[outCount++] = {curMean, curWeight};
      cumWeight += curWeight;
      double q = cumWeight / totalWeight;
      qLimit = InvScaleK1(ScaleK1(q, compression_) + 1.0, compression_);
      weightLimit = qLimit * totalWeight;
      curMean = pt.mean;
      curWeight = pt.weight;
    }
  }
  out[outCount++] = {curMean, curWeight};

  if (reverseSweep && outCount > 1) {
    std::reverse(out, out + outCount);
  }

  if (outCount > kTdigestMaxCentroids || !EnsureCentroidCapacity(static_cast<uint32_t>(outCount))) {
    RMFree(out);
    return false;
  }
  std::memcpy(centroids_, out, outCount * sizeof(Centroid));
  RMFree(out);
  numCentroids_ = static_cast<uint32_t>(outCount);
  mergedWeight_ = totalWeight;
  RebuildPrefixWeights();
  return true;
}

void TdigestSketch::Compress() {
  if (numBuffered_ == 0) return;

  double totalWeight = mergedWeight_ + unmergedWeight_;
  size_t totalCount = static_cast<size_t>(numCentroids_) + numBuffered_;

  // Sort only the newly buffered observations, then merge with already-sorted centroids_.
  std::sort(buffer_, buffer_ + numBuffered_,
            [](const Centroid& a, const Centroid& b) { return a.mean < b.mean; });

  auto* merged = static_cast<Centroid*>(RMAlloc(sizeof(Centroid) * totalCount));
  if (!merged) return;

  std::merge(centroids_, centroids_ + numCentroids_,
             buffer_, buffer_ + numBuffered_,
             merged,
             [](const Centroid& a, const Centroid& b) { return a.mean < b.mean; });

  bool ok = RebuildFromSorted(merged, totalCount, totalWeight);
  RMFree(merged);
  if (!ok) return;

  numBuffered_ = 0;
  unmergedWeight_ = 0.0;
  numCompressions_++;
}

bool TdigestSketch::Merge(std::span<const TdigestSketch* const> sources) {
  size_t total = static_cast<size_t>(numCentroids_) + numBuffered_;
  for (auto* s : sources) total += static_cast<size_t>(s->numCentroids_) + s->numBuffered_;

  if (total == 0) {
    numCentroids_ = 0;
    numBuffered_ = 0;
    mergedWeight_ = 0.0;
    unmergedWeight_ = 0.0;
    return true;
  }

  auto* points = static_cast<Centroid*>(RMAlloc(sizeof(Centroid) * total));
  if (!points) return false;

  size_t pos = 0;
  if (numCentroids_ > 0) {
    std::memcpy(points + pos, centroids_, numCentroids_ * sizeof(Centroid));
    pos += numCentroids_;
  }
  if (numBuffered_ > 0) {
    std::memcpy(points + pos, buffer_, numBuffered_ * sizeof(Centroid));
    pos += numBuffered_;
  }

  double totalWeight = mergedWeight_ + unmergedWeight_;
  bool haveBound = !Empty();
  double newMin = min_, newMax = max_;

  for (auto* s : sources) {
    if (s->numCentroids_ > 0) {
      std::memcpy(points + pos, s->centroids_, s->numCentroids_ * sizeof(Centroid));
      pos += s->numCentroids_;
    }
    if (s->numBuffered_ > 0) {
      std::memcpy(points + pos, s->buffer_, s->numBuffered_ * sizeof(Centroid));
      pos += s->numBuffered_;
    }
    totalWeight += s->mergedWeight_ + s->unmergedWeight_;
    if (!s->Empty()) {
      if (!haveBound) {
        newMin = s->min_;
        newMax = s->max_;
        haveBound = true;
      } else {
        newMin = std::min(newMin, s->min_);
        newMax = std::max(newMax, s->max_);
      }
    }
  }

  std::sort(points, points + pos,
            [](const Centroid& a, const Centroid& b) { return a.mean < b.mean; });

  bool ok = RebuildFromSorted(points, pos, totalWeight);
  RMFree(points);
  if (!ok) return false;

  numBuffered_ = 0;
  unmergedWeight_ = 0.0;
  min_ = newMin;
  max_ = newMax;
  numCompressions_++;
  return true;
}

double TdigestSketch::ValueAtRank(double rank) const {
  if (numCentroids_ == 0) return kNan;
  if (numCentroids_ == 1) return centroids_[0].mean;
  if (rank <= 0.0) return min_;
  if (rank >= mergedWeight_) return max_;

  // Interpolate between (0, min_) and first centroid midpoint when in left tail.
  if (rank < prefixWeights_[0]) {
    if (centroids_[0].weight <= 1.0 || prefixWeights_[0] <= 0.0) return min_;
    double t = rank / prefixWeights_[0];
    return min_ + t * (centroids_[0].mean - min_);
  }

  // Interpolate between last centroid midpoint and (mergedWeight_, max_) when in right tail.
  uint32_t last = numCentroids_ - 1;
  if (rank > prefixWeights_[last]) {
    if (centroids_[last].weight <= 1.0) return max_;
    double denom = mergedWeight_ - prefixWeights_[last];
    if (denom <= 0.0) return max_;
    double t = (rank - prefixWeights_[last]) / denom;
    return centroids_[last].mean + t * (max_ - centroids_[last].mean);
  }

  const double* it = std::lower_bound(prefixWeights_, prefixWeights_ + numCentroids_, rank);
  size_t i = static_cast<size_t>(it - prefixWeights_);
  if (i == 0) return centroids_[0].mean;
  if (i >= numCentroids_) return centroids_[last].mean;

  double aRank = prefixWeights_[i - 1];
  double bRank = prefixWeights_[i];
  double aVal = centroids_[i - 1].mean;
  double bVal = centroids_[i].mean;
  double denom = bRank - aRank;
  if (denom <= 0.0) return aVal;
  double t = (rank - aRank) / denom;
  return aVal + t * (bVal - aVal);
}

double TdigestSketch::RankReal(double value) const {
  if (numCentroids_ == 0) return 0.0;

  auto CumBefore = [this](uint32_t idx) -> double {
    if (idx == 0) return 0.0;
    if (idx >= numCentroids_) return mergedWeight_;
    return prefixWeights_[idx] - centroids_[idx].weight * 0.5;
  };

  const Centroid* begin = centroids_;
  const Centroid* end = centroids_ + numCentroids_;
  const Centroid* firstGe = std::lower_bound(
    begin, end, value,
    [](const Centroid& c, double v) { return c.mean < v; });
  const Centroid* firstGt = std::upper_bound(
    firstGe, end, value,
    [](double v, const Centroid& c) { return v < c.mean; });

  uint32_t lo = static_cast<uint32_t>(firstGe - centroids_);
  uint32_t hi = static_cast<uint32_t>(firstGt - centroids_);
  double less = CumBefore(lo);
  double equal = CumBefore(hi) - less;
  return less + equal * 0.5;
}

double TdigestSketch::Quantile(double q) {
  Compress();
  if (Empty()) return kNan;
  if (!std::isfinite(q)) return kNan;
  if (q <= 0.0) return min_;
  if (q >= 1.0) return max_;

  return ValueAtRank(q * TotalWeight());
}

double TdigestSketch::Cdf(double value) {
  Compress();
  if (Empty()) return kNan;
  if (value < min_) return 0.0;
  if (value > max_) return 1.0;
  return RankReal(value) / TotalWeight();
}

namespace {
int64_t ClampFloorToInt64(double v) {
  if (!std::isfinite(v) || v <= 0.0) return 0;
  constexpr double kMaxI64 = 9223372036854775808.0;  // 2^63 (exact in double)
  double f = std::floor(v);
  if (f >= kMaxI64) return INT64_MAX;
  return static_cast<int64_t>(f);
}
}  // namespace

int64_t TdigestSketch::Rank(double value) {
  Compress();
  if (Empty()) return -2;
  if (value < min_) return -1;
  if (value > max_) return ClampFloorToInt64(TotalWeight());
  return ClampFloorToInt64(RankReal(value));
}

int64_t TdigestSketch::RevRank(double value) {
  Compress();
  if (Empty()) return -2;
  if (value > max_) return -1;
  if (value < min_) return ClampFloorToInt64(TotalWeight());
  return ClampFloorToInt64(TotalWeight() - RankReal(value));
}

double TdigestSketch::ByRank(double rank) {
  Compress();
  if (Empty()) return kNan;
  double n = TotalWeight();
  if (rank <= 0.0) return min_;
  if (rank >= n) return kInf;
  if (rank >= n - 1.0) return max_;

  return ValueAtRank(rank);
}

double TdigestSketch::ByRevRank(double revRank) {
  Compress();
  if (Empty()) return kNan;
  double n = TotalWeight();
  if (revRank <= 0.0) return max_;
  if (revRank >= n) return -kInf;
  if (revRank >= n - 1.0) return min_;

  return ValueAtRank(n - revRank);
}

double TdigestSketch::TrimmedMean(double lowQ, double highQ) {
  Compress();
  if (Empty()) return kNan;
  if (!std::isfinite(lowQ) || !std::isfinite(highQ) || lowQ < 0.0 || highQ > 1.0 || lowQ >= highQ) {
    return kNan;
  }

  double n = TotalWeight();
  double lo = lowQ * n;
  double hi = highQ * n;
  double sumW = 0.0, sumWMean = 0.0;
  double cum = 0.0;
  for (uint32_t i = 0; i < numCentroids_; i++) {
    double w = centroids_[i].weight;
    double segLo = cum, segHi = cum + w;
    double overlap = std::min(segHi, hi) - std::max(segLo, lo);
    if (overlap > 0.0) {
      sumW += overlap;
      sumWMean += overlap * centroids_[i].mean;
    }
    cum += w;
  }
  if (sumW <= 0.0) return kNan;
  return sumWMean / sumW;
}

double TdigestSketch::Min() const { return Empty() ? kNan : min_; }
double TdigestSketch::Max() const { return Empty() ? kNan : max_; }

void TdigestSketch::Reset() {
  numCentroids_ = 0;
  numBuffered_ = 0;
  mergedWeight_ = 0.0;
  unmergedWeight_ = 0.0;
  min_ = 0.0;
  max_ = 0.0;
  numCompressions_ = 0;
}

std::optional<TdigestSketch> TdigestSketch::Clone() const {
  TdigestSketch copy;
  copy.compression_ = compression_;

  if (!copy.EnsureCentroidCapacity(numCentroids_)) return std::nullopt;
  if (numCentroids_ > 0) {
    std::memcpy(copy.centroids_, centroids_, numCentroids_ * sizeof(Centroid));
    std::memcpy(copy.prefixWeights_, prefixWeights_, numCentroids_ * sizeof(double));
  }
  copy.numCentroids_ = numCentroids_;

  if (!copy.EnsureBufferCapacity(numBuffered_)) return std::nullopt;
  if (numBuffered_ > 0) {
    std::memcpy(copy.buffer_, buffer_, numBuffered_ * sizeof(Centroid));
  }
  copy.numBuffered_ = numBuffered_;

  copy.mergedWeight_ = mergedWeight_;
  copy.unmergedWeight_ = unmergedWeight_;
  copy.min_ = min_;
  copy.max_ = max_;
  copy.numCompressions_ = numCompressions_;
  return copy;
}

bool TdigestSketch::AllocateForLoad(uint32_t numCentroids, uint32_t numBuffered) {
  if (numCentroids > kTdigestMaxCentroids || numBuffered > kTdigestMaxCentroids) return false;
  if (!EnsureCentroidCapacity(numCentroids)) return false;
  if (!EnsureBufferCapacity(numBuffered)) return false;
  numCentroids_ = 0;
  numBuffered_ = 0;
  return true;
}

void TdigestSketch::AppendCentroidForLoad(double mean, double weight) {
  centroids_[numCentroids_].mean = mean;
  centroids_[numCentroids_].weight = weight;
  numCentroids_++;
}

void TdigestSketch::AppendBufferedForLoad(double mean, double weight) {
  buffer_[numBuffered_].mean = mean;
  buffer_[numBuffered_].weight = weight;
  numBuffered_++;
}

void TdigestSketch::SetBookkeepingForLoad(double mergedWeight, double unmergedWeight, double rawMin,
                                           double rawMax, uint64_t numCompressions) {
  mergedWeight_ = mergedWeight;
  unmergedWeight_ = unmergedWeight;
  min_ = rawMin;
  max_ = rawMax;
  numCompressions_ = numCompressions;
  RebuildPrefixWeights();
}
