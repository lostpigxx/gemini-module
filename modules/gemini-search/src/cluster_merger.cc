#include "cluster_merger.h"

#include <algorithm>
#include <queue>
#include <string_view>
#include <unordered_set>

namespace gemini_search {

bool CompareMergeCandidates(const MergeCandidate& a, const MergeCandidate& b, MergeSortMode mode) {
  switch (mode) {
    case MergeSortMode::kDocIdAsc:
      if (a.doc_id != b.doc_id) return a.doc_id < b.doc_id;
      return a.shard_idx < b.shard_idx;
    case MergeSortMode::kScoreDesc:
      if (a.score != b.score) return a.score > b.score;
      if (a.doc_id != b.doc_id) return a.doc_id < b.doc_id;
      return a.shard_idx < b.shard_idx;
    case MergeSortMode::kDistanceAsc:
      if (a.score != b.score) return a.score < b.score;
      if (a.doc_id != b.doc_id) return a.doc_id < b.doc_id;
      return a.shard_idx < b.shard_idx;
    case MergeSortMode::kSortNumAsc:
      if (a.sort_num != b.sort_num) return a.sort_num < b.sort_num;
      if (a.doc_id != b.doc_id) return a.doc_id < b.doc_id;
      return a.shard_idx < b.shard_idx;
    case MergeSortMode::kSortNumDesc:
      if (a.sort_num != b.sort_num) return a.sort_num > b.sort_num;
      if (a.doc_id != b.doc_id) return a.doc_id < b.doc_id;
      return a.shard_idx < b.shard_idx;
    case MergeSortMode::kSortStrAsc:
      if (a.sort_str != b.sort_str) return a.sort_str < b.sort_str;
      if (a.doc_id != b.doc_id) return a.doc_id < b.doc_id;
      return a.shard_idx < b.shard_idx;
    case MergeSortMode::kSortStrDesc:
      if (a.sort_str != b.sort_str) return a.sort_str > b.sort_str;
      if (a.doc_id != b.doc_id) return a.doc_id < b.doc_id;
      return a.shard_idx < b.shard_idx;
  }
  return a.doc_id < b.doc_id;
}

MergedSearchResult MergeSearchShards(const std::vector<ShardPhase1Slice>& shards,
                                     MergeSortMode mode, size_t offset, size_t count,
                                     bool has_limit, size_t max_results, size_t knn_cap) {
  MergedSearchResult result;
  uint64_t raw_total = 0;
  for (const auto& s : shards) {
    raw_total += s.total_hits;
  }

  // Fast-path: pure count query (LIMIT 0 0) with no items requested
  if (has_limit && offset == 0 && count == 0 && knn_cap == 0) {
    // Still deduplicate any returned items if shards sent them (normally items are empty)
    std::unordered_set<std::string_view> seen;
    for (const auto& s : shards) {
      for (const auto& item : s.items) {
        if (!seen.insert(item.doc_id).second) {
          result.duplicate_count++;
        }
      }
    }
    result.global_total =
        (raw_total >= result.duplicate_count) ? (raw_total - result.duplicate_count) : 0;
    return result;
  }

  struct HeapNode {
    size_t slice_pos = 0;
    size_t item_pos = 0;
  };

  auto heap_cmp = [&](const HeapNode& lhs, const HeapNode& rhs) {
    const auto& a = shards[lhs.slice_pos].items[lhs.item_pos];
    const auto& b = shards[rhs.slice_pos].items[rhs.item_pos];
    // std::priority_queue puts the largest element on top, so invert CompareMergeCandidates
    return CompareMergeCandidates(b, a, mode);
  };

  std::priority_queue<HeapNode, std::vector<HeapNode>, decltype(heap_cmp)> pq(heap_cmp);
  for (size_t si = 0; si < shards.size(); si++) {
    if (!shards[si].items.empty()) {
      pq.push({si, 0});
    }
  }

  // Compute maximum unique items we need to pull from the heap
  size_t need_unique = max_results;
  if (has_limit) {
    if (offset > max_results && count == 0) {
      need_unique = 0;
    } else if (offset <= SIZE_MAX - count) {
      need_unique = offset + count;
    }
    if (count > max_results) {
      need_unique = (offset <= SIZE_MAX - max_results) ? (offset + max_results) : SIZE_MAX;
    }
  }
  if (knn_cap > 0 && need_unique > knn_cap) {
    need_unique = knn_cap;
  }

  // For KNN LIMIT 0 0, we still need to count unique merged hits up to knn_cap
  bool knn_count_only = (knn_cap > 0 && has_limit && offset == 0 && count == 0);
  if (knn_count_only) {
    need_unique = knn_cap;
  }

  std::unordered_set<std::string_view> seen_ids;
  size_t unique_seen = 0;
  std::vector<MergeCandidate> collected;
  if (!knn_count_only && need_unique > offset) {
    size_t expected_winners = need_unique - offset;
    if (expected_winners > max_results) expected_winners = max_results;
    collected.reserve(std::min<size_t>(expected_winners, 1024));
  }

  while (!pq.empty()) {
    HeapNode top = pq.top();
    pq.pop();

    const auto& cand = shards[top.slice_pos].items[top.item_pos];
    if (top.item_pos + 1 < shards[top.slice_pos].items.size()) {
      pq.push({top.slice_pos, top.item_pos + 1});
    }

    if (!seen_ids.insert(cand.doc_id).second) {
      result.duplicate_count++;
      continue;
    }

    if (unique_seen < need_unique) {
      if (!knn_count_only && unique_seen >= offset && collected.size() < max_results) {
        collected.push_back(cand);
        collected.back().shard_idx = shards[top.slice_pos].shard_idx;
      }
      unique_seen++;
      if (unique_seen >= need_unique) {
        break;
      }
    }
  }

  if (knn_cap > 0) {
    // For KNN queries, total hits before outer LIMIT is the merged unique KNN count (<= knn_cap)
    result.global_total = static_cast<uint64_t>(unique_seen);
  } else {
    result.global_total =
        (raw_total >= result.duplicate_count) ? (raw_total - result.duplicate_count) : 0;
  }

  if (!knn_count_only) {
    result.winners = std::move(collected);
    for (size_t i = 0; i < result.winners.size(); i++) {
      result.winners_by_shard[result.winners[i].shard_idx].push_back(i);
    }
  }
  return result;
}

}  // namespace gemini_search
