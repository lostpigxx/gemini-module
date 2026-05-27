# gemini-bloom 功能设计问题

审查基准：`lostpigxx/gemini-module` 的 `main` 分支，重点文件为 `modules/gemini-bloom/**`、根 `CMakeLists.txt`、`README.md`、`include/mock_redismodule_io.h`。对照对象为 `RedisBloom/RedisBloom` 当前公开实现与 redis.io 命令文档。

严重等级：`P0` = 数据损坏/未定义行为/协议级不兼容；`P1` = 生产可用性或兼容性重大问题；`P2` = 功能、性能、维护性明显缺陷；`P3` = 较小但应修复的问题。


## 1. P1：兼容目标不清晰，设计上同时像“替代 RedisBloom”和“独立模块”

**位置**：`README.md`、源码注释、`redis_bloom_module.cc`。

项目使用 `BF.*` 命令名、RedisBloom 数据类型名 `MBbloom--`、RedisBloom flag 数值，并在注释中多次声明 wire/RDB 兼容；但模块名是 `GeminiBloom`，SCANDUMP/LOADCHUNK cursor 协议、BF.INFO 单字段返回、配置接口都不完全兼容。

**问题本质**：兼容性是产品级契约，不是局部实现细节。当前没有文档说明哪些兼容、哪些不兼容，用户会自然假设它是 RedisBloom drop-in replacement。

**建议**：

- 在 README 增加兼容矩阵：命令、返回值、RESP2/RESP3、RDB、AOF、SCANDUMP/LOADCHUNK、配置项、MODULE LIST、ACL。
- 若目标是替代 RedisBloom，优先修复协议不兼容；若不是，避免使用会误导的“interoperable”注释。

## 2. P1：不可信输入边界没有设计成独立验证层

**位置**：`bloom_rdb.cc`、`bloom_commands.cc:CmdLoadchunk`。

RDB 和 LOADCHUNK 都是外部输入。当前设计把“解析、对象构建、写入 Redis key、bit array copy”混在一起，缺少先验验证层。

**后果**：

- header 一旦被解析，可能已经分配对象并设置到 key。
- `LOADCHUNK cursor=1` 甚至先删除旧 key 再解析新 header。
- 校验逻辑散落，导致 `RDB` 和 `SCANDUMP` 的安全边界不一致。

**建议**：

- 抽出 `DecodedFilterSpec`：只表示已验证的 header/meta。
- 先完整校验 spec，再构建 `ScalingBloomFilter`。
- `LOADCHUNK` 用临时对象或两阶段 commit，避免坏输入破坏旧数据。

## 3. P1：批量写入的 partial write 语义没有定义

**位置**：`CmdMadd`、`CmdInsert`、`PutAndReply`。

当 NONSCALING filter 中途满了，命令已经插入前面的 item，并把 error 作为数组元素写出。这个行为没有在 README 或命令文档中定义。

**设计问题**：

- 如果追求 RedisBloom 兼容，应复刻 RedisBloom 的中途错误行为和回复长度。
- 如果追求清晰 API，应明确“批量命令非原子，可能部分成功”。
- 如果追求强语义，应先容量预检，保证全成功或全失败。

**建议**：定义一种语义并写入测试。默认建议对齐 RedisBloom。

## 4. P1：`BF.CARD/TotalItems` 的“精确性”没有文档说明

**位置**：`ScalingBloomFilter::Put`、`CmdCard`。

BloomFilter 无法可靠地区分“真实重复”和“假阳性”。当前 `Put` 只要 `IsDuplicate` 返回 true 就不增加 `totalItems_`，所以 `BF.CARD` 表示“被认为新插入的次数”，不是严格 distinct count。

**影响**：

- 用户可能把 `BF.CARD` 当成精确基数。
- 高 false positive rate 下，`CARD` 会低估实际唯一 item 数。

**建议**：

- 文档中明确 `BF.CARD` 的语义与误差来源。
- 如果目标是 RedisBloom 兼容，应确认命令文档和返回行为一致。

## 5. P1：固定大小/扩容设计存在双重状态，容易产生矛盾

**位置**：`AllocFilter`、`ScalingBloomFilter` flags、`ExpansionFactor`。

`AllocFilter` 中 `fixed || expansion == 0` 会设置 FixedSize，但传给构造函数的 expansion factor 是 `expansion > 0 ? expansion : 2`。这意味着 fixed filter 内部仍保存 expansion factor 2，只是 `BF.INFO Expansion` 返回 null。

**问题**：

- 状态模型不单一：到底是 expansion=0 表示 non-scaling，还是 flag 表示 non-scaling？
- RDB/SCANDUMP 中仍会写 expansion factor，迁移后可能产生歧义。

**建议**：

- 内部统一：`FixedSize` 时 expansionFactor 可固定为 0，或保留原值但文档明确“ignored”。
- 校验不允许 `FixedSize && expansionFactor > 0` 出现在外部 wire header，除非为了 RedisBloom 兼容有明确原因。

## 6. P2：功能面只覆盖 BF 子集，缺少排障与迁移配套能力

当前实现注册：`BF.RESERVE`、`BF.ADD`、`BF.MADD`、`BF.INSERT`、`BF.EXISTS`、`BF.MEXISTS`、`BF.INFO`、`BF.CARD`、`BF.SCANDUMP`、`BF.LOADCHUNK`。

缺口包括：

- `BF.DEBUG`：RedisBloom 源码提供，生产排障很有用。
- 命令元信息：ACL category、COMMAND INFO、RESP3 map。
- 兼容性工具：dump header inspect、format version inspect、RedisBloom fixture roundtrip。
- 运行时配置：现代 Redis module config API。

**建议**：把“最低可用 BF 子集”和“RedisBloom 兼容子集”拆开规划，避免半兼容状态。

## 7. P2：核心数据结构与 Redis 命令层耦合过重

**位置**：`bloom_commands.cc` 直接创建和操作 `ScalingBloomFilter`；`bloom_rdb.cc` 同时包含 wire/RDB/module callback。

**问题**：

- 命令行为难以单测，只能靠 TCL 启 Redis。
- RDB、SCANDUMP、AOF 的格式逻辑重复但不共享统一抽象。
- 错误码、错误消息、Redis reply 直接散在业务逻辑里。

**建议**：

- 分三层：`core`、`format`、`redis_adapter`。
- `core` 返回 typed enum，例如 `PutResult::Inserted/Duplicate/Full/Invalid`。
- `redis_adapter` 只负责参数解析、reply 和 key 生命周期。

## 8. P2：没有明确的大对象策略

**位置**：hash 输入长度、SCANDUMP chunk、AOF rewrite、RDB load。

当前代码对超长 item、超大 filter、超大 dump chunk 都没有统一策略。

**具体问题**：

- hash 对超长 item 截断。
- SCANDUMP 一次返回整层 bit array。
- AOF rewrite 一次 emit 整层 bit array。
- RDB load 对巨大 `numLayers/dataSize` 没有一致上限。

**建议**：

- 规定最大 item 长度，超过则返回错误。
- SCANDUMP/AOF 按固定最大 chunk 输出。
- RDB/LOADCHUNK 对总内存有显式上限或基于 Redis OOM 策略安全失败。

## 9. P2：文档没有覆盖命令行为和限制

README 只说明构建、加载、测试。缺少：

- 每个命令的语法、返回值和错误。
- 参数范围。
- 是否兼容 RedisBloom。
- RDB/AOF/SCANDUMP 兼容边界。
- RESP2/RESP3 差异。
- NONSCALING、EXPANSION、NOCREATE 的组合规则。

**建议**：新增 `doc/gemini-bloom.md`，并把测试用例与文档示例保持一致。

## 10. P3：错误消息没有规范化

不同命令返回：

- `ERR allocation failure`
- `ERR reached capacity limit (non-scaling mode)`
- `ERR expected a positive capacity value`
- `ERR unrecognized option`
- `ERR unknown subcommand for BF.INFO`

**问题**：不兼容 RedisBloom 文案时，客户端测试和用户排障都会受影响。

**建议**：建立 error code/message 表。若目标是兼容 RedisBloom，就使用 RedisBloom 文案；若目标是独立实现，也应保证同类错误风格一致。
