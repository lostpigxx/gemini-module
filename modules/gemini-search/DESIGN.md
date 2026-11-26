# gemini-search 设计文档

## 1. 概述

gemini-search 是一个基于 C++20 从零实现的 Redis 全文检索、向量检索与混合检索模块（clean-room 实现），提供 `FT.*` 命令接口（32 个命令，含 `FT.HYBRID`），覆盖全文检索（BM25/WAND Top-K 剪枝/TFIDF/DISMAX/DOCSCORE、中/日/韩 CJK Bigram + 自定义词典 FMM 分词、英/法/德/西词干）、TAG、NUMERIC、VECTOR（FLAT / 线程安全稠密 ID HNSW、NEON/AVX2 SIMD、FP32/FP64/FP16/BF16、SQ8 4× 量化 & BQ 1-bit 32× 量化）、GEO（0.5° 空间网格）、GEOSHAPE、聚合管道、Per-Index 细粒度读写锁 + 全路径异步非阻塞卸载（`FT.SEARCH` / `FT.HYBRID` / `FT.AGGREGATE`）、自动补全、拼写纠错、同义词与词典。

### 1.1 目标平台与定位

- **目标 Redis 版本**：仅面向 **Redis 7**（基线 Redis 7.2.11），不考虑 Redis 5/6 兼容。
- **Clean-room 原则**：严格仅基于公开命令文档（redis.io）和学术算法（BM25、Porter2、Double Metaphone、HNSW arXiv:1603.09320、RRF、Ray Casting）独立设计与实现，禁止阅读或引用 RediSearch 源码（RSALv2/SSPL）。
- **数据格式**：使用私有模块数据类型名 `GmSearch-`（当前 `kSearchEncVer = 15`，向下兼容 `1..15`）和私有 `aux_save2`/`aux_load` 持久化格式，全部写命令调用 `RedisModule_ReplicateVerbatim` 支持 Master-Replica `PSYNC` 全量与在线复制。

---

## 2. 架构设计

### 2.1 模块拆分（Phase 28 重构后）

```
modules/gemini-search/
  src/
    redis_search_module.cc   # 模块入口：RedisModule_OnLoad，注册 GmSearch- 与全部回调
    search_commands.h/.cc    # 公共入口 + RegisterSearchCommands + 1s 周期维护定时器
    search_internal.h        # 命令层内部共享声明（状态访问器、helper、命令函数）
    search_state.cc          # 全局状态（g_indices/g_aliases/g_cursors/词典/配置）与公共 helper
    search_index_cmds.cc     # FT.CREATE / DROPINDEX / INFO / _LIST / ADD / DEL / ALTER / ALIAS* / TAGVALS
    search_query_cmds.cc     # FT.SEARCH / FT.HYBRID / EXPLAIN / EXPLAINCLI / PROFILE / 高亮与摘要
    search_agg_cmds.cc       # FT.AGGREGATE / CURSOR / 聚合管道与游标生命周期
    search_aux_cmds.cc       # FT.SUG* / DICT* / SYN* / SPELLCHECK / CONFIG / _DEBUG
    search_keyspace.cc       # ON HASH / ON JSON 扫描、后台增量建索引 Timer、Keyspace 事件与 FLUSHDB/LOADING
    search_rdb.h/.cc         # RDB 序列化/反序列化（encver 1..15）、aux_save2/aux_load、AOF Rewrite、ModuleType 回调
    # ── 纯算法核心层（不 include redismodule.h，可独立编译运行 GTest）──
    index_spec.h/.cc         # Schema 定义（FieldSpec / IndexSpec）
    document_store.h/.cc     # 文档存储与精确内存统计（MemoryUsage）
    tag_index.h/.cc          # TAG 倒排索引（紧凑排序向量 + 槽位复用）
    numeric_index.h/.cc      # NUMERIC 有序范围索引（紧凑排序向量 + 槽位复用）
    sort_index.h             # 预解析 SORTABLE 列加速 SORTBY
    text_index.h/.cc         # TEXT 倒排索引（位置信息、BM25 + WAND Top-K、前缀/模糊/短语/词干/音似/CJK FMM）
    stemmer.h/.cc            # 多语言词干提取（英/法/德/西）
    phonetic.h/.cc           # Double Metaphone 音似编码
    vector_index.h/.cc       # VECTOR FLAT 暴力 KNN（L2/COSINE/IP、NEON/AVX2 SIMD、FP16/BF16/SQ8/BQ）
    hnsw_index.h/.cc         # VECTOR HNSW（直接 RDB 序列化、BQ 两阶段重排、自适应 Filtered HNSW）
    hybrid_fusion.h/.cc      # FT.HYBRID 多路融合（RRF / LINEAR）
    search_executor.h/.cc    # 异步查询线程池（配合 RedisModule_BlockClient 非阻塞卸载）
    geo_index.h/.cc          # GEO 经纬度索引（0.5° 空间网格 + Haversine 半径检索）
    geoshape_index.h/.cc     # GEOSHAPE WKT 解析与多边形空间关系判断
    query_parser.h/.cc       # 递归下降查询解析器（DIALECT 1/2）、AST 求值与协作式超时
    expr_engine.h/.cc        # 聚合表达式解析与求值引擎（APPLY / FILTER）
    suggest_dict.h/.cc       # Trie 自动补全词典、全局词典与同义词组
```

**跨平台链接隔离**：纯算法核心层严禁直接或间接 include `redismodule.h`，确保 Linux GNU ld 下所有 GTest 二进制可独立链接（详见 `doc/cross_platform_linking.md`）。

### 2.2 内存与生命周期管理

- **索引存储**：`FT.CREATE idx ...` 在 Redis DB 中创建一个类型为 `GmSearch-` 的 key，其 value 为指向索引名的 `std::string*`，完整 `IndexEntry` 存放在全局 `g_indices[idx]`。
- **周期维护定时器**：`RegisterSearchCommands` 启动 1000 ms 周期定时器 `MaintenanceTimerCallback`：
  1. 清理超出 `MAXIDLE` 的 `FT.AGGREGATE` 游标；
  2. 自动删除超出 `TEMPORARY <seconds>` 空闲 TTL 的临时索引（含关联别名与 Redis key）。
- **游标配额**：每个索引最多允许 `kMaxCursorsPerIndex = 128` 个活跃游标，防止客户端泄漏导致内存无限增长。
- **后台增量异步建索引（Phase 64）**：当 DB key 数量 `>= BG_INDEX_MIN_KEYS` 时，`FT.CREATE` 立即返回 `OK`，后台通过 Redis 7 `RedisModule_CreateTimer` + `RedisModule_ScanCursor` 按时间片（`BG_INDEX_BATCH_KEYS` / `BG_INDEX_TICK_MS`）增量扫描并写入索引，在 `FT.INFO` 中暴露 `indexing` 与 `percent_indexed`，并在 `FT.DROPINDEX` / `FLUSHDB` 时安全取消。

---

## 3. 查询与协议语义

- **DIALECT 支持**：默认 `DEFAULT_DIALECT = 2`（可通过 `FT.CONFIG SET DEFAULT_DIALECT` 或单条查询 `DIALECT <n>` 覆盖）。
  - `DIALECT 1`：`-a b` 解析为 `-(a AND b)`（否定作用域延续至后续 AND 子句）。
  - `DIALECT 2`：`-a b` 解析为 `(-a) AND b`（否定仅绑定紧邻的单个原子）。
- **PHONETIC 查询**：`TEXT` 字段配置 `PHONETIC dm:en` 后，写入时建立音似倒排，查询时在非 `VERBATIM` 模式下自动并集 `PhoneticLookup` / `PhoneticSearch`。
- **协作式 `TIMEOUT`**：在 `EvaluateQuery`、`EvaluateQueryScored` 及 `FT.AGGREGATE` 管道阶段边界检查 monotonic deadline，超时立即返回 `ERR query timeout`。
- **高级向量检索（Phase 32）**：支持 `VECTOR` `FLOAT32` / `FLOAT64`、`@vec:[VECTOR_RANGE radius $param]=>{$YIELD_DISTANCE_AS: alias}`、KNN `EF_RUNTIME` 与 `AS <alias>` 自定义距离字段名。
- **高级 Schema 与索引选项（Phase 33）**：支持 `TAG SEPARATOR` / `CASESENSITIVE`、`SKIPINITIALSCAN`、索引期 `FILTER <expr>`、`FT.DROPINDEX DD`、`FT.SEARCH FILTER <field> <min> <max>`。
- **评分与返回增强（Phase 34）**：支持 `SCORER`（`BM25`/`TFIDF`/`TFIDF.DOCNORM`/`DISMAX`/`DOCSCORE`）+ 字段 `WEIGHT` 权重、`EXPLAINSCORE`、`RETURN n field AS alias`、`WITHSORTKEYS`。
- **多语言词干与通配符（Phase 35）**：支持英文/法文/德文/西班牙文词干（`LANGUAGE english|french|german|spanish`），以及后缀（`*suffix`）、中缀（`*infix*`）通配检索与 `WITHSUFFIXTRIE`。
- **RESP2 / RESP3**：全部命令使用 `RedisModule_ReplyWith*` 回复，Redis 7 自动完成 RESP2/RESP3 编码。

---

## 4. 持久化与 ModuleType 回调（Redis 7）

### 4.1 RDB 序列化（`GmSearch-`，`kSearchEncVer = 15`）

- 向下兼容加载 `encver 1..15`。
- `encver 13` 完整持久化：索引名、前缀、数据源类型（`HASH`/`JSON`）、全部字段定义（含 `VECTOR` 参数与 `FLOAT32`/`FLOAT64`/`FLOAT16`/`BFLOAT16`、`QUANTIZATION SQ8/BQ`、`json_path`、`sortable`/`noindex`/`index_missing`/`index_empty`/`weight`、`nostem`/`phonetic`、`tag_separator`/`case_sensitive`、`geoshape_coord`）、索引选项（自定义 stopwords、`nofreqs`/`nooffsets`/`nohl`、`temporary_ttl`、`language`、`language_field`、`custom_dict`、`filter_expr`）、每索引同义词组、全部文档及二进制 `PAYLOAD`。
- `encver 14`（Phase 62）新增 `HnswIndex` 分层邻接表与量化向量直接持久化（活节点紧凑重编号、启动线性加载，图块缺失时安全回退到重建）。
- `encver 15`（Phase 63）新增 BQ 两阶段精排配置（`RERANK FP16|SQ8`、`OVERSAMPLE`）与精排向量持久化。
- 开启 `REDISMODULE_OPTIONS_HANDLE_IO_ERRORS`，反序列化阶段对 IO 错误、枚举值范围、向量维度、HNSW 图边/度/节点上限、NaN/负权重及各类集合数量上限做严格校验，非法流直接返回 `nullptr`。

### 4.2 全局非 Key 状态持久化（`aux_save2` / `aux_load`）

- 在 `REDISMODULE_AUX_AFTER_RDB` 阶段持久化 `g_aliases`、`g_suggest_dicts`、`g_term_dicts` 和 `g_config`（版本号 `kSearchAuxEncVer = 1`）。
- 反序列化先读入临时容器，全部校验通过后原子提交，损坏流返回 `REDISMODULE_ERR` 且不污染已有状态。

### 4.3 AOF Rewrite

- 使用 Redis 7 `RedisModule_EmitAOF(aof, cmd, "v", argv, argc)` 向量接口重写 `FT.CREATE`、`FT.SYNUPDATE` 和 `FT.ADD`，支持任意字段数与包含 NUL 字节的二进制向量值。

### 4.4 ModuleType 回调

| 回调 | 实现 | 作用 |
|---|---|---|
| `rdb_load` / `rdb_save` | `RdbLoadSearch` / `RdbSaveSearch` | 索引 RDB 持久化 |
| `aux_save2` / `aux_load` | `AuxSaveSearch` / `AuxLoadSearch` | 全局别名/词典/补全/配置持久化 |
| `aof_rewrite` | `AofRewriteSearch` | 无字段上限的命令式 AOF 重写 |
| `free` | `FreeSearch` | 释放 `IndexEntry` 与 key value |
| `mem_usage2` | `SearchMemUsage2` | 统计 `DocumentStore` 精确字节数 + 各索引结构（含 HNSW/SQ8/BQ）内存 |
| `digest` | `DigestSearch` | 确定性摘要（参与 `DEBUG DIGEST-VALUE`） |
| `copy2` | `CopySearch2` | 支持 `COPY` 深拷贝为独立索引 |
| `free_effort2` | `FreeEffortSearch2` | 返回 `1 + doc_store.Size()` 供 Redis 判断 lazy free |
| `defrag` | `DefragSearch` | C++ 堆分配安全 no-op（返回 0） |

---

## 5. Keyspace 与服务端事件

- 订阅 `HASH | GENERIC | MODULE | EXPIRED | EVICTED` keyspace 事件：
  - `del` / `unlink` / `expired` / `evicted` / `rename_from`：从匹配前缀的索引中移除文档。
  - `rename_to` / `restore` / `copy_to`：按索引类型重新索引。
  - `hset` / `hdel` / `hincrby*` / `hmset` / `hsetnx`：增量重索引 Hash；若所有被索引字段均被删除，自动清理残留文档。
  - `json.set` / `json.del` / `json.forget` / `json.merge` / `json.clear` / `json.toggle` / `json.arr*` / `json.num*` / `json.strappend`：通过 `JSON.GET` 提取 JSONPath 字段并重索引（配合 `gemini-json` 的 `RedisModule_NotifyKeyspaceEvent`）。
- 订阅服务端事件：
  - `REDISMODULE_EVENT_FLUSHDB`：在 `FLUSHDB_END` 时清空游标、别名、补全词典和全局词典。
  - `REDISMODULE_EVENT_LOADING`：加载 RDB/AOF 期间跳过冗余 keyspace 重索引。

---

## 6. 测试与 CI 体系

- **GTest 单元测试**（17 个测试二进制，覆盖核心算法与并发安全）：覆盖 `TextIndex`（含 CJK FMM 词典分词、BM25 WAND Top-K 精确等价性）、`TagIndex`/`NumericIndex`、`VectorIndex`/`HnswIndex`（含 FP32/SQ8/BQ SIMD 任意维度一致性与 4 线程并发读压测）、`HybridFusion`、`SearchExecutor`、`QueryParser`、`ExprEngine`、`Rdb/Aux/AOF/ModuleType`（含恶意流拒绝）及 ASan+UBSan 内存安全测试。
- **Tcl 集成测试**（`modules/gemini-search/tests/tcl/search_test.tcl`）：覆盖全部 32 个 `FT.*` 命令（含 `FT.HYBRID`、`QUANTIZATION SQ8/BQ`、`RERANK FP16/SQ8`、`OVERSAMPLE`、自适应 Filtered HNSW、后台增量异步建索引、CJK FMM `DICT`、全路径异步非阻塞查询）、BGSAVE/BGREWRITEAOF 重启、Master-Replica `PSYNC` 复制、Keyspace 事件、`ON JSON` 与 `gemini-json` 联调、RESP3 连接。
- **SDK 兼容测试**（`ci/sdk_compat_test.py`）：使用 `redis-py` 高层 `r.ft()` API 在 RESP2 与 RESP3 下验证 `ON HASH` / `ON JSON`、`FT.CREATE`/`FT.SEARCH`/`FT.AGGREGATE`/KNN/`FT.INFO`/Suggest/Spellcheck/Alias/TagVals 全链路兼容。
- **Python 长稳测试**（`ci/search_soak_test.py`）：持续混合执行 `HSET`/`UNLINK`/HNSW 增删、`FT.SEARCH` 正确性校验、`FT.AGGREGATE WITHCURSOR`、`FT.PROFILE` 和 `MEMORY USAGE` 内存增长检查（支持 RESP2 + RESP3，60s 实测 ~500k ops，内存增量 `+0.20 MB`）。
- **Docker CI**：在 `Dockerfile.redis7`（Redis 7.2.11）中通过 `ci/run_tests.sh` 并行运行三模块全部 21 个测试任务（GTest Normal/ASan、Tcl、Compat、Migrate、SDK Compat、3 节点 Cluster、Soak）。

---

## 7. 已知限制

1. **单机/主从架构**：支持单节点与 Master-Replica `PSYNC` 全量/增量复制，暂未实现 Redis Cluster 跨分片 Scatter-Gather 协调查询（规划在 v2.0）。
2. **无 RediSearch RDB 互通**：仅提供命令级与客户端 SDK 兼容（已通过 `redis-py` RESP2/RESP3 端到端验证），RDB 格式为 `GmSearch-` 私有格式。
3. **音似编码仅支持英文**：词干已内置英文/法文/德文/西班牙文，`phonetic.cc`（Double Metaphone）目前仅面向英文。

---

## 8. 核心演进总结与性能基线（截至 Phase 65）

### 8.1 近期里程碑（v5–v1.1，Phases 40–65）

| 阶段 | 核心能力与优化 |
|---|---|
| **v5 (P40–P41)** | 递归深度/向量维度/NaN 边界加固、`LIMIT 0 0` 计数快路径、`FT.ADD PAYLOAD`/`WITHPAYLOADS`、`INDEXMISSING`/`INDEXEMPTY`、`RedisModule_ReplicateVerbatim` + Master-Replica `PSYNC` 验证 |
| **v6 (P42–P46)** | `FT.HYBRID`（BM25 + Vector RRF/LINEAR 融合）、稠密 ID `HnswIndex`、`FLOAT16`/`BFLOAT16`/`QUANTIZATION SQ8`、多键 `SORTBY MAX` + 14 个聚合函数 + 聚合 KNN、UTF-8 CJK Bigram 分词与高亮、`FT.INFO` 内存观测 + `ON_TIMEOUT`/`MAXSEARCHRESULTS` 治理 |
| **v7 (P47–P51)** | `TextIndex` 稠密 `uint32_t` ID + 前向词表（$O(\text{doc\_terms})$ 删除）、`TagIndex`/`NumericIndex` 紧凑排序向量 + 槽位复用、FP32 ARM NEON & x86-64 AVX2 SIMD 内核、`SearchExecutor` 线程池 + `RedisModule_BlockClient` 异步 KNN 卸载 |
| **v8 (P52–P54)** | `SQ8` ARM NEON & x86-64 AVX2 SIMD 内核、`FT.HYBRID` 与非游标 `FT.AGGREGATE` 异步非阻塞卸载、CJK 自定义词典前向最大匹配（FMM）分词（`FT.DICTADD` + `FT.CREATE ... DICT`） |
| **v9 (P55–P58)** | 1-bit 二值量化 `QUANTIZATION BQ`（32× 内存压缩 + NEON `vcntq_u8`/`popcnt` 汉明距离）、BM25 Top-K WAND 剪枝引擎（`TextIndex::SearchTopK`）、`HnswIndex` `thread_local` 并发读安全、Per-Index 细粒度 `shared_mutex` 读写锁、非 KNN `FT.SEARCH` 全路径异步卸载 |
| **v1.0 (P59–P61)** | RDB `encver 13`（BQ/`DICT`/`LANGUAGE_FIELD` 闭环）、`mem_usage2`、RDB 逐字节模糊测试、HNSW 零分配 `GreedySearchLayer` + BQ 预量化 + WAND 接入 `FT.SEARCH`、`ON JSON` 多值数组通配索引 |
| **v1.1 (P62–P65)** | **P62**：HNSW 图结构直接 RDB 序列化（`encver 14`，2k×768d 加载提速 ~550×）；**P63**：BQ 两阶段检索（`RERANK FP16/SQ8` + `OVERSAMPLE`，`encver 15`）；**P64**：`FT.CREATE` 基于 Redis 7 Timer + ScanCursor 的后台增量异步建索引及 `redis-py` SDK 兼容闭环；**P65**：按过滤选择度自适应切换的 Filtered HNSW（零分配 epoch-bitset + 连通性保持遍历） |

### 8.2 `search_bench` 实测基线（Apple Silicon ARM64 NEON）

| 基准项 | 延迟 / 耗时 | 备注 |
|---|---|---|
| `FlatVector FP32 Cosine KNN-10 (5k × 768d)` | `550.96 us/op` | NEON SIMD FP32 基线 |
| `FlatVector SQ8 Cosine KNN-10 (5k × 768d)` | `615.97 us/op` | 4× 内存压缩，NEON 动态反量化 |
| `FlatVector BQ Hamming KNN-10 (5k × 768d)` | **`19.57 us/op`** | **32× 内存压缩，较 FP32 提速 ~28×** |
| `FlatVector L2 KNN-10 (5k × 128d)` | `40.15 us/op` | NEON SIMD FP32 |
| `HnswIndex L2 KNN-10 (2k × 128d)` | **`10.79 us/op`** | 线程安全零分配遍历 |
| `HnswIndex Filtered 50% (2k × 128d)` | **`24.90 us/op`** | Phase 65 自适应 Filtered HNSW |
| `HnswIndex Deserialize (2k × 128d / 2k × 768d BQ)` | **`198 us / 227 us`** | Phase 62 直接图反序列化（~550× 提速） |
| `HnswIndex BQ / BQ+FP16 OS4 (2k × 768d)` | **`10.71 us / 58.73 us`** | Phase 63 两阶段粗排 + FP16 精排 |
| `TextIndex::Search Full BM25 (20k docs)` | `589.39 us/op` | 4 词全量 BM25 |
| `TextIndex::SearchTopK WAND-10 (20k docs)` | **`76.33 us/op`** | **与全量等价，提速 7.7×** |
| `TextIndex::Update/Remove (30k vocab)` | `3.29 us/op` | 稠密 ID + 前向词表 |
| `SortableColumn SORTBY (10k)` vs `DocStore strtod` | `335.44 us/op` vs `7595.65 us/op` | 预解析列提速 ~22× |

---

## 9. 后续演进方向（v2.0 长期按需）

详见全仓规划 [doc/roadmap.md](../../doc/roadmap.md)：

1. **分布式集群查询（Cluster Scatter-Gather）**：在 Redis Cluster 多分片架构下支持跨节点分发与归并排序（`FT.SEARCH` / `FT.HYBRID` / `FT.AGGREGATE`）。
2. **Product Quantization (PQ，乘积量化)**：面向千万级高维向量（768d/1536d）提供 64× 压缩（子空间 k-means 码本 + ADC 查表距离），并按需评估 mmap / 磁盘混合存储。
3. **多语言分词与音似增强**：统计/HMM 中文分词、更多语种词干与非英文音似编码。
