> 分析基准：GitHub 仓库 `lostpigxx/gemini-module` 的 `main` 分支当前可读取源码；重点目录为 `modules/gemini-bloom`。  
> 方法：静态源码审查 + 与当前 RedisBloom 官方源码/命令文档做行为对照。未在本地编译运行；容器内无法直接 `git clone`，因此结论基于 GitHub API 拉取到的文件内容。  
> 严重性：P0=可能崩溃/数据损坏/安全风险；P1=兼容性或核心语义错误；P2=明显设计/性能/可维护性缺陷；P3=低风险但应修正。

# 07. 测试覆盖问题

## 总览

当前测试并非空白：已有 GTest 单元测试覆盖 BloomLayer、ScalingBloomFilter、RDB/wire 基本 round-trip；TCL 集成测试覆盖 Redis 命令、RDB/AOF persistence、wrong type 和若干边界值。

但这些测试仍不能支撑“RedisBloom 兼容”或“边界安全”的结论。最大缺口是：没有官方 RedisBloom golden corpus，没有 sanitizers/coverage/fuzz，RDB/LOADCHUNK 恶意输入测试不足，RESP3/ACL/命令元数据未覆盖。

| ID | 严重性 | 测试区域 | 覆盖缺口 |
|---|---:|---|---|
| TEST-01 | P0 | RedisBloom 兼容 | 无官方 RedisBloom RDB/SCANDUMP/hash golden corpus |
| TEST-02 | P0 | Sanitizer | 没有 UBSan/ASan/LSan/TSan 的强制 CI |
| TEST-03 | P0 | C++ lifetime | 未覆盖 `RMRealloc`/未构造赋值等对象生命周期问题 |
| TEST-04 | P0 | 参数边界 | 未测 `EXPANSION > UINT_MAX`、极小 error rate、巨大 capacity |
| TEST-05 | P0 | RDB fuzz | 损坏 RDB 元数据测试严重不足 |
| TEST-06 | P0 | LOADCHUNK fuzz | header/chunk 畸形输入覆盖不足 |
| TEST-07 | P1 | SCANDUMP compat | 只测 Gemini→Gemini round-trip，不测 RedisBloom↔Gemini |
| TEST-08 | P1 | RESP3 | 完全没有 RESP3 测试 |
| TEST-09 | P1 | command metadata | 未测 ACL category、COMMAND INFO、fast/read/write flags |
| TEST-10 | P1 | multi write | 未测 full 时 `BF.MADD`/`BF.INSERT` 的数组长度、错误元素和部分写入 |
| TEST-11 | P1 | wrong type | `BF.RESERVE` wrong type 未覆盖 |
| TEST-12 | P1 | allocation failure | 没有 OOM/allocator failure 注入测试 |
| TEST-13 | P1 | persistence race | TCL 使用固定 sleep 等 BGSAVE/AOF，测试不稳定 |
| TEST-14 | P1 | replication/cluster | 未测 replica、AOF apply、cluster key spec |
| TEST-15 | P2 | parser | option 重复、顺序、组合、case、尾随参数覆盖不足 |
| TEST-16 | P2 | FPR | 误判率测试阈值宽，且只覆盖少数组合 |
| TEST-17 | P2 | 32-bit/hash flags | Hash32、RawBits、Round/NoRound 组合覆盖不足 |
| TEST-18 | P2 | coverage metrics | 没有代码覆盖率报告，无法知道分支覆盖 |
| TEST-19 | P2 | benchmark | 没有性能基准和回归阈值 |
| TEST-20 | P2 | IO error mock | `mock_redismodule_io.h` 不模拟 RedisModule_IsIOError |
| TEST-21 | P3 | TCL harness | 测试框架、client、server 管理耦合，失败诊断弱 |
| TEST-22 | P3 | build integration | GTest 静默缺失，TCL 未进 CTest |

## 已有覆盖

### GTest 覆盖

`bloom_filter_test.cc` 覆盖：

- MurmurHash2/MurmurHash64A deterministic。
- Hash policy consistency。
- BloomLayer create/move/insert/test。
- 粗略 false positive rate。
- power-of-two bit ceil 路径。
- BloomFlags operator。

`sb_chain_test.cc` 覆盖：

- ScalingBloomFilter 构造/析构。
- Put/Contains。
- auto expansion。
- fixed-size overflow。
- TotalCapacity/BytesUsed。
- span 接口。

`bloom_rdb_test.cc` 覆盖：

- RDB empty/populated/multilayer round-trip。
- metadata preserved。
- bit array exact match。
- fixed-size flag preserved。
- encver 2 backward compatibility。
- unknown encver rejection。
- SCANDUMP/LOADCHUNK header round-trip simulation。
- truncated header/zero layers/too many layers。
- expansion factor、fp rate 组合。

### TCL 集成测试覆盖

`tests/tcl/bloom_test.tcl` 覆盖：

- BF.RESERVE、ADD、EXISTS、MADD、MEXISTS、INSERT、INFO、CARD。
- wrong arity、invalid rate/capacity/expansion。
- NONSCALING overflow。
- auto scaling。
- SCANDUMP/LOADCHUNK Gemini 自身 round-trip。
- wrong type。
- LOADCHUNK malformed header、wrong size。
- empty string、long item、binary item。
- RDB restart。
- AOF rewrite + restart。

这些覆盖是有价值的，但仍有大量关键分支未覆盖。

## 详细缺口

### TEST-01：缺少 RedisBloom golden corpus

当前所有兼容性测试都是“自己写、自己读”。这无法证明与 RedisBloom 互通。

必须增加 golden：

```text
tests/golden/redis-bloom/
  rb-version.txt
  bf.reserve.0_01.1000.rdb
  bf.reserve.0_01.1000.scandump/
  known_items_bitarray.bin
  resp2_replies.txt
  resp3_replies.txt
```

测试方向：

1. RedisBloom 生成 RDB → Gemini 加载 → 查询所有 item。
2. Gemini 生成 RDB → RedisBloom 加载 → 查询所有 item。
3. RedisBloom SCANDUMP → Gemini LOADCHUNK。
4. Gemini SCANDUMP → RedisBloom LOADCHUNK。
5. 同一参数+同一 item 后 bit array exact match。
6. BF.INFO RESP2/RESP3 exact match。

当前最容易失败的是 SCANDUMP/LOADCHUNK cursor 语义，必须优先测。

### TEST-02：没有强制 sanitizer CI

CMake 有 `ENABLE_ASAN`：

```cmake
option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
```

但没有看到 CI 或测试目标强制跑 sanitizer。当前代码存在典型 sanitizer 应发现的问题：

- C++ object lifetime UB。
- shift UB。
- integer divide by zero。
- allocation overflow。
- use of unconstructed object。
- potential leaks on error paths。

建议矩阵：

```text
Debug + ASan + UBSan
Release + ASan
Debug + LSan
```

UBSan 必须开启：

```cmake
-fsanitize=undefined,address -fno-omit-frame-pointer
```

### TEST-03：没有对象生命周期专项测试

普通 round-trip 测试可能不会触发崩溃，因为 UB 不一定显性失败。需要：

- ASAN/UBSAN 下多层扩容。
- 构造足够多 layers 触发多次 `RMRealloc`。
- RDB/LOADCHUNK 反复 load/free。
- 随机 load/delete/key replace。
- 使用 `-fsanitize=object-size`、`-fsanitize=alignment`。

此外，建议用 clang 的 lifetime 分析或 Valgrind 补充。

### TEST-04：参数边界缺失

未看到这些测试：

```text
BF.RESERVE k 0.01 10 EXPANSION 4294967296
BF.INSERT k EXPANSION 4294967296 ITEMS a
loadmodule redis_bloom.so EXPANSION 4294967296
BF.RESERVE k 1e-300 9223372036854775807
BF.RESERVE k nan 100
BF.RESERVE k inf 100
BF.RESERVE k 0.999999999999 100
BF.RESERVE k 0.0000000000000000001 100
```

目标：

- 不崩溃。
- 返回稳定错误。
- 不创建半成品 key。
- 不复制到 AOF/replica。

### TEST-05：RDB 损坏输入测试不足

`bloom_rdb_test.cc` 有 unknown encver，但缺少字段级恶意 RDB：

- `numLayers = SIZE_MAX`。
- `totalBits = UINT64_MAX`。
- `log2Bits = 64/255`。
- `hashCount = 0`。
- `fpRate = NaN/Inf/0/负数/>=1`。
- `bitsPerEntry = NaN/Inf/负数`。
- `capacity = 0`。
- `itemCount > capacity`。
- `totalItems != sum(itemCount)`。
- bit array blob 小于/大于 expected。
- layer itemCount 截断。
- flags 带未知 bit。
- encver 0/1/2/3/4 差异。

每个 case 应断言 load 返回 null，并无泄漏。

### TEST-06：LOADCHUNK fuzz 不足

已有 LOADCHUNK safety 测试覆盖了 header too short、zero layers、bad size。但还缺：

- header 长度大于 required。
- `dataSize > expectedSize`。
- `totalBits + 7` 溢出。
- `log2Bits` 与 `totalBits` 不一致。
- `hashCount=0`。
- `itemCount>capacity`。
- `numLayers=1024` 且每层超大。
- unknown flags。
- cursor 极大值。
- 重复加载同一 chunk。
- 先加载 chunk 再加载 header。
- header 后不加载 chunk 直接查询。
- 导入中途写入，然后再加载 chunk。

建议用 libFuzzer/AFL++ fuzz `DeserializeHeader()` 和 command-level LOADCHUNK。

### TEST-07：SCANDUMP/LOADCHUNK 只测自己兼容自己

TCL 里两段 SCANDUMP/LOADCHUNK 都是：

```text
Gemini SCANDUMP -> Gemini LOADCHUNK
```

这不能证明 RedisBloom compatibility。尤其当前 Gemini cursor 是层索引，RedisBloom cursor 是 byte offset，此类问题只能通过跨实现测试发现。

建议新增 dockerized matrix：

```text
redis-server + RedisBloom official
redis-server + GeminiBloom
```

测试四向：

```text
official dump -> official load   baseline
gemini dump   -> gemini load     current
official dump -> gemini load     must pass for compatibility
gemini dump   -> official load   must pass for compatibility
```

### TEST-08：没有 RESP3 测试

TCL client 只解析 RESP2。RedisBloom 对 RESP3 会返回 bool/map。Gemini 没有 RESP3 适配，但测试完全看不到。

建议：

- 用 `HELLO 3` 切换 RESP3。
- 测：
  - `BF.EXISTS`
  - `BF.MEXISTS`
  - `BF.ADD`
  - `BF.MADD`
  - `BF.INSERT`
  - `BF.INFO`
- 对照 RedisBloom 官方输出。

### TEST-09：未测 ACL category 和 command metadata

需要测试：

```redis
ACL CAT bloom
COMMAND INFO BF.EXISTS
COMMAND INFO BF.INFO
COMMAND DOCS BF.INFO
```

期望：

- command flags 与 RedisBloom 一致。
- ACL categories 包含 bloom/read/write/fast。
- first key / last key / step 正确。

当前 Gemini 没有 ACL category 注册，这类测试会暴露差异。

### TEST-10：多元素写入 full 行为未覆盖回复结构

已有测试只检查 fixed-size 最终会拒绝，但没有精确验证：

- `BF.MADD` 在第 N 个 item full 时返回什么数组。
- 返回数组长度是否等于输入 item 数。
- 后续 item 是否被处理。
- 已插入 item 是否复制到 AOF。
- replica 是否一致。
- RESP 协议层是不是 top-level error 还是 array error element。

建议用 raw RESP 读取首字节和数组长度，类似已有 wrong type 测试。

### TEST-11：`BF.RESERVE` wrong type 未覆盖

TCL 测了 `BF.ADD/EXISTS/INFO/MADD/INSERT/CARD/SCANDUMP` on string key，但没测：

```redis
SET s x
BF.RESERVE s 0.01 100
```

当前实现会返回 `ERR key already exists`，而不是 WRONGTYPE。应加入精确测试。

### TEST-12：没有 allocator failure 注入测试

需要可控 allocator：

```cpp
SetAllocFailAfter(n)
```

覆盖：

- `AllocFilter()` object alloc fail。
- `BloomLayer::Create()` bit array alloc fail。
- `AppendLayer()` realloc fail。
- `SCANDUMP` header alloc fail。
- `AofRewriteBloom` header alloc fail。
- `DeserializeHeader()` 第 k 层 alloc fail。

断言：

- 不泄漏。
- 不创建半成品 key。
- 错误回复正确。
- AOF rewrite 不静默产出损坏结果。

### TEST-13：TCL persistence 测试用固定 sleep，容易 flaky

当前：

```tcl
r BGSAVE
after 2000
...
r BGREWRITEAOF
after 2000
```

问题：

- 慢机器上 2 秒不够。
- 快机器上浪费时间。
- 如果 save/rewrite 失败，测试可能没有明确诊断。

建议轮询：

```redis
LASTSAVE
INFO persistence
aof_rewrite_in_progress
aof_last_bgrewrite_status
rdb_last_bgsave_status
```

直到成功或 timeout。

### TEST-14：未测 replication/cluster

需要至少覆盖：

- master 加载 Gemini，replica 加载 Gemini，写入 BF.ADD/BF.MADD/BF.INSERT 后 replica 查询一致。
- AOF rewrite 后 replica restart。
- `COMMAND GETKEYS` / cluster key spec。
- readonly 命令在 replica 上行为。
- `BF.LOADCHUNK` 大 payload replication。

### TEST-15：parser 组合覆盖不足

缺少：

```text
BF.RESERVE k 0.01 100 EXPANSION 2 EXPANSION 3
BF.RESERVE k 0.01 100 NONSCALING NONSCALING
BF.RESERVE k 0.01 100 EXPANSION
BF.RESERVE k 0.01 100 EXPANSION abc
BF.INSERT k ERROR 0.01 ERROR 0.02 ITEMS a
BF.INSERT k ITEMS a ERROR 0.01
BF.INSERT k NOCREATE CAPACITY 100 ITEMS a
BF.INSERT k NONSCALING EXPANSION 2 ITEMS a
BF.INSERT k EXPANSION 2 NONSCALING ITEMS a
BF.INSERT k ITEMS
```

每个都要明确 expected behavior，并与 RedisBloom 对齐。

### TEST-16：误判率测试覆盖太窄

当前 FPR test：

```cpp
cap=10000, fp=0.01, Use64Bit
EXPECT_LT(actual, 0.03)
```

问题：

- 阈值是目标的 3 倍，较宽。
- 只测一个 capacity/fp。
- 不测 scaling 后整体误判率。
- 不测 fixed-size。
- 不测 NoRound 与 rounded 两种模式。
- 不测 32-bit hash。

建议使用 deterministic seed 数据集，并覆盖矩阵：

```text
fp: 0.1, 0.01, 0.001
capacity: 10, 100, 10000
flags: 64+NoRound, 64+Round, 32+NoRound
scaling: expansion 1/2/4
```

### TEST-17：flags 组合覆盖不足

当前命令路径只创建 `Use64Bit | NoRound`。测试也主要集中在 64-bit。缺少：

- `Hash32Policy` 插入/查询完整路径。
- `RawBits` 行为。
- `FixedSize + NoRound + Use64Bit` RDB/wire。
- unknown flags reject。
- RedisBloom flag bit exact golden。

### TEST-18：没有 coverage 报告

没有看到：

- gcov/lcov target。
- llvm-cov target。
- branch coverage。
- function coverage。
- coverage threshold。

建议 CI 输出：

```text
line coverage >= 85%
branch coverage >= 75%
core files 100% function coverage
```

同时单独列出未覆盖函数：

- error branches
- allocation failure branches
- corrupted load branches
- command parser error branches

### TEST-19：没有性能基准

缺 benchmark 会导致性能回退不可见。建议至少覆盖：

```text
BF.ADD QPS: varying key length, capacity, fp
BF.EXISTS QPS: present/missing
scaling layers: 1/10/100/1000
BF.INFO latency
SCANDUMP throughput
LOADCHUNK throughput
AOF rewrite size/time
```

输出 p50/p95/p99。

### TEST-20：IO error mock 不完整

`mock_redismodule_io.h` 安装：

```cpp
RedisModule_SaveUnsigned
RedisModule_LoadUnsigned
RedisModule_SaveDouble
RedisModule_LoadDouble
RedisModule_SaveStringBuffer
RedisModule_LoadStringBuffer
RedisModule_Free
```

但 `RdbReader` 使用 `RedisModule_IsIOError()`，mock 没有设置它，也没有错误状态。`ReadBytes()` 失败只返回 false，Load 函数忽略。

结果：

- truncated stream 不会真实模拟 Redis IO error。
- `RdbReader::Ok()` 路径覆盖不可靠。
- 读取失败可能被当成 0 值继续测试。

建议：

```cpp
struct MockRdbStream {
  bool io_error = false;
};
Mock_LoadUnsigned(...) { if (!ReadBytes(...)) io_error = true; }
Mock_IsIOError(io) { return StreamOf(io)->io_error; }
```

### TEST-21：TCL harness 失败诊断弱

`start_redis` 里：

```tcl
catch { exec redis-server ... }
```

会吞掉 exec 失败，然后只靠后续连接 timeout。建议：

- 不要吞掉 exec stderr。
- 启动失败立即打印 command、exit code、log。
- 每个测试使用独立 dbdir，避免 `/tmp` 共享污染。
- 使用 `try/finally` 保证 stop_redis 执行。

### TEST-22：测试构建入口不可靠

当前 CMake 中：

- GTest 不存在就跳过。
- GTest targets `EXCLUDE_FROM_ALL`。
- `bloom_test` custom target 只跑 GTest，不跑 TCL。
- 没有 `ctest`.

建议统一：

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

并把 integration tests 标为 label，可选择运行：

```bash
ctest -L unit
ctest -L integration
ctest -L compatibility
```
