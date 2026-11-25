# gemini-search Development Plan v5 — Performance Optimization (Phases 37–39)

Target platform: **Redis 7** (clean-room C++20).

## Benchmark Summary (Apple Silicon, `-O3`, `search_bench`)

| Workload | Baseline (P37) | Optimized (P38–P39) | Speedup |
|---|---:|---:|---:|
| `TextIndex::PrefixLookup` (30k terms) | 190.08 us/op | 6.37 us/op | **29.8×** |
| `TextIndex::SuffixLookup` (30k terms) | 179.88 us/op | 0.08 us/op | **~2,200×** |
| `GeoIndex::RadiusQuery 10km` (30k points) | 468.16 us/op | 0.01 us/op | **>40,000×** |
| `GeoIndex::RadiusQuery 50km` (30k points) | 463.66 us/op | 0.04 us/op | **>11,000×** |
| `FlatVector L2 KNN-10` (5k × 128d) | 161.35 us/op | 114.33 us/op | **1.41×** |
| `FlatVector Cos KNN-10` (5k × 128d) | 243.25 us/op | 117.88 us/op | **2.06×** |
| `SORTBY` numeric (10k docs) | 7,622.66 us/op | 300.75 us/op | **25.3×** |

## Milestone Summary

| Phase | Optimization | Status |
|---|---|---|
| **P37** | Reproducible standalone `search_bench` benchmark suite | Done |
| **P38** | Ordered term & reversed-term indices in `TextIndex` ($O(\log V + k)$ prefix/suffix); 0.5° spatial grid in `GeoIndex` | Done |
| **P39** | Pre-parsed `SortableColumn` (`sort_index.h`) for `SORTABLE` fields; contiguous flat float buffer + 4-way unrolled distance kernels in `FlatVectorIndex` | Done |
