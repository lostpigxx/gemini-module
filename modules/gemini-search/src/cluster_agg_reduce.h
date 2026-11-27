#pragma once

#include "expr_engine.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gemini_search {

enum class ReduceFunc : uint8_t {
  kCount = 0,
  kSum = 1,
  kAvg = 2,
  kMin = 3,
  kMax = 4,
  kCountDistinct = 5,
  kStddev = 6,
  kQuantile = 7,
  kTolist = 8,
  kFirstValue = 9,
  kRandomSample = 10,
  kCountDistinctish = 11,
};

struct ReduceOp {
  ReduceFunc func = ReduceFunc::kCount;
  std::string field;
  std::string alias;
  double quantile_pct = 0.0;
  std::string first_value_by;
  bool first_value_desc = false;
  long random_sample_count = 0;
};

enum class PipelineStageType : uint8_t {
  kLoad = 0,
  kApply = 1,
  kFilter = 2,
  kGroupby = 3,
  kSortby = 4,
  kLimit = 5,
};

struct PipelineStage {
  PipelineStageType type = PipelineStageType::kLoad;
  // LOAD
  std::vector<std::string> load_fields;
  // APPLY
  std::string apply_expr;
  std::string apply_alias;
  // FILTER
  std::string filter_expr;
  // GROUPBY + REDUCE
  std::vector<std::string> groupby_fields;
  std::vector<ReduceOp> reducers;
  // SORTBY (multi-key + optional MAX)
  std::vector<std::pair<std::string, bool>> sortby_keys;
  long long sortby_max = -1;
  // LIMIT
  long long limit_offset = 0;
  long long limit_count = 0;
};

using AggRow = std::unordered_map<std::string, std::string>;

// Safety caps for single-group distinct/list accumulation during Map-Reduce
constexpr size_t kMaxPartialDistinctPerGroup = 200000;
constexpr size_t kMaxPartialListPerGroup = 200000;

// 14-bit precision HyperLogLog (16,384 registers, ~0.81% standard error, lossless max-merge)
struct HllSketch {
  static constexpr uint32_t kPrecision = 14;
  static constexpr size_t kNumRegs = 1u << kPrecision;  // 16384
  static constexpr size_t kSparseThreshold = 2048;

  // Compact sparse representation when non-zero registers <= kSparseThreshold; dense otherwise
  bool is_dense = false;
  std::vector<std::pair<uint16_t, uint8_t>> sparse;
  std::vector<uint8_t> dense;  // size == kNumRegs when is_dense == true

  void Add(const std::string& val);
  void Merge(const HllSketch& other);
  double Estimate() const;
  void Compact();  // Ensures sparse is sorted & deduplicated or promoted to dense
};

// Exact-then-centroid Quantile sketch:
// - Keeps exact sorted values when count <= kExactLimit (0.0 error vs standalone)
// - Compresses into weighted centroids (<= kMaxCentroids) for large groups (< 1% tail error)
struct QuantileSketch {
  static constexpr size_t kExactLimit = 4096;
  static constexpr size_t kMaxCentroids = 256;

  bool is_compressed = false;
  std::vector<double> exact_vals;
  // Sorted by mean: (mean, weight)
  std::vector<std::pair<double, double>> centroids;

  void Add(double v);
  void FinalizeMap();
  void Merge(const QuantileSketch& other);
  double Quantile(double pct) const;
  bool Empty() const { return exact_vals.empty() && centroids.empty(); }
};

struct PartialReducerState {
  ReduceFunc func = ReduceFunc::kCount;
  uint64_t count = 0;        // COUNT, AVG n, STDDEV n, RANDOM_SAMPLE total_seen
  bool has_val = false;      // SUM, MIN, MAX, FIRST_VALUE
  double sum = 0.0;          // SUM, AVG
  double min_val = 0.0;      // MIN
  double max_val = 0.0;      // MAX
  double mean = 0.0;         // STDDEV (Chan)
  double m2 = 0.0;           // STDDEV (Chan)
  std::string first_val;     // FIRST_VALUE
  double first_sort_val = 0.0;
  std::vector<std::string> str_vals;  // COUNT_DISTINCT, TOLIST, RANDOM_SAMPLE
  HllSketch hll;                      // COUNT_DISTINCTISH
  QuantileSketch quantile;            // QUANTILE
};

struct PartialGroup {
  std::string group_key;
  std::vector<std::pair<std::string, std::string>> key_fields;
  std::vector<PartialReducerState> reducers;
};

struct ClusterAggPlanSplit {
  enum class Mode : uint8_t {
    kRawRows = 0,
    kMapReduceGroupby = 1,
  };

  Mode mode = Mode::kRawRows;
  // Stages pushed down to shards (in kMapReduceGroupby: pre-GROUPBY LOAD/APPLY/FILTER)
  std::vector<PipelineStage> map_stages;
  // First GROUPBY stage (valid only when mode == kMapReduceGroupby)
  PipelineStage groupby_stage;
  // Stages executed on Coordinator after global merge/reduce
  std::vector<PipelineStage> reduce_stages;
  // Optional per-shard row cap in kRawRows mode (0 = unlimited unless raw_zero_limit is true)
  size_t raw_shard_limit = 0;
  // True when an explicit LIMIT ... 0 forces 0 rows in kRawRows mode
  bool raw_zero_limit = false;
  // Whether kRawRows mode pushed a SORTBY to shards
  bool raw_has_sortby = false;
  std::vector<std::pair<std::string, bool>> raw_sortby_keys;
};

// Pure C++ helpers (zero redismodule.h dependency)
bool TryParseAggDouble(const std::string& s, double& out);
void ApplyAggLimit(std::vector<AggRow>& rows, long long offset, long long count);
void ApplyAggSortby(std::vector<AggRow>& rows,
                    const std::vector<std::pair<std::string, bool>>& keys,
                    long long max_rows = -1);
void ApplyAggGroupby(std::vector<AggRow>& rows, const std::vector<std::string>& gb_fields,
                     const std::vector<ReduceOp>& reducers);
bool ExecuteAggPipelineStages(std::vector<AggRow>& rows, const std::vector<PipelineStage>& stages,
                              int64_t deadline_ms, std::string& error_out);

// ClusterAggPlanner: splits an FT.AGGREGATE pipeline into Map (shard) and Reduce (Coordinator)
ClusterAggPlanSplit SplitAggPipeline(const std::vector<PipelineStage>& stages, bool has_knn);

// Map phase (shard side): builds per-group partial reducer states with max_groups quota
bool BuildPartialGroups(const std::vector<AggRow>& rows,
                        const std::vector<std::string>& gb_fields,
                        const std::vector<ReduceOp>& reducers, size_t max_groups,
                        std::vector<PartialGroup>& out_groups, std::string& error_out);

// Reduce phase (Coordinator side): merges partial groups across all shards & finalizes into AggRows
bool MergeAndFinalizeGroups(const std::vector<std::vector<PartialGroup>>& shard_groups,
                            const std::vector<ReduceOp>& reducers, size_t max_groups,
                            std::vector<AggRow>& out_rows, std::string& error_out);

// Merges two PartialReducerState values for the same reducer (exposed for unit tests)
void MergeReducerState(PartialReducerState& dst, const PartialReducerState& src, const ReduceOp& op);
std::string FinalizeReducerState(PartialReducerState& st, const ReduceOp& op);

}  // namespace gemini_search
