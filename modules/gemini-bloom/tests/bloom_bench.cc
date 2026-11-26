#include "bloom_filter.h"
#include "cf_chain.h"
#include "cms_sketch.h"
#include "cuckoo_filter.h"
#include "murmur2.h"
#include "sb_chain.h"
#include "tdigest_sketch.h"
#include "topk_sketch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double ElapsedMs(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

std::vector<std::string> GenerateKeys(size_t count, const char* prefix = "item:") {
  std::vector<std::string> keys;
  keys.reserve(count);
  for (size_t i = 0; i < count; i++) {
    keys.push_back(std::string(prefix) + std::to_string(i));
  }
  return keys;
}

void BenchMurmur() {
  std::printf("\n=== MurmurHash ===\n");
  constexpr size_t kN = 1'000'000;
  auto keys = GenerateKeys(kN, "benchmark_key_payload_");
  uint64_t sink = 0;

  auto t0 = Clock::now();
  for (const auto& k : keys) {
    sink ^= MurmurHash2(k.data(), static_cast<int>(k.size()), 0x9747b28cu);
  }
  auto t1 = Clock::now();
  double ms32 = ElapsedMs(t0, t1);
  std::printf("  MurmurHash2   (1M keys): %7.2f ms (%6.2f M ops/s) [sink=%llu]\n",
              ms32, (kN / 1e6) / (ms32 / 1e3), static_cast<unsigned long long>(sink));

  sink = 0;
  t0 = Clock::now();
  for (const auto& k : keys) {
    sink ^= MurmurHash64A(k.data(), static_cast<int>(k.size()), 0xc6a4a7935bd1e995ULL);
  }
  t1 = Clock::now();
  double ms64 = ElapsedMs(t0, t1);
  std::printf("  MurmurHash64A (1M keys): %7.2f ms (%6.2f M ops/s) [sink=%llu]\n",
              ms64, (kN / 1e6) / (ms64 / 1e3), static_cast<unsigned long long>(sink));
}

void BenchBloom() {
  std::printf("\n=== Bloom Filter (NoRound + 64-bit, cap=1M, fp=0.01) ===\n");
  constexpr size_t kN = 500'000;
  auto present = GenerateKeys(kN, "bf_present_");
  auto absent = GenerateKeys(kN, "bf_absent_");

  ScalingBloomFilter bf(1'000'000, 0.01, BloomFlags::Use64Bit | BloomFlags::NoRound, 2);

  auto t0 = Clock::now();
  size_t inserted = 0;
  for (const auto& k : present) {
    auto r = bf.Put(AsBytes(k.data(), k.size()));
    if (r.value_or(false)) inserted++;
  }
  auto t1 = Clock::now();
  double putMs = ElapsedMs(t0, t1);
  std::printf("  Put 500k (single layer): %7.2f ms (%6.2f M ops/s) [ins=%zu, mem=%zu B]\n",
              putMs, (kN / 1e6) / (putMs / 1e3), inserted, bf.BytesUsed());

  t0 = Clock::now();
  size_t hits = 0;
  for (const auto& k : present) {
    if (bf.Contains(AsBytes(k.data(), k.size()))) hits++;
  }
  t1 = Clock::now();
  double hitMs = ElapsedMs(t0, t1);
  std::printf("  Contains 500k (hit)    : %7.2f ms (%6.2f M ops/s) [hits=%zu]\n",
              hitMs, (kN / 1e6) / (hitMs / 1e3), hits);

  t0 = Clock::now();
  size_t fps = 0;
  for (const auto& k : absent) {
    if (bf.Contains(AsBytes(k.data(), k.size()))) fps++;
  }
  t1 = Clock::now();
  double missMs = ElapsedMs(t0, t1);
  std::printf("  Contains 500k (miss)   : %7.2f ms (%6.2f M ops/s) [fp=%zu (%.4f%%)]\n",
              missMs, (kN / 1e6) / (missMs / 1e3), fps, 100.0 * fps / kN);

  // Scaling multi-layer test
  ScalingBloomFilter sbf(10'000, 0.01, BloomFlags::Use64Bit | BloomFlags::NoRound, 2);
  t0 = Clock::now();
  for (size_t i = 0; i < 200'000; i++) {
    (void)sbf.Put(AsBytes(present[i].data(), present[i].size()));
  }
  t1 = Clock::now();
  double scaleMs = ElapsedMs(t0, t1);
  std::printf("  Scaling Put 200k (%zuL) : %7.2f ms (%6.2f M ops/s) [mem=%zu B]\n",
              sbf.NumLayers(), scaleMs, 0.2 / (scaleMs / 1e3), sbf.BytesUsed());
}

void BenchCuckoo() {
  std::printf("\n=== Cuckoo Filter (cap=256k, bucketSize=4, maxIter=20) ===\n");
  constexpr size_t kN = 200'000;
  auto present = GenerateKeys(kN, "cf_present_");
  auto absent = GenerateKeys(kN, "cf_absent_");

  CuckooChain cf(262'144, 4, 20, 2, 32);

  auto t0 = Clock::now();
  size_t added = 0;
  for (const auto& k : present) {
    if (cf.Add(AsBytes(k.data(), k.size())).has_value()) added++;
  }
  auto t1 = Clock::now();
  double addMs = ElapsedMs(t0, t1);
  std::printf("  Add 200k (%zuL)         : %7.2f ms (%6.2f M ops/s) [added=%zu, mem=%llu B]\n",
              cf.NumLayers(), addMs, (kN / 1e6) / (addMs / 1e3), added,
              static_cast<unsigned long long>(cf.BytesUsed()));

  t0 = Clock::now();
  size_t hits = 0;
  for (const auto& k : present) {
    if (cf.Contains(AsBytes(k.data(), k.size()))) hits++;
  }
  t1 = Clock::now();
  double hitMs = ElapsedMs(t0, t1);
  std::printf("  Contains 200k (hit)    : %7.2f ms (%6.2f M ops/s) [hits=%zu]\n",
              hitMs, (kN / 1e6) / (hitMs / 1e3), hits);

  t0 = Clock::now();
  size_t fps = 0;
  for (const auto& k : absent) {
    if (cf.Contains(AsBytes(k.data(), k.size()))) fps++;
  }
  t1 = Clock::now();
  double missMs = ElapsedMs(t0, t1);
  std::printf("  Contains 200k (miss)   : %7.2f ms (%6.2f M ops/s) [fp=%zu]\n",
              missMs, (kN / 1e6) / (missMs / 1e3), fps);

  t0 = Clock::now();
  size_t deleted = 0;
  for (size_t i = 0; i < kN / 2; i++) {
    if (cf.Delete(AsBytes(present[i].data(), present[i].size()))) deleted++;
  }
  t1 = Clock::now();
  double delMs = ElapsedMs(t0, t1);
  std::printf("  Delete 100k            : %7.2f ms (%6.2f M ops/s) [del=%zu]\n",
              delMs, ((kN / 2) / 1e6) / (delMs / 1e3), deleted);
}

void BenchCms() {
  std::printf("\n=== Count-Min Sketch (width=2000, depth=7) ===\n");
  constexpr size_t kN = 300'000;
  auto keys = GenerateKeys(50'000, "cms_key_");

  auto cms = *CmsSketch::Create(2000, 7);

  auto t0 = Clock::now();
  uint64_t sink = 0;
  for (size_t i = 0; i < kN; i++) {
    const auto& k = keys[i % keys.size()];
    sink ^= cms.IncrBy(AsBytes(k.data(), k.size()), 1).value_or(0);
  }
  auto t1 = Clock::now();
  double incrMs = ElapsedMs(t0, t1);
  std::printf("  IncrBy 300k            : %7.2f ms (%6.2f M ops/s) [sink=%llu]\n",
              incrMs, (kN / 1e6) / (incrMs / 1e3), static_cast<unsigned long long>(sink));

  t0 = Clock::now();
  sink = 0;
  for (size_t i = 0; i < kN; i++) {
    const auto& k = keys[i % keys.size()];
    sink ^= cms.Query(AsBytes(k.data(), k.size()));
  }
  t1 = Clock::now();
  double queryMs = ElapsedMs(t0, t1);
  std::printf("  Query 300k             : %7.2f ms (%6.2f M ops/s) [sink=%llu]\n",
              queryMs, (kN / 1e6) / (queryMs / 1e3), static_cast<unsigned long long>(sink));

  auto dst = *CmsSketch::Create(65536, 7);
  auto s1 = *CmsSketch::Create(65536, 7);
  auto s2 = *CmsSketch::Create(65536, 7);
  for (size_t i = 0; i < 1000; i++) {
    (void)s1.IncrBy(AsBytes(keys[i].data(), keys[i].size()), 3);
    (void)s2.IncrBy(AsBytes(keys[i].data(), keys[i].size()), 5);
  }
  const CmsSketch* srcs[2] = {&s1, &s2};
  double weights[2] = {1.0, 2.0};
  t0 = Clock::now();
  for (int r = 0; r < 100; r++) {
    (void)dst.Merge(srcs, weights);
  }
  t1 = Clock::now();
  double mergeMs = ElapsedMs(t0, t1);
  std::printf("  Merge 100x (65536x7)   : %7.2f ms (%6.2f merges/s)\n",
              mergeMs, 100.0 / (mergeMs / 1e3));
}

void BenchTopk() {
  std::printf("\n=== Top-K (HeavyKeeper) ===\n");
  constexpr size_t kN = 100'000;
  auto keys = GenerateKeys(10'000, "topk_item_");

  // Small k = 20
  {
    auto tk = *TopkSketch::Create(20, 2000, 7, 0.9);
    auto t0 = Clock::now();
    for (size_t i = 0; i < kN; i++) {
      // Skewed distribution
      size_t idx = (i % 5 == 0) ? (i % 20) : (i % keys.size());
      (void)tk.Add(AsBytes(keys[idx].data(), keys[idx].size()));
    }
    auto t1 = Clock::now();
    double ms = ElapsedMs(t0, t1);
    std::printf("  Add 100k (k=20)        : %7.2f ms (%6.2f M ops/s)\n",
                ms, (kN / 1e6) / (ms / 1e3));

    t0 = Clock::now();
    for (size_t i = 0; i < 100; i++) {
      (void)tk.IncrBy(AsBytes(keys[i].data(), keys[i].size()), 1000);
    }
    t1 = Clock::now();
    double incMs = ElapsedMs(t0, t1);
    std::printf("  IncrBy 100x1000 (k=20) : %7.2f ms (%6.2f K calls/s)\n",
                incMs, 0.1 / (incMs / 1e3));
  }

  // Large k = 1000
  {
    auto tk = *TopkSketch::Create(1000, 4000, 7, 0.9);
    auto t0 = Clock::now();
    for (size_t i = 0; i < kN; i++) {
      size_t idx = (i % 4 == 0) ? (i % 1000) : (i % keys.size());
      (void)tk.Add(AsBytes(keys[idx].data(), keys[idx].size()));
    }
    auto t1 = Clock::now();
    double ms = ElapsedMs(t0, t1);
    std::printf("  Add 100k (k=1000)      : %7.2f ms (%6.2f M ops/s)\n",
                ms, (kN / 1e6) / (ms / 1e3));

    t0 = Clock::now();
    size_t found = 0;
    for (size_t i = 0; i < kN; i++) {
      const auto& k = keys[i % 2000];
      if (tk.Query(AsBytes(k.data(), k.size()))) found++;
    }
    t1 = Clock::now();
    double qMs = ElapsedMs(t0, t1);
    std::printf("  Query 100k (k=1000)    : %7.2f ms (%6.2f M ops/s) [found=%zu]\n",
                qMs, (kN / 1e6) / (qMs / 1e3), found);
  }
}

void BenchTdigest() {
  std::printf("\n=== T-Digest (compression=100, 200k obs) ===\n");
  constexpr size_t kN = 200'000;
  std::mt19937_64 rng(42);
  std::exponential_distribution<double> expDist(1.0);
  std::vector<double> data(kN);
  for (size_t i = 0; i < kN; i++) data[i] = expDist(rng);

  auto td = *TdigestSketch::Create(100.0);
  auto t0 = Clock::now();
  for (double v : data) {
    (void)td.Add(v);
  }
  td.Compress();
  auto t1 = Clock::now();
  double addMs = ElapsedMs(t0, t1);
  std::printf("  Add+Compress 200k      : %7.2f ms (%6.2f M ops/s) [centroids=%u]\n",
              addMs, (kN / 1e6) / (addMs / 1e3), td.NumCentroids());

  // Accuracy check on exponential (heavy tail) distribution
  std::vector<double> sorted = data;
  std::sort(sorted.begin(), sorted.end());
  auto ExactQ = [&](double q) {
    size_t idx = static_cast<size_t>(q * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
  };
  double qs[] = {0.5, 0.9, 0.99, 0.999, 0.9999};
  std::printf("  Tail accuracy (Exp(1)) :");
  for (double q : qs) {
    double est = td.Quantile(q);
    double exact = ExactQ(q);
    double relErr = std::fabs(est - exact) / std::max(exact, 1e-12) * 100.0;
    std::printf(" p%.2f=%.2f%%", q * 100.0, relErr);
  }
  std::printf("\n");

  t0 = Clock::now();
  double qSink = 0.0;
  for (int i = 0; i < 100'000; i++) {
    qSink += td.Quantile(qs[i % 5]);
  }
  t1 = Clock::now();
  double qMs = ElapsedMs(t0, t1);
  std::printf("  Quantile 100k          : %7.2f ms (%6.2f M ops/s) [sink=%.2f]\n",
              qMs, 0.1 / (qMs / 1e3), qSink);

  t0 = Clock::now();
  double cdfSink = 0.0;
  for (int i = 0; i < 100'000; i++) {
    cdfSink += td.Cdf(data[i % 1000]);
  }
  t1 = Clock::now();
  double cdfMs = ElapsedMs(t0, t1);
  std::printf("  CDF 100k               : %7.2f ms (%6.2f M ops/s) [sink=%.2f]\n",
              cdfMs, 0.1 / (cdfMs / 1e3), cdfSink);
}

}  // namespace

int main() {
  std::printf("=== gemini-bloom Benchmark Suite ===\n");
  BenchMurmur();
  BenchBloom();
  BenchCuckoo();
  BenchCms();
  BenchTopk();
  BenchTdigest();
  return 0;
}
