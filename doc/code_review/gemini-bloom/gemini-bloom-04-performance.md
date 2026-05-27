# Codex Review: gemini-bloom 性能问题审计

## 1. 多层查询顺序与 RedisBloom 相反，热点命中路径更慢

位置：`modules/gemini-bloom/src/sb_chain.cc:86-90`

当前 `IsDuplicate` 使用 `std::ranges::any_of(Layers())`，按旧 layer 到新 layer 顺序查询。RedisBloom 从最新 layer 向旧
layer 查询，因为新写入的数据通常集中在最新 layer。

影响：

- 扩容后，近期数据命中需要先扫过所有旧 layer。
- `BF.EXISTS`、`BF.ADD` 的 duplicate pre-check 都受影响。

建议：

- 改成 reverse iteration：从 `numLayers_ - 1` 到 0。
- 增加多层场景 benchmark：旧数据命中、新数据命中、miss 三类。

## 2. `SCANDUMP` 和 AOF rewrite 没有 16MB chunk 上限

位置：

- `modules/gemini-bloom/src/bloom_commands.cc:457-464`
- `modules/gemini-bloom/src/bloom_rdb.cc:231-236`

RedisBloom 使用 `MAX_SCANDUMP_SIZE = 16MB` 分块。当前实现每个 layer 一次性返回完整 bit array。

影响：

- 大 filter 的单条 reply/AOF bulk string 可能非常大。
- Redis event loop 被长时间阻塞。
- 客户端、代理、复制链路更容易遇到大 bulk string 问题。

建议：

- 按 byte offset cursor 分块，最大 chunk 16MB。
- AOF rewrite 复用同一分块逻辑。

## 3. 小容量 filter 最小 1024 bit，内存放大严重

位置：`modules/gemini-bloom/src/bloom_filter.cc:103-106`

对 capacity 很小的 key，理论 bit 数可能几十到几百，当前至少 1024 bit。

影响：

- 大量小 filter 的内存消耗远高于 RedisBloom。
- 用户通过 `CAPACITY` 控制资源的直觉被破坏。

建议：

- 删除固定下限，或改成配置项。
- 用 benchmark 覆盖 capacity=1、10、100、1000 的内存和误判率。

## 4. bit array 没有按 RedisBloom 的 8 字节对齐，兼容和访问模式都不稳定

位置：`modules/gemini-bloom/src/bloom_filter.cc:115`

RedisBloom 的 bit array byte size 会按 64-bit 对齐。当前只按 byte 对齐。

影响：

- 对齐后的 bits 不是同一个 modulo 空间，影响兼容。
- 大 filter 的访问没有直接利用 64-bit 对齐优势。

建议：

- 如果目标兼容 RedisBloom，按 RedisBloom `bytes`/`bits` 计算方式实现。

## 5. `BytesUsed()` 低估实际分配，影响内存调优

位置：`modules/gemini-bloom/src/sb_chain.cc:131-137`

`layers_` 实际按 `layerCapacity_` 扩容，但 `BytesUsed()` 只算 `numLayers_`。

影响：

- `BF.INFO Size` 低估。
- Redis memory usage 回调低估，可能影响 eviction 和运维判断。

建议：

- 统计 `layerCapacity_ * sizeof(FilterLayer)`。
- 或改成 RedisBloom 一样每增加一层精确 realloc，不保留多余 capacity。

## 6. `RMRealloc` 搬移非平凡对象既是正确性问题，也是性能隐患

位置：`modules/gemini-bloom/src/sb_chain.cc:62-77`

用 raw realloc 管理 `FilterLayer` 阵列避免了逐个 move，但这是 C++ UB。未来修复时如果改成逐个 move，需要关注 layer 增长成本。

建议：

- 用 vector-like RAII 容器，增长时显式 move。
- 由于 layer 数通常很小，正确性优先于微优化。

## 7. 扩容失败阈值 `kMinFpRate = 1e-15` 可能提前让 scaling filter 变满

位置：`modules/gemini-bloom/src/sb_chain.cc:100-102`

当 layer 多到下一层 fp rate 小于 `1e-15`，当前实现停止扩容并返回 full。RedisBloom 主要依赖 bloom_init 和内存分配失败返回错误。

影响：

- `EXPANSION 1`、小初始容量、长时间增长时，当前实现可能比 RedisBloom 更早拒绝写入。
- 这是隐藏容量上限，`BF.INFO` 也没有暴露。

建议：

- 明确该阈值是否是产品要求。
- 若追求兼容，移除固定阈值，改为按参数可表示性和内存分配失败处理。

## 8. INFO 路径每次线性扫描 layer

位置：

- `modules/gemini-bloom/src/sb_chain.cc:124-129`
- `modules/gemini-bloom/src/sb_chain.cc:131-137`

`TotalCapacity()` 和 `BytesUsed()` 每次都扫描所有 layer。正常 layer 数少时问题不大，但 `EXPANSION 1` 会制造很多 layer。

建议：

- 维护 cached total capacity 和 bytes used。
- 或限制/告警过多 layer。

## 9. 没有性能基准

当前测试只验证功能，没有 benchmark。

建议至少增加：

- `BF.ADD` 单 key 单 layer 吞吐。
- `BF.EXISTS` miss/hit 吞吐。
- 多层 filter 中新 layer hit、旧 layer hit、miss 的对比。
- `EXPANSION 1` vs `EXPANSION 2/4`。
- 大 filter `SCANDUMP/LOADCHUNK` 分块耗时和最大 reply size。
