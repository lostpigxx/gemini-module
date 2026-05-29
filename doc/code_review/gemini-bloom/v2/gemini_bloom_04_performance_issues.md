> 分析基准：GitHub 仓库 `lostpigxx/gemini-module` 的 `main` 分支当前可读取源码；重点目录为 `modules/gemini-bloom`。  
> 方法：静态源码审查 + 与当前 RedisBloom 官方源码/命令文档做行为对照。未在本地编译运行；容器内无法直接 `git clone`，因此结论基于 GitHub API 拉取到的文件内容。  
> 严重性：P0=可能崩溃/数据损坏/安全风险；P1=兼容性或核心语义错误；P2=明显设计/性能/可维护性缺陷；P3=低风险但应修正。

# 04. 性能问题

## 总览

本文件关注时间复杂度、内存放大、分配策略、持久化/迁移吞吐和高层数退化。部分问题与 RedisBloom 兼容性相关，但这里按性能影响展开。

| ID | 严重性 | 位置 | 性能问题 |
|---|---:|---|---|
| PERF-01 | P1 | `src/bloom_commands.cc:20-23`, `src/bloom_filter.cc:147-170` | 命令创建路径强制 `NoRound`，导致每个 probe 都走 `% totalBits` |
| PERF-02 | P1 | `src/sb_chain.cc:90-96, 126-128` | 查询/插入需要扫描所有层，层数多时线性退化 |
| PERF-03 | P1 | `EXPANSION=1` | 容量增长不增长，层数增长最快，性能最差 |
| PERF-04 | P2 | `BF.INFO`, `mem_usage` | `TotalCapacity()`/`BytesUsed()` 每次 O(numLayers)，在极端层数下与 O(1) 预期存在张力 |
| PERF-05 | P1 | `BF.SCANDUMP`, `AOF rewrite` | 整层 chunk 可能产生超大 bulk string / 超大 AOF 命令 |
| PERF-06 | P1 | `BF.LOADCHUNK` | header 阶段一次性分配并清零所有层 bit arrays |
| PERF-07 | P2 | `src/sb_chain.cc:66-82` | 层数组扩容需要搬迁整个数组，且当前用 `realloc` 有 UB |
| PERF-08 | P2 | `src/bloom_filter.cc:158-170` | 插入未命中 bit 时先 TestBit 再 SetBit，重复计算地址 |
| PERF-09 | P2 | `src/bloom_filter.cc:25-36` | 每个 item 做两次 MurmurHash64A，短 key 场景 hash 成本占比高 |
| PERF-10 | P2 | RDB/AOF/SCANDUMP | 没有 chunk size/backpressure 策略 |
| PERF-11 | P2 | 内存布局 | 每层 bit array 独立分配，层多时 allocator overhead 和碎片增加 |
| PERF-12 | P3 | tests | 统计型 FPR 单测固定跑 10k insert + 100k query，反馈慢且非 benchmark |

## 详细问题

### PERF-01：`NoRound` 让 hot path 走取模

`AllocFilter()` 固定使用：

```cpp
auto flg = BloomFlags::Use64Bit | BloomFlags::NoRound;
```

`BloomLayer::Create()` 在 `NoRound` 下不会把 `totalBits_` round 到 power-of-two，也不会设置 `log2Bits_`。于是查询/插入：

```cpp
bool isPow2 = IsPowerOfTwo();  // false
uint64_t pos = raw % totalBits;
```

每个 item 要做 `hashCount` 次 64-bit modulo。Modulo 通常比 bitmask 慢很多。

这是 RedisBloom 兼容性和性能之间的取舍：RedisBloom 也使用 NOROUND，所以如果目标是 bit-level 兼容，不能直接改。但至少应意识到：

- `BloomFlags::NoRound` 当前让代码里的 `mask` 优化几乎只服务于非命令/测试路径。
- 如果 Gemini 允许非兼容模式，可提供 `ROUND`/`FAST` 模式，用 power-of-two bit array 换取更快查询。

### PERF-02：所有查询都要反向扫描全部层

`IsDuplicate()`：

```cpp
for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
  if (it->bloom.Test(hp)) return true;
}
```

`Contains()` 和 `Put()` 都走这条路径。复杂度：

```text
O(numLayers * hashCount)
```

当 `EXPANSION=1`、初始 capacity 很小或长期写入后，`numLayers` 会增长很多。Bloom filter 的查询本来应接近 O(k)，多层 scaling 后退化明显。

优化方向：

- 缓存总容量与层上限，限制最大层数。
- 对 expansion=1 给出强警告或默认禁止。
- 使用更大的默认 initial capacity。
- 对层数过多时提供 rebuild/compact 机制。

### PERF-03：`EXPANSION=1` 是最坏扩容参数

`GrowIfNeeded()`：

```cpp
uint64_t nextCap = prevCap * expansionFactor_;
```

当 `expansionFactor_ == 1`，每层 capacity 永远相同。写入 N 个元素时层数近似 `N / initialCapacity`，查询复杂度线性增长。

RedisBloom 文档允许 expansion=1，因为它节省内存；但 Gemini 没有：

- 最大层数限制。
- `BF.INFO` 饱和度告警。
- 文档说明。
- benchmark 证明可接受范围。

建议：

- 保持兼容时允许 expansion=1，但设置 `max_layers`。
- 在 `BF.RESERVE`/`BF.INSERT` 文档里写清楚性能后果。
- 添加性能测试：expansion=1/2/4 在百万级数据下的 QPS 和 p99。

### PERF-04：`BF.INFO` 和 `mem_usage` 是 O(numLayers)

`TotalCapacity()`：

```cpp
std::transform_reduce(layerSpan.begin(), layerSpan.end(), ...)
```

`BytesUsed()` 也遍历所有层。复核结论：原报告列为 P1 过重。Redis 官方文档把 `BF.INFO` 标为 O(1)，但 RedisBloom 实现也存在按层汇总信息的路径；在正常 expansion 设置下层数通常较小，这不是 hot path 上的立即阻断问题。真正的风险集中在 `EXPANSION=1`、初始容量很小或畸形/长期写入导致层数异常偏高的对象上。

当前影响：

- 高层数 filter 上频繁调用 `BF.INFO` 会线性变慢。
- Redis `MEMORY USAGE` 或 module mem_usage callback 也可能变慢。
- 层数越多，监控命令越重。

优化：

- 在 `ScalingBloomFilter` 中维护 `totalCapacity_` 和 `bytesUsed_`。
- AppendLayer / LoadChunk / RDB load 时更新缓存。
- Debug-only 校验缓存与遍历结果一致。

### PERF-05：SCANDUMP/AOF 使用整层 bit array 作为单个 chunk

Gemini：

```cpp
ReplyWithStringBuffer(... layer.bloom.GetBitArray(), layer.bloom.GetDataSize());
```

AOF rewrite 也按层写：

```cpp
RedisModule_EmitAOF(... layer.bloom.GetBitArray(), layer.bloom.GetDataSize());
```

问题：

- 单层可能很大，返回/写入一个巨大 bulk string。
- 网络缓冲、client output buffer、AOF rewrite buffer 压力大。
- 与 RedisBloom 16MB 分块策略不一致。

优化：

- 实现 byte-offset chunking。
- 每个 chunk 限制最大 16MB 或更小可配置值。
- AOF rewrite 使用同一 chunk iterator。

### PERF-06：`LOADCHUNK` header 阶段预分配所有层 bit array

`DeserializeHeader()` 对每层执行：

```cpp
auto layer = BloomLayer::FromWireMeta(meta[i], filterFlags);
layer.bitArray_ = RMCalloc(layer.dataSize_, 1);
```

header 一加载就为所有层分配并清零 bit array，即使后续 chunk 永远不到。

影响：

- 大 dump restore 的内存峰值提前出现。
- 攻击者只发 header 就能触发大量 zero-fill。
- 如果最终 chunk 失败，内存已经消耗。

优化：

- 分层 lazy allocate：收到对应 chunk 时再分配。
- 或 import session 中先验证总大小和 quota，再分配。
- 对 header 总 bytes 设置上限。

### PERF-07：层数组扩容搬迁成本

当前 `AppendLayer()` 每次 `layerCapacity_` 满了就翻倍，并搬迁整个 `FilterLayer` 数组。层数很大时，扩容总成本摊销为 O(numLayers)，但单次扩容仍有尖峰。

更重要的是，当前 `realloc` 对非平凡对象是 UB。修复为安全 move 后，单次扩容会显式 move 多个对象，也要计入性能。

优化方向：

- 如果支持高层数，考虑链表/分段数组。
- 预估层数并 reserve。
- 默认限制最大层数。

### PERF-08：Insert 对 unset bit 重复计算地址

当前：

```cpp
if (!TestBit(pos)) {
  SetBit(pos);
  anyNew = true;
}
```

`TestBit()` 和 `SetBit()` 都调用 `ResolveBit(pos)`。可合并为：

```cpp
auto [byteOff, mask] = ResolveBit(pos);
uint8_t old = bitArray_[byteOff];
bitArray_[byteOff] = old | mask;
anyNew |= ((old & mask) == 0);
```

这减少函数调用、地址计算和内存访问。hot path 上每个 item 有 `k` 次 probe，收益可观。

### PERF-09：短 key 场景 hash 成本占比较高

`Hash64Policy::Compute()` 对每个 item 调两次 `MurmurHash64A`：

```cpp
uint64_t h1 = MurmurHash64A(ptr, len, seed);
return {h1, MurmurHash64A(ptr, len, h1)};
```

这是 RedisBloom/经典 double hashing 的常规做法，不是错误。但短 key、高 QPS 场景下 hash 成本会成为主要 CPU 消耗。

可选优化：

- 保持兼容模式不变。
- 非兼容模式可考虑更快 hash，如 XXH3，前提是格式/结果不要求 RedisBloom bit-compatible。
- benchmark 后决定。

### PERF-10：缺少 backpressure/chunk 策略

`SCANDUMP`、`LOADCHUNK`、AOF rewrite 都没有统一 chunk 上限和 backpressure 设计。大对象迁移时会造成：

- 单次命令耗时过长。
- client output buffer 增大。
- AOF rewrite 大命令。
- replica apply 大命令。

建议所有二进制导出/导入路径共用：

```text
max_chunk_bytes
max_total_bytes
max_layers
```

### PERF-11：多层多分配造成 allocator overhead 和碎片

每层有一个独立 bit array。扩容频繁、层数多时：

- allocator metadata overhead 增加。
- 内存碎片上升。
- active defrag 没有 callback，无法像 RedisBloom 一样帮助整理。

建议：

- 实现 defrag callback。
- 对小层合并分配或分段池化。
- 限制层数或支持 compaction。

### PERF-12：FPR 单测不是 benchmark，却承担了性能成本

`bloom_filter_test.cc` 固定执行：

- 10,000 次插入
- 10,000 次 no false negative 验证
- 100,000 次 false positive 统计

这对单元测试偏重。它只能粗略验证误判率，没有输出 QPS、延迟、CPU profile。

建议：

- 单测保留小规模 deterministic golden。
- 单独增加 benchmark target，例如 Google Benchmark。
- FPR 统计测试放到 nightly 或性能套件。
