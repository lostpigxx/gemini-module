# gemini-bloom 性能问题

审查基准：`lostpigxx/gemini-module` 的 `main` 分支，重点文件为 `modules/gemini-bloom/**`、根 `CMakeLists.txt`、`README.md`、`include/mock_redismodule_io.h`。对照对象为 `RedisBloom/RedisBloom` 当前公开实现与 redis.io 命令文档。

严重等级：`P0` = 数据损坏/未定义行为/协议级不兼容；`P1` = 生产可用性或兼容性重大问题；`P2` = 功能、性能、维护性明显缺陷；`P3` = 较小但应修复的问题。


## 1. P1：重复检测按旧层到新层扫描，扩容后热点写入变慢

**位置**：`modules/gemini-bloom/src/sb_chain.cc:ScalingBloomFilter::IsDuplicate`。

当前 `IsDuplicate` 使用 `std::ranges::any_of(Layers(), ...)`，也就是从第 0 层扫到最新层。RedisBloom 的 `SBChain_Add/Check` 是从最新层倒序扫描。

**为什么这是性能问题**：

- 扩容后，新插入元素主要在最新层。
- 对重复写入最近元素，Gemini 需要先扫所有旧层，最后才命中新层。
- 层数越多，热点重复写性能越差。

**复杂度影响**：

- 理论复杂度都是 `O(numLayers * hashCount)`。
- 实际常数差异很大：倒序扫描能让“最近数据”在第一层命中。

**修复方向**：从 `numLayers_ - 1` 倒序扫描到 0。

## 2. P1：SCANDUMP/AOF 每次输出整层 bit array，没有 chunk 上限

**位置**：`CmdScandump`、`AofRewriteBloom`。

Gemini 每层 bit array 一次性返回或写入 AOF。RedisBloom 使用最大 16MiB chunk。

**后果**：

- 大 filter 会产生超大 Redis reply，阻塞 Redis 单线程。
- 网络缓冲、客户端内存和 AOF rewrite 内存压力显著上升。
- 单层超过客户端或代理限制时，迁移失败。

**修复方向**：实现固定最大 chunk size，复用 RedisBloom iterator 语义。

## 3. P1：小容量 filter 被强制至少 1024 bit，内存开销高于 RedisBloom

**位置**：`BloomLayer::Create`。

`totalBits_ = max(capacity * bitsPerEntry, 1024)`。这对默认容量 100 影响不算极端，但对 `capacity=1/2/10` 的 filter，内存膨胀非常明显。

**后果**：

- 大量小 filter 场景下内存浪费。
- `BF.INFO Size` 与 RedisBloom 不一致。
- RDB/AOF/SCANDUMP payload 变大。

**修复方向**：按 RedisBloom NoRound 的 bit/byte 对齐规则计算，不设置 1024 bit 下限；若保留下限，必须文档化为非兼容优化。

## 4. P1：`BloomMemUsage` 低估真实内存，影响 Redis 内存治理

**位置**：`ScalingBloomFilter::BytesUsed`、`BloomMemUsage`。

见代码 bug 文件 §13。性能维度下的影响是：Redis 的内存统计、内存淘汰策略、运维告警会低估模块对象内存。

**修复方向**：统计 `layerCapacity_`、allocator overhead 可选；至少统计所有已分配数组容量。

## 5. P1：RDB load 多一次 bit array 拷贝

**位置**：`BloomLayer::ReadFrom`。

RedisModule 的 `LoadStringBuffer` 已经返回一段新分配 buffer。Gemini 又分配 `bitArray_` 并复制进去。RedisBloom 参考实现直接把加载得到的 buffer 作为 bloom bit array，并记录其长度。

**后果**：

- RDB 加载峰值内存约为 bit array 的两倍。
- 大量或大型 BloomFilter 重启恢复更慢。

**修复方向**：

- 校验 blob 长度后直接接管 `LoadStringBuffer` 返回的内存，前提是释放函数一致。
- 或者使用 move/copy 策略但避免峰值翻倍，例如分块加载。

## 6. P1：缺少 defrag callback，长期运行更容易碎片化

**位置**：`redis_bloom_module.cc` 的 `RedisModuleTypeMethods`。

`ScalingBloomFilter` 包含两级动态内存：`layers_` 和每层 `bitArray_`。没有 defrag callback 时，Redis active defrag 无法搬迁这些模块内部指针。

**后果**：

- 大量创建/删除/扩容 filter 后，RSS 可能持续高于逻辑内存。
- 生产 Redis 实例的碎片率更难控制。

**修复方向**：实现 `defrag`：重定位 filter object、`layers_`、每层 `bitArray_`。

## 7. P2：读命令没有使用 no-touch/no-stats 等 OpenKey flag

**位置**：`CmdExists`、`CmdMexists`、`CmdInfo`、`CmdCard`、`CmdScandump`。

这些命令基本是只读查询，但打开 key 时只用 `REDISMODULE_READ`。在热点查询场景下，访问 key 可能影响 LRU/LFU 和 keyspace stats。

**后果**：

- 热点 Bloom 查询会污染 LRU/LFU 信息。
- 纯观测命令如 `BF.INFO` 也可能影响访问统计。

**修复方向**：按命令语义使用 `REDISMODULE_OPEN_KEY_NOTOUCH`、`NOSTATS` 等 flag；是否启用要与 Redis 版本兼容性一起评估。

## 8. P2：layer 数组扩容策略会复制所有 layer，且目前实现还有 UB

**位置**：`AppendLayer`。

扩容时容量 4、8、16 翻倍。对于层数不多的 BloomFilter 通常不是瓶颈，但每次扩容都要搬迁所有 `FilterLayer`。

**后果**：

- 在极端 expansion=1、小初始容量、海量插入场景下，层数会增长较多，扩容搬迁成本上升。
- 当前用 `realloc` 对非平凡对象 UB，性能问题和正确性问题叠加。

**修复方向**：修复对象生命周期后，可继续使用翻倍扩容；同时限制 expansion=1 的层数增长或增加合理上限。

## 9. P2：`EXPANSION 1` 可导致层数线性增长

**位置**：`GrowIfNeeded`。

当 expansion factor 为 1 时，每层容量不增长，只是 error rate 继续减半。插入量增长时层数约线性增长。

**后果**：

- `Contains/Put` 需要扫描越来越多层。
- header、RDB、AOF 元数据持续膨胀。

**修复方向**：

- 与 RedisBloom 保持兼容则允许 expansion=1，但文档警告。
- 若追求生产性能，可将默认或建议范围设为 `>=2`，或对层数设置硬上限。

## 10. P2：hash 长度截断既是正确性问题，也是性能/DoS 入口

**位置**：`Hash32Policy/Hash64Policy`。

超长 value 仍会读取前 `INT_MAX` 字节，成本极高，却忽略后缀。攻击者可发送共享长前缀的大 value，制造高 CPU 和高冲突率。

**修复方向**：设置合理最大 item size，超过则错误返回；或使用支持 streaming/size_t 的 hash。

## 11. P2：命令层没有批量 hash/批量 probe 优化

**位置**：`CmdMadd`、`CmdMexists`、`CmdInsert`。

当前每个 item 完整走一次 hash 和层扫描。对于批量命令，可以至少做以下优化：

- 对同一个命令的 key/type/flags 只解析一次，已做到。
- 按最新层优先扫描，减少重复热点开销。
- 对 `MEXISTS` 只读场景可以避免构建不必要中间对象，当前已基本直接回复。

这不是首要瓶颈，优先级低于 cursor chunk 和层扫描顺序。

## 12. P3：临时 header 分配可优化

**位置**：`CmdScandump`、`AofRewriteBloom`。

header size 为 `sizeof(header)+numLayers*sizeof(meta)`，通常很小。现在每次 `BF.SCANDUMP cursor=0` 都动态分配。可以使用 Redis module pool allocator 或小对象栈/arena 策略，但这不是主要问题。

**建议**：在完成协议修复后再优化。
