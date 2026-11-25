# gemini-search Development Plan v7 — Core Engine Architecture & Concurrency (Phases 47–51)

Target platform: **Redis 7** (clean-room C++20).
Every phase ships with before/after `search_bench` metrics, comprehensive GTest unit tests (incl. ASan/UBSan & multi-threaded stress tests), and Redis 7.2.11 Tcl integration tests.

## Architectural Motivation

Through v6 (Phases 1–46), `gemini-search` achieved broad feature parity, strong security hardening, and full Redis 7 lifecycle/replication support. Profiling the core index structures reveals four remaining architectural ceilings:

1. **String-keyed inverted indices & $O(V)$ deletes**: `TextIndex::Posting` stores `std::string doc_id` and `TextIndex::Remove` scans the entire vocabulary ($O(V)$); `TagIndex` and `NumericIndex` use `std::set<std::string>` (red-black tree nodes, ~72B/entry + string allocations).
2. **Scalar vector distance kernels**: `L2Distance`, `CosineDistance`, `InnerProductDistance`, and `Sq8Distance` use scalar loops rather than hardware SIMD (ARM NEON / x86-64 AVX2).
3. **Single-threaded execution on Redis main loop**: Heavy KNN, `VECTOR_RANGE`, and `FT.HYBRID` queries hold the Redis main thread until completion.
4. **Documentation & Release 1.0 alignment**: `README.md` and `modules/gemini-search/DESIGN.md` need full sync with Phases 32–50 and unified 3-module benchmarks.

---

## Phased Plan (Phases 47–51)

| Phase | Scope | Key Deliverables | Verification & Benchmarks | Status |
|---|---|---|---|---|
| **P47** | **Dense `uint32_t` Doc-ID `TextIndex` + $O(\text{doc\_terms})$ Delete** | Replace `std::string doc_id` in `TextIndex::Posting` with dense `uint32_t` IDs (`id_to_str_`, `str_to_id_`, `free_ids_`); add per-doc forward term list (`doc_terms_`) to eliminate $O(V)$ `Remove()` scans; keep postings sorted by `uint32_t` ID for fast intersection | `search_bench`: `TextIndex::Remove` (30k vocab) & `TextIndex::Search` speedup; GTest slot-reuse, BM25, phrase, CJK, and ASan tests | Done |
| **P48** | **Compact Integer-ID `TagIndex` & `NumericIndex`** | Replace `std::set<std::string>` in `TagIndex` and `NumericIndex` with dense `uint32_t` doc IDs + sorted `std::vector<uint32_t>` (with flat `(double, uint32_t)` range storage for `NumericIndex`) and fast sorted-vector merge/intersection | `search_bench`: `TagIndex::LookupOr` & `NumericIndex::RangeQuery` (30k docs) speedup + memory reduction; full GTest + ASan + Tcl tests | Done |
| **P49** | **Hardware SIMD Vector Kernels (ARM NEON + x86-64 AVX2)** | Add compile-time dispatched SIMD implementations for `L2Distance`, `CosineDistance`, `InnerProductDistance`, and `Sq8Distance` (`#if defined(__aarch64__)` NEON `<arm_neon.h>`, `#elif defined(__AVX2__)` AVX2+FMA `<immintrin.h>`, scalar fallback) with exact numerical parity (`1e-5` tolerance) across arbitrary/non-multiple-of-4 dimensions | `search_bench`: `FlatVector` & `HnswIndex` (128d / 768d) L2, Cosine, IP, SQ8 speedup; GTest parity tests across dims `1, 3, 4, 7, 8, 15, 16, 128, 384, 768` | Done |
| **P50** | **Non-Blocking Async Query Offload (`RedisModule_BlockClient` + Thread Pool)** | Add `SearchExecutor` worker thread pool (`search_executor.h/.cc`) + per-index `std::shared_mutex`; writers (`FT.ADD`/`FT.DEL`/`FT.CREATE`/`FT.DROPINDEX`/keyspace) acquire `unique_lock`, async readers acquire `shared_lock`; offload heavy read-only `FT.SEARCH` (KNN) and `FT.HYBRID` via `RedisModule_BlockClient` when index size >= `FT.CONFIG` `ASYNC_MIN_DOCS` (default `0` = off or configurable threshold); worker threads strictly touch only C++ index state and return plain `QueryReplyData` to main-thread `reply_callback` | Multi-threaded GTest concurrency stress test (concurrent readers + writers under ASan/TSan-safe locking); Redis 7.2.11 Tcl concurrent client test (verifying `PING`/`GET` stay responsive during async search + `FT.CONFIG ASYNC_MIN_DOCS`) | Done |
| **P51** | **v1.0 Documentation Sync & Full 3-Module Release Verification** | Update root `README.md` and `modules/gemini-search/DESIGN.md` with v4–v7 architecture, command matrix, and benchmark tables; run full Docker Redis 7.2.11 3-module CI (`ci/run_tests.sh`) | Full 3-module CI green (GTest, ASan, Tcl, Compat, Migrate, RESP3/RESP2 Soak) | Done |

---

## Design Guardrails
- **Zero API / Wire Regressions**: All 371+ Redis 7 Tcl tests and 16+ GTest binaries must remain 100% green at every phase boundary.
- **Cross-Platform Pure Core**: `TextIndex`, `TagIndex`, `NumericIndex`, `VectorIndex`, `HnswIndex`, and `SearchExecutor` remain free of `redismodule.h` so standalone GTest & `search_bench` link cleanly under both Apple ld64 and Linux GNU ld.
- **Redis Thread-Safety Contract (P50)**: Background worker threads NEVER call `RedisModule_*` key/reply APIs; all Redis API calls happen exclusively on the main thread before `BlockClient` and inside `reply_callback` / `free_privdata`.
