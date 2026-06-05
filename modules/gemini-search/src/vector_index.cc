#include "vector_index.h"
#include "hnsw_index.h"

#include <algorithm>
#include <cmath>
#include <cstring>

float L2Distance(const float* a, const float* b, size_t dim) {
  float sum = 0;
  for (size_t i = 0; i < dim; i++) {
    float d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
}

float CosineDistance(const float* a, const float* b, size_t dim) {
  float dot = 0, norm_a = 0, norm_b = 0;
  for (size_t i = 0; i < dim; i++) {
    dot += a[i] * b[i];
    norm_a += a[i] * a[i];
    norm_b += b[i] * b[i];
  }
  float denom = std::sqrt(norm_a) * std::sqrt(norm_b);
  if (denom == 0) return 1.0f;
  return 1.0f - dot / denom;
}

float InnerProductDistance(const float* a, const float* b, size_t dim) {
  float dot = 0;
  for (size_t i = 0; i < dim; i++) {
    dot += a[i] * b[i];
  }
  return -dot;
}

FlatVectorIndex::FlatVectorIndex(size_t dim, DistanceMetric metric)
    : dim_(dim), metric_(metric) {}

void FlatVectorIndex::Add(const std::string& doc_id, const float* data) {
  vectors_[doc_id] = std::vector<float>(data, data + dim_);
}

bool FlatVectorIndex::Remove(const std::string& doc_id) {
  return vectors_.erase(doc_id) > 0;
}

std::vector<KnnResult> FlatVectorIndex::KnnQuery(const float* query,
                                                  size_t k) const {
  using DistFn = float (*)(const float*, const float*, size_t);
  DistFn dist_fn;
  switch (metric_) {
    case DistanceMetric::kL2: dist_fn = L2Distance; break;
    case DistanceMetric::kCosine: dist_fn = CosineDistance; break;
    case DistanceMetric::kIP: dist_fn = InnerProductDistance; break;
  }

  std::vector<KnnResult> results;
  results.reserve(vectors_.size());
  for (auto& [doc_id, vec] : vectors_) {
    float score = dist_fn(query, vec.data(), dim_);
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

std::vector<KnnResult> FlatVectorIndex::KnnQueryFiltered(
    const float* query, size_t k,
    const std::vector<std::string>& candidates) const {
  using DistFn = float (*)(const float*, const float*, size_t);
  DistFn dist_fn;
  switch (metric_) {
    case DistanceMetric::kL2: dist_fn = L2Distance; break;
    case DistanceMetric::kCosine: dist_fn = CosineDistance; break;
    case DistanceMetric::kIP: dist_fn = InnerProductDistance; break;
  }

  std::vector<KnnResult> results;
  for (auto& doc_id : candidates) {
    auto it = vectors_.find(doc_id);
    if (it == vectors_.end()) continue;
    float score = dist_fn(query, it->second.data(), dim_);
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

size_t FlatVectorIndex::Size() const { return vectors_.size(); }
size_t FlatVectorIndex::Dim() const { return dim_; }
DistanceMetric FlatVectorIndex::Metric() const { return metric_; }
void FlatVectorIndex::Clear() { vectors_.clear(); }

DistFn GetDistanceFunction(DistanceMetric metric) {
  switch (metric) {
    case DistanceMetric::kL2: return L2Distance;
    case DistanceMetric::kCosine: return CosineDistance;
    case DistanceMetric::kIP: return InnerProductDistance;
  }
  return L2Distance;
}

VectorIndexBase& VectorFieldIndices::GetOrCreate(
    const std::string& field_name, const VectorFieldParams& params) {
  auto it = field_indices_.find(field_name);
  if (it != field_indices_.end()) return *it->second;

  std::unique_ptr<VectorIndexBase> idx;
  if (params.algorithm == VectorAlgorithm::kHnsw) {
    idx = std::make_unique<HnswIndex>(params.dim, params.metric,
                                      params.m, params.ef_construction);
  } else {
    idx = std::make_unique<FlatVectorIndex>(params.dim, params.metric);
  }
  auto [new_it, inserted] = field_indices_.emplace(field_name, std::move(idx));
  return *new_it->second;
}

const VectorIndexBase* VectorFieldIndices::Get(
    const std::string& field_name) const {
  auto it = field_indices_.find(field_name);
  if (it == field_indices_.end()) return nullptr;
  return it->second.get();
}

void VectorFieldIndices::Clear() { field_indices_.clear(); }
