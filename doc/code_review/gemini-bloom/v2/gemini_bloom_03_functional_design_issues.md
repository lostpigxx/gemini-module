> 分析基准：GitHub 仓库 `lostpigxx/gemini-module` 的 `main` 分支当前可读取源码；重点目录为 `modules/gemini-bloom`。  
> 方法：静态源码审查 + 与当前 RedisBloom 官方源码/命令文档做行为对照。未在本地编译运行；容器内无法直接 `git clone`，因此结论基于 GitHub API 拉取到的文件内容。  
> 严重性：P0=可能崩溃/数据损坏/安全风险；P1=兼容性或核心语义错误；P2=明显设计/性能/可维护性缺陷；P3=低风险但应修正。

# 03. 功能设计问题

## 总览

本文件关注“即使代码没有崩溃，也会让功能边界、产品定位、可运维性或用户语义变差”的问题。底层 bug 见 `01_code_bugs.md`；RedisBloom 对齐问题见 `02_redis_bloom_compatibility.md`。

| ID | 严重性 | 位置 | 设计问题 |
|---|---:|---|---|
| DESIGN-01 | P1 | 模块整体 | 产品定位不清：drop-in RedisBloom clone 还是 Gemini 独立模块 |
| DESIGN-02 | P1 | `BF.SCANDUMP`/`BF.LOADCHUNK` | 增量导入不是事务性的，半成品 key 对客户端可见 |
| DESIGN-03 | P1 | `BF.LOADCHUNK cursor=1` | header load 可破坏已有 Bloom key，没有显式 replace 语义 |
| DESIGN-04 | P1 | 多元素写入 | `BF.MADD`/`BF.INSERT` 没有定义清楚部分成功与回滚语义 |
| DESIGN-05 | P1 | 参数层 | 缺少资源上限策略，容易从功能入口变成 OOM/DoS 入口 |
| DESIGN-06 | P1 | 持久化格式 | wire format 没有显式 magic/version/checksum |
| DESIGN-07 | P2 | `BF.CARD`/`TotalItems` | “插入数量”语义没有解释 Bloom false positive 导致的欠计数 |
| DESIGN-08 | P2 | `BF.INFO` | 可观测性不足，缺少每层容量、误判率、已用量、饱和度 |
| DESIGN-09 | P2 | 配置 | 只支持 load-time argv，不支持 Redis module config 生命周期 |
| DESIGN-10 | P2 | 错误处理 | OOM 行为不一致：命令报错、SCANDUMP 伪结束、AOF rewrite 静默 |
| DESIGN-11 | P2 | 扩容策略 | `EXPANSION=1` 允许无限多同容量层，缺少引导/警告 |
| DESIGN-12 | P2 | 兼容声明 | 代码注释多次说“intended to match”，但没有用户可读兼容矩阵 |
| DESIGN-13 | P3 | 命令集 | 缺少调试/诊断命令或内部状态导出 |
| DESIGN-14 | P3 | 文档 | 缺少面向用户的行为文档、边界说明、迁移说明 |

## 详细问题

### DESIGN-01：产品定位不清

当前实现同时做了两件互相拉扯的事：

- 命令名使用 RedisBloom 的 `BF.*`。
- 数据类型名使用 RedisBloom 的 `MBbloom--`。
- 模块名却是 `GeminiBloom`。
- README/代码注释没有明确声明“完全替代 RedisBloom”还是“只实现 RedisBloom Bloom 子集”。

这会造成两个实际问题：

1. 如果目标是替代 RedisBloom，则当前 RESP3、SCANDUMP/LOADCHUNK、ACL、错误文本、`BF.INFO` 等还不够兼容。
2. 如果目标是 Gemini 自己的模块，则复用 `BF.*` 和 `MBbloom--` 会导致无法与 RedisBloom 共存。

建议新增一个顶层设计文档，明确：

- 支持的 RedisBloom 版本范围。
- 命令兼容级别：语法、返回值、错误文本、RESP2/RESP3、RDB、AOF、SCANDUMP。
- 是否允许与 RedisBloom 共存。
- 不兼容项列表。

### DESIGN-02：LOADCHUNK 导入过程不是事务性的

当前 `BF.LOADCHUNK` 的过程是：

1. cursor=1 创建一个空的 filter shell。
2. 后续 cursor 分别写入 bit array。
3. 任何时刻 key 都已经存在，可被 `BF.EXISTS`、`BF.INFO`、`BF.ADD` 访问。

这意味着：

- 客户端只加载 header 后宕机，key 会永久处于“全 0 bit array 但带 itemCount 元数据”的状态。
- 其他客户端可在导入中途查询，得到大量 false negative。
- 其他客户端可在导入中途写入，和后续 chunk 覆盖互相踩踏。

建议：

- 引入 import session：`LOADCHUNK` 写入临时对象，最后一个 chunk 才原子 publish。
- 或至少在对象内增加 `loading` 状态，未完成前所有 BF 命令返回错误。
- 或要求用户先导入到临时 key，再 `RENAME` 原子替换；模块文档必须明确。

### DESIGN-03：`LOADCHUNK cursor=1` 会替换已有 Bloom key

Gemini 对已有 Bloom key 的 header load 会先 delete，再 set 新 filter。这是隐式 replace。

问题：

- 用户误把 dump 加载到已有 key，会直接丢旧数据。
- 如果后续 chunk 加载失败，旧 key 已无法恢复。
- RedisBloom 官方行为是 header 只能加载到空 key；这更安全。

建议：

- 默认拒绝已有 key。
- 如确实需要 replace，设计成 `BF.LOADCHUNK key iter data REPLACE` 或单独 `BF.RESTORE` 命令。
- replace 应该先完整构造临时对象，成功后原子替换。

### DESIGN-04：多元素写入的部分成功语义不清

`BF.MADD` 和 `BF.INSERT` 都会逐项写入。对于 fixed-size filter，某个 item 可能触发 full。当前代码没有说明：

- full 之前成功写入的 item 是否应该保留。
- full 之后是否继续处理后续 item。
- 返回数组长度是否等于输入 item 数。
- 如果命令中间失败，是否复制到 AOF/replica。

这类语义必须明确，否则客户端无法安全重试。

建议选择一种：

- RedisBloom 兼容：按 RedisBloom 当前源码停止后续处理并设置实际数组长度。
- 事务语义：全体预检，任意失败则不改变状态。
- 部分成功语义：固定返回每个 item 状态，但 error 也必须作为稳定的 per-item 数据结构，而不是协议 error。

### DESIGN-05：资源上限缺失

入口参数缺少统一上限：

- `capacity`
- `error_rate`
- `expansion`
- `numLayers`
- `dataSize`
- SCANDUMP chunk size
- RDB/LOADCHUNK total memory

这会让功能入口直接变成内存攻击面。例如用户可创建极大 capacity，或者构造 header 让 `LOADCHUNK` 预分配巨大 bit arrays。

建议：

- 全局配置最大 capacity、最大 bytes、最大 layers、最大 chunk。
- 所有入口共用同一套 `ValidateConfig` / `ValidateWireHeader`。
- 在错误信息里明确超限字段。

### DESIGN-06：wire format 缺少 magic/version/checksum

`WireFilterHeader` 只有：

```cpp
uint64_t totalItems;
uint32_t numLayers;
uint32_t flags;
uint32_t expansionFactor;
```

它没有：

- magic number
- format version
- endian marker
- checksum
- header length
- per-chunk offset/range

因此随机二进制 payload 只要碰巧满足几个字段，就可能被当成合法 header。格式升级时也不容易区分不同版本。

建议：

如果必须兼容 RedisBloom SCANDUMP wire format，就按 RedisBloom 的格式走；如果设计 Gemini 自有格式，至少加：

```text
magic = "GMBF"
format_version
header_length
endianness
flags
crc32/header_crc
```

### DESIGN-07：`BF.CARD`/`TotalItems` 语义没有解释

Bloom filter 无法精确知道“真实唯一元素数”。当前 `TotalItems` 只在 `Put()` 返回新插入时增加：

```cpp
if (*result) totalItems_++;
```

但 Bloom filter 可能对一个从未插入过的 item 返回 false positive，于是该 item 不会写入，也不会计数。RedisBloom 文档通常把它解释为“导致至少一个 bit 被设为 1 的 item 数”或近似插入计数。

建议在文档和 `BF.INFO` 中明确：

- `BF.CARD` 不是集合基数。
- 它是“被 filter 认为新插入并导致状态改变的次数”。
- 在高误判率/高饱和度时会低估真实 unique input。

### DESIGN-08：可观测性不足

`BF.INFO` 只有：

- Capacity
- Size
- Number of filters
- Number of items inserted
- Expansion rate

对于排查误判率、扩容和容量问题，缺少：

- 每层 capacity
- 每层 itemCount
- 每层 error rate
- 每层 bit size / bytes
- 当前 top layer saturation
- hash count
- flags
- RDB/wire version

建议增加调试命令或扩展 info，例如：

```text
BF.DEBUG key
BF.INFO key DETAIL
```

如果要兼容 RedisBloom，可实现 `BF.DEBUG`。

### DESIGN-09：配置只在 load-time 生效

`BloomConfigLoad()` 只解析模块加载参数：

```cpp
ERROR_RATE
INITIAL_SIZE
EXPANSION
```

没有接入 Redis module config API。结果：

- 运行时无法 `CONFIG GET/SET`。
- 无法通过 Redis 配置文件统一管理。
- 没有 min/max 元数据。
- 与 RedisBloom 运维方式不一致。

建议：

- 用 `RedisModule_Register*Config` 注册配置项。
- 对不支持运行时修改的项标记 immutable。
- 保留 legacy load args，但内部统一走同一校验函数。

### DESIGN-10：OOM 行为不一致

不同路径的 OOM 行为：

- 创建 filter：返回 `ERR allocation failure`。
- `BF.SCANDUMP` header alloc 失败：返回 `[0, ""]`，伪装正常结束。
- AOF rewrite header alloc 失败：直接 return，可能丢 key。
- AppendLayer alloc 失败：`Put()` 返回 nullopt，被解释成 fixed-size capacity error，不一定是真正 OOM。

这让客户端和运维无法准确判断失败原因。

建议定义统一错误模型：

```text
ERR allocation failure
ERR non scaling filter is full
ERR corrupted payload
ERR unsupported payload
```

并让内部 API 区分 OOM 与 capacity full。

### DESIGN-11：`EXPANSION=1` 缺少风险提示

RedisBloom 文档允许 expansion=1，含义是“节省内存但创建更多 sub-filters”。Gemini 也允许。但当前实现没有任何可观测或限制：

- 多层越多，查询越慢。
- `BF.INFO` 的 Size/Capacity 每次 O(numLayers)。
- SCANDUMP/LOADCHUNK chunk 数量增加。

建议：

- 文档明确 expansion=1 的时间复杂度风险。
- 给默认值 2 保持不变。
- 可配置最大层数，达到后拒绝扩容。

### DESIGN-12：兼容声明只在代码注释里，而且语气是不确定的

代码里多处写：

- “intended to match RedisBloom”
- “full compatibility has not been verified”
- “for future interoperability”

这对实现者有帮助，但对用户不够。用户需要知道哪些是保证项。

建议新增 `docs/gemini-bloom-compatibility.md`：

```text
RedisBloom version tested: x.y.z
Commands: compatible / partial / incompatible
RDB: compatible with encver 4, tested by golden corpus
SCANDUMP: compatible/incompatible
RESP3: supported/not supported
Known differences
```

### DESIGN-13：缺少调试/诊断命令

当用户遇到误判率升高、容量达到、扩容层过多时，只靠 `BF.INFO` 不够。RedisBloom 源码提供 `BF.DEBUG`，Gemini 未实现。

建议：

- 实现 RedisBloom 兼容的 `BF.DEBUG`。
- 或提供 `BF.INFO key DETAIL`，但要避免破坏现有 `BF.INFO` 兼容性。

### DESIGN-14：用户文档不足

当前可见文档没有把以下行为讲清楚：

- `BF.ADD` auto-create 默认参数。
- `BF.INSERT NOCREATE` 与创建参数组合的行为。
- `NONSCALING` 满容量时的返回。
- `SCANDUMP/LOADCHUNK` 是否兼容 RedisBloom。
- `BF.CARD` 的概率语义。
- RDB/AOF 兼容性边界。
- RESP2/RESP3 差异。

建议为 `modules/gemini-bloom` 单独提供 README，并附协议示例和边界案例。
