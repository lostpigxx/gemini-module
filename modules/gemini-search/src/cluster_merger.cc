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

MergedHybridResult MergeHybridShards(const std::vector<ShardHybridSlice>& shards,
                                     int64_t text_limit, size_t knn_k, const HybridConfig& hcfg,
                                     size_t offset, size_t count, bool has_limit) {
  MergedHybridResult out;

  // Deterministic owner shard per doc_id (smallest shard_idx wins on duplicate during MIGRATE)
  std::unordered_map<std::string, uint32_t> doc_owner;
  std::unordered_map<std::string, double> text_best;
  std::unordered_map<std::string, float> vec_best;

  // Process shards in deterministic shard_idx order
  std::vector<const ShardHybridSlice*> ordered;
  ordered.reserve(shards.size());
  for (const auto& s : shards) ordered.push_back(&s);
  std::sort(ordered.begin(), ordered.end(),
            [](const ShardHybridSlice* a, const ShardHybridSlice* b) {
              return a->shard_idx < b->shard_idx;
            });

  for (const auto* s : ordered) {
    for (const auto& th : s->text_hits) {
      doc_owner.try_emplace(th.doc_id, s->shard_idx);
      auto it = text_best.find(th.doc_id);
      if (it == text_best.end()) {
        text_best.emplace(th.doc_id, th.score);
      } else if (th.score > it->second) {
        it->second = th.score;
      }
    }
    for (const auto& vh : s->vec_hits) {
      doc_owner.try_emplace(vh.doc_id, s->shard_idx);
      auto it = vec_best.find(vh.doc_id);
      if (it == vec_best.end()) {
        vec_best.emplace(vh.doc_id, vh.score);
      } else if (vh.score < it->second) {
        it->second = vh.score;
      }
    }
  }

  std::vector<ScoredResult> global_text;
  global_text.reserve(text_best.size());
  for (const auto& [doc_id, sc] : text_best) {
    global_text.push_back({doc_id, sc});
  }
  std::sort(global_text.begin(), global_text.end(),
            [](const ScoredResult& a, const ScoredResult& b) {
              if (a.score != b.score) return a.score > b.score;
              return a.doc_id < b.doc_id;
            });
  if (text_limit > 0 && static_cast<int64_t>(global_text.size()) > text_limit) {
    global_text.resize(static_cast<size_t>(text_limit));
  }

  std::vector<KnnResult> global_vec;
  global_vec.reserve(vec_best.size());
  for (const auto& [doc_id, dist] : vec_best) {
    global_vec.push_back({doc_id, dist});
  }
  std::sort(global_vec.begin(), global_vec.end(),
            [](const KnnResult& a, const KnnResult& b) {
              if (a.score != b.score) return a.score < b.score;
              return a.doc_id < b.doc_id;
            });
  if (knn_k > 0 && global_vec.size() > knn_k) {
    global_vec.resize(knn_k);
  }

  std::vector<HybridHit> fused = FuseHybridResults(global_text, global_vec, hcfg);
  out.total_fused = static_cast<uint64_t>(fused.size());

  if (has_limit && offset == 0 && count == 0) {
    return out;
  }

  if (has_limit) {
    if (offset >= fused.size()) {
      fused.clear();
    } else {
      size_t end_pos = fused.size();
      if (offset <= SIZE_MAX - count && offset + count < end_pos) {
        end_pos = offset + count;
      }
      fused = std::vector<HybridHit>(fused.begin() + static_cast<ptrdiff_t>(offset),
                                     fused.begin() + static_cast<ptrdiff_t>(end_pos));
    }
  }

  out.winners = std::move(fused);
  for (size_t i = 0; i < out.winners.size(); i++) {
    auto oit = doc_owner.find(out.winners[i].doc_id);
    if (oit != doc_owner.end()) {
      out.winners_by_shard[oit->second].push_back(i);
    }
  }
  return out;
}

std::vector<RpcFieldBm25Stats> AggregateGlobalBm25FieldStats(
    const std::vector<std::vector<RpcFieldBm25Stats>>& shard_stats) {
  struct Acc {
    uint64_t live_docs = 0;
    uint64_t total_doc_len = 0;
    std::unordered_map<std::string, uint64_t> term_df;
  };
  std::unordered_map<std::string, Acc> by_field;
  for (const auto& shard : shard_stats) {
    for (const auto& fs : shard) {
      auto& acc = by_field[fs.field_name];
      acc.live_docs += fs.live_docs;
      acc.total_doc_len += fs.total_doc_len;
      for (const auto& [term, df] : fs.term_df) {
        acc.term_df[term] += df;
      }
    }
  }
  std::vector<RpcFieldBm25Stats> out;
  out.reserve(by_field.size());
  for (auto& [fname, acc] : by_field) {
    RpcFieldBm25Stats st;
    st.field_name = fname;
    st.live_docs = acc.live_docs;
    st.total_doc_len = acc.total_doc_len;
    st.term_df.reserve(acc.term_df.size());
    for (auto& [t, df] : acc.term_df) {
      st.term_df.emplace_back(t, df);
    }
    std::sort(st.term_df.begin(), st.term_df.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    out.push_back(std::move(st));
  }
  std::sort(out.begin(), out.end(),
            [](const RpcFieldBm25Stats& a, const RpcFieldBm25Stats& b) {
              return a.field_name < b.field_name;
            });
  return out;
}

}  // namespace gemini_search
