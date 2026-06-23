# 01 - 代码 Bug 审计

本文件只列当前代码中仍可能导致错误行为、恢复损坏、崩溃、DoS 或数据不一致的问题。RedisBloom 协议差异见 `02_redis_bloom_compatibility.md`。

## BUG-01：RDB layer metadata 校验仍弱于 wire 校验，可接受非法 filter 状态

**级别：P0/P1**

### 证据

`BloomLayer::ReadFrom()` 只校验了部分字段：

- `log2Bits_ >= 64`
- `hashCount_ == 0 && totalBits_ > 0`
- `log2Bits_ > 0 && totalBits_ != (1ULL << log2Bits_)`
- `totalBits_ > UINT64_MAX - 7`
- blob length 等于按 `totalBits_` 推导出的 `dataSize_`

位置：`modules/gemini-bloom/src/bloom_rdb.cc:54-84`。

但同文件 `ValidateLayerMeta()` 对 wire header 多做了：

- `totalBits == 0` reject
- `fpRate` finite 且 `> 0`
- `bitsPerEntry` finite 且 `>= 0`
- `dataSize == ceil(totalBits / 8)`

位置：`modules/gemini-bloom/src/bloom_rdb.cc:196-206`。

### 问题

RDB 路径仍可能接受 wire 路径会拒绝的状态，例如：

```text
totalBits = 0
hashCount = 1
log2Bits = 0
```

如果空 blob 和 `RMAlloc(0)` 在具体平台返回非空指针，该 layer 可加载成功。随后 `ProbePosition()` 会走 `raw % totalBits`，触发除 0。相关热路径在 `bloom_filter.h:82-87`、`bloom_filter.cc:156-181`。

RDB 路径也没有拒绝：

- `fpRate = NaN / Inf / >= 1`
- `bitsPerEntry = NaN / Inf`
- `capacity = 0`
- `hashCount` 与 `bitsPerEntry` 不匹配

### 影响

- 损坏 RDB 可能造成除 0、错误查询结果或无法扩容。
- `BF.INFO` / `BF.CARD` 可能暴露不可信元数据。
- 后续 AOF/RDB rewrite 会固化非法状态。

### 修复建议

抽出统一的 `ValidateLayerState()`，RDB 与 wire 都使用同一套规则。至少应覆盖 RedisBloom `bloom_validate_integrity()` 等价约束：

```cpp
if (capacity == 0) return false;
if (!std::isfinite(fpRate) || fpRate <= 0.0 || fpRate >= 1.0) return false;
if (!std::isfinite(bitsPerEntry) || bitsPerEntry <= 0.0) return false;
if (totalBits == 0) return false;
if (dataSize != (totalBits + 7) / 8) return false;
if (log2Bits > 0 && totalBits != (1ULL << log2Bits)) return false;
if (hashCount == 0) return false;
```

如果追求 RedisBloom 兼容，还应校验 `hashCount == ceil(ln2 * bitsPerEntry)`。

## BUG-02：反序列化不校验 item 计数一致性，`BF.CARD` 和扩容判断可被污染

**级别：P1**

### 证据

RDB load：

- filter header 读取 `totalItems`
- 每层读取 `itemCount`
- 没有检查 `sum(layer.itemCount) == totalItems`
- 没有检查 `layer.itemCount <= layer.capacity`

位置：`modules/gemini-bloom/src/bloom_rdb.cc:132-169`。

Wire header：

- `WireFilterHeader.totalItems` 和 `WireLayerMeta.itemCount` 被直接信任。
- `ValidateLayerMeta()` 没有检查 `itemCount`。

位置：`modules/gemini-bloom/src/bloom_rdb.cc:209-249`。

运行时：

- `BF.CARD` 直接返回 `filter->TotalItems()`。
- `GrowIfNeeded()` 只看 top layer 的 `itemCount < capacity`。

位置：`modules/gemini-bloom/src/bloom_commands.cc:461-477`，`modules/gemini-bloom/src/sb_chain.cc:102-112`。

### 问题

损坏 RDB 或 LOADCHUNK header 可以构造：

```text
totalItems = 1000000
layer0.itemCount = 0
capacity = 100
```

此时：

- `BF.CARD` 返回 1000000。
- `BF.INFO Items` 返回 1000000。
- 扩容逻辑认为当前层仍未满。

反过来，如果 `itemCount > capacity`，下次插入会过早扩容，fixed filter 会过早报满。

### 修复建议

加载完成后做 filter-level integrity check：

```cpp
uint64_t sum = 0;
for (const auto& layer : layers) {
  if (layer.itemCount > layer.bloom.GetCapacity()) return nullptr;
  if (sum > UINT64_MAX - layer.itemCount) return nullptr;
  sum += layer.itemCount;
}
if (sum != totalItems) return nullptr;
```

对于 historical RedisBloom 数据，如果存在计数不完全可靠的旧版本，需要显式版本分支和 golden corpus，而不是无条件信任。

## BUG-03：未知 flags 和未支持 flags 可从 RDB/wire 进入核心状态

**级别：P1/P2**

### 证据

`FromUnderlying(unsigned v)` 直接 `static_cast<BloomFlags>(v)`，不做 mask：

位置：`modules/gemini-bloom/src/bloom_filter.h:38-44`。

RDB 与 wire header 都直接使用：

- RDB：`bloom_rdb.cc:137-143`
- wire：`bloom_rdb.cc:217-235`

`BloomFlags::RawBits` 暴露为已知 flag，但命令层永远只创建 `Use64Bit | NoRound`，RawBits 语义没有可用命令入口，也没有完整测试。`BloomLayer::Create()` 的 RawBits 分支会设置 `hashCount_ = 0`，这对普通 `Test()` 语义是不安全的。

位置：`modules/gemini-bloom/src/bloom_filter.cc:103-107`。

### 问题

未知 bit 会被持久化、AOF rewrite、SCANDUMP header 继续传播。维护者后续新增 flag 时，旧数据中残留的未知 bit 可能突然改变行为。

RawBits 作为“看似支持”的 flag 暴露在 public enum 中，但当前核心 membership 逻辑并没有独立处理 zero-hash/raw-bit 模式。

### 修复建议

定义已支持 flags mask：

```cpp
constexpr unsigned kSupportedFlags =
  ToUnderlying(BloomFlags::NoRound) |
  ToUnderlying(BloomFlags::Use64Bit) |
  ToUnderlying(BloomFlags::FixedSize);
```

加载时拒绝未知 bit。若暂不支持 RawBits / `BLOOM_OPT_ENTS_IS_BITS`，也应拒绝；若要支持，应补齐独立语义和 golden tests。

## BUG-04：命令与加载路径缺少统一资源上限，可被超大参数触发内存/CPU DoS

**级别：P1**

### 证据

命令层：

- capacity 只要求 `long long > 0`。
- expansion 最大允许到 `UINT_MAX`。
- error rate 允许任意 `(0, 1)`。

位置：`modules/gemini-bloom/src/bloom_commands.cc:104-158`、`244-275`。

配置层：

- `INITIAL_SIZE` 只要求 `> 0`。
- `EXPANSION` 允许到 `UINT_MAX`。

位置：`modules/gemini-bloom/src/bloom_config.cc:26-49`。

加载层：

- wire header 只限制 `numLayers <= 1024`，没有限制每层 `dataSize` 或总 data size。
- RDB 路径依赖 blob 本身长度，没有统一的最大容量策略。

位置：`modules/gemini-bloom/src/bloom_rdb.cc:196-249`。

### 影响

用户可以请求极大的 filter，使 Redis 主线程在计算、分配或读取大 blob 时长时间阻塞或 OOM。`deny-oom` 只能阻止 Redis 已经处于 OOM 状态时继续执行，不能替代模块自己的参数上限。

### 修复建议

集中定义并复用资源上限，例如：

```text
capacity:       1 .. 1<<30
expansion:      0 .. 32768
numLayers:      1 .. 1024
dataSize/layer: <= configurable max
totalDataSize:  <= configurable max
errorRate:      (0, 0.25] 或明确选择 RedisBloom 行为
```

命令、module config、RDB、wire header 必须调用同一个 validator。

## BUG-05：multi-item full/error 语义会继续处理后续 item，容易产生难以重放的部分失败

**级别：P1/P2**

### 证据

`PutAndReply()` 遇到 full/expansion failure 会向当前数组写入 error，然后返回 `false`：

位置：`modules/gemini-bloom/src/bloom_commands.cc:88-96`。

`BF.MADD` 和 `BF.INSERT` 循环不停止：

- `BF.MADD`：`bloom_commands.cc:204-214`
- `BF.INSERT`：`bloom_commands.cc:343-352`

### 问题

对 fixed filter，如果 batch 前半部分插入成功，后面某个 item 报 full，当前实现仍会继续处理剩余 item，并最终在 `changed == true` 时 `RedisModule_ReplicateVerbatim()`。

这会产生几个风险：

- 返回数组包含 error 元素后还可能包含后续结果，客户端很难定义重试语义。
- AOF/replication 会记录一个执行期间产生 error 的写命令。
- 与 RedisBloom upstream 当前实现不一致；RedisBloom 在 `rv == -2` 后停止后续 item，并用 postponed array length 设置实际返回长度。

### 修复建议

至少在第一个不可恢复错误后停止处理后续 item，并使用 postponed array length：

```cpp
RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
size_t replied = 0;
for (...) {
  auto result = filter->Put(...);
  if (!result) {
    RedisModule_ReplyWithError(ctx, FilterFullError(filter));
    replied++;
    break;
  }
  ...
}
RedisModule_ReplySetArrayLength(ctx, replied);
```

更稳妥的设计是先 dry-run 容量或改成显式 prefix-commit 语义，并写入兼容测试。

