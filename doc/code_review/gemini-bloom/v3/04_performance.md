# 04 — 性能问题

本文件关注时间复杂度、内存分配、Redis 单线程阻塞、持久化性能和可扩展性。部分问题与 RedisBloom official 算法相同，但 gemini-bloom 仍需要明确成本或加防护。

## PERF-01：SCANDUMP 一次返回整层 bit array，可能阻塞 Redis event loop

**级别：P1/P2**

### 证据

- `CmdScandump()` 对 cursor 1..N 返回整层 `layer.bloom.GetBitArray()` 和 `layer.bloom.GetDataSize()`。
- 没有 `MAX_SCANDUMP_SIZE` 或 chunk size 限制。
- RedisBloom official 使用 `MAX_SCANDUMP_SIZE = 16MB` 分块。
- 位置：`src/bloom_commands.cc:163-170`，RedisBloom `src/rebloom.c:151-188`。

### 影响

大 filter 的单层 bit array 可能达到几十 MB、几百 MB。Redis 模块命令在主线程执行，单次返回大 bulk string 会导致 latency spike、网络 buffer 压力、replica/AOF 传输压力和客户端 timeout。

### 建议

实现 RedisBloom byte-offset chunking，并限制每次 chunk 最大 16MB 或可配置值。

---

## PERF-02：LOADCHUNK 要求整层 chunk，导入大 filter 时峰值内存和网络压力高

**级别：P1/P2**

### 证据

`BF.LOADCHUNK` cursor > 1 时要求 `dataLen == layer.bloom.GetDataSize()`。位置：`src/bloom_commands.cc:223-231`。

### 影响

加载大 filter 时必须一次提交整层 bit array。相比官方分块协议，客户端内存峰值更高、Redis input buffer 更大、失败重试粒度更粗、大 key 恢复时阻塞更长。

### 建议

同 PERF-01，LOADCHUNK 支持任意 offset/length chunk。

---

## PERF-03：`BF.INFO` 标称 O(1)，当前 `TotalCapacity()` / `BytesUsed()` 是 O(numLayers)

**级别：P2**

### 证据

Redis docs 标 `BF.INFO` time complexity O(1)。当前 `TotalCapacity()` 遍历所有 layer；`BytesUsed()` 遍历所有 layer。位置：`src/sb_chain.cc:136-149`，`src/bloom_commands.cc:54-104`。

### 影响

当 expansion=1 或小 capacity 长时间增长时，numLayers 可很大。`BF.INFO` 会随层数线性增长。

### 建议

维护增量缓存：

```cpp
uint64_t totalCapacity_;
size_t totalBytes_;
```

AppendLayer/Load/SetLayer 时更新。`BF.INFO` 直接返回缓存。

---

## PERF-04：`BF.ADD`/`BF.INSERT` 在多层 filter 上每次都扫描所有层

**级别：P2**

### 证据

`IsDuplicate()` 从最新层到最旧层逐层 `bloom.Test()`；`Put()` 每个 item 都调用 `IsDuplicate()`。位置：`src/sb_chain.cc:96-129`。

### 影响

复杂度为：

```text
O(numLayers * hashCount)
```

当 `EXPANSION 1` 且初始 capacity 很小时，层数近似 `N / capacity`，插入成本会线性恶化。

### 建议

- 限制或警告 `EXPANSION 1`。
- 默认 expansion 至少 2。
- 文档写明多层成本。
- 对极多层场景考虑 periodic merge/rebuild 或分层索引，但这会改变 Bloom 误判特性，需要谨慎。

---

## PERF-05：`BloomLayer::Insert()` 对新 bit 做了两次地址解析/两次内存访问

**级别：P3/P2**

### 证据

当前逻辑：

```cpp
if (!TestBit(pos)) {
  SetBit(pos);
  anyNew = true;
}
```

`TestBit()` 和 `SetBit()` 都会 `ResolveBit(pos)`，并访问同一 byte。

### 影响

每个 probe 多一次函数调用和地址计算。Bloom filter 热路径通常非常频繁。

### 建议

合并为 test-and-set：

```cpp
auto [byteOff, mask] = ResolveBit(pos);
uint8_t old = bitArray_[byteOff];
if ((old & mask) == 0) {
  bitArray_[byteOff] = old | mask;
  anyNew = true;
}
```

RedisBloom official 的 `test_bit_set_bit()` 就是这个形态。

---

## PERF-06：Hash 计算对每个 item 扫描输入两次

**级别：P3**

Hash32/Hash64 都先算 `h1`，再以 `h1` 为 seed 计算 `h2`。位置：`src/bloom_filter.cc:25-37`。

对大 item，hash 成本约为两次线性扫描。RedisBloom official 也使用同样策略，因此这是兼容性成本，不是单独错误。

### 建议

如果保持 RedisBloom 兼容，不能随意改 hash。可以优化周边开销；在 native mode 可选择更快 hash pair 方案，但不能与 RedisBloom 混用。

---

## PERF-07：`AppendLayer()` 扩容 layer array 时移动所有 layer，层数多时有抖动

**级别：P3/P2**

### 证据

`layerCapacity_` 不足时分配新数组，逐个 placement-new move，析构旧 layer。初始 capacity 为 4，倍增。位置：`src/sb_chain.cc:66-88`。

### 影响

单次扩容 O(numLayers)。虽然摊销合理，但 `EXPANSION 1` 导致层数很多时仍会出现延迟尖峰。

### 建议

- 使用 `std::vector<FilterLayer, RedisAllocator>` 简化并依赖成熟容器。
- 或在 `EXPANSION 1` + 小 capacity 时预估层数。
- 也可改为链表/分块数组，避免移动已有 layer，但会牺牲遍历局部性。

---

## PERF-08：`BytesUsed()` 统计 layerCapacity 而非 numLayers，可能高估但符合已分配内存；需要文档化

**级别：P3**

`BytesUsed()` 计算 `sizeof(ScalingBloomFilter) + layerCapacity_ * sizeof(FilterLayer)`。位置：`src/sb_chain.cc:143-149`。

这是 payload 已分配内存，不是逻辑层数内存。对用户来说 `BF.INFO Size` 可能看起来在某些层数扩容后突然跳变。

### 建议

保留该统计也可以，但文档必须说明：Size 包括预留 layer slot。若要对齐 RedisBloom，需要测量官方 `BF.INFO Size` 规则。

---

## PERF-09：RDB/LOADCHUNK 加载路径缺少 early cap，可能先分配再失败

**级别：P2**

`DeserializeHeader()` 校验不足，允许 `dataSize > expectedSize`；`FromWireMeta()` 直接按 `dataSize` 分配。位置：`src/bloom_rdb.cc:192-245`。

### 影响

恶意 header 可迫使大分配；即使后续 chunk 不提供，也已经消耗内存。

### 建议

在分配前做所有 cap 和 exact-size 校验，并使用 TryAlloc 风格接口。

---

## PERF-10：TCL 集成测试使用固定 sleep 等待 BGSAVE/BGREWRITEAOF，CI 下会慢且不稳定

**级别：P3**

### 证据

- RDB 测试 `BGSAVE` 后 `after 2000`。
- AOF rewrite 后 `after 2000`。
- 位置：`tests/tcl/bloom_test.tcl:744-782, 806-874`。

### 影响

快机器浪费时间；慢机器或高负载 CI 可能还没完成就重启，导致 flaky。

### 建议

轮询 `INFO persistence`：`rdb_bgsave_in_progress=0`、`aof_rewrite_in_progress=0`，并检查上次状态为 `ok`。

---

## PERF-11：没有 benchmark，无法量化核心路径退化

**级别：P2**

当前没有基准覆盖单层 add/exists、多层 add/exists、expansion=1 极端层数、SCANDUMP/LOADCHUNK 大 filter、RDB/AOF rewrite、false positive rate 与 bits/hash 参数关系。

### 建议

新增 benchmark targets：

```text
bench_bloom_add
bench_bloom_exists
bench_scaling_layers
bench_scandump_large
bench_rdb_roundtrip_large
```

输出至少包括 p50/p95/p99 latency、RSS、bytes per entry。
