# 06 - 实现细节问题

本文件关注工程实现细节、局部抽象、校验一致性和未来维护风险。

## IMPL-01：RDB 与 wire 校验逻辑重复且不一致

**级别：P1**

RDB 校验写在 `BloomLayer::ReadFrom()`，wire 校验写在 `ValidateLayerMeta()`。两者规则不一致，已经导致 `01_code_bugs.md#BUG-01`。

位置：

- `modules/gemini-bloom/src/bloom_rdb.cc:54-84`
- `modules/gemini-bloom/src/bloom_rdb.cc:196-206`

### 建议

建立一个 canonical validator：

```cpp
struct LayerStateView { ... };
ValidationResult ValidateLayerState(LayerStateView, BloomFlags, BloomLimits);
ValidationResult ValidateFilterState(FilterStateView, BloomLimits);
```

RDB、wire、测试构造器都调用它。

## IMPL-02：手动 C++ lifetime 已改善，但仍暴露高风险 public builder

**级别：P2**

当前 `FromRdbShell()` 已避免 v3 中“析构未构造 layer”的问题：

位置：`modules/gemini-bloom/src/sb_chain.cc:151-175`。

但 `SetLayer()` 仍是 public，且调用方必须保证 index 顺序、slot 未构造、layer ownership 正确。

### 建议

用专用 builder 封装：

```cpp
class ScalingBloomBuilder {
 public:
  Expected<void, Error> AddLayer(FilterLayer&&);
  Expected<ScalingBloomFilter*, Error> Finish();
};
```

## IMPL-03：`BloomFlags` 没有 known-mask 检查

**级别：P1/P2**

`FromUnderlying()` 接受任意 unsigned：

位置：`modules/gemini-bloom/src/bloom_filter.h:38-44`。

这会让 unknown flag 从 RDB/wire 进入 runtime，并在保存时继续传播。

### 建议

```cpp
std::optional<BloomFlags> ParseFlags(unsigned raw) {
  if ((raw & ~kSupportedFlags) != 0) return std::nullopt;
  return static_cast<BloomFlags>(raw);
}
```

## IMPL-04：`RawBits` flag 存在但语义未完成

**级别：P2**

`BloomFlags::RawBits` 在 enum 中存在，`BloomLayer::Create()` 也有分支：

位置：

- `modules/gemini-bloom/src/bloom_filter.h:16-22`
- `modules/gemini-bloom/src/bloom_filter.cc:103-107`

但命令层不创建 RawBits filter，RDB/wire 没有兼容测试。RawBits 分支设置 `hashCount_ = 0`，普通 membership 循环会把 zero-hash layer 当成“所有元素存在”。

### 建议

短期：加载时拒绝 RawBits。长期：如果要支持 RedisBloom `BLOOM_OPT_ENTS_IS_BITS`，按 upstream 语义重命名并补 exact vector。

## IMPL-05：packed struct raw wire format 缺少格式文档

**级别：P2/P3**

`WireLayerMeta` 和 `WireFilterHeader` 使用 `#pragma pack(push, 1)`，包含 native integer 和 double：

位置：`modules/gemini-bloom/src/sb_chain.h:84-106`。

RedisBloom 也使用 native packed-ish 格式，但 gemini 自研实现仍应写明：

- endian 假设
- IEEE-754 double 假设
- struct packing
- encver 与 header version 关系
- 字段顺序对应 RedisBloom 哪个版本/commit

## IMPL-06：hash 实现看起来对齐 RedisBloom，但缺少 official vectors 固化

**级别：P2**

当前代码使用：

- 32-bit seed `0x9747b28c`
- 64-bit seed `0xc6a4a7935bd1e995`
- `h2 = hash(data, seed=h1)`

位置：`modules/gemini-bloom/src/bloom_filter.cc:12-35`。

RedisBloom upstream `deps/bloom/bloom.c` 使用同样模式。现有测试只验证 deterministic，不验证官方 exact output。

### 建议

保存 official hash vectors：

```text
""
"a"
"hello"
"hello\0world"
10KB binary
```

校验 32-bit、64-bit 的 `h1/h2` exact 值。

## IMPL-07：`PutAndReply()` 混合 mutation、Redis reply 和控制流

**级别：P2**

位置：`modules/gemini-bloom/src/bloom_commands.cc:88-96`。

这个 helper 会：

- 调用 `filter->Put()`
- 直接写 Redis reply
- 用 bool 返回“是否新增”
- 把错误和 duplicate 都返回 `false`

这让调用方无法区分 duplicate、full、OOM、corrupt state，也难以实现 postponed array length。

### 建议

核心层返回结构化结果，命令层先收集 result，再统一 reply 和 replicate。

## IMPL-08：parser 未记录重复 option，行为由最后一次赋值决定

**级别：P2**

`CmdReserve()` 和 `ParseInsertOptions()` 只记录部分互斥状态，不记录重复 `ERROR`、`CAPACITY`、`EXPANSION`、`NOCREATE`。

位置：

- `modules/gemini-bloom/src/bloom_commands.cc:119-147`
- `modules/gemini-bloom/src/bloom_commands.cc:239-302`

### 建议

每个 option 维护 `seen` bitset，重复即错误，除非明确声明允许重复且最后一个生效。

## IMPL-09：command flags、key spec、ACL 不是集中声明

**级别：P2/P3**

命令注册只是一张局部数组：

位置：`modules/gemini-bloom/src/bloom_commands.cc:593-620`。

没有 command metadata 测试，也没有与 Redis docs 的映射表。

### 建议

建立 command spec table：

```cpp
struct CommandSpec {
  const char* name;
  RedisModuleCmdFunc handler;
  const char* moduleFlags;
  ExpectedAcl acl;
  ExpectedRespShape resp2;
  ExpectedRespShape resp3;
};
```

测试从同一张表生成 `COMMAND INFO` / ACL / arity cases。

## IMPL-10：AOF rewrite error path 需要明确是否会让 rewrite 失败

**级别：P2**

`AofRewriteBloom()` 如果 header allocation 失败，只调用 `RedisModule_LogIOError()` 然后 `return`：

位置：`modules/gemini-bloom/src/bloom_rdb.cc:274-280`。

需要确认 Redis 是否会因此终止当前 AOF rewrite。如果不会，rewritten AOF 会静默缺失该 key。

### 建议

写一个 failure-injection 测试确认语义。若 `LogIOError` 不会 abort，需要改成可让 rewrite 失败的 API 或避免动态分配大 header。

