# gemini-module Post-v1.0.0 演进规划（Roadmap）

> 基线版本：**`v1.0.0`**（2026-11-26，Redis 7.2.11，C++20 Clean-room）  
> 现有模块：`gemini-bloom`（BF/CF/CMS/Top-K/T-Digest）、`gemini-json`、`gemini-search`

---

## 一、v1.1 — 生产可用性与工程化闭环（近期优先）

> 详细实施方案见 [v1.1-development-plan.md](./v1.1-development-plan.md)。

1. **工程化与文档收口**
   - 清理 `.gitignore`（忽略 `build_*` 构建目录），统一三模块 Benchmark 回归脚本（`bloom_bench` / `json_bench` / `search_bench`）。
   - 补齐 `modules/gemini-json/DESIGN.md` 设计文档。
2. **`gemini-search` v1.1 核心能力升级（Phases 62–64）**
   - **P62 — HNSW 图结构直接 RDB 序列化（`encver 14`）**：持久化 `HnswIndex` 分层邻接表与量化向量数据，重启恢复从 $O(N \log N)$ 重建降至线性反序列化。
   - **P63 — BQ 两阶段检索（Oversample & Rerank）**：在保留 BQ 32× 压缩与超低延迟粗排的同时，支持可选 FP16/SQ8 精排，将高维向量（768d/1536d）召回率提升至 95%+。
   - **P64 — 后台增量异步建索引（Background Indexing）**：将 `FT.CREATE` 存量扫描改造为基于 Redis 7 Timer + ScanCursor 的时间片异步推进，避免大库建索引阻塞主线程，并在 `FT.INFO` 中暴露 `indexing` 与 `percent_indexed`。
3. **主流客户端 SDK 端到端兼容性测试**
   - 新增 `ci/sdk_compat_test.py`，基于 `redis-py` 官方高层 API（`bf()` / `cf()` / `cms()` / `topk()` / `tdigest()`、`json()`、`ft()`）验证 RESP2 与 RESP3 返回值解析 100% 兼容，并接入 Docker CI。

---

## 二、v1.2 — 生态扩展与新模块版图（中期）

1. **新模块：`gemini-timeseries`（`TS.*`，补齐 Redis Stack 四大件）**
   - Clean-room C++20 实现，复用现有 Redis 7 Module API、SIMD、RDB/AOF/PSYNC 与 Fuzz 体系。
   - 核心能力：Gorilla / Delta-of-Delta 压缩块、多标签倒排索引（`TS.MRANGE` / `TS.MGET` / `TS.QUERYINDEX`）、Compaction Rule 自动降采样、`RETENTION` / `DUPLICATE_POLICY`、Redis 7 KeySpecs。
2. **Valkey 8 / Redis 8 兼容性矩阵**
   - 在离线 Docker CI 中新增 Valkey 8 与 Redis 8 构建环境，验证三模块 ABI 兼容性、Tcl 集成测试与 Soak 回归。
3. **Redis Cluster 集成测试**
   - 搭建多节点 Redis Cluster 测试脚本，验证 `gemini-bloom` / `gemini-json` 的动态 KeySpecs 路由、`CROSSSLOT` 错误语义、以及 `MIGRATE` 跨槽位迁移（RDB 传输）。

---

## 三、v2.0 — 架构跃迁与深度优化（长期按需）

1. **`gemini-search` 分布式集群查询（Cluster Scatter-Gather）**
   - 在 Redis Cluster 多分片架构下支持跨节点分发与归并排序（`FT.SEARCH` / `FT.HYBRID` / `FT.AGGREGATE`）。
2. **`gemini-search` 过滤 + 向量联合检索与大规模向量扩展**
   - 针对 RAG 常见模式 `(@tag:{...} @num:[...])=>[KNN 10 @vec ...]`，按过滤选择度自适应选择 Filtered HNSW 遍历或 Flat 扫描。
   - 评估 Product Quantization (PQ) 或 mmap / 磁盘混合存储以支撑千万级向量。
   - 多语言分词增强（统计/HMM 分词、更多语种词干与音似编码）。
3. **`gemini-json` 容器紧凑值存储**
   - 落地 [optimization-plan-v2.md](./gemini-json/optimization-plan-v2.md) Phase 9 备选方案 B（容器按值存储 / Typed Array），进一步压缩超大 JSON 数组内存占用。
4. **`gemini-bloom` 维护策略**
   - 5 个子模块已在 v1.0 完成深度优化、Fuzz 与全量 Soak 闭环，默认进入稳定维护模式；仅在有明确业务需求时按需补充（如 `CF`/`CMS`/`Top-K`/`T-Digest` 与 RedisBloom RDB 互通或 `BF.DEBUG`）。
