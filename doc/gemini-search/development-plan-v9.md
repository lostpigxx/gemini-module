# gemini-search Development Plan v9 — Vector & Search Core Performance (Phases 55–58)

Target platform: **Redis 7** (clean-room C++20).

| Phase | Scope | Status |
|---|---|---|
| **P55** | **1-bit Binary Quantization (BQ) + SIMD Hamming Distance**: `VectorQuantization::kBq` (32× RAM reduction), sign-bit packing, `popcnt`/NEON `vcntq_u8`/AVX2 Hamming distance, integrated into `FlatVectorIndex`, `HnswIndex`, `FT.CREATE ... QUANTIZATION BQ`, `FT.INFO`, and RDB + GTest/bench/Tcl | Done |
| **P56** | **BM25 Top-K WAND Pruning Engine**: Maintain term-level BM25 upper bounds in `TextIndex`, implement `SearchTopK` with WAND pivot jumping & early termination, wire into Top-K BM25 queries + equivalence GTest & benchmark | Pending |
| **P57** | **Per-Index Fine-Grained RWLock + Thread-Safe HNSW + Full-Path Async `FT.SEARCH`**: `thread_local` HNSW visited-epoch buffer, per-index `shared_mutex` in `IndexEntry`, non-KNN async `FT.SEARCH` offload via `SearchExecutor` + concurrent GTest & Tcl tests | Pending |
| **P58** | **Benchmark Summary & Full Docker Redis 7.2.11 Verification**: Refresh `DESIGN.md` & `README.md` with v9 benchmarks, verify full 3-module Linux GCC GTest + ASan/UBSan + Redis 7.2.11 Tcl + RESP2/RESP3 Soak | Pending |
