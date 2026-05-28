#pragma once

#include "index_spec.h"

#include <cstddef>
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

class FlatVectorIndex {
  size_t dim_;
  DistanceMetric metric_;
  std::unordered_map<std::string, std::vector<float>> vectors_;

 public:
  FlatVectorIndex(size_t dim, DistanceMetric metric);
  void Add(const std::string& doc_id, const float* data);
  bool Remove(const std::string& doc_id);
  std::vector<KnnResult> KnnQuery(const float* query, size_t k) const;
  size_t Size() const;
  size_t Dim() const;
  DistanceMetric Metric() const;
  void Clear();
};

class VectorFieldIndices {
  std::unordered_map<std::string, FlatVectorIndex> field_indices_;

 public:
  FlatVectorIndex& GetOrCreate(const std::string& field_name, size_t dim,
                                DistanceMetric metric);
  const FlatVectorIndex* Get(const std::string& field_name) const;
  void Clear();
};
