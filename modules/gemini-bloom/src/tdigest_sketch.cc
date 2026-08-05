#include "tdigest_sketch.h"
#include "rm_alloc.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

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
  if (buffer_) {
    RMFree(buffer_);
    buffer_ = nullptr;
  }
}

TdigestSketch::TdigestSketch(TdigestSketch&& other) noexcept
    : compression_(other.compression_),
      centroids_(other.centroids_),
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
    if (buffer_) RMFree(buffer_);

    compression_ = other.compression_;
    centroids_ = other.centroids_;
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
  centroidCapacity_ = newCap;
  return true;
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

bool TdigestSketch::RebuildFromPoints(std::vector<Centroid> points, double totalWeight) {
  std::sort(points.begin(), points.end(),
            [](const Centroid& a, const Centroid& b) { return a.mean < b.mean; });

  std::vector<Centroid> buckets;
  buckets.reserve(points.size());
  double targetWeight = compression_ > 0.0 ? totalWeight / compression_ : totalWeight;

  size_t i = 0;
  while (i < points.size()) {
    double sumW = points[i].weight;
    double sumWMean = points[i].weight * points[i].mean;
    size_t j = i + 1;
    while (j < points.size() && sumW + points[j].weight <= targetWeight) {
      sumW += points[j].weight;
      sumWMean += points[j].weight * points[j].mean;
      j++;
    }
    buckets.push_back({sumWMean / sumW, sumW});
    i = j;
  }

  if (!EnsureCentroidCapacity(static_cast<uint32_t>(buckets.size()))) return false;
  if (!buckets.empty()) {
    std::memcpy(centroids_, buckets.data(), buckets.size() * sizeof(Centroid));
  }
  numCentroids_ = static_cast<uint32_t>(buckets.size());
  mergedWeight_ = totalWeight;
  return true;
}

void TdigestSketch::Compress() {
  if (numBuffered_ == 0) return;

  double totalWeight = mergedWeight_ + unmergedWeight_;
  std::vector<Centroid> points;
  points.reserve(static_cast<size_t>(numCentroids_) + numBuffered_);
  for (uint32_t i = 0; i < numCentroids_; i++) points.push_back(centroids_[i]);
  for (uint32_t i = 0; i < numBuffered_; i++) points.push_back(buffer_[i]);

  if (!RebuildFromPoints(std::move(points), totalWeight)) return;

  numBuffered_ = 0;
  unmergedWeight_ = 0.0;
  numCompressions_++;
}

bool TdigestSketch::Merge(std::span<const TdigestSketch* const> sources) {
  size_t total = static_cast<size_t>(numCentroids_) + numBuffered_;
  for (auto* s : sources) total += static_cast<size_t>(s->numCentroids_) + s->numBuffered_;

  std::vector<Centroid> points;
  points.reserve(total);
  for (uint32_t i = 0; i < numCentroids_; i++) points.push_back(centroids_[i]);
  for (uint32_t i = 0; i < numBuffered_; i++) points.push_back(buffer_[i]);

  double totalWeight = mergedWeight_ + unmergedWeight_;
  bool haveBound = !Empty();
  double newMin = min_, newMax = max_;

  for (auto* s : sources) {
    for (uint32_t i = 0; i < s->numCentroids_; i++) points.push_back(s->centroids_[i]);
    for (uint32_t i = 0; i < s->numBuffered_; i++) points.push_back(s->buffer_[i]);
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

  if (points.empty()) {
    numCentroids_ = 0;
    numBuffered_ = 0;
    mergedWeight_ = 0.0;
    unmergedWeight_ = 0.0;
    return true;
  }

  if (!RebuildFromPoints(std::move(points), totalWeight)) return false;

  numBuffered_ = 0;
  unmergedWeight_ = 0.0;
  min_ = newMin;
  max_ = newMax;
  numCompressions_++;
  return true;
}

std::vector<TdigestSketch::Anchor> TdigestSketch::BuildAnchors() const {
  std::vector<Anchor> anchors;
  anchors.reserve(numCentroids_);
  double cum = 0.0;
  for (uint32_t i = 0; i < numCentroids_; i++) {
    double w = centroids_[i].weight;
    anchors.push_back({cum + w / 2.0, centroids_[i].mean});
    cum += w;
  }
  if (!anchors.empty()) {
    anchors.front().value = min_;
    anchors.back().value = max_;
  }
  return anchors;
}

double TdigestSketch::ValueAtRank(double rank, const std::vector<Anchor>& anchors) {
  if (anchors.empty()) return kNan;
  if (anchors.size() == 1) return anchors[0].value;
  if (rank <= anchors.front().rank) return anchors.front().value;
  if (rank >= anchors.back().rank) return anchors.back().value;

  for (size_t i = 1; i < anchors.size(); i++) {
    if (rank <= anchors[i].rank) {
      const Anchor& a = anchors[i - 1];
      const Anchor& b = anchors[i];
      double t = (rank - a.rank) / (b.rank - a.rank);
      return a.value + t * (b.value - a.value);
    }
  }
  return anchors.back().value;
}

double TdigestSketch::RankReal(double value) const {
  double less = 0.0, equal = 0.0;
  for (uint32_t i = 0; i < numCentroids_; i++) {
    if (centroids_[i].mean < value) {
      less += centroids_[i].weight;
    } else if (centroids_[i].mean == value) {
      equal += centroids_[i].weight;
    }
  }
  return less + equal / 2.0;
}

double TdigestSketch::Quantile(double q) {
  Compress();
  if (Empty()) return kNan;
  if (!std::isfinite(q)) return kNan;
  if (q <= 0.0) return min_;
  if (q >= 1.0) return max_;

  auto anchors = BuildAnchors();
  return ValueAtRank(q * TotalWeight(), anchors);
}

double TdigestSketch::Cdf(double value) {
  Compress();
  if (Empty()) return kNan;
  if (value < min_) return 0.0;
  if (value > max_) return 1.0;
  return RankReal(value) / TotalWeight();
}

int64_t TdigestSketch::Rank(double value) {
  Compress();
  if (Empty()) return -2;
  if (value < min_) return -1;
  if (value > max_) return static_cast<int64_t>(std::floor(TotalWeight()));
  return static_cast<int64_t>(std::floor(RankReal(value)));
}

int64_t TdigestSketch::RevRank(double value) {
  Compress();
  if (Empty()) return -2;
  if (value > max_) return -1;
  if (value < min_) return static_cast<int64_t>(std::floor(TotalWeight()));
  return static_cast<int64_t>(std::floor(TotalWeight() - RankReal(value)));
}

double TdigestSketch::ByRank(double rank) {
  Compress();
  if (Empty()) return kNan;
  double n = TotalWeight();
  if (rank <= 0.0) return min_;
  if (rank >= n) return kInf;
  if (rank >= n - 1.0) return max_;

  auto anchors = BuildAnchors();
  return ValueAtRank(rank, anchors);
}

double TdigestSketch::ByRevRank(double revRank) {
  Compress();
  if (Empty()) return kNan;
  double n = TotalWeight();
  if (revRank <= 0.0) return max_;
  if (revRank >= n) return -kInf;
  if (revRank >= n - 1.0) return min_;

  auto anchors = BuildAnchors();
  return ValueAtRank(n - revRank, anchors);
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
}
