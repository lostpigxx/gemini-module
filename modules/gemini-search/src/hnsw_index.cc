#include "hnsw_index.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <random>

HnswIndex::HnswIndex(size_t dim, DistanceMetric metric, size_t m,
                     size_t ef_construction, VectorQuantization quantization)
    : dim_(dim),
      metric_(metric),
      quantization_(quantization),
      dist_fn_(GetDistanceFunction(metric)),
      m_(m),
      m_max0_(m * 2),
      ef_construction_(ef_construction),
      ef_runtime_(10),
      ml_(1.0 / std::log(static_cast<double>(m > 1 ? m : 2))) {}

int HnswIndex::RandomLevel(const std::string& doc_id) const {
  auto seed = static_cast<unsigned>(std::hash<std::string>{}(doc_id));
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  double r = dist(rng);
  if (r == 0.0) r = 1e-10;
  int level = static_cast<int>(std::floor(-std::log(r) * ml_));
  return std::min(level, 32);
}

float HnswIndex::DistanceTo(const float* query, uint32_t node_id) const {
  if (node_id >= alive_.size() || !alive_[node_id]) {
    return std::numeric_limits<float>::max();
  }
  if (quantization_ == VectorQuantization::kSq8) {
    return Sq8Distance(query, sq8_vectors_.data() + static_cast<size_t>(node_id) * dim_,
                       sq8_meta_[node_id], dim_, metric_);
  }
  return dist_fn_(query, vectors_.data() + static_cast<size_t>(node_id) * dim_, dim_);
}

uint32_t HnswIndex::NextEpoch() const {
  if (visited_epoch_.size() < alive_.size()) {
    visited_epoch_.resize(alive_.size(), 0);
  }
  if (current_epoch_ == std::numeric_limits<uint32_t>::max()) {
    std::fill(visited_epoch_.begin(), visited_epoch_.end(), 0);
    current_epoch_ = 1;
  } else {
    current_epoch_++;
  }
  return current_epoch_;
}

std::vector<uint32_t> HnswIndex::SearchLayer(const float* query, uint32_t entry,
                                             size_t ef, int layer) const {
  using Pair = std::pair<float, uint32_t>;
  auto cmp_min = [](const Pair& a, const Pair& b) { return a.first > b.first; };
  auto cmp_max = [](const Pair& a, const Pair& b) { return a.first < b.first; };

  std::priority_queue<Pair, std::vector<Pair>, decltype(cmp_min)> candidates(cmp_min);
  std::priority_queue<Pair, std::vector<Pair>, decltype(cmp_max)> result(cmp_max);

  const uint32_t epoch = NextEpoch();
  float entry_dist = DistanceTo(query, entry);
  candidates.push({entry_dist, entry});
  result.push({entry_dist, entry});
  visited_epoch_[entry] = epoch;

  while (!candidates.empty()) {
    auto [c_dist, c_id] = candidates.top();
    candidates.pop();

    float worst_result = result.top().first;
    if (c_dist > worst_result) break;

    if (layer >= static_cast<int>(neighbors_[c_id].size())) continue;
    const auto& nb_list = neighbors_[c_id][static_cast<size_t>(layer)];
    for (uint32_t nb_id : nb_list) {
      if (visited_epoch_[nb_id] == epoch) continue;
      visited_epoch_[nb_id] = epoch;
      if (!alive_[nb_id]) continue;

      float n_dist = DistanceTo(query, nb_id);
      worst_result = result.top().first;

      if (n_dist < worst_result || result.size() < ef) {
        candidates.push({n_dist, nb_id});
        result.push({n_dist, nb_id});
        if (result.size() > ef) result.pop();
      }
    }
  }

  std::vector<uint32_t> out;
  out.reserve(result.size());
  while (!result.empty()) {
    out.push_back(result.top().second);
    result.pop();
  }
  std::reverse(out.begin(), out.end());
  return out;
}

std::vector<uint32_t> HnswIndex::SelectNeighbors(
    const float* query, const std::vector<uint32_t>& candidates,
    size_t max_neighbors) const {
  std::vector<std::pair<float, uint32_t>> scored;
  scored.reserve(candidates.size());
  for (uint32_t id : candidates) {
    if (id < alive_.size() && alive_[id]) {
      scored.push_back({DistanceTo(query, id), id});
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

  for (int lc = max_level_; lc > level; lc--) {
    auto nearest = SearchLayer(data, cur_entry, 1, lc);
    if (!nearest.empty()) cur_entry = nearest[0];
  }

  std::vector<float> tmp_nb_vec;
  for (int lc = std::min(level, max_level_); lc >= 0; lc--) {
    auto candidates = SearchLayer(data, cur_entry, ef_construction_, lc);
    size_t max_conn = (lc == 0) ? m_max0_ : m_;
    auto chosen = SelectNeighbors(data, candidates, max_conn);

    neighbors_[nid][static_cast<size_t>(lc)] = chosen;

    for (uint32_t nb_id : chosen) {
      if (!alive_[nb_id] || lc >= static_cast<int>(neighbors_[nb_id].size())) continue;
      auto& nb_list = neighbors_[nb_id][static_cast<size_t>(lc)];
      nb_list.push_back(nid);

      if (nb_list.size() > max_conn) {
        const float* nb_vec_ptr = nullptr;
        if (quantization_ == VectorQuantization::kSq8) {
          tmp_nb_vec.resize(dim_);
          DequantizeSq8(sq8_vectors_.data() + static_cast<size_t>(nb_id) * dim_,
                        sq8_meta_[nb_id], dim_, tmp_nb_vec.data());
          nb_vec_ptr = tmp_nb_vec.data();
        } else {
          nb_vec_ptr = vectors_.data() + static_cast<size_t>(nb_id) * dim_;
        }
        nb_list = SelectNeighbors(nb_vec_ptr, nb_list, max_conn);
      }
    }

    if (!candidates.empty()) cur_entry = candidates[0];
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
  return KnnQueryEf(query, k, ef_runtime_);
}

std::vector<KnnResult> HnswIndex::KnnQueryEf(const float* query, size_t k,
                                              size_t ef_runtime) const {
  if (live_count_ == 0 || entry_point_ == kInvalidNode) return {};

  uint32_t cur = entry_point_;
  for (int lc = max_level_; lc > 0; lc--) {
    auto nearest = SearchLayer(query, cur, 1, lc);
    if (!nearest.empty()) cur = nearest[0];
  }

  size_t ef = std::max(ef_runtime > 0 ? ef_runtime : ef_runtime_, k);
  auto candidates = SearchLayer(query, cur, ef, 0);

  std::vector<KnnResult> results;
  results.reserve(candidates.size());
  for (uint32_t nid : candidates) {
    if (alive_[nid]) {
      results.push_back({doc_ids_[nid], DistanceTo(query, nid)});
    }
  }
  std::sort(results.begin(), results.end(),
            [](const KnnResult& a, const KnnResult& b) {
              return a.score < b.score;
            });
  if (results.size() > k) results.resize(k);
  return results;
}

std::vector<KnnResult> HnswIndex::RangeQuery(const float* query,
                                              float radius) const {
  std::vector<KnnResult> results;
  for (uint32_t i = 0; i < alive_.size(); i++) {
    if (!alive_[i]) continue;
    float score = DistanceTo(query, i);
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
    const std::vector<std::string>& candidates) const {
  std::vector<KnnResult> results;
  results.reserve(candidates.size());
  for (const auto& doc_id : candidates) {
    auto it = id_to_node_.find(doc_id);
    if (it == id_to_node_.end()) continue;
    results.push_back({doc_id, DistanceTo(query, it->second)});
  }
  size_t n = std::min(k, results.size());
  std::partial_sort(results.begin(), results.begin() + static_cast<ptrdiff_t>(n),
                    results.end(),
                    [](const KnnResult& a, const KnnResult& b) {
                      return a.score < b.score;
                    });
  results.resize(n);
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
  doc_ids_.clear();
  alive_.clear();
  max_layers_.clear();
  neighbors_.clear();
  free_slots_.clear();
  id_to_node_.clear();
  visited_epoch_.clear();
  current_epoch_ = 0;
  entry_point_ = kInvalidNode;
  max_level_ = -1;
  live_count_ = 0;
}
