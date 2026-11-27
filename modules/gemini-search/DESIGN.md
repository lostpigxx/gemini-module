# gemini-search 设计文档

## 1. 概述

gemini-search 是一个基于 C++20 从零实现的 Redis 全文检索、向量检索与混合检索模块（clean-room 实现），提供 `FT.*` 命令接口（32 个命令，含 `FT.HYBRID`），覆盖全文检索（BM25/WAND Top-K 剪枝/TFIDF/DISMAX/DOCSCORE、中/日/韩 CJK Bigram + 自定义词典 FMM 分词、英/法/德/西词干）、TAG、NUMERIC、VECTOR（FLAT / 线程安全稠密 ID HNSW、NEON/AVX2 SIMD、FP32/FP64/FP16/BF16、SQ8 4× 量化 & BQ 1-bit 32× 量化 + 两阶段精排 + 自适应 Filtered HNSW）、GEO（0.5° 空间网格）、GEOSHAPE、聚合管道、Per-Index 细粒度读写锁 + 全路径异步非阻塞卸载（`FT.SEARCH` / `FT.HYBRID` / `FT.AGGREGATE`）、自动补全、拼写纠错、同义词、词典，以及 **Redis Cluster 分布式 Scatter-Gather 查询引擎（Phases 66–72）**。

### 1.1 目标平台与定位

- **目标 Redis 版本**：仅面向 **Redis 7**（基线 Redis 7.2.11），不考虑 Redis 5/6 兼容。
- **Clean-room 原则**：严格仅基于公开命令文档（redis.io）和学术算法（BM25、Porter2、Double Metaphone、HNSW arXiv:1603.09320、RRF、Ray Casting、Chan 并行方差、HyperLogLog、T-Digest）独立设计与实现，禁止阅读或引用 RediSearch 源码（RSALv2/SSPL）。
- **数据格式**：单机模式使用私有模块数据类型名 `GmSearch-`（当前 `kSearchEncVer = 15`，向下兼容 `1..15`）和 `aux_save2`/`aux_load`（`kSearchAuxEncVer = 2`，向下兼容 `1..2`）；Cluster 模式下将索引 Schema、同义词、Schema Epoch 与 HNSW 图块统一持久化在 `aux_save2` v2 中，避免在集群 DB 中产生孤立裸 key。全部写命令调用 `RedisModule_ReplicateVerbatim` / `RedisModule_Replicate` 支持 Master-Replica `PSYNC` 全量与在线复制。

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
    search_rdb.h/.cc         # RDB 序列化/反序列化（encver 1..15）、aux_save2/aux_load（v1/v2）、AOF Rewrite、ModuleType 回调
    cluster_topology.h/.cc   # Redis Cluster 检测、Cluster Bus Hello 自动发现、slot_to_master/owns_slots、IP 白名单与 FT._DEBUG CLUSTER
    cluster_coordinator.h/.cc # 分布式 Coordinator 状态机（DDL 两阶段广播/回滚、SchemaSync、FT.SEARCH/HYBRID/AGGREGATE/INFO/TAGVALS/SPELLCHECK/SUG/PROFILE/CURSOR）
    # ── 纯算法核心层（不 include redismodule.h，可独立编译运行 GTest）──
    index_spec.h/.cc         # Schema 定义（FieldSpec / IndexSpec）
    document_store.h/.cc     # 文档存储与精确内存统计（MemoryUsage）
    tag_index.h/.cc          # TAG 倒排索引（紧凑排序向量 + 槽位复用）
    numeric_index.h/.cc      # NUMERIC 有序范围索引（紧凑排序向量 + 槽位复用）
    sort_index.h             # 预解析 SORTABLE 列加速 SORTBY
    text_index.h/.cc         # TEXT 倒排索引（位置信息、BM25 + WAND Top-K、外部全局 DFS IDF 注入、前缀/模糊/短语/词干/音似/CJK FMM）
    stemmer.h/.cc            # 多语言词干提取（英/法/德/西）
    phonetic.h/.cc           # Double Metaphone 音似编码
    vector_index.h/.cc       # VECTOR FLAT 暴力 KNN（L2/COSINE/IP、NEON/AVX2 SIMD、FP16/BF16/SQ8/BQ）
    hnsw_index.h/.cc         # VECTOR HNSW（直接 RDB 序列化、BQ 两阶段重排、自适应 Filtered HNSW）
    hybrid_fusion.h/.cc      # FT.HYBRID 多路融合（RRF / LINEAR）
    search_executor.h/.cc    # 异步查询线程池（配合 RedisModule_BlockClient 非阻塞卸载及远端分片只读执行）
    geo_index.h/.cc          # GEO 经纬度索引（0.5° 空间网格 + Haversine 半径检索）
    geoshape_index.h/.cc     # GEOSHAPE WKT 解析与多边形空间关系判断
    query_parser.h/.cc       # 递归下降查询解析器（DIALECT 1/2）、AST 求值与协作式超时
    expr_engine.h/.cc        # 聚合表达式解析与求值引擎（APPLY / FILTER）
    suggest_dict.h/.cc       # Trie 自动补全词典、全局词典与同义词组
    cluster_codec.h/.cc      # 二进制 RPC 帧编解码（GMSC 魔数、双 CRC32、32 MiB 上限）与全 RPC 负载序列化
    cluster_rpc.h/.cc        # 非阻塞多路复用 TCP RPC Server/Client、64-bit keyed MAC 鉴权与并发配额限流
    cluster_merger.h/.cc     # M-way 分布式归并堆、doc_id 确定性去重、FT.HYBRID 全局融合与 BM25 DFS 统计聚合
    cluster_agg_reduce.h/.cc # FT.AGGREGATE 两阶段 Map-Reduce 全部 12 种 REDUCE 可序列化中间态与并行合并器
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

- 在 `REDISMODULE_AUX_AFTER_RDB` 阶段持久化 `g_aliases`、`g_suggest_dicts`、`g_term_dicts`、`g_config`（版本号 `kSearchAuxEncVer = 2`，完全向下兼容 `encver 1`）。
- 在 Cluster 模式下（`encver 2`），额外持久化集群 Schema Epoch、全部索引的 `IndexSpec`/同义词组及本地 `HnswIndex` 预构建图块；节点重启后在 `LOADING_ENDED` 阶段自动恢复全部本地分片索引，不在 Redis DB 中写入裸 `GmSearch-` key。
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
  - `rename_to` / `restore` / `copy_to`：按索引类型重新索引（天然覆盖 Redis Cluster 在线 `MIGRATE` 目标端 `restore` 与源端 `del`）。
  - `hset` / `hdel` / `hincrby*` / `hmset` / `hsetnx`：增量重索引 Hash；若所有被索引字段均被删除，自动清理残留文档。
  - `json.set` / `json.del` / `json.forget` / `json.merge` / `json.clear` / `json.toggle` / `json.arr*` / `json.num*` / `json.strappend`：通过 `JSON.GET` 提取 JSONPath 字段并重索引（配合 `gemini-json` 的 `RedisModule_NotifyKeyspaceEvent`）。
- 订阅服务端事件：
  - `REDISMODULE_EVENT_FLUSHDB`：在 `FLUSHDB_END` 时清空游标、别名、补全词典和全局词典。
  - `REDISMODULE_EVENT_LOADING`：加载 RDB/AOF 期间跳过冗余 keyspace 重索引；Cluster 模式下在 `LOADING_ENDED` 自动重建/挂接本地分片索引。

---

## 6. Redis Cluster 分布式 Scatter-Gather 架构（Phases 66–72）

完整架构设计参见 [cluster-scatter-gather-design.md](../../doc/gemini-search/cluster-scatter-gather-design.md)。单机模式下零开销（不启动 RPC 线程、不监听端口、不注册 Cluster Bus 回调）；检测到 `REDISMODULE_CTX_FLAGS_CLUSTER` 后自动启用：

1. **控制面拓扑发现与数据面二进制 RPC（Phase 66）**：
   - 通过 Redis Cluster Bus 广播轻量 `ClusterBusHello`（含 `rpc_port`、`schema_epoch`、`protocol_ver`），结合 `CLUSTER NODES` / `CLUSTER SLOTS` 构建线程安全不可变快照 `ClusterTopology`（维护 `slot_to_master` 与每节点 `owns_slots`）。
   - 节点间数据面采用独立 I/O 线程的非阻塞多路复用 TCP RPC（`GMSC` 魔数、30 字节定长头 + 头/负载双 CRC32、32 MiB 单帧上限、复用长连接池），并通过 `FT._DEBUG CLUSTER [PING]` 提供全链路观测。
2. **分布式 DDL、持久化与自愈（Phase 67）**：
   - `FT.CREATE` / `FT.DROPINDEX` / `FT.ALTER` / `FT.ALIAS*` / `FT.SYNUPDATE` / `FT.DICT*` 采用两阶段广播 + 冲突自动补偿回滚，并携带单调递增 `schema_epoch`。
   - 新加入或落后节点通过 `kRpcSchemaSyncReq/Resp` 自动拉取全量 Schema 并触发后台增量索引；多 Master 集群下明确拒绝无前缀绑定单槽位的旧版 `FT.ADD`/`FT.DEL`。
3. **Query-Then-Fetch 与多路融合（Phases 68–69）**：
   - **远端分片零 Redis 主线程阻塞**：RPC 收到查询后直接提交至 `SearchExecutor` 线程池，仅短持 `GetIndicesMutex()` 拷贝 `std::shared_ptr<IndexEntry>`，再持 per-index `shared_lock` 执行局部检索。
   - **`FT.SEARCH`**：Phase 1 各分片仅回传 Top-`(offset + count)` 的轻量 `(doc_id, score/sort_key)`，Coordinator 通过 $M$-way 堆归并并按 `doc_id` 确定性去重后，Phase 2 仅向胜出分片拉取最终 `count` 条文档正文与高亮/摘要（2.5 KB 文档在 `LIMIT 90 10` 下节省 **95.1%** 跨节点带宽）。
   - **`FT.HYBRID` & BM25 DFS**：各分片分别回传文本 Top-$L_1$ 与向量 Top-$L_2$ 原始分，Coordinator 全局归并后执行全局 RRF / LINEAR 融合；开启 `CLUSTER_BM25_DFS` 时额外增加 Phase 0 采集全局 `(N, avgdl, df(t))` 实现严重倾斜场景下的单机精确 BM25 等价排序。
   - **辅助命令集群化**：`FT.INFO`（聚合全集群文档/词项/内存/加权后台建索引进度与 `cluster_shards`）、`FT.TAGVALS`（全局并集去重）、`FT.SPELLCHECK`（全局词表过滤与建议分聚合）、`FT.SUG*`（按 CRC16 hash slot 单点路由）及 `FT.PROFILE SEARCH`（分片耗时剖析）。
4. **两阶段 Map-Reduce 聚合与跨节点游标（Phase 70）**：
   - `ClusterAggPlanner` 自动将前置 `LOAD`/`APPLY`/`FILTER` 与首个 `GROUPBY` 下推至各分片并行执行 Map，完整支持全部 12 种 `REDUCE` 的可序列化并行合并（含 Chan 并行 `STDDEV`、14-bit 稀疏/稠密 HyperLogLog `COUNT_DISTINCTISH`、精确+T-Digest `QUANTILE`、加权蓄水池 `RANDOM_SAMPLE`），受 `CLUSTER_AGG_MAX_GROUPS` 内存配额保护。
   - `FT.CURSOR` 采用 24-bit 节点标签编码游标 ID，客户端可向任意集群节点发起 `FT.CURSOR READ`/`DEL`，自动透明转发至宿主 Coordinator。
5. **弹性容错、Failover 与安全加固（Phase 71）**：
   - **在线 `MIGRATE` 一致性**：`FT.SEARCH` / `FT.HYBRID` / `FT.AGGREGATE` 在归并层对迁移双写窗口内的重复 `doc_id` 确定性去重并修正 `global_total`。
   - **主从 Failover & `ON_TIMEOUT`**：按 `owns_slots` 自动过滤掉已失去槽位的故障旧 Master；任一持槽分片异常时，`ON_TIMEOUT FAIL` 立即报错，`ON_TIMEOUT RETURN` 跳过异常分片返回部分结果并递增 `cluster_partial_results_total`；检测到 `FAIL`/`PFAIL` 立即主动断开旧连接快速熔断。
   - **RPC 安全边界**：支持可选 `CLUSTER_AUTH_TOKEN` 64-bit keyed MAC 逐帧鉴权（常量时间校验）、动态集群节点 IP 白名单（`SetPeerValidator`）及服务端全局（默认 4096）/ 客户端单节点（默认 1024）并发配额限流。

---

## 7. 测试与 CI 体系

- **GTest 单元测试**（21 个测试二进制，覆盖核心算法、集群核心与并发安全）：覆盖 `TextIndex`、`TagIndex`/`NumericIndex`、`VectorIndex`/`HnswIndex`、`HybridFusion`、`SearchExecutor`、`QueryParser`、`ExprEngine`、`Rdb/Aux/AOF/ModuleType`、`ClusterCodec`（含字节级截断/CRC Fuzz）、`ClusterRpc`（并发多路复用、超时丢弃、重启重连、MAC 鉴权、IP 白名单、并发配额）、`ClusterMerger`（10k 文档多模式对拍、BM25 DFS 90% 倾斜、MIGRATE 双写去重）、`ClusterAggReduce`（全部 12 种 Reducer 1/3/5/8 分片对拍）及 ASan+UBSan 内存安全测试。
- **Tcl 集成测试**（`modules/gemini-search/tests/tcl/search_test.tcl`）：覆盖全部 32 个 `FT.*` 命令、BGSAVE/BGREWRITEAOF 重启、Master-Replica `PSYNC` 复制、Keyspace 事件、`ON JSON` 与 `gemini-json` 联调、RESP3 连接。
- **SDK 兼容测试**（`ci/sdk_compat_test.py`）：使用 `redis-py` 高层 `r.ft()` API 在 RESP2 与 RESP3 下验证端到端兼容。
- **Redis 7 Cluster 集成测试**（`ci/cluster_test.py`，9 阶段）：覆盖多节点路由/`MOVED`/`CROSSSLOT`/6 类型 `MIGRATE`、Phase 66 拓扑/RPC/配置、Phase 67 DDL 广播/回滚/持久化重启/第 4 节点自愈、Phase 68 3,000 文档 33 查询单机黄金对拍、Phase 69 `FT.HYBRID`/90% 倾斜 BM25 DFS/辅助命令、Phase 70 `FT.AGGREGATE` 全 12 Reducer/跨节点游标/`CLUSTER_AGG_MAX_GROUPS`、Phase 71 并发 500-slot `MIGRATE` 与 6 节点（3M+3R）`SIGKILL` Failover + `CLUSTER_AUTH_TOKEN`。
- **Python 长稳测试**：
  - 单机 `ci/search_soak_test.py`（RESP2 + RESP3，60s ~500k ops，内存增量 `+0.20 MB`）；
  - 集群 `ci/search_cluster_soak_test.py`（3 节点混合 `HSET`/`JSON.SET`/`DEL`/`FT.SEARCH`/`FT.HYBRID`/`FT.AGGREGATE`/跨节点 `FT.CURSOR`/在线 `MIGRATE`，20s 实测 ~60k ops，零重复、零拒帧、内存增量 `+0.12 MB`）。
- **Docker CI**：在 `Dockerfile.redis7`（Redis 7.2.11）中通过 `ci/run_tests.sh` 并行运行三模块全部 22 个测试任务（60s 内完成）。

---

## 8. 已知限制

1. **无 RediSearch RDB 互通**：仅提供命令级与客户端 SDK 兼容（已通过 `redis-py` RESP2/RESP3 端到端验证），RDB 格式为 `GmSearch-` 私有格式。
2. **Cluster 模式下的旧版写入命令**：多 Master 集群模式下不允许使用无 key 槽位归属的旧版 `FT.ADD`/`FT.DEL`，需使用标准 `HSET`/`JSON.SET` 写入配合自动索引。
3. **音似编码仅支持英文**：词干已内置英文/法文/德文/西班牙文，`phonetic.cc`（Double Metaphone）目前仅面向英文。

---

## 9. 核心演进总结与性能基线（截至 Phase 72）

### 9.1 近期里程碑（v5–v2.0，Phases 40–72）

| 阶段 | 核心能力与优化 |
|---|---|
| **v5 (P40–P41)** | 递归深度/向量维度/NaN 边界加固、`LIMIT 0 0` 计数快路径、`FT.ADD PAYLOAD`/`WITHPAYLOADS`、`INDEXMISSING`/`INDEXEMPTY`、`RedisModule_ReplicateVerbatim` + Master-Replica `PSYNC` 验证 |
| **v6 (P42–P46)** | `FT.HYBRID`（BM25 + Vector RRF/LINEAR 融合）、稠密 ID `HnswIndex`、`FLOAT16`/`BFLOAT16`/`QUANTIZATION SQ8`、多键 `SORTBY MAX` + 14 个聚合函数 + 聚合 KNN、UTF-8 CJK Bigram 分词与高亮、`FT.INFO` 内存观测 + `ON_TIMEOUT`/`MAXSEARCHRESULTS` 治理 |
| **v7 (P47–P51)** | `TextIndex` 稠密 `uint32_t` ID + 前向词表（$O(\text{doc\_terms})$ 删除）、`TagIndex`/`NumericIndex` 紧凑排序向量 + 槽位复用、FP32 ARM NEON & x86-64 AVX2 SIMD 内核、`SearchExecutor` 线程池 + `RedisModule_BlockClient` 异步 KNN 卸载 |
| **v8 (P52–P54)** | `SQ8` ARM NEON & x86-64 AVX2 SIMD 内核、`FT.HYBRID` 与非游标 `FT.AGGREGATE` 异步非阻塞卸载、CJK 自定义词典前向最大匹配（FMM）分词（`FT.DICTADD` + `FT.CREATE ... DICT`） |
| **v9 (P55–P58)** | 1-bit 二值量化 `QUANTIZATION BQ`（32× 内存压缩 + NEON `vcntq_u8`/`popcnt` 汉明距离）、BM25 Top-K WAND 剪枝引擎（`TextIndex::SearchTopK`）、`HnswIndex` `thread_local` 并发读安全、Per-Index 细粒度 `shared_mutex` 读写锁、非 KNN `FT.SEARCH` 全路径异步卸载 |
| **v1.0 (P59–P61)** | RDB `encver 13`（BQ/`DICT`/`LANGUAGE_FIELD` 闭环）、`mem_usage2`、RDB 逐字节模糊测试、HNSW 零分配 `GreedySearchLayer` + BQ 预量化 + WAND 接入 `FT.SEARCH`、`ON JSON` 多值数组通配索引 |
| **v1.1 (P62–P65)** | **P62**：HNSW 图结构直接 RDB 序列化（`encver 14`，2k×768d 加载提速 ~550×）；**P63**：BQ 两阶段检索（`RERANK FP16/SQ8` + `OVERSAMPLE`，`encver 15`）；**P64**：`FT.CREATE` 后台增量异步建索引及 `redis-py` SDK 兼容闭环；**P65**：按过滤选择度自适应切换的 Filtered HNSW |
| **v2.0 (P66–P72)** | **Redis Cluster 分布式 Scatter-Gather 全栈闭环**：**P66** 拓扑自动发现与二进制 RPC；**P67** 两阶段 DDL 广播/回滚、`aux_save2` v2 与自动 SchemaSync；**P68** `FT.SEARCH` Query-Then-Fetch + $M$-way 归并；**P69** `FT.HYBRID` 全局融合、2-Phase BM25 DFS 与辅助命令集群化；**P70** `FT.AGGREGATE` 全 12 Reducer Map-Reduce 与跨节点 `FT.CURSOR`；**P71** 在线 `MIGRATE` 一致性、6 节点主从 Failover、`ON_TIMEOUT FAIL/RETURN` 与 `CLUSTER_AUTH_TOKEN`/白名单/限流加固；**P72** 集群 Soak、1N vs 3N 性能基线与文档收口 |

### 9.2 单机 `search_bench` 实测基线（Apple Silicon ARM64 NEON）

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

### 9.3 集群 1 节点 vs 3 节点实测基线（`ci/search_cluster_bench.py`，12,000 文档 × 128d HNSW + 2 KB 负载，Docker Redis 7.2.11）

| 查询场景 | 1 节点 P50 / P99 | 3 节点 P50 / P99 | 1 节点 QPS (6 并发) | 3 节点 QPS (6 并发) | 吞吐扩展比 |
|---|---|---|---|---|---|
| `FT.SEARCH BM25 (LIMIT 0 10)` | `7.76 / 8.07 ms` | **`2.83 / 5.27 ms`** | `128.1` | **`759.3`** | **`5.93×`** |
| `FT.SEARCH Deep (LIMIT 90 10)` | `8.77 / 9.06 ms` | **`4.39 / 5.52 ms`** | `115.6` | **`485.5`** | **`4.20×`** |
| `FT.SEARCH HNSW KNN-10` | `0.19 / 0.24 ms` | `0.69 / 0.80 ms` | `7243.9` | `4586.2` | `0.63×`（Coordinator+RPC 额外开销仅 `~0.50 ms`） |
| `FT.HYBRID RRF (K=20, L=10)` | `12.67 / 12.87 ms` | **`7.42 / 10.87 ms`** | `79.4` | **`404.8`** | **`5.10×`** |
| `FT.AGGREGATE GROUPBY 10 cats` | `25.35 / 27.62 ms` | **`8.70 / 11.79 ms`** | `41.3` | **`217.4`** | **`5.26×`** |

- **Query-Then-Fetch 带宽节省（3 分片、单文档 ~2.5 KB）**：`LIMIT 0 10` 从 `75.0 KB` 降至 `26.2 KB`（**-65.1%**）；`LIMIT 90 10` 从 `750.0 KB` 降至 `36.7 KB`（**-95.1%**）；`LIMIT 490 10` 从 `3,750.0 KB` 降至 `83.6 KB`（**-97.8%**）。

---

## 10. 后续演进方向（长期按需）

详见全仓规划 [doc/roadmap.md](../../doc/roadmap.md)：

1. **Product Quantization (PQ，乘积量化)**：面向千万级高维向量（768d/1536d）提供 64× 压缩（子空间 k-means 码本 + ADC 查表距离），并按需评估 mmap / 磁盘混合存储。
2. **多语言分词与音似增强**：统计/HMM 中文分词、更多语种词干与非英文音似编码。
