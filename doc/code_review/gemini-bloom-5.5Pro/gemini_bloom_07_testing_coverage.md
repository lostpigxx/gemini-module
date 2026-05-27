# gemini-bloom 测试场景、代码覆盖与遗漏点

审查基准：`lostpigxx/gemini-module` 的 `main` 分支，重点文件为 `modules/gemini-bloom/**`、根 `CMakeLists.txt`、`README.md`、`include/mock_redismodule_io.h`。对照对象为 `RedisBloom/RedisBloom` 当前公开实现与 redis.io 命令文档。

严重等级：`P0` = 数据损坏/未定义行为/协议级不兼容；`P1` = 生产可用性或兼容性重大问题；`P2` = 功能、性能、维护性明显缺陷；`P3` = 较小但应修复的问题。


## 1. 当前测试资产概览

| 测试文件 | 已覆盖 | 主要缺口 |
|---|---|---|
| `tests/bloom_filter_test.cc` | hash determinism、layer 创建/移动、插入/查询、粗略 false positive、power-of-two、flag 运算 | RedisBloom golden hash、RawBits、32-bit mode、极端参数、损坏 metadata、超长 item |
| `tests/sb_chain_test.cc` | 构造、Put/Contains、自动扩容、fixed overflow、TotalCapacity、BytesUsed | layer realloc UB、FromRdbShell UB、expansion=0/1 长期行为、溢出、倒序扫描性能 |
| `tests/bloom_rdb_test.cc` | 自编码自解码 RDB、encver2、wire header roundtrip、简单 corrupt header、重复 roundtrip | RedisBloom fixture、encver0/1、短 blob、dataSize mismatch、trailing bytes、IO error、恶意 log2/hashCount |
| `tests/tcl/bloom_test.tcl` | 真实 Redis 启动下的基础命令、部分 wrongtype、SCANDUMP/LOADCHUNK 自 roundtrip、RDB restart | RESP3、AOF rewrite、RedisBloom 互操作、LOADCHUNK 官方 cursor、全部 wrongtype、配置、ACL、COMMAND INFO、大 chunk |

## 2. P0：没有 RedisBloom 官方 fixture，无法证明兼容性

当前 RDB 和 wire 测试都是“Gemini 写 → Gemini 读”。这种测试只能证明自洽，不能证明与 RedisBloom 兼容。

**必须新增**：

1. 用 RedisBloom 生成 RDB，Gemini 加载并验证所有 item。
2. 用 Gemini 生成 RDB，RedisBloom 加载并验证所有 item。
3. 用 RedisBloom `BF.SCANDUMP` 生成 chunk 序列，Gemini `BF.LOADCHUNK` 加载。
4. 用 Gemini `BF.SCANDUMP` 生成 chunk 序列，RedisBloom `BF.LOADCHUNK` 加载。
5. 对比 `BF.INFO`、`BF.CARD`、`BF.EXISTS`、`BF.MEXISTS` 的返回形态。

**当前风险**：最严重的 SCANDUMP/LOADCHUNK cursor 不兼容无法被现有测试发现。

## 3. P0：TCL 的 SCANDUMP/LOADCHUNK 测试用法错误，掩盖协议问题

**位置**：`tests/tcl/bloom_test.tcl` 的 SCANDUMP/LOADCHUNK roundtrip。

测试中：

- `scan_iter` 使用 SCANDUMP 返回的 next cursor；
- 但 `LOADCHUNK` 使用独立的 `load_iter` 从 1 自增。

RedisBloom 文档语义是：`LOADCHUNK` 的 iterator 必须与 SCANDUMP 返回的数据关联。也就是说应使用 dump 流中对应的 iterator，而不是自己生成层号。

**后果**：

- Gemini 自定义 layer cursor 协议被测试认可。
- RedisBloom 兼容性测试会失败。

**修复测试**：

- 保存每次 SCANDUMP 的 `(iter, data)`，其中 header 的 LOADCHUNK iter 为 1，后续 chunk 用 SCANDUMP 返回的 iterator 语义。
- 增加大于 16MiB 的单层 filter，强制产生多 chunk。

## 4. P0：没有测试 C++ 对象生命周期 UB

**缺口位置**：`AppendLayer`、`FromRdbShell`、`SetLayer`。

现有测试没有专门触发：

- 多次 layer array 扩容；
- RDB/SCANDUMP 反序列化中途失败后析构；
- UBSAN 下对未构造对象赋值/析构。

**必须新增**：

- `expansion=1, capacity=1`，插入到层数超过 4、8、16，触发多次 layer realloc。
- `DeserializeHeader` 构造 3 层，其中第 2 层 dataSize 触发分配失败或 invalid，再检查析构安全。
- CI 增加 UBSAN，至少本地 target：`bloom_ubsan_test`。

## 5. P0：没有损坏 RDB blob 测试

`BloomLayer::ReadFrom` 对短 blob、长 blob、长度不匹配都静默处理。现有 mock 也会掩盖短读。

**必须新增用例**：

| 场景 | 期望 |
|---|---|
| bit array blob 比 `ceil(bits/8)` 短 1 字节 | RDB load 失败 |
| bit array blob 比 expected 长 | RDB load 失败或严格定义兼容策略 |
| `totalBits=0` | load 失败 |
| `log2Bits=64` | load 失败 |
| `hashCount=0` 且非 RawBits | load 失败 |
| `fpRate<=0` 或 `>=1` | load 失败 |
| `bitsPerEntry` 与 fp/hash 不一致 | load 失败 |

## 6. P1：`mock_redismodule_io.h` 会掩盖 IO 错误

**位置**：`include/mock_redismodule_io.h`。

`ReadBytes` 返回 bool，但 `Mock_LoadUnsigned/Double/StringBuffer` 忽略返回值。短读时会返回 0 或部分填充 buffer。

**后果**：

- 损坏 RDB 测试可能误通过。
- 生产 IO error 路径没有被模拟。

**修复测试基础设施**：

- `MockRdbStream` 增加 `bool io_error`。
- 所有 Load 函数在短读时设置 error。
- `RdbReader` 可检查 error。
- 单测断言 corrupted stream 必须失败。

## 7. P1：BF.INFO 测试把非兼容行为固定成期望

**位置**：`tests/tcl/bloom_test.tcl`。

测试期望：

- `BF.INFO reserve_basic Capacity` 返回 scalar `1000`。

RedisBloom 文档要求 RESP2 下单字段返回 singleton array。现有测试实际上会阻止修复兼容性。

**修复**：

- 按 RESP2/RESP3 分别写期望。
- 对 RedisBloom 和 Gemini 同跑同一套兼容测试。

## 8. P1：wrongtype 覆盖不完整

现有 TCL wrongtype 只覆盖：

- `BF.ADD`
- `BF.EXISTS`
- `BF.INFO`

缺少：

- `BF.MADD`
- `BF.MEXISTS`
- `BF.INSERT`
- `BF.CARD`
- `BF.SCANDUMP`
- `BF.LOADCHUNK`
- `BF.RESERVE` on wrong module type vs string key

**特别必须测**：`BF.MEXISTS string_key a b`，当前实现会返回数组内多个 WRONGTYPE error。

## 9. P1：没有 RESP3 测试

RedisBloom 对布尔、map/array 返回在 RESP2/RESP3 下可能不同。当前 TCL client 只按 RESP2 简化解析。

**必须新增**：

- `HELLO 3` 后测试 `BF.EXISTS`、`BF.MEXISTS` 是否返回 bool。
- `BF.INFO` 完整返回是否是 map。
- 单字段返回是否符合文档。

## 10. P1：没有 AOF rewrite / BGREWRITEAOF 测试

当前只有 RDB `BGSAVE + restart`。但 `AofRewriteBloom` 正是兼容性问题高发位置。

**必须新增**：

1. 开启 appendonly。
2. 写入多层 filter。
3. 执行 `BGREWRITEAOF`。
4. 重启 Redis，加载 AOF。
5. 验证所有 item。
6. 对大于 16MiB 的 bit array 验证 chunk cursor。
7. 用 RedisBloom 加载 Gemini 生成的 AOF，反向也测。

## 11. P1：没有模块加载配置测试

**位置**：`bloom_config.cc`。

缺少测试：

- legacy `ERROR_RATE`、`INITIAL_SIZE`、`EXPANSION` 成功。
- 正式 `bf-error-rate`、`bf-initial-size`、`bf-expansion-factor` 当前不支持，应明确失败或修复后成功。
- expansion 超过 `UINT_MAX` 或 RedisBloom max。
- error rate > 0.25 的 RedisBloom cap 行为。
- 参数个数奇偶错误。

## 12. P1：没有命令参数组合测试

缺少：

- `BF.RESERVE k 0.01 100 NONSCALING EXPANSION 2` 应按兼容目标报错。
- `BF.RESERVE k 0.01 100 EXPANSION 0` 应等价 NONSCALING。
- `BF.INSERT k NOCREATE ERROR 0.01 ITEMS a` 应报错。
- `BF.INSERT k NOCREATE CAPACITY 10 ITEMS a` 应报错。
- option 重复出现：`ERROR 0.1 ERROR 0.01`。
- `ITEMS` 后再出现 option-looking token 是否应当作为 item。

## 13. P1：没有大对象和边界性能测试

现有“very long item”只有 10,000 字节，远小于 hash 的 `INT_MAX` 截断风险，也不能覆盖 Redis 大 bulk string 场景。

建议至少增加：

- item size 接近配置上限。
- 超过允许上限时返回错误。
- 单层 bit array > 16MiB 的 SCANDUMP。
- 多层 header 接近 `kMaxLayers`。
- `capacity=1`、`expansion=1` 长时间插入。

## 14. P2：false positive 测试太弱且不可诊断

`bloom_filter_test.cc` 对 10,000 插入、100,000 查询断言 FP < 0.03。问题：

- 没有固定随机种子，因为使用顺序字符串，分布有限。
- 没有 RedisBloom 对照。
- 没有统计置信区间。
- 没有测试不同 fp rates、不同 capacity、不同 layer count。

**建议**：加入 property/statistical tests：

- 对多组 `(capacity, fpRate, flags)` 运行。
- 记录实际 FP 分布。
- 与 RedisBloom golden 输出对比 bit positions 或 membership。

## 15. P2：hash 测试没有 golden vector

现有 hash 测试只验证 deterministic 和不同输入 hash 不同。必须新增 RedisBloom golden vector：

- `""`
- `"hello"`
- binary string with ` `
- unicode bytes
- 长字符串
- 32-bit mode
- 64-bit mode

目标是证明 bit positions 与 RedisBloom 完全一致。

## 16. P2：没有 RawBits/`BLOOM_OPT_ENTS_IS_BITS` 测试

代码声明 `RawBits` flag，但没有测试。当前实现会导致所有查询返回 true。

**建议**：

- 若不支持 RawBits：增加反序列化拒绝测试。
- 若支持 RawBits：增加与 RedisBloom `BLOOM_OPT_ENTS_IS_BITS` 兼容测试。

## 17. P2：没有 memory usage 测试

现有 `BytesUsed` 只断言大于 `sizeof(ScalingBloomFilter)`。缺少：

- 初始 layerCapacity=4 时是否统计完整数组。
- 多次扩容后是否统计真实分配容量。
- 与 Redis `MEMORY USAGE` 的集成测试。

## 18. P2：RDB persistence 测试 flakiness

TCL 测试执行 `BGSAVE` 后固定 `after 2000`。这在慢机器或 CI 上可能不稳定。

**修复**：

- 轮询 `LASTSAVE` 或 `INFO persistence`，等待 `rdb_bgsave_in_progress:0`。
- 使用唯一临时目录，而不是固定 `/tmp/bloom_tcl_test.rdb`。

## 19. P2：集成测试使用固定 `/tmp` 文件名，不适合并行 CI

**位置**：`tests/tcl/bloom_test.tcl:start_redis/stop_redis`。

固定：

- `/tmp/bloom_tcl_test.log`
- `/tmp/bloom_tcl_test.rdb`

**后果**：并行执行或失败残留会互相污染。

**修复**：每次测试创建唯一 temp dir，例如 `/tmp/gemini-bloom-test-$pid-$port/`。

## 20. P2：没有 coverage/CI 强制门禁

CMake 只有在 `GTest_FOUND` 时创建测试 target；否则测试静默不存在。也没有 `add_test()`、coverage target、CI 配置。

**建议**：

- `BUILD_TESTING=ON` 时 GTest 缺失应失败或自动 FetchContent。
- `enable_testing()` + `add_test()`。
- CI matrix：GCC/Clang、ASAN、UBSAN、Redis 7.x/8.x、RESP2/RESP3。
- 生成 line/function coverage 报告，重点要求 `bloom_commands.cc`、`bloom_rdb.cc` 的错误路径覆盖。

## 21. P3：测试代码自身也应二进制安全

TCL RESP client 用 string/list 处理 SCANDUMP 二进制 payload。Tcl 通常能处理 NUL，但 list 展示、比较和参数展开容易踩坑。

**建议**：

- 对二进制 chunk 使用 bytearray-safe 处理。
- 增加包含 ` `、`
`、高位字节的 item 和 dump roundtrip。

## 22. 建议新增的最小兼容测试矩阵

| 维度 | 必测项 |
|---|---|
| Redis 协议 | RESP2、RESP3 |
| 持久化 | RDB save/load、AOF rewrite/reload |
| 跨实现 | RedisBloom→Gemini、Gemini→RedisBloom |
| 参数 | ERROR、CAPACITY、EXPANSION、NONSCALING、NOCREATE、ITEMS 组合 |
| 错误 | wrongtype、missing key、bad cursor、bad chunk、bad header、OOM mock |
| 大小 | small capacity、default capacity、large >16MiB layer、many layers |
| 安全 | truncated RDB、extra header bytes、invalid flags、invalid log2/hashCount |
| 性能 | newest duplicate hit、oldest duplicate hit、多层 contains、scandump chunk |
