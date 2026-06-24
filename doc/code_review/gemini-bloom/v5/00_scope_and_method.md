# 00 - 审计范围与方法

## 审计对象

审计对象为当前工作区：

- 核心 Bloom layer：`modules/gemini-bloom/src/bloom_filter.*`
- Scaling Bloom chain：`modules/gemini-bloom/src/sb_chain.*`
- Redis 命令层：`modules/gemini-bloom/src/bloom_commands.cc`
- RDB / DUMP/RESTORE / SCANDUMP / LOADCHUNK / AOF / replication：`modules/gemini-bloom/src/bloom_rdb.*`
- 模块入口和配置：`modules/gemini-bloom/src/redis_bloom_module.cc`、`modules/gemini-bloom/src/bloom_config.*`
- 测试：`modules/gemini-bloom/tests/*.cc`、`modules/gemini-bloom/tests/tcl/bloom_test.tcl`

## 产品目标假设

本轮按以下目标判断正确性：

- 需要支持与 RedisBloom 互通，包括迁入和迁出。
- 需要支持 RDB、DUMP/RESTORE、AOF、SCANDUMP/LOADCHUNK、replication 的互通或明确声明不互通。
- 不需要支持 RESP3。因此 RESP3 raw reply shape 不作为 P0/P1 阻断项。

## 已执行测试

本轮按用户要求在 Docker container `974d83bcff5c` (`strange_feynman`) 内编译、运行、测试。路径映射：

```text
macOS:     /Users/liuyu/centos_ex/projects/VibeCoding/gemini-module
container: /workspace/projects/VibeCoding/gemini-module
```

隔离构建，不使用仓库内旧 `build/`：

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
```

容器内 CMake 没有生成 GTest targets，因为没有普通 `GTest` CMake package。尝试强行指向 LLVM gtest 静态库会因缺少 LLVM Support 符号而链接失败。因此本轮用容器内 RocksDB 附带的普通 gtest 1.8 fused sources 手工编译三个测试二进制：

```text
/tmp/gemini-module-v5-bloom_filter_test
/tmp/gemini-module-v5-sb_chain_test
/tmp/gemini-module-v5-bloom_rdb_test
```

RedisBloom oracle 固定使用 Redis 6.2.17 + RedisBloom v2.8.20：

```text
redis-server:
  /workspace/projects/Environments/OpenSourceRedis/redis-6.2-redisbloom/bin/redis-server
  Redis server v=6.2.17

RedisBloom source:
  /tmp/redisbloom-v2.8.20
  tag v2.8.20, commit 7aa71f3

RedisBloom module:
  /tmp/redisbloom-v2.8.20/bin/linux-x64-release/redisbloom.so
  MODULE LIST: name=bf, ver=20820
```

RedisBloom v2.8.20 构建命令：

```text
docker exec 974d83bcff5c \
  git -C /workspace/projects/VibeCoding/ValBloom/reference/redis-bloom fetch origin tag v2.8.20

docker exec 974d83bcff5c \
  git -C /workspace/projects/VibeCoding/ValBloom/reference/redis-bloom \
    worktree add --detach /tmp/redisbloom-v2.8.20 v2.8.20

docker exec 974d83bcff5c \
  git -C /tmp/redisbloom-v2.8.20 submodule update --init --recursive

docker exec 974d83bcff5c \
  make -C /tmp/redisbloom-v2.8.20 build
```

完整兼容性矩阵：

```text
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
```

补充审计矩阵：

```text
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

结果：

```text
TCL integration:                 137 passed, 6 failed
manual bloom_filter_test:         27 passed
manual sb_chain_test:             15 passed with ExtremeParamsRejected filtered
manual bloom_rdb_test:            54 passed, 4 failed
RedisBloom interop probe:         RDB simple corpus passed both directions;
                                  RDB-preamble AOF passed both directions;
                                  SCANDUMP/LOADCHUNK failed both directions;
                                  command-AOF failed both directions.
RedisBloom v2.8.20 matrix:        126 compatibility cells, 88 passed, 38 failed, 0 errors
                                  Full test/result record:
                                  doc/code_review/gemini-bloom/v5/06_redis62_redisbloom2820_compat_results.md
RedisBloom extended audit:        incremental AOF 4 pass / 2 fail / 0 errors
                                  MIGRATE + TTL restore 2 pass / 0 fail / 0 errors
                                  BF.DEBUG missing in gemini
                                  readonly replica SCANDUMP fails only on gemini
                                  LOADCHUNK header over existing key differs
```

仓库内旧 `build/redis_bloom.so` 不作为本轮依据。本轮所有运行结果均使用容器内 `/tmp/gemini-module-v5-docker-build/redis_bloom.so`。

## 兼容性矩阵覆盖

本轮 Redis 6.2 + RedisBloom v2.8.20 矩阵覆盖：

```text
corpora:
  empty_scaling
  single_layer
  multi_exp2
  fixed_full
  expansion1
  expansion4
  binary_items
  long_item
  large_empty_16mb

paths:
  RDB RedisBloom -> gemini
  RDB gemini -> RedisBloom
  DUMP/RESTORE RedisBloom -> gemini
  DUMP/RESTORE gemini -> RedisBloom
  SCANDUMP/LOADCHUNK RedisBloom -> gemini
  SCANDUMP/LOADCHUNK gemini -> RedisBloom
  command-AOF RedisBloom -> gemini
  command-AOF gemini -> RedisBloom
  RDB-preamble AOF RedisBloom -> gemini
  RDB-preamble AOF gemini -> RedisBloom
  live replication RedisBloom master -> gemini replica
  live replication gemini master -> RedisBloom replica
  fullsync replication RedisBloom master -> gemini replica
  fullsync replication gemini master -> RedisBloom replica
```

路径汇总：

```text
RDB file load/save:        18/18 passed
DUMP/RESTORE:              18/18 passed
SCANDUMP/LOADCHUNK:         0/18 passed
command-AOF rewrite:        0/18 passed
RDB-preamble AOF rewrite:  18/18 passed
live replication:          16/18 passed
fullsync replication:      18/18 passed
```

## 补充审计覆盖

`redisbloom_extended_audit.py` 覆盖主矩阵未展开的兼容面：

```text
COMMAND INFO:
  BF.ADD / BF.MADD / BF.INSERT / BF.INFO / BF.CARD /
  BF.SCANDUMP / BF.LOADCHUNK / BF.DEBUG

readonly replica:
  在同模块 master -> replica 上执行 BF.SCANDUMP

LOADCHUNK edge:
  对已有 Bloom key 执行 BF.LOADCHUNK key 1 <header>

incremental AOF command stream:
  single_layer / expansion1 / fixed_full
  RedisBloom -> gemini, gemini -> RedisBloom

Redis server migration helpers:
  MIGRATE COPY REPLACE with TTL
  DUMP/RESTORE with explicit TTL

command semantics:
  missing key, wrongtype, duplicate item, EXPANSION 0,
  unknown option, BF.INFO unknown/field reply

module load args:
  INITIAL_SIZE / ERROR_RATE
  EXPANSION
  CF_MAX_EXPANSIONS
```

补充审计结果摘要：

```text
incremental AOF:                  4 passed, 2 failed, 0 errors
MIGRATE + TTL DUMP/RESTORE:       2 passed, 0 failed, 0 errors
BF.DEBUG command:                 RedisBloom present, gemini missing
readonly replica BF.SCANDUMP:     RedisBloom OK, gemini READONLY error
LOADCHUNK header over old key:    RedisBloom rejects header, gemini replaces key
```

## 严重级别

- P0：阻断 RedisBloom 迁入迁出、可能造成 silent corruption、恢复错误或崩溃。
- P1：明确的 RedisBloom 兼容差异、错误业务结果、持久化语义错误、明显安全边界缺失。
- P2：测试覆盖不足、边界行为未固定、实现维护风险高。
- P3：文档、组织、可观测性或长期演进问题。
