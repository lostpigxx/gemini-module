# Codex Review: gemini-bloom 代码 Bug 审计

审计对象：`modules/gemini-bloom`。基线包括当前源码、Tcl 集成测试、gcov 结果，以及 RedisBloom 参考实现。

## 高风险问题

### 1. `BF.SCANDUMP` 返回的 cursor 语义错误，会让标准恢复流程丢失 bit array

位置：`modules/gemini-bloom/src/bloom_commands.cc:453-468`

当前实现：

- `BF.SCANDUMP key 0` 返回 `[1, header]`。
- `BF.SCANDUMP key 1` 在最后一个 layer 时返回 `[0, bitarray]`。

RedisBloom/Redis 文档语义是：返回 `(iter, data)`；`iter == 0` 表示迭代结束，标准客户端通常不会把 `(0, data)` 交给
`BF.LOADCHUNK`。RedisBloom 参考实现的 `SBChain_GetEncodedChunk` 会先返回数据 chunk 对应的非零 offset cursor，下一次
再返回 `(0, "")`。

当前 Tcl 测试用自增的 `load_iter` 绕过了这个协议问题：

- `modules/gemini-bloom/tests/tcl/bloom_test.tcl:460-468`

实际风险：

- 按官方文档写的备份/恢复客户端只会加载 header，不会加载 bit array。
- 恢复后的 filter 可能 `BF.CARD` 显示有元素，但 `BF.EXISTS` 对原始元素返回 0，产生 false negative。
- AOF rewrite 也采用自定义 cursor 序列，跨 RedisBloom/Redis 8 恢复会失败。

建议：

- 实现 RedisBloom 的 offset cursor 协议，而不是 layer index 协议。
- `SCANDUMP` 返回数据 chunk 时必须返回该 chunk 的 LOADCHUNK iterator；只有额外一次 scan 结束时返回 `(0, "")`。
- 测试必须使用 `SCANDUMP` 返回的 iterator 原样调用 `LOADCHUNK`。

### 2. `BF.LOADCHUNK` 会先删除已有 key，再验证 header，损坏输入会造成数据丢失

位置：`modules/gemini-bloom/src/bloom_commands.cc:488-498`

`cursor == 1` 时，如果 key 已存在，当前代码先 `RedisModule_DeleteKey(key)`，然后才 `DeserializeHeader(data, dataLen)`。
如果 header 是坏的，命令返回错误，但原 key 已经被删除。

实际风险：

- 用户对已有 Bloom filter 或普通 Redis key 执行一个 malformed `BF.LOADCHUNK key 1 <bad>`，会删除原数据。
- 这是典型的错误路径破坏数据问题。

建议：

- 先完整解析和验证 header，成功后再替换 key。
- 对已有非 Bloom 类型 key 返回 WRONGTYPE，不能删除。
- 对已有 Bloom key 的 overwrite 语义要明确，并与官方行为做兼容测试。

### 3. `BF.LOADCHUNK` 会删除非 Bloom key，违反 wrong-type 保护

位置：`modules/gemini-bloom/src/bloom_commands.cc:488-493`

当前逻辑只要 `cursor == 1` 且 key 非空，就无条件删除 key。这包括 string/list/hash 等普通 Redis 类型。

实际风险：

- `SET k v` 后执行 `BF.LOADCHUNK k 1 <valid header>` 会把 string key 替换成 Bloom filter。
- 官方命令文档把 wrong key type 列为错误场景；Redis 模块通常也不能让恢复命令绕过类型保护。

建议：

- 如果 key 非空且类型不是 `BloomType`，直接返回 `WRONGTYPE`。
- 若保留 overwrite 语义，只允许覆盖 Bloom key，并先验证完整 header。

### 4. RDB 读取 bit array 时不校验长度，短 blob 会读入未初始化内存，长 blob 会静默截断

位置：`modules/gemini-bloom/src/bloom_rdb.cc:50-62`

`BloomLayer::ReadFrom` 使用 `RMAlloc(dataSize_)` 后，只复制 `min(bufLen, dataSize_)` 字节：

- `bufLen < dataSize_`：剩余字节未初始化，产生随机 bit。
- `bufLen > dataSize_`：多余数据静默丢弃。

实际风险：

- 损坏 RDB 可能被接受，导致 false positive 激增或状态不可复现。
- 未初始化内存会让同一 RDB 在不同进程中表现不同。
- 这类路径没有被当前 Tcl 测试覆盖。

建议：

- 要求 `bufLen == dataSize_`，否则 RDB load 失败。
- 如果需要容忍旧格式，也必须有版本门控和显式 zero-fill。

### 5. 反序列化缺少完整性校验，恶意 header/RDB 可触发 OOM、除零或 UB

位置：

- `modules/gemini-bloom/src/bloom_rdb.cc:168-198`
- `modules/gemini-bloom/src/sb_chain.cc:92-102`

`DeserializeHeader` 只检查 header 长度和 `numLayers <= 1024`，未校验：

- flags 是否只包含已知位。
- `expansionFactor > 0`。
- `dataSize == aligned_bytes(totalBits)`。
- `totalBits > 0`，`hashCount > 0`。
- `fpRate`、`bitsPerEntry` 是有限数且范围合理。
- layer itemCount 之和是否等于 totalItems。
- 每层 itemCount 是否超过 capacity。

如果 corrupt header 设置 `expansionFactor = 0` 且非 fixed，后续插入会在 `UINT64_MAX / expansionFactor_` 处除零。

建议：

- 引入 `ValidateLayerMeta` 和 `ValidateFilterMeta`。
- 对 RDB load 和 SCANDUMP header 使用同一套校验。
- 失败时原 key 不应被修改。

### 6. `FilterLayer` 数组使用 `RMRealloc/RMCalloc` 管理非平凡 C++ 对象，存在对象生命周期 UB

位置：

- `modules/gemini-bloom/src/sb_chain.cc:62-77`
- `modules/gemini-bloom/src/sb_chain.cc:141-163`

`FilterLayer` 内含 `BloomLayer`，而 `BloomLayer` 有析构函数、移动构造和移动赋值，属于非平凡对象。当前实现存在两个问题：

- `RMRealloc` 直接搬移已构造的 `FilterLayer`，绕过 C++ move 构造。
- `FromRdbShell` 用 `RMCalloc` 分配 raw storage 后设置 `numLayers_`，`SetLayer` 对未构造对象做赋值。

实际风险：

- 当前字段简单时可能“看起来能跑”，但在 C++ 语义上是未定义行为。
- 一旦 `BloomLayer` 增加字段或不再能 bitwise relocate，可能出现 double free、leak 或崩溃。

建议：

- 用 `std::vector<FilterLayer>`，并接入 Redis allocator；或显式使用 placement-new/move/destroy。
- `FromRdbShell` 应逐个 placement-new 构造 layer，失败时只析构已构造数量。

### 7. 构造首层失败会泄漏已扩容的 layer storage

位置：

- `modules/gemini-bloom/src/sb_chain.cc:17-20`
- `modules/gemini-bloom/src/sb_chain.cc:62-72`

`AppendLayer` 会先 `RMRealloc` 分配 `layers_`，然后创建 `BloomLayer`。如果 `BloomLayer::Create` 失败，构造函数把
`layers_` 置空，但没有释放刚分配的 layer storage。

建议：

- `AppendLayer` 在首次创建失败时释放 storage，或由构造函数在置空前释放。
- 更好的修复是用 RAII 容器统一管理 layer storage。

### 8. `BytesUsed()` 少算了 `layerCapacity_` 预留空间

位置：`modules/gemini-bloom/src/sb_chain.cc:131-137`

`AppendLayer` 的数组容量按 4、8、16 扩张，但 `BytesUsed()` 只统计 `numLayers_ * sizeof(FilterLayer)`，没有统计
`layerCapacity_ * sizeof(FilterLayer)`。

实际风险：

- `BF.INFO Size` 和 `mem_usage` 低估真实内存。
- Redis eviction、memory introspection 和容量规划会得到偏小数据。

建议：

- 统计实际分配容量，或改成每次精确分配并与 RedisBloom 行为一致。

### 9. `MEXISTS` 在 wrong type 上返回数组错误，而不是顶层错误

位置：`modules/gemini-bloom/src/bloom_commands.cc:329-334`

当前 `BF.MEXISTS string_key a b` 会返回数组，每个元素是 WRONGTYPE 错误。常规 Redis 命令语义和官方文档都把 wrong type
视为命令级错误。

建议：

- 与 `BF.EXISTS`、`BF.INFO` 保持一致：发现 wrong type 直接返回顶层 WRONGTYPE。

### 10. `BF.RESERVE`/`BF.INSERT` 接受 `EXPANSION 0`，并把它隐式改成 non-scaling

位置：

- `modules/gemini-bloom/src/bloom_commands.cc:17-23`
- `modules/gemini-bloom/src/bloom_commands.cc:110-117`
- `modules/gemini-bloom/src/bloom_commands.cc:224-230`

RedisBloom BF 的 `EXPANSION` 是正整数。当前代码允许 0，并把 flags 设成 fixed-size，但内部 expansionFactor 又保存为 2。

实际风险：

- 用户显式要求 expansion 0，实际得到的是 non-scaling filter。
- `BF.INFO Expansion` 返回 null，与输入参数不一致。
- 兼容 RedisBloom 时行为不同。

建议：

- 对 BF 命令拒绝 `EXPANSION < 1`。
- 不要用 expansion 0 表达 non-scaling；只接受显式 `NONSCALING`。
