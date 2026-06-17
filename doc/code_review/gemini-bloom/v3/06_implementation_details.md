# 06 — 代码实现细节问题

本文件关注 C++ 实现质量、边界校验、内存生命周期、API 封装和可维护性。会和 `01_code_bugs.md` 有交叉，但这里侧重实现机制。

## IMPL-01：手写 RAII + raw allocation + placement new，复杂度过高

**级别：P1/P2**

### 证据

- `BloomLayer` 手动持有 `uint8_t* bitArray_` 并在析构中 `RMFree()`。
- `ScalingBloomFilter` 手动持有 `FilterLayer* layers_`，手动移动、placement-new、析构。
- `FromRdbShell()` 对 calloc 内存延迟构造。
- 位置：`src/bloom_filter.cc:41-76`，`src/sb_chain.cc:27-88, 153-175`。

### 问题

当前已经出现需要非常细致管理 object lifetime 的场景。C++ 对“分配了字节但对象未构造”的要求很严格，错误路径极易 UB。

### 建议

使用带 Redis allocator 的容器封装：

```cpp
using ByteBuffer = RedisBuffer<uint8_t>;
using LayerVector = RedisVector<FilterLayer>;
```

或至少封装 `LayerStorage`，内部维护 `constructed_` 数量。

---

## IMPL-02：校验逻辑分散且不一致

**级别：P1**

### 证据

- 命令创建校验 rate/capacity/expansion。
- `BloomLayer::Create()` 只做部分数值溢出校验。
- RDB load 有一套弱校验。
- wire load 有 `ValidateLayerMeta()`，但规则仍不完整。
- official RedisBloom 有 `bloom_validate_integrity()` 和 `SB_ValidateIntegrity()` 两层。

### 问题

同一个状态可以通过某条路径创建、另一条路径拒绝，或者通过加载路径创建出命令路径永远不会创建的非法对象。

### 建议

建立唯一 validator：

```cpp
struct LayerState { ... };
ValidationResult ValidateLayerState(const LayerState&, ValidationMode mode);
ValidationResult ValidateFilterState(const FilterState&, ValidationMode mode);
```

所有 create/load/wire/rdb/aof 都调用它。

---

## IMPL-03：`BloomFlags` 接受未知 bit，未来行为不可控

**级别：P2**

`FromUnderlying(unsigned v)` 直接 `static_cast<BloomFlags>(v)`，没有 mask。RDB/wire 可以携带未知 flag。`HasFlag()` 只检查已知 bit，但未知 bit 会继续保存、AOF rewrite、INFO 间接传播。

### 建议

```cpp
constexpr unsigned kKnownBloomFlags =
  ToUnderlying(BloomFlags::NoRound) |
  ToUnderlying(BloomFlags::RawBits) |
  ToUnderlying(BloomFlags::Use64Bit) |
  ToUnderlying(BloomFlags::FixedSize);

if ((v & ~kKnownBloomFlags) != 0) reject;
```

---

## IMPL-04：bit addressing 没有断言/防御式边界检查

**级别：P1/P2**

`TestBit()`/`SetBit()` 直接通过 `ResolveBit()` 访问 `bitArray_[byteOff]`。只要上游 metadata 错，立刻越界。当前加载路径确实可能让 metadata 错，见 `01_code_bugs.md#BUG-01`。

### 建议

release 版本可依赖 validator，但 debug/ASAN 版本应加 assert：

```cpp
assert(bitIndex < totalBits_);
assert(byteOff < dataSize_);
```

对外部输入生成的对象，validator 必须保证这些 assert 永远成立。

---

## IMPL-05：`RawBits` 命名与 RedisBloom `BLOOM_OPT_ENTS_IS_BITS` 容易误导

**级别：P2**

`RawBits` 听起来像“capacity 就是 bit 数”，但 RedisBloom flag 2 语义是“entries 参数是 bit exponent n2”。当前实现既不符合名字的可用语义，也不符合 RedisBloom 语义。

### 建议

如果要保留：

```cpp
enum class BloomFlags {
  NoRound = 1,
  EntriesIsBitsExponent = 2,
  Force64 = 4,
  NoScaling = 8
};
```

直接使用 official 命名，减少误读。

---

## IMPL-06：`PutAndReply()` 把核心 mutation 和 Redis reply 耦合

**级别：P2**

`PutAndReply()` 同时调用 `filter->Put()`、写 Redis reply、返回 bool。这让命令层无法先收集结果、判断是否出错、再决定返回形状和 replicate 行为。也让 unit test 很难绕过 RedisModule API 测 parser/semantics。

### 建议

拆为：

```cpp
PutStatus PutOne(...);
void ReplyPutResult(...);
```

多 item 命令先执行策略，再统一回复。

---

## IMPL-07：`CmdInfo()` 字段匹配大小写兼容，但输出 label hardcode 分散

**级别：P3**

单字段接受 `"Capacity"` 等大小写变体，完整返回 hardcode `"Number of filters"` 等 label。若后续要支持 RESP3 map、客户端字段 alias、RedisBloom exact labels，当前写法易错。

### 建议

定义字段表：

```cpp
struct InfoField {
  const char* argName;
  const char* fullLabel;
  ValueGetter getter;
};
```

单字段和完整输出复用同一表。

---

## IMPL-08：`RedisModule_ReplyWithLongLong` 没有集中封装 checked conversion

**级别：P2**

多处 `static_cast<long long>` 分散。未来新增字段容易重复问题。

### 建议

```cpp
int ReplyUint64AsRespInteger(RedisModuleCtx* ctx, uint64_t v) {
  if (v > static_cast<uint64_t>(LLONG_MAX)) {
    return RedisModule_ReplyWithError(ctx, "ERR integer overflow");
  }
  return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(v));
}
```

---

## IMPL-09：`Hash32Policy`/`Hash64Policy` 静态策略可读性不错，但没有 golden vector 测试支撑

**级别：P2/P3**

单测只检查 deterministic，不检查 official hash 值。hash 是 wire/RDB 兼容根基。只测“同输入同输出”不能证明种子、端序、tail mixing 与 RedisBloom 一致。

### 建议

从 RedisBloom official 生成 hash vectors：`""`、`"a"`、`"hello"`、binary with `\0`、10KB item，校验 `h1/h2` exact。

---

## IMPL-10：`ProbePosition()` 用 `raw & mask` 优化，但没有验证 mask 只用于 power-of-two bit count

**级别：P1**

`UseBitMasking()` 只看 `log2Bits_`，不看 `totalBits_` 是否真是 `1 << log2Bits_`。这就是 BUG-01 的实现根因。

### 建议

用类型拆分：

```cpp
struct ModuloByBits { uint64_t bits; };
struct ModuloByMask { uint64_t bits; uint64_t mask; };
```

构造 `ModuloByMask` 时强校验 `bits == mask + 1`。

---

## IMPL-11：RDB reader 的 `Ok()` 风格容易漏检

**级别：P2**

之前已经通过测试修过“读取 itemCount 后未检查 `r.Ok()`”的问题。当前仍然需要人工记住每次读取后检查。错误状态是 reader 内部 sticky flag，但接口返回普通值。调用方忘记检查就会把错误路径默认值 0 当合法值。

### 建议

让 reader 返回 `std::optional<T>` 或 expected：

```cpp
Expected<uint64_t, RdbError> GetUint();
```

或集中读取完整 struct，内部处理所有错误。

---

## IMPL-12：AOF rewrite 依赖 `BF.LOADCHUNK` 的私有 cursor 协议

**级别：P1/P2**

AOF 输出是 public command stream。若命令名是 `BF.LOADCHUNK`，用户会自然假设它符合 RedisBloom LOADCHUNK 协议。但当前 AOF 使用的是 gemini 私有 header+whole-layer cursor。

### 建议

- 若保持私有协议，使用私有 command，例如 `GEMINI.BF.LOADCHUNK`。
- 若使用 `BF.LOADCHUNK`，必须实现 official 协议。

---

## IMPL-13：`SerializeHeader()`/`DeserializeHeader()` 使用 packed struct + double raw memory，缺少 endian/version 说明

**级别：P3**

RedisBloom official 也使用 packed struct raw memory。因此如果目标是兼容，这不是单独错误。但 gemini 需要在 `FORMAT.md` 中说明：host endian 假设、double IEEE-754 假设、struct packing 固定、encver 与 wire header version 的关系。

---

## IMPL-14：`AllocFilter()` 固定 flags，命令层和核心层能力不匹配

**级别：P2**

```cpp
auto flg = BloomFlags::Use64Bit | BloomFlags::NoRound;
if (fixed) flg = flg | BloomFlags::FixedSize;
```

核心层支持多 flag，命令层只创建一种。loader 又可能读其他 flag。能力矩阵没有在类型系统中体现。

### 建议

增加 `FilterOptions`：

```cpp
struct FilterOptions {
  HashWidth hashWidth;
  RoundingMode rounding;
  ScalingMode scaling;
};
```

加载时转换并验证；命令创建只允许 documented subset。

---

## IMPL-15：`MatchArg()` 使用 `strncasecmp`，参数字符串不是 null-terminated，当前写法正确但需保留测试

**级别：P3**

`strncasecmp(arg.data(), target.data(), arg.size())` 配合 size 相等检查是正确的，不依赖 null terminator。这里不是 bug。

### 建议

保留 binary/embedded null option 测试。例如 option 参数 `EXPANSION\0x` 应不匹配 `EXPANSION`。

---

## IMPL-16：`RdbWriter::PutBlob()` 接收 `uint64_t len`，Redis API 常用 `size_t`

**级别：P3**

`RedisModule_SaveStringBuffer()` 的长度参数通常是 `size_t`。当前传 `uint64_t` 可能在 32-bit 平台或不同签名下产生截断/告警。

### 建议

统一内部 length 类型。Redis module 面向 Redis 主流 64-bit 平台，但代码层仍应避免隐式窄化。

---

## IMPL-17：缺少 namespace，公共符号容易和其它模块/库冲突

**级别：P3**

全局符号包括 `MurmurHash2`、`MurmurHash64A`、`BloomType`、`g_bloomConfig`、`RdbLoadBloom`、`AofRewriteBloom`。

### 建议

C++ 代码放入：

```cpp
namespace gemini::bloom { ... }
```

只暴露 Redis module callback 必须的 C ABI 符号。

---

## IMPL-18：没有把 official RedisBloom source assumptions 固化成注释或 tests

**级别：P2**

源码注释写了“intended to match RedisBloom”，但没有说明具体参考版本、commit、字段对应表和不可变约束。

### 建议

在 `redisbloom_wire.h` 中维护字段对应注释：

```text
RedisBloom commit <sha>
dumpedChainHeader.size       -> WireFilterHeader.totalItems
dumpedChainHeader.nfilters   -> WireFilterHeader.numLayers
...
bloom.bytes                  -> WireLayerMeta.dataSize
bloom.bits                   -> WireLayerMeta.totalBits
```

并附 golden tests。
