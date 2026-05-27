# Codex Review: gemini-bloom 功能设计问题

## 1. 兼容目标没有产品级定义

当前实现同时做了两件互相冲突的事：

- 注册 RedisBloom 数据类型名 `MBbloom--`，暗示 RDB/AOF/SCANDUMP 与 RedisBloom 兼容。
- 在 SCANDUMP/LOADCHUNK、配置参数、错误文本和 RESP3 类型上又使用了自定义行为。

位置：

- `modules/gemini-bloom/src/redis_bloom_module.cc:23-26`
- `modules/gemini-bloom/src/bloom_commands.cc:418-519`

建议先明确目标：

- 如果目标是 RedisBloom BF 兼容：必须对齐命令语义、wire format、RDB/AOF、RESP2/RESP3、错误行为。
- 如果目标是独立 Bloom module：不要使用 RedisBloom 的 module type name，也不要在注释中声明互操作。

## 2. `LOADCHUNK` 缺少恢复事务/状态机设计

当前 `LOADCHUNK` 每次命令都是直接修改 key：

- header chunk 创建 filter。
- 后续 chunk 直接 memcpy 到对应 layer。
- 没有记录 restore 是否完成。
- header-only filter 立即对外可见。

风险：

- 恢复中断后，用户能看到一个 `BF.CARD` 有值但实际 bit array 未加载的 filter。
- 错误 chunk 可能留下半恢复状态。
- 无法防止 chunk 重放、乱序、缺失。

建议：

- 设计恢复状态机：header -> partial chunks -> complete。
- 完成前禁止 `ADD/EXISTS/INFO` 使用半恢复对象，或使用临时 key/临时对象。
- 至少在 `LOADCHUNK` 失败时删除新建的 partial filter。

## 3. 小容量 filter 被强制放大到 1024 bit，语义和内存行为不透明

位置：`modules/gemini-bloom/src/bloom_filter.cc:103-106`

当前所有普通 BloomLayer 至少分配 1024 bit。用户请求 `BF.RESERVE k 0.01 1` 时，实际分配远大于理论需要。

问题：

- `capacity` 是用户理解的容量，不是内存下限；强制最小 bit 数应该在文档和 INFO 中可解释。
- 与 RedisBloom 的 small filter 行为不一致。
- 对大量小 key 场景内存浪费明显。

建议：

- 删除 1024 bit 下限，或变成显式配置。
- 如果保留，需要在设计文档中说明理由和兼容影响。

## 4. `EXPANSION 0` 被设计成 fixed-size 的隐藏别名

位置：`modules/gemini-bloom/src/bloom_commands.cc:17-23`

`EXPANSION 0` 被解释成 fixed-size，但用户没有指定 `NONSCALING`。这会让配置和命令语义变得隐式。

建议：

- 只用 `NONSCALING` 表示 fixed-size。
- `EXPANSION` 只接受正整数。

## 5. 缺少可观察性命令

当前只有 `BF.INFO`，没有 RedisBloom 的 `BF.DEBUG`。

缺失影响：

- 无法直接查看每个 layer 的 bytes、bits、hashes、hashwidth、capacity、size、ratio。
- 性能和兼容性问题只能靠外部工具猜测。

建议：

- 实现 `BF.DEBUG` 或内部-only debug 命令。
- 输出应兼容 RedisBloom，便于差异测试。

## 6. 配置设计与模块边界不清

位置：`modules/gemini-bloom/src/bloom_config.cc:7-55`

当前 `BloomConfigLoad` 是模块 load args parser，但：

- 参数名没有集中写入 README。
- 没有 RedisBloom/Redis 8 配置兼容矩阵。
- `defaultExpansion` 影响自动创建 filter，但 `BF.RESERVE` 的默认 expansion 又来自它；这和 RedisBloom 固定默认 2 的行为不同。

建议：

- 写明“支持的 module args”和“RedisBloom 不兼容项”。
- 对默认 expansion 做范围校验，不允许 0。

## 7. 只实现 BF 子集时，模块命名容易误导

构建产物叫 `redis_bloom.so`，module name 是 `GeminiBloom`，命令只包含 BF.*。

如果用户把它当 RedisBloom 替代品，会缺少：

- CF.*
- CMS.*
- TOPK.*
- TDIGEST.*
- BF.DEBUG

建议：

- README 中明确“只实现 Bloom Filter BF.* 子集”。
- 如果未来要兼容 RedisBloom，应有命令覆盖表和 staged roadmap。

## 8. `BF.INFO Size` 混合了逻辑内存和 C++ 对象估算

位置：`modules/gemini-bloom/src/sb_chain.cc:131-137`

`Size` 当前包含 `ScalingBloomFilter`、`FilterLayer` 和 bit arrays 的估算值，但不包含 layer storage 的预留容量，也不一定等同 RedisBloom 的 `BFMemUsage`。

建议：

- 明确 `Size` 是 Redis allocator 实际占用估算还是 RedisBloom 兼容估算。
- 若追求兼容，按 RedisBloom 的结构统计。
- 若追求实际内存，统计 `layerCapacity_` 和 allocator overhead 近似值。

## 9. 没有跨模块共享策略

`gemini-json` 和 `gemini-bloom` 都有自己的 `rm_alloc.h`，Redis Module API wrapper 也散落在各模块中。

建议：

- 抽出共享 allocator/API wrapper 到 `include/` 或 `modules/common/`。
- 统一 module test mock、RDB helper 和 command parsing 风格。

## 10. 没有兼容性等级文档

建议新增一份明确的兼容等级：

- Command syntax compatibility
- Reply compatibility
- Error compatibility
- RDB compatibility
- SCANDUMP/LOADCHUNK compatibility
- AOF compatibility
- RESP3 compatibility
- Official RedisBloom golden corpus pass/fail

没有这张表，后续很容易把“基本命令可用”误认为“RedisBloom 兼容”。
