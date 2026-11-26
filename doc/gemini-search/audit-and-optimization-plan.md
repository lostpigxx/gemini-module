# gemini-search 深度审计报告与优化改造规划（Phase 59–61）

> 目标平台：**Redis 7**（基线 Redis 7.2.11，Clean-room C++20）  
> 审计范围：`modules/gemini-search/src/*`（14,045 行）、`modules/gemini-search/tests/*`、`doc/gemini-search/development-plan-v10.md`

---

## 一、核心审计发现

| 严重级别 | 模块与位置 | 问题描述 | 影响 |
|---|---|---|---|
| **高危（数据丢失）** | `search_rdb.cc:250-257` | `RdbLoadSearch` 校验 `raw_q > static_cast<uint64_t>(VectorQuantization::kSq8)`，但 Phase 55 新增的 `VectorQuantization::kBq` 枚举值为 `2` | **所有 `QUANTIZATION BQ` 索引在 RDB 重启 / PSYNC / DUMP-RESTORE 时加载失败并返回 `nullptr`，导致数据丢失** |
| **高（持久化缺口）** | `search_rdb.cc:156, 426-454` | `IndexSpec::custom_dict`（`DICT <name>`）未持久化到 RDB，也未在 `AofRewriteSearch` 中写出且未在 `FT.INFO` 暴露；`AofRewriteSearch` 亦遗漏 `LANGUAGE_FIELD` | 重启或 AOF 重写后自定义词典分词与文档级语言字段失效 |
| **高（性能瓶颈）** | `hnsw_index.cc:40-51, 199-202, 299-313` | 1. BQ 模式下每次 `DistanceTo()` 都对同一查询向量重复执行 `QuantizeBq()`（单次查询重复量化 ~200 次）；<br>2. 上层贪心下降调用 `SearchLayer(ef=1)` 每层堆分配 2 个 `priority_queue` + `vector`；<br>3. `KnnQueryEf` 丢弃 `SearchLayer` 已算距离，对全部 `ef` 候选重复调用 `DistanceTo()` | HNSW 构建与查询（尤其 BQ）存在大量冗余计算与堆分配 |
| **高（性能未接通）** | `search_query_cmds.cc` | `TextIndex::SearchTopK`（WAND 剪枝，基准比全量 BM25 快 **7.8×**）已实现但未接入 `FT.SEARCH` 主查询路径 | `FT.SEARCH ... LIMIT offset count` 仍走全量 BM25 评分 |
| **中（功能缺陷）** | `search_keyspace.cc:80-155` | `IndexJsonKey` 仅对 `TAG` 解析 JSON 数组且写死 `,` 分隔符（忽略 `fspec.tag_separator`）；对 `TEXT` / `NUMERIC` 的多值 JSONPath 数组（如 `$.items[*].title`）未正确展开 | `ON JSON` 数组字段索引不完整，自定义分隔符失效 |
| **中（Redis 7 适配）** | `redis_search_module.cc:24`, `search_rdb.cc:575-579` | 仍使用旧版 `mem_usage` 而非 Redis 7 `mem_usage2`；`SearchMemUsage` 对 HNSW/SQ8/BQ 内存估算沿用旧 `sizeof(std::string)` 邻居模型 | `MEMORY USAGE` 估算失真 |

---

## 二、分 Phase 改造计划

| Phase | 核心主题 | 关键交付与验证 |
|---|---|---|
| **Phase 59** | **RDB `encver 13` 闭环、BQ 致命 Bug 修复与模糊测试** | 修复 `RdbLoadSearch` 接受 `VectorQuantization::kBq`；升级 `kSearchEncVer = 13` 持久化 `custom_dict`（兼容 `1..12`）；`AofRewriteSearch` 补齐 `DICT` / `LANGUAGE_FIELD`；`FT.INFO` 暴露 `custom_dict`；升级 Redis 7 `mem_usage2` 并修正 HNSW/SQ8/BQ 内存估算；新增 BQ/DICT RDB 往返与截断/篡改模糊测试 |
| **Phase 60** | **HNSW 查询/构建加速与 WAND 查询路径接入** | `HnswIndex` 零分配上层贪心下沉 `GreedySearchLayer`、BQ 查询单次预量化、免重复算距；`FT.SEARCH` 默认 BM25 分页查询接入 `TextIndex::SearchTopK`（WAND）；`search_bench` 对比实测 + 差分 GTest + ASan/UBSan |
| **Phase 61** | **`ON JSON` 多值数组 JSONPath 索引增强** | `IndexJsonKey` 完整支持 JSON 数组展开至 `TAG`（遵守 `tag_separator` + 字符串反转义）、`TEXT`（多值拼接索引）、`NUMERIC`（支持数组元素解析）；补齐 GTest 与 Tcl 集成用例 |

## Phase 60 Benchmark Results (`search_bench`, macOS Apple Silicon)

| Benchmark | Baseline | After Phase 60 | Note |
|---|---|---|---|
| `HnswIndex Build (2kx128)` | `91.06 ms` | `85.41 ms` | `-6.2%` (`GreedySearchLayer` + reuse `SearchLayer` distances) |
| `HnswIndex BQ Build (2kx768)` | — | `91.25 ms` | Direct BQ word pruning without dequantize/requantize |
| `HnswIndex BQ KNN-10 (2kx768)` | — | `7.47 us/op` | Query pre-quantized once per search (~73x faster than Flat FP32 `547 us/op`) |
| `TextIndex::SearchTopK WAND-10 (20k)` | `71.17 us/op` | `71.84 us/op` | Wired into `FT.SEARCH` BM25 `LIMIT` fast-path (7.8x vs full BM25 `564 us/op`) |
