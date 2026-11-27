#pragma once

#include "vector_index.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

class HnswIndex : public VectorIndexBase {
 public:
  HnswIndex(size_t dim, DistanceMetric metric,
            size_t m = 16, size_t ef_construction = 200,
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
  bool Contains(const std::string& doc_id) const { return id_to_node_.count(doc_id) > 0; }
  size_t Dim() const override;
  DistanceMetric Metric() const override;
  void Clear() override;

  size_t M() const;
  size_t EfConstruction() const;
  size_t EfRuntime() const;
  void SetEfRuntime(size_t ef);
  VectorQuantization Quantization() const { return quantization_; }
  VectorRerankType Rerank() const { return rerank_; }
  size_t BqOversample() const { return bq_oversample_; }

  // Direct binary serialization/deserialization of the compacted HNSW graph
  // (avoids O(N log N) graph reconstruction on RDB reload).
  std::string SerializeGraph() const;
  bool DeserializeGraph(const char* data, size_t len);

 private:
  static constexpr uint32_t kInvalidNode = std::numeric_limits<uint32_t>::max();

  size_t dim_;
  DistanceMetric metric_;
  VectorQuantization quantization_;
  VectorRerankType rerank_;
  size_t bq_oversample_;
  DistFn dist_fn_;
  size_t m_;
  size_t m_max0_;
  size_t ef_construction_;
  size_t ef_runtime_;
  double ml_;

  uint32_t entry_point_ = kInvalidNode;
  int max_level_ = -1;
  size_t live_count_ = 0;

  // Dense node storage indexed by uint32_t node_id
  std::vector<float> vectors_;
  std::vector<uint8_t> sq8_vectors_;
  std::vector<Sq8Meta> sq8_meta_;
  std::vector<uint16_t> fp16_vectors_;
  size_t bq_words_ = 0;
  std::vector<uint64_t> bq_vectors_;
  std::vector<std::string> doc_ids_;
  std::vector<uint8_t> alive_;
  std::vector<int> max_layers_;
  std::vector<std::vector<std::vector<uint32_t>>> neighbors_;
  std::vector<uint32_t> free_slots_;
  std::unordered_map<std::string, uint32_t> id_to_node_;

  int RandomLevel(const std::string& doc_id) const;
  float DistanceTo(const float* query, const uint64_t* query_bq, uint32_t node_id) const;
  float RerankDistanceTo(const float* query, uint32_t node_id) const;

  uint32_t GreedySearchLayer(const float* query, const uint64_t* query_bq,
                             uint32_t entry, int layer) const;
  std::vector<std::pair<float, uint32_t>> SearchLayer(
      const float* query, const uint64_t* query_bq, uint32_t entry,
      size_t ef, int layer) const;
  std::vector<uint32_t> SelectNeighbors(const float* query, const uint64_t* query_bq,
                                        const std::vector<uint32_t>& candidates,
                                        size_t max_neighbors) const;
};
