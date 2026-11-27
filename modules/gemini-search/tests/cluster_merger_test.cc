#include "cluster_merger.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

using namespace gemini_search;

namespace {

std::vector<MergeCandidate> MakeDataset(size_t n, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> score_dist(0.0, 100.0);
  std::uniform_int_distribution<int> num_dist(-500, 500);
  const char* words[] = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"};

  std::vector<MergeCandidate> docs;
  docs.reserve(n);
  for (size_t i = 0; i < n; i++) {
    MergeCandidate c;
    char id_buf[32];
    std::snprintf(id_buf, sizeof(id_buf), "doc:%05zu", i);
    c.doc_id = id_buf;
    c.score = score_dist(rng);
    c.sort_num = static_cast<double>(num_dist(rng));
    c.sort_str = std::string(words[rng() % 8]) + "_" + std::to_string(rng() % 50);
    docs.push_back(std::move(c));
  }
  return docs;
}

std::vector<ShardPhase1Slice> SplitIntoShards(const std::vector<MergeCandidate>& all_docs,
                                              size_t num_shards, MergeSortMode mode,
                                              size_t shard_limit_k, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<ShardPhase1Slice> shards(num_shards);
  for (size_t s = 0; s < num_shards; s++) {
    shards[s].shard_idx = static_cast<uint32_t>(s);
  }
  for (const auto& d : all_docs) {
    size_t target = rng() % num_shards;
    MergeCandidate copy = d;
    copy.shard_idx = static_cast<uint32_t>(target);
    shards[target].items.push_back(std::move(copy));
  }
  for (auto& s : shards) {
    s.total_hits = s.items.size();
    std::sort(s.items.begin(), s.items.end(),
              [mode](const MergeCandidate& a, const MergeCandidate& b) {
                return CompareMergeCandidates(a, b, mode);
              });
    if (shard_limit_k > 0 && s.items.size() > shard_limit_k) {
      s.items.resize(shard_limit_k);
    }
  }
  return shards;
}

}  // namespace

TEST(ClusterMergerTest, TenThousandDocsAcross1_3_7ShardsMatchesSingleNodeAllModes) {
  const auto dataset = MakeDataset(10000, 0x123456789ABCDEF0ULL);

  const std::vector<MergeSortMode> modes = {
      MergeSortMode::kDocIdAsc,    MergeSortMode::kScoreDesc,   MergeSortMode::kDistanceAsc,
      MergeSortMode::kSortNumAsc,  MergeSortMode::kSortNumDesc, MergeSortMode::kSortStrAsc,
      MergeSortMode::kSortStrDesc,
  };
  const std::vector<size_t> shard_counts = {1, 3, 7};
  const std::vector<std::pair<size_t, size_t>> limits = {
      {0, 10}, {0, 100}, {25, 30}, {500, 50}, {9990, 20}, {10050, 10},
  };

  for (MergeSortMode mode : modes) {
    // Ground truth single-node sort
    auto expected = dataset;
    std::sort(expected.begin(), expected.end(),
              [mode](const MergeCandidate& a, const MergeCandidate& b) {
                return CompareMergeCandidates(a, b, mode);
              });

    for (size_t num_shards : shard_counts) {
      for (const auto& [offset, count] : limits) {
        size_t shard_k = offset + count;
        auto shards = SplitIntoShards(dataset, num_shards, mode, shard_k,
                                      static_cast<uint64_t>(mode) * 31 + num_shards);
        auto merged = MergeSearchShards(shards, mode, offset, count, /*has_limit=*/true,
                                        /*max_results=*/1000000);

        EXPECT_EQ(merged.global_total, dataset.size());
        EXPECT_EQ(merged.duplicate_count, 0u);

        size_t exp_start = std::min(offset, expected.size());
        size_t exp_end = std::min(offset + count, expected.size());
        ASSERT_EQ(merged.winners.size(), exp_end - exp_start);

        for (size_t i = 0; i < merged.winners.size(); i++) {
          EXPECT_EQ(merged.winners[i].doc_id, expected[exp_start + i].doc_id);
        }

        // Verify winners_by_shard covers every winner exactly once
        size_t mapped_count = 0;
        for (const auto& [sidx, idxs] : merged.winners_by_shard) {
          for (size_t wi : idxs) {
            ASSERT_LT(wi, merged.winners.size());
            EXPECT_EQ(merged.winners[wi].shard_idx, sidx);
            mapped_count++;
          }
        }
        EXPECT_EQ(mapped_count, merged.winners.size());
      }
    }
  }
}

TEST(ClusterMergerTest, DuplicateDocIdDeduplicationDuringMigrate) {
  // Simulate live MIGRATE window where doc:002 and doc:005 appear on two shards
  ShardPhase1Slice s0;
  s0.shard_idx = 0;
  s0.total_hits = 4;
  s0.items = {
      {"doc:001", 0, 10.0, 1.0, "1", false},
      {"doc:002", 0, 9.0, 2.0, "2", false},
      {"doc:005", 0, 5.0, 5.0, "5", false},
      {"doc:008", 0, 2.0, 8.0, "8", false},
  };

  ShardPhase1Slice s1;
  s1.shard_idx = 1;
  s1.total_hits = 4;
  s1.items = {
      {"doc:002", 1, 9.0, 2.0, "2", false},  // duplicate of s0
      {"doc:003", 1, 8.0, 3.0, "3", false},
      {"doc:005", 1, 5.0, 5.0, "5", false},  // duplicate of s0
      {"doc:009", 1, 1.0, 9.0, "9", false},
  };

  ShardPhase1Slice s2;
  s2.shard_idx = 2;
  s2.total_hits = 0;  // empty shard

  auto merged = MergeSearchShards({s0, s1, s2}, MergeSortMode::kScoreDesc, 0, 10,
                                  /*has_limit=*/true, /*max_results=*/100);
  EXPECT_EQ(merged.duplicate_count, 2u);
  EXPECT_EQ(merged.global_total, 6u);
  ASSERT_EQ(merged.winners.size(), 6u);

  std::vector<std::string> got_ids;
  for (const auto& w : merged.winners) got_ids.push_back(w.doc_id);
  EXPECT_EQ(got_ids,
            (std::vector<std::string>{"doc:001", "doc:002", "doc:003", "doc:005", "doc:008", "doc:009"}));

  // Verify no duplicate doc_ids exist
  std::unordered_set<std::string> uniq(got_ids.begin(), got_ids.end());
  EXPECT_EQ(uniq.size(), got_ids.size());
}

TEST(ClusterMergerTest, KnnCapAndLimitZeroZero) {
  ShardPhase1Slice s0;
  s0.shard_idx = 0;
  s0.total_hits = 3;
  s0.items = {
      {"a", 0, 0.1, 0, "", false},
      {"d", 0, 0.4, 0, "", false},
      {"g", 0, 0.7, 0, "", false},
  };
  ShardPhase1Slice s1;
  s1.shard_idx = 1;
  s1.total_hits = 3;
  s1.items = {
      {"b", 1, 0.2, 0, "", false},
      {"e", 1, 0.5, 0, "", false},
      {"h", 1, 0.8, 0, "", false},
  };

  // KNN K=4 with LIMIT 1 2 -> global top-4 is [a, b, d, e], slice [1..3) is [b, d]
  auto m1 = MergeSearchShards({s0, s1}, MergeSortMode::kDistanceAsc, 1, 2, /*has_limit=*/true,
                              /*max_results=*/100, /*knn_cap=*/4);
  ASSERT_EQ(m1.winners.size(), 2u);
  EXPECT_EQ(m1.winners[0].doc_id, "b");
  EXPECT_EQ(m1.winners[1].doc_id, "d");

  // KNN K=4 with LIMIT 0 0 -> returns global_total = 4 and 0 winners
  auto m2 = MergeSearchShards({s0, s1}, MergeSortMode::kDistanceAsc, 0, 0, /*has_limit=*/true,
                              /*max_results=*/100, /*knn_cap=*/4);
  EXPECT_EQ(m2.global_total, 4u);
  EXPECT_TRUE(m2.winners.empty());

  // Non-KNN LIMIT 0 0 -> sums shard totals directly
  ShardPhase1Slice c0{0, 120, {}};
  ShardPhase1Slice c1{1, 95, {}};
  ShardPhase1Slice c2{2, 85, {}};
  auto m3 = MergeSearchShards({c0, c1, c2}, MergeSortMode::kDocIdAsc, 0, 0, /*has_limit=*/true,
                              /*max_results=*/100);
  EXPECT_EQ(m3.global_total, 300u);
  EXPECT_TRUE(m3.winners.empty());
}
