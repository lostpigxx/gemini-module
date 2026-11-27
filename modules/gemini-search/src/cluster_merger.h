#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace gemini_search {

enum class MergeSortMode : uint8_t {
  kDocIdAsc = 0,
  kScoreDesc = 1,
  kDistanceAsc = 2,
  kSortNumAsc = 3,
  kSortNumDesc = 4,
  kSortStrAsc = 5,
  kSortStrDesc = 6,
};

struct MergeCandidate {
  std::string doc_id;
  uint32_t shard_idx = 0;
  double score = 0.0;     // BM25 score, KNN distance, or VECTOR_RANGE yield distance
  double sort_num = 0.0;  // Numeric sort key (when sorting numerically)
  std::string sort_str;   // Raw sort key string (used for string sort and WITHSORTKEYS)
  bool has_yield_dist = false;
};

struct ShardPhase1Slice {
  uint32_t shard_idx = 0;
  uint64_t total_hits = 0;
  std::vector<MergeCandidate> items;  // Pre-sorted locally using MergeSortMode
};

struct MergedSearchResult {
  uint64_t global_total = 0;
  uint64_t duplicate_count = 0;
  std::vector<MergeCandidate> winners;
  // Maps shard_idx -> indices into `winners` for Phase 2 targeted Fetch
  std::unordered_map<uint32_t, std::vector<size_t>> winners_by_shard;
};

// Returns true iff `a` strictly precedes `b` under `mode` (with deterministic doc_id/shard tie-break).
bool CompareMergeCandidates(const MergeCandidate& a, const MergeCandidate& b, MergeSortMode mode);

// M-way heap merge across shard Phase 1 slices with deterministic doc_id deduplication,
// optional KNN top-K cap, LIMIT [offset, offset + count) slicing, and max_results cap.
MergedSearchResult MergeSearchShards(const std::vector<ShardPhase1Slice>& shards,
                                     MergeSortMode mode, size_t offset, size_t count,
                                     bool has_limit, size_t max_results, size_t knn_cap = 0);

}  // namespace gemini_search
