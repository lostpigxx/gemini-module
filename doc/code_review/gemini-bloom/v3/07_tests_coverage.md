# 07 — 测试覆盖问题

本文件按“场景覆盖、代码覆盖、协议覆盖、异常覆盖、性能覆盖、可靠性覆盖”拆分。当前仓库已有 GTest 和 TCL 集成测试，数量不少，但主要验证自洽行为，缺少 RedisBloom 金样本、恶意输入、RESP3、运行时错误注入和覆盖率度量。

## TEST-01：没有 RedisBloom official golden corpus，无法证明兼容

**级别：P1**

### 现状

当前 RDB/wire 测试都是：

```text
gemini create -> gemini serialize -> gemini deserialize -> gemini query
```

TCL 也是：

```text
gemini BF.SCANDUMP -> gemini BF.LOADCHUNK
```

### 缺口

缺少：

```text
RedisBloom BF.SCANDUMP -> gemini BF.LOADCHUNK
gemini BF.SCANDUMP -> RedisBloom BF.LOADCHUNK
RedisBloom RDB -> gemini RDB load
gemini RDB -> RedisBloom RDB load
RedisBloom AOF -> gemini replay
gemini AOF -> RedisBloom replay
```

### 建议

新增 `tests/compat/redisbloom_golden/`，保存 official RedisBloom 生成的 small/large/single-layer/multi-layer dumps、NoRound/rounded/Fixed/Expansion 0、RESP2/RESP3 command replies、binary item fixtures、expected metadata JSON。

---

## TEST-02：SCANDUMP/LOADCHUNK 没测官方 byte-offset cursor

**级别：P1**

TCL 测试使用“把返回 iterator 直接传给 LOADCHUNK”，但由于 dump 和 load 都是 gemini 实现，自洽通过不代表协议兼容。

### 缺口

必须测试 RedisBloom 例子级行为：

```text
BF.SCANDUMP bf 0 -> iterator 1
BF.SCANDUMP bf 1 -> iterator = 1 + chunk_len
```

以及：chunk 小于层大小、chunk 跨多层前后边界、大于 16MB 层被分块、`iter < bufLen` 被拒绝、chunk 写入 offset 正确。

---

## TEST-03：NoRound bit/byte 对齐没有任何 exact test

**级别：P1**

当前 false-positive/no-false-negative 测试只验证 gemini 自己 hash 和 bit layout 自洽。没有验证：

- `bytes` 是否按 RedisBloom 对齐到 8 字节
- `bits == bytes * 8`
- probe modulo 是否使用对齐后的 bits
- official header 中 `bytes/bits` 是否 exact match

### 建议

对 `capacity=10,error=0.1`、`capacity=100,error=0.01`、`capacity=1000,error=0.001` 做 exact vector，校验 bpe、hashes、bytes、bits、n2/log2Bits、entries/capacity、固定 item 的 bit positions。

---

## TEST-04：RDB/wire 恶意 metadata 测试不足

**级别：P0/P1**

### 当前已有

已有测试覆盖 truncated itemCount、log2Bits >= 64、numLayers > kMaxLayers、zero layers、truncated header。

### 仍缺少

| 场景 | 期望 |
|---|---|
| `log2Bits > 0` 但 `totalBits < 1<<log2Bits` | reject |
| `log2Bits > 0` 但 `totalBits != 1<<log2Bits` | reject |
| `dataSize > expectedSize` | reject |
| `dataSize == 0` | reject |
| `hashCount == 0` | reject |
| `hashCount` 巨大 | reject |
| `fpRate = NaN/Inf/0/1/-x` | reject |
| `bitsPerEntry = NaN/Inf/0/-x` | reject |
| `capacity == 0` | reject |
| `itemCount > capacity` | reject |
| `totalItems != sum(itemCount)` | reject |
| unknown flags | reject |
| non-fixed `expansionFactor == 0` in RDB | reject |
| header trailing bytes | reject |
| header length exact mismatch | reject |
| allocation failure after partial layer construction | no UB/leak |

---

## TEST-05：没有 OOM/failure injection 测试

**级别：P1/P2**

当前测试无法模拟 `RMAlloc` fail、`RMCalloc` fail、`RedisModule_ModuleTypeSetValue` fail、`RedisModule_LoadStringBuffer` fail after partial reads、`RedisModule_SaveStringBuffer` fail、AOF rewrite header allocation fail。

### 建议

测试版 allocator 增加 failure counter：

```cpp
SetAllocFailAfter(n);
```

覆盖 AppendLayer allocation fail、BloomLayer bitArray allocation fail、FromRdbShell layer allocation fail、DeserializeHeader layer i allocation fail、AofRewriteBloom header allocation fail。同时开启 ASAN/UBSAN 检查 destructor path。

---

## TEST-06：命令 parser 没有单元测试，主要靠 TCL 黑盒覆盖

**级别：P2**

`ParseInsertOptions()`、`CmdReserve()` option parser 没有直接单测。TCL 覆盖了 happy path 和少量错误，但没有完整矩阵。

### 建议

把 parser 从 RedisModuleString 抽象为 testable token list：

```cpp
Expected<InsertOptions, ParseError> ParseInsertOptions(TokenSpan);
```

单测覆盖 option 顺序任意、重复 option、missing value、`ITEMS` missing、`ITEMS` empty、`NOCREATE + CAPACITY`、`NOCREATE + ERROR`、`NONSCALING + EXPANSION`、`EXPANSION 0`、`EXPANSION > UINT_MAX`、non-numeric capacity/error/expansion、unknown option before/after ITEMS。

---

## TEST-07：BF.INFO RESP2/RESP3 返回形状未覆盖

**级别：P1**

当前 TCL 测试期望：

```text
BF.INFO reserve_basic Capacity -> 1000
```

这与 Redis docs RESP2 singleton array 不一致。

### 缺口

- RESP2 full array exact labels/order
- RESP2 single field singleton array
- RESP3 full map
- RESP3 single field shape
- non-scaling expansion null shape
- wrong key/missing key error shape

### 建议

TCL 或 Python RESP client 明确发送 `HELLO 2`、`HELLO 3`，并检查 raw RESP type byte，而不是只检查 Tcl list/string 值。

---

## TEST-08：多 item 命令 full/error 语义覆盖不足

**级别：P2**

当前有 non-scaling overflow 测试，但只检查“会 reject”，没有检查完整返回结构和状态。

### 缺口

- `BF.MADD` fixed filter 部分满：返回数组长度、错误元素位置、后续 item 是否处理
- `BF.INSERT` fixed filter 部分满
- full 后 `BF.CARD` 是否等于实际成功插入数
- error 后 AOF/RDB 是否保留一致状态
- retry 同一命令后的结果

---

## TEST-09：没有 RESP raw binary 安全测试

**级别：P2**

TCL client 通过 Tcl string/list 传递 SCANDUMP chunk。对于 arbitrary binary blob，Tcl list 语义可能掩盖问题。

### 缺口

header/chunk 包含 `\0`、`{}`、spaces、backslashes、`\r\n`、大 chunk、随机 bytes。

### 建议

使用 Python redis client 或自写 raw RESP binary harness，避免 Tcl list/string 表示影响二进制数据。

---

## TEST-10：没有 Redis key type/ModuleType API 边界测试矩阵

**级别：P2**

当前 TCL 覆盖 string key 上的若干 WRONGTYPE。

### 缺口

- list/hash/set/zset/stream key
- empty key
- same module type but corrupt value（需要 mock）
- official RedisBloom key 与 gemini key 同名场景（若可加载）
- `BF.LOADCHUNK` 覆盖 existing wrong-type、existing bloom overwrite、missing data chunk

---

## TEST-11：没有 replication 测试

**级别：P2**

缺少 primary-replica 测试：duplicate `BF.ADD` 不 replicate 是否符合预期、`BF.MADD` partial success replication、`BF.LOADCHUNK` 多 chunk 复制、AOF rewrite 后 replica restart、replica 上 `BF.CARD`/`BF.EXISTS` 一致性。

### 建议

启动 master/replica，执行命令后检查两边状态一致。

---

## TEST-12：没有 command metadata/ACL/cluster key spec 测试

**级别：P3/P2**

需要检查：

```text
COMMAND INFO BF.ADD
COMMAND GETKEYS BF.ADD key item
ACL DRYRUN user BF.ADD key item
```

确保 command flags、key positions、ACL category 与 RedisBloom 文档一致。

---

## TEST-13：没有 module load config 测试

**级别：P2**

`BloomConfigLoad()` 未集成测试：default args、valid `ERROR_RATE`、valid `INITIAL_SIZE`、valid `EXPANSION`、invalid numeric、missing values、unknown args、`EXPANSION 0` 决策、very large values、case-insensitive keys。

---

## TEST-14：缺少 32-bit/legacy flag 兼容测试

**级别：P2**

虽然命令层固定 Use64Bit，但 loader 支持 flags。若要读旧 RedisBloom 数据，必须测试 no `Use64Bit`、rounded mode、NoRound false、RawBits/ENTS_IS_BITS、encver < 2、encver 2、encver 4。当前只粗略覆盖 encver2，不覆盖 hash/probe exact compatibility。

---

## TEST-15：false positive rate 测试太宽，且没有参数矩阵

**级别：P3/P2**

当前 `BloomLayerTest.FalsePositiveRate` 用 10k inserts、100k queries，目标 0.01，断言 `<0.03`。

### 缺口

多个 fp rate、多个 capacity、scaling 多层 overall FP rate、fixed vs scaling first layer tightening、RedisBloom official 同参数对照。

### 建议

把 statistical test 固定 random seed，输出实际 FP，并设合理置信区间。对 CI 保持稳定。

---

## TEST-16：没有性能 benchmark 或 latency regression

**级别：P2**

需要覆盖 `BF.ADD` 单层/多层、`BF.EXISTS` 多层 miss/hit-old-layer/hit-new-layer、`BF.MADD` batch size 1/10/1000、SCANDUMP/LOADCHUNK 大 filter、RDB/AOF rewrite。

---

## TEST-17：没有 sanitizer/coverage CI 证据

**级别：P2**

CMake 有 `ENABLE_ASAN` option，但没有看到 CI 配置或覆盖率 target。GTest 也只有 `find_package(GTest QUIET)` 后创建目标。

### 建议

新增 CI matrix：gcc release、clang asan+ubsan、coverage、redis integration、compat redisbloom。

---

## TEST-18：TCL persistence tests 用固定 sleep，容易 flaky

**级别：P3**

`BGSAVE` 后 `after 2000`，`BGREWRITEAOF` 后 `after 2000`。建议轮询 `INFO persistence`，直到 `rdb_bgsave_in_progress:0`、`aof_rewrite_in_progress:0`，并检查上次状态为 ok。

---

## TEST-19：没有并发/中断恢复测试

**级别：P2**

缺少 LOADCHUNK header 后断开连接、LOADCHUNK 中途错误后继续、SCANDUMP 期间 filter 被修改、BGSAVE 期间写入、BGREWRITEAOF 期间写入、多客户端同时 ADD/EXISTS。

Redis 单线程保证命令原子，但跨多命令协议如 SCANDUMP/LOADCHUNK 需要明确语义。

---

## TEST-20：没有源码覆盖报告，无法判断“代码覆盖率”

**级别：P2**

本次无法给出精确 coverage percentage，因为仓库未提供覆盖率产物，且容器无法 clone/run。静态看，以下文件/路径覆盖明显不足：

| 文件/路径 | 覆盖风险 |
|---|---|
| `bloom_commands.cc` parser 错误矩阵 | 主要靠 TCL，缺少 raw RESP/RESP3 |
| `bloom_config.cc` | 几乎未见集成覆盖 |
| `bloom_rdb.cc` allocation failure | 未覆盖 |
| `AofRewriteBloom` error path | 未覆盖 |
| `RdbLoadBloom` malicious metadata | 覆盖不足 |
| `BloomLayer::Create RawBits` | 未覆盖 |
| 32-bit hash path | 未覆盖 |
| rounded mode | 未覆盖 |
| unknown flags | 未覆盖 |

### 建议

引入 `llvm-cov`/`gcovr`，最低门槛：line coverage >= 80%，branch coverage >= 70%，critical parser/loader validators >= 95%。关键是 branch/path 覆盖，不只是 line 覆盖。
