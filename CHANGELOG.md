# Changelog

All notable changes to `gemini-module` are documented in this file.

## [Unreleased]

### gemini-json (`redis_json.so`)
- **Typed Numeric Arrays (`int64_t[]` / `double[]`)**: Pure integer and numeric arrays are now stored contiguously at `8 B/elem` inside the existing 24-byte `JsonValue` layout (`kFlagIntArray` / `kFlagDoubleArray`) with zero per-element heap allocations, lossless `int -> double` promotion (`|v| <= 2^53`), transparent lazy boxing (`EnsureBoxedArray`) on mixed types or element mutation, and zero-boxing fast paths across Parser, Serializer, `Clone`/`Destroy`/`Defrag`, `Digest`, binary RDB (`kTagIntArray`/`kTagDoubleArray`, backward-compatible with `kTagArray`), `JSON.ARR*`, and `JSON.RESP`. Cuts 100k int array DOM footprint from `3,367 KB` to **`1,024 KB` (-69.6%)**.

### gemini-search (`redis_search.so`)
- **Phase 62 — HNSW Direct RDB Graph Persistence (`encver 14`)**: Compact live-node serialization & strict bounds-checked deserialization of `HnswIndex` layered neighbor graphs (`FP32` / `SQ8` / `BQ`), cutting 2k×768d reload time from `~91 ms` to `0.16 ms` (**~550× faster**).
- **Phase 63 — BQ Two-Stage Oversample & Rerank (`encver 15`)**: Optional `RERANK FP16|SQ8` and `OVERSAMPLE <n>` on `QUANTIZATION BQ` fields (with query-level override in `FT.SEARCH`, `FT.AGGREGATE`, and `FT.HYBRID ... VSIM`), combining BQ coarse speed with FP16/SQ8 precision recall.
- **Phase 64 — Background Incremental Async Indexing**: Non-blocking `FT.CREATE` initial scan via Redis 7 `RedisModule_CreateTimer` + `RedisModule_ScanCursor` time-sliced batches (`BG_INDEX_MIN_KEYS`, `BG_INDEX_BATCH_KEYS`, `BG_INDEX_TICK_MS`), `FT.INFO` `indexing`/`percent_indexed` progress reporting, and safe cancellation on `FT.DROPINDEX`/`FLUSHDB`.
- **Phase 65 — Adaptive Filtered HNSW**: Selectivity-aware `HnswIndex::KnnQueryFiltered` that dynamically switches between exact brute-force (low selectivity / small candidate sets) and zero-allocation epoch-bitset Filtered HNSW graph traversal (medium/high selectivity, with connectivity-preserving hops, `ef_runtime` scaling, BQ+FP16/SQ8 rerank, and exact fallback).
- **Phases 66–72 — Redis Cluster Distributed Scatter-Gather**:
  - **Phase 66 — Cluster Infrastructure, Binary RPC & Topology Discovery**: Zero-overhead standalone guard, Cluster Bus `Hello` port/epoch auto-discovery, non-blocking multiplexed binary TCP RPC (`GMSC` magic, dual header/payload CRC32, 32 MiB frame guard), `FT._DEBUG CLUSTER [PING]`, `CLUSTER_*` `FT.CONFIG` controls, and R5 `std::shared_ptr<IndexEntry>` safe index lifetime.
  - **Phase 67 — Distributed DDL, `aux_save2` v2 Persistence & Schema Self-Healing**: Two-step DDL broadcast (`FT.CREATE`/`DROPINDEX`/`ALTER`/`ALIAS*`/`SYNUPDATE`/`DICT*`) with automatic compensation rollback on shard conflict, `kSearchAuxEncVer = 2` cluster persistence (no bare Redis DB index keys) + HNSW restart recovery, and `kRpcSchemaSyncReq/Resp` automatic catch-up for newly joined/lagging nodes.
  - **Phase 68 — `FT.SEARCH` Query-Then-Fetch & $M$-way Merger**: Lock-only remote shard execution (zero remote Redis main-thread touch), $M$-way heap merger (`cluster_merger`) with deterministic `doc_id` deduplication across BM25 / `SORTBY` / KNN (`FLAT`/`HNSW`/`SQ8`/`BQ+Rerank`/Filtered HNSW/`VECTOR_RANGE`), verified by 10k-doc GTest and 3,000-doc / 33-query standalone golden parity test.
  - **Phase 69 — `FT.HYBRID` Global Fusion, 2-Phase BM25 DFS & Cluster Aux Commands**: Global RRF/LINEAR fusion across shards, optional 2-Phase BM25 DFS (`CLUSTER_BM25_DFS`) for exact global IDF parity under heavy shard skew, cluster `FT.INFO` (including weighted background indexing progress & `cluster_shards`), `FT.TAGVALS`, `FT.SPELLCHECK`, slot-routed `FT.SUG*`, and `FT.PROFILE SEARCH`.
  - **Phase 70 — `FT.AGGREGATE` Two-Phase Map-Reduce & Cluster `FT.CURSOR`**: Serializable partial states & parallel reducers for all 12 `REDUCE` functions (Chan parallel `STDDEV`, 14-bit sparse/dense HLL `COUNT_DISTINCTISH`, exact-then-T-Digest `QUANTILE`, weighted reservoir `RANDOM_SAMPLE`, etc.), `ClusterAggPlanner` pushdown split, `CLUSTER_AGG_MAX_GROUPS` quota protection, and 24-bit node-tagged cross-node `FT.CURSOR READ`/`DEL`/`MAXIDLE`.
  - **Phase 71 — Fault Tolerance, Live `MIGRATE` Consistency, Failover & Security Hardening**: Stateless keyed 64-bit MAC authentication (`CLUSTER_AUTH_TOKEN`), dynamic cluster peer IP whitelist, server/client inflight RPC concurrency quotas, slot-aware Master-Replica failover (`owns_slots` via `CLUSTER SLOTS` + fast `FAIL`/`PFAIL` disconnect), unified `ON_TIMEOUT FAIL` vs `RETURN` + `cluster_partial_results_total` metric, verified with concurrent 500-slot live `MIGRATE` and 6-node (3M+3R) `SIGKILL` failover tests.
  - **Phase 72 — Cluster Soak, Benchmarks & Documentation**: Added `ci/search_cluster_soak_test.py` (3-node mixed `HSET`/`JSON.SET`/`DEL`/`FT.SEARCH`/`FT.HYBRID`/`FT.AGGREGATE`/`FT.CURSOR`/`MIGRATE` soak wired into `ci/run_tests.sh` as job #22; 59.9k ops in 20s with `+0.12 MB` memory delta) and `ci/search_cluster_bench.py` (12k-doc 1N vs 3N scaling: `4.2×–5.9×` QPS scaling on BM25/Deep/Hybrid/Aggregate, `~0.50 ms` coordinator RPC overhead on HNSW KNN-10, `65%–98%` Query-Then-Fetch bandwidth savings).
- **Client SDK Compatibility Fixes**: Added `FT.CREATE` `SCORE`/`SCORE_FIELD`/`PAYLOAD_FIELD`/`NOFIELDS` and standard `VECTOR FLAT|HNSW <attr_count>` parsing, fixed `ON HASH` auto-indexing of `VECTOR` fields in `IndexHashKey`, and added `SCORER`/`VERBATIM`/`ADDSCORES` support in `FT.AGGREGATE`.

### Tooling, Tests & Documentation
- Parallelized all 22 Docker CI test jobs in `ci/run_tests.sh` (Stage 1 Normal + ASan parallel build, Stage 2 22 concurrent jobs including `ci/cluster_test.py` and `ci/search_cluster_soak_test.py`) and added `.dockerignore` (`build_*/`, `.git/`), cutting Docker build context from `1.28 GB` to `<500 KB` and full CI wall time from `~6.5 min` to `~60s`.
- Expanded `ci/cluster_test.py` to 9 end-to-end Redis 7 Cluster test stages covering all 3 modules (routing/`MOVED`/`CROSSSLOT`/6-type `MIGRATE` + `gemini-search` Phases 66–71 including 3,000-doc golden parity, concurrent 500-slot `MIGRATE`, and 6-node 3M+3R failover + RPC auth).
- Fixed `CMS.MERGE` and `TDIGEST.MERGE` in `gemini-bloom` to register `"getkeys-api"` (`RedisModule_IsKeysPositionRequest`) alongside Redis 7 `SetCommandInfo` KeySpecs so Redis Cluster enforces `CROSSSLOT` on all source keys.
- Added `ci/sdk_compat_test.py` (`redis-py` end-to-end compatibility test across all 3 modules for RESP2 & RESP3) and wired it into `ci/run_tests.sh`.
- Added unified `ci/run_benchmarks.sh` script for `bloom_bench`, `json_bench`, and `search_bench`.
- Added `modules/gemini-json/DESIGN.md`, `doc/roadmap.md`, and `doc/v1.1-development-plan.md`, and updated `.gitignore` for `build_*` directories.

## [v1.0.0] - 2026-11-26

First production-ready release of `gemini-module`, a C++20 Redis 7 module suite containing **gemini-bloom**, **gemini-json**, and **gemini-search**, fully verified under GTest, ASan/UBSan, fuzz/differential testing, and Docker Redis 7.2.11 CI (Tcl, Compat, Soak, and RDB Migration).

### Target Platform
- Unified on **Redis 7** (baseline Redis 7.2.11) with full Redis 7 Module API support (`mem_usage2`, `copy2`, `free_effort2`, `aux_save2`, KeySpecs, RESP2/RESP3 dual protocol, and `PSYNC` replication).

---

### gemini-bloom (`redis_bloom.so`)
- **5 Probabilistic Data Structures**: Scalable Bloom Filter (`MBbloom--`, RDB-compatible with RedisBloom 2.6), Scaling Cuckoo Filter (`MBcuckoo-`), Count-Min Sketch (`MBcms----`), Top-K HeavyKeeper (`MBtopk---`), and T-Digest (`MBtdigest`).
- **Performance & Memory Optimizations (Phases 1–5)**:
  - Bloom: word-aligned MurmurHash, `FastMod64` division-free modulo, software prefetching, zero-copy RDB load.
  - Cuckoo: LUT fingerprint hashing, stack-allocated kick path, borrow-free SWAR byte counting.
  - CMS & Top-K: `FastMod32` + integer merge, precomputed decay LUT, batched `IncrBy`, `O(1)` hash-indexed HeavyKeeper heap, and 32 MiB chunked AOF rewrite (`CMS.LOADCHUNK` / `TOPK.LOADCHUNK`).
  - T-Digest: Dunning-Ertl `k1` scale function with accurate tail interpolation (p99.9 error < 1%), `O(log C)` zero-allocation rank/quantile queries, and read-only replica support (`REPLICAOF`).
- **Hardening & Verification**: Redis 7 KeySpecs, `mem_usage2`, RDB truncation/corruption fuzz suite, 370/370 RedisBloom 2.6 compatibility tests (RESP3 & RESP2), 4-direction RDB migration tests, and 5-submodule soak tests.

---

### gemini-json (`redis_json.so`)
- **RedisJSON-Compatible Command Set**: `JSON.SET`, `JSON.GET`, `JSON.DEL`/`FORGET`, `JSON.TYPE`, `JSON.CLEAR`, `JSON.TOGGLE`, `JSON.ARR*`, `JSON.STR*`, `JSON.NUM*`, `JSON.OBJ*`, `JSON.MGET`, `JSON.MSET`, `JSON.MERGE`, `JSON.DEBUG`, `JSON.RESP`.
- **Performance & Architecture**:
  - Compact 24-byte `JsonValue` DOM + 32-byte small-key-optimized `ObjEntry`.
  - Fast parser/serializer with ARM NEON / SSE2 / SWAR SIMD string scanning.
  - Full JSONPath engine with RFC 9535 filter functions, precompiled regexes, and path cache.
  - Fast RDB serialization/deserialization + keyspace notifications for `gemini-search` `ON JSON` integration.
- **Hardening & Verification**: DoS evaluation budgets, depth/OOM guards, RDB byte-by-byte truncation/corruption fuzzing, and RESP3/RESP2 soak tests.

---

### gemini-search (`redis_search.so`)
- **Clean-Room Full-Text, Vector & Hybrid Search (Phases 0–61)**:
  - **Field Types**: `TEXT` (BM25, TFIDF, DISMAX, DOCSCORE, English/French/German/Spanish stemming, CJK bigram + custom dictionary FMM segmentation, Double Metaphone phonetic), `TAG` (`SEPARATOR`, `CASESENSITIVE`), `NUMERIC` (including multi-value arrays), `VECTOR` (`FLAT` & thread-safe dense-ID `HNSW`, NEON/AVX2 SIMD, `FLOAT32`/`FLOAT64`/`FLOAT16`/`BFLOAT16`, `SQ8` 4× & 1-bit `BQ` 32× quantization), `GEO` (0.5° spatial grid), `GEOSHAPE` (`FLAT`/`SPHERICAL`).
  - **Query Engine**: `DIALECT 1/2`, boolean combinations, prefix/suffix/infix/fuzzy/optional terms, synonyms, exact phrase with `SLOP`/`INORDER`, `ismissing(@f)`/`isempty(@f)`, `VECTOR_RANGE`, `FT.HYBRID` (RRF/LINEAR fusion), `FT.AGGREGATE` + `FT.CURSOR`, `HIGHLIGHT`/`SUMMARIZE`, `FT.SUG*`, `FT.SPELLCHECK`, `FT.PROFILE`.
  - **Concurrency & Performance (Phases 56–60)**: Per-index `shared_mutex` + full-path non-blocking async offload (`FT.SEARCH`, `FT.HYBRID`, `FT.AGGREGATE` via `RedisModule_BlockClient`); pre-quantized BQ HNSW + zero-allocation `GreedySearchLayer` (`7.47 us/op` 2k×768d KNN-10); WAND Top-K BM25 pruning wired into `FT.SEARCH`.
  - **Persistence & `ON JSON` (Phases 59 & 61)**: RDB `encver 13` (persisting `QUANTIZATION BQ`, `DICT`, `LANGUAGE_FIELD`, backward-compatible with `1..12`), `aux_save2`/`aux_load`, `AofRewriteSearch` closure, `mem_usage2`, RDB byte-by-byte fuzzing, and `ON JSON` multi-value array wildcard indexing (`$.tags[*]`, `$.items[*].title`, `$.prices[*]`).
- **Verification**: 17 GTest suites (normal & ASan/UBSan), 380/380 Tcl integration tests (including `ON JSON` + `gemini-json`, BGSAVE/AOF restart, and `PSYNC` replication), and RESP3/RESP2 soak tests (520k+ ops each).
