# gemini-module

> **Release: `v1.0.0`** — Target: **Redis 7** (baseline Redis 7.2.11) · See [CHANGELOG.md](CHANGELOG.md) for release notes.

A collection of Redis 7 modules, built from scratch with C++20.

## Modules

### gemini-bloom — Probabilistic Data Structures

RedisBloom-family probabilistic data types, all served by a single `redis_bloom.so`:

| Type | RDB type name | Commands |
|---|---|---|
| Scalable Bloom Filter | `MBbloom--` (RedisBloom-compatible) | `BF.RESERVE`, `BF.ADD`, `BF.MADD`, `BF.INSERT`, `BF.EXISTS`, `BF.MEXISTS`, `BF.INFO`, `BF.CARD`, `BF.SCANDUMP`, `BF.LOADCHUNK` |
| Scaling Cuckoo Filter | `MBcuckoo-` (private) | `CF.RESERVE`, `CF.ADD`, `CF.ADDNX`, `CF.INSERT`, `CF.INSERTNX`, `CF.EXISTS`, `CF.MEXISTS`, `CF.DEL`, `CF.COUNT`, `CF.INFO`, `CF.SCANDUMP`, `CF.LOADCHUNK` |
| Count-Min Sketch | `MBcms----` (private) | `CMS.INITBYDIM`, `CMS.INITBYPROB`, `CMS.INCRBY`, `CMS.QUERY`, `CMS.MERGE`, `CMS.INFO` |
| Top-K (HeavyKeeper) | `MBtopk---` (private) | `TOPK.RESERVE`, `TOPK.ADD`, `TOPK.INCRBY`, `TOPK.QUERY`, `TOPK.COUNT`, `TOPK.LIST`, `TOPK.INFO` |
| T-Digest | `MBtdigest` (private) | `TDIGEST.CREATE`, `TDIGEST.ADD`, `TDIGEST.RESET`, `TDIGEST.MERGE`, `TDIGEST.QUANTILE`, `TDIGEST.CDF`, `TDIGEST.RANK`, `TDIGEST.REVRANK`, `TDIGEST.BYRANK`, `TDIGEST.BYREVRANK`, `TDIGEST.TRIMMED_MEAN`, `TDIGEST.MIN`, `TDIGEST.MAX`, `TDIGEST.INFO` |

Only the Bloom Filter shares RedisBloom's RDB type name and supports RDB-level migration to/from RedisBloom.
CF / CMS / Top-K / T-Digest are command-compatible but use private RDB formats. CMS / Top-K / T-Digest also
register a private `*.LOADCHUNK` command used only for AOF rewrite replay. See
[modules/gemini-bloom/DESIGN.md](modules/gemini-bloom/DESIGN.md) and
[doc/gemini-bloom/audit-and-optimization-plan.md](doc/gemini-bloom/audit-and-optimization-plan.md) for details.

### gemini-json — JSON Data Type

JSON data type with JSONPath-based operations, compatible with the RedisJSON command set:
`JSON.SET`, `JSON.GET`, `JSON.DEL`, `JSON.TYPE`, `JSON.CLEAR`, `JSON.TOGGLE`,
`JSON.ARRAPPEND`, `JSON.ARRINDEX`, `JSON.ARRINSERT`, `JSON.ARRLEN`, `JSON.ARRPOP`, `JSON.ARRTRIM`,
`JSON.STRAPPEND`, `JSON.STRLEN`, `JSON.NUMINCRBY`, `JSON.NUMMULTBY`,
`JSON.OBJKEYS`, `JSON.OBJLEN`, `JSON.MGET`, `JSON.MSET`, `JSON.MERGE`,
`JSON.DEBUG`, `JSON.RESP`, `JSON.FORGET`.

Key features: compact 24-byte `JsonValue` DOM with ≤21B inline SSO strings, 32-byte SKO `ObjEntry`, contiguous **Typed Numeric Arrays** (`int64_t[]`/`double[]`, 8 B/elem with transparent lazy boxing), ARM NEON / x86-64 SSE2 / SWAR SIMD parser & serializer, RFC 9535 JSONPath filter functions (`length`/`count`/`match`/`search`) with LRU path cache & precompiled regexes, dual-mode RDB (native binary + RedisJSON `COMPAT` text), Redis 7 KeySpecs, and keyspace notifications for `gemini-search` `ON JSON`.
Design doc: [modules/gemini-json/DESIGN.md](modules/gemini-json/DESIGN.md).

### gemini-search — Full-Text, Vector & Hybrid Search

Clean-room search engine targeting **Redis 7**, indexing Redis Hash (`ON HASH`) and JSON (`ON JSON`) documents.

Field types: `TEXT` (BM25 + WAND Top-K pruning, TFIDF/DISMAX/DOCSCORE scoring, English/French/German/Spanish stemming, UTF-8 CJK bigram + custom dictionary FMM segmentation, phonetic matching), `TAG` (`SEPARATOR`, `CASESENSITIVE`), `NUMERIC`, `VECTOR` (`FLAT` / thread-safe dense-ID `HNSW` with direct RDB graph persistence `encver 14`, ARM NEON / AVX2 SIMD kernels, `FLOAT32`/`FLOAT64`/`FLOAT16`/`BFLOAT16`, `QUANTIZATION SQ8` & 1-bit `BQ` with two-stage `RERANK FP16|SQ8` + `OVERSAMPLE` `encver 15`, and selectivity-aware **Adaptive Filtered HNSW**), `GEO` (0.5° spatial grid), `GEOSHAPE` (`FLAT`/`SPHERICAL`).

Query & indexing features: boolean combinations (`DIALECT 1/2`), prefix / suffix / infix / fuzzy / optional terms, synonyms, exact phrase with `SLOP` / `INORDER`, `ismissing(@f)` / `isempty(@f)`, numeric ranges, `GEOFILTER`, KNN (`EF_RUNTIME`, `OVERSAMPLE`, `AS`), `VECTOR_RANGE`, **`FT.HYBRID` (BM25 + Vector RRF/LINEAR fusion)**, per-index fine-grained `shared_mutex` + full-path non-blocking async offload (`FT.SEARCH`, `FT.HYBRID`, `FT.AGGREGATE` via `RedisModule_BlockClient`), timer-driven **background incremental indexing** (`FT.CREATE` via `RedisModule_CreateTimer` + `RedisModule_ScanCursor`, `FT.INFO` `indexing`/`percent_indexed`), `HIGHLIGHT` / `SUMMARIZE`, `SORTBY` / `LIMIT 0 0` / `RETURN AS` / `WITHPAYLOADS` / `WITHSORTKEYS` / `EXPLAINSCORE`, and full Master-Replica `PSYNC` replication.

| Category | Commands |
|---|---|
| Index management | `FT.CREATE`, `FT.ALTER`, `FT.DROPINDEX` (`DD`), `FT.INFO`, `FT._LIST`, `FT.ALIASADD`, `FT.ALIASUPDATE`, `FT.ALIASDEL`, `FT.TAGVALS` |
| Documents (legacy) | `FT.ADD` (`PAYLOAD`), `FT.DEL` |
| Query | `FT.SEARCH`, `FT.HYBRID`, `FT.AGGREGATE` (multi-key `SORTBY MAX`, KNN, rich `APPLY` functions), `FT.CURSOR`, `FT.EXPLAIN`, `FT.EXPLAINCLI`, `FT.PROFILE` |
| Autocomplete | `FT.SUGADD`, `FT.SUGGET`, `FT.SUGDEL`, `FT.SUGLEN` |
| Spellcheck / dictionaries / synonyms | `FT.SPELLCHECK`, `FT.DICTADD`, `FT.DICTDEL`, `FT.DICTDUMP`, `FT.SYNUPDATE`, `FT.SYNDUMP` |
| Configuration / debug | `FT.CONFIG` (`ON_TIMEOUT`, `MAXSEARCHRESULTS`, `ASYNC_MIN_DOCS`, `BG_INDEX_MIN_KEYS`, ...), `FT._DEBUG` |

Design doc: [modules/gemini-search/DESIGN.md](modules/gemini-search/DESIGN.md). Roadmaps & plans:
[Project Roadmap](doc/roadmap.md),
[v1.1 Plan](doc/v1.1-development-plan.md) (P62–65),
[v1](doc/gemini-search/development-plan.md) (P0–12),
[v2](doc/gemini-search/development-plan-v2.md) (P13–26),
[v3](doc/gemini-search/development-plan-v3.md) (P27–31),
[v4](doc/gemini-search/development-plan-v4.md) (P32–35),
[v5](doc/gemini-search/development-plan-v5.md) (P37–41),
[v6](doc/gemini-search/development-plan-v6.md) (P42–46),
[v7](doc/gemini-search/development-plan-v7.md) (P47–51),
[v8](doc/gemini-search/development-plan-v8.md) (P52–54),
[v9](doc/gemini-search/development-plan-v9.md) (P55–58),
[audit & v10](doc/gemini-search/audit-and-optimization-plan.md) (P59–61).

## Project Structure

```
gemini-module/
├── CMakeLists.txt              # Umbrella project
├── Dockerfile.redis7           # Offline Redis 7.2.11 CI image
├── ci/                         # Parallel CI runner, SDK compat, Cluster, Soak & Benchmark scripts
├── include/                    # Shared headers (redismodule.h, test mocks)
├── modules/
│   ├── gemini-bloom/           # Bloom / CF / CMS / Top-K / T-Digest module
│   ├── gemini-json/            # JSON module
│   └── gemini-search/          # Full-text, Vector & Hybrid search module
└── doc/                        # Architecture roadmaps and development plans
```

## Build

Requires CMake 3.14+ and a C++20 compiler.

```bash
cmake -B build
cmake --build build -j$(nproc)
```

Output files:

| Module | Shared Library |
|--------|---------------|
| gemini-bloom | `build/redis_bloom.so` |
| gemini-json | `build/redis_json.so` |
| gemini-search | `build/redis_search.so` |

### AddressSanitizer

```bash
cmake -B build -DENABLE_ASAN=ON
cmake --build build -j$(nproc)
```

## Load Modules

```bash
# Load a single module
redis-server --loadmodule ./build/redis_bloom.so

# Load multiple modules
redis-server \
  --loadmodule ./build/redis_bloom.so \
  --loadmodule ./build/redis_json.so \
  --loadmodule ./build/redis_search.so
```

## Run Tests

### Unit Tests (Google Test)

```bash
# Build and run all tests for a specific module
cmake --build build --target bloom_test
cmake --build build --target json_test
cmake --build build --target search_test
```

If GTest is not in a standard path:

```bash
cmake -B build -DGTest_DIR=/opt/homebrew/lib/cmake/GTest
```

### Integration Tests (TCL)

Requires a `redis-server` binary in `$PATH`:

```bash
# gemini-bloom
tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl ./build/redis_bloom.so

# gemini-bloom Cuckoo Filter / Count-Min Sketch / Top-K / T-Digest
tclsh modules/gemini-bloom/tests/tcl/cf_test.tcl ./build/redis_bloom.so
tclsh modules/gemini-bloom/tests/tcl/cms_test.tcl ./build/redis_bloom.so
tclsh modules/gemini-bloom/tests/tcl/topk_test.tcl ./build/redis_bloom.so
tclsh modules/gemini-bloom/tests/tcl/tdigest_test.tcl ./build/redis_bloom.so

# gemini-json
tclsh modules/gemini-json/tests/tcl/json_test.tcl ./build/redis_json.so

# gemini-search
tclsh modules/gemini-search/tests/tcl/search_test.tcl ./build/redis_search.so
```

### Benchmarks

```bash
# Run Release benchmarks for all 3 modules (bloom_bench / json_bench / search_bench)
bash ci/run_benchmarks.sh
```

## Docker CI

Docker CI 统一面向 **Redis 7**（离线构建，支持无外网环境；`ci/run_tests.sh` 在 Stage 1 完成 Normal + ASan 并行编译后，Stage 2 并行运行全部 **21 个**测试任务，短时长 soak 下约 45 秒完成）：

| 环境 | Redis | RedisBloom | Dockerfile |
|---|---|---|---|
| redis7 | 7.2.11 | 2.6.25 | `Dockerfile.redis7` |

CI 运行以下全量测试：

| 测试项 | 说明 |
|---|---|
| GTest + Fuzz（普通 & ASan） | `bloom_test`（13 套，含 `bloom_fuzz_test`）、`json_test`（6 套，含 `json_stress_test`）、`search_test`（17 套） |
| Tcl 集成测试 | 启动 Redis 7.2 加载模块，验证 `BF.*`、`CF.*`、`CMS.*`、`TOPK.*`、`TDIGEST.*`、`JSON.*`、`FT.*` |
| Compat 对比测试 | 同时启动 gemini-bloom 和 RedisBloom 2.6.25，对比所有 `BF.*` 命令返回值（RESP3 + RESP2） |
| RDB 迁移测试 | 4 方向 RDB 迁入迁出（RB→GB、GB→RB、GB→GB、RB→RB）及多跳链式迁移，验证 `BF.EXISTS` 100% 一致（RESP3 + RESP2） |
| SDK 兼容测试 | `ci/sdk_compat_test.py`，使用 `redis-py` 高层 API 端到端验证三模块（RESP3 + RESP2） |
| Cluster 集成测试 | `ci/cluster_test.py`，3 节点 Redis 7 Cluster 验证跨分片路由、`MOVED`、`CROSSSLOT` 与全部 6 种模块类型的在线 `MIGRATE` |
| Soak 长稳测试 | 持续运行 `gemini-bloom`（BF/CF/CMS/Top-K/T-Digest 全子模块）、`gemini-json`、`gemini-search` 验证不变量与内存无泄漏（RESP3 + RESP2） |

### 运行 Redis 7 CI

```bash
# 构建并运行
docker build -f Dockerfile.redis7 -t gemini-module:redis7 .
docker run --rm gemini-module:redis7

# 缩短 soak 测试时间（默认 bloom 300s / json 30s / search 60s）
docker run --rm \
  -e SOAK_DURATION_SEC=30 \
  -e JSON_SOAK_DURATION_SEC=30 \
  -e SEARCH_SOAK_DURATION_SEC=30 \
  gemini-module:redis7

# 或通过脚本一键构建并运行
bash ci/run_all_envs.sh
```

### 离线构建

所有外部依赖源码以 tarball 形式存放在 `deps/` 目录中，Dockerfile 从本地文件解压编译，支持纯离线构建（无外网内网环境）。
