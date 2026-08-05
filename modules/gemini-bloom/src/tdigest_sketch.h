#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// --- T-Digest: streaming quantile approximation ---
//
// Reference: Ted Dunning, Otmar Ertl, "Computing Extremely Accurate Quantiles
// Using t-Digests" (2019), and the public TDIGEST.* command documentation on
// redis.io. This implementation is an independent, clean-room design derived
// only from the paper's publicly described centroid concept and the public
// command docs; no RedisBloom/t-digest-c source was consulted.
//
// Simplification versus the paper's scale-function-based clustering: new
// observations are buffered uncompressed, then folded into a sorted centroid
// list using a flat target bucket weight of (totalWeight / compression)
// rather than the paper's arcsin scale function. This trades some accuracy
// at the tails for a much simpler, independently designed algorithm.
//
// Rank/CDF/RevRank are computed directly from the centroid list as
// (sum of weights of centroids with mean < value) + half (sum of weights of
// centroids with mean == value) -- an exact, well-defined function of the
// current centroids, matching the "smaller + half equal" definition in the
// public command docs. Quantile/ByRank/ByRevRank/TrimmedMean instead treat
// each centroid as occupying a contiguous span of rank-space proportional to
// its weight and interpolate piecewise-linearly between centroid centers,
// with the exact min/max observation (tracked separately from the
// centroids, so they are never blurred by compression) substituted at the
// rank-space boundaries.

constexpr double kTdigestMinCompression = 1.0;
constexpr double kTdigestMaxCompression = 1'000'000.0;
constexpr double kTdigestDefaultCompression = 100.0;
// Safety cap on centroid/buffer array sizes (~64 MiB per array at 16 bytes/centroid).
constexpr uint32_t kTdigestMaxCentroids = 1u << 22;

class TdigestSketch {
public:
  struct Centroid {
    double mean = 0.0;
    double weight = 0.0;
  };

  TdigestSketch() = default;
  ~TdigestSketch();

  TdigestSketch(const TdigestSketch&) = delete;
  TdigestSketch& operator=(const TdigestSketch&) = delete;

  TdigestSketch(TdigestSketch&& other) noexcept;
  TdigestSketch& operator=(TdigestSketch&& other) noexcept;

  // compression must be in [kTdigestMinCompression, kTdigestMaxCompression].
  static std::optional<TdigestSketch> Create(double compression);

  // Buffers one observation. weight must be finite and > 0. Automatically
  // triggers Compress() when the buffer fills up. Returns false on invalid
  // input or allocation failure (sketch left unmodified).
  bool Add(double value, double weight = 1.0);

  // Folds all buffered observations into the sorted centroid list. No-op if
  // the buffer is already empty.
  void Compress();

  // Merges the full (centroid + buffered) contents of every source into
  // *this*, then compresses using this sketch's own compression setting.
  // Does not modify the sources. Returns false on allocation failure.
  bool Merge(std::span<const TdigestSketch* const> sources);

  double Quantile(double q);
  double Cdf(double value);
  // -2 if empty; -1 if value < min; total observation count if value > max;
  // otherwise floor(observations smaller than value + half observations
  // equal to value).
  int64_t Rank(double value);
  // -2 if empty; -1 if value > max; total observation count if value < min;
  // otherwise floor(observations larger than value + half observations
  // equal to value).
  int64_t RevRank(double value);
  // nan if empty; min at rank<=0; max at rank>=n-1; inf if rank>=n.
  double ByRank(double rank);
  // nan if empty; max at revrank<=0; min at revrank>=n-1; -inf if revrank>=n.
  double ByRevRank(double revRank);
  // Mean of observations whose order-statistic position falls within
  // [lowQ*n, highQ*n). nan if empty or if no observations qualify.
  double TrimmedMean(double lowQ, double highQ);
  double Min() const;
  double Max() const;

  void Reset();

  std::optional<TdigestSketch> Clone() const;

  // Overrides the compression setting used by future Compress() calls
  // (does not retroactively re-bucket existing centroids). Used by
  // TDIGEST.MERGE's no-OVERRIDE-but-COMPRESSION-given case. Returns false
  // (no change) if compression is out of range.
  bool SetCompression(double compression);

  double Compression() const { return compression_; }
  uint32_t NumCentroids() const { return numCentroids_; }
  uint32_t NumBuffered() const { return numBuffered_; }
  double MergedWeight() const { return mergedWeight_; }
  double UnmergedWeight() const { return unmergedWeight_; }
  uint64_t NumCompressions() const { return numCompressions_; }
  double TotalWeight() const { return mergedWeight_ + unmergedWeight_; }
  bool Empty() const { return TotalWeight() == 0.0; }

  const Centroid* GetCentroidArray() const { return centroids_; }
  Centroid* GetCentroidArray() { return centroids_; }
  const Centroid* GetBufferArray() const { return buffer_; }
  Centroid* GetBufferArray() { return buffer_; }
  uint32_t BufferCapacity() const { return bufferCapacity_; }
  uint32_t CentroidCapacity() const { return centroidCapacity_; }

  // For defrag: adopt a relocated buffer. Caller owns the relocation (e.g.
  // via RedisModule_DefragAlloc) and transfers ownership.
  void AdoptCentroidArray(Centroid* relocated) { centroids_ = relocated; }
  void AdoptBufferArray(Centroid* relocated) { buffer_ = relocated; }

  // For RDB/wire load: reconstruct internal state directly from
  // already-validated data. `numCentroids`/`numBuffered` sizes and the
  // append counts must match; validation of value ranges happens at the RDB
  // layer before these are called.
  bool AllocateForLoad(uint32_t numCentroids, uint32_t numBuffered);
  void AppendCentroidForLoad(double mean, double weight);
  void AppendBufferedForLoad(double mean, double weight);
  void SetBookkeepingForLoad(double mergedWeight, double unmergedWeight, double rawMin, double rawMax,
                              uint64_t numCompressions);

private:
  struct Anchor {
    double rank;
    double value;
  };

  // Groups `points` (arbitrary order) into a new sorted centroid array using
  // this sketch's compression setting, replacing centroids_/buffer_.
  // `totalWeight` must equal the sum of all points' weights. Returns false
  // on allocation failure (sketch left in a valid but unspecified state).
  bool RebuildFromPoints(std::vector<Centroid> points, double totalWeight);

  // Grows buffer_/centroids_ (via RMRealloc) so capacity >= needed. Returns
  // false on allocation failure (array left unmodified).
  bool EnsureBufferCapacity(uint32_t needed);
  bool EnsureCentroidCapacity(uint32_t needed);

  // Rank-space anchors for ByRank/ByRevRank/Quantile/TrimmedMean: one point
  // per centroid at (cumulative weight before it + half its own weight,
  // its mean), with the first and last points' values overridden to the
  // exact tracked min_/max_.
  std::vector<Anchor> BuildAnchors() const;
  static double ValueAtRank(double rank, const std::vector<Anchor>& anchors);

  // (sum of weights of centroids with mean < value) + half (sum of weights
  // of centroids with mean == value). Direct O(numCentroids_) scan.
  double RankReal(double value) const;

  double compression_ = 0.0;
  Centroid* centroids_ = nullptr;
  uint32_t numCentroids_ = 0;
  uint32_t centroidCapacity_ = 0;
  Centroid* buffer_ = nullptr;
  uint32_t numBuffered_ = 0;
  uint32_t bufferCapacity_ = 0;
  double mergedWeight_ = 0.0;
  double unmergedWeight_ = 0.0;
  double min_ = 0.0;
  double max_ = 0.0;
  uint64_t numCompressions_ = 0;
};
