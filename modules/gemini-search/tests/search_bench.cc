#include "document_store.h"
#include "geo_index.h"
#include "hnsw_index.h"
#include "numeric_index.h"
#include "sort_index.h"
#include "tag_index.h"
#include "text_index.h"
#include "vector_index.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

template <typename Fn>
static double BenchUs(const char* name, int iters, Fn&& fn) {
  // Warmup
  fn();
  auto t0 = Clock::now();
  for (int i = 0; i < iters; i++) {
    fn();
  }
  auto t1 = Clock::now();
  double total_us =
      std::chrono::duration<double, std::micro>(t1 - t0).count();
  double avg_us = total_us / iters;
  std::printf("%-34s  %9.2f us/op  (%d iters)\n", name, avg_us, iters);
  return avg_us;
}

int main() {
  std::mt19937 rng(2026);
  volatile size_t sink = 0;

  std::printf("=== gemini-search Benchmark ===\n\n");

  // 1. TextIndex: 30,000 docs, ~30,000 distinct terms
  {
    TextIndex tidx;
    const char* prefixes[] = {"alpha", "beta", "gamma", "delta", "search",
                              "redis", "vector", "index", "query", "cache"};
    const char* suffixes[] = {"ing", "tion", "able", "ment", "ness",
                              "ized", "izer", "work", "fast", "node"};
    for (int i = 0; i < 30000; i++) {
      std::string term = std::string(prefixes[i % 10]) +
                         std::to_string(i) + suffixes[(i / 10) % 10];
      tidx.Add("doc" + std::to_string(i), term);
    }
    BenchUs("TextIndex::PrefixLookup (30k)", 200, [&]() {
      sink += tidx.PrefixLookup("search12", 200).size();
    });
    BenchUs("TextIndex::SuffixLookup (30k)", 200, [&]() {
      sink += tidx.SuffixLookup("99ing", 200).size();
    });
    int churn_i = 0;
    BenchUs("TextIndex::Update/Remove (30k)", 500, [&]() {
      std::string id = "doc" + std::to_string(churn_i++ % 30000);
      tidx.Add(id, "search12 updated fast node");
    });
  }

  // 2. GeoIndex: 30,000 points worldwide, 10 km city radius query
  {
    GeoIndex gidx;
    std::uniform_real_distribution<double> lon_dist(-180.0, 180.0);
    std::uniform_real_distribution<double> lat_dist(-80.0, 80.0);
    for (int i = 0; i < 30000; i++) {
      gidx.Add("g" + std::to_string(i), lon_dist(rng), lat_dist(rng));
    }
    BenchUs("GeoIndex::RadiusQuery 10km (30k)", 500, [&]() {
      sink += gidx.RadiusQuery(-122.4194, 37.7749, 10000.0).size();
    });
    BenchUs("GeoIndex::RadiusQuery 50km (30k)", 500, [&]() {
      sink += gidx.RadiusQuery(116.4074, 39.9042, 50000.0).size();
    });
  }

  // 3. Vector distance & FLAT/HNSW KNN: 5,000 x 128-dim vectors
  {
    constexpr size_t kDim = 128;
    constexpr int kVecs = 5000;
    std::uniform_real_distribution<float> fdist(-1.0f, 1.0f);
    FlatVectorIndex flat(kDim, DistanceMetric::kL2);
    FlatVectorIndex flat_cos(kDim, DistanceMetric::kCosine);
    FlatVectorIndex flat_sq8(kDim, DistanceMetric::kL2, VectorQuantization::kSq8);
    std::vector<float> buf(kDim);
    for (int i = 0; i < kVecs; i++) {
      for (size_t d = 0; d < kDim; d++) buf[d] = fdist(rng);
      std::string id = "v" + std::to_string(i);
      flat.Add(id, buf.data());
      flat_cos.Add(id, buf.data());
      flat_sq8.Add(id, buf.data());
    }
    std::vector<float> q(kDim);
    for (size_t d = 0; d < kDim; d++) q[d] = fdist(rng);

    BenchUs("FlatVector L2 KNN-10 (5kx128)", 100, [&]() {
      sink += flat.KnnQuery(q.data(), 10).size();
    });
    BenchUs("FlatVector SQ8 L2 KNN-10 (5kx128)", 100, [&]() {
      sink += flat_sq8.KnnQuery(q.data(), 10).size();
    });
    BenchUs("FlatVector Cos KNN-10 (5kx128)", 100, [&]() {
      sink += flat_cos.KnnQuery(q.data(), 10).size();
    });

    HnswIndex hnsw(kDim, DistanceMetric::kL2, 16, 100);
    auto th0 = Clock::now();
    for (int i = 0; i < 2000; i++) {
      for (size_t d = 0; d < kDim; d++) buf[d] = fdist(rng);
      hnsw.Add("h" + std::to_string(i), buf.data());
    }
    double hnsw_build_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - th0).count();
    std::printf("%-34s  %9.2f ms     (2000x128)\n", "HnswIndex Build (2kx128)", hnsw_build_ms);
    std::string hnsw_blob;
    BenchUs("HnswIndex Serialize (2kx128)", 10, [&]() {
      hnsw_blob = hnsw.SerializeGraph();
      sink += hnsw_blob.size();
    });
    BenchUs("HnswIndex Deserialize (2kx128)", 10, [&]() {
      HnswIndex restored(kDim, DistanceMetric::kL2, 16, 100);
      restored.DeserializeGraph(hnsw_blob.data(), hnsw_blob.size());
      sink += restored.Size();
    });
    BenchUs("HnswIndex L2 KNN-10 (2kx128)", 200, [&]() {
      sink += hnsw.KnnQueryEf(q.data(), 10, 40).size();
    });
    std::vector<std::string> half_cands;
    half_cands.reserve(1000);
    for (int i = 0; i < 2000; i += 2) half_cands.push_back("h" + std::to_string(i));
    BenchUs("HnswIndex Filtered 50% (2kx128)", 200, [&]() {
      sink += hnsw.KnnQueryFiltered(q.data(), 10, half_cands, 40).size();
    });
  }

  // 4. DocumentStore string-to-double sort vs SortableColumn pre-extracted sort (10,000 docs)
  {
    DocumentStore store;
    SortableColumn col;
    std::vector<std::string> ids;
    ids.reserve(10000);
    std::uniform_real_distribution<double> pdist(1.0, 100000.0);
    for (int i = 0; i < 10000; i++) {
      std::string id = "d" + std::to_string(i);
      double v = pdist(rng);
      store.Add(id, {{"price", std::to_string(v)}});
      col.SetNumeric(id, v);
      ids.push_back(id);
    }
    BenchUs("DocStore strtod SORTBY (10k)", 50, [&]() {
      auto copy = ids;
      std::sort(copy.begin(), copy.end(),
                [&](const std::string& a, const std::string& b) {
                  double na = std::strtod(store.Get(a)->fields.at("price").c_str(), nullptr);
                  double nb = std::strtod(store.Get(b)->fields.at("price").c_str(), nullptr);
                  return na < nb;
                });
      sink += copy.size();
    });
    BenchUs("SortableColumn SORTBY (10k)", 50, [&]() {
      std::vector<std::pair<double, size_t>> keyed;
      keyed.reserve(ids.size());
      for (size_t i = 0; i < ids.size(); i++) {
        keyed.emplace_back(col.GetNumeric(ids[i]), i);
      }
      std::sort(keyed.begin(), keyed.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      sink += keyed.size();
    });
  }

  // 5. TagIndex & NumericIndex (30,000 docs)
  {
    TagIndex tidx;
    NumericIndex nidx;
    for (int i = 0; i < 30000; i++) {
      std::string id = "d" + std::to_string(i);
      tidx.Add("cat" + std::to_string(i % 50), id);
      nidx.Add(static_cast<double>(i % 10000), id);
    }
    BenchUs("TagIndex::LookupOr 3 tags (30k)", 200, [&]() {
      sink += tidx.LookupOr({"cat1", "cat2", "cat3"}).size();
    });
    BenchUs("NumericIndex::Range 5% (30k)", 200, [&]() {
      sink += nidx.RangeQuery(1000.0, false, 1500.0, false).size();
    });
  }

  // 6. Binary Quantization (BQ) vs FP32 vs SQ8 (5k x 768d)
  {
    const size_t dim = 768;
    FlatVectorIndex fp32(dim, DistanceMetric::kCosine, VectorQuantization::kNone);
    FlatVectorIndex sq8(dim, DistanceMetric::kCosine, VectorQuantization::kSq8);
    FlatVectorIndex bq(dim, DistanceMetric::kCosine, VectorQuantization::kBq);
    std::vector<float> vec(dim), q(dim);
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (size_t i = 0; i < 5000; i++) {
      for (size_t d = 0; d < dim; d++) vec[d] = dist(rng);
      std::string id = "b" + std::to_string(i);
      fp32.Add(id, vec.data());
      sq8.Add(id, vec.data());
      bq.Add(id, vec.data());
    }
    for (size_t d = 0; d < dim; d++) q[d] = dist(rng);
    BenchUs("FlatVector FP32 Cosine KNN-10 (5kx768)", 100, [&]() {
      sink += fp32.KnnQuery(q.data(), 10).size();
    });
    BenchUs("FlatVector SQ8 Cosine KNN-10 (5kx768)", 100, [&]() {
      sink += sq8.KnnQuery(q.data(), 10).size();
    });
    BenchUs("FlatVector BQ Hamming KNN-10 (5kx768)", 500, [&]() {
      sink += bq.KnnQuery(q.data(), 10).size();
    });

    HnswIndex hnsw_bq(dim, DistanceMetric::kCosine, 16, 100, VectorQuantization::kBq);
    auto tb0 = Clock::now();
    for (size_t i = 0; i < 2000; i++) {
      for (size_t d = 0; d < dim; d++) vec[d] = dist(rng);
      hnsw_bq.Add("hb" + std::to_string(i), vec.data());
    }
    double hnsw_bq_build_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - tb0).count();
    std::printf("%-34s  %9.2f ms     (2000x768)\n", "HnswIndex BQ Build (2kx768)", hnsw_bq_build_ms);
    std::string hnsw_bq_blob;
    BenchUs("HnswIndex BQ Serialize (2kx768)", 10, [&]() {
      hnsw_bq_blob = hnsw_bq.SerializeGraph();
      sink += hnsw_bq_blob.size();
    });
    BenchUs("HnswIndex BQ Deserialize (2kx768)", 10, [&]() {
      HnswIndex restored(dim, DistanceMetric::kCosine, 16, 100, VectorQuantization::kBq);
      restored.DeserializeGraph(hnsw_bq_blob.data(), hnsw_bq_blob.size());
      sink += restored.Size();
    });
    BenchUs("HnswIndex BQ KNN-10 (2kx768)", 500, [&]() {
      sink += hnsw_bq.KnnQueryEf(q.data(), 10, 40).size();
    });

    HnswIndex hnsw_bq_fp16(dim, DistanceMetric::kCosine, 16, 100, VectorQuantization::kBq,
                           VectorRerankType::kFp16, 4);
    for (size_t i = 0; i < 2000; i++) {
      for (size_t d = 0; d < dim; d++) vec[d] = dist(rng);
      hnsw_bq_fp16.Add("hbf" + std::to_string(i), vec.data());
    }
    BenchUs("HnswIndex BQ+FP16 OS4 (2kx768)", 500, [&]() {
      sink += hnsw_bq_fp16.KnnQueryEf(q.data(), 10, 40, 4).size();
    });
  }

  // 7. TextIndex WAND Top-10 vs Full BM25 Search (20k docs)
  {
    TextIndex widx;
    std::vector<std::string> common = {"the", "common", "data", "system", "node"};
    for (int i = 0; i < 20000; i++) {
      std::string text = common[i % 5] + " " + common[(i + 1) % 5];
      if (i % 10 == 0) text += " alpha";
      if (i % 15 == 0) text += " beta";
      if (i % 20 == 0) text += " gamma";
      if (i % 500 == 0) text += " alpha alpha beta gamma";
      widx.Add("w" + std::to_string(i), text);
    }
    std::vector<std::string> q = {"common", "alpha", "beta", "gamma"};
    BenchUs("TextIndex::Search Full BM25 (20k)", 50, [&]() {
      sink += widx.Search(q).size();
    });
    BenchUs("TextIndex::SearchTopK WAND-10 (20k)", 200, [&]() {
      sink += widx.SearchTopK(q, 10).size();
    });
  }

  (void)sink;
  return 0;
}
