# gemini-bloom 代码 Bug 挑刺

审查基准：`lostpigxx/gemini-module` 的 `main` 分支，重点文件为 `modules/gemini-bloom/**`、根 `CMakeLists.txt`、`README.md`、`include/mock_redismodule_io.h`。对照对象为 `RedisBloom/RedisBloom` 当前公开实现与 redis.io 命令文档。

严重等级：`P0` = 数据损坏/未定义行为/协议级不兼容；`P1` = 生产可用性或兼容性重大问题；`P2` = 功能、性能、维护性明显缺陷；`P3` = 较小但应修复的问题。


## 1. P0：`AppendLayer` 用 `realloc` 搬迁非平凡 C++ 对象，存在未定义行为

**位置**：`modules/gemini-bloom/src/sb_chain.cc:ScalingBloomFilter::AppendLayer`；`modules/gemini-bloom/src/bloom_filter.h/.cc:BloomLayer`。

`FilterLayer` 内含 `BloomLayer`，而 `BloomLayer` 自定义了析构函数、移动构造、移动赋值，并持有 `bitArray_` 所有权。`AppendLayer` 在扩容时直接对 `FilterLayer* layers_` 调用 `RMRealloc`，这等价于按字节搬迁对象；对非 trivially relocatable 的 C++ 对象不成立。

**后果**：

- 标准层面是未定义行为；在优化编译、不同 libc allocator、ASAN/UBSAN 下都可能暴露。
- 如果未来 `BloomLayer` 增加更多资源字段，问题会从“侥幸可跑”变成双重释放、泄漏或悬挂指针。
- 当前测试不会稳定发现，因为对象内容主要是裸指针和整数。

**修复方向**：

- 用 `std::vector<FilterLayer, RedisAllocator<FilterLayer>>`，或者自写 `ReserveLayers`：新分配 raw storage → placement-new move 每个元素 → 显式析构旧元素 → 释放旧 storage。
- 禁止对含非平凡对象的数组使用 `realloc`。

## 2. P0：`FromRdbShell` 为 `FilterLayer` 分配了零初始化字节，但没有构造对象

**位置**：`modules/gemini-bloom/src/sb_chain.cc:ScalingBloomFilter::FromRdbShell` 与 `SetLayer`。

`FromRdbShell` 使用 `RMCalloc(shell.numLayers, sizeof(FilterLayer))` 得到一段字节内存，然后直接设置 `numLayers_ = shell.numLayers`。之后 `SetLayer` 执行：对 `layers_[index]` 做赋值。这里的问题是 `layers_[index]` 从未 placement-new 构造过。

**后果**：

- `SetLayer` 对未构造对象调用移动赋值，是未定义行为。
- 析构函数按 `numLayers_` 对每个 `layers_[i]` 调 `~FilterLayer()`，也会对未构造对象析构。
- 如果 RDB/LOADCHUNK 反序列化中途失败，析构路径同样不安全。

**修复方向**：

- `FromRdbShell` 只设置 `layerCapacity_`，`numLayers_` 先置 0；每次读入一层后 placement-new 构造并递增 `numLayers_`。
- 或者分配后循环 `new (&layers_[i]) FilterLayer{{}, 0};`，保证每个槽位有对象生命周期。

## 3. P0：RDB 读取 bit array 时长度不匹配也静默接受，短 payload 会留下未初始化内存

**位置**：`modules/gemini-bloom/src/bloom_rdb.cc:BloomLayer::ReadFrom`。

读取 RDB blob 后，代码按 `totalBits_` 计算 `dataSize_`，再 `RMAlloc(dataSize_)`，然后复制 `min(bufLen, dataSize_)`。当 `bufLen < dataSize_` 时，尾部未写入；由于使用 `RMAlloc` 而不是 `RMCalloc`，尾部是未初始化数据。

**后果**：

- 损坏或截断 RDB 可能被当成有效 BloomFilter 加载。
- 未初始化尾部可能随机置位，导致大量假阳性。
- 这类输入来自 RDB 文件或复制/迁移，属于不可信边界。

**修复方向**：

- 严格要求 `bufLen == expectedDataSize`，不等则返回错误。
- 至少对短 payload 使用 `RMCalloc` 并拒绝加载，不能静默修补。
- 加载后调用完整 integrity check：`bits == bytes * 8`、`hashes == ceil(ln2*bpe)`、`error in (0,1)`、`n2 <= 63`。

## 4. P0：反序列化元数据未校验，可能触发越界、除零或移位 UB

**位置**：`modules/gemini-bloom/src/bloom_rdb.cc:BloomLayer::ReadFrom`、`BloomLayer::FromWireMeta`、`DeserializeHeader`；`modules/gemini-bloom/src/bloom_filter.cc:ComputeModuloMask/Test/Insert`。

RDB 与 SCANDUMP header 中的 `hashCount`、`totalBits`、`dataSize`、`log2Bits`、`fpRate`、`bitsPerEntry`、`capacity` 基本被信任。后续 `ProbePosition` 会使用 `% totalBits`；`ComputeModuloMask` 会执行 `1ULL << log2Bits_`。

**后果**：

- `totalBits == 0` 会导致取模除零。
- `log2Bits >= 64` 会导致左移未定义行为。
- `dataSize < ceil(totalBits/8)` 会导致 `TestBit/SetBit` 越界。
- `hashCount == 0` 会让 `Test` 对任意元素返回 true。

**修复方向**：

- 增加统一的 `ValidateLayerMeta`。
- 校验 `dataSize == (totalBits + 7) / 8` 或与 RedisBloom 对齐规则一致。
- 校验 `hashCount >= 1`，除非明确支持 raw bitset；当前代码并未正确支持 raw bitset。

## 5. P0：`RawBits` 分支会让所有查询命中，且插入不改变 bitset

**位置**：`modules/gemini-bloom/src/bloom_filter.cc:BloomLayer::Create`；`BloomLayer::Test/Insert`。

`RawBits` 设置 `hashCount_ = 0`。`Test` 的循环次数为 0，于是直接返回 true；`Insert` 的循环次数为 0，于是返回 false 且不置位。

**后果**：

- 只要外部 RDB/SCANDUMP header 或未来命令路径带入 `RawBits` flag，该过滤器就退化为“所有元素都存在”。
- 与 RedisBloom 的 `BLOOM_OPT_ENTS_IS_BITS` 语义不等价；RedisBloom 仍会计算 hash 数并正常查询。

**修复方向**：

- 若不支持 raw bits，反序列化时拒绝该 flag。
- 若要支持，需要按 RedisBloom `BLOOM_OPT_ENTS_IS_BITS` 语义初始化 `entries/bits/hashCount/bpe`，不能把 `hashCount` 置 0。

## 6. P1：`Hash32Policy/Hash64Policy` 对超长元素静默截断到 `INT_MAX`

**位置**：`modules/gemini-bloom/src/bloom_filter.cc:Hash32Policy::Compute`、`Hash64Policy::Compute`。

代码将 `std::span` 的长度压到 `INT_MAX` 后传给 MurmurHash。结果是两个长度超过 `INT_MAX`、且前 `INT_MAX` 字节相同的不同值会得到相同 hash。

**后果**：

- 极大 value 下出现系统性误判，不再是 BloomFilter 的随机假阳性模型。
- 这是静默语义变化，用户无从得知后缀被忽略。

**修复方向**：

- 如果 hash 函数只支持 `int len`，命令层应拒绝超过 `INT_MAX` 的元素。
- 更好是改造 hash 实现支持 `size_t` 长度。

## 7. P1：`BloomLayer::Create` 对超大容量和 bit 数计算缺少溢出保护

**位置**：`modules/gemini-bloom/src/bloom_filter.cc:BloomLayer::Create`。

`rawBits = double(cap) * bpe` 后再转 `uint64_t`。当 `cap` 极大或 `fpRate` 极小时，浮点精度、无穷大、整数转换、`totalBits + 7` 都可能出问题。

**后果**：

- 可能错误分配过小数组，之后访问越界。
- 可能误返回 allocation failure，而不是明确的参数越界。
- 与 RedisBloom 的配置上限相比，这里没有硬上限，风险更大。

**修复方向**：

- 命令层限制 capacity/error/expansion 范围。
- 参数计算用显式上界检查：`cap <= max / bpe`、`totalBits <= UINT64_MAX - 7`。
- 对 `NaN/inf` 直接拒绝。

## 8. P1：`BF.LOADCHUNK key 1 data` 会删除任何已有 key，坏 header 也会导致数据丢失

**位置**：`modules/gemini-bloom/src/bloom_commands.cc:CmdLoadchunk`。

当 cursor 为 1 时，如果 key 非空，代码先 `RedisModule_DeleteKey(key)`，再解析 header。若 header 解析失败，旧 key 已被删除。

**后果**：

- 对字符串、列表或已有 Bloom key 执行错误的 `BF.LOADCHUNK k 1 <bad>` 会破坏数据。
- 与 Redis 的“先验证、后替换”基本安全边界不符。

**修复方向**：

- 先在临时对象中完成 header 解析和校验。
- 只有解析成功并确认目标允许覆盖时才替换 key。
- 对非空 key 应明确遵循 RedisBloom 行为，不能静默删除。

## 9. P1：`BF.MEXISTS` 对 wrongtype 先回复数组，再塞多个错误元素

**位置**：`modules/gemini-bloom/src/bloom_commands.cc:CmdMexists`。

函数先 `ReplyWithArray(count)`，之后才检查 key 是否为 Bloom 类型。wrongtype 时按 item 数量回复多个 `WRONGTYPE` error 元素。

**后果**：

- 客户端看到的是“数组回复中包含 error 元素”，不是普通命令级错误。
- 与 `BF.EXISTS`、`BF.INFO` 等命令的 wrongtype 行为不一致。
- 多数 Redis 客户端不会按这种形态处理 wrongtype。

**修复方向**：

- 在写任何 reply 之前完成 key 类型校验。
- wrongtype 时返回单个 `RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE)`。

## 10. P1：`BF.MADD/BF.INSERT` 在固定数组回复中间插入错误，且已部分修改数据

**位置**：`modules/gemini-bloom/src/bloom_commands.cc:PutAndReply`、`CmdMadd`、`CmdInsert`。

`CmdMadd/CmdInsert` 先声明固定数组长度，再逐项插入。若 NONSCALING 过滤器中途满了，`PutAndReply` 会在数组元素位置回复 error，并继续后续循环。

**后果**：

- 回复形态变成混合整数和 error，客户端处理困难。
- 命令可能部分成功、部分失败，但没有文档说明。
- 与 RedisBloom 使用 postponed array 并在错误处截断回复的实现不同。

**修复方向**：

- 使用 `REDISMODULE_POSTPONED_ARRAY_LEN`，并在错误处停止。
- 明确文档化 partial write；若要原子性，则先预检查容量或实现 rollback。

## 11. P1：RDB IO 错误处理缺失

**位置**：`modules/gemini-bloom/src/redis_bloom_module.cc`、`modules/gemini-bloom/src/bloom_rdb.cc`。

模块没有设置 `REDISMODULE_OPTIONS_HANDLE_IO_ERRORS`，`RdbReader` 也没有检查 `RedisModule_IsIOError` 或等价机制。

**后果**：

- 截断 RDB、IO 错误、LoadStringBuffer 失败时可能被普通数据处理。
- 测试 mock 也没有模拟 IO error，因此风险被掩盖。

**修复方向**：

- OnLoad 设置 IO error handling option。
- 每次读取后检查 IO 错误并终止加载。
- mock 中增加短读和错误注入。

## 12. P1：`strncasecmp` 使用头文件错误，影响可移植构建

**位置**：`modules/gemini-bloom/src/bloom_commands.cc`、`modules/gemini-bloom/src/bloom_config.cc`。

代码包含 `<cstring>`，但使用 POSIX `strncasecmp`；该函数声明位于 `<strings.h>`，不是 C++ 标准库 `<cstring>`。

**后果**：

- 在严格编译器/平台上会报未声明函数。
- Windows/MSVC 不提供 `strncasecmp`。

**修复方向**：

- POSIX 平台包含 `<strings.h>`。
- 更稳妥是实现二进制安全的 ASCII case-insensitive 比较，避免依赖非标准 API。

## 13. P2：`BloomMemUsage` / `BytesUsed` 低估内存

**位置**：`modules/gemini-bloom/src/sb_chain.cc:BytesUsed`；`modules/gemini-bloom/src/bloom_rdb.cc:BloomMemUsage`。

`BytesUsed` 只按 `numLayers_ * sizeof(FilterLayer)` 计算 layer 数组，但实际分配容量是 `layerCapacity_`，初始至少 4，并随扩容翻倍。

**后果**：

- Redis `MEMORY USAGE`、内存淘汰策略和运维观测低估模块内存。
- 大量小 filter 下误差明显。

**修复方向**：

- 统计 `layerCapacity_ * sizeof(FilterLayer)`。
- 根据 Redis 模块约定是否需要加入 allocator overhead，至少不能漏掉已分配对象容量。

## 14. P2：`DeserializeHeader` 接受带尾随字节的 header

**位置**：`modules/gemini-bloom/src/bloom_rdb.cc:DeserializeHeader`。

当前只判断 `length < required`，`length > required` 也被接受。

**后果**：

- 损坏或拼接 payload 被静默接受。
- 与严格的 wire format 校验不一致，降低 fuzzing 和安全边界质量。

**修复方向**：将判断改为 `length == required`，并对每个 layer meta 做 integrity 校验。

## 15. P2：`AofRewriteBloom` header 分配失败时静默丢 key

**位置**：`modules/gemini-bloom/src/bloom_rdb.cc:AofRewriteBloom`。

如果 `RMAlloc(hdrBytes)` 返回 null，函数直接 `return`。

**后果**：

- AOF rewrite 可能遗漏该 Bloom key。
- 运维层只看到 rewrite 成功，但重启后数据缺失。

**修复方向**：AOF rewrite 回调虽然不能直接返回错误，也应至少记录错误并考虑使用 RedisModule API 支持的 OOM/abort 机制；更好是避免大临时分配或使用可失败路径的明确策略。
