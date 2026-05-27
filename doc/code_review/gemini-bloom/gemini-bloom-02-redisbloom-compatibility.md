# Codex Review: gemini-bloom 与 RedisBloom 兼容性问题

基线：

- Redis 命令文档：`https://redis.io/docs/latest/commands/bf.reserve/`
- Redis 命令文档：`https://redis.io/docs/latest/commands/bf.scandump/`
- Redis 命令文档：`https://redis.io/docs/latest/commands/bf.loadchunk/`
- RedisBloom 参考实现：`src/rebloom.c`
- RedisBloom SBF 实现：`src/sb.c`
- RedisBloom bloom 实现：`deps/bloom/bloom.c`

## 高风险兼容性问题

### 1. 共享了 RedisBloom 数据类型名，但 SCANDUMP/LOADCHUNK 协议不兼容

位置：

- 数据类型名：`modules/gemini-bloom/src/redis_bloom_module.cc:23-26`
- SCANDUMP：`modules/gemini-bloom/src/bloom_commands.cc:453-468`
- LOADCHUNK：`modules/gemini-bloom/src/bloom_commands.cc:488-518`

当前模块注册了 `MBbloom--`，这意味着它声明自己能读写 RedisBloom Bloom 数据类型。但当前 `BF.SCANDUMP/BF.LOADCHUNK`
的 iterator 协议使用 layer index，而 RedisBloom 使用 byte offset。

后果：

- RedisBloom/Redis 8 产生的 chunks 不能可靠导入 gemini-bloom。
- gemini-bloom 产生的 chunks 不能按官方客户端流程导入 RedisBloom。
- AOF rewrite 发出的 `BF.LOADCHUNK` 序列也不是 RedisBloom 的 offset cursor 序列。

必须修复后才能声称 wire compatibility。

### 2. NoRound bit/byte 布局与 RedisBloom 不一致

位置：`modules/gemini-bloom/src/bloom_filter.cc:103-116`

RedisBloom 在 `BLOOM_OPT_NOROUND` 下会先算 bit 数，再把字节数向 8 字节对齐，并把实际 `bits` 设置为 `bytes * 8`。
当前实现只做 `(totalBits + 7) / 8`，没有 64-bit 对齐，也没有把 `totalBits_` 调整到 `dataSize_ * 8`。

后果：

- 同样的 capacity/error 下，bit array 大小和 modulo 范围不同。
- RedisBloom 的 SCANDUMP header 完整性检查要求 `bits == bytes * 8`；gemini-bloom 自己生成的小 filter header 可能被拒绝。
- 即使 hash seed 一样，bit 位置也会不同，导致跨实现 false negative。

例子：

- capacity=100、error 约 0.005 时，当前会使用原始 bit 数附近的 byte size。
- RedisBloom 会把 byte size 按 8 字节对齐，并用对齐后的 bit 数做 modulo。

### 3. `BF.SCANDUMP` 没有遵循“非零 cursor 表示可恢复 chunk”的官方流程

位置：`modules/gemini-bloom/src/bloom_commands.cc:453-468`

官方流程是把 `BF.SCANDUMP` 返回的 `(iter, data)` pair 传给 `BF.LOADCHUNK`。当前实现最后一个数据 chunk 返回
`iter=0`，这会被标准客户端解释为结束标记。

当前测试没有暴露问题，因为测试没有把返回的 iterator 原样用于 LOADCHUNK：

- `modules/gemini-bloom/tests/tcl/bloom_test.tcl:460-468`

### 4. `AofRewriteBloom` 使用自定义 cursor 序列

位置：`modules/gemini-bloom/src/bloom_rdb.cc:220-237`

RedisBloom AOF rewrite 使用：

- header: cursor 1
- data chunks: RedisBloom offset cursor
- 每个 chunk 最大 16MB

当前实现使用：

- header: cursor 1
- layer 1: cursor 2
- layer 2: cursor 3

后果：

- AOF 文件只能被 gemini-bloom 自己的非标准 LOADCHUNK 解释。
- mixed deployment、官方 RedisBloom -> gemini-bloom、gemini-bloom -> RedisBloom 的 AOF/replication 都不可靠。

### 5. `BF.RESERVE EXPANSION 0` 和 `NONSCALING + EXPANSION` 行为不兼容

位置：`modules/gemini-bloom/src/bloom_commands.cc:110-119`

RedisBloom BF 的 `EXPANSION` 必须大于等于 1，并且 `NONSCALING` 与 `EXPANSION` 不能同时使用。当前实现：

- 允许 `EXPANSION 0`。
- 允许同时指定 `NONSCALING EXPANSION 2`。
- 把 expansion 0 隐式映射成 fixed-size。

建议：

- BF 命令层严格拒绝 `EXPANSION < 1`。
- 同时出现 `NONSCALING` 和 `EXPANSION` 时返回错误。

### 6. RESP3 行为未对齐 RedisBloom

位置：

- `modules/gemini-bloom/src/bloom_commands.cc:82`
- `modules/gemini-bloom/src/bloom_commands.cc:160`
- `modules/gemini-bloom/src/bloom_commands.cc:310`
- `modules/gemini-bloom/src/bloom_commands.cc:340`

RedisBloom 参考实现对部分 BF boolean 结果在 RESP3 下返回 bool。当前实现始终返回 integer。

影响：

- RESP2 客户端大概率可用。
- RESP3 客户端的类型兼容性不完整。

建议：

- 增加 RESP3 模式测试。
- 按 RedisBloom 对 `ADD/MADD/EXISTS/MEXISTS` 的 RESP3 类型进行兼容。

### 7. `BF.DEBUG` 未实现

位置：`modules/gemini-bloom/src/bloom_commands.cc:529-540`

RedisBloom 参考实现注册了 `bf.debug`。当前 gemini-bloom 只注册公开 BF 命令，没有 debug 命令。

影响：

- 与 RedisBloom 的实际命令面不完全一致。
- 缺少对内部 layer/bytes/hashwidth/capacity/ratio 的调试入口。

建议：

- 如果目标是 RedisBloom BF 兼容，实现 `BF.DEBUG`。
- 如果目标只支持公开文档命令，应在 README/AGENTS 中明确“非完整 RedisBloom 兼容”。

### 8. 配置参数面不兼容

位置：`modules/gemini-bloom/src/bloom_config.cc:7-55`

当前模块 load args：

- `ERROR_RATE`
- `INITIAL_SIZE`
- `EXPANSION`

RedisBloom 参考实现支持：

- `error_rate`
- `initial_size`
- `cf_max_expansions`

问题：

- 当前额外引入 BF 默认 `EXPANSION`，并且允许 0，可能让 `BF.ADD` 自动创建 non-scaling filter。
- 没有 Redis 8 风格 `bf-default-*` 配置项。
- 兼容目标不清晰。

### 9. 错误文本和错误类型不兼容

位置：`modules/gemini-bloom/src/bloom_commands.cc`

典型差异：

- RedisBloom key already exists 常见文本是 `ERR item exists`，当前是 `ERR key already exists`。
- RedisBloom full non-scaling filter 是 `ERR non scaling filter is full`，当前是 `ERR reached capacity limit (non-scaling mode)`。
- `BF.INFO` unknown field RedisBloom 是 `Invalid information value`，当前是 `ERR unknown subcommand for BF.INFO`。

如果只看人类可读性，这些差异不大；如果要通过 RedisBloom 兼容测试或客户端依赖错误匹配，就会失败。

### 10. 当前兼容性声明过强

位置：

- `modules/gemini-bloom/src/bloom_filter.h:12-16`
- `modules/gemini-bloom/src/bloom_rdb.cc:24-27`
- `modules/gemini-bloom/src/redis_bloom_module.cc:23-26`

源码注释多次说 wire format 与 RedisBloom 兼容。但从 iterator、NoRound bit layout、RESP3、错误文本、AOF rewrite 看，这个声明还没有证据支持。

建议：

- 在修复前降低注释措辞，改为“intended compatibility”。
- 增加 official RedisBloom golden corpus：RDB、AOF、SCANDUMP/LOADCHUNK、RESP2/RESP3 reply trace。
