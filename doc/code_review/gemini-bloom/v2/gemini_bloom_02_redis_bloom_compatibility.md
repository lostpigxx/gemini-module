> 分析基准：GitHub 仓库 `lostpigxx/gemini-module` 的 `main` 分支当前可读取源码；重点目录为 `modules/gemini-bloom`。  
> 方法：静态源码审查 + 与当前 RedisBloom 官方源码/命令文档做行为对照。未在本地编译运行；容器内无法直接 `git clone`，因此结论基于 GitHub API 拉取到的文件内容。  
> 严重性：P0=可能崩溃/数据损坏/安全风险；P1=兼容性或核心语义错误；P2=明显设计/性能/可维护性缺陷；P3=低风险但应修正。

# 02. 与 RedisBloom 的兼容性问题

## 先确认已对齐的点

有些基础点是对齐 RedisBloom 的，避免误判：

1. Gemini 使用 RedisBloom Bloom 数据类型名 `MBbloom--`，并用 encoding version 4 注册类型：`RedisModule_CreateDataType(ctx, "MBbloom--", kCurrentEncVer, ...)`。
2. 当前 RedisBloom 官方源码也以 `MBbloom--` 和 `BF_MIN_GROWTH_ENC` 注册 Bloom 类型。
3. Gemini 的 RDB 字段顺序与 RedisBloom 当前源码的 `BFRdbSave()` 基本一致：filter 总数/flags/growth，然后每层 entries/error/hashes/bpe/bits/n2/bit-array/item-count。
4. Gemini 默认创建 filter 时使用 `Use64Bit | NoRound`，RedisBloom 创建 Bloom chain 时也使用 `BLOOM_OPT_FORCE64 | BLOOM_OPT_NOROUND`。

因此，核心 RDB 格式方向是对的。真正的问题集中在命令行为、SCANDUMP/LOADCHUNK 迭代协议、RESP3/ACL、错误语义和边界兼容。

## 问题清单

| ID | 严重性 | 位置 | 兼容性问题 |
|---|---:|---|---|
| COMPAT-01 | P0 | `BF.SCANDUMP`/`BF.LOADCHUNK` | 迭代器语义与 RedisBloom 官方实现不兼容 |
| COMPAT-02 | P0 | `BF.SCANDUMP` | Gemini 按“每层一个 chunk”，RedisBloom 按最大 16MB 连续 byte-range chunk |
| COMPAT-03 | P1 | `BF.LOADCHUNK` | Gemini 允许 header 替换已有 Bloom key；RedisBloom 要求 header 只能加载到空 key |
| COMPAT-04 | P1 | `BF.INFO key FIELD` | Gemini 返回裸 integer/null；RedisBloom 返回 RESP2 单元素 array / RESP3 map |
| COMPAT-05 | P1 | RESP3 | Gemini 没有 RESP3 bool/map 适配 |
| COMPAT-06 | P1 | command metadata | 缺少 RedisBloom 的 ACL category `bloom` 和 `fast` command flags |
| COMPAT-07 | P1 | `BF.RESERVE EXPANSION 0` | RedisBloom 源码把 0 视作 NONSCALING；Gemini 直接拒绝 |
| COMPAT-08 | P1 | `BF.RESERVE` wrong type | Gemini 返回 `ERR key already exists`，RedisBloom 返回 WRONGTYPE |
| COMPAT-09 | P1 | `BF.RESERVE` arity/option handling | Gemini 没有 RedisBloom 的最大 arity 和配置范围约束 |
| COMPAT-10 | P1 | `BF.INSERT` full/error behavior | 多元素写入遇到 full 的回复结构和停止规则不一致 |
| COMPAT-11 | P1 | module coexistence | 不能与 RedisBloom 同时加载：命令名和数据类型名冲突 |
| COMPAT-12 | P3 | `BF.DEBUG` | RedisBloom 源码注册 `bf.debug`，Gemini 未实现；公开命令兼容时应记录为调试命令差异 |
| COMPAT-13 | P1 | RDB load validation | Gemini 可能接受 RedisBloom 会拒绝的损坏 RDB |
| COMPAT-14 | P2 | defrag callback | RedisBloom Bloom type 有 defrag；Gemini type methods 没有 |
| COMPAT-15 | P2 | module identity/config | RedisBloom module name/config 体系与 Gemini 不同 |
| COMPAT-16 | P2 | command error strings | 多处错误字符串不一致，影响严格客户端测试 |
| COMPAT-17 | P2 | `NOCREATE + CAPACITY/ERROR` | 与 Redis 官方文档描述不一致；需按目标 RedisBloom 版本确认 |
| COMPAT-18 | P2 | SCANDUMP header length | RedisBloom 要求 header 长度精确，Gemini 接受尾随字节 |

## 详细问题

### COMPAT-01：SCANDUMP/LOADCHUNK 迭代器语义不兼容

Gemini 的协议注释和实现是：

```text
SCANDUMP key 0   → [1, header]
SCANDUMP key 1   → [2, layer0_bits]
SCANDUMP key 2   → [3, layer1_bits]
...
```

代码中，cursor > 0 时把 `cursor - 1` 当层索引：

```cpp
size_t idx = static_cast<size_t>(cursor - 1);
ReplyWithLongLong(ctx, cursor + 1);
ReplyWithStringBuffer(... layer.bloom.GetBitArray(), layer.bloom.GetDataSize());
```

RedisBloom 官方实现不是“层索引迭代器”，而是“跨所有 filter bit-array 的字节偏移迭代器”：

```c
const char *SBChain_GetEncodedChunk(const SBChain *sb, long long *curIter, size_t *len,
                                    size_t maxChunkSize) {
    SBLink *link = getLinkPos(sb, *curIter, &offset);
    ...
    *len = maxChunkSize;
    size_t linkRemaining = link->inner.bytes - offset;
    if (linkRemaining < *len) *len = linkRemaining;
    *curIter += *len;
    return (const char *)(link->inner.bf + offset);
}
```

后果：

- RedisBloom dump 出来的 chunk cursor 通常是 `1 + chunk_length`，不是 `2`。
- Gemini `BF.LOADCHUNK dst <official_cursor> <chunk>` 会把 cursor 解释成 `idx = cursor - 2`，大概率报 `cursor exceeds layer count`。
- Gemini dump 出来的第二个 cursor 是 `2`，RedisBloom `SBChain_LoadEncodedChunk()` 会检查 `iter < bufLen`，当 chunk 大于 2 字节时直接拒绝。
- 所以 header 布局即使一致，chunk 迭代协议仍然不互通。

修复建议：

- 实现 RedisBloom 的 byte-offset iterator，而不是 layer-index iterator。
- 设置 chunk 最大尺寸。
- `LOADCHUNK` 应按 `iter - bufLen` 找目标 offset，而不是按 `cursor - 2` 找 layer。

### COMPAT-02：Gemini chunk 粒度是整层，RedisBloom 是最大 16MB

RedisBloom 定义：

```c
#define MAX_SCANDUMP_SIZE (1024 * 1024 * 16)
...
SBChain_GetEncodedChunk(sb, &iter, &bufLen, MAX_SCANDUMP_SIZE)
```

Gemini 当前一次返回整层 bit array：

```cpp
ReplyWithStringBuffer(... layer.bloom.GetBitArray(), layer.bloom.GetDataSize());
```

当单层大于 16MB：

- Gemini 返回超大 bulk string。
- 与 RedisBloom 客户端的 chunk-size 假设不一致。
- AOF rewrite 也会产生非常大的单条 `BF.LOADCHUNK`。

修复建议：按 RedisBloom 的 `MAX_SCANDUMP_SIZE` 或可配置上限分块。

### COMPAT-03：LOADCHUNK header 对已有 Bloom key 的处理不一致

RedisBloom 官方逻辑：

```c
if (status == SB_EMPTY && iter == 1) {
    SBChain *sb = SB_NewChainFromHeader(...);
    RedisModule_ModuleTypeSetValue(key, BFType, sb);
} else if (status != SB_OK) {
    return RedisModule_ReplyWithError(ctx, statusStrerror(status));
}
...
SBChain_LoadEncodedChunk(sb, iter, buf, bufLen, &errMsg)
```

也就是 header 只能创建空 key。已有 Bloom key 不会被 header 替换。

Gemini 在 `cursor == 1` 且 key 是已有 Bloom 时会：

```cpp
RedisModule_DeleteKey(key);
...
RedisModule_ModuleTypeSetValue(key, BloomType, filter);
```

这会破坏 RedisBloom 兼容性，也给误用 `LOADCHUNK` 带来数据覆盖风险。

修复建议：按 RedisBloom 行为拒绝对已有 key 的 header load；如果产品要支持 replace，应另设命令或显式 `REPLACE` 参数。

### COMPAT-04：`BF.INFO key FIELD` 返回形态不一致

RedisBloom 官方 `BFInfo_RedisCommand()` 对单字段请求使用：

```c
RedisModule_ReplyWithMapOrArray(ctx, 1, false);
...
RedisModule_ReplyWithLongLong(ctx, value);
```

这意味着 RESP2 下是单元素 array，RESP3 下是 map-like 回复。

Gemini 当前直接返回：

```cpp
return RedisModule_ReplyWithLongLong(ctx, value);
```

测试里也写死期望：

```tcl
r BF.INFO reserve_basic Capacity
} {1000}
```

严格兼容客户端如果按 RedisBloom 的单元素数组解析，会失败。

修复建议：实现 `ReplyWithMapOrArray` 等价逻辑；至少 RESP2 下返回 array。

### COMPAT-05：没有 RESP3 适配

RedisBloom 官方对 boolean 响应会根据 RESP3 返回 bool：

```c
if (_is_resp3(ctx)) {
    RedisModule_ReplyWithBool(ctx, reply);
} else {
    RedisModule_ReplyWithLongLong(ctx, reply ? 1 : 0);
}
```

`BF.INFO` 也会根据 RESP3 返回 map。

Gemini 全部使用 integer/array/simple string，没有检测 `REDISMODULE_CTX_FLAGS_RESP3`。这会影响 RESP3 客户端行为和官方文档兼容。

修复建议：

- 增加 `IsResp3(ctx)` helper。
- `BF.ADD/BF.MADD/BF.INSERT/BF.EXISTS/BF.MEXISTS` 在 RESP3 下返回 bool。
- `BF.INFO` 在 RESP3 下返回 map。

### COMPAT-06：缺少 ACL category 和 fast flags

RedisBloom 官方注册命令时带 ACL category 和更精细 flags，例如：

```c
RegisterCommand(ctx, "bf.exists", BFCheck_RedisCommand, "readonly fast", "read");
RegisterCommand(ctx, "bf.info", BFInfo_RedisCommand, "readonly fast", "read fast");
RegisterCommand(ctx, "bf.scandump", BFScanDump_RedisCommand, "readonly fast", "read");
```

Gemini 当前：

```cpp
{"BF.EXISTS",   CmdExists,    "readonly"},
{"BF.MEXISTS",  CmdMexists,   "readonly"},
{"BF.INFO",     CmdInfo,      "readonly"},
{"BF.SCANDUMP", CmdScandump,  "readonly"},
```

影响：

- `ACL CAT bloom` / `COMMAND INFO` 与 RedisBloom 不一致。
- 客户端、代理、运维工具依赖 command metadata 时会出现差异。
- `fast` 缺失影响 Redis 的命令分类。

修复建议：引入 RedisBloom 等价的 ACL category 和 command info 注册。

### COMPAT-07：`EXPANSION 0` 行为不一致

RedisBloom 官方 `BF.RESERVE` 源码里：

```c
if (expansion == 0) {
    nonScaling = BLOOM_OPT_NO_SCALING;
} else if (nonScaling == BLOOM_OPT_NO_SCALING) {
    return RedisModule_ReplyWithError(ctx, "Nonscaling filters cannot expand");
}
```

`BF.INSERT` 解析后也有：

```c
if (options.expansion == 0) {
    options.nonScaling = BLOOM_OPT_NO_SCALING;
}
```

Gemini 则把 `EXPANSION 0` 视为非法：

```cpp
val < 1
```

这会导致某些 RedisBloom 客户端或迁移脚本无法复用。

修复建议：如果目标是 RedisBloom 源码兼容，应接受 `EXPANSION 0` 并等价为 `NONSCALING`。如果目标是按当前文档“positive integer”，则需要明确写入兼容性矩阵。

### COMPAT-08：`BF.RESERVE` wrong type 错误不兼容

RedisBloom 对 existing key 通过 `bfGetChain()` 区分：

- empty → 可创建
- existing Bloom → `ERR item exists`
- wrong type → WRONGTYPE

Gemini 对所有非空 key 都返回：

```cpp
ERR key already exists
```

严格客户端测试会失败，错误处理逻辑也会误判。

### COMPAT-09：`BF.RESERVE` 参数范围和 arity 不一致

RedisBloom 源码：

```c
if (argc < 4 || argc > 7) return RedisModule_WrongArity(ctx);
```

同时用配置项的 min/max 校验 error rate、capacity、expansion。

Gemini：

- 只检查 `argc < 4`。
- 没有最大 arity。
- 允许重复 option 反复覆盖。
- 没有 capacity/expansion 上限。
- 可能产生 `unsigned` 截断问题。

这不仅是安全问题，也是兼容性问题：RedisBloom 会拒绝的命令，Gemini 可能接受。

### COMPAT-10：`BF.INSERT`/`BF.MADD` full 行为不一致

RedisBloom `bfInsertCommon()` 遇到 `SB_FULL`：

```c
if (rv == -2) {
    RedisModule_ReplyWithError(ctx, "ERR non scaling filter is full");
}
...
for (size_t ii = 0; ii < nitems && rv != -2; ++ii)
```

它会停止后续 items，并用 postponed array length 修正实际返回数量。

Gemini 使用固定长度 array：

```cpp
RedisModule_ReplyWithArray(ctx, count);
```

然后每个 item 调 `PutAndReply()`。遇到 full 后并不中止循环，也没有修正 array length。行为与 RedisBloom 源码不一致。

### COMPAT-11：不能与 RedisBloom 同时加载

Gemini：

- 注册同名命令 `BF.*`
- 注册同名数据类型 `MBbloom--`

RedisBloom 也注册这些命令和数据类型。Redis module command/type name 都要求唯一。因此 Gemini 不能与 RedisBloom 同时加载。

这是一个产品定位问题：

- 如果是 drop-in replacement：同名是合理的，但要说明不能共存。
- 如果是独立 Gemini 模块：应使用命令 namespace，例如 `GBF.*`，并避免复用 `MBbloom--`。

### COMPAT-12：缺少 `BF.DEBUG`

RedisBloom 当前源码注册：

```c
RegisterCommand(ctx, "bf.debug", BFDebug_RedisCommand, "readonly fast", "read");
```

Gemini 只注册 10 个公开 BF 命令，没有 `BF.DEBUG`。

复核结论：存在差异，但原报告列为 P1 过重。`BF.DEBUG` 更接近源码级/诊断命令兼容项，不是公开 BF 命令集合里的核心读写语义。如果目标是 RedisBloom 源码级命令全集兼容，应实现该命令；如果目标是公开文档命令兼容，应把它列为“非公开/调试命令不兼容”，并在兼容性声明里写清楚。

### COMPAT-13：RDB load 校验强度不一致

RedisBloom `BFRdbLoad()` 载入每层后会调用：

```c
if (bloom_validate_integrity(bm) != 0) {
    err = true;
    return NULL;
}
```

Gemini RDB load 没有等价完整校验。结果是：

- RedisBloom 会拒绝的损坏 RDB，Gemini 可能接受。
- 接受后可能在查询时触发 false negative、OOB 或 UB。
- 这会造成“能加载但行为不等价”的隐性兼容问题。

### COMPAT-14：缺少 defrag callback

RedisBloom type methods 包含：

```c
.defrag = BFDefrag
```

Gemini type methods 只有：

```cpp
rdb_load, rdb_save, aof_rewrite, free, mem_usage
```

缺少 active defrag 支持。功能上不是命令兼容问题，但在开启 Redis active defragmentation 的生产环境下，内存行为与 RedisBloom 不一致。

### COMPAT-15：module identity 和配置体系不同

RedisBloom：

```c
RedisModule_Init(ctx, "bf", REBLOOM_MODULE_VERSION, ...)
RedisModule_RegisterStringConfig / RedisModule_LoadConfigs
```

Gemini：

```cpp
RedisModule_Init(ctx, "GeminiBloom", 1, ...)
BloomConfigLoad(ctx, argv, argc)
```

影响：

- `MODULE LIST` / `INFO MODULES` 显示不同。
- Redis config system 中不能像 RedisBloom 一样 `CONFIG GET/SET` 模块配置。
- 加载参数名称和范围也不完全一致。

如果目标是“兼容 RedisBloom 的 BF 命令”，这可以接受；如果目标是“替换 RedisBloom 模块”，则不够。

### COMPAT-16：错误字符串不一致

示例：

- RedisBloom missing/wrong lookup 通过 `statusStrerror()` 返回 `"ERR not found"` 或 WRONGTYPE。
- Gemini `BF.INFO` 对空 key 返回 `"ERR key does not exist"`。
- Gemini `BF.RESERVE` 对已有 key 返回 `"ERR key already exists"`，RedisBloom 对已有 Bloom 是 `"ERR item exists"`。

Redis 协议通常不建议客户端强依赖错误文本，但 RedisBloom 测试、迁移脚本、SDK 可能会匹配文本。

### COMPAT-17：`NOCREATE + CAPACITY/ERROR` 与 Redis 官方文档不一致

Redis 官方 BF.INSERT 文档写明：`NOCREATE` 与 `CAPACITY` 或 `ERROR` 同时指定是错误。Gemini parser 没有阻止：

```cpp
BF.INSERT k NOCREATE CAPACITY 1000 ERROR 0.01 ITEMS a
```

但当前 RedisBloom master 源码片段里也未显式阻止该组合。因此这里需要按目标版本确认：

- 若以官方文档为准：Gemini 不兼容。
- 若以当前 RedisBloom 源码为准：Gemini 可能一致，但需要测试验证具体发布版本。

### COMPAT-18：SCANDUMP header 长度校验不一致

RedisBloom `SB_NewChainFromHeader()` 要求：

```c
if (bufLen != sizeof(*header) + sizeof(header->links[0]) * header->nfilters) goto err;
```

Gemini 只要求：

```cpp
if (length < required) return nullptr;
```

即允许尾随垃圾。严格格式兼容应拒绝 `length > required`。
