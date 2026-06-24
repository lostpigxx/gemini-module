# 05 - 已知失败用例

本文件记录本轮实际执行得到的失败，不包括只从代码推断的问题。Redis 6.2 + RedisBloom v2.8.20 完整兼容性测试项和结果详见 `06_redis62_redisbloom2820_compat_results.md`。

## 执行环境

```text
container:      974d83bcff5c (strange_feynman)
redis-server:   6.2.17
workspace:      /workspace/projects/VibeCoding/gemini-module
build dir:      /tmp/gemini-module-v5-docker-build
gemini module:  /tmp/gemini-module-v5-docker-build/redis_bloom.so
RedisBloom:     v2.8.20, MODULE LIST ver=20820
RedisBloom so:  /tmp/redisbloom-v2.8.20/bin/linux-x64-release/redisbloom.so
result JSON:    doc/code_review/gemini-bloom/v5/compat_matrix_results_redis62_redisbloom2820.json
extended JSON:  doc/code_review/gemini-bloom/v5/extended_audit_results_redis62_redisbloom2820.json
```

核心执行方式：

```text
docker exec 974d83bcff5c \
  cmake -S /workspace/projects/VibeCoding/gemini-module \
        -B /tmp/gemini-module-v5-docker-build \
        -DCMAKE_BUILD_TYPE=Debug

docker exec 974d83bcff5c \
  cmake --build /tmp/gemini-module-v5-docker-build --target redis_bloom

docker exec 974d83bcff5c \
  tclsh /workspace/projects/VibeCoding/gemini-module/modules/gemini-bloom/tests/tcl/bloom_test.tcl \
        /tmp/gemini-module-v5-docker-build/redis_bloom.so

docker exec 974d83bcff5c \
  python3 /workspace/projects/VibeCoding/gemini-module/doc/code_review/gemini-bloom/v5/redisbloom_compat_matrix.py \
    --gemini-module /tmp/gemini-module-v5-docker-build/redis_bloom.so \
    --redis-server /workspace/projects/Environments/OpenSourceRedis/redis-6.2-redisbloom/bin/redis-server \
    --redisbloom-module /tmp/redisbloom-v2.8.20/bin/linux-x64-release/redisbloom.so \
    --env-name redis-6.2-redisbloom-v2.8.20 \
    --redis-tag 6.2.17 \
    --redisbloom-tag v2.8.20 \
    --module-ver 20820 \
    --include-large \
    --output /workspace/projects/VibeCoding/gemini-module/doc/code_review/gemini-bloom/v5/compat_matrix_results_redis62_redisbloom2820.json

docker exec 974d83bcff5c \
  python3 /workspace/projects/VibeCoding/gemini-module/doc/code_review/gemini-bloom/v5/redisbloom_extended_audit.py \
    --gemini-module /tmp/gemini-module-v5-docker-build/redis_bloom.so \
    --redis-server /workspace/projects/Environments/OpenSourceRedis/redis-6.2-redisbloom/bin/redis-server \
    --redisbloom-module /tmp/redisbloom-v2.8.20/bin/linux-x64-release/redisbloom.so \
    --env-name redis-6.2-redisbloom-v2.8.20 \
    --redis-tag 6.2.17 \
    --redisbloom-tag v2.8.20 \
    --module-ver 20820 \
    --output /workspace/projects/VibeCoding/gemini-module/doc/code_review/gemini-bloom/v5/extended_audit_results_redis62_redisbloom2820.json
```

## 通过的测试

```text
TCL integration:                  137 passed, 6 failed
bloom_filter_test:                 27 passed
bloom_rdb_test:                    54 passed, 4 failed
sb_chain_test filtered:            15 passed
RedisBloom compatibility matrix:   88 passed, 38 failed, 0 errors
RedisBloom extended audit:
  incremental AOF:                 4 passed, 2 failed, 0 errors
  MIGRATE + TTL restore:           2 passed, 0 failed, 0 errors
  BF.DEBUG:                        RedisBloom present, gemini missing
  readonly replica BF.SCANDUMP:    RedisBloom OK, gemini READONLY
  LOADCHUNK existing key:          gemini replaces, RedisBloom rejects header
```

`sb_chain_test filtered` 指过滤 `ScalingBloomTest.ExtremeParamsRejected` 后运行：

```text
/tmp/gemini-module-v5-sb_chain_test --gtest_filter=-ScalingBloomTest.ExtremeParamsRejected
```

兼容性矩阵中确认通过的路径：

```text
RDB file load/save:        18/18 passed
DUMP/RESTORE:              18/18 passed
RDB-preamble AOF rewrite:  18/18 passed
fullsync replication:      18/18 passed
```

## GTest 失败：RDB / wire metadata 校验

`bloom_rdb_test`：

```text
58 tests from 3 test suites ran
54 passed
4 failed
```

失败用例：

```text
BloomRdb.RejectsBitsPerEntryZero
  Expected loaded == nullptr
  Actual loaded != nullptr
  Meaning: RDB path accepts bitsPerEntry=0.

BloomRdb.RejectsHashCountInconsistentWithBitsPerEntry
  Expected loaded == nullptr
  Actual loaded != nullptr
  Meaning: RDB path accepts hashCount inconsistent with ceil(ln2 * bitsPerEntry).

BloomWire.RejectsBitsPerEntryZero
  Expected loaded == nullptr
  Actual loaded != nullptr
  Meaning: LOADCHUNK/wire header path accepts bitsPerEntry=0.

BloomWire.RejectsHashCountInconsistentWithBitsPerEntry
  Expected loaded == nullptr
  Actual loaded != nullptr
  Meaning: LOADCHUNK/wire header path accepts hashCount inconsistent with ceil(ln2 * bitsPerEntry).
```

相关实现位置：

- `modules/gemini-bloom/src/bloom_rdb.cc:52-63`
- `modules/gemini-bloom/src/bloom_rdb.cc:239-274`

这些是当前目标内失败，建议作为 P1 修复。

## GTest 失败：极端参数巨大分配

`sb_chain_test` 全量运行到 `ScalingBloomTest.ExtremeParamsRejected` 时 abort：

```text
[ RUN      ] ScalingBloomTest.ExtremeParamsRejected
rosetta error: could not find free space for allocation size efa05fa53e2000
```

含义：

- 该用例期望极端参数被拒绝，但当前路径仍可能进入巨大 allocation。
- 在当前 Docker/Rosetta 环境中，它不是普通 assertion failure，而是进程 abort。
- 过滤该用例后，其余 15 个 `sb_chain_test` 用例通过。

相关实现位置：`modules/gemini-bloom/src/bloom_filter.cc:85-127`。

## TCL 失败：RESP3 目标外兼容差异

`bloom_test.tcl`：

```text
137 passed
6 failed
```

其中 5 个是 RESP3。用户已明确不需要 RESP3，因此这些记录为目标外失败：

```text
EXPECTED RESP3 GAP: BF.ADD returns boolean type
EXPECTED RESP3 GAP: BF.EXISTS returns boolean type
EXPECTED RESP3 GAP: BF.MADD returns array of booleans
EXPECTED RESP3 GAP: BF.MEXISTS returns array of booleans
EXPECTED RESP3 GAP: BF.INFO full response returns map type
```

处理建议：

- 如果继续不支持 RESP3，应从必过 CI 中移除或标记 expected-fail。
- 如果未来声明 RedisBloom full protocol compatibility，再补 RESP3 boolean/map。

## TCL 失败：SCANDUMP cursor 兼容

这是当前目标内失败：

```text
EXPECTED COMPAT GAP: SCANDUMP layer cursor should advance by byte length
  expected byte-offset cursor 129, got 2
```

相关实现位置：

- `modules/gemini-bloom/src/bloom_commands.cc:519-571`
- `modules/gemini-bloom/src/bloom_commands.cc:574-628`

含义：

- gemini 当前 cursor 是 layer index。
- RedisBloom v2.8.20 cursor 是 byte offset。
- gemini 自身 SCANDUMP/LOADCHUNK round-trip 会通过，但不能证明 RedisBloom 迁入迁出。

## RedisBloom 互通失败：SCANDUMP / LOADCHUNK

完整兼容性矩阵结果：

```text
BF.SCANDUMP / BF.LOADCHUNK: 0/18 passed
```

典型 chunk 序列：

```text
multi_exp2, RedisBloom -> gemini
RedisBloom chunks:
  [[1, 179], [17, 16], [49, 32], [121, 72], [0, 0]]
LOADCHUNK replies:
  OK, ERR cursor exceeds layer count, ERR cursor exceeds layer count, ERR cursor exceeds layer count
target check:
  card=40, expected_card=40, found=0/40

multi_exp2, gemini -> RedisBloom
gemini chunks:
  [[1, 179], [2, 128], [3, 128], [4, 128], [0, 0]]
LOADCHUNK replies:
  OK, ERR received bad data, ERR received bad data, ERR received bad data
target check:
  card=40, expected_card=40, found=0/40
```

empty corpus 也失败，因为 data chunk load 返回错误，不能因没有 membership item 而判通过：

```text
empty_scaling, RedisBloom -> gemini:
  chunks:    [[1, 73], [145, 144], [0, 0]]
  LOADCHUNK: OK, ERR cursor exceeds layer count

empty_scaling, gemini -> RedisBloom:
  chunks:    [[1, 73], [2, 144], [0, 0]]
  LOADCHUNK: OK, ERR received bad data
```

大 filter corpus 证明 RedisBloom v2.8.20 有 16MiB split，而 gemini 仍输出整层私有 chunk：

```text
large_empty_16mb, RedisBloom chunks:
  [[1, 73], [16777217, 16777216], [20677041, 3899824], [0, 0]]

large_empty_16mb, gemini chunks:
  [[1, 73], [2, 20677040], [0, 0]]
```

这说明 header shell 可能被创建，但 data chunks 没有正确写入，最终形成 `CARD` 正常但 membership 全部 false negative 的半恢复 key。

## RedisBloom 互通失败：command-AOF rewrite

完整兼容性矩阵结果：

```text
command-AOF rewrite: 0/18 passed
```

当关闭 RDB preamble，强制 module command-AOF rewrite 时，AOF 文件包含 `BF.LOADCHUNK`，双向回放失败：

```text
gemini command-AOF -> RedisBloom:
  non-empty corpus: target card matches source card, found=0/N
  empty corpus: membership check cannot expose false negative
  log: AOF-loading-client critical error after bf.loadchunk

RedisBloom command-AOF -> gemini:
  non-empty corpus: target card matches source card, found=0/N
  empty corpus: membership check cannot expose false negative
  log: AOF-loading-client critical error after BF.LOADCHUNK
```

对照组：Redis 6.2.17 默认 `aof-use-rdb-preamble yes` 时，AOF rewrite 文件是 RDB preamble，不包含 `BF.LOADCHUNK`；该路径在 18/18 个双向矩阵单元中通过。

## RedisBloom 互通失败：live replication `BF.CARD`

完整兼容性矩阵结果：

```text
live replication command stream: 16/18 passed
```

失败只出现在 `expansion1` corpus：

```text
RedisBloom master -> gemini replica:
  source BF.CARD=19, found=20/20
  target BF.CARD=20, found=20/20

gemini master -> RedisBloom replica:
  source BF.CARD=20, found=20/20
  target BF.CARD=19, found=20/20
```

membership 没有 false negative，但 `BF.CARD` 是可观察 API，因此 live command replay 不能声明完全兼容。对照组：fullsync replication 走 RDB snapshot，18/18 单元通过。

## RedisBloom 互通失败：incremental AOF `BF.CARD`

补充审计覆盖未触发 `BGREWRITEAOF` 的 append-only command stream。结果：

```text
incremental AOF command stream: 4 passed, 2 failed, 0 errors
```

通过项：

```text
single_layer:
  RedisBloom -> gemini:      card=8, found=8/8
  gemini -> RedisBloom:      card=8, found=8/8

fixed_full:
  RedisBloom -> gemini:      card=2, found=2/2
  gemini -> RedisBloom:      card=2, found=2/2
```

失败项仍是 `expansion1`：

```text
RedisBloom incremental AOF -> gemini:
  source BF.CARD=19, found=20/20
  target BF.CARD=20, found=20/20

gemini incremental AOF -> RedisBloom:
  source BF.CARD=20, found=20/20
  target BF.CARD=19, found=20/20
```

这说明 `BF.CARD` 分歧不是 replication 独有，而是所有 command replay 路径都会遇到。

## RedisBloom 互通失败：readonly replica `BF.SCANDUMP`

补充审计在同模块 master -> replica 下执行 `BF.SCANDUMP key 0`：

```text
COMMAND INFO BF.SCANDUMP
  gemini:     flags=[write]
  RedisBloom: flags=[readonly, fast]

gemini replica:
  BF.SCANDUMP key 0 -> READONLY You can't write against a read only replica.

RedisBloom replica:
  BF.SCANDUMP key 0 -> [1, <73-byte header>]
```

这会阻断从只读副本导出 Bloom key 的迁移方式。

## RedisBloom 互通失败：`BF.LOADCHUNK` header 覆盖已有 key

补充审计实际观察：

```text
before:
  dst old=1, new=0, BF.CARD=1

gemini:
  BF.LOADCHUNK dst 1 <src-header> -> OK
  after header: old=0, new=0, BF.CARD=1

RedisBloom v2.8.20:
  BF.LOADCHUNK dst 1 <src-header> -> ERR received bad data
  after header: old=1, new=0, BF.CARD=1
```

这是目标内安全/兼容失败：gemini 使用 public `BF.LOADCHUNK` header chunk 执行 replace，RedisBloom v2.8.20 不这样做。

## Redis server 迁移正向用例：MIGRATE 与 TTL RESTORE

补充审计新增通过项：

```text
MIGRATE COPY REPLACE with PEXPIRE:
  RedisBloom -> gemini:      target card=8, found=8/8, target PTTL ~= 599996 ms
  gemini -> RedisBloom:      target card=8, found=8/8, target PTTL ~= 599996 ms

DUMP/RESTORE with explicit TTL:
  RedisBloom -> gemini:      restore card=8, found=8/8, restore PTTL ~= 599999 ms
  gemini -> RedisBloom:      restore card=8, found=8/8, restore PTTL = 600000 ms
```

这些用例加强 RDB object 兼容的正向证据，但不改变 SCANDUMP/LOADCHUNK、command-AOF 和 command replay 的失败结论。

## RedisBloom oracle 差异：RESP2 / command semantics

本轮 Redis 6.2.17 + RedisBloom v2.8.20 oracle 实际观察到：

```text
BF.INFO info CAPACITY
  gemini:     100
  RedisBloom: [100]

BF.INFO info SIZE
  gemini:     440
  RedisBloom: [240]

BF.MADD fixed a b c d
  gemini:     [1, 1, ERR non scaling filter is full, ERR non scaling filter is full]
  RedisBloom: [1, 1, ERR non scaling filter is full]

BF.INSERT fixed_insert ITEMS a b c d
  gemini:     [1, 1, ERR non scaling filter is full, ERR non scaling filter is full]
  RedisBloom: [1, 1, ERR non scaling filter is full]

BF.INSERT nokey NOCREATE CAPACITY 10 ITEMS a
  gemini:     ERR NOCREATE cannot be used with CAPACITY or ERROR
  RedisBloom: ERR not found

BF.DEBUG debug_key
  gemini:     ERR unknown command
  RedisBloom: [size:1, bytes:16 bits:128 hashes:9 ...]

BF.RESERVE reserve_exp0 0.01 10 EXPANSION 0
  gemini:     OK
  RedisBloom: ERR expansion should be greater or equal to 1

BF.INSERT insert_exp0 EXPANSION 0 ITEMS a
  gemini:     [1]
  RedisBloom: ERR Bad argument received

BF.RESERVE reserve_unknown 0.01 10 UNKNOWN 1
  gemini:     ERR unrecognized option
  RedisBloom: OK

BF.INFO missing
  gemini:     ERR key does not exist
  RedisBloom: ERR not found

BF.INFO info_key FILTERS
  gemini:     1
  RedisBloom: [1]
```

这些不一定都阻断 RDB 类迁移，但会影响 RedisBloom client compatibility、监控口径和 differential correctness。

## RedisBloom oracle 差异：module identity 与 load args

补充审计观察：

```text
MODULE LIST
  gemini:     name=GeminiBloom, ver=1
  RedisBloom: name=bf, ver=20820

--loadmodule <module> INITIAL_SIZE 7 ERROR_RATE 0.05
  gemini:     starts; BF.ADD default key -> Capacity=7, Expansion rate=2
  RedisBloom: starts; BF.ADD default key -> Capacity=7, Expansion rate=2

--loadmodule <module> EXPANSION 4
  gemini:     starts; BF.ADD default key -> Expansion rate=4
  RedisBloom: module initialization failed with "Unrecognized option"

--loadmodule <module> CF_MAX_EXPANSIONS 8
  gemini:     module initialization failed with "Unrecognized config argument"
  RedisBloom: starts
```

这些是部署和 client probing 兼容差异，不影响当前 RDB object 迁入迁出结论。

## 环境失败：GTest discovery / linking

这些不是模块功能失败，但会影响验证可靠性：

```text
cmake --build /tmp/gemini-module-v5-docker-build --target bloom_filter_test
  failed: no rule to make target

Reason:
  container CMake did not find a normal GTest package, so test targets were not generated.

Attempted workaround:
  -DGTEST_INCLUDE_DIR=/usr/include/llvm-gtest
  -DGTEST_LIBRARY=/usr/lib64/libllvm_gtest.a
  -DGTEST_MAIN_LIBRARY=/usr/lib64/libllvm_gtest_main.a

Result:
  link failed with missing LLVM Support symbols such as llvm::raw_os_ostream.
```

最终采用 RocksDB bundled gtest fused sources 手工编译测试二进制。这是验证 workaround，不应作为长期 CI 入口。

## 历史环境问题：旧 build 和 macOS rpath

旧 v5 记录中还发现：

```text
cmake --build build --target bloom_test
  failed because build/CMakeCache.txt was generated for /workspace/projects/VibeCoding/gemini-module/build

tclsh ... build/redis_bloom.so
  Redis failed to load module: slice is not valid mach-o file

cmake --build /private/tmp/gemini-module-v5-build --target bloom_test
  compiled tests, then failed running bloom_filter_test:
  dyld: Library not loaded: @rpath/libgtest_main.1.11.0.dylib
```

本轮容器验证不依赖这些旧产物。
