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

float Float16ToFloat32(uint16_t h) {
  uint32_t sign = static_cast<uint32_t>(h >> 15) << 31;
  uint32_t exp = (h >> 10) & 0x1Fu;
  uint32_t mant = h & 0x3FFu;
  uint32_t bits = 0;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign;
    } else {
      // Subnormal
      exp = 1;
      while ((mant & 0x400u) == 0) {
        mant <<= 1;
        exp--;
      }
      mant &= 0x3FFu;
      bits = sign | ((exp + 112u) << 23) | (mant << 13);
    }
  } else if (exp == 31u) {
    bits = sign | 0x7F800000u | (mant << 13);
  } else {
    bits = sign | ((exp + 112u) << 23) | (mant << 13);
  }
  float out = 0.0f;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

uint16_t Float32ToFloat16(float f) {
  uint32_t bits = 0;
  std::memcpy(&bits, &f, sizeof(bits));
  uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000u);
  int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
  uint32_t mant = bits & 0x7FFFFFu;
  if (exp <= 0) {
    if (exp < -10) return sign;
    mant |= 0x800000u;
    uint32_t shift = static_cast<uint32_t>(14 - exp);
    return static_cast<uint16_t>(sign | (mant >> shift));
  }
  if (exp >= 31) {
    if (((bits >> 23) & 0xFFu) == 0xFFu && mant != 0) {
      return static_cast<uint16_t>(sign | 0x7E00u);  // NaN
    }
    return static_cast<uint16_t>(sign | 0x7C00u);  // Inf
  }
  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) |
                               ((mant + 0x1000u) >> 13));
}

float BFloat16ToFloat32(uint16_t bf) {
  uint32_t bits = static_cast<uint32_t>(bf) << 16;
  float out = 0.0f;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

uint16_t Float32ToBFloat16(float f) {
  uint32_t bits = 0;
  std::memcpy(&bits, &f, sizeof(bits));
  // Round to nearest even
  uint32_t lsb = (bits >> 16) & 1u;
  bits += 0x7FFFu + lsb;
  return static_cast<uint16_t>(bits >> 16);
}

void QuantizeSq8(const float* src, size_t dim, uint8_t* dst, Sq8Meta& meta) {
  if (dim == 0) {
    meta = {0.0f, 0.0f};
    return;
  }
  float mn = src[0], mx = src[0];
  for (size_t i = 1; i < dim; i++) {
    if (src[i] < mn) mn = src[i];
    if (src[i] > mx) mx = src[i];
  }
  meta.min_val = mn;
  float range = mx - mn;
  if (range <= 1e-12f) {
    meta.scale = 0.0f;
    std::memset(dst, 0, dim);
    return;
  }
  meta.scale = range / 255.0f;
  float inv = 255.0f / range;
  for (size_t i = 0; i < dim; i++) {
    int q = static_cast<int>(std::lround((src[i] - mn) * inv));
    if (q < 0) q = 0;
    if (q > 255) q = 255;
    dst[i] = static_cast<uint8_t>(q);
  }
}

void DequantizeSq8(const uint8_t* src, const Sq8Meta& meta, size_t dim, float* dst) {
  for (size_t i = 0; i < dim; i++) {
    dst[i] = meta.min_val + static_cast<float>(src[i]) * meta.scale;
  }
}

float Sq8Distance(const float* query, const uint8_t* qvec, const Sq8Meta& meta,
                  size_t dim, DistanceMetric metric) {
  const float mn = meta.min_val;
  const float sc = meta.scale;
  if (metric == DistanceMetric::kL2) {
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    size_t i = 0;
    for (; i + 3 < dim; i += 4) {
      float d0 = query[i] - (mn + static_cast<float>(qvec[i]) * sc);
      float d1 = query[i + 1] - (mn + static_cast<float>(qvec[i + 1]) * sc);
      float d2 = query[i + 2] - (mn + static_cast<float>(qvec[i + 2]) * sc);
      float d3 = query[i + 3] - (mn + static_cast<float>(qvec[i + 3]) * sc);
      s0 += d0 * d0; s1 += d1 * d1; s2 += d2 * d2; s3 += d3 * d3;
    }
    float sum = (s0 + s1) + (s2 + s3);
    for (; i < dim; i++) {
      float d = query[i] - (mn + static_cast<float>(qvec[i]) * sc);
      sum += d * d;
    }
    return sum;
  }
  if (metric == DistanceMetric::kIP) {
    float sum = 0.0f;
    for (size_t i = 0; i < dim; i++) {
      sum += query[i] * (mn + static_cast<float>(qvec[i]) * sc);
    }
    return -sum;
  }
  // Cosine
  float dot = 0.0f, na = 0.0f, nb = 0.0f;
  for (size_t i = 0; i < dim; i++) {
    float a = query[i];
    float b = mn + static_cast<float>(qvec[i]) * sc;
    dot += a * b;
    na += a * a;
    nb += b * b;
  }
  float denom = std::sqrt(na * nb);
  if (denom == 0.0f) return 1.0f;
  return 1.0f - dot / denom;
}

FlatVectorIndex::FlatVectorIndex(size_t dim, DistanceMetric metric,
                                 VectorQuantization quantization)
    : dim_(dim), metric_(metric), quantization_(quantization) {}

float FlatVectorIndex::DistanceAt(const float* query, size_t idx, DistFn dist_fn) const {
  if (quantization_ == VectorQuantization::kSq8) {
    return Sq8Distance(query, sq8_data_.data() + idx * dim_, sq8_meta_[idx],
                       dim_, metric_);
  }
  return dist_fn(query, data_.data() + idx * dim_, dim_);
}

void FlatVectorIndex::Add(const std::string& doc_id, const float* data) {
  auto it = id_to_idx_.find(doc_id);
  if (it != id_to_idx_.end()) {
    size_t idx = it->second;
    if (quantization_ == VectorQuantization::kSq8) {
      QuantizeSq8(data, dim_, sq8_data_.data() + idx * dim_, sq8_meta_[idx]);
    } else {
      std::memcpy(data_.data() + idx * dim_, data, dim_ * sizeof(float));
    }
    return;
  }
  size_t idx = ids_.size();
  id_to_idx_[doc_id] = idx;
  ids_.push_back(doc_id);
  if (quantization_ == VectorQuantization::kSq8) {
    sq8_data_.resize((idx + 1) * dim_);
    sq8_meta_.emplace_back();
    QuantizeSq8(data, dim_, sq8_data_.data() + idx * dim_, sq8_meta_[idx]);
  } else {
    data_.insert(data_.end(), data, data + dim_);
  }
}

bool FlatVectorIndex::Remove(const std::string& doc_id) {
  auto it = id_to_idx_.find(doc_id);
  if (it == id_to_idx_.end()) return false;
  size_t idx = it->second;
  size_t last = ids_.size() - 1;
  if (idx != last) {
    if (quantization_ == VectorQuantization::kSq8) {
      std::memcpy(sq8_data_.data() + idx * dim_, sq8_data_.data() + last * dim_, dim_);
      sq8_meta_[idx] = sq8_meta_[last];
    } else {
      std::memcpy(data_.data() + idx * dim_, data_.data() + last * dim_,
                  dim_ * sizeof(float));
    }
    ids_[idx] = std::move(ids_[last]);
    id_to_idx_[ids_[idx]] = idx;
  }
  ids_.pop_back();
  if (quantization_ == VectorQuantization::kSq8) {
    sq8_data_.resize(ids_.size() * dim_);
    sq8_meta_.pop_back();
  } else {
    data_.resize(ids_.size() * dim_);
  }
  id_to_idx_.erase(it);
  return true;
}

std::vector<KnnResult> FlatVectorIndex::KnnQuery(const float* query,
                                                  size_t k) const {
  DistFn dist_fn = GetDistanceFunction(metric_);
  const size_t total = ids_.size();
  std::vector<KnnResult> results;
  results.reserve(total);
  for (size_t idx = 0; idx < total; idx++) {
    results.push_back({ids_[idx], DistanceAt(query, idx, dist_fn)});
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
  for (const auto& doc_id : candidates) {
    auto it = id_to_idx_.find(doc_id);
    if (it == id_to_idx_.end()) continue;
    results.push_back({doc_id, DistanceAt(query, it->second, dist_fn)});
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
  for (size_t idx = 0; idx < total; idx++) {
    float score = DistanceAt(query, idx, dist_fn);
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
  sq8_data_.clear();
  sq8_meta_.clear();
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
  if (dtype == VectorDataType::kFloat16) {
    if (blob.size() != dim * sizeof(uint16_t)) return false;
    out.resize(dim);
    for (size_t i = 0; i < dim; i++) {
      uint16_t h = 0;
      std::memcpy(&h, blob.data() + i * sizeof(uint16_t), sizeof(uint16_t));
      out[i] = Float16ToFloat32(h);
    }
    return true;
  }
  if (dtype == VectorDataType::kBFloat16) {
    if (blob.size() != dim * sizeof(uint16_t)) return false;
    out.resize(dim);
    for (size_t i = 0; i < dim; i++) {
      uint16_t bf = 0;
      std::memcpy(&bf, blob.data() + i * sizeof(uint16_t), sizeof(uint16_t));
      out[i] = BFloat16ToFloat32(bf);
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
                                      params.m, params.ef_construction,
                                      params.quantization);
  } else {
    idx = std::make_unique<FlatVectorIndex>(params.dim, params.metric,
                                            params.quantization);
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
