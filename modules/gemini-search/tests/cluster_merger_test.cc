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

TEST(ClusterMergerTest, HybridGlobalRrfAndLinearParityAcross3Shards) {
  std::mt19937 rng(2026069);
  std::uniform_real_distribution<double> text_dist(0.5, 25.0);
  std::uniform_real_distribution<float> vec_dist(0.01f, 2.0f);

  std::vector<ScoredResult> all_text;
  std::vector<KnnResult> all_vec;
  std::vector<ShardHybridSlice> shards(3);
  for (uint32_t s = 0; s < 3; s++) shards[s].shard_idx = s;

  // Distribute 600 docs across 3 shards; some only in text, some only in vec, some in both
  for (int i = 0; i < 600; i++) {
    char id_buf[32];
    std::snprintf(id_buf, sizeof(id_buf), "hdoc:%04d", i);
    std::string id(id_buf);
    uint32_t s = static_cast<uint32_t>(i % 3);
    if (i % 5 != 0) {
      double tscore = text_dist(rng);
      all_text.push_back({id, tscore});
      shards[s].text_hits.push_back({id, tscore});
    }
    if (i % 4 != 0) {
      float vscore = vec_dist(rng);
      all_vec.push_back({id, vscore});
      shards[s].vec_hits.push_back({id, vscore});
    }
  }

  // Simulate per-shard top-K truncation (each shard keeps top text_limit=50 and top knn_k=30)
  const int64_t text_limit = 50;
  const size_t knn_k = 30;
  for (auto& sh : shards) {
    std::sort(sh.text_hits.begin(), sh.text_hits.end(),
              [](const ScoredResult& a, const ScoredResult& b) {
                if (a.score != b.score) return a.score > b.score;
                return a.doc_id < b.doc_id;
              });
    if (static_cast<int64_t>(sh.text_hits.size()) > text_limit) {
      sh.text_hits.resize(static_cast<size_t>(text_limit));
    }
    std::sort(sh.vec_hits.begin(), sh.vec_hits.end(), [](const KnnResult& a, const KnnResult& b) {
      if (a.score != b.score) return a.score < b.score;
      return a.doc_id < b.doc_id;
    });
    if (sh.vec_hits.size() > knn_k) sh.vec_hits.resize(knn_k);
  }

  // Standalone reference: sort + truncate to text_limit / knn_k, then FuseHybridResults
  std::sort(all_text.begin(), all_text.end(), [](const ScoredResult& a, const ScoredResult& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.doc_id < b.doc_id;
  });
  all_text.resize(static_cast<size_t>(text_limit));
  std::sort(all_vec.begin(), all_vec.end(), [](const KnnResult& a, const KnnResult& b) {
    if (a.score != b.score) return a.score < b.score;
    return a.doc_id < b.doc_id;
  });
  all_vec.resize(knn_k);

  for (FusionMethod method : {FusionMethod::kRrf, FusionMethod::kLinear}) {
    HybridConfig hcfg;
    hcfg.method = method;
    hcfg.rrf_k = 60.0;
    hcfg.text_weight = 1.5;
    hcfg.vector_weight = 0.8;
    hcfg.alpha = 0.35;

    auto ref = FuseHybridResults(all_text, all_vec, hcfg);
    auto merged = MergeHybridShards(shards, text_limit, knn_k, hcfg, /*offset=*/5, /*count=*/20,
                                    /*has_limit=*/true);

    ASSERT_EQ(merged.total_fused, ref.size());
    ASSERT_EQ(merged.winners.size(), 20u);
    for (size_t i = 0; i < 20; i++) {
      EXPECT_EQ(merged.winners[i].doc_id, ref[5 + i].doc_id);
      EXPECT_NEAR(merged.winners[i].score, ref[5 + i].score, 1e-12);
      EXPECT_EQ(merged.winners[i].text_rank, ref[5 + i].text_rank);
      EXPECT_EQ(merged.winners[i].vector_rank, ref[5 + i].vector_rank);
    }

    // Verify LIMIT 0 0 count-only
    auto cnt_only = MergeHybridShards(shards, text_limit, knn_k, hcfg, 0, 0, /*has_limit=*/true);
    EXPECT_EQ(cnt_only.total_fused, ref.size());
    EXPECT_TRUE(cnt_only.winners.empty());
  }
}

TEST(ClusterMergerTest, TwoPhaseDfsBm25ExactParityUnder90PercentSkew) {
  // Construct a heavily skewed 3-shard corpus where 90% of "hotterm" docs live on shard 0
  // while shard 1 & shard 2 have very few "hotterm" docs and different doc lengths.
  IndexSpec spec;
  spec.name = "skew_idx";
  FieldSpec f;
  f.name = "body";
  f.type = FieldType::kText;
  spec.fields.push_back(f);

  DocumentStore standalone_docs;
  TextFieldIndices standalone_text;
  TagFieldIndices empty_tags;
  NumericFieldIndices empty_nums;
  GeoFieldIndices empty_geos;

  std::vector<DocumentStore> shard_docs(3);
  std::vector<TextFieldIndices> shard_text(3);

  auto AddDoc = [&](int shard_idx, const std::string& doc_id, const std::string& body) {
    standalone_docs.Add(doc_id, {{"body", body}});
    standalone_text.GetOrCreate("body").Add(doc_id, body);
    shard_docs[static_cast<size_t>(shard_idx)].Add(doc_id, {{"body", body}});
    shard_text[static_cast<size_t>(shard_idx)].GetOrCreate("body").Add(doc_id, body);
  };

  // Shard 0: 90 docs containing "hotterm" + "common" with longer text
  for (int i = 0; i < 90; i++) {
    std::string id = "s0_doc_" + std::to_string(i);
    std::string body = "hotterm common alpha beta gamma delta";
    if (i % 7 == 0) body += " hotterm rareterm";
    AddDoc(0, id, body);
  }
  // Shard 1: 5 docs with "hotterm", 95 background docs without "hotterm"
  for (int i = 0; i < 100; i++) {
    std::string id = "s1_doc_" + std::to_string(i);
    std::string body = (i < 5) ? "hotterm rareterm short" : "background filler words here";
    AddDoc(1, id, body);
  }
  // Shard 2: 5 docs with "hotterm", 95 very long background docs
  for (int i = 0; i < 100; i++) {
    std::string id = "s2_doc_" + std::to_string(i);
    std::string body =
        (i < 5) ? "hotterm common"
                : "very long background document containing many unrelated filler tokens alpha beta";
    AddDoc(2, id, body);
  }

  QueryOptions qopts;
  qopts.scorer = ScorerType::kBm25;
  ParsedQuery pq;
  std::string err;
  ASSERT_TRUE(ParseQuery("hotterm | rareterm", pq, err, qopts));

  // Standalone reference scores
  auto ref_scores = EvaluateQueryScored(pq.root, spec, standalone_docs, empty_tags, empty_nums,
                                        standalone_text, empty_geos, err, qopts);
  ASSERT_TRUE(err.empty());
  std::sort(ref_scores.begin(), ref_scores.end(), [](const ScoredResult& a, const ScoredResult& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.doc_id < b.doc_id;
  });

  // Phase 0: collect local term stats from each shard
  std::vector<std::vector<RpcFieldBm25Stats>> all_shard_stats(3);
  for (size_t s = 0; s < 3; s++) {
    auto local_stats = CollectQueryTermStats(pq.root, spec, shard_text[s], qopts);
    for (auto& ls : local_stats) {
      RpcFieldBm25Stats fs;
      fs.field_name = ls.field_name;
      fs.live_docs = ls.live_docs;
      fs.total_doc_len = ls.total_doc_len;
      for (const auto& [t, df] : ls.term_df) fs.term_df.emplace_back(t, df);
      all_shard_stats[s].push_back(std::move(fs));
    }
  }

  // Coordinator aggregates global BM25 stats
  auto global_vec = AggregateGlobalBm25FieldStats(all_shard_stats);
  std::unordered_map<std::string, GlobalBm25Stats> global_map;
  for (const auto& fs : global_vec) {
    GlobalBm25Stats g;
    g.total_docs = fs.live_docs;
    g.avg_doc_len =
        fs.live_docs > 0 ? (static_cast<double>(fs.total_doc_len) / static_cast<double>(fs.live_docs))
                         : 0.0;
    for (const auto& [t, df] : fs.term_df) g.term_df[t] = df;
    global_map[fs.field_name] = std::move(g);
  }

  // Phase 1 with global stats injected
  QueryOptions dfs_qopts = qopts;
  dfs_qopts.global_bm25_stats = &global_map;

  std::vector<ShardPhase1Slice> slices(3);
  for (size_t s = 0; s < 3; s++) {
    slices[s].shard_idx = static_cast<uint32_t>(s);
    auto sr = EvaluateQueryScored(pq.root, spec, shard_docs[s], empty_tags, empty_nums,
                                  shard_text[s], empty_geos, err, dfs_qopts);
    ASSERT_TRUE(err.empty());
    std::sort(sr.begin(), sr.end(), [](const ScoredResult& a, const ScoredResult& b) {
      if (a.score != b.score) return a.score > b.score;
      return a.doc_id < b.doc_id;
    });
    slices[s].total_hits = sr.size();
    for (auto& r : sr) {
      slices[s].items.push_back({r.doc_id, static_cast<uint32_t>(s), r.score, 0.0, "", false});
    }
  }

  auto merged = MergeSearchShards(slices, MergeSortMode::kScoreDesc, 0, ref_scores.size(),
                                  /*has_limit=*/true, /*max_results=*/10000);
  ASSERT_EQ(merged.winners.size(), ref_scores.size());
  for (size_t i = 0; i < ref_scores.size(); i++) {
    EXPECT_EQ(merged.winners[i].doc_id, ref_scores[i].doc_id);
    EXPECT_NEAR(merged.winners[i].score, ref_scores[i].score, 1e-12);
  }
}

TEST(ClusterMergerTest, MigrateDualWriteWindowDedupSearchAndHybrid) {
  // Simulate live MIGRATE dual-existence window where 25 docs exist on both Shard 0 and Shard 1.
  std::vector<ShardPhase1Slice> s_slices(3);
  for (uint32_t s = 0; s < 3; s++) s_slices[s].shard_idx = s;

  // 100 unique docs total: 0..49 on shard 0, 25..74 on shard 1 (25..49 overlap!), 75..99 on shard 2
  auto add_search = [&](uint32_t s, int id) {
    MergeCandidate c;
    c.doc_id = "mig:" + std::to_string(id);
    c.shard_idx = s;
    c.score = 1000.0 - static_cast<double>(id);
    c.sort_num = static_cast<double>(id);
    s_slices[s].items.push_back(std::move(c));
    s_slices[s].total_hits++;
  };
  for (int i = 0; i < 50; i++) add_search(0, i);
  for (int i = 25; i < 75; i++) add_search(1, i);
  for (int i = 75; i < 100; i++) add_search(2, i);

  auto sm = MergeSearchShards(s_slices, MergeSortMode::kSortNumAsc, 0, 200,
                              /*has_limit=*/true, /*max_results=*/1000);
  ASSERT_EQ(sm.global_total, 100u);
  ASSERT_EQ(sm.winners.size(), 100u);
  std::unordered_set<std::string> seen;
  for (size_t i = 0; i < sm.winners.size(); i++) {
    EXPECT_EQ(sm.winners[i].doc_id, "mig:" + std::to_string(i));
    EXPECT_TRUE(seen.insert(sm.winners[i].doc_id).second);
  }

  // Hybrid dual-write dedup across RRF and LINEAR
  std::vector<ShardHybridSlice> h_slices(3);
  for (uint32_t s = 0; s < 3; s++) h_slices[s].shard_idx = s;
  for (int i = 0; i < 50; i++) {
    h_slices[0].text_hits.push_back({"mig:" + std::to_string(i), 100.0 - i});
    h_slices[0].vec_hits.push_back({"mig:" + std::to_string(i), 0.01f * static_cast<float>(i)});
  }
  for (int i = 25; i < 75; i++) {
    h_slices[1].text_hits.push_back({"mig:" + std::to_string(i), 100.0 - i});
    h_slices[1].vec_hits.push_back({"mig:" + std::to_string(i), 0.01f * static_cast<float>(i)});
  }
  for (int i = 75; i < 100; i++) {
    h_slices[2].text_hits.push_back({"mig:" + std::to_string(i), 100.0 - i});
    h_slices[2].vec_hits.push_back({"mig:" + std::to_string(i), 0.01f * static_cast<float>(i)});
  }

  for (FusionMethod method : {FusionMethod::kRrf, FusionMethod::kLinear}) {
    HybridConfig hcfg;
    hcfg.method = method;
    auto hm = MergeHybridShards(h_slices, /*text_limit=*/100, /*knn_k=*/100, hcfg, 0, 200,
                                /*has_limit=*/true);
    ASSERT_EQ(hm.total_fused, 100u);
    ASSERT_EQ(hm.winners.size(), 100u);
    std::unordered_set<std::string> hseen;
    for (const auto& w : hm.winners) {
      EXPECT_TRUE(hseen.insert(w.doc_id).second);
    }
  }
}
