# gemini-search Development Plan v5 — Performance, Security & Protocol Alignment (Phases 37–41)

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

| Phase | Scope | Status |
|---|---|---|
| **P37** | Reproducible standalone `search_bench` benchmark suite | Done |
| **P38** | Ordered term & reversed-term indices in `TextIndex` ($O(\log V + k)$ prefix/suffix); 0.5° spatial grid in `GeoIndex` | Done |
| **P39** | Pre-parsed `SortableColumn` (`sort_index.h`) for `SORTABLE` fields; contiguous flat float buffer + 4-way unrolled distance kernels in `FlatVectorIndex` | Done |
| **P40** | Security/boundary hardening (`kMaxDepth=64` recursion guards in query & expression parsers, `DIM<=65536`, finite `WEIGHT` & `GEOFILTER` bounds, checked `FT.PROFILE` counts) + real Docker Redis 7.2.11 offline CI verification (Linux GCC/GNU ld, Linux ASan, Redis 7 Tcl suite incl. `COPY` & `aux_save2`, RESP3/RESP2 soak) | Done |
| **P41** | Protocol & ecosystem alignment: `LIMIT 0 0` total-count fast path, `FT.ADD PAYLOAD` + `FT.SEARCH WITHPAYLOADS`, `INDEXMISSING`/`INDEXEMPTY` + `ismissing(@f)`/`isempty(@f)` (`encver 11`), `RedisModule_ReplicateVerbatim` on all write commands + real Redis 7.2.11 Master-Replica `PSYNC` & live replication verification | Done |
