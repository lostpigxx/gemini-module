# 04 - 测试与验证缺口

现有测试已经能覆盖不少 gemini-bloom 自身行为，但距离“RedisBloom 可迁入迁出”还缺关键验证层。

## TEST-01：缺 RedisBloom official golden corpus

**级别：P0**

必须补：

```text
RedisBloom SCANDUMP -> gemini LOADCHUNK
gemini SCANDUMP -> RedisBloom LOADCHUNK
RedisBloom RDB -> gemini load
gemini RDB -> RedisBloom load
RedisBloom AOF -> gemini replay
gemini AOF -> RedisBloom replay
```

建议目录：

```text
modules/gemini-bloom/tests/fixtures/redisbloom-2.4.20/
modules/gemini-bloom/tests/fixtures/redis-8-bloom/
```

fixture 内容：

- 命令 trace。
- RDB 文件。
- AOF rewrite 文件。
- SCANDUMP chunk 序列。
- expected metadata JSON。
- inserted items 和 must-exist item list。

本轮新增的 `redisbloom_compat_matrix.py` 已把 Redis 6.2.17 + RedisBloom v2.4.20 固定为本轮 oracle，并把完整结果固化为：

```text
doc/code_review/gemini-bloom/v5/06_redis62_redisbloom2420_compat_results.md
doc/code_review/gemini-bloom/v5/compat_matrix_results_redis62_redisbloom2420.json
```

它覆盖 9 个 corpus、7 类路径、双向共 126 个兼容性单元，证明：

- RDB、`DUMP/RESTORE`、RDB-preamble AOF、fullsync replication 在本轮范围内双向通过。
- SCANDUMP/LOADCHUNK 与 command-AOF 双向失败，说明迁移协议仍是阻断项。
- live replication 在 `EXPANSION 1` 下存在 `BF.CARD` 可观察分歧。

剩余缺口是把这些 runtime corpus 升级为仓库内可重复 fixture 和 CI gate，而不是只保留脚本与结果 JSON。

## TEST-02：缺 RedisBloom differential oracle

**级别：P1**

同一组命令应同时跑在 RedisBloom 和 gemini-bloom 上，比较：

- RESP2 reply shape。
- error 类型和错误发生位置。
- `BF.INFO` full 和单字段。
- `BF.CARD`。
- inserted items 的 no-false-negative。
- dump/load 后结果一致性。

这比纯 gemini round-trip 更有价值，因为当前很多测试只证明“自己写自己读”。

本轮已补 Redis 6.2.17 + RedisBloom v2.4.20 oracle：

```text
doc/code_review/gemini-bloom/v5/redisbloom_compat_matrix.py
RedisBloom module: v2.4.20, MODULE LIST ver=20420
```

已发现的 oracle 差异：

```text
BF.INFO key FIELD:
  gemini returns scalar, RedisBloom returns singleton array

BF.INFO Size:
  gemini=440, RedisBloom=240 for the same one-item filter

BF.MADD / BF.INSERT fixed overflow:
  gemini returns two error elements after two successes
  RedisBloom returns one error element after two successes

BF.INSERT NOCREATE + CAPACITY:
  gemini -> ERR NOCREATE cannot be used with CAPACITY or ERROR
  RedisBloom -> ERR not found
```

仍需把 oracle 从脚本输出升级为 CI test，并扩展更多 command traces，例如 duplicate item、partial overflow replication、`COPY`、更多 `MIGRATE` corpus 等 Redis server command 组合。本轮补充审计已覆盖 single-layer `MIGRATE COPY REPLACE` 与 TTL restore 的双向正向用例。

## TEST-03：SCANDUMP/LOADCHUNK byte-offset 边界测试不足

**级别：P0/P1**

当前已有一个失败用例证明 cursor 不按 byte length 前进：

```text
expected byte-offset cursor 129, got 2
```

还需要覆盖：

- header chunk。
- chunk 小于 layer size。
- chunk 位于 layer 中间。
- chunk 结束在 layer 边界。
- chunk 跨 layer。
- 大于 16MB layer 被拆分。
- `iter < dataLen` reject。
- cursor 越界 reject。

本轮新增 RedisBloom v2.4.20 实测边界：

```text
RedisBloom chunks:
  [(1, 179), (17, 16), (49, 32), (121, 72), (0, 0)]

gemini chunks:
  [(1, 179), (2, 128), (3, 128), (4, 128), (0, 0)]
```

本轮还覆盖了大于 16MB layer 的 split corpus：

```text
RedisBloom large_empty_16mb chunks:
  [(1, 73), (16777217, 16777216), (20677041, 3899824), (0, 0)]

gemini large_empty_16mb chunks:
  [(1, 73), (2, 20677040), (0, 0)]
```

本轮新增 `redisbloom_malicious_wire_audit.py` 已补黑盒恶意 `BF.LOADCHUNK` payload：

```text
per module:
  header cases=5032
  data cases=2007
  existing-key header cases=12
```

该 harness 覆盖 native truncation、extra byte、native mutation、random header、gemini 结构化 header、data length mutation、random data，以及已有 key 写入保护。剩余缺口是把这些 payload 固化成 CI fixture，并进一步扩展 deterministic 跨 layer chunk、layer 中间 chunk、`iter < dataLen`、跨 16MB split 边界等 RedisBloom byte-offset 语义单测。

## TEST-04：raw RESP binary harness 已有矩阵和黑盒 payload 覆盖，但仍缺 CI 固化

**级别：P2**

本轮矩阵的 `binary_items` corpus 已通过 raw RESP client 覆盖：

- chunk 中含 `\0`。
- chunk 中含 `\r\n`。
- chunk 中含空格、反斜杠、Tcl list 特殊字符。
- 大 bulk string。
- raw 1..31 bytes。

本轮新增随机 payload 覆盖：

```text
RDB decoder random fuzz:       100000 cases
wire decoder random fuzz:      100000 cases
BF.LOADCHUNK header random:    2500 cases per module
BF.LOADCHUNK data random:      1000 cases per module
```

剩余缺口是把随机 seed、最小失败样本、binary corpus 和 replay fixture 固化到 CI，而不是只保留审计 runner 与 JSON 结果。

## TEST-05：replication 基础路径已覆盖，partial-command 仍需扩展

**级别：P2**

本轮矩阵已覆盖：

```text
live replication command stream:
  RedisBloom master -> gemini replica
  gemini master -> RedisBloom replica
  16/18 cells pass; expansion1 双向 BF.CARD 分歧

fullsync replication snapshot:
  RedisBloom master -> gemini replica
  gemini master -> RedisBloom replica
  18/18 cells pass
```

仍需扩展：

- duplicate `BF.ADD` 是否不复制且主从一致。
- `BF.MADD` / `BF.INSERT` partial success 后主从一致。
- 通过 `BF.LOADCHUNK` 创建对象时的 replication 错误传播。
- AOF rewrite 后 replica restart。

## TEST-06：已有 fuzz 审计，但仍缺 release gate

**级别：P2**

本轮已补：

```text
DeserializeHeader structured + random fuzz
mock RDB stream structured + random fuzz
BF.LOADCHUNK header/data black-box fuzz
ASAN + UBSAN decoder fuzz
3GB declared blob/dataSize resource-bomb harness
```

结果固化在：

```text
doc/code_review/gemini-bloom/v5/07_fuzz_and_malicious_payload_results.md
doc/code_review/gemini-bloom/v5/rdb_wire_fuzz_results_redis62_redisbloom2420.json
doc/code_review/gemini-bloom/v5/rdb_wire_fuzz_asan_results_redis62_redisbloom2420.json
doc/code_review/gemini-bloom/v5/malicious_wire_audit_results_redis62_redisbloom2420.json
```

仍建议作为 release gate 固化：

- `DeserializeHeader` fuzz。
- mock RDB stream fuzz。
- `BF.LOADCHUNK` payload fuzz。
- command parser fuzz。
- ASAN + UBSAN。
- 覆盖率报告。

尤其是 RDB 和 LOADCHUNK，应该视为非信任输入。

## TEST-07：测试环境本身有可修复问题

**级别：P3**

旧 macOS 结果中发现：

- 仓库内 `build/` 的 CMake cache 来自另一路径，直接 `cmake --build build --target bloom_test` 失败。
- 隔离 build 的 GTest 可执行文件缺少 dylib rpath，custom target 运行失败，需要 `DYLD_LIBRARY_PATH=/opt/anaconda3/lib`。
- 仓库内 `build/redis_bloom.so` 不是本机可加载 Mach-O 文件，TCL 不能直接用它。

本轮 Docker 结果中新增：

- 容器内 CMake 没有普通 `GTest` package，因此 `bloom_filter_test` / `sb_chain_test` / `bloom_rdb_test` targets 不生成。
- 强行指向 `/usr/lib64/libllvm_gtest.a` 会链接失败，因为 LLVM gtest 依赖 LLVM Support symbols。
- 使用 RocksDB 自带 gtest fused sources 手工编译可运行测试，但 `sb_chain_test.cc` 独立编译暴露缺少 `<cstring>` include，需要 `-include cstring` workaround。
- `ScalingBloomTest.ExtremeParamsRejected` 在当前 Docker/Rosetta 环境触发巨大分配 abort；过滤该用例后其余 `sb_chain_test` 通过。

建议：

- 不提交平台相关 build 产物。
- 文档化 macOS 下 GTest dylib rpath 或改为静态链接。
- 给 TCL 测试提供统一构建入口，避免误用旧 `.so`。
- 给 Docker 环境提供稳定 GTest 依赖或 vendored test-only gtest。
- 修复 `sb_chain_test.cc` 的直接 include 依赖，并把极端参数测试改成不会触发真实巨大分配。

## TEST-08：缺 command-AOF 与 RDB-preamble AOF 分层验证

**级别：P1**

本轮 Redis 6.2.17 + RedisBloom v2.4.20 矩阵证明 AOF 不能只写“通过/失败”，必须区分 Redis 的 AOF rewrite 模式：

```text
aof-use-rdb-preamble yes:
  AOF rewrite 文件是 RDB preamble，不包含 BF.LOADCHUNK。
  gemini -> RedisBloom: 9/9 corpora pass
  RedisBloom -> gemini: 9/9 corpora pass

aof-use-rdb-preamble no:
  AOF rewrite 文件包含 BF.LOADCHUNK。
  gemini -> RedisBloom: 0/9 corpora pass
  RedisBloom -> gemini: 0/9 corpora pass
```

目标测试矩阵应显式包含两种模式，否则默认 Redis 6/7 的 RDB-preamble 可能掩盖 module `aof_rewrite` 失败。

## TEST-09：补充审计覆盖了命令元数据和 server 原生命令，但仍需 CI 固化

**级别：P1/P2**

本轮新增 `redisbloom_extended_audit.py`，覆盖主 126-cell 矩阵没有展开的兼容面：

```text
COMMAND INFO:
  BF.DEBUG missing in gemini
  BF.SCANDUMP flags write vs readonly/fast
  BF.INFO / BF.CARD missing fast flag

readonly replica:
  RedisBloom BF.SCANDUMP works on replica
  gemini BF.SCANDUMP returns READONLY

LOADCHUNK existing key:
  gemini cursor=1 header replaces existing Bloom key
  RedisBloom cursor=1 header over existing Bloom key returns ERR received bad data

incremental AOF:
  single_layer and fixed_full pass both directions
  expansion1 fails both directions by BF.CARD drift

Redis server helpers:
  MIGRATE COPY REPLACE with TTL passes both directions
  DUMP/RESTORE with explicit TTL passes both directions

module args:
  INITIAL_SIZE / ERROR_RATE common path passes
  EXPANSION is gemini-only and fails RedisBloom module load
  CF_MAX_EXPANSIONS is RedisBloom-only and fails gemini module load
```

结果固化在：

```text
doc/code_review/gemini-bloom/v5/extended_audit_results_redis62_redisbloom2420.json
```

这些仍是审计 runner，不是 CI gate。建议下一步把以下最小集合转成自动测试：

- `COMMAND INFO BF.SCANDUMP` 必须是 `readonly`，且 replica 上可执行。
- `BF.LOADCHUNK key 1 <header>` 对已有 Bloom key 的行为必须与 RedisBloom 目标语义一致。
- incremental AOF command stream 与 live replication 使用同一 oracle 检查 `BF.CARD`。
- `MIGRATE` / TTL restore 保留为正向 regression，防止 RDB object 兼容倒退。
- module load args 的支持列表固定成文档与启动测试。
