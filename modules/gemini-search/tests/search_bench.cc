#include "document_store.h"
#include "geo_index.h"
#include "hnsw_index.h"
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
    std::vector<float> buf(kDim);
    for (int i = 0; i < kVecs; i++) {
      for (size_t d = 0; d < kDim; d++) buf[d] = fdist(rng);
      std::string id = "v" + std::to_string(i);
      flat.Add(id, buf.data());
      flat_cos.Add(id, buf.data());
    }
    std::vector<float> q(kDim);
    for (size_t d = 0; d < kDim; d++) q[d] = fdist(rng);

    BenchUs("FlatVector L2 KNN-10 (5kx128)", 100, [&]() {
      sink += flat.KnnQuery(q.data(), 10).size();
    });
    BenchUs("FlatVector Cos KNN-10 (5kx128)", 100, [&]() {
      sink += flat_cos.KnnQuery(q.data(), 10).size();
    });
  }

  // 4. DocumentStore string-to-double sort baseline (10,000 docs)
  {
    DocumentStore store;
    std::vector<std::string> ids;
    ids.reserve(10000);
    std::uniform_real_distribution<double> pdist(1.0, 100000.0);
    for (int i = 0; i < 10000; i++) {
      std::string id = "d" + std::to_string(i);
      store.Add(id, {{"price", std::to_string(pdist(rng))}});
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
  }

  (void)sink;
  return 0;
}
