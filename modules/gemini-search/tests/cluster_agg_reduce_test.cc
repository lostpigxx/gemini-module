#include "cluster_agg_reduce.h"
#include "cluster_codec.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace gemini_search;

namespace {

std::vector<std::string> SplitCsvSorted(const std::string& s) {
  std::vector<std::string> out;
  if (s.empty()) return out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) {
    out.push_back(item);
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

TEST(ClusterAggReduceTest, All12ReducersRandomShardSplitParityAcross1_3_5_8Shards) {
  std::mt19937 rng(707070);
  std::uniform_real_distribution<double> price_dist(10.0, 5000.0);
  std::uniform_int_distribution<int> user_dist(0, 149);

  const std::vector<std::string> cats = {"electronics", "books", "home", "sports", "toys"};
  std::vector<AggRow> all_rows;
  all_rows.reserve(2000);
  for (int i = 0; i < 2000; i++) {
    AggRow r;
    r["cat"] = cats[static_cast<size_t>(i % cats.size())];
    r["price"] = std::to_string(std::round(price_dist(rng) * 100.0) / 100.0);
    r["user"] = "u_" + std::to_string(user_dist(rng));
    r["sku"] = "sku_" + std::to_string(i);
    r["rank"] = std::to_string((i * 37) % 10000);
    all_rows.push_back(std::move(r));
  }

  std::vector<ReduceOp> reducers;
  {
    ReduceOp r;
    r.func = ReduceFunc::kCount;
    r.alias = "cnt";
    reducers.push_back(r);
  }
  {
    ReduceOp r;
    r.func = ReduceFunc::kSum;
    r.field = "price";
    r.alias = "sum_p";
    reducers.push_back(r);
  }
  {
    ReduceOp r;
    r.func = ReduceFunc::kAvg;
    r.field = "price";
    r.alias = "avg_p";
    reducers.push_back(r);
  }
  {
    ReduceOp r;
    r.func = ReduceFunc::kMin;
    r.field = "price";
    r.alias = "min_p";
    reducers.push_back(r);
  }
  {
    ReduceOp r;
    r.func = ReduceFunc::kMax;
    r.field = "price";
    r.alias = "max_p";
    reducers.push_back(r);
  }
  {
    ReduceOp r;
    r.func = ReduceFunc::kStddev;
    r.field = "price";
    r.alias = "std_p";
    reducers.push_back(r);
  }
  {
    ReduceOp r;
    r.func = ReduceFunc::kCountDistinct;
    r.field = "user";
    r.alias = "cd_u";
    reducers.push_back(r);
  }
  {
    ReduceOp r;
    r.func = ReduceFunc::kCountDistinctish;
    r.field = "user";
    r.alias = "cdi_u";
    reducers.push_back(r);
  }
  {
    ReduceOp r;
    r.func = ReduceFunc::kTolist;
    r.field = "sku";
    r.alias = "skus";
    reducers.push_back(r);
  }
  {
    ReduceOp r;
    r.func = ReduceFunc::kFirstValue;
    r.field = "sku";
    r.first_value_by = "rank";
    r.first_value_desc = true;
    r.alias = "top_sku";
    reducers.push_back(r);
  }
  {
    ReduceOp r;
    r.func = ReduceFunc::kFirstValue;
    r.field = "sku";
    r.first_value_by = "rank";
    r.first_value_desc = false;
    r.alias = "bot_sku";
    reducers.push_back(r);
  }
  {
    ReduceOp r;
    r.func = ReduceFunc::kQuantile;
    r.field = "price";
    r.quantile_pct = 0.90;
    r.alias = "p90";
    reducers.push_back(r);
  }
  {
    ReduceOp r;
    r.func = ReduceFunc::kRandomSample;
    r.field = "sku";
    r.random_sample_count = 7;
    r.alias = "samp";
    reducers.push_back(r);
  }

  // Standalone single-node baseline
  std::vector<AggRow> ref_rows = all_rows;
  ApplyAggGroupby(ref_rows, {"cat"}, reducers);
  ApplyAggSortby(ref_rows, {{"cat", false}});
  ASSERT_EQ(ref_rows.size(), cats.size());

  std::unordered_map<std::string, AggRow> ref_by_cat;
  for (const auto& r : ref_rows) ref_by_cat[r.at("cat")] = r;

  for (size_t num_shards : {size_t{1}, size_t{3}, size_t{5}, size_t{8}}) {
    std::vector<std::vector<AggRow>> shard_rows(num_shards);
    for (const auto& r : all_rows) {
      size_t s = rng() % num_shards;
      shard_rows[s].push_back(r);
    }

    // Run Map + binary wire serialization round-trip per shard
    std::vector<std::vector<PartialGroup>> decoded_shard_groups(num_shards);
    for (size_t s = 0; s < num_shards; s++) {
      RpcAggMapResponse resp;
      resp.ok = true;
      resp.mode = 1;
      std::string err;
      ASSERT_TRUE(BuildPartialGroups(shard_rows[s], {"cat"}, reducers, 100000, resp.partial_groups,
                                     err))
          << err;
      std::string wire;
      EncodeAggMapResponse(resp, wire);
      RpcAggMapResponse decoded;
      ASSERT_TRUE(DecodeAggMapResponse(wire, decoded));
      decoded_shard_groups[s] = std::move(decoded.partial_groups);
    }

    std::vector<AggRow> merged_rows;
    std::string err;
    ASSERT_TRUE(MergeAndFinalizeGroups(decoded_shard_groups, reducers, 100000, merged_rows, err))
        << err;
    ApplyAggSortby(merged_rows, {{"cat", false}});
    ASSERT_EQ(merged_rows.size(), cats.size());

    for (const auto& r : merged_rows) {
      const auto& ref = ref_by_cat.at(r.at("cat"));
      EXPECT_EQ(r.at("cnt"), ref.at("cnt"));
      EXPECT_NEAR(std::stod(r.at("sum_p")), std::stod(ref.at("sum_p")), 1e-4);
      EXPECT_NEAR(std::stod(r.at("avg_p")), std::stod(ref.at("avg_p")), 1e-5);
      EXPECT_NEAR(std::stod(r.at("min_p")), std::stod(ref.at("min_p")), 1e-6);
      EXPECT_NEAR(std::stod(r.at("max_p")), std::stod(ref.at("max_p")), 1e-6);
      EXPECT_NEAR(std::stod(r.at("std_p")), std::stod(ref.at("std_p")), 1e-4);
      EXPECT_EQ(r.at("cd_u"), ref.at("cd_u"));
      // HLL 14-bit is exact/within ~3% for ~150 distinct users
      double exact_cd = std::stod(ref.at("cd_u"));
      double hll_cd = std::stod(r.at("cdi_u"));
      EXPECT_NEAR(hll_cd, exact_cd, std::max(5.0, exact_cd * 0.04));
      EXPECT_EQ(SplitCsvSorted(r.at("skus")), SplitCsvSorted(ref.at("skus")));
      EXPECT_EQ(r.at("top_sku"), ref.at("top_sku"));
      EXPECT_EQ(r.at("bot_sku"), ref.at("bot_sku"));
      EXPECT_NEAR(std::stod(r.at("p90")), std::stod(ref.at("p90")), 1e-6);

      // RANDOM_SAMPLE returns exactly 7 valid SKUs from this category
      auto sampled = SplitCsvSorted(r.at("samp"));
      auto all_skus_vec = SplitCsvSorted(ref.at("skus"));
      std::unordered_set<std::string> sku_set(all_skus_vec.begin(), all_skus_vec.end());
      EXPECT_EQ(sampled.size(), 7u);
      for (const auto& sv : sampled) {
        EXPECT_TRUE(sku_set.count(sv) > 0);
      }
    }
  }
}

TEST(ClusterAggReduceTest, ChanStddevNumericalStabilityAndLargeScaleHllAndQuantile) {
  // 1. Chan parallel variance merge stability on huge-offset values (1e9 + x)
  std::vector<double> vals;
  vals.reserve(3000);
  for (int i = 0; i < 3000; i++) vals.push_back(1e9 + static_cast<double>(i % 100));

  double mean = 0.0;
  for (double v : vals) mean += v;
  mean /= static_cast<double>(vals.size());
  double var_sum = 0.0;
  for (double v : vals) var_sum += (v - mean) * (v - mean);
  double true_std = std::sqrt(var_sum / static_cast<double>(vals.size()));

  ReduceOp std_op;
  std_op.func = ReduceFunc::kStddev;
  std_op.field = "v";
  std_op.alias = "s";

  std::vector<std::vector<PartialGroup>> sgroups(3);
  for (size_t s = 0; s < 3; s++) {
    std::vector<AggRow> rows;
    for (size_t i = s; i < vals.size(); i += 3) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.6f", vals[i]);
      rows.push_back({{"g", "all"}, {"v", buf}});
    }
    std::string err;
    ASSERT_TRUE(BuildPartialGroups(rows, {"g"}, {std_op}, 100, sgroups[s], err));
  }
  std::vector<AggRow> out;
  std::string err;
  ASSERT_TRUE(MergeAndFinalizeGroups(sgroups, {std_op}, 100, out, err));
  ASSERT_EQ(out.size(), 1u);
  EXPECT_NEAR(std::stod(out[0]["s"]), true_std, 1e-4);

  // 2. Large-scale HLL (20,000 distinct items split across 5 shards -> dense 16KB merge, < 3% err)
  std::vector<HllSketch> hlls(5);
  for (int i = 0; i < 20000; i++) {
    hlls[static_cast<size_t>(i % 5)].Add("item_key_" + std::to_string(i));
  }
  HllSketch merged_hll;
  for (auto& h : hlls) {
    h.Compact();
    merged_hll.Merge(h);
  }
  double est = merged_hll.Estimate();
  EXPECT_NEAR(est, 20000.0, 20000.0 * 0.03);

  // 3. Large-scale Quantile centroid compression (20,000 uniform values across 4 shards -> < 1% tail err)
  std::vector<QuantileSketch> qsk(4);
  for (int i = 0; i < 20000; i++) {
    qsk[static_cast<size_t>(i % 4)].Add(static_cast<double>(i));
  }
  QuantileSketch merged_q;
  for (auto& q : qsk) {
    q.FinalizeMap();
    merged_q.Merge(q);
  }
  EXPECT_TRUE(merged_q.is_compressed);
  EXPECT_NEAR(merged_q.Quantile(0.50), 10000.0, 200.0);
  EXPECT_NEAR(merged_q.Quantile(0.95), 19000.0, 200.0);
  EXPECT_NEAR(merged_q.Quantile(0.99), 19800.0, 200.0);
}

TEST(ClusterAggReduceTest, PlannerSplitAndMaxGroupsProtection) {
  // 1. Pre-APPLY -> Pre-FILTER -> GROUPBY -> Post-APPLY -> SORTBY -> LIMIT
  std::vector<PipelineStage> pipe;
  {
    PipelineStage s;
    s.type = PipelineStageType::kApply;
    s.apply_expr = "@price * 2";
    s.apply_alias = "p2";
    pipe.push_back(s);
  }
  {
    PipelineStage s;
    s.type = PipelineStageType::kFilter;
    s.filter_expr = "@p2 > 10";
    pipe.push_back(s);
  }
  {
    PipelineStage s;
    s.type = PipelineStageType::kGroupby;
    s.groupby_fields = {"cat"};
    ReduceOp r;
    r.func = ReduceFunc::kCount;
    r.alias = "cnt";
    s.reducers = {r};
    pipe.push_back(s);
  }
  {
    PipelineStage s;
    s.type = PipelineStageType::kSortby;
    s.sortby_keys = {{"cnt", true}};
    pipe.push_back(s);
  }
  {
    PipelineStage s;
    s.type = PipelineStageType::kLimit;
    s.limit_offset = 0;
    s.limit_count = 5;
    pipe.push_back(s);
  }

  auto split = SplitAggPipeline(pipe, /*has_knn=*/false);
  EXPECT_EQ(split.mode, ClusterAggPlanSplit::Mode::kMapReduceGroupby);
  EXPECT_EQ(split.map_stages.size(), 2u);
  EXPECT_EQ(split.groupby_stage.groupby_fields, (std::vector<std::string>{"cat"}));
  EXPECT_EQ(split.reduce_stages.size(), 2u);

  // KNN forces kRawRows so global top-K happens before GROUPBY
  auto knn_split = SplitAggPipeline(pipe, /*has_knn=*/true);
  EXPECT_EQ(knn_split.mode, ClusterAggPlanSplit::Mode::kRawRows);
  EXPECT_EQ(knn_split.reduce_stages.size(), pipe.size());

  // Pure SORTBY + LIMIT without GROUPBY pushes down SORTBY and shard cap
  std::vector<PipelineStage> raw_pipe;
  {
    PipelineStage s;
    s.type = PipelineStageType::kSortby;
    s.sortby_keys = {{"price", true}};
    raw_pipe.push_back(s);
  }
  {
    PipelineStage s;
    s.type = PipelineStageType::kLimit;
    s.limit_offset = 10;
    s.limit_count = 20;
    raw_pipe.push_back(s);
  }
  auto raw_split = SplitAggPipeline(raw_pipe, /*has_knn=*/false);
  EXPECT_EQ(raw_split.mode, ClusterAggPlanSplit::Mode::kRawRows);
  EXPECT_TRUE(raw_split.raw_has_sortby);
  EXPECT_EQ(raw_split.raw_shard_limit, 30u);

  // CLUSTER_AGG_MAX_GROUPS quota rejection on Map
  std::vector<AggRow> many_groups;
  for (int i = 0; i < 50; i++) many_groups.push_back({{"k", "g_" + std::to_string(i)}});
  std::vector<PartialGroup> out_g;
  std::string err;
  EXPECT_FALSE(BuildPartialGroups(many_groups, {"k"}, {}, /*max_groups=*/10, out_g, err));
  EXPECT_NE(err.find("CLUSTER_AGG_MAX_GROUPS"), std::string::npos);
}
