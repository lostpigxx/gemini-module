# 07 - 测试覆盖问题

当前测试已经覆盖了不少基础行为。本轮实际结果：

```text
GTest: 47 passed
TCL:   89 passed
```

但这些测试主要证明 gemini-bloom 自洽，不能证明 RedisBloom 兼容，也没有完整覆盖恶意输入、RESP3、资源失败和大数据场景。

## TEST-01：没有 RedisBloom official golden corpus

**级别：P1**

缺少：

```text
RedisBloom SCANDUMP -> gemini LOADCHUNK
gemini SCANDUMP -> RedisBloom LOADCHUNK
RedisBloom RDB -> gemini load
gemini RDB -> RedisBloom load
RedisBloom AOF -> gemini replay
gemini AOF -> RedisBloom replay
```

### 建议

新增 `tests/fixtures/redisbloom-2.8/` 或 Redis 8 Bloom fixture，保存：

- RESP2/RESP3 raw command traces
- small/large SCANDUMP chunks
- RDB files
- AOF rewrite files
- expected metadata JSON

## TEST-02：SCANDUMP/LOADCHUNK 没测官方 byte-offset cursor

**级别：P1**

当前 TCL 测试“使用返回 iterator 直接 LOADCHUNK”，但 dump/load 两端都是 gemini，所以 layer-index 私有协议也会通过。

位置：`modules/gemini-bloom/tests/tcl/bloom_test.tcl:449-515`。

### 必测场景

- `SCANDUMP key 0 -> iter 1`
- 下一次 iter 等于 `1 + chunk_len`
- chunk 小于 layer size
- chunk 在 layer 中间
- chunk 在 layer 边界
- 大于 16MB layer 被拆分
- `iter < dataLen` reject

## TEST-03：RESP3 raw type 未覆盖

**级别：P1**

TCL client 当前主要按 RESP2 解析。缺少 `HELLO 3` 后 raw type 检查：

- `BF.ADD` 应返回 boolean
- `BF.EXISTS` 应返回 boolean
- `BF.MADD` / `BF.MEXISTS` array element 应为 boolean
- `BF.INFO` full 应为 map
- `BF.INFO key CAPACITY` exact shape

当前测试还把 `BF.INFO key Capacity -> 1000` 标量行为固定住了。

位置：`modules/gemini-bloom/tests/tcl/bloom_test.tcl:370-376`。

## TEST-04：RDB malicious metadata 覆盖仍不足

**级别：P0/P1**

已有：

- truncated itemCount
- `log2Bits >= 64`
- excessive numLayers
- zero layers / truncated header

仍缺少：

| 场景 | 期望 |
|---|---|
| RDB `totalBits == 0 && hashCount > 0` | reject |
| RDB `fpRate = NaN/Inf/0/1/-x` | reject |
| RDB `bitsPerEntry = NaN/Inf/0/-x` | reject |
| RDB `capacity == 0` | reject |
| RDB `hashCount != ceil(ln2 * bitsPerEntry)` | reject |
| RDB `itemCount > capacity` | reject |
| RDB `totalItems != sum(itemCount)` | reject |
| unknown flags | reject |
| RawBits flag | reject or fully support |
| huge `dataSize` / huge `totalBits` | reject before allocation |
| partial layer allocation failure | no leak/no UB |

## TEST-05：没有 OOM/failure injection

**级别：P1/P2**

需要模拟：

- `RMAlloc` fail
- `RMCalloc` fail
- `RedisModule_ModuleTypeSetValue` fail
- `RedisModule_LoadStringBuffer` fail after partial stream
- `DeserializeHeader` 第 N 层分配失败
- `AofRewriteBloom` header allocation fail

建议 test allocator 提供：

```cpp
SetAllocFailAfter(n);
```

并在 ASAN/UBSAN 下跑。

## TEST-06：命令 parser 缺少纯单元测试矩阵

**级别：P2**

TCL 覆盖了 happy path 和部分错误，但 parser 仍应有纯单元测试：

- option 任意顺序
- missing value
- duplicate option
- unknown option before/after `ITEMS`
- `ITEMS` missing / empty
- `NOCREATE + CAPACITY`
- `NOCREATE + ERROR`
- `NONSCALING + EXPANSION`
- `EXPANSION 0`
- overflow / non-numeric / negative

## TEST-07：module config 未集成测试

**级别：P2**

`BloomConfigLoad()` 没有集成测试覆盖：

- default args
- valid `ERROR_RATE`
- valid `INITIAL_SIZE`
- valid `EXPANSION`
- missing value
- invalid numeric
- unknown arg
- `EXPANSION 0` 决策
- very large values
- case-insensitive names

位置：`modules/gemini-bloom/src/bloom_config.cc:9-57`。

## TEST-08：COMMAND INFO / ACL / key spec 未测试

**级别：P2**

需要覆盖：

```text
COMMAND INFO BF.ADD
COMMAND INFO BF.SCANDUMP
COMMAND GETKEYS BF.ADD key item
ACL DRYRUN user BF.SCANDUMP key 0
```

并与 Redis 文档或项目声明对齐。

## TEST-09：fixed filter partial failure 语义未精确覆盖

**级别：P2**

当前只检查“会 reject”，没有检查：

- 返回数组长度
- error 元素位置
- full 后是否继续处理后续 item
- 前缀成功是否保留
- `BF.CARD` 是否等于实际成功插入数
- AOF replay 是否成功
- replica 是否一致

## TEST-10：replication 测试缺失

**级别：P2**

缺少 master/replica 场景：

- duplicate `BF.ADD` 不 replicate 是否符合预期
- `BF.MADD` 部分成功/部分错误
- `BF.LOADCHUNK` 多 chunk 复制
- AOF rewrite 后 replica restart
- replica 上 `BF.CARD` / `BF.EXISTS` 一致性

## TEST-11：缺少 fuzz / sanitizer CI

**级别：P2**

建议目标：

- `DeserializeHeader` fuzz
- mock RDB stream fuzz
- command parser fuzz
- ASAN + UBSAN
- coverage report

尤其是 RDB/LOADCHUNK 应视为非信任输入。

## TEST-12：SCANDUMP binary safety 测试不足

**级别：P2**

TCL string/list 会掩盖一部分二进制问题。需要 raw RESP harness 覆盖：

- header/chunk 包含 `\0`
- chunk 包含空格、`\r\n`、反斜杠、Tcl list 特殊字符
- 大 chunk
- 随机 bytes

## TEST-13：缺少 official hash/bit-position exact vector

**级别：P2**

现有 hash 测试只验证 deterministic。兼容性需要 exact vector：

- 32-bit `h1/h2`
- 64-bit `h1/h2`
- fixed item 的 bit positions
- NoRound bytes/bits exact value
- rounded mode exact value

## TEST-14：缺少性能回归测试

**级别：P2/P3**

建议基准：

- add/exists throughput
- multi command batch
- expansion=1/2/4 layer count
- SCANDUMP/LOADCHUNK chunk latency
- RDB load/save
- AOF rewrite size and time

