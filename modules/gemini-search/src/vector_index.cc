#include "vector_index.h"
#include "hnsw_index.h"

#include <algorithm>
#include <cmath>
#include <cstring>

float L2Distance(const float* a, const float* b, size_t dim) {
  float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
  size_t i = 0;
  for (; i + 3 < dim; i += 4) {
    float d0 = a[i] - b[i];
    float d1 = a[i + 1] - b[i + 1];
    float d2 = a[i + 2] - b[i + 2];
    float d3 = a[i + 3] - b[i + 3];
    s0 += d0 * d0;
    s1 += d1 * d1;
    s2 += d2 * d2;
    s3 += d3 * d3;
  }
  float sum = (s0 + s1) + (s2 + s3);
  for (; i < dim; i++) {
    float d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
}

float CosineDistance(const float* a, const float* b, size_t dim) {
  float dot0 = 0, dot1 = 0, dot2 = 0, dot3 = 0;
  float na0 = 0, na1 = 0, na2 = 0, na3 = 0;
  float nb0 = 0, nb1 = 0, nb2 = 0, nb3 = 0;
  size_t i = 0;
  for (; i + 3 < dim; i += 4) {
    float a0 = a[i], a1 = a[i + 1], a2 = a[i + 2], a3 = a[i + 3];
    float b0 = b[i], b1 = b[i + 1], b2 = b[i + 2], b3 = b[i + 3];
    dot0 += a0 * b0; dot1 += a1 * b1; dot2 += a2 * b2; dot3 += a3 * b3;
    na0 += a0 * a0;  na1 += a1 * a1;  na2 += a2 * a2;  na3 += a3 * a3;
    nb0 += b0 * b0;  nb1 += b1 * b1;  nb2 += b2 * b2;  nb3 += b3 * b3;
  }
  float dot = (dot0 + dot1) + (dot2 + dot3);
  float norm_a = (na0 + na1) + (na2 + na3);
  float norm_b = (nb0 + nb1) + (nb2 + nb3);
  for (; i < dim; i++) {
    dot += a[i] * b[i];
    norm_a += a[i] * a[i];
    norm_b += b[i] * b[i];
  }
  float denom = std::sqrt(norm_a * norm_b);
  if (denom == 0) return 1.0f;
  return 1.0f - dot / denom;
}

float InnerProductDistance(const float* a, const float* b, size_t dim) {
  float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
  size_t i = 0;
  for (; i + 3 < dim; i += 4) {
    s0 += a[i] * b[i];
    s1 += a[i + 1] * b[i + 1];
    s2 += a[i + 2] * b[i + 2];
    s3 += a[i + 3] * b[i + 3];
  }
  float dot = (s0 + s1) + (s2 + s3);
  for (; i < dim; i++) {
    dot += a[i] * b[i];
  }
  return -dot;
}

FlatVectorIndex::FlatVectorIndex(size_t dim, DistanceMetric metric)
    : dim_(dim), metric_(metric) {}

void FlatVectorIndex::Add(const std::string& doc_id, const float* data) {
  auto it = id_to_idx_.find(doc_id);
  if (it != id_to_idx_.end()) {
    std::memcpy(data_.data() + it->second * dim_, data, dim_ * sizeof(float));
    return;
  }
  size_t idx = ids_.size();
  id_to_idx_[doc_id] = idx;
  ids_.push_back(doc_id);
  data_.insert(data_.end(), data, data + dim_);
}

bool FlatVectorIndex::Remove(const std::string& doc_id) {
  auto it = id_to_idx_.find(doc_id);
  if (it == id_to_idx_.end()) return false;
  size_t idx = it->second;
  size_t last = ids_.size() - 1;
  if (idx != last) {
    std::memcpy(data_.data() + idx * dim_, data_.data() + last * dim_,
                dim_ * sizeof(float));
    ids_[idx] = std::move(ids_[last]);
    id_to_idx_[ids_[idx]] = idx;
  }
  ids_.pop_back();
  data_.resize(ids_.size() * dim_);
  id_to_idx_.erase(it);
  return true;
}

std::vector<KnnResult> FlatVectorIndex::KnnQuery(const float* query,
                                                  size_t k) const {
  DistFn dist_fn = GetDistanceFunction(metric_);
  const size_t total = ids_.size();
  std::vector<KnnResult> results;
  results.reserve(total);
  const float* base = data_.data();
  for (size_t idx = 0; idx < total; idx++) {
    float score = dist_fn(query, base + idx * dim_, dim_);
    results.push_back({ids_[idx], score});
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
  DistFn dist_fn = GetDistanceFunction(metric_);
  std::vector<KnnResult> results;
  const float* base = data_.data();
  for (const auto& doc_id : candidates) {
    auto it = id_to_idx_.find(doc_id);
    if (it == id_to_idx_.end()) continue;
    float score = dist_fn(query, base + it->second * dim_, dim_);
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

std::vector<KnnResult> FlatVectorIndex::RangeQuery(const float* query,
                                                    float radius) const {
  DistFn dist_fn = GetDistanceFunction(metric_);
  const size_t total = ids_.size();
  std::vector<KnnResult> results;
  const float* base = data_.data();
  for (size_t idx = 0; idx < total; idx++) {
    float score = dist_fn(query, base + idx * dim_, dim_);
    if (score <= radius) {
      results.push_back({ids_[idx], score});
    }
  }
  std::sort(results.begin(), results.end(),
            [](const KnnResult& a, const KnnResult& b) {
              if (a.score != b.score) return a.score < b.score;
              return a.doc_id < b.doc_id;
            });
  return results;
}

size_t FlatVectorIndex::Size() const { return ids_.size(); }
size_t FlatVectorIndex::Dim() const { return dim_; }
DistanceMetric FlatVectorIndex::Metric() const { return metric_; }
void FlatVectorIndex::Clear() {
  data_.clear();
  ids_.clear();
  id_to_idx_.clear();
}

bool DecodeVectorBlob(const std::string& blob, size_t dim,
                      VectorDataType dtype, std::vector<float>& out) {
  if (dim == 0) return false;
  if (dtype == VectorDataType::kFloat64) {
    if (blob.size() != dim * sizeof(double)) return false;
    out.resize(dim);
    for (size_t i = 0; i < dim; i++) {
      double d = 0.0;
      std::memcpy(&d, blob.data() + i * sizeof(double), sizeof(double));
      out[i] = static_cast<float>(d);
    }
    return true;
  }
  if (blob.size() != dim * sizeof(float)) return false;
  out.resize(dim);
  std::memcpy(out.data(), blob.data(), blob.size());
  return true;
}

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
