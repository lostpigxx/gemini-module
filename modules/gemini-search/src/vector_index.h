#pragma once

#include "index_spec.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct KnnResult {
  std::string doc_id;
  float score;
};

float L2Distance(const float* a, const float* b, size_t dim);
float CosineDistance(const float* a, const float* b, size_t dim);
float InnerProductDistance(const float* a, const float* b, size_t dim);

using DistFn = float (*)(const float*, const float*, size_t);
DistFn GetDistanceFunction(DistanceMetric metric);

// Half-precision / BFloat16 conversions
float Float16ToFloat32(uint16_t h);
uint16_t Float32ToFloat16(float f);
float BFloat16ToFloat32(uint16_t bf);
uint16_t Float32ToBFloat16(float f);

// Decode a binary vector blob (FLOAT32, FLOAT64, FLOAT16, BFLOAT16) into float32 buffer.
bool DecodeVectorBlob(const std::string& blob, size_t dim,
                      VectorDataType dtype, std::vector<float>& out);

// Per-vector 8-bit scalar quantization (SQ8) helpers
struct Sq8Meta {
  float min_val = 0.0f;
  float scale = 0.0f;  // (max - min) / 255.0f
};

void QuantizeSq8(const float* src, size_t dim, uint8_t* dst, Sq8Meta& meta);
void DequantizeSq8(const uint8_t* src, const Sq8Meta& meta, size_t dim, float* dst);
float Sq8Distance(const float* query, const uint8_t* qvec, const Sq8Meta& meta,
                  size_t dim, DistanceMetric metric);

// 1-bit Binary Quantization (BQ) helpers (32x compression vs FP32)
inline size_t BqWords(size_t dim) { return (dim + 63) / 64; }
void QuantizeBq(const float* src, size_t dim, uint64_t* dst);
void DequantizeBq(const uint64_t* src, size_t dim, float* dst);
float BqHammingDistance(const uint64_t* a, const uint64_t* b, size_t dim);

// FP16 exact distance helper (used for BQ two-stage reranking)
float Fp16Distance(const float* query, const uint16_t* fp16_vec,
                   size_t dim, DistanceMetric metric);

class VectorIndexBase {
 public:
  virtual ~VectorIndexBase() = default;
  virtual void Add(const std::string& doc_id, const float* data) = 0;
  virtual bool Remove(const std::string& doc_id) = 0;
  virtual std::vector<KnnResult> KnnQuery(const float* query, size_t k) const = 0;
  virtual std::vector<KnnResult> KnnQueryEf(const float* query, size_t k,
                                            size_t /*ef_runtime*/,
                                            size_t /*oversample*/ = 0) const {
    return KnnQuery(query, k);
  }
  virtual std::vector<KnnResult> KnnQueryFiltered(
      const float* query, size_t k,
      const std::vector<std::string>& candidates,
      size_t /*ef_runtime*/ = 0,
      size_t /*oversample*/ = 0) const = 0;
  virtual std::vector<KnnResult> RangeQuery(const float* query, float radius) const = 0;
  virtual size_t Size() const = 0;
  virtual size_t Dim() const = 0;
  virtual DistanceMetric Metric() const = 0;
  virtual void Clear() = 0;
};

class FlatVectorIndex : public VectorIndexBase {
  size_t dim_;
  DistanceMetric metric_;
  VectorQuantization quantization_;
  VectorRerankType rerank_;
  size_t bq_oversample_;
  std::vector<float> data_;
  std::vector<uint8_t> sq8_data_;
  std::vector<Sq8Meta> sq8_meta_;
  std::vector<uint16_t> fp16_data_;
  size_t bq_words_ = 0;
  std::vector<uint64_t> bq_data_;
  std::vector<std::string> ids_;
  std::unordered_map<std::string, size_t> id_to_idx_;

  float DistanceAt(const float* query, size_t idx, DistFn dist_fn) const;
  float RerankDistanceAt(const float* query, size_t idx) const;

 public:
  FlatVectorIndex(size_t dim, DistanceMetric metric,
                  VectorQuantization quantization = VectorQuantization::kNone,
                  VectorRerankType rerank = VectorRerankType::kNone,
                  size_t bq_oversample = 4);
  void Add(const std::string& doc_id, const float* data) override;
  bool Remove(const std::string& doc_id) override;
  std::vector<KnnResult> KnnQuery(const float* query, size_t k) const override;
  std::vector<KnnResult> KnnQueryEf(const float* query, size_t k,
                                    size_t ef_runtime,
                                    size_t oversample = 0) const override;
  std::vector<KnnResult> KnnQueryFiltered(
      const float* query, size_t k,
      const std::vector<std::string>& candidates,
      size_t ef_runtime = 0,
      size_t oversample = 0) const override;
  std::vector<KnnResult> RangeQuery(const float* query, float radius) const override;
  size_t Size() const override;
  size_t Dim() const override;
  DistanceMetric Metric() const override;
  void Clear() override;
};

class VectorFieldIndices {
  std::unordered_map<std::string, std::unique_ptr<VectorIndexBase>> field_indices_;

 public:
  VectorIndexBase& GetOrCreate(const std::string& field_name,
                                const VectorFieldParams& params);
  const VectorIndexBase* Get(const std::string& field_name) const;
  void Clear();
};
