#include "cluster_agg_reduce.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <random>
#include <unordered_set>

namespace gemini_search {

namespace {

int64_t AggMonotonicMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000LL + static_cast<int64_t>(ts.tv_nsec) / 1000000LL;
}

// Fast 64-bit hash (FNV-1a 64-bit + SplitMix64 finalizer for uniform HLL bits)
uint64_t Hash64(const std::string& s) {
  uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : s) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ULL;
  }
  h ^= h >> 30;
  h *= 0xbf58476d1ce4e5b9ULL;
  h ^= h >> 27;
  h *= 0x94d049bb133111ebULL;
  h ^= h >> 31;
  return h;
}

uint8_t LeadingZerosPlusOne(uint64_t w, int bits) {
  uint8_t rho = 1;
  uint64_t mask = 1ULL << (bits - 1);
  while (rho <= bits && (w & mask) == 0) {
    rho++;
    mask >>= 1;
  }
  return rho;
}

void CompressCentroids(std::vector<std::pair<double, double>>& centroids, size_t max_centroids) {
  if (centroids.size() <= max_centroids) return;
  std::sort(centroids.begin(), centroids.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  double total_weight = 0.0;
  for (const auto& [m, w] : centroids) total_weight += w;
  if (total_weight <= 0.0) return;

  // T-Digest k1 scaling: finer resolution near tails (q -> 0 and q -> 1), coarser in middle
  std::vector<std::pair<double, double>> merged;
  merged.reserve(max_centroids);

  double delta = static_cast<double>(max_centroids);
  double cum_w = 0.0;
  double cur_mean = centroids[0].first;
  double cur_w = centroids[0].second;
  constexpr double kPi = 3.14159265358979323846;

  auto KScale = [delta](double q) -> double {
    double clamped = std::clamp(q, 0.0, 1.0);
    return (delta / (2.0 * kPi)) * std::asin(2.0 * clamped - 1.0);
  };

  double k_limit = KScale(0.0) + 1.0;

  for (size_t i = 1; i < centroids.size(); i++) {
    double next_w = centroids[i].second;
    double q_after = (cum_w + cur_w + next_w) / total_weight;
    if (KScale(q_after) <= k_limit) {
      double new_w = cur_w + next_w;
      cur_mean = (cur_mean * cur_w + centroids[i].first * next_w) / new_w;
      cur_w = new_w;
    } else {
      merged.emplace_back(cur_mean, cur_w);
      cum_w += cur_w;
      k_limit = KScale(cum_w / total_weight) + 1.0;
      cur_mean = centroids[i].first;
      cur_w = next_w;
    }
  }
  merged.emplace_back(cur_mean, cur_w);
  centroids = std::move(merged);
}

std::string MakeGroupKey(const AggRow& row, const std::vector<std::string>& gb_fields) {
  std::string key;
  for (size_t i = 0; i < gb_fields.size(); i++) {
    if (i > 0) key.push_back('\x01');
    auto fit = row.find(gb_fields[i]);
    if (fit != row.end()) key += fit->second;
  }
  return key;
}

}  // namespace

// ============================================================================
// HyperLogLog Sketch (14-bit, 16,384 registers)
// ============================================================================

void HllSketch::Add(const std::string& val) {
  uint64_t h = Hash64(val);
  uint16_t idx = static_cast<uint16_t>(h >> (64 - kPrecision));
  uint64_t rem = h & ((1ULL << (64 - kPrecision)) - 1ULL);
  uint8_t rho = LeadingZerosPlusOne(rem, 64 - static_cast<int>(kPrecision));

  if (is_dense) {
    if (rho > dense[idx]) dense[idx] = rho;
    return;
  }

  sparse.emplace_back(idx, rho);
  if (sparse.size() > kSparseThreshold * 2) {
    Compact();
  }
}

void HllSketch::Compact() {
  if (is_dense || sparse.empty()) return;
  std::sort(sparse.begin(), sparse.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first < b.first;
    return a.second > b.second;
  });
  size_t out = 0;
  for (size_t i = 0; i < sparse.size(); i++) {
    if (out == 0 || sparse[i].first != sparse[out - 1].first) {
      sparse[out++] = sparse[i];
    }
  }
  sparse.resize(out);

  if (sparse.size() > kSparseThreshold) {
    dense.assign(kNumRegs, 0);
    for (const auto& [idx, rho] : sparse) {
      if (idx < kNumRegs && rho > dense[idx]) dense[idx] = rho;
    }
    sparse.clear();
    sparse.shrink_to_fit();
    is_dense = true;
  }
}

void HllSketch::Merge(const HllSketch& other) {
  if (other.is_dense) {
    if (!is_dense) {
      dense.assign(kNumRegs, 0);
      for (const auto& [idx, rho] : sparse) {
        if (idx < kNumRegs && rho > dense[idx]) dense[idx] = rho;
      }
      sparse.clear();
      is_dense = true;
    }
    if (other.dense.size() == kNumRegs) {
      for (size_t i = 0; i < kNumRegs; i++) {
        if (other.dense[i] > dense[i]) dense[i] = other.dense[i];
      }
    }
    return;
  }

  if (is_dense) {
    for (const auto& [idx, rho] : other.sparse) {
      if (idx < kNumRegs && rho > dense[idx]) dense[idx] = rho;
    }
    return;
  }

  sparse.insert(sparse.end(), other.sparse.begin(), other.sparse.end());
  if (sparse.size() > kSparseThreshold) {
    Compact();
  }
}

double HllSketch::Estimate() const {
  double m = static_cast<double>(kNumRegs);
  double alpha_m = 0.7213 / (1.0 + 1.079 / m);

  double inv_sum = 0.0;
  size_t zero_regs = 0;

  if (is_dense && dense.size() == kNumRegs) {
    for (size_t i = 0; i < kNumRegs; i++) {
      uint8_t r = dense[i];
      if (r == 0) zero_regs++;
      inv_sum += std::ldexp(1.0, -static_cast<int>(r));
    }
  } else {
    // Deduplicate sparse on-the-fly into a temp map if not compacted
    std::unordered_map<uint16_t, uint8_t> regs;
    regs.reserve(sparse.size());
    for (const auto& [idx, rho] : sparse) {
      if (idx >= kNumRegs) continue;
      auto it = regs.find(idx);
      if (it == regs.end() || rho > it->second) regs[idx] = rho;
    }
    zero_regs = kNumRegs - regs.size();
    inv_sum = static_cast<double>(zero_regs);
    for (const auto& [idx, rho] : regs) {
      inv_sum += std::ldexp(1.0, -static_cast<int>(rho));
    }
  }

  double raw_est = alpha_m * m * m / inv_sum;
  // Small range correction (Linear Counting)
  if (raw_est <= 2.5 * m && zero_regs > 0) {
    return m * std::log(m / static_cast<double>(zero_regs));
  }
  return raw_est;
}

// ============================================================================
// Quantile Sketch (Exact <= 4096, T-Digest Centroids > 4096)
// ============================================================================

void QuantileSketch::Add(double v) {
  if (!std::isfinite(v)) return;
  if (!is_compressed) {
    exact_vals.push_back(v);
    if (exact_vals.size() > kExactLimit) {
      centroids.reserve(exact_vals.size());
      for (double x : exact_vals) centroids.emplace_back(x, 1.0);
      exact_vals.clear();
      exact_vals.shrink_to_fit();
      is_compressed = true;
      CompressCentroids(centroids, kMaxCentroids);
    }
  } else {
    centroids.emplace_back(v, 1.0);
    if (centroids.size() > kMaxCentroids * 4) {
      CompressCentroids(centroids, kMaxCentroids);
    }
  }
}

void QuantileSketch::FinalizeMap() {
  if (!is_compressed) {
    std::sort(exact_vals.begin(), exact_vals.end());
  } else {
    CompressCentroids(centroids, kMaxCentroids);
  }
}

void QuantileSketch::Merge(const QuantileSketch& other) {
  if (other.Empty()) return;
  if (!is_compressed && !other.is_compressed &&
      exact_vals.size() + other.exact_vals.size() <= kExactLimit) {
    exact_vals.insert(exact_vals.end(), other.exact_vals.begin(), other.exact_vals.end());
    return;
  }

  // Promote to centroid representation
  if (!is_compressed) {
    centroids.reserve(exact_vals.size() + other.exact_vals.size() + other.centroids.size());
    for (double x : exact_vals) centroids.emplace_back(x, 1.0);
    exact_vals.clear();
    is_compressed = true;
  }
  if (!other.is_compressed) {
    for (double x : other.exact_vals) centroids.emplace_back(x, 1.0);
  } else {
    centroids.insert(centroids.end(), other.centroids.begin(), other.centroids.end());
  }
  CompressCentroids(centroids, kMaxCentroids);
}

double QuantileSketch::Quantile(double pct) const {
  double q = std::clamp(pct, 0.0, 1.0);
  if (!is_compressed) {
    if (exact_vals.empty()) return 0.0;
    std::vector<double> sorted = exact_vals;
    std::sort(sorted.begin(), sorted.end());
    size_t idx = static_cast<size_t>(q * static_cast<double>(sorted.size() - 1));
    if (idx >= sorted.size()) idx = sorted.size() - 1;
    return sorted[idx];
  }
  if (centroids.empty()) return 0.0;
  std::vector<std::pair<double, double>> sorted = centroids;
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  double total_w = 0.0;
  for (const auto& [m, w] : sorted) total_w += w;
  if (total_w <= 0.0) return sorted.front().first;

  double target = q * total_w;
  if (target <= sorted.front().second * 0.5) return sorted.front().first;
  if (target >= total_w - sorted.back().second * 0.5) return sorted.back().first;

  double cum = sorted[0].second * 0.5;
  for (size_t i = 0; i + 1 < sorted.size(); i++) {
    double step = (sorted[i].second + sorted[i + 1].second) * 0.5;
    if (cum + step >= target) {
      double frac = (step > 0.0) ? ((target - cum) / step) : 0.0;
      return sorted[i].first + frac * (sorted[i + 1].first - sorted[i].first);
    }
    cum += step;
  }
  return sorted.back().first;
}

// ============================================================================
// Pure C++ Pipeline Primitives (shared by standalone & Cluster Coordinator)
// ============================================================================

bool TryParseAggDouble(const std::string& s, double& out) {
  if (s.empty()) return false;
  char* ep = nullptr;
  out = std::strtod(s.c_str(), &ep);
  return ep != s.c_str() && *ep == '\0' && std::isfinite(out);
}

void ApplyAggLimit(std::vector<AggRow>& rows, long long offset, long long count) {
  long long total = static_cast<long long>(rows.size());
  if (offset >= total) {
    rows.clear();
    return;
  }
  auto begin = rows.begin() + static_cast<ptrdiff_t>(offset);
  auto end = (count >= 0 && offset + count < total)
                 ? rows.begin() + static_cast<ptrdiff_t>(offset + count)
                 : rows.end();
  rows = std::vector<AggRow>(begin, end);
}

void ApplyAggSortby(std::vector<AggRow>& rows,
                    const std::vector<std::pair<std::string, bool>>& keys, long long max_rows) {
  if (keys.empty()) return;
  auto cmp = [&](const AggRow& a, const AggRow& b) {
    for (const auto& [field, desc] : keys) {
      auto ait = a.find(field);
      auto bit = b.find(field);
      const std::string va = (ait != a.end()) ? ait->second : "";
      const std::string vb = (bit != b.end()) ? bit->second : "";
      if (va == vb) continue;
      double na = 0.0, nb = 0.0;
      bool num = TryParseAggDouble(va, na) && TryParseAggDouble(vb, nb);
      if (num) {
        if (na == nb) continue;
        return desc ? (na > nb) : (na < nb);
      }
      return desc ? (va > vb) : (va < vb);
    }
    return false;
  };
  if (max_rows > 0 && static_cast<size_t>(max_rows) < rows.size()) {
    std::partial_sort(rows.begin(), rows.begin() + static_cast<ptrdiff_t>(max_rows), rows.end(),
                      cmp);
    rows.resize(static_cast<size_t>(max_rows));
  } else {
    std::sort(rows.begin(), rows.end(), cmp);
  }
}

void ApplyAggGroupby(std::vector<AggRow>& rows, const std::vector<std::string>& gb_fields,
                     const std::vector<ReduceOp>& reducers) {
  std::vector<PartialGroup> partial;
  std::string ignored_err;
  BuildPartialGroups(rows, gb_fields, reducers, /*max_groups=*/0, partial, ignored_err);

  std::vector<AggRow> out;
  out.reserve(partial.size());
  for (auto& pg : partial) {
    AggRow row;
    for (const auto& [k, v] : pg.key_fields) row[k] = v;
    for (size_t ri = 0; ri < reducers.size() && ri < pg.reducers.size(); ri++) {
      row[reducers[ri].alias] = FinalizeReducerState(pg.reducers[ri], reducers[ri]);
    }
    out.push_back(std::move(row));
  }
  rows = std::move(out);
}

bool ExecuteAggPipelineStages(std::vector<AggRow>& rows, const std::vector<PipelineStage>& stages,
                              int64_t deadline_ms, std::string& error_out) {
  for (const auto& stage : stages) {
    if (deadline_ms > 0 && AggMonotonicMs() > deadline_ms) {
      error_out = "ERR query timeout";
      return false;
    }
    switch (stage.type) {
      case PipelineStageType::kLoad:
        break;
      case PipelineStageType::kApply: {
        ExprNode expr;
        if (!ParseExpr(stage.apply_expr, expr, error_out)) return false;
        for (auto& row : rows) {
          auto val = EvalExpr(expr, row);
          row[stage.apply_alias] = val.AsString();
        }
        break;
      }
      case PipelineStageType::kFilter: {
        ExprNode expr;
        if (!ParseExpr(stage.filter_expr, expr, error_out)) return false;
        rows.erase(
            std::remove_if(rows.begin(), rows.end(),
                           [&](const AggRow& row) { return !EvalExpr(expr, row).Truthy(); }),
            rows.end());
        break;
      }
      case PipelineStageType::kGroupby:
        if (stage.groupby_fields.empty() && stage.reducers.empty()) break;
        ApplyAggGroupby(rows, stage.groupby_fields, stage.reducers);
        break;
      case PipelineStageType::kSortby:
        ApplyAggSortby(rows, stage.sortby_keys, stage.sortby_max);
        break;
      case PipelineStageType::kLimit:
        ApplyAggLimit(rows, stage.limit_offset, stage.limit_count);
        break;
    }
  }
  return true;
}

// ============================================================================
// ClusterAggPlanner: Pipeline Split (Map Pushdown vs Coordinator Reduce)
// ============================================================================

ClusterAggPlanSplit SplitAggPipeline(const std::vector<PipelineStage>& stages, bool has_knn) {
  ClusterAggPlanSplit split;

  // For KNN queries inside FT.AGGREGATE, global top-K selection must occur before any
  // GROUPBY/APPLY/FILTER/SORTBY/LIMIT, so shards return local KNN rows and Coordinator runs pipeline.
  if (has_knn) {
    split.mode = ClusterAggPlanSplit::Mode::kRawRows;
    split.reduce_stages = stages;
    return split;
  }

  // Locate first GROUPBY stage, checking if any LIMIT precedes it
  size_t first_gb = stages.size();
  bool limit_before_gb = false;
  for (size_t i = 0; i < stages.size(); i++) {
    if (stages[i].type == PipelineStageType::kLimit) {
      limit_before_gb = true;
    }
    if (stages[i].type == PipelineStageType::kGroupby &&
        (!stages[i].groupby_fields.empty() || !stages[i].reducers.empty())) {
      first_gb = i;
      break;
    }
  }

  if (first_gb < stages.size() && !limit_before_gb) {
    // Two-phase Map-Reduce:
    //   Map (shard): pre-GROUPBY LOAD/APPLY/FILTER + Partial Reduce of first GROUPBY
    //   Reduce (Coordinator): Merge partial groups + post-GROUPBY APPLY/FILTER/SORTBY/LIMIT/GROUPBY
    split.mode = ClusterAggPlanSplit::Mode::kMapReduceGroupby;
    for (size_t i = 0; i < first_gb; i++) {
      // SORTBY before GROUPBY does not affect group membership (unless FIRST_VALUE without BY,
      // in which case pushing SORTBY to map_stages preserves local order too)
      split.map_stages.push_back(stages[i]);
    }
    split.groupby_stage = stages[first_gb];
    for (size_t i = first_gb + 1; i < stages.size(); i++) {
      split.reduce_stages.push_back(stages[i]);
    }
    return split;
  }

  // No eligible GROUPBY: pure projection / filter / sort / limit mode
  // Push down leading LOAD, APPLY, FILTER stages to shards; optionally push down first SORTBY+LIMIT cap
  split.mode = ClusterAggPlanSplit::Mode::kRawRows;
  size_t cut = 0;
  while (cut < stages.size() &&
         (stages[cut].type == PipelineStageType::kLoad ||
          stages[cut].type == PipelineStageType::kApply ||
          stages[cut].type == PipelineStageType::kFilter)) {
    split.map_stages.push_back(stages[cut]);
    cut++;
  }

  // Check if next stage is SORTBY (possibly followed by LIMIT) for per-shard top-K pushdown
  if (cut < stages.size() && stages[cut].type == PipelineStageType::kSortby) {
    split.raw_has_sortby = true;
    split.raw_sortby_keys = stages[cut].sortby_keys;
    size_t cap = 0;
    if (stages[cut].sortby_max > 0) {
      cap = static_cast<size_t>(stages[cut].sortby_max);
    }
    if (cut + 1 < stages.size() && stages[cut + 1].type == PipelineStageType::kLimit) {
      long long off = std::max<long long>(0, stages[cut + 1].limit_offset);
      long long cnt = std::max<long long>(0, stages[cut + 1].limit_count);
      size_t lim_cap = static_cast<size_t>(off + cnt);
      cap = (cap == 0) ? lim_cap : std::min(cap, lim_cap);
    }
    split.raw_shard_limit = cap;
  } else if (cut < stages.size() && stages[cut].type == PipelineStageType::kLimit &&
             cut + 1 == stages.size()) {
    long long off = std::max<long long>(0, stages[cut].limit_offset);
    long long cnt = std::max<long long>(0, stages[cut].limit_count);
    split.raw_shard_limit = static_cast<size_t>(off + cnt);
  }

  for (size_t i = cut; i < stages.size(); i++) {
    split.reduce_stages.push_back(stages[i]);
  }
  return split;
}

// ============================================================================
// Map & Reduce for All 12 Reducers
// ============================================================================

bool BuildPartialGroups(const std::vector<AggRow>& rows,
                        const std::vector<std::string>& gb_fields,
                        const std::vector<ReduceOp>& reducers, size_t max_groups,
                        std::vector<PartialGroup>& out_groups, std::string& error_out) {
  out_groups.clear();

  // Map group_key -> index in out_groups
  std::unordered_map<std::string, size_t> group_idx;
  // Auxiliary distinct sets for O(1) dedup per (group, reducer) during Map
  std::vector<std::vector<std::unordered_set<std::string>>> distinct_sets;
  // Deterministic per-Map RNG for Reservoir Sampling (RANDOM_SAMPLE)
  std::mt19937_64 rng(0x517CC1B727220A95ULL ^ static_cast<uint64_t>(rows.size()));

  for (const auto& row : rows) {
    std::string gk = MakeGroupKey(row, gb_fields);
    auto git = group_idx.find(gk);
    size_t gi = 0;
    if (git == group_idx.end()) {
      if (max_groups > 0 && out_groups.size() >= max_groups) {
        error_out = "ERR cluster aggregate exceeded CLUSTER_AGG_MAX_GROUPS limit";
        return false;
      }
      gi = out_groups.size();
      group_idx.emplace(gk, gi);

      PartialGroup pg;
      pg.group_key = std::move(gk);
      pg.key_fields.reserve(gb_fields.size());
      for (const auto& gf : gb_fields) {
        auto fit = row.find(gf);
        pg.key_fields.emplace_back(gf, (fit != row.end()) ? fit->second : "");
      }
      pg.reducers.resize(reducers.size());
      for (size_t ri = 0; ri < reducers.size(); ri++) {
        pg.reducers[ri].func = reducers[ri].func;
      }
      out_groups.push_back(std::move(pg));
      distinct_sets.emplace_back(reducers.size());
    } else {
      gi = git->second;
    }

    auto& pg = out_groups[gi];
    for (size_t ri = 0; ri < reducers.size(); ri++) {
      const auto& op = reducers[ri];
      auto& st = pg.reducers[ri];

      if (op.func == ReduceFunc::kCount) {
        st.count++;
        continue;
      }

      auto fit = row.find(op.field);
      if (fit == row.end()) continue;
      const std::string& fval = fit->second;

      switch (op.func) {
        case ReduceFunc::kCount:
          break;
        case ReduceFunc::kCountDistinct: {
          auto& dset = distinct_sets[gi][ri];
          if (dset.size() < kMaxPartialDistinctPerGroup && dset.insert(fval).second) {
            st.str_vals.push_back(fval);
          }
          break;
        }
        case ReduceFunc::kCountDistinctish: {
          st.hll.Add(fval);
          break;
        }
        case ReduceFunc::kTolist: {
          if (st.str_vals.size() < kMaxPartialListPerGroup) {
            st.str_vals.push_back(fval);
          }
          break;
        }
        case ReduceFunc::kFirstValue: {
          if (!op.first_value_by.empty()) {
            double sv = 0.0;
            auto sit = row.find(op.first_value_by);
            if (sit != row.end()) TryParseAggDouble(sit->second, sv);
            if (!st.has_val ||
                (op.first_value_desc ? (sv > st.first_sort_val) : (sv < st.first_sort_val)) ||
                (sv == st.first_sort_val && fval < st.first_val)) {
              st.has_val = true;
              st.first_val = fval;
              st.first_sort_val = sv;
            }
          } else if (!st.has_val) {
            st.has_val = true;
            st.first_val = fval;
          }
          break;
        }
        case ReduceFunc::kRandomSample: {
          size_t k = (op.random_sample_count > 0) ? static_cast<size_t>(op.random_sample_count) : 0;
          st.count++;
          if (k == 0) break;
          if (st.str_vals.size() < k) {
            st.str_vals.push_back(fval);
          } else {
            uint64_t j = rng() % st.count;
            if (j < k) {
              st.str_vals[static_cast<size_t>(j)] = fval;
            }
          }
          break;
        }
        case ReduceFunc::kSum:
        case ReduceFunc::kAvg:
        case ReduceFunc::kMin:
        case ReduceFunc::kMax:
        case ReduceFunc::kStddev:
        case ReduceFunc::kQuantile: {
          double val = 0.0;
          if (!TryParseAggDouble(fval, val)) break;
          if (op.func == ReduceFunc::kSum) {
            st.sum += val;
            st.has_val = true;
          } else if (op.func == ReduceFunc::kAvg) {
            st.sum += val;
            st.count++;
          } else if (op.func == ReduceFunc::kMin) {
            if (!st.has_val || val < st.min_val) {
              st.min_val = val;
              st.has_val = true;
            }
          } else if (op.func == ReduceFunc::kMax) {
            if (!st.has_val || val > st.max_val) {
              st.max_val = val;
              st.has_val = true;
            }
          } else if (op.func == ReduceFunc::kStddev) {
            // Welford's online algorithm -> exact Chan triple (count, mean, m2)
            st.count++;
            double delta = val - st.mean;
            st.mean += delta / static_cast<double>(st.count);
            double delta2 = val - st.mean;
            st.m2 += delta * delta2;
          } else if (op.func == ReduceFunc::kQuantile) {
            st.quantile.Add(val);
          }
          break;
        }
      }
    }
  }

  for (auto& pg : out_groups) {
    for (auto& st : pg.reducers) {
      if (st.func == ReduceFunc::kCountDistinctish) {
        st.hll.Compact();
      } else if (st.func == ReduceFunc::kQuantile) {
        st.quantile.FinalizeMap();
      }
    }
  }
  return true;
}

void MergeReducerState(PartialReducerState& dst, const PartialReducerState& src,
                       const ReduceOp& op) {
  switch (op.func) {
    case ReduceFunc::kCount:
      dst.count += src.count;
      break;
    case ReduceFunc::kSum:
      if (src.has_val) {
        dst.sum += src.sum;
        dst.has_val = true;
      }
      break;
    case ReduceFunc::kAvg:
      dst.sum += src.sum;
      dst.count += src.count;
      break;
    case ReduceFunc::kMin:
      if (src.has_val && (!dst.has_val || src.min_val < dst.min_val)) {
        dst.min_val = src.min_val;
        dst.has_val = true;
      }
      break;
    case ReduceFunc::kMax:
      if (src.has_val && (!dst.has_val || src.max_val > dst.max_val)) {
        dst.max_val = src.max_val;
        dst.has_val = true;
      }
      break;
    case ReduceFunc::kStddev: {
      // Chan's parallel variance merge algorithm (numerically stable)
      if (src.count == 0) break;
      if (dst.count == 0) {
        dst.count = src.count;
        dst.mean = src.mean;
        dst.m2 = src.m2;
        break;
      }
      double na = static_cast<double>(dst.count);
      double nb = static_cast<double>(src.count);
      double n = na + nb;
      double delta = src.mean - dst.mean;
      dst.mean = dst.mean + delta * (nb / n);
      dst.m2 = dst.m2 + src.m2 + delta * delta * (na * nb / n);
      dst.count += src.count;
      break;
    }
    case ReduceFunc::kCountDistinct: {
      // Collect raw strings; deduplicated in FinalizeReducerState (or keep bounded)
      for (const auto& v : src.str_vals) {
        if (dst.str_vals.size() < kMaxPartialDistinctPerGroup * 2) {
          dst.str_vals.push_back(v);
        }
      }
      break;
    }
    case ReduceFunc::kCountDistinctish: {
      dst.hll.Merge(src.hll);
      break;
    }
    case ReduceFunc::kTolist: {
      for (const auto& v : src.str_vals) {
        if (dst.str_vals.size() < kMaxPartialListPerGroup) {
          dst.str_vals.push_back(v);
        }
      }
      break;
    }
    case ReduceFunc::kFirstValue: {
      if (!src.has_val) break;
      if (!op.first_value_by.empty()) {
        if (!dst.has_val ||
            (op.first_value_desc ? (src.first_sort_val > dst.first_sort_val)
                                 : (src.first_sort_val < dst.first_sort_val)) ||
            (src.first_sort_val == dst.first_sort_val && src.first_val < dst.first_val)) {
          dst.has_val = true;
          dst.first_val = src.first_val;
          dst.first_sort_val = src.first_sort_val;
        }
      } else if (!dst.has_val) {
        dst.has_val = true;
        dst.first_val = src.first_val;
      }
      break;
    }
    case ReduceFunc::kQuantile: {
      dst.quantile.Merge(src.quantile);
      break;
    }
    case ReduceFunc::kRandomSample: {
      if (src.count == 0) break;
      size_t k = (op.random_sample_count > 0) ? static_cast<size_t>(op.random_sample_count) : 0;
      if (k == 0) {
        dst.count += src.count;
        break;
      }
      if (dst.count == 0) {
        dst.count = src.count;
        dst.str_vals = src.str_vals;
        if (dst.str_vals.size() > k) dst.str_vals.resize(k);
        break;
      }
      uint64_t total_n = dst.count + src.count;
      if (dst.str_vals.size() + src.str_vals.size() <= k) {
        dst.str_vals.insert(dst.str_vals.end(), src.str_vals.begin(), src.str_vals.end());
        dst.count = total_n;
        break;
      }
      // Statistically unbiased weighted merge of two reservoir samples of populations dst.count & src.count:
      // draw k items without replacement choosing from dst with prob rem_a / (rem_a + rem_b)
      std::mt19937_64 rng(0x9E3779B97F4A7C15ULL ^ (dst.count * 1315423911ULL) ^ src.count);
      std::vector<std::string> pool_a = dst.str_vals;
      std::vector<std::string> pool_b = src.str_vals;
      std::shuffle(pool_a.begin(), pool_a.end(), rng);
      std::shuffle(pool_b.begin(), pool_b.end(), rng);
      uint64_t rem_a = dst.count;
      uint64_t rem_b = src.count;
      size_t ia = 0, ib = 0;
      std::vector<std::string> merged;
      merged.reserve(k);
      while (merged.size() < k && (ia < pool_a.size() || ib < pool_b.size())) {
        if (ia >= pool_a.size()) {
          merged.push_back(std::move(pool_b[ib++]));
          if (rem_b > 0) rem_b--;
        } else if (ib >= pool_b.size()) {
          merged.push_back(std::move(pool_a[ia++]));
          if (rem_a > 0) rem_a--;
        } else {
          uint64_t tot = rem_a + rem_b;
          uint64_t pick = (tot > 0) ? (rng() % tot) : 0;
          if (pick < rem_a) {
            merged.push_back(std::move(pool_a[ia++]));
            if (rem_a > 0) rem_a--;
          } else {
            merged.push_back(std::move(pool_b[ib++]));
            if (rem_b > 0) rem_b--;
          }
        }
      }
      dst.str_vals = std::move(merged);
      dst.count = total_n;
      break;
    }
  }
}

std::string FinalizeReducerState(PartialReducerState& st, const ReduceOp& op) {
  switch (op.func) {
    case ReduceFunc::kCount:
      return std::to_string(st.count);
    case ReduceFunc::kSum:
      return st.has_val ? std::to_string(st.sum) : "0";
    case ReduceFunc::kAvg:
      return (st.count > 0) ? std::to_string(st.sum / static_cast<double>(st.count)) : "0";
    case ReduceFunc::kMin:
      return st.has_val ? std::to_string(st.min_val) : "0";
    case ReduceFunc::kMax:
      return st.has_val ? std::to_string(st.max_val) : "0";
    case ReduceFunc::kStddev: {
      if (st.count > 1) {
        double variance = st.m2 / static_cast<double>(st.count);
        if (variance < 0.0) variance = 0.0;
        return std::to_string(std::sqrt(variance));
      }
      return "0";
    }
    case ReduceFunc::kCountDistinct: {
      std::unordered_set<std::string> uniq(st.str_vals.begin(), st.str_vals.end());
      return std::to_string(static_cast<long long>(uniq.size()));
    }
    case ReduceFunc::kCountDistinctish: {
      long long est = static_cast<long long>(std::llround(st.hll.Estimate()));
      return std::to_string(est);
    }
    case ReduceFunc::kTolist: {
      std::string joined;
      for (size_t i = 0; i < st.str_vals.size(); i++) {
        if (i > 0) joined.push_back(',');
        joined += st.str_vals[i];
      }
      return joined;
    }
    case ReduceFunc::kFirstValue:
      return st.has_val ? st.first_val : "";
    case ReduceFunc::kQuantile:
      return st.quantile.Empty() ? "0" : std::to_string(st.quantile.Quantile(op.quantile_pct));
    case ReduceFunc::kRandomSample: {
      std::string joined;
      for (size_t i = 0; i < st.str_vals.size(); i++) {
        if (i > 0) joined.push_back(',');
        joined += st.str_vals[i];
      }
      return joined;
    }
  }
  return "0";
}

bool MergeAndFinalizeGroups(const std::vector<std::vector<PartialGroup>>& shard_groups,
                            const std::vector<ReduceOp>& reducers, size_t max_groups,
                            std::vector<AggRow>& out_rows, std::string& error_out) {
  out_rows.clear();
  std::vector<PartialGroup> merged;
  std::unordered_map<std::string, size_t> group_idx;

  for (const auto& sgroups : shard_groups) {
    for (const auto& sg : sgroups) {
      auto git = group_idx.find(sg.group_key);
      if (git == group_idx.end()) {
        if (max_groups > 0 && merged.size() >= max_groups) {
          error_out = "ERR cluster aggregate exceeded CLUSTER_AGG_MAX_GROUPS limit";
          return false;
        }
        size_t idx = merged.size();
        group_idx.emplace(sg.group_key, idx);
        PartialGroup copy;
        copy.group_key = sg.group_key;
        copy.key_fields = sg.key_fields;
        copy.reducers.resize(reducers.size());
        for (size_t ri = 0; ri < reducers.size(); ri++) {
          copy.reducers[ri].func = reducers[ri].func;
          if (ri < sg.reducers.size()) {
            MergeReducerState(copy.reducers[ri], sg.reducers[ri], reducers[ri]);
          }
        }
        merged.push_back(std::move(copy));
      } else {
        auto& dst = merged[git->second];
        for (size_t ri = 0; ri < reducers.size() && ri < sg.reducers.size(); ri++) {
          MergeReducerState(dst.reducers[ri], sg.reducers[ri], reducers[ri]);
        }
      }
    }
  }

  out_rows.reserve(merged.size());
  for (auto& pg : merged) {
    AggRow row;
    for (const auto& [k, v] : pg.key_fields) row[k] = v;
    for (size_t ri = 0; ri < reducers.size() && ri < pg.reducers.size(); ri++) {
      row[reducers[ri].alias] = FinalizeReducerState(pg.reducers[ri], reducers[ri]);
    }
    out_rows.push_back(std::move(row));
  }
  return true;
}

}  // namespace gemini_search
