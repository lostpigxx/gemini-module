#pragma once

#include "index_spec.h"

#include <cstddef>
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

class VectorIndexBase {
 public:
  virtual ~VectorIndexBase() = default;
  virtual void Add(const std::string& doc_id, const float* data) = 0;
  virtual bool Remove(const std::string& doc_id) = 0;
  virtual std::vector<KnnResult> KnnQuery(const float* query, size_t k) const = 0;
  virtual std::vector<KnnResult> KnnQueryFiltered(const float* query, size_t k,
                                                   const std::vector<std::string>& candidates) const = 0;
  virtual size_t Size() const = 0;
  virtual size_t Dim() const = 0;
  virtual DistanceMetric Metric() const = 0;
  virtual void Clear() = 0;
};

class FlatVectorIndex : public VectorIndexBase {
  size_t dim_;
  DistanceMetric metric_;
  std::unordered_map<std::string, std::vector<float>> vectors_;

 public:
  FlatVectorIndex(size_t dim, DistanceMetric metric);
  void Add(const std::string& doc_id, const float* data) override;
  bool Remove(const std::string& doc_id) override;
  std::vector<KnnResult> KnnQuery(const float* query, size_t k) const override;
  std::vector<KnnResult> KnnQueryFiltered(const float* query, size_t k,
                                           const std::vector<std::string>& candidates) const override;
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
