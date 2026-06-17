# 01 — 代码 Bug 审计

本文件只列“会直接造成错误行为、未定义行为、崩溃风险、数据损坏、恢复错误”的问题。RedisBloom 兼容性差异另见 `02_redis_bloom_compatibility.md`；测试缺口另见 `07_tests_coverage.md`。

## BUG-01：RDB/LOADCHUNK 可构造 `log2Bits` 与 `totalBits` 不一致，后续查询可能越界

**级别：P0**

### 证据

- `BloomLayer::UseBitMasking()` 只检查 `log2Bits_ > 0 && log2Bits_ < 64`。
- `ProbePosition()` 在 `useMask=true` 时返回 `raw & ((1ULL << log2Bits_) - 1)`。
- `BloomLayer::TestBit()`/`SetBit()` 对 `byteOffset` 没有边界检查。
- `BloomLayer::ReadFrom()` 只拒绝 `log2Bits >= 64` 和 `totalBits > UINT64_MAX - 7`，没有校验 `totalBits >= 1 << log2Bits` 或 `totalBits == 1 << log2Bits`。
- `ValidateLayerMeta()` 同样只检查 `log2Bits < 64`、`dataSize >= ceil(totalBits/8)`，没有校验 mask 空间与实际 bit array 一致。
- 相关位置：`src/bloom_filter.h:82-89, 137-141`，`src/bloom_filter.cc:130-142, 149-173`，`src/bloom_rdb.cc:56-84, 192-201`。

### 问题

恶意或损坏 RDB/LOADCHUNK header 可以设置：

```text
totalBits = 500
dataSize  = ceil(500 / 8) = 63
log2Bits  = 10
```

这会通过当前校验。之后 `UseBitMasking()` 为 true，probe 范围变成 `0..1023`。`TestBit(800)` 会访问 `bitArray_[100]`，但数组只有 63 字节。

### 影响

- 越界读：`BF.EXISTS` / `BF.MEXISTS` 可能读越界。
- 越界写：`BF.ADD` / `BF.MADD` / `BF.INSERT` 可能写越界。
- 可由持久化文件或 `BF.LOADCHUNK` 输入触发。
- 这类问题在 Redis 模块中通常会表现为 Redis 进程崩溃或堆破坏。

### 修复建议

在 RDB 和 wire header 校验中加入完整 bloom integrity 校验：

```cpp
if (meta.log2Bits > 0) {
  if (meta.log2Bits >= 64) return false;
  uint64_t maskBits = 1ULL << meta.log2Bits;
  if (meta.totalBits != maskBits) return false;
  if (meta.dataSize != ((maskBits + 7) / 8)) return false;
} else {
  if (meta.dataSize != ((meta.totalBits + 7) / 8)) return false;
}
```

如果要兼容 RedisBloom，rounded mode 还应以 official `bloom_validate_integrity()` 为金标准：`bits == bytes * 8` 且 `n2 != 0` 时 `bits >= 1ULL << n2`。

---

## BUG-02：`FromRdbShell()` 先设置 `numLayers_`，但层对象尚未构造；失败清理时析构未构造对象

**级别：P0**

### 证据

- `ScalingBloomFilter::FromRdbShell()` 使用 `RMCalloc(shell.numLayers, sizeof(FilterLayer))` 分配原始内存，并立即设置 `numLayers_ = shell.numLayers`。
- `SetLayer()` 对单个 slot placement-new 构造 `FilterLayer`。
- `ScalingBloomFilter::~ScalingBloomFilter()` 对 `0..numLayers_-1` 全部调用 `layers_[i].~FilterLayer()`。
- 相关位置：`src/sb_chain.cc:27-31, 153-175`，`src/bloom_rdb.cc:148-163, 234-241`。

### 问题

RDB load 或 header deserialize 的中间步骤失败时，只构造了前 N 个 layer，但析构函数会析构全部 `shell.numLayers` 个 slot。未构造对象上调用析构函数是 C++ 未定义行为。

```text
FromRdbShell(shell.numLayers = 3)
  layers_ = calloc(3 slots)
  numLayers_ = 3

ReadFrom loop:
  SetLayer(0)  // slot 0 constructed
  Read layer 1 fails
  filter->~ScalingBloomFilter()
    destruct slot 0
    destruct slot 1  // 未构造
    destruct slot 2  // 未构造
```

### 影响

- RDB/LOADCHUNK 错误路径可能触发 UB。
- ASAN/UBSAN 下应能暴露；生产上可能表现为随机崩溃或无声内存破坏。

### 修复建议

把 `numLayers_` 拆成“已构造数量”和“目标数量”。例如：

```cpp
filter->numLayers_ = 0;
filter->layerCapacity_ = shell.numLayers;

for (...) {
  filter->SetLayer(i, ...);
  filter->numLayers_++;
}
```

或者新增 `constructedLayers_`，析构只析构已构造层。

---

## BUG-03：RDB load 不校验 `expansionFactor == 0`，后续扩容会除以 0

**级别：P0**

### 证据

- `ScalingBloomFilter::ReadFrom()` 从 RDB 读 `shell.expansionFactor` 后没有校验。
- `GrowIfNeeded()` 中执行 `prevCap > UINT64_MAX / expansionFactor_`。
- wire `DeserializeHeader()` 有 `expansionFactor == 0` 校验，但 RDB 路径没有。
- 相关位置：`src/bloom_rdb.cc:130-143`，`src/sb_chain.cc:104-115`，`src/bloom_rdb.cc:212-215`。

### 问题

损坏 RDB 可以创建非 fixed-size filter，且 `expansionFactor_ = 0`。当最后一层满后执行 `GrowIfNeeded()`，会进行 `UINT64_MAX / 0`。

### 影响

- 运行时崩溃。
- 可由持久化文件触发，属于恢复路径输入校验缺陷。

### 修复建议

RDB 与 wire 使用同一套 header/filter integrity validator：

```cpp
if (!HasFlag(flags, BloomFlags::FixedSize) && expansionFactor == 0) return nullptr;
if (expansionFactor > UINT_MAX) return nullptr;
```

同时建议限制到 RedisBloom 官方范围：`0..32768`，其中 0 表示 non-scaling 或明确拒绝，但必须一致。

---

## BUG-04：RDB load 缺少 `hashCount`、`totalBits`、`fpRate`、`bitsPerEntry` 等基本校验

**级别：P0/P1**

### 证据

- `BloomLayer::ReadFrom()` 对 RDB 字段只做了有限检查：`log2Bits_ >= 64`、`totalBits_ > UINT64_MAX - 7`、blob 长度匹配。
- 不检查：`hashCount_ > 0`、`totalBits_ > 0`、`fpRate_` finite 且 `0 < fpRate < 1`、`bitsPerEntry_` finite 且 `> 0`、`capacity_ > 0`、`hashCount_ == ceil(ln2 * bitsPerEntry_)`。
- 相关位置：`src/bloom_rdb.cc:56-84`，`src/bloom_filter.cc:149-173`。

### 问题

几个直接错误例子：

```text
hashCount = 0, totalBits > 0
  Test() 循环 0 次，直接 return true
  => 所有元素都“存在”

totalBits = 0, hashCount > 0, log2Bits = 0
  ProbePosition(... % totalBits)
  => 除以 0

fpRate = NaN / Inf
  后续 BF.INFO/AOF/RDB 继续传播非法元数据
```

### 影响

- 查询语义严重错误。
- 可触发除 0。
- 恶意 RDB 可制造“所有元素存在”的 filter。

### 修复建议

把 `ValidateLayerMeta()` 抽成可复用 `ValidateLayerState()`，RDB 与 wire 都调用，并覆盖 official `bloom_validate_integrity()` 的约束。

---

## BUG-05：RDB/wire 不校验 item 计数一致性，`BF.CARD`/扩容行为可被损坏元数据污染

**级别：P1**

### 证据

- `ScalingBloomFilter::ReadFrom()` 读取 `totalItems` 和每层 `itemCount` 后，不校验 `totalItems == sum(layer.itemCount)`。
- `DeserializeHeader()` 同样信任 header 的 `totalItems` 和每层 `itemCount`。
- `GrowIfNeeded()` 只看 top layer 的 `itemCount < capacity`。
- 相关位置：`src/bloom_rdb.cc:118-166, 204-245`，`src/sb_chain.cc:104-129`。

### 问题

损坏数据可设置：

```text
totalItems = 1_000_000
layer0.itemCount = 0
```

此时 `BF.CARD` 返回 1,000,000，但扩容逻辑认为当前层未满。反过来，也可设置 `layer.itemCount > capacity`，导致下一次插入立即扩容，或者 fixed filter 过早拒绝。

### 影响

- `BF.CARD` 错。
- `BF.INFO Items` 错。
- 扩容/容量判断错。
- AOF/RDB 再保存会固化错误元数据。

### 修复建议

加载后检查：

```cpp
uint64_t sum = 0;
for (layer : layers) {
  if (layer.itemCount > layer.bloom.GetCapacity()) return nullptr;
  if (sum > UINT64_MAX - layer.itemCount) return nullptr;
  sum += layer.itemCount;
}
if (sum != totalItems) return nullptr;
```

---

## BUG-06：`ValidateLayerMeta()` 允许 `dataSize > expectedSize`，可导致 LOADCHUNK 内存放大

**级别：P1/P0（取决于 Redis allocator 行为）**

### 证据

- `ValidateLayerMeta()` 只拒绝 `meta.dataSize < expectedSize`，允许任意更大的 `meta.dataSize`。
- `BloomLayer::FromWireMeta()` 按 `meta.dataSize` `RMCalloc()`。
- `BF.LOADCHUNK` 对 data chunk 要求 `dataLen == layer.bloom.GetDataSize()`。
- 相关位置：`src/bloom_rdb.cc:192-201, 99-110`，`src/bloom_commands.cc:223-231`。

### 问题

攻击者可以构造：

```text
totalBits = 1024
expectedSize = 128
dataSize = 4GB
```

header 校验通过，模块尝试分配 4GB，然后要求后续 chunk 也必须是 4GB。

### 影响

- 单个 LOADCHUNK header 可造成大内存分配。
- 如果 RedisModule_Calloc 不是 TryCalloc 路径，可能带来进程级 OOM 风险。
- 即便分配失败，错误路径仍可能触发 BUG-02 的未构造 slot 析构问题。

### 修复建议

对 normal/NoRound 模式强制 `dataSize == expectedSize`。如果为了兼容官方 RedisBloom，expected 应按 official `bytes` 规则计算，而不是按当前 raw `totalBits` 规则。

---

## BUG-07：`DeserializeHeader()` 接受 trailing bytes，损坏 header 不会被拒绝

**级别：P1**

### 证据

- `DeserializeHeader()` 只检查 `length < required`，没有检查 `length == required`。
- RedisBloom official `SB_NewChainFromHeader()` 要求 header 长度精确等于结构大小。
- 相关位置：`src/bloom_rdb.cc:204-210`。

### 问题

`BF.LOADCHUNK key 1 <valid-header + extra-bytes>` 会被接受。多余字节被忽略。

### 影响

- 与 official 行为不一致。
- 客户端/工具若使用精确校验来判断 dump 是否损坏，gemini-bloom 会接受部分损坏数据。
- 也可能掩盖协议拼接/编码错误。

### 修复建议

```cpp
if (length != required) return nullptr;
```

---

## BUG-08：`RawBits`/`BloomFlags::RawBits` 实现是坏的：`hashCount_ = 0`

**级别：P1**

### 证据

- `BloomLayer::Create()` 在 `RawBits` 分支设置 `bitsPerEntry_ = 0`、`totalBits_ = cap`、`hashCount_ = 0`。
- `Test()` 在 `hashCount_ == 0` 时返回 true。
- wire 校验又拒绝 `hashCount == 0 && totalBits > 0`。
- 相关位置：`src/bloom_filter.cc:100-103, 149-157`，`src/bloom_rdb.cc:192-201`。

### 问题

该 flag 看起来想对应 RedisBloom 的 `BLOOM_OPT_ENTS_IS_BITS`，但 official 语义是“输入是 bit 指数 n2”，仍会计算 `entries`、`bpe`、`hashes`。当前实现创建出来的 layer 没有 hash 函数，语义不可用。

### 影响

- 任何内部或未来命令使用 `RawBits` 都会产生“所有元素存在”的 filter。
- wire 校验与构造逻辑互相矛盾：构造会生成 wire 自己拒绝的数据。

### 修复建议

要么删除 `RawBits`，要么按 RedisBloom `BLOOM_OPT_ENTS_IS_BITS` 实现完整语义：限制参数范围、计算 bits、entries、bpe、hashes，并加测试。

---

## BUG-09：scaling filter 有隐藏最大层数/容量，满后返回 “non-scaling mode” 错误

**级别：P1/P2**

### 证据

- `GrowIfNeeded()` 设置 `constexpr double kMinFpRate = 1e-15`。
- 当下一层 `nextRate < 1e-15` 时返回 false。
- `PutAndReply()` 把任何 `std::nullopt` 都回复为 `ERR reached capacity limit (non-scaling mode)`。
- 相关位置：`src/sb_chain.cc:104-115`，`src/bloom_commands.cc:83-92, 176-180`。

### 问题

这是 scaling filter，不是 non-scaling filter，但超过内部 `kMinFpRate` 后会像 fixed filter 一样拒绝。错误信息误导用户，且行为没有在 API/README 中说明。

### 影响

- 长生命周期 filter 在增长到一定层数后会突然停止扩展。
- 错误信息误判问题根因。
- AOF/RDB 恢复后也继承这个隐藏状态。

### 修复建议

区分错误类型：

```cpp
enum class PutError { FullNonScaling, CapacityOverflow, FpRateUnderflow, OOM };
```

对 scaling filter 的 FP underflow 要么继续使用最小 rate，要么给明确错误：`ERR cannot expand filter: false-positive rate underflow`。

---

## BUG-10：`BF.MADD` / `BF.INSERT` 在 non-scaling 满时会在固定长度数组内继续执行后续 item

**级别：P1**

### 证据

- `CmdMadd()` 先 `ReplyWithArray(ctx, count)`，循环中 `PutAndReply()` 遇到满会 `ReplyWithError()`，但外层继续下一个 item。
- `CmdInsert()` 同样先固定 array length，再循环所有 item。
- 相关位置：`src/bloom_commands.cc:186-208, 317-327, 83-92`。

### 问题

一旦 fixed filter 满，后续 item 不可能成功，但代码仍继续执行。结果可能是：

```text
[1, 1, ERR full, ERR full, ERR full]
```

而不是在第一个 full 处停止或返回清晰的 top-level/simple error。官方实现的 common insert loop 在 full 后停止继续处理。

### 影响

- 返回结构对客户端不友好。
- 已经插入的前缀 item 与错误响应混杂，调用方很难做补偿。
- 对 AOF/replication 来说，部分成功路径也更难推理。

### 修复建议

改为 postponed array length，遇到 fatal insert error 立即停止，并明确文档化“前缀成功，错误处停止”或实现 all-or-nothing。

---

## BUG-11：`BF.INFO` / `BF.CARD` 把 `uint64_t`/`size_t` 强转为 `long long`，超出范围会返回负数

**级别：P1**

### 证据

- `BF.INFO Capacity/Size/Filters/Items` 使用 `static_cast<long long>()`。
- `BF.CARD` 对 `TotalItems()` 强转 `long long`。
- 相关位置：`src/bloom_commands.cc:73-83, 88-102, 106-122`。

### 问题

Redis integer reply 是 signed 64-bit。当前内部容量可通过 expansion 接近或超过 `LLONG_MAX`，转换后会得到负数或实现定义行为。

### 影响

- `BF.INFO Capacity`/`Items`/`Size`/`BF.CARD` 可能返回负数。
- 客户端可能把结果解释成错误状态。

### 修复建议

限制所有可增长计数不超过 `LLONG_MAX`，或者返回错误：

```cpp
if (value > static_cast<uint64_t>(LLONG_MAX)) {
  return RedisModule_ReplyWithError(ctx, "ERR value exceeds RESP integer range");
}
```

---

## BUG-12：`SetLayer()` 对已构造 slot 再调用会泄漏旧 `BloomLayer`

**级别：P2**

### 证据

- `SetLayer(index, FilterLayer&&)` 直接 placement-new 到 `layers_[index]`。
- 没有判断该 slot 是否已经构造，也不会先析构旧对象。
- 相关位置：`src/sb_chain.cc:172-175`。

### 问题

当前正常 RDB/LOADCHUNK 路径每个 index 理论上只调用一次。但接口是 public，未来复用或错误路径重复调用会覆盖已构造对象，旧 bitArray 泄漏。

### 修复建议

把 `SetLayer()` 改为 private 并用 `constructedLayers_` 管理，或者要求调用方传入“slot 未构造”状态并 assert。

---

## BUG-13：`BF.INSERT ITEMS` 后无 item 返回错误不准确

**级别：P2**

### 证据

- `ParseInsertOptions()` 找到 `ITEMS` 后设置 `itemsStart = i + 1`。
- `CmdInsert()` 若 `opts.itemsStart >= argc`，返回 `ERR ITEMS keyword not found`。
- 相关位置：`src/bloom_commands.cc:261-287`。

### 问题

命令里有 `ITEMS`，只是缺少 item。错误消息说 `ITEMS keyword not found`，信息错误。官方路径对 `items_index == argc` 返回 wrong arity。

### 修复建议

改成：

```cpp
if (opts.itemsStart >= argc) return RedisModule_WrongArity(ctx);
```

或返回 `ERR ITEMS expects at least one item`。

---

## BUG-14：`BF.RESERVE` 没有上限 arity，重复 option 会被接受

**级别：P2/P1（协议兼容角度见 02）**

### 证据

- `CmdReserve()` 只检查 `argc < 4`，然后从 argv[4] 线性解析直到结束。
- 没有 `argc > 7` 检查，也不拒绝重复 `EXPANSION`、重复 `NONSCALING`。
- 相关位置：`src/bloom_commands.cc:96-139`。

### 问题

这些命令会被接受或以“最后一个 wins”的方式解析：

```text
BF.RESERVE k 0.01 100 EXPANSION 2 EXPANSION 3
BF.RESERVE k 0.01 100 NONSCALING NONSCALING
```

### 修复建议

固定 option 组合矩阵：每个 option 至多一次；总 argc 上限与 RedisBloom 对齐；重复参数返回错误。

---

## BUG-15：`BloomLayer::Create()` 对 `falsePositiveRate` 只依赖调用方校验，内部不自洽

**级别：P2**

### 证据

- `BloomLayer::Create()` 不检查 `falsePositiveRate > 0 && falsePositiveRate < 1`。
- 命令层做了校验，但 RDB/测试/未来调用可能绕过。
- 相关位置：`src/bloom_filter.cc:93-126`。

### 问题

核心类型的构造函数应自校验，否则未来新增调用点容易引入 NaN、Inf、负数、0 hash count 等状态。

### 修复建议

在 `BloomLayer::Create()` 第一行加入参数校验：

```cpp
if (cap == 0 || !std::isfinite(falsePositiveRate) ||
    falsePositiveRate <= 0.0 || falsePositiveRate >= 1.0) {
  return std::nullopt;
}
```

---

## BUG-16：`bloom_config.cc` 使用 `strncasecmp` 但未显式 include `<strings.h>`

**级别：P3/P2（平台相关）**

### 证据

- `bloom_config.cc` include `<climits>` 和 `<cstring>`。
- `strncasecmp` 是 POSIX `<strings.h>` 声明，不是 ISO C++ `<cstring>`。
- `bloom_commands.cc` 正确 include 了 `<strings.h>`。
- 相关位置：`src/bloom_config.cc:3-7`，`src/bloom_commands.cc:9-12`。

### 问题

在某些标准库/编译选项下会出现未声明函数编译错误。

### 修复建议

在 `bloom_config.cc` 中显式加入：

```cpp
#include <strings.h>
```

---

## BUG-17：`BF.LOADCHUNK` header 成功后对象处于“半恢复”可见状态

**级别：P2**

### 证据

- cursor 1 加载 header 后立即 `ModuleTypeSetValue()`。
- 各层 bit array 初始为 0，后续 cursor 才逐层 `memcpy()`。
- 相关位置：`src/bloom_commands.cc:195-215`，`src/bloom_rdb.cc:226-245`。

### 问题

如果客户端加载 header 后失败/中断，key 已经存在，`BF.CARD`/`BF.INFO` 使用 header item count，但 `BF.EXISTS` 基于全 0 bit array，语义自相矛盾。

### 修复建议

设计 staging 状态：header 后 key 标记为 loading，只有所有 chunks 完成后转为可读；或者要求 LOADCHUNK 全量事务化/临时 key，再 rename。

---

## BUG-18：AOF rewrite 失败时只记录 warning 并跳过 key，可能造成持久化数据丢失

**级别：P1/P2**

### 证据

- `AofRewriteBloom()` 分配 header buffer 失败时：`RedisModule_LogIOError(... "key omitted")` 后 `return`。
- 相关位置：`src/bloom_rdb.cc:266-287`。

### 问题

当 AOF rewrite 发生内存分配失败，当前 key 被省略。日志说明了问题，但持久化结果缺失该 key。

### 修复建议

- 尽量避免额外大分配。
- 若 Redis Module API 支持，应让 rewrite 失败，而不是静默省略。
- 加入 allocation failure 集成测试。

---

## BUG-19：`BytesUsed()` 对恶意 RDB/wire `dataSize` 可能发生 `size_t` 加法溢出

**级别：P2**

### 证据

- `BytesUsed()` 用 `transform_reduce` 加总 `sizeof(ScalingBloomFilter) + layerCapacity_ * sizeof(FilterLayer) + dataSize`。
- `dataSize` 来自 RDB/wire 元数据或 bit array size。
- 相关位置：`src/sb_chain.cc:143-149`，`src/bloom_rdb.cc:99-110`。

### 问题

若加载路径接受超大 `dataSize`，`BytesUsed()` 可能 wrap 到小数。

### 修复建议

所有 size 加法使用 checked add；加载时限制 `dataSize == expectedSize`。

---

## BUG-20：`OpenOrCreate()` 创建默认 filter 后，如果后续命令在同一批处理失败，状态已变更

**级别：P2**

### 证据

- `BF.MADD` 对空 key 先创建 filter，再逐项插入。
- 对 fixed filter 满的处理会产生错误元素，但不会回滚。
- 相关位置：`src/bloom_commands.cc:41-75, 186-208`。

### 问题

当前没有命令级事务语义。对普通 Redis 命令来说部分成功不一定是 bug，但这里的错误返回与状态变更混在一起，没有明确设计说明。

### 修复建议

文档化 partial success，或者在参数/容量预检查阶段先判断是否可能完整执行。
