# 03 - 持久化与安全缺口

RDB、DUMP/RESTORE、AOF、SCANDUMP/LOADCHUNK、replication 都是高风险入口。它们可能来自备份、迁移工具、外部服务或不可信客户端。

## SAFE-01：RDB / wire 仍接受 `bitsPerEntry == 0`

**级别：P1**

当前 validator：

```cpp
if (!std::isfinite(f.bitsPerEntry) || f.bitsPerEntry < 0.0) return false;
```

位置：`modules/gemini-bloom/src/bloom_rdb.cc:52-63`。

这会接受 `bitsPerEntry == 0`。本轮失败用例：

- `BloomRdb.RejectsBitsPerEntryZero`
- `BloomWire.RejectsBitsPerEntryZero`

建议改为：

```cpp
if (!std::isfinite(f.bitsPerEntry) || f.bitsPerEntry <= 0.0) return false;
```

并保留现有失败测试作为回归。

## SAFE-02：RDB / wire 不校验 `hashCount == ceil(ln2 * bitsPerEntry)`

**级别：P1**

当前 `ValidateLayerFields()` 没有检查 `hashCount` 与 `bitsPerEntry` 的一致性。

本轮失败用例：

- `BloomRdb.RejectsHashCountInconsistentWithBitsPerEntry`
- `BloomWire.RejectsHashCountInconsistentWithBitsPerEntry`

影响：

- 恶意或损坏 RDB 可以构造与元数据不一致的 filter。
- `BF.INFO` 暴露的 metadata 与实际 membership 行为不一致。
- 后续 RDB/AOF rewrite 会固化非法状态。

建议：

```cpp
uint32_t expected = std::max(1u,
  static_cast<uint32_t>(std::ceil(std::numbers::ln2 * f.bitsPerEntry)));
if (f.hashCount != expected) return false;
```

如果为了兼容历史 RedisBloom 数据不能强校验，应通过版本分支和 golden corpus 证明例外，而不是无条件接受。

## SAFE-03：计数求和和 data size 求和缺少 overflow 防御

**级别：P1/P2**

当前代码有：

```cpp
itemSum += count;
totalDataSize += meta[i].dataSize;
```

位置：

- `modules/gemini-bloom/src/bloom_rdb.cc:183-210`
- `modules/gemini-bloom/src/bloom_rdb.cc:264-274`

虽然已有 `numLayers` 和 `totalDataSize > 4GB` 上限，但加法本身没有 overflow guard。恶意 metadata 可以尝试绕过 sum check 或资源上限。

建议：

- `if (itemSum > UINT64_MAX - count) return nullptr;`
- `if (totalDataSize > kMaxTotalDataSize - meta[i].dataSize) return nullptr;`
- RDB path 也引入与 wire path 一致的总 data size 上限。

## SAFE-04：`LOADCHUNK` header 后半恢复对象对客户端可见

**级别：P1/P2**

当前 `LOADCHUNK cursor=1` 反序列化 header 后立即把 key 设置为 Bloom 对象，layer bit arrays 还是全 0。现有测试已固定行为：

```text
Half-restored filter returns 0 for EXISTS
BF.CARD still shows header-declared count
```

位置：`modules/gemini-bloom/tests/tcl/bloom_test.tcl:1410-1431`。

风险：

- 客户端在 restore 中途读到 false negative。
- `BF.CARD` 与实际 bit array 状态暂时不一致。
- 中途失败后 key 会长期停留在半恢复状态。

本轮 Redis 6.2.17 + RedisBloom v2.8.20 兼容性矩阵把该风险从理论变成目标内失败：

```text
RedisBloom v2.8.20 SCANDUMP -> gemini LOADCHUNK:
  first header chunk: OK
  data chunks: ERR cursor exceeds layer count
  BF.CARD: 40
  inserted item found: 0/40

gemini SCANDUMP -> RedisBloom v2.8.20 LOADCHUNK:
  first header chunk: OK
  data chunks: ERR received bad data
  BF.CARD: 40
  inserted item found: 0/40
```

command-AOF rewrite 关闭 RDB preamble 后也复现同一类半恢复状态；完整矩阵中 command-AOF 18/18 单元失败：

```text
gemini command-AOF -> RedisBloom v2.8.20: non-empty corpora found=0/N
RedisBloom v2.8.20 command-AOF -> gemini: non-empty corpora found=0/N
```

建议：

- 若追求强正确性，引入 staging 状态，全部 chunk 加载完成后再 publish。
- 若保持 RedisBloom 类似行为，至少文档化 `LOADCHUNK` 不是事务性 restore，并禁止业务读写半恢复 key。

## SAFE-05：资源失败注入不足

**级别：P2**

缺少对以下路径的自动验证：

- `RMAlloc` / `RMCalloc` 第 N 次失败。
- `RedisModule_ModuleTypeSetValue` 失败。
- `RedisModule_LoadStringBuffer` partial stream / allocation failure。
- `DeserializeHeader` 第 N 层创建失败。
- `AofRewriteBloom` header allocation failure。

建议在 mock allocator 中加入 failure injection，例如：

```cpp
SetAllocFailAfter(n);
```

并在 ASAN/UBSAN 下跑 RDB/wire/fuzz 测试。

## SAFE-06：极端 capacity/error 参数仍可触发巨大分配

**级别：P1/P2**

`BloomLayer::Create()` 只检查 `rawBits = capacity * bitsPerEntry` 是否超过 `UINT64_MAX - 7`，但没有统一的 per-layer / total allocation 上限。某些组合没有算术 overflow，却会尝试不可接受的大分配。

位置：`modules/gemini-bloom/src/bloom_filter.cc:85-127`。

本轮容器内手工编译运行 `sb_chain_test` 时，`ScalingBloomTest.ExtremeParamsRejected` 触发 Rosetta 级别 abort：

```text
[ RUN      ] ScalingBloomTest.ExtremeParamsRejected
rosetta error: could not find free space for allocation size efa05fa53e2000
```

过滤该用例后其余 15 个 `sb_chain_test` 用例通过。该失败说明：

- 极端参数测试本身不能稳定作为普通 CI 用例运行。
- 当前实现缺少先于 allocator 的明确 data size 上限。
- Redis 命令层已有 `kMaxCapacity = 1<<30`，但底层 core/RDB/wire/test helper 仍需要同一个 resource limit validator，避免外部 RDB/wire 或未来调用绕过命令层。

建议：

- 在 `BloomLayer::Create()` 中加入 max data size / max total bits 上限，且错误发生在 `RMCalloc` 前。
- RDB/wire validator 与命令 parser 复用同一资源上限。
- 把 `ExtremeParamsRejected` 改成不会要求真实巨大分配的 deterministic unit test。

## SAFE-07：`BF.LOADCHUNK cursor=1` 会覆盖已有 Bloom key，行为不同于 RedisBloom

**级别：P1/P2**

当前 `CmdLoadchunk()` 在 `cursor == 1` 时，如果 key 已经是 Bloom module key，会先 `RedisModule_DeleteKey(key)`，再把 header 反序列化后的 filter 设置到同名 key：

```text
if (keyType != REDISMODULE_KEYTYPE_EMPTY) {
  RedisModule_DeleteKey(key);
  key = RedisModule_OpenKey(...);
}
RedisModule_ModuleTypeSetValue(key, BloomType, filter)
```

位置：`modules/gemini-bloom/src/bloom_commands.cc:590-607`。

补充审计构造了同一实例内的 `src` / `dst` 两个 Bloom key。`dst` 原本包含 `old`，`src` 包含 `new`，然后对 `dst` 执行 `BF.LOADCHUNK dst 1 <src-header>`：

```text
before:
  dst old=1, new=0, BF.CARD=1

gemini:
  LOADCHUNK dst 1 <header> -> OK
  after header: old=0, new=0, BF.CARD=1

RedisBloom v2.8.20:
  LOADCHUNK dst 1 <header> -> ERR received bad data
  after header: old=1, new=0, BF.CARD=1
```

影响：

- 迁移工具如果误把 header chunk 写到已有 key，gemini 会直接删除旧内容；RedisBloom 在 header 阶段拒绝，不会删除旧内容。
- header 成功后 gemini 仍发布半恢复对象，直到 data chunks 全部加载前都会出现 `BF.CARD` 与 membership 不一致。
- 该行为放大了 `SCANDUMP/LOADCHUNK` 协议不兼容的风险：错误 chunk 序列不只是失败，还可能覆盖已有业务 key。

建议：

- 若追求 RedisBloom 兼容，`cursor=1` 只允许 empty key；对已有 Bloom key 返回 RedisBloom-compatible error。
- 如果需要 replace 语义，应要求显式私有命令或显式 option，不能复用 public `BF.LOADCHUNK key 1` 静默替换。
- staging restore 仍是更强方案：所有 chunk 校验完成后再 publish。
