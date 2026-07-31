#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// --- Cuckoo filter: a single fixed-size bucket-array sub-filter ---
//
// Reference: Fan, Andersen, Kaminsky, Mitzenmacher, "Cuckoo Filter:
// Practically Better Than Bloom" (CoNEXT 2014). This implementation is an
// independent, clean-room design derived only from the paper's publicly
// described algorithm (partial-key cuckoo hashing) and the public CF.*
// command documentation on redis.io; no RediSearch/RedisBloom source was
// consulted.
//
// Each item is represented by a single-byte fingerprint stored in one of
// `bucketSize_` slots within one of two candidate buckets. A slot value of
// 0 means "empty" — the fingerprint hash is forced away from 0 so 0 never
// collides with a real fingerprint.

constexpr uint64_t kCfMaxBuckets = 1ULL << 30;
constexpr uint32_t kCfMaxBucketSize = 255;
constexpr uint32_t kCfMaxIterations = 65535;

class CuckooFilter {
public:
  CuckooFilter() = default;
  ~CuckooFilter();

  CuckooFilter(const CuckooFilter&) = delete;
  CuckooFilter& operator=(const CuckooFilter&) = delete;

  CuckooFilter(CuckooFilter&& other) noexcept;
  CuckooFilter& operator=(CuckooFilter&& other) noexcept;

  // numBuckets is rounded up to the next power of two internally by the
  // caller (cf_chain.cc); Create() requires an already-power-of-two value.
  static std::optional<CuckooFilter> Create(uint64_t numBuckets, uint32_t bucketSize,
                                             uint32_t maxIterations);

  // Tri-state result mirrors ScalingBloomFilter::Put: a value means the
  // insert completed (always true for cuckoo — duplicates are stored as
  // separate fingerprints); no value means this sub-filter is full (the
  // kick loop exhausted maxIterations_ without finding a free slot).
  std::optional<bool> Insert(std::span<const std::byte> item);
  bool Contains(std::span<const std::byte> item) const;
  bool Delete(std::span<const std::byte> item);
  uint64_t Count(std::span<const std::byte> item) const;

  std::optional<CuckooFilter> Clone() const;

  // For defrag: adopt a relocated bucket-array buffer. Caller owns the
  // relocation (e.g. via RedisModule_DefragAlloc) and transfers ownership.
  void AdoptBucketArray(uint8_t* relocated) { buckets_ = relocated; }

  // For RDB/wire deserialization: after populating the bucket array directly
  // (bypassing Insert()), recompute numItems_ by scanning for non-empty
  // slots so later Delete() calls don't underflow an uninitialized counter.
  void RecountItems();

  uint64_t NumBuckets() const { return numBuckets_; }
  uint32_t BucketSize() const { return bucketSize_; }
  uint32_t MaxIterations() const { return maxIterations_; }
  uint64_t NumItems() const { return numItems_; }
  const uint8_t* GetBucketArray() const { return buckets_; }
  uint8_t* GetBucketArray() { return buckets_; }
  uint64_t GetDataSize() const { return numBuckets_ * static_cast<uint64_t>(bucketSize_); }

private:
  uint8_t* SlotsOf(uint64_t bucketIndex) { return buckets_ + bucketIndex * bucketSize_; }
  const uint8_t* SlotsOf(uint64_t bucketIndex) const { return buckets_ + bucketIndex * bucketSize_; }

  // Returns the slot index within the bucket holding `fp`, or -1 if absent.
  int FindSlot(uint64_t bucketIndex, uint8_t fp) const;
  // Returns the slot index of the first empty (zero) slot, or -1 if full.
  int FindEmptySlot(uint64_t bucketIndex) const;

  uint64_t numBuckets_ = 0;
  uint32_t bucketSize_ = 0;
  uint32_t maxIterations_ = 0;
  uint64_t numItems_ = 0;
  uint8_t* buckets_ = nullptr;
};

// --- Fingerprint / candidate-bucket derivation ---
// Exposed for unit testing of the partial-key symmetry property.

uint8_t CfFingerprint(std::span<const std::byte> item);
uint64_t CfIndexOf(std::span<const std::byte> item, uint64_t numBuckets);
uint64_t CfAltIndex(uint64_t index, uint8_t fp, uint64_t numBuckets);
