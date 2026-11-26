# gemini-module Post-v1.0.0 演进规划（Roadmap）

> 基线版本：**`v1.0.0`**（2026-11-26，Redis 7.2.11，C++20 Clean-room）  
> 现有模块：`gemini-bloom`（BF/CF/CMS/Top-K/T-Digest）、`gemini-json`、`gemini-search`

---

## 一、v1.1 — 生产可用性与工程化闭环（✅ 已完成开发，待发版）

> 详细实施方案见 [v1.1-development-plan.md](./v1.1-development-plan.md)。以下条目均已合入 `main` 并通过本地 macOS（Normal + ASan）及 Docker Redis 7.2.11 全量 21 项并行 CI 验证。

1. **✅ 工程化、CI 并行化与文档收口**
   - 清理 `.gitignore`（忽略 `build_*` 构建目录），新增 `.dockerignore` 并将 `ci/run_tests.sh` 改造为 21 任务全并行（全量 CI 从 ~6.5 min 降至 ~45s）。
   - 新增统一三模块 Benchmark 回归脚本 `ci/run_benchmarks.sh`（`bloom_bench` / `json_bench` / `search_bench`）。
   - 补齐 [modules/gemini-json/DESIGN.md](../modules/gemini-json/DESIGN.md) 设计文档。
2. **✅ `gemini-search` v1.1 核心能力升级（Phases 62–65）**
   - **P62 — HNSW 图结构直接 RDB 序列化（`encver 14`）**：持久化 `HnswIndex` 分层邻接表与量化向量数据，2k×768d 重启加载从 `~91 ms` 降至 `0.16 ms`（~550× 提速）。
   - **P63 — BQ 两阶段检索（Oversample & Rerank，`encver 15`）**：在保留 BQ 32× 压缩与超低延迟粗排的同时，支持可选 `RERANK FP16|SQ8` 与 `OVERSAMPLE`（索引级与查询级覆盖）。
   - **P64 — 后台增量异步建索引（Background Indexing）**：将 `FT.CREATE` 存量扫描改造为基于 Redis 7 Timer + ScanCursor 的时间片异步推进（`BG_INDEX_MIN_KEYS` / `BG_INDEX_BATCH_KEYS` / `BG_INDEX_TICK_MS`），并在 `FT.INFO` 中暴露 `indexing` 与 `percent_indexed`。
   - **P65 — 自适应 Filtered HNSW**：针对 `(@tag:{...} @ts:[...])=>[KNN ...]` 过滤联合检索，按过滤选择度动态切换暴力扫描（低选择度/小候选集）与零分配 epoch-bitset 连通性保持 Filtered HNSW 图遍历（中高选择度），兼容 `EF_RUNTIME` 与 BQ+FP16/SQ8 重排。
3. **✅ 主流客户端 SDK 端到端兼容性测试**
   - 新增 `ci/sdk_compat_test.py`，基于 `redis-py` 官方高层 API（`bf()` / `cf()` / `cms()` / `topk()` / `tdigest()`、`json()`、`ft()`）验证 RESP2 与 RESP3 返回值解析 100% 兼容，并修复 `FT.CREATE`/`ON HASH` 向量/`FT.AGGREGATE` 协议细节后接入 Docker CI。
4. **✅ 3 节点 Redis 7 Cluster 集成测试（提前完成）**
   - 新增 `ci/cluster_test.py` 并接入 Docker CI，验证多节点路由、`MOVED`、`CROSSSLOT` 以及全部 6 种模块类型（BF/CF/CMS/Top-K/T-Digest/JSON）的在线跨槽位 `MIGRATE`；补齐 `CMS.MERGE`/`TDIGEST.MERGE` 的 `"getkeys-api"` 支持。
5. **✅ `gemini-json` 紧凑数值数组 Typed Numeric Arrays（提前完成）**
   - 落地 [optimization-plan-v2.md](./gemini-json/optimization-plan-v2.md) Phase 9 方案 B：`int64_t[]`/`double[]` 连续存储（8 B/elem）+ 无损提升 + 按需惰性装箱，100k 整数数组 DOM 内存从 `3,367 KB` 降至 **`1,024 KB`（-69.6%）**。

---

## 二、v1.2 — 生态扩展与新模块版图（中期）

1. **新模块：`gemini-timeseries`（`TS.*`，补齐 Redis Stack 四大件）**
   - Clean-room C++20 实现，复用现有 Redis 7 Module API、SIMD、RDB/AOF/PSYNC 与 Fuzz 体系。
   - 核心能力：Gorilla / Delta-of-Delta 压缩块、多标签倒排索引（`TS.MRANGE` / `TS.MGET` / `TS.QUERYINDEX`）、Compaction Rule 自动降采样、`RETENTION` / `DUPLICATE_POLICY`、Redis 7 KeySpecs。
2. **Valkey 8 / Redis 8 兼容性矩阵（按需暂缓）**
   - 按需在离线 Docker CI 中新增 Valkey 8 与 Redis 8 构建环境，验证三模块 ABI 兼容性、Tcl 集成测试与 Soak 回归。

---

## 三、v2.0 — 架构跃迁与深度优化（长期按需）

1. **`gemini-search` 分布式集群查询（Cluster Scatter-Gather）**
   - 在 Redis Cluster 多分片架构下支持跨节点分发与归并排序（`FT.SEARCH` / `FT.HYBRID` / `FT.AGGREGATE`）。
2. **`gemini-search` 千万级向量扩展（按需）**
   - **Product Quantization (PQ，乘积量化)**：面向千万级高维向量（如 768d/1536d）提供 64× 压缩（子空间 k-means 码本 + ADC 查表距离），以及评估 mmap / 磁盘混合存储。
   - 多语言分词增强（统计/HMM 分词、更多语种词干与音似编码）。
3. **`gemini-bloom` 维护策略**
   - 5 个子模块已在 v1.0 完成深度优化、Fuzz 与全量 Soak 闭环，并在 v1.1 补齐 Cluster `CROSSSLOT`/`MIGRATE` 与 `redis-py` 验证，默认进入稳定维护模式；仅在有明确业务需求时按需补充（如 `CF`/`CMS`/`Top-K`/`T-Digest` 与 RedisBloom RDB 互通或 `BF.DEBUG`）。
