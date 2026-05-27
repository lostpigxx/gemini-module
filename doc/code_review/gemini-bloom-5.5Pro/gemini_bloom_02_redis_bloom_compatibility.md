# gemini-bloom 与 RedisBloom 兼容性问题

审查基准：`lostpigxx/gemini-module` 的 `main` 分支，重点文件为 `modules/gemini-bloom/**`、根 `CMakeLists.txt`、`README.md`、`include/mock_redismodule_io.h`。对照对象为 `RedisBloom/RedisBloom` 当前公开实现与 redis.io 命令文档。

严重等级：`P0` = 数据损坏/未定义行为/协议级不兼容；`P1` = 生产可用性或兼容性重大问题；`P2` = 功能、性能、维护性明显缺陷；`P3` = 较小但应修复的问题。


## 1. 当前已经对齐的点

这些点说明项目确实尝试对齐 RedisBloom，但不能掩盖后面的协议差异。

| 项 | gemini-bloom | RedisBloom 参考 | 结论 |
|---|---|---|---|
| 数据类型名 | `MBbloom--` | `MBbloom--` | 名称对齐 |
| 当前 Bloom RDB encver | `4` | `4` | 大版本对齐 |
| flag 数值 | `NoRound=1, RawBits=2, Use64Bit=4, FixedSize=8` | `BLOOM_OPT_NOROUND=1, BLOOM_OPT_ENTS_IS_BITS=2, BLOOM_OPT_FORCE64=4, BLOOM_OPT_NO_SCALING=8` | 数值对齐 |
| Murmur hash seed | 32-bit `0x9747b28c`，64-bit `0xc6a4a7935bd1e995` | 相同 seed | hash seed 对齐 |
| RDB encver4 字段序 | total size、filters、options、growth、每层 metadata、bit array、size | 同类字段序 | 基本对齐，但校验不足 |

## 2. P0：`BF.SCANDUMP/BF.LOADCHUNK` cursor 协议不兼容

**位置**：`modules/gemini-bloom/src/bloom_commands.cc:CmdScandump/CmdLoadchunk`；`modules/gemini-bloom/src/bloom_rdb.cc:AofRewriteBloom`。  
**RedisBloom 参考**：`src/rebloom.c:BFScanDump_RedisCommand/BFLoadChunk_RedisCommand`、`src/sb.c:SBChain_GetEncodedChunk/SBChain_LoadEncodedChunk`。

RedisBloom 的协议是：

1. `BF.SCANDUMP key 0` 返回 header 和 next iterator `1`。
2. 后续 `BF.SCANDUMP key iter` 返回不超过 16MiB 的 bitset chunk，并把 iterator 更新为“字节偏移语义”的下一个值。
3. `BF.LOADCHUNK key iter data` 必须使用 SCANDUMP 返回的 iterator 与对应 data。

Gemini 当前实现是：

1. cursor `0` 返回 header，next cursor 固定为 `1`。
2. cursor `1..N` 每次返回完整一层 bit array，next cursor 是层号加一。
3. `BF.LOADCHUNK` 把 cursor `1` 解释为 header，把 cursor `2..N+1` 解释为第 `cursor-2` 层数据。

**后果**：

- RedisBloom 产生的大 filter dump 无法按 Gemini 逻辑加载，尤其是单层超过 16MiB 时。
- Gemini 产生的 dump/AOF 对 RedisBloom 不可互操作，因为 cursor 不是 RedisBloom 的 byte-offset iterator。
- 当前 TCL 测试用独立 `load_iter` 自增，而不是使用 SCANDUMP 返回的 iterator，正好掩盖了这个问题。

**修复方向**：

- 复刻 RedisBloom 的 iterator 语义：header 后，chunk cursor 是全局 bit array 字节偏移 + 1。
- 实现最大 chunk size，至少采用 RedisBloom 的 16MiB 上限。
- `LOADCHUNK` 使用 `iter - bufLen` 定位目标 layer 与 offset，而不是 `cursor-2`。
- 测试必须用官方用法：`LOADCHUNK dst <SCANDUMP 返回的 cursor> <data>`。

## 3. P0：AOF rewrite 使用 Gemini 自定义 cursor，跨实现不可重放

**位置**：`modules/gemini-bloom/src/bloom_rdb.cc:AofRewriteBloom`。

当前 AOF rewrite 写出：

- header：`BF.LOADCHUNK key 1 <hdr>`
- 每层 bit array：`BF.LOADCHUNK key 2 <layer0>`、`BF.LOADCHUNK key 3 <layer1>` ……

RedisBloom AOF rewrite 则用 `SBChain_GetEncodedChunk` 返回的 iterator 写出 chunk。二者在大 filter、多 chunk 单层、迁移到 RedisBloom 时语义不同。

**后果**：

- Gemini 生成的 AOF 不应宣称可由 RedisBloom 加载。
- RedisBloom 生成的 AOF 也不应假定 Gemini 可加载。

**修复方向**：AOF rewrite 与 `BF.SCANDUMP` 共享同一套 RedisBloom cursor/chunk 编码。

## 4. P0：`BF.LOADCHUNK` 覆盖已有 key 的行为与 RedisBloom 不一致且危险

**位置**：`modules/gemini-bloom/src/bloom_commands.cc:CmdLoadchunk`。

RedisBloom 只在目标 key 为空且 `iter == 1` 时用 header 创建 filter；否则要求已有 key 是 BloomFilter，再加载 chunk。Gemini 在 `cursor == 1` 时会删除任何已有 key，然后尝试按 header 创建。

**后果**：

- 错误调用可删除字符串/列表/已有 Bloom key。
- 跨实现迁移脚本若重试 header chunk，Gemini 可能破坏已有目标。

**修复方向**：先解析 header 到临时对象，成功后再替换；并明确是否允许覆盖。若目标是 RedisBloom 兼容，应按 RedisBloom 行为：非空目标不应在 header 阶段被静默删除。

## 5. P1：`BF.INFO key <field>` 回复形态不兼容

**位置**：`modules/gemini-bloom/src/bloom_commands.cc:CmdInfo`。  
**Redis 文档要求**：`BF.INFO key [CAPACITY | SIZE | FILTERS | ITEMS | EXPANSION]`；RESP2 下请求单个可选字段返回单元素数组，RESP3 下完整 info 返回 map。

Gemini 对单字段直接返回整数或 null，例如 `BF.INFO k Capacity -> 1000`。这与文档和常见客户端期望不一致。

**后果**：

- 客户端按 RedisBloom 协议解析时会失败。
- 当前 TCL 测试把错误行为固定为期望值。

**修复方向**：

- 单字段返回 singleton array，或至少提供兼容开关。
- 完整 info 在 RESP3 下应使用 map-or-array API。

## 6. P1：模块身份不兼容：`GeminiBloom` vs `bf`

**位置**：`modules/gemini-bloom/src/redis_bloom_module.cc:RedisModule_Init`。

RedisBloom 的模块名是 `bf`，Gemini 使用 `GeminiBloom`。这不影响 `BF.*` 命令名，但会影响：

- `MODULE LIST` 识别；
- 运维脚本、健康检查、客户端 feature detection；
- 与 RedisBloom 模块版本号相关的兼容判断。

**修复方向**：明确选择：

- 若目标是 drop-in replacement，应考虑模块名/version 暴露兼容策略；
- 若目标是独立实现，应在 README 中声明“不保证 MODULE LIST 级别兼容”。

## 7. P1：配置接口不兼容

**位置**：`modules/gemini-bloom/src/bloom_config.cc`。  
**RedisBloom 参考**：`bf-error-rate`、`bf-initial-size`、`bf-expansion-factor` 是正式配置项；`ERROR_RATE`、`INITIAL_SIZE` 是 legacy 形式。

Gemini 只支持模块加载参数：

- `ERROR_RATE`
- `INITIAL_SIZE`
- `EXPANSION`

缺少：

- `bf-error-rate`
- `bf-initial-size`
- `bf-expansion-factor`
- Redis module config API 注册和 runtime config 支持
- RedisBloom 对 `bf-error-rate > 0.25` 的 cap 逻辑

**后果**：

- 使用现代 RedisBloom 配置名的部署脚本不能直接迁移。
- 错误率、容量、扩容范围与 RedisBloom 不一致。

**修复方向**：支持正式配置名，保留 legacy 名并打印 deprecation warning；实现范围校验和 error-rate cap。

## 8. P1：参数范围与 RedisBloom 不一致

**位置**：`bloom_config.cc`、`bloom_commands.cc:CmdReserve/ParseInsertOptions`。

RedisBloom 对 Bloom 默认容量和 expansion 有明确范围，例如初始容量最大 `1 << 30`、expansion 最大 `32768`。Gemini 只校验正数或非负数，并把 `long long` 转成 `uint64_t` 或 `unsigned`。

**后果**：

- 超大参数可触发分配、溢出或截断。
- `EXPANSION` 大于 `UINT_MAX` 时转成 `unsigned` 会截断。
- 与 RedisBloom 的错误信息和边界语义不同。

**修复方向**：复用 RedisBloom 的范围：capacity `[1, 1<<30]`，expansion `[0, 32768]`，error rate `(0,1)` 且 cap 到 `0.25` 或严格按文档选择。

## 9. P1：`BF.RESERVE NONSCALING EXPANSION n` 处理不兼容

**位置**：`modules/gemini-bloom/src/bloom_commands.cc:CmdReserve`。

RedisBloom 在 `NONSCALING` 与正数 `EXPANSION` 同时出现时返回错误。Gemini 允许二者同时出现，并把 filter 设为 fixed-size。

**后果**：

- 迁移测试对相同命令得到不同结果。
- 用户配置错误被静默吞掉。

**修复方向**：当 `NONSCALING` 与 `EXPANSION > 0` 同时出现时返回错误；`EXPANSION 0` 可等价于 NONSCALING。

## 10. P1：`BF.INSERT NOCREATE` 与 `CAPACITY/ERROR` 的组合不符合文档

**位置**：`modules/gemini-bloom/src/bloom_commands.cc:ParseInsertOptions`。

Redis 文档说明：`NOCREATE` 与 `CAPACITY` 或 `ERROR` 同时指定是错误，因为这些参数只在创建时生效。Gemini 没有检查这个组合。

**后果**：

- 兼容测试会失败。
- 用户以为 `CAPACITY/ERROR` 修改了已有 filter，实际被忽略。

**修复方向**：解析结束后增加组合校验：`NOCREATE && (capacitySpecified || errorSpecified)` 直接返回错误。

## 11. P1：NoRound 小容量 sizing 与 RedisBloom 不一致

**位置**：`modules/gemini-bloom/src/bloom_filter.cc:BloomLayer::Create`。

Gemini 对非 RawBits layer 使用 `max(rawBits, 1024)`，即最少 1024 bit。RedisBloom 的 NoRound 路径是按 `entries * bpe` 计算 bit，再按 64-bit 对齐到字节，最小不是 1024 bit。

**后果**：

- 小容量 filter 的 `BF.INFO Size`、RDB metadata、SCANDUMP metadata 与 RedisBloom 不一致。
- 默认容量 100 时，Gemini 会比 RedisBloom 使用更多内存。

**修复方向**：如果目标是 RedisBloom wire/data 兼容，应移除 1024 bit 下限，改成 RedisBloom 的 bit/byte 对齐规则。

## 12. P1：encver 0/1 的默认 flags 可能不兼容

**位置**：`modules/gemini-bloom/src/bloom_rdb.cc:ScalingBloomFilter::ReadFrom`。

Gemini 对 `encver < 2` 默认 `Use64Bit | NoRound`。RedisBloom 当前加载代码在 `encver < BF_MIN_OPTIONS_ENC` 时不会读取 options，结构体初始 options 为 0。

**后果**：

- 老 RDB 文件的 hash width 和 bit addressing 可能被错误解释，造成 false negative。
- 当前测试只覆盖 encver2，没有覆盖 encver0/1。

**修复方向**：用官方 RedisBloom 历史格式构造 fixture，确认 encver0/1 的真实 flags；不要用猜测默认值。

## 13. P2：命令 flag/ACL/COMMAND INFO 不完整

**位置**：`modules/gemini-bloom/src/bloom_commands.cc:RegisterBloomCommands`。

Gemini 只注册基础命令 flag，如 `readonly`、`write deny-oom`。RedisBloom 还注册 ACL category、command info，并对部分命令标记 `fast` 等。

**后果**：

- `COMMAND INFO`、ACL 分类、客户端命令发现能力与 RedisBloom 不一致。
- 安全策略按 `@bloom` 分类控制时不能直接替换。

**修复方向**：增加 command info 注册和 ACL category；对照目标 Redis/RedisBloom 版本确定每个命令的 flags。

## 14. P2：缺少 RedisBloom 现有的 `BF.DEBUG`

**位置**：命令注册表未包含 `BF.DEBUG`。

RedisBloom 源码中有 `BF.DEBUG`，用于输出 filter/link 级别信息。Gemini 未实现。

**后果**：

- 排障脚本和兼容测试缺一项。
- 对内部 layer 参数排查不方便。

**修复方向**：实现兼容的 `BF.DEBUG`，或在文档中明确不支持。

## 15. P2：没有 defrag callback

**位置**：`modules/gemini-bloom/src/redis_bloom_module.cc` 的 `RedisModuleTypeMethods`。

RedisBloom 为 Bloom 类型注册了 defrag callback。Gemini 只注册 rdb/load/save/aof/free/mem_usage。

**后果**：

- Redis active defrag 无法迁移模块内部指针。
- 长期运行、大量创建删除 filter 时内存碎片更难回收。

**修复方向**：实现 defrag 回调，重定位 `ScalingBloomFilter`、`layers_` 和每层 `bitArray_`。
