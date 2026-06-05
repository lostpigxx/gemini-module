#include "hnsw_index.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <random>
#include <unordered_set>

HnswIndex::HnswIndex(size_t dim, DistanceMetric metric, size_t m,
                      size_t ef_construction)
    : dim_(dim),
      metric_(metric),
      dist_fn_(GetDistanceFunction(metric)),
      m_(m),
      m_max0_(m * 2),
      ef_construction_(ef_construction),
      ef_runtime_(10),
      ml_(1.0 / std::log(static_cast<double>(m))),
      max_level_(-1) {}

int HnswIndex::RandomLevel(const std::string& doc_id) const {
  auto seed = static_cast<unsigned>(std::hash<std::string>{}(doc_id));
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  double r = dist(rng);
  if (r == 0.0) r = 1e-10;
  int level = static_cast<int>(std::floor(-std::log(r) * ml_));
  return std::min(level, 32);
}

float HnswIndex::DistanceTo(const float* a, const std::string& b_id) const {
  auto it = nodes_.find(b_id);
  if (it == nodes_.end()) return std::numeric_limits<float>::max();
  return dist_fn_(a, it->second.vector.data(), dim_);
}

std::vector<std::string> HnswIndex::SearchLayer(const float* query,
                                                  const std::string& entry,
                                                  size_t ef,
                                                  int layer) const {
  using Pair = std::pair<float, std::string>;
  auto cmp_min = [](const Pair& a, const Pair& b) { return a.first > b.first; };
  auto cmp_max = [](const Pair& a, const Pair& b) { return a.first < b.first; };

  std::priority_queue<Pair, std::vector<Pair>, decltype(cmp_min)> candidates(cmp_min);
  std::priority_queue<Pair, std::vector<Pair>, decltype(cmp_max)> result(cmp_max);
  std::unordered_set<std::string> visited;

  float entry_dist = DistanceTo(query, entry);
  candidates.push({entry_dist, entry});
  result.push({entry_dist, entry});
  visited.insert(entry);

  while (!candidates.empty()) {
    auto [c_dist, c_id] = candidates.top();
    candidates.pop();

    float worst_result = result.top().first;
    if (c_dist > worst_result) break;

    auto node_it = nodes_.find(c_id);
    if (node_it == nodes_.end()) continue;
    auto& node = node_it->second;

    if (layer >= static_cast<int>(node.neighbors.size())) continue;
    for (auto& neighbor_id : node.neighbors[layer]) {
      if (visited.count(neighbor_id)) continue;
      visited.insert(neighbor_id);

      float n_dist = DistanceTo(query, neighbor_id);
      worst_result = result.top().first;

      if (n_dist < worst_result || result.size() < ef) {
        candidates.push({n_dist, neighbor_id});
        result.push({n_dist, neighbor_id});
        if (result.size() > ef) result.pop();
      }
    }
  }

  std::vector<std::string> out;
  out.reserve(result.size());
  while (!result.empty()) {
    out.push_back(result.top().second);
    result.pop();
  }
  std::reverse(out.begin(), out.end());
  return out;
}

std::vector<std::string> HnswIndex::SelectNeighbors(
    const float* query, const std::vector<std::string>& candidates,
    size_t max_neighbors) const {
  std::vector<std::pair<float, std::string>> scored;
  scored.reserve(candidates.size());
  for (auto& id : candidates) {
    scored.push_back({DistanceTo(query, id), id});
  }
  std::sort(scored.begin(), scored.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  std::vector<std::string> result;
  size_t n = std::min(max_neighbors, scored.size());
  result.reserve(n);
  for (size_t i = 0; i < n; i++) {
    result.push_back(scored[i].second);
  }
  return result;
}

void HnswIndex::Add(const std::string& doc_id, const float* data) {
  Remove(doc_id);

  int level = RandomLevel(doc_id);
  HnswNode node;
  node.vector.assign(data, data + dim_);
  node.max_layer = level;
  node.neighbors.resize(level + 1);
  nodes_[doc_id] = std::move(node);

  if (max_level_ < 0) {
    entry_point_ = doc_id;
    max_level_ = level;
    return;
  }

  std::string cur_entry = entry_point_;

  for (int lc = max_level_; lc > level; lc--) {
    auto nearest = SearchLayer(data, cur_entry, 1, lc);
    if (!nearest.empty()) cur_entry = nearest[0];
  }

  for (int lc = std::min(level, max_level_); lc >= 0; lc--) {
    auto candidates = SearchLayer(data, cur_entry, ef_construction_, lc);
    size_t max_conn = (lc == 0) ? m_max0_ : m_;
    auto neighbors = SelectNeighbors(data, candidates, max_conn);

    nodes_[doc_id].neighbors[lc] = neighbors;

    for (auto& nb_id : neighbors) {
      auto nb_it = nodes_.find(nb_id);
      if (nb_it == nodes_.end()) continue;
      auto& nb_node = nb_it->second;
      if (lc >= static_cast<int>(nb_node.neighbors.size())) continue;

      nb_node.neighbors[lc].push_back(doc_id);

      if (nb_node.neighbors[lc].size() > max_conn) {
        nb_node.neighbors[lc] = SelectNeighbors(
            nb_node.vector.data(), nb_node.neighbors[lc], max_conn);
      }
    }

    if (!candidates.empty()) cur_entry = candidates[0];
  }

  if (level > max_level_) {
    entry_point_ = doc_id;
    max_level_ = level;
  }
}

bool HnswIndex::Remove(const std::string& doc_id) {
  auto it = nodes_.find(doc_id);
  if (it == nodes_.end()) return false;

  auto& node = it->second;
  for (int lc = 0; lc <= node.max_layer; lc++) {
    if (lc >= static_cast<int>(node.neighbors.size())) break;
    for (auto& nb_id : node.neighbors[lc]) {
      auto nb_it = nodes_.find(nb_id);
      if (nb_it == nodes_.end()) continue;
      if (lc >= static_cast<int>(nb_it->second.neighbors.size())) continue;
      auto& nb_list = nb_it->second.neighbors[lc];
      nb_list.erase(std::remove(nb_list.begin(), nb_list.end(), doc_id),
                    nb_list.end());
    }
  }

  bool was_entry = (doc_id == entry_point_);
  nodes_.erase(it);

  if (nodes_.empty()) {
    entry_point_.clear();
    max_level_ = -1;
    return true;
  }

  if (was_entry) {
    for (int lc = max_level_; lc >= 0; lc--) {
      for (auto& [id, n] : nodes_) {
        if (n.max_layer >= lc) {
          entry_point_ = id;
          max_level_ = n.max_layer;
          goto found_entry;
        }
      }
    }
    found_entry:;

    while (max_level_ > 0) {
      bool has_node = false;
      for (auto& [id, n] : nodes_) {
        if (n.max_layer >= max_level_) { has_node = true; break; }
      }
      if (has_node) break;
      max_level_--;
    }
  }

  return true;
}

std::vector<KnnResult> HnswIndex::KnnQuery(const float* query,
                                            size_t k) const {
  if (nodes_.empty()) return {};

  std::string cur = entry_point_;
  for (int lc = max_level_; lc > 0; lc--) {
    auto nearest = SearchLayer(query, cur, 1, lc);
    if (!nearest.empty()) cur = nearest[0];
  }

  size_t ef = std::max(ef_runtime_, k);
  auto candidates = SearchLayer(query, cur, ef, 0);

  std::vector<KnnResult> results;
  results.reserve(candidates.size());
  for (auto& id : candidates) {
    results.push_back({id, DistanceTo(query, id)});
  }
  std::sort(results.begin(), results.end(),
            [](const KnnResult& a, const KnnResult& b) {
              return a.score < b.score;
            });
  if (results.size() > k) results.resize(k);
  return results;
}

std::vector<KnnResult> HnswIndex::KnnQueryFiltered(
    const float* query, size_t k,
    const std::vector<std::string>& candidates) const {
  std::vector<KnnResult> results;
  for (auto& doc_id : candidates) {
    auto it = nodes_.find(doc_id);
    if (it == nodes_.end()) continue;
    float score = dist_fn_(query, it->second.vector.data(), dim_);
    results.push_back({doc_id, score});
  }
  size_t n = std::min(k, results.size());
  std::partial_sort(results.begin(), results.begin() + static_cast<long>(n),
                    results.end(),
                    [](const KnnResult& a, const KnnResult& b) {
                      return a.score < b.score;
                    });
  results.resize(n);
  return results;
}

size_t HnswIndex::Size() const { return nodes_.size(); }
size_t HnswIndex::Dim() const { return dim_; }
DistanceMetric HnswIndex::Metric() const { return metric_; }
size_t HnswIndex::M() const { return m_; }
size_t HnswIndex::EfConstruction() const { return ef_construction_; }
size_t HnswIndex::EfRuntime() const { return ef_runtime_; }
void HnswIndex::SetEfRuntime(size_t ef) { ef_runtime_ = ef; }

void HnswIndex::Clear() {
  nodes_.clear();
  entry_point_.clear();
  max_level_ = -1;
}
