# gemini-bloom 代码实现细节问题

审查基准：`lostpigxx/gemini-module` 的 `main` 分支，重点文件为 `modules/gemini-bloom/**`、根 `CMakeLists.txt`、`README.md`、`include/mock_redismodule_io.h`。对照对象为 `RedisBloom/RedisBloom` 当前公开实现与 redis.io 命令文档。

严重等级：`P0` = 数据损坏/未定义行为/协议级不兼容；`P1` = 生产可用性或兼容性重大问题；`P2` = 功能、性能、维护性明显缺陷；`P3` = 较小但应修复的问题。


## 1. P0：手写内存管理与 C++ 对象生命周期不匹配

**位置**：`sb_chain.cc`、`bloom_filter.cc`、`rm_alloc.h`。

当前核心结构使用 `RMAlloc/RMCalloc/RMRealloc/RMFree` 管理 C++ 对象数组，又对 `BloomLayer` 自定义析构和移动。这个组合很容易破坏对象生命周期，已经在 `AppendLayer` 和 `FromRdbShell` 中出现未定义行为。

**建议**：

- 要么把 `BloomLayer` 设计成 trivially relocatable 的 C struct，并明确只用 C 风格管理；
- 要么使用 C++ RAII 容器，并为 Redis allocator 写 allocator adapter。

## 2. P1：`Put` 返回 `std::optional<bool>` 语义不够清晰

**位置**：`ScalingBloomFilter::Put`。

当前约定：

- `true` = 新插入；
- `false` = 重复；
- `nullopt` = fixed-size full。

**问题**：

- 命令层必须知道 `nullopt` 的唯一含义。
- 未来如果出现 OOM、invalid state、metadata corrupted，`optional<bool>` 不够表达。

**建议**：

```cpp
enum class PutResult {
  Inserted,
  Duplicate,
  Full,
  OutOfMemory,
  InvalidState,
};
```

命令层再把 enum 映射到 Redis reply。

## 3. P1：`BloomFlags` 接受任意 bit，没有 known-mask 校验

**位置**：`bloom_filter.h:FromUnderlying`、`bloom_rdb.cc:DeserializeHeader/ReadFrom`。

`FromUnderlying` 只是 cast。外部 RDB/LOADCHUNK 可以带入未知 flag bit。

**后果**：

- 未知 bit 被静默保留并重新序列化。
- 行为不可预测；未来新增 flag 时难以兼容。

**建议**：

```cpp
constexpr unsigned kKnownBloomFlags =
  ToUnderlying(BloomFlags::NoRound) |
  ToUnderlying(BloomFlags::RawBits) |
  ToUnderlying(BloomFlags::Use64Bit) |
  ToUnderlying(BloomFlags::FixedSize);
```

反序列化时拒绝 `flags & ~kKnownBloomFlags`。

## 4. P1：`BloomLayer::Create` 同时处理参数计算、兼容 flag、raw bits、分配

**位置**：`BloomLayer::Create`。

函数职责过多，导致 `RawBits` 这样的路径看似支持、实则错误。

**建议拆分**：

- `ComputeBloomParams(cap, fp, flags) -> expected params or error`
- `AllocateBloomLayer(params) -> BloomLayer`
- `ValidateBloomParams(params)`
- `CreateFromWireMeta(meta, flags)`

这样 RDB/SCANDUMP 与命令创建可复用同一套校验。

## 5. P1：SCANDUMP header 使用 native packed struct，缺少格式边界说明

**位置**：`sb_chain.h:WireLayerMeta/WireFilterHeader`。

RedisBloom 也使用 packed C struct，但这意味着 wire format 事实上绑定：

- 整数大小；
- native endian；
- IEEE double；
- struct packing；
- 当前编译器 ABI。

当前代码注释说这是 protocol-level constants，但没有单独文档和 fixture。

**建议**：

- 为 wire header 写静态断言：`sizeof(WireFilterHeader)`、`sizeof(WireLayerMeta)`。
- 增加 RedisBloom 官方 fixture 字节对比。
- 文档中明确 endian/ABI 假设。

## 6. P1：RDB reader wrapper 无法表达读取失败

**位置**：`RdbReader::GetUint/GetFloat/GetBlob`。

这些函数直接返回值，没有错误状态。短读、类型不匹配、IO error 都无法向上层传播。

**建议**：

- `RdbReader` 内部维护 `bool ok_`。
- 每次读取后检查 RedisModule IO error。
- 上层只在 `reader.ok()` 时构建对象。

## 7. P1：注释对兼容性的断言过强

**位置**：`bloom_filter.h` flags 注释、`bloom_rdb.cc` RDB/wire 注释、`redis_bloom_module.cc` type name 注释。

代码多次使用“required for interoperability”“protocol constants”等表述。但当前实现仍存在 BF.INFO、SCANDUMP/LOADCHUNK、配置、参数范围、RESP3 等兼容缺口。

**建议**：

- 注释改为可验证事实，例如“numeric values intentionally match RedisBloom option bits”。
- 把兼容性声明放到文档和测试 fixture 中，而不是代码注释里直接宣称。

## 8. P2：参数解析没有记录 option 是否显式指定

**位置**：`ParseInsertOptions`、`CmdReserve`。

解析后只保留值，不保留 `errorSpecified/capacitySpecified/expansionSpecified/nonScalingSpecified`。

**后果**：

- 无法校验 `NOCREATE + ERROR/CAPACITY`。
- 无法校验重复 option 或互斥 option。
- 错误消息难以精确。

**建议**：`InsertOptions` 增加 specified flags。

## 9. P2：`MatchArg` 使用 POSIX `strncasecmp`，且没有集中处理 Redis 参数比较

**位置**：`bloom_commands.cc:MatchArg`、`bloom_config.cc`。

问题包括：

- 头文件错误；
- 非标准 API；
- 参数比较策略散落；
- 没有统一处理二进制安全与 ASCII 大小写。

**建议**：实现 `AsciiEqualsIgnoreCase(std::string_view, std::string_view)`。

## 10. P2：`GetFilter` 对 key 状态的假设太隐式

**位置**：`bloom_commands.cc:GetFilter`。

`GetFilter` 只检查 ModuleType 是否等于 BloomType，不区分 empty、非 module、其他 module type。调用方必须先做 KeyType 检查，否则行为依赖 RedisModule API 对非 module key 的处理。

**建议**：封装成：

```cpp
enum class LookupStatus { Ok, Missing, Empty, WrongType };
LookupStatus LookupBloomFilter(RedisModuleKey*, ScalingBloomFilter**);
```

命令层统一处理错误。

## 11. P2：`OpenOrCreate` 返回 `outKey` 但调用方实际不使用

**位置**：`OpenOrCreate`、`CmdAdd`、`CmdMadd`。

`CmdAdd/CmdMadd` 声明 `RedisModuleKey* key`，但除了传给 `OpenOrCreate` 外没有使用。这个 out 参数增加了误解空间。

**建议**：删除 `outKey`，只返回 filter 与 created 状态；如果需要 key 生命周期，封装 RAII key wrapper。

## 12. P2：`TotalCapacity`/`BytesUsed` 没有溢出保护

**位置**：`ScalingBloomFilter::TotalCapacity`、`BytesUsed`。

`transform_reduce` 累加 `uint64_t` 或 `size_t`，没有检测溢出。正常参数范围下问题不大，但 RDB/LOADCHUNK 可构造异常元数据。

**建议**：反序列化阶段先限制容量和层数；累加函数使用 checked add。

## 13. P2：`DeserializeHeader` 有 `kMaxLayers`，RDB load 没有同等上限

**位置**：`DeserializeHeader`、`ScalingBloomFilter::ReadFrom`。

SCANDUMP header 限制 `numLayers <= 1024`，RDB load 不限制。两个外部入口的安全策略不一致。

**建议**：定义统一的 `kMaxLayers` 和最大总字节数，在所有反序列化入口使用。

## 14. P2：`AofRewriteBloom` 与 `CmdScandump` 没有共享编码逻辑

**位置**：`bloom_rdb.cc:AofRewriteBloom`、`bloom_commands.cc:CmdScandump`。

两者都需要输出 header 和 chunks，但分别实现。当前都采用“整层 bit array”思路；后续修复 RedisBloom chunk 协议时，重复实现容易再次分叉。

**建议**：抽象：

```cpp
class BloomDumpCursor {
  DumpChunk Next();
};
```

SCANDUMP 和 AOF rewrite 共用。

## 15. P3：`AsBytes` 两个 overload 可以更明确

**位置**：`bloom_filter.h:AsBytes`。

`const char*` 和 `const void*` overload 都存在。当前用法没出错，但对调用者来说语义不明显。

**建议**：

- 命令层使用 `AsByteSpan(const char*, size_t)`。
- 测试或内部 binary buffer 使用 `AsByteSpan(const uint8_t*, size_t)`。
- 避免泛化 `const void*`，减少误传对象地址。

## 16. P3：编译选项缺少 UBSAN target

**位置**：根 `CMakeLists.txt`。

只有 `ENABLE_ASAN`。本项目的主要高风险是 C++ 对象生命周期、移位、整数溢出、未初始化读；UBSAN/Integer sanitizer 更关键。

**建议**：增加：

- `ENABLE_UBSAN`
- `ENABLE_MSAN`，如环境支持
- `ENABLE_TSAN`，如未来多线程或后台任务出现
