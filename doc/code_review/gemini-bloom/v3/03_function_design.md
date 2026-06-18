# 03 — 功能设计问题

本文件关注 API 设计、边界语义、用户可预期性和长期演进。底层越界/崩溃类问题见 `01_code_bugs.md`；与 RedisBloom 的协议差异见 `02_redis_bloom_compatibility.md`。

## DESIGN-01：项目声明“full BF.* command set”，但实现目标没有定义清楚

**级别：P1/P2**

README 写 gemini-bloom 支持 full `BF.*` command set，并列出 10 个命令。源码注释多处写“intended to match RedisBloom / full compatibility not verified”。当前设计没有明确回答：

```text
目标是：
1. 命令名兼容？
2. RedisBloom 行为兼容？
3. RDB/SCANDUMP/AOF 二进制兼容？
4. RESP2/RESP3 回复类型兼容？
5. 只支持 subset？
```

这几个目标的工程要求完全不同。当前实现复用了 `MBbloom--` data type name，又没有完成 RedisBloom wire/RDB 兼容，风险最高。

### 建议

新增 `modules/gemini-bloom/COMPATIBILITY.md`：

| 层级 | 当前状态 | 目标 | 测试依据 |
|---|---|---|---|
| 命令名 | 部分 | 完全/部分 | 命令矩阵 |
| 参数解析 | 部分 | RedisBloom 兼容 | official parser tests |
| RESP2 | 部分 | 完全 | protocol golden |
| RESP3 | 缺失 | 完全/不支持 | HELLO 3 tests |
| SCANDUMP/LOADCHUNK | 私有协议 | official wire | golden corpus |
| RDB/AOF | 未验证 | official binary | RedisBloom cross-load |

---

## DESIGN-02：序列化格式同时承担“内部持久化”和“RedisBloom 兼容”两个目标，导致边界混乱

**级别：P1**

`bloom_rdb.cc` 同时实现 Redis Module RDB callbacks、SCANDUMP/LOADCHUNK wire protocol、AOF rewrite、layer metadata 转换、integrity validation。但格式版本、兼容性目标和验证策略没有被拆开。

结果是：

```text
RDB load 校验弱
wire load 校验稍强但仍不完整
AOF rewrite 依赖 custom LOADCHUNK
DataType 名称却使用 RedisBloom 名称
```

### 建议

拆成三层：

```text
+-------------------------+
| Redis callbacks          |
| rdb_load/save/aof        |
+------------+------------+
             |
+------------v------------+
| Format adapters          |
| gemini-native / redisbloom|
+------------+------------+
             |
+------------v------------+
| Core validated model     |
| ScalingBloomFilter       |
+-------------------------+
```

---

## DESIGN-03：错误模型过于粗糙，`std::optional<bool>` 无法表达失败原因

**级别：P2**

`ScalingBloomFilter::Put()` 返回：

```cpp
std::optional<bool>
// true  = inserted
// false = duplicate
// nullopt = full/failure
```

调用方把所有 `nullopt` 都解释为 `ERR reached capacity limit (non-scaling mode)`。但 `nullopt` 可能代表 non-scaling 满、capacity overflow、expansion factor 非法、FP rate underflow、allocation failure、corrupt loaded metadata 等。

### 建议

改成结构化结果：

```cpp
enum class PutStatus {
  Inserted,
  Duplicate,
  FullNonScaling,
  CapacityOverflow,
  FpRateUnderflow,
  Oom,
  CorruptState,
};
```

命令层根据状态产生协议兼容错误。

---

## DESIGN-04：多 item 命令没有明确 partial-success 语义

**级别：P2**

`BF.MADD`、`BF.INSERT` 一边插入一边回复。遇到 non-scaling full 时，已插入的前缀保留，后续仍继续处理。

当前没有定义：

- 出错前的成功项是否应保留？
- 出错后是否继续处理？
- 返回数组长度是否必须等于输入 item 数？
- AOF 是否只复制有变更命令？
- 客户端重试应如何避免重复插入？

### 建议

选择并文档化一种：

1. RedisBloom 兼容：对齐 official 行为。
2. All-or-nothing：先 dry-run 容量，再统一 commit。
3. Prefix commit：明确第一个错误后停止，返回前缀结果 + error。

当前实现是“固定长度数组 + error element + 继续”，最难维护。

---

## DESIGN-05：`BF.LOADCHUNK` header 后 key 立即可见，缺少 loading 状态

**级别：P2**

`LOADCHUNK cursor=1` 创建 filter shell，后续 chunks 才填充 bit array。此时 `BF.CARD` 已返回 header item count，但 `BF.EXISTS` 基于全 0 bit array。

### 影响

- 客户端中断会留下半恢复对象。
- 并发读会观察到自相矛盾状态。
- AOF/replica 如果中途失败，排障困难。

### 建议

引入 staging object：

```text
LOADCHUNK header -> LoadingBloomFilter
LOADCHUNK chunks -> 填充
LOADCHUNK final  -> validate -> publish as ScalingBloomFilter
```

如果 RedisBloom 兼容协议没有显式 final，则可以在加载完最后一个字节时自动 publish。

---

## DESIGN-06：核心类型暴露了过多序列化内部接口

**级别：P2**

`ScalingBloomFilter` public API 包含 `Layers()`、`RdbShell`、`FromRdbShell()`、`SetLayer()`、`WriteTo()`、`ReadFrom()`。

核心数据结构与持久化层高度耦合。`SetLayer()` 这种要求调用方理解对象 lifetime 的方法是 public，会扩大误用面。

### 建议

- `ScalingBloomFilter` 只暴露业务语义：`Put`、`Contains`、`Info`、`MemoryUsage`。
- 序列化层通过 friend/internal builder 访问。
- builder 管理“已构造层数”，消除 `01_code_bugs.md#BUG-02` 类生命周期风险。

---

## DESIGN-07：capacity、items、memory size 没有统一的上限策略

**级别：P2**

命令层 capacity 只要求 `long long > 0`，expansion 最大 `UINT_MAX`。RedisBloom official config 对 initial size、expansion factor 有明确 max，例如 bf initial size 最大 `1<<30`、expansion 最大 `32768`。

### 影响

- 用户可以创建极大参数，导致 allocation failure 或极慢运算。
- `BF.INFO` signed integer reply 可能溢出。
- 持久化加载与命令创建的约束不一致。

### 建议

设置统一 config 上限：

```text
capacity:       1 .. 1<<30   // 或可配置
expansion:      0/1 .. 32768
numLayers:      1 .. 1024
dataSize:       <= server/module memory policy
totalItems:     <= LLONG_MAX
```

加载路径、命令路径、config 路径共享同一套 validator。

---

## DESIGN-08：是否支持 RedisBloom rounded/RawBits/32-bit 历史数据没有产品决策

**级别：P2**

源码中保留了 `NoRound`、`RawBits`、`Use64Bit` 等 flags，并在 RDB encver 中尝试支持老格式。但命令层永远创建 `Use64Bit | NoRound`，RawBits 实现又不可用。

### 影响

- 用户误以为旧 RedisBloom 数据可加载。
- 维护者不清楚哪些 flag 应被支持、哪些应拒绝。
- 测试矩阵不完整。

### 建议

明确：

| Flag | 创建支持 | 加载支持 | 行为 |
|---|---:|---:|---|
| NoRound | 是 | 是 | 对齐 RedisBloom |
| Use64Bit | 是 | 是 | 默认 |
| RawBits/ENTS_IS_BITS | 否/是 | 否/是 | 二选一，不能半实现 |
| FixedSize | 是 | 是 | NONSCALING |
| 32-bit hash | 否/兼容加载 | 待定 | 需要 golden |

---

## DESIGN-09：配置设计仍是 module-load 参数风格，未对齐 Redis 运行时配置

**级别：P3/P2**

`BloomConfigLoad()` 只解析 `ERROR_RATE`、`INITIAL_SIZE`、`EXPANSION` 这类 loadmodule argv。RedisBloom official 已有 Redis module config registration。

### 影响

用户无法通过 Redis config 机制查看/设置 Bloom 默认参数，也没有 `CONFIG GET`/`CONFIG SET` 兼容层。

### 建议

若目标是现代 Redis 模块，使用 `RedisModule_RegisterStringConfig` 一类机制；保留 loadmodule args 作为兼容入口。

---

## DESIGN-10：核心算法没有“compatibility mode”和“native mode”的显式边界

**级别：P2**

目前实现既想轻量自研，又复用 RedisBloom 名称/格式。更稳妥的设计是分成：

```text
native mode:
  data type = GeminiBloom private name
  wire/rdb  = 自有格式
  命令语义 = 文档明确

redisbloom mode:
  data type = RedisBloom compatible
  wire/rdb  = golden-corpus verified
  命令语义 = official tests
```

### 建议

在 OnLoad 参数中提供：

```text
COMPAT_MODE redisbloom|native
```

在未完成 official golden 测试前，默认应使用 native data type name，避免破坏用户数据预期。

---

## DESIGN-11：`BF.INFO Size` 的定义没有和 Redis `MEMORY USAGE` 关系说清

**级别：P3**

`BytesUsed()` 返回 `sizeof(ScalingBloomFilter) + layerCapacity_ * sizeof(FilterLayer) + bit arrays`，不包含 Redis key/object overhead，也不一定与 RedisBloom `BF.INFO Size` 完全一致。

### 建议

文档写清楚“module payload bytes”还是“Redis object total estimate”。最好增加 `MEMORY USAGE` 对照测试。

---

## DESIGN-12：没有面向迁移/互操作的工具设计

**级别：P3**

如果目标是替代 RedisBloom，用户需要从 official RedisBloom dump/RDB/AOF 导入、导出为 official 可读、检查 filter metadata、对比两个 filter 是否等价、验证 hash/probe/golden item。当前没有工具或命令支持这些任务。

### 建议

提供 `BF.DEBUG` 或 `BF.INFO DEBUG`，输出：

```text
format-version
flags/options
hash-width
bits/bytes alignment
per-layer bytes/bits/items/capacity/error/hash-count
compatibility-status
```
