#pragma once

#include "vector_index.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

struct HnswNode {
  std::vector<float> vector;
  int max_layer;
  std::vector<std::vector<std::string>> neighbors;
};

class HnswIndex : public VectorIndexBase {
 public:
  HnswIndex(size_t dim, DistanceMetric metric,
            size_t m = 16, size_t ef_construction = 200);

  void Add(const std::string& doc_id, const float* data) override;
  bool Remove(const std::string& doc_id) override;
  std::vector<KnnResult> KnnQuery(const float* query, size_t k) const override;
  std::vector<KnnResult> KnnQueryFiltered(
      const float* query, size_t k,
      const std::vector<std::string>& candidates) const override;
  size_t Size() const override;
  size_t Dim() const override;
  DistanceMetric Metric() const override;
  void Clear() override;

  size_t M() const;
  size_t EfConstruction() const;
  size_t EfRuntime() const;
  void SetEfRuntime(size_t ef);

 private:
  size_t dim_;
  DistanceMetric metric_;
  DistFn dist_fn_;
  size_t m_;
  size_t m_max0_;
  size_t ef_construction_;
  size_t ef_runtime_;
  double ml_;

  std::string entry_point_;
  int max_level_;
  std::unordered_map<std::string, HnswNode> nodes_;

  int RandomLevel(const std::string& doc_id) const;
  float DistanceTo(const float* a, const std::string& b_id) const;

  std::vector<std::string> SearchLayer(const float* query,
                                        const std::string& entry, size_t ef,
                                        int layer) const;
  std::vector<std::string> SelectNeighbors(const float* query,
                                            const std::vector<std::string>& candidates,
                                            size_t max_neighbors) const;
};
