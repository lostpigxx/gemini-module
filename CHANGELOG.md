# Changelog

All notable changes to `gemini-module` are documented in this file.

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
