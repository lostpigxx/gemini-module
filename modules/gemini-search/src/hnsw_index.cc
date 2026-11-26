#include "hnsw_index.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <random>

HnswIndex::HnswIndex(size_t dim, DistanceMetric metric, size_t m,
                     size_t ef_construction, VectorQuantization quantization,
                     VectorRerankType rerank, size_t bq_oversample)
    : dim_(dim),
      metric_(metric),
      quantization_(quantization),
      rerank_(quantization == VectorQuantization::kBq ? rerank : VectorRerankType::kNone),
      bq_oversample_(bq_oversample > 0 ? bq_oversample : 4),
      dist_fn_(GetDistanceFunction(metric)),
      m_(m),
      m_max0_(m * 2),
      ef_construction_(ef_construction),
      ef_runtime_(10),
      ml_(1.0 / std::log(static_cast<double>(m > 1 ? m : 2))),
      bq_words_(BqWords(dim)) {}

int HnswIndex::RandomLevel(const std::string& doc_id) const {
  auto seed = static_cast<unsigned>(std::hash<std::string>{}(doc_id));
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  double r = dist(rng);
  if (r == 0.0) r = 1e-10;
  int level = static_cast<int>(std::floor(-std::log(r) * ml_));
  return std::min(level, 32);
}

namespace {

struct BqQueryBuf {
  uint64_t stack_buf[32];
  std::vector<uint64_t> heap_buf;
  const uint64_t* ptr = nullptr;

  void Init(const float* query, size_t dim, size_t bq_words, VectorQuantization q) {
    if (q != VectorQuantization::kBq || !query) {
      ptr = nullptr;
      return;
    }
    uint64_t* dst = stack_buf;
    if (bq_words > 32) {
      heap_buf.resize(bq_words);
      dst = heap_buf.data();
    }
    QuantizeBq(query, dim, dst);
    ptr = dst;
  }
};

}  // namespace

float HnswIndex::RerankDistanceTo(const float* query, uint32_t node_id) const {
  if (node_id >= alive_.size() || !alive_[node_id]) {
    return std::numeric_limits<float>::max();
  }
  if (rerank_ == VectorRerankType::kFp16) {
    return Fp16Distance(query, fp16_vectors_.data() + static_cast<size_t>(node_id) * dim_,
                        dim_, metric_);
  }
  if (rerank_ == VectorRerankType::kSq8) {
    return Sq8Distance(query, sq8_vectors_.data() + static_cast<size_t>(node_id) * dim_,
                       sq8_meta_[node_id], dim_, metric_);
  }
  return DistanceTo(query, nullptr, node_id);
}

float HnswIndex::DistanceTo(const float* query, const uint64_t* query_bq,
                            uint32_t node_id) const {
  if (node_id >= alive_.size() || !alive_[node_id]) {
    return std::numeric_limits<float>::max();
  }
  if (quantization_ == VectorQuantization::kSq8) {
    return Sq8Distance(query, sq8_vectors_.data() + static_cast<size_t>(node_id) * dim_,
                       sq8_meta_[node_id], dim_, metric_);
  }
  if (quantization_ == VectorQuantization::kBq) {
    if (query_bq) {
      return BqHammingDistance(
          query_bq, bq_vectors_.data() + static_cast<size_t>(node_id) * bq_words_, dim_);
    }
    BqQueryBuf buf;
    buf.Init(query, dim_, bq_words_, quantization_);
    return BqHammingDistance(
        buf.ptr, bq_vectors_.data() + static_cast<size_t>(node_id) * bq_words_, dim_);
  }
  return dist_fn_(query, vectors_.data() + static_cast<size_t>(node_id) * dim_, dim_);
}

uint32_t HnswIndex::GreedySearchLayer(const float* query, const uint64_t* query_bq,
                                      uint32_t entry, int layer) const {
  uint32_t cur = entry;
  float cur_dist = DistanceTo(query, query_bq, cur);
  while (true) {
    bool improved = false;
    if (layer < static_cast<int>(neighbors_[cur].size())) {
      for (uint32_t nb_id : neighbors_[cur][static_cast<size_t>(layer)]) {
        if (!alive_[nb_id]) continue;
        float d = DistanceTo(query, query_bq, nb_id);
        if (d < cur_dist) {
          cur_dist = d;
          cur = nb_id;
          improved = true;
        }
      }
    }
    if (!improved) break;
  }
  return cur;
}

std::vector<std::pair<float, uint32_t>> HnswIndex::SearchLayer(
    const float* query, const uint64_t* query_bq, uint32_t entry,
    size_t ef, int layer) const {
  using Pair = std::pair<float, uint32_t>;
  auto cmp_min = [](const Pair& a, const Pair& b) { return a.first > b.first; };
  auto cmp_max = [](const Pair& a, const Pair& b) { return a.first < b.first; };

  std::priority_queue<Pair, std::vector<Pair>, decltype(cmp_min)> candidates(cmp_min);
  std::priority_queue<Pair, std::vector<Pair>, decltype(cmp_max)> result(cmp_max);

  // Thread-local zero-allocation visited-epoch buffer (safe for concurrent readers)
  struct ThreadVisited {
    std::vector<uint32_t> epoch;
    uint32_t cur = 0;
    uint32_t Next(size_t n) {
      if (epoch.size() < n) epoch.resize(n, 0);
      if (cur == std::numeric_limits<uint32_t>::max()) {
        std::fill(epoch.begin(), epoch.end(), 0);
        cur = 1;
      } else {
        cur++;
      }
      return cur;
    }
  };
  thread_local ThreadVisited tv;
  const uint32_t epoch = tv.Next(alive_.size());
  float entry_dist = DistanceTo(query, query_bq, entry);
  candidates.push({entry_dist, entry});
  result.push({entry_dist, entry});
  tv.epoch[entry] = epoch;

  while (!candidates.empty()) {
    auto [c_dist, c_id] = candidates.top();
    candidates.pop();

    float worst_result = result.top().first;
    if (c_dist > worst_result) break;

    if (layer >= static_cast<int>(neighbors_[c_id].size())) continue;
    const auto& nb_list = neighbors_[c_id][static_cast<size_t>(layer)];
    for (uint32_t nb_id : nb_list) {
      if (tv.epoch[nb_id] == epoch) continue;
      tv.epoch[nb_id] = epoch;
      if (!alive_[nb_id]) continue;

      float n_dist = DistanceTo(query, query_bq, nb_id);
      worst_result = result.top().first;

      if (n_dist < worst_result || result.size() < ef) {
        candidates.push({n_dist, nb_id});
        result.push({n_dist, nb_id});
        if (result.size() > ef) result.pop();
      }
    }
  }

  std::vector<Pair> out;
  out.reserve(result.size());
  while (!result.empty()) {
    out.push_back(result.top());
    result.pop();
  }
  std::reverse(out.begin(), out.end());
  return out;
}

std::vector<uint32_t> HnswIndex::SelectNeighbors(
    const float* query, const uint64_t* query_bq,
    const std::vector<uint32_t>& candidates, size_t max_neighbors) const {
  std::vector<std::pair<float, uint32_t>> scored;
  scored.reserve(candidates.size());
  for (uint32_t id : candidates) {
    if (id < alive_.size() && alive_[id]) {
      scored.push_back({DistanceTo(query, query_bq, id), id});
    }
  }
  size_t n = std::min(max_neighbors, scored.size());
  if (n < scored.size()) {
    std::partial_sort(scored.begin(), scored.begin() + static_cast<ptrdiff_t>(n),
                      scored.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
  } else {
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
  }

  std::vector<uint32_t> result;
  result.reserve(n);
  for (size_t i = 0; i < n; i++) {
    result.push_back(scored[i].second);
  }
  return result;
}

void HnswIndex::Add(const std::string& doc_id, const float* data) {
  Remove(doc_id);

  int level = RandomLevel(doc_id);
  uint32_t nid = 0;
  if (!free_slots_.empty()) {
    nid = free_slots_.back();
    free_slots_.pop_back();
    doc_ids_[nid] = doc_id;
    alive_[nid] = 1;
    max_layers_[nid] = level;
    neighbors_[nid].assign(static_cast<size_t>(level + 1), {});
    if (quantization_ == VectorQuantization::kSq8) {
      QuantizeSq8(data, dim_, sq8_vectors_.data() + static_cast<size_t>(nid) * dim_,
                  sq8_meta_[nid]);
    } else if (quantization_ == VectorQuantization::kBq) {
      QuantizeBq(data, dim_, bq_vectors_.data() + static_cast<size_t>(nid) * bq_words_);
      if (rerank_ == VectorRerankType::kFp16) {
        uint16_t* dst = fp16_vectors_.data() + static_cast<size_t>(nid) * dim_;
        for (size_t d = 0; d < dim_; d++) dst[d] = Float32ToFloat16(data[d]);
      } else if (rerank_ == VectorRerankType::kSq8) {
        QuantizeSq8(data, dim_, sq8_vectors_.data() + static_cast<size_t>(nid) * dim_,
                    sq8_meta_[nid]);
      }
    } else {
      std::memcpy(vectors_.data() + static_cast<size_t>(nid) * dim_, data,
                  dim_ * sizeof(float));
    }
  } else {
    nid = static_cast<uint32_t>(alive_.size());
    doc_ids_.push_back(doc_id);
    alive_.push_back(1);
    max_layers_.push_back(level);
    neighbors_.emplace_back(static_cast<size_t>(level + 1));
    if (quantization_ == VectorQuantization::kSq8) {
      sq8_vectors_.resize(static_cast<size_t>(nid + 1) * dim_);
      sq8_meta_.emplace_back();
      QuantizeSq8(data, dim_, sq8_vectors_.data() + static_cast<size_t>(nid) * dim_,
                  sq8_meta_[nid]);
    } else if (quantization_ == VectorQuantization::kBq) {
      bq_vectors_.resize(static_cast<size_t>(nid + 1) * bq_words_);
      QuantizeBq(data, dim_, bq_vectors_.data() + static_cast<size_t>(nid) * bq_words_);
      if (rerank_ == VectorRerankType::kFp16) {
        fp16_vectors_.resize(static_cast<size_t>(nid + 1) * dim_);
        uint16_t* dst = fp16_vectors_.data() + static_cast<size_t>(nid) * dim_;
        for (size_t d = 0; d < dim_; d++) dst[d] = Float32ToFloat16(data[d]);
      } else if (rerank_ == VectorRerankType::kSq8) {
        sq8_vectors_.resize(static_cast<size_t>(nid + 1) * dim_);
        sq8_meta_.emplace_back();
        QuantizeSq8(data, dim_, sq8_vectors_.data() + static_cast<size_t>(nid) * dim_,
                    sq8_meta_[nid]);
      }
    } else {
      vectors_.insert(vectors_.end(), data, data + dim_);
    }
  }
  id_to_node_[doc_id] = nid;
  live_count_++;

  if (max_level_ < 0 || entry_point_ == kInvalidNode) {
    entry_point_ = nid;
    max_level_ = level;
    return;
  }

  uint32_t cur_entry = entry_point_;
  const uint64_t* data_bq =
      (quantization_ == VectorQuantization::kBq)
          ? (bq_vectors_.data() + static_cast<size_t>(nid) * bq_words_)
          : nullptr;

  for (int lc = max_level_; lc > level; lc--) {
    cur_entry = GreedySearchLayer(data, data_bq, cur_entry, lc);
  }

  std::vector<float> tmp_nb_vec;
  for (int lc = std::min(level, max_level_); lc >= 0; lc--) {
    auto candidates = SearchLayer(data, data_bq, cur_entry, ef_construction_, lc);
    size_t max_conn = (lc == 0) ? m_max0_ : m_;

    // candidates are already sorted ascending by distance from SearchLayer
    std::vector<uint32_t> chosen;
    chosen.reserve(std::min(max_conn, candidates.size()));
    for (const auto& [dist, cid] : candidates) {
      (void)dist;
      if (cid != nid && cid < alive_.size() && alive_[cid]) {
        chosen.push_back(cid);
        if (chosen.size() >= max_conn) break;
      }
    }

    neighbors_[nid][static_cast<size_t>(lc)] = chosen;

    for (uint32_t nb_id : chosen) {
      if (!alive_[nb_id] || lc >= static_cast<int>(neighbors_[nb_id].size())) continue;
      auto& nb_list = neighbors_[nb_id][static_cast<size_t>(lc)];
      nb_list.push_back(nid);

      if (nb_list.size() > max_conn) {
        const float* nb_vec_ptr = nullptr;
        const uint64_t* nb_bq_ptr = nullptr;
        if (quantization_ == VectorQuantization::kSq8) {
          tmp_nb_vec.resize(dim_);
          DequantizeSq8(sq8_vectors_.data() + static_cast<size_t>(nb_id) * dim_,
                        sq8_meta_[nb_id], dim_, tmp_nb_vec.data());
          nb_vec_ptr = tmp_nb_vec.data();
        } else if (quantization_ == VectorQuantization::kBq) {
          nb_bq_ptr = bq_vectors_.data() + static_cast<size_t>(nb_id) * bq_words_;
        } else {
          nb_vec_ptr = vectors_.data() + static_cast<size_t>(nb_id) * dim_;
        }
        nb_list = SelectNeighbors(nb_vec_ptr, nb_bq_ptr, nb_list, max_conn);
      }
    }

    if (!candidates.empty()) cur_entry = candidates[0].second;
  }

  if (level > max_level_) {
    entry_point_ = nid;
    max_level_ = level;
  }
}

bool HnswIndex::Remove(const std::string& doc_id) {
  auto it = id_to_node_.find(doc_id);
  if (it == id_to_node_.end()) return false;

  uint32_t nid = it->second;
  id_to_node_.erase(it);

  int node_max_layer = max_layers_[nid];
  for (int lc = 0; lc <= node_max_layer; lc++) {
    if (lc >= static_cast<int>(neighbors_[nid].size())) break;
    for (uint32_t nb_id : neighbors_[nid][static_cast<size_t>(lc)]) {
      if (!alive_[nb_id] || lc >= static_cast<int>(neighbors_[nb_id].size())) continue;
      auto& nb_list = neighbors_[nb_id][static_cast<size_t>(lc)];
      nb_list.erase(std::remove(nb_list.begin(), nb_list.end(), nid), nb_list.end());
    }
  }

  alive_[nid] = 0;
  neighbors_[nid].clear();
  doc_ids_[nid].clear();
  free_slots_.push_back(nid);
  live_count_--;

  if (live_count_ == 0) {
    entry_point_ = kInvalidNode;
    max_level_ = -1;
    return true;
  }

  if (nid == entry_point_) {
    uint32_t best_id = kInvalidNode;
    int best_layer = -1;
    for (uint32_t i = 0; i < alive_.size(); i++) {
      if (alive_[i] && max_layers_[i] > best_layer) {
        best_layer = max_layers_[i];
        best_id = i;
      }
    }
    entry_point_ = best_id;
    max_level_ = best_layer;
  }

  return true;
}

std::vector<KnnResult> HnswIndex::KnnQuery(const float* query, size_t k) const {
  return KnnQueryEf(query, k, ef_runtime_, 0);
}

std::vector<KnnResult> HnswIndex::KnnQueryEf(const float* query, size_t k,
                                              size_t ef_runtime,
                                              size_t oversample) const {
  if (live_count_ == 0 || entry_point_ == kInvalidNode || k == 0) return {};

  BqQueryBuf qbq;
  qbq.Init(query, dim_, bq_words_, quantization_);

  uint32_t cur = entry_point_;
  for (int lc = max_level_; lc > 0; lc--) {
    cur = GreedySearchLayer(query, qbq.ptr, cur, lc);
  }

  const bool do_rerank =
      (quantization_ == VectorQuantization::kBq && rerank_ != VectorRerankType::kNone);
  size_t cand_k = k;
  if (do_rerank) {
    size_t os = (oversample > 0) ? oversample : bq_oversample_;
    cand_k = std::min(live_count_, std::max(k, k * os));
  }

  size_t ef = std::max(ef_runtime > 0 ? ef_runtime : ef_runtime_, cand_k);
  auto candidates = SearchLayer(query, qbq.ptr, cur, ef, 0);

  if (do_rerank) {
    std::vector<KnnResult> reranked;
    reranked.reserve(std::min(cand_k, candidates.size()));
    for (const auto& [dist, nid] : candidates) {
      (void)dist;
      if (alive_[nid]) {
        reranked.push_back({doc_ids_[nid], RerankDistanceTo(query, nid)});
        if (reranked.size() >= cand_k) break;
      }
    }
    size_t n = std::min(k, reranked.size());
    std::partial_sort(reranked.begin(),
                      reranked.begin() + static_cast<ptrdiff_t>(n),
                      reranked.end(),
                      [](const KnnResult& a, const KnnResult& b) {
                        return a.score < b.score;
                      });
    reranked.resize(n);
    return reranked;
  }

  std::vector<KnnResult> results;
  results.reserve(std::min(k, candidates.size()));
  for (const auto& [dist, nid] : candidates) {
    if (alive_[nid]) {
      results.push_back({doc_ids_[nid], dist});
      if (results.size() >= k) break;
    }
  }
  return results;
}

std::vector<KnnResult> HnswIndex::RangeQuery(const float* query,
                                              float radius) const {
  BqQueryBuf qbq;
  qbq.Init(query, dim_, bq_words_, quantization_);

  std::vector<KnnResult> results;
  for (uint32_t i = 0; i < alive_.size(); i++) {
    if (!alive_[i]) continue;
    float score = DistanceTo(query, qbq.ptr, i);
    if (score <= radius) {
      results.push_back({doc_ids_[i], score});
    }
  }
  std::sort(results.begin(), results.end(),
            [](const KnnResult& a, const KnnResult& b) {
              if (a.score != b.score) return a.score < b.score;
              return a.doc_id < b.doc_id;
            });
  return results;
}

std::vector<KnnResult> HnswIndex::KnnQueryFiltered(
    const float* query, size_t k,
    const std::vector<std::string>& candidates,
    size_t ef_runtime, size_t oversample) const {
  if (live_count_ == 0 || entry_point_ == kInvalidNode || k == 0 || candidates.empty()) {
    return {};
  }

  const bool do_rerank =
      (quantization_ == VectorQuantization::kBq && rerank_ != VectorRerankType::kNone);

  auto brute_force = [&](const std::vector<uint32_t>* resolved_nids) -> std::vector<KnnResult> {
    BqQueryBuf qbq;
    if (!do_rerank) qbq.Init(query, dim_, bq_words_, quantization_);

    std::vector<KnnResult> results;
    if (resolved_nids) {
      results.reserve(resolved_nids->size());
      for (uint32_t nid : *resolved_nids) {
        float d = do_rerank ? RerankDistanceTo(query, nid)
                            : DistanceTo(query, qbq.ptr, nid);
        results.push_back({doc_ids_[nid], d});
      }
    } else {
      results.reserve(candidates.size());
      for (const auto& doc_id : candidates) {
        auto it = id_to_node_.find(doc_id);
        if (it == id_to_node_.end()) continue;
        float d = do_rerank ? RerankDistanceTo(query, it->second)
                            : DistanceTo(query, qbq.ptr, it->second);
        results.push_back({doc_id, d});
      }
    }
    size_t n = std::min(k, results.size());
    std::partial_sort(results.begin(), results.begin() + static_cast<ptrdiff_t>(n),
                      results.end(),
                      [](const KnnResult& a, const KnnResult& b) {
                        return a.score < b.score;
                      });
    results.resize(n);
    return results;
  };

  // Adaptive selectivity threshold:
  // For small candidate sets (< 512) or low selectivity (< 5%) or small graphs (< 1024),
  // direct brute-force over candidates is faster and 100% exact.
  constexpr size_t kBruteForceMaxCands = 512;
  constexpr size_t kMinGraphSizeForFilteredHnsw = 1024;
  constexpr double kMinSelectivityForFilteredHnsw = 0.05;

  double est_selectivity =
      static_cast<double>(std::min(candidates.size(), live_count_)) /
      static_cast<double>(live_count_);
  if (live_count_ < kMinGraphSizeForFilteredHnsw ||
      candidates.size() <= kBruteForceMaxCands ||
      est_selectivity < kMinSelectivityForFilteredHnsw) {
    return brute_force(nullptr);
  }

  // Thread-local zero-allocation epoch-stamped allowed set & visited set
  struct ThreadEpochBuf {
    std::vector<uint32_t> epoch;
    uint32_t cur = 0;
    uint32_t Next(size_t n) {
      if (epoch.size() < n) epoch.resize(n, 0);
      if (cur == std::numeric_limits<uint32_t>::max()) {
        std::fill(epoch.begin(), epoch.end(), 0);
        cur = 1;
      } else {
        cur++;
      }
      return cur;
    }
  };
  thread_local ThreadEpochBuf allowed_buf;
  thread_local ThreadEpochBuf visited_buf;

  const uint32_t allow_epoch = allowed_buf.Next(alive_.size());
  size_t valid_cands = 0;
  std::vector<uint32_t> valid_nids;
  valid_nids.reserve(std::min(candidates.size(), live_count_));
  for (const auto& doc_id : candidates) {
    auto it = id_to_node_.find(doc_id);
    if (it == id_to_node_.end()) continue;
    uint32_t nid = it->second;
    if (nid < alive_.size() && alive_[nid] && allowed_buf.epoch[nid] != allow_epoch) {
      allowed_buf.epoch[nid] = allow_epoch;
      valid_nids.push_back(nid);
      valid_cands++;
    }
  }
  if (valid_cands == 0) return {};

  double actual_sel = static_cast<double>(valid_cands) / static_cast<double>(live_count_);
  if (valid_cands <= kBruteForceMaxCands || actual_sel < kMinSelectivityForFilteredHnsw) {
    return brute_force(&valid_nids);
  }

  // Filtered HNSW graph traversal (uses BQ Hamming if quantized, with optional rerank)
  BqQueryBuf qbq;
  qbq.Init(query, dim_, bq_words_, quantization_);

  uint32_t cur = entry_point_;
  for (int lc = max_level_; lc > 0; lc--) {
    cur = GreedySearchLayer(query, qbq.ptr, cur, lc);
  }

  size_t cand_k = k;
  if (do_rerank) {
    size_t os = (oversample > 0) ? oversample : bq_oversample_;
    cand_k = std::min(valid_cands, std::max(k, k * os));
  } else {
    cand_k = std::min(valid_cands, k);
  }
  size_t target_ef = std::max(ef_runtime > 0 ? ef_runtime : ef_runtime_, cand_k);
  size_t max_expansions = static_cast<size_t>(
      std::ceil(static_cast<double>(target_ef) / std::max(actual_sel, 0.05)));
  max_expansions = std::min(live_count_, std::max(max_expansions * 2, target_ef * 4));

  using Pair = std::pair<float, uint32_t>;
  auto cmp_min = [](const Pair& a, const Pair& b) { return a.first > b.first; };
  auto cmp_max = [](const Pair& a, const Pair& b) { return a.first < b.first; };
  std::priority_queue<Pair, std::vector<Pair>, decltype(cmp_min)> frontier(cmp_min);
  std::priority_queue<Pair, std::vector<Pair>, decltype(cmp_max)> filtered(cmp_max);

  const uint32_t visit_epoch = visited_buf.Next(alive_.size());
  float entry_dist = DistanceTo(query, qbq.ptr, cur);
  frontier.push({entry_dist, cur});
  visited_buf.epoch[cur] = visit_epoch;
  if (allowed_buf.epoch[cur] == allow_epoch) {
    filtered.push({entry_dist, cur});
  }

  size_t expanded = 0;
  while (!frontier.empty() && expanded < max_expansions) {
    auto [c_dist, c_id] = frontier.top();
    frontier.pop();
    expanded++;

    if (filtered.size() >= target_ef && c_dist > filtered.top().first) {
      break;
    }

    if (neighbors_[c_id].empty()) continue;
    const auto& nb_list = neighbors_[c_id][0];
    for (uint32_t nb_id : nb_list) {
      if (visited_buf.epoch[nb_id] == visit_epoch) continue;
      visited_buf.epoch[nb_id] = visit_epoch;
      if (!alive_[nb_id]) continue;

      float n_dist = DistanceTo(query, qbq.ptr, nb_id);
      if (filtered.size() < target_ef || n_dist < filtered.top().first) {
        frontier.push({n_dist, nb_id});
        if (allowed_buf.epoch[nb_id] == allow_epoch) {
          filtered.push({n_dist, nb_id});
          if (filtered.size() > target_ef) filtered.pop();
        }
      }
    }
  }

  // Fallback to exact brute-force if disconnected graph yielded fewer than min(k, valid_cands)
  if (filtered.size() < std::min(k, valid_cands)) {
    return brute_force(&valid_nids);
  }

  std::vector<Pair> matched;
  matched.reserve(filtered.size());
  while (!filtered.empty()) {
    matched.push_back(filtered.top());
    filtered.pop();
  }
  std::reverse(matched.begin(), matched.end());

  if (do_rerank) {
    std::vector<KnnResult> reranked;
    reranked.reserve(std::min(cand_k, matched.size()));
    for (const auto& [dist, nid] : matched) {
      (void)dist;
      reranked.push_back({doc_ids_[nid], RerankDistanceTo(query, nid)});
      if (reranked.size() >= cand_k) break;
    }
    size_t n = std::min(k, reranked.size());
    std::partial_sort(reranked.begin(),
                      reranked.begin() + static_cast<ptrdiff_t>(n),
                      reranked.end(),
                      [](const KnnResult& a, const KnnResult& b) {
                        return a.score < b.score;
                      });
    reranked.resize(n);
    return reranked;
  }

  std::vector<KnnResult> results;
  results.reserve(std::min(k, matched.size()));
  for (const auto& [dist, nid] : matched) {
    results.push_back({doc_ids_[nid], dist});
    if (results.size() >= k) break;
  }
  return results;
}

size_t HnswIndex::Size() const { return live_count_; }
size_t HnswIndex::Dim() const { return dim_; }
DistanceMetric HnswIndex::Metric() const { return metric_; }
size_t HnswIndex::M() const { return m_; }
size_t HnswIndex::EfConstruction() const { return ef_construction_; }
size_t HnswIndex::EfRuntime() const { return ef_runtime_; }
void HnswIndex::SetEfRuntime(size_t ef) { ef_runtime_ = ef; }

void HnswIndex::Clear() {
  vectors_.clear();
  sq8_vectors_.clear();
  sq8_meta_.clear();
  fp16_vectors_.clear();
  bq_vectors_.clear();
  doc_ids_.clear();
  alive_.clear();
  max_layers_.clear();
  neighbors_.clear();
  free_slots_.clear();
  id_to_node_.clear();
  entry_point_ = kInvalidNode;
  max_level_ = -1;
  live_count_ = 0;
}

namespace {

constexpr uint32_t kHnswGraphMagic = 0x57534E48u;  // "HNSW"
constexpr uint32_t kHnswGraphVersion = 2u;
constexpr uint32_t kMaxHnswRdbNodes = 100000000u;
constexpr int32_t kMaxHnswRdbLevel = 32;

template <typename T>
void AppendPod(std::string& out, const T& val) {
  const char* p = reinterpret_cast<const char*>(&val);
  out.append(p, sizeof(T));
}

template <typename T>
bool ReadPod(const char* data, size_t len, size_t& pos, T& out) {
  if (pos > len || len - pos < sizeof(T)) return false;
  std::memcpy(&out, data + pos, sizeof(T));
  pos += sizeof(T);
  return true;
}

bool ReadBytes(const char* data, size_t len, size_t& pos, void* dst, size_t n) {
  if (pos > len || len - pos < n) return false;
  if (n > 0) std::memcpy(dst, data + pos, n);
  pos += n;
  return true;
}

}  // namespace

std::string HnswIndex::SerializeGraph() const {
  // Compact live nodes to dense IDs 0..live_count_-1 so tombstones are stripped
  std::vector<uint32_t> old_to_new(alive_.size(), kInvalidNode);
  std::vector<uint32_t> new_to_old;
  new_to_old.reserve(live_count_);
  for (uint32_t i = 0; i < alive_.size(); i++) {
    if (alive_[i]) {
      old_to_new[i] = static_cast<uint32_t>(new_to_old.size());
      new_to_old.push_back(i);
    }
  }

  uint32_t num_nodes = static_cast<uint32_t>(new_to_old.size());
  uint32_t new_entry = kInvalidNode;
  int32_t new_max_level = -1;
  if (num_nodes > 0 && entry_point_ < alive_.size() && alive_[entry_point_]) {
    new_entry = old_to_new[entry_point_];
    new_max_level = max_level_;
  } else if (num_nodes > 0) {
    // Defensive fallback: pick highest-layer live node
    for (uint32_t nid = 0; nid < num_nodes; nid++) {
      int ly = max_layers_[new_to_old[nid]];
      if (ly > new_max_level) {
        new_max_level = ly;
        new_entry = nid;
      }
    }
  }

  size_t vec_bytes_per_node = 0;
  if (quantization_ == VectorQuantization::kSq8) {
    vec_bytes_per_node = sizeof(Sq8Meta) + dim_ * sizeof(uint8_t);
  } else if (quantization_ == VectorQuantization::kBq) {
    vec_bytes_per_node = bq_words_ * sizeof(uint64_t);
    if (rerank_ == VectorRerankType::kFp16) {
      vec_bytes_per_node += dim_ * sizeof(uint16_t);
    } else if (rerank_ == VectorRerankType::kSq8) {
      vec_bytes_per_node += sizeof(Sq8Meta) + dim_ * sizeof(uint8_t);
    }
  } else {
    vec_bytes_per_node = dim_ * sizeof(float);
  }

  std::string out;
  out.reserve(64 + static_cast<size_t>(num_nodes) * (32 + vec_bytes_per_node + m_max0_ * 4));

  AppendPod<uint32_t>(out, kHnswGraphMagic);
  AppendPod<uint32_t>(out, kHnswGraphVersion);
  AppendPod<uint32_t>(out, static_cast<uint32_t>(dim_));
  AppendPod<uint8_t>(out, static_cast<uint8_t>(metric_));
  AppendPod<uint8_t>(out, static_cast<uint8_t>(quantization_));
  AppendPod<uint8_t>(out, static_cast<uint8_t>(rerank_));
  AppendPod<uint32_t>(out, static_cast<uint32_t>(bq_oversample_));
  AppendPod<uint32_t>(out, static_cast<uint32_t>(m_));
  AppendPod<uint32_t>(out, static_cast<uint32_t>(m_max0_));
  AppendPod<uint32_t>(out, static_cast<uint32_t>(ef_construction_));
  AppendPod<uint32_t>(out, static_cast<uint32_t>(ef_runtime_));
  AppendPod<uint32_t>(out, num_nodes);
  AppendPod<uint32_t>(out, new_entry);
  AppendPod<int32_t>(out, new_max_level);

  std::vector<uint32_t> mapped_nbs;
  for (uint32_t nid = 0; nid < num_nodes; nid++) {
    uint32_t old_id = new_to_old[nid];
    const std::string& did = doc_ids_[old_id];
    AppendPod<uint32_t>(out, static_cast<uint32_t>(did.size()));
    out.append(did.data(), did.size());

    int32_t layer_cap = std::min<int32_t>(
        max_layers_[old_id],
        static_cast<int32_t>(neighbors_[old_id].size()) - 1);
    if (layer_cap < 0) layer_cap = 0;
    AppendPod<int32_t>(out, layer_cap);

    if (quantization_ == VectorQuantization::kSq8) {
      AppendPod<float>(out, sq8_meta_[old_id].min_val);
      AppendPod<float>(out, sq8_meta_[old_id].scale);
      const uint8_t* ptr = sq8_vectors_.data() + static_cast<size_t>(old_id) * dim_;
      out.append(reinterpret_cast<const char*>(ptr), dim_ * sizeof(uint8_t));
    } else if (quantization_ == VectorQuantization::kBq) {
      const uint64_t* ptr = bq_vectors_.data() + static_cast<size_t>(old_id) * bq_words_;
      out.append(reinterpret_cast<const char*>(ptr), bq_words_ * sizeof(uint64_t));
      if (rerank_ == VectorRerankType::kFp16) {
        const uint16_t* fptr = fp16_vectors_.data() + static_cast<size_t>(old_id) * dim_;
        out.append(reinterpret_cast<const char*>(fptr), dim_ * sizeof(uint16_t));
      } else if (rerank_ == VectorRerankType::kSq8) {
        AppendPod<float>(out, sq8_meta_[old_id].min_val);
        AppendPod<float>(out, sq8_meta_[old_id].scale);
        const uint8_t* sptr = sq8_vectors_.data() + static_cast<size_t>(old_id) * dim_;
        out.append(reinterpret_cast<const char*>(sptr), dim_ * sizeof(uint8_t));
      }
    } else {
      const float* ptr = vectors_.data() + static_cast<size_t>(old_id) * dim_;
      out.append(reinterpret_cast<const char*>(ptr), dim_ * sizeof(float));
    }

    for (int32_t lc = 0; lc <= layer_cap; lc++) {
      mapped_nbs.clear();
      if (static_cast<size_t>(lc) < neighbors_[old_id].size()) {
        for (uint32_t old_nb : neighbors_[old_id][static_cast<size_t>(lc)]) {
          if (old_nb < old_to_new.size() && old_to_new[old_nb] != kInvalidNode &&
              old_to_new[old_nb] != nid) {
            mapped_nbs.push_back(old_to_new[old_nb]);
          }
        }
      }
      AppendPod<uint32_t>(out, static_cast<uint32_t>(mapped_nbs.size()));
      if (!mapped_nbs.empty()) {
        out.append(reinterpret_cast<const char*>(mapped_nbs.data()),
                   mapped_nbs.size() * sizeof(uint32_t));
      }
    }
  }

  return out;
}

bool HnswIndex::DeserializeGraph(const char* data, size_t len) {
  if (!data && len > 0) return false;
  size_t pos = 0;

  uint32_t magic = 0, version = 0, dim = 0;
  uint8_t raw_metric = 0, raw_quant = 0, raw_rerank = 0;
  uint32_t bq_os = static_cast<uint32_t>(bq_oversample_);
  uint32_t m = 0, m_max0 = 0, ef_c = 0, ef_r = 0;
  uint32_t num_nodes = 0, entry_point = kInvalidNode;
  int32_t max_level = -1;

  if (!ReadPod(data, len, pos, magic) || magic != kHnswGraphMagic) return false;
  if (!ReadPod(data, len, pos, version) || (version != 1u && version != kHnswGraphVersion)) {
    return false;
  }
  if (!ReadPod(data, len, pos, dim) || dim != dim_) return false;
  if (!ReadPod(data, len, pos, raw_metric) ||
      raw_metric != static_cast<uint8_t>(metric_)) {
    return false;
  }
  if (!ReadPod(data, len, pos, raw_quant) ||
      raw_quant != static_cast<uint8_t>(quantization_)) {
    return false;
  }
  if (version >= 2u) {
    if (!ReadPod(data, len, pos, raw_rerank) ||
        raw_rerank != static_cast<uint8_t>(rerank_)) {
      return false;
    }
    if (!ReadPod(data, len, pos, bq_os) || bq_os == 0 || bq_os > 1024) return false;
  } else {
    if (rerank_ != VectorRerankType::kNone) return false;
  }
  if (!ReadPod(data, len, pos, m) || m == 0 || m > 1024) return false;
  if (!ReadPod(data, len, pos, m_max0) || m_max0 < m || m_max0 > 4096) return false;
  if (!ReadPod(data, len, pos, ef_c) || ef_c == 0 || ef_c > 100000) return false;
  if (!ReadPod(data, len, pos, ef_r) || ef_r > 100000) return false;
  if (!ReadPod(data, len, pos, num_nodes) || num_nodes > kMaxHnswRdbNodes) return false;
  if (!ReadPod(data, len, pos, entry_point)) return false;
  if (!ReadPod(data, len, pos, max_level)) return false;

  if (num_nodes == 0) {
    if (entry_point != kInvalidNode || max_level != -1 || pos != len) return false;
    Clear();
    bq_oversample_ = bq_os;
    m_ = m;
    m_max0_ = m_max0;
    ef_construction_ = ef_c;
    ef_runtime_ = ef_r;
    ml_ = 1.0 / std::log(static_cast<double>(m_ > 1 ? m_ : 2));
    return true;
  }

  if (entry_point >= num_nodes || max_level < 0 || max_level > kMaxHnswRdbLevel) {
    return false;
  }

  size_t vec_bytes_per_node = 0;
  if (quantization_ == VectorQuantization::kSq8) {
    vec_bytes_per_node = sizeof(float) * 2 + dim_ * sizeof(uint8_t);
  } else if (quantization_ == VectorQuantization::kBq) {
    vec_bytes_per_node = bq_words_ * sizeof(uint64_t);
    if (rerank_ == VectorRerankType::kFp16) {
      vec_bytes_per_node += dim_ * sizeof(uint16_t);
    } else if (rerank_ == VectorRerankType::kSq8) {
      vec_bytes_per_node += sizeof(float) * 2 + dim_ * sizeof(uint8_t);
    }
  } else {
    vec_bytes_per_node = dim_ * sizeof(float);
  }
  // Minimum bytes per node: doc_id_len(4) + max_layer(4) + vec_bytes + layer0_deg(4)
  size_t min_node_bytes = 12 + vec_bytes_per_node;
  if (num_nodes > (len - pos) / min_node_bytes) return false;

  std::vector<float> new_vectors;
  std::vector<uint8_t> new_sq8_vectors;
  std::vector<Sq8Meta> new_sq8_meta;
  std::vector<uint16_t> new_fp16_vectors;
  std::vector<uint64_t> new_bq_vectors;
  if (quantization_ == VectorQuantization::kSq8) {
    new_sq8_vectors.resize(static_cast<size_t>(num_nodes) * dim_);
    new_sq8_meta.resize(num_nodes);
  } else if (quantization_ == VectorQuantization::kBq) {
    new_bq_vectors.resize(static_cast<size_t>(num_nodes) * bq_words_);
    if (rerank_ == VectorRerankType::kFp16) {
      new_fp16_vectors.resize(static_cast<size_t>(num_nodes) * dim_);
    } else if (rerank_ == VectorRerankType::kSq8) {
      new_sq8_vectors.resize(static_cast<size_t>(num_nodes) * dim_);
      new_sq8_meta.resize(num_nodes);
    }
  } else {
    new_vectors.resize(static_cast<size_t>(num_nodes) * dim_);
  }

  std::vector<std::string> new_doc_ids(num_nodes);
  std::vector<uint8_t> new_alive(num_nodes, 1);
  std::vector<int> new_max_layers(num_nodes, 0);
  std::vector<std::vector<std::vector<uint32_t>>> new_neighbors(num_nodes);
  std::unordered_map<std::string, uint32_t> new_id_to_node;
  new_id_to_node.reserve(num_nodes);

  for (uint32_t nid = 0; nid < num_nodes; nid++) {
    uint32_t did_len = 0;
    if (!ReadPod(data, len, pos, did_len) || did_len == 0 || did_len > len - pos) {
      return false;
    }
    new_doc_ids[nid].assign(data + pos, did_len);
    pos += did_len;
    if (!new_id_to_node.emplace(new_doc_ids[nid], nid).second) {
      return false;  // duplicate doc_id
    }

    int32_t layer_cap = 0;
    if (!ReadPod(data, len, pos, layer_cap) || layer_cap < 0 || layer_cap > max_level) {
      return false;
    }
    new_max_layers[nid] = layer_cap;

    if (quantization_ == VectorQuantization::kSq8) {
      float mv = 0.0f, sc = 0.0f;
      if (!ReadPod(data, len, pos, mv) || !ReadPod(data, len, pos, sc)) return false;
      if (!std::isfinite(mv) || !std::isfinite(sc) || sc < 0.0f) return false;
      new_sq8_meta[nid] = Sq8Meta{mv, sc};
      if (!ReadBytes(data, len, pos,
                     new_sq8_vectors.data() + static_cast<size_t>(nid) * dim_,
                     dim_ * sizeof(uint8_t))) {
        return false;
      }
    } else if (quantization_ == VectorQuantization::kBq) {
      if (!ReadBytes(data, len, pos,
                     new_bq_vectors.data() + static_cast<size_t>(nid) * bq_words_,
                     bq_words_ * sizeof(uint64_t))) {
        return false;
      }
      if (rerank_ == VectorRerankType::kFp16) {
        if (!ReadBytes(data, len, pos,
                       new_fp16_vectors.data() + static_cast<size_t>(nid) * dim_,
                       dim_ * sizeof(uint16_t))) {
          return false;
        }
      } else if (rerank_ == VectorRerankType::kSq8) {
        float mv = 0.0f, sc = 0.0f;
        if (!ReadPod(data, len, pos, mv) || !ReadPod(data, len, pos, sc)) return false;
        if (!std::isfinite(mv) || !std::isfinite(sc) || sc < 0.0f) return false;
        new_sq8_meta[nid] = Sq8Meta{mv, sc};
        if (!ReadBytes(data, len, pos,
                       new_sq8_vectors.data() + static_cast<size_t>(nid) * dim_,
                       dim_ * sizeof(uint8_t))) {
          return false;
        }
      }
    } else {
      if (!ReadBytes(data, len, pos,
                     new_vectors.data() + static_cast<size_t>(nid) * dim_,
                     dim_ * sizeof(float))) {
        return false;
      }
    }

    auto& node_layers = new_neighbors[nid];
    node_layers.resize(static_cast<size_t>(layer_cap + 1));
    for (int32_t lc = 0; lc <= layer_cap; lc++) {
      uint32_t deg = 0;
      if (!ReadPod(data, len, pos, deg)) return false;
      uint32_t max_conn = (lc == 0) ? m_max0 : m;
      if (deg > max_conn || deg > (len - pos) / sizeof(uint32_t)) return false;
      auto& nb_list = node_layers[static_cast<size_t>(lc)];
      nb_list.resize(deg);
      if (deg > 0) {
        if (!ReadBytes(data, len, pos, nb_list.data(), deg * sizeof(uint32_t))) {
          return false;
        }
        for (uint32_t nb : nb_list) {
          if (nb >= num_nodes || nb == nid) return false;
        }
      }
    }
  }

  if (pos != len) return false;

  // Commit atomically after full validation
  bq_oversample_ = bq_os;
  m_ = m;
  m_max0_ = m_max0;
  ef_construction_ = ef_c;
  ef_runtime_ = ef_r;
  ml_ = 1.0 / std::log(static_cast<double>(m_ > 1 ? m_ : 2));
  entry_point_ = entry_point;
  max_level_ = max_level;
  live_count_ = num_nodes;
  vectors_ = std::move(new_vectors);
  sq8_vectors_ = std::move(new_sq8_vectors);
  sq8_meta_ = std::move(new_sq8_meta);
  fp16_vectors_ = std::move(new_fp16_vectors);
  bq_vectors_ = std::move(new_bq_vectors);
  doc_ids_ = std::move(new_doc_ids);
  alive_ = std::move(new_alive);
  max_layers_ = std::move(new_max_layers);
  neighbors_ = std::move(new_neighbors);
  free_slots_.clear();
  id_to_node_ = std::move(new_id_to_node);
  return true;
}
