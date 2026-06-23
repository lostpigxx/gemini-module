# 03 - 功能设计问题

本文件关注即使当前代码不崩溃，也会让功能边界、用户预期、恢复语义或长期演进变差的问题。

## DESIGN-01：产品定位仍不清楚：RedisBloom 兼容层还是 Gemini 私有模块

**级别：P1**

README 写：

```text
supporting the full BF.* command set
```

位置：`README.md:7-10`。

实现又同时具备以下信号：

- 命令名使用 RedisBloom `BF.*`。
- shared library 叫 `redis_bloom.so`。
- data type name 使用 RedisBloom `MBbloom--`。
- module identity 是 `GeminiBloom`。
- 多处注释承认格式“intended to match”但未验证。

这会让用户无法判断：

```text
能否替代 RedisBloom？
能否读取 RedisBloom RDB？
能否与 Redis 8 内置 Bloom 共存？
SCANDUMP/AOF 是否可跨实现迁移？
RESP3 客户端是否可直接使用？
```

### 建议

新增 `modules/gemini-bloom/COMPATIBILITY.md`，按层级写清楚：

| 层级 | 当前状态 | 目标 | 验证方式 |
|---|---|---|---|
| 命令名 | 覆盖主流 BF 命令 | 待定 | command matrix |
| 参数语义 | 部分兼容 | 待定 | parser golden |
| RESP2 | 部分兼容 | 待定 | raw RESP tests |
| RESP3 | 未兼容 | 待定 | HELLO 3 tests |
| SCANDUMP/LOADCHUNK | 私有协议 | 待定 | RedisBloom golden |
| RDB/AOF | 未验证互通 | 待定 | cross-load |

## DESIGN-02：`BF.LOADCHUNK` header 后 key 立即可见，半恢复对象可被读写

**级别：P1/P2**

`BF.LOADCHUNK cursor=1` 会创建完整 `ScalingBloomFilter` shell，但 bit arrays 仍是全 0。后续 layer chunks 才逐个填充。

位置：`modules/gemini-bloom/src/bloom_commands.cc:550-570`。

在 header 和最后一个 data chunk 之间：

- `BF.CARD` 返回 header 声明的 item count。
- `BF.INFO` 返回完整 metadata。
- `BF.EXISTS` 基于全 0 或部分恢复 bit array 查询。
- `BF.ADD` 可以修改尚未恢复完成的 filter。

### 建议

引入 loading/staging 状态：

```text
LOADCHUNK header -> LoadingBloomFilter
LOADCHUNK chunks -> 填充 staging buffer
加载完最后一个字节 -> validate -> 原子发布为 ScalingBloomFilter
```

如果目标是 RedisBloom 兼容，也至少应记录“LOADCHUNK 期间不得读写该 key”的限制，并用测试验证中断后的行为。

## DESIGN-03：序列化内部接口暴露到核心类型 public API

**级别：P2**

`ScalingBloomFilter` public API 包含：

- `Layers()`
- `RdbShell`
- `FromRdbShell()`
- `SetLayer()`
- `WriteTo()`
- `ReadFrom()`

位置：`modules/gemini-bloom/src/sb_chain.h:45-65`。

这些接口要求调用方理解 placement-new、layer 构造数量、bit array ownership 和序列化格式。它们本应属于 internal builder / codec。

### 建议

拆成：

```text
core:
  ScalingBloomFilter::Put / Contains / InfoSnapshot

codec:
  RdbCodec
  ScanDumpCodec
  RedisBloomCompatCodec

builder:
  ValidatedFilterBuilder
```

核心类型不应暴露可直接破坏内部状态的 `SetLayer()`。

## DESIGN-04：`std::optional<bool>` 无法表达插入失败原因

**级别：P2**

`ScalingBloomFilter::Put()` 返回：

```cpp
std::optional<bool>
// true = inserted
// false = duplicate
// nullopt = full/failure
```

位置：`modules/gemini-bloom/src/sb_chain.h:38-40`，`src/sb_chain.cc:117-127`。

但 `nullopt` 可能代表：

- fixed filter full
- expansion factor overflow
- next capacity overflow
- next FP rate 低于阈值
- allocation failure
- 从损坏 RDB 加载出的非法状态

命令层只能统一报：

```text
ERR non scaling filter is full
ERR filter expansion failed
```

位置：`modules/gemini-bloom/src/bloom_commands.cc:81-96`。

### 建议

改为结构化状态：

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

这样命令层才能返回兼容错误，并决定是否可继续处理 batch。

## DESIGN-05：资源策略没有产品化定义

**级别：P2**

当前 capacity、expansion、layer size、total dump size 没有统一上限。命令、配置、RDB、wire 各自校验，规则不一致。

### 建议

新增 `BloomLimits`：

```cpp
struct BloomLimits {
  uint64_t maxCapacity;
  uint32_t maxExpansion;
  uint32_t maxLayers;
  uint64_t maxLayerBytes;
  uint64_t maxTotalBytes;
  double maxErrorRate;
};
```

命令创建、配置加载、RDB load、LOADCHUNK header 都必须通过同一个 limits object。

## DESIGN-06：`BF.INFO Size` 语义没有说明是 RedisBloom 兼容值还是内部估算

**级别：P2/P3**

`BytesUsed()` 返回：

```cpp
sizeof(ScalingBloomFilter) + layerCapacity_ * sizeof(FilterLayer) + bit arrays
```

位置：`modules/gemini-bloom/src/sb_chain.cc:141-147`。

这不是 Redis allocator 实际占用，也未证明与 RedisBloom `BF.INFO Size` 一致。它还包含预留 layer slots，而用户可能理解为 bit array bytes。

### 建议

文档明确 Size 语义。若追求 RedisBloom 兼容，应根据 RedisBloom 结构统计并做 golden test；若追求内部估算，应换字段名或在 `BF.DEBUG` 中暴露细节。

## DESIGN-07：没有 debug/diagnostic 命令，排障只能依赖 5 个 INFO 字段

**级别：P3**

当用户怀疑 filter 损坏、层数异常、容量不合理、误判率过高时，当前只有：

```text
Capacity
Size
Number of filters
Number of items inserted
Expansion rate
```

### 建议

实现私有诊断命令，例如：

```text
GEMINI.BF.DEBUG key
```

输出每层 capacity、itemCount、fpRate、bits、bytes、hashCount、log2Bits、flags、saturation。

## DESIGN-08：native mode 与 RedisBloom compatibility mode 没有边界

**级别：P1/P2**

当前实现既想自研 C++ core，又复用 RedisBloom wire/name/commands。更稳妥的产品边界是：

```text
native mode:
  私有 data type name
  私有 RDB/AOF/LOADCHUNK 格式
  可选择更强校验和更好错误语义

compat mode:
  RedisBloom data type name
  RedisBloom RDB/AOF/SCANDUMP exact format
  RedisBloom RESP2/RESP3 exact shape
```

在没有这个边界前，每次修复都容易在“更好实现”和“兼容行为”之间摇摆。

