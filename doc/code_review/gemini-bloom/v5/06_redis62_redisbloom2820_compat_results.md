# 06 - Redis 6.2 + RedisBloom v2.8.20 兼容性测试结果

本文件固化本轮兼容性测试覆盖范围和结果。兼容性目标固定为：

```text
gemini side:     Redis 6.2.17 + /tmp/gemini-module-v5-docker-build/redis_bloom.so
RedisBloom side: Redis 6.2.17 + RedisBloom v2.8.20, MODULE LIST ver=20820
container:       974d83bcff5c (strange_feynman)
workspace:       /workspace/projects/VibeCoding/gemini-module
result JSON:     doc/code_review/gemini-bloom/v5/compat_matrix_results_redis62_redisbloom2820.json
extended JSON:   doc/code_review/gemini-bloom/v5/extended_audit_results_redis62_redisbloom2820.json
```

RESP3 不属于本轮必需兼容目标。RedisBloom 其他版本、Redis 8 内置 Bloom 不属于本文件结论范围。

## 运行命令

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

脚本语法检查：

```text
docker exec 974d83bcff5c \
  python3 -m py_compile /workspace/projects/VibeCoding/gemini-module/doc/code_review/gemini-bloom/v5/redisbloom_compat_matrix.py
```

补充审计：

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

docker exec 974d83bcff5c \
  python3 -m py_compile /workspace/projects/VibeCoding/gemini-module/doc/code_review/gemini-bloom/v5/redisbloom_extended_audit.py
```

## 覆盖矩阵

Corpus 覆盖：

```text
empty_scaling       empty scaling filter
single_layer        8 inserted items, single layer
multi_exp2          40 inserted items, EXPANSION 2
fixed_full          NONSCALING filter at capacity
expansion1          EXPANSION 1, captures BF.CARD false-positive behavior
expansion4          EXPANSION 4
binary_items        empty bulk, NUL, CRLF, whitespace, Tcl-special bytes, raw 1..31 bytes
long_item           one 64KiB item
large_empty_16mb    empty large filter, forces RedisBloom 16MiB SCANDUMP split
```

Path 覆盖：

```text
RDB file load/save                    RedisBloom -> gemini, gemini -> RedisBloom
DUMP/RESTORE                          RedisBloom -> gemini, gemini -> RedisBloom
BF.SCANDUMP / BF.LOADCHUNK            RedisBloom -> gemini, gemini -> RedisBloom
command-AOF rewrite                   RedisBloom -> gemini, gemini -> RedisBloom
RDB-preamble AOF rewrite              RedisBloom -> gemini, gemini -> RedisBloom
live replication command stream       RedisBloom master -> gemini replica, gemini master -> RedisBloom replica
fullsync replication snapshot         RedisBloom master -> gemini replica, gemini master -> RedisBloom replica
```

总计：9 corpus x 7 path families x 2 directions = 126 个兼容性单元。

判定规则：

- 目标端不能有 Redis error reply、AOF-loading-client critical log 或脚本级 error。
- 目标端 `BF.CARD` 必须等于源端迁移前 `BF.CARD`。
- 所有 inserted items 在目标端必须 `BF.EXISTS == 1`。
- `BF.CARD` 不强制等于 inserted item 数，因为 Bloom filter 可能因 false positive 使 `BF.ADD` 返回 0；例如 RedisBloom v2.8.20 在 `expansion1` corpus 中源端 `BF.CARD=19`，但 20 个 inserted items 都可查到。

## 结果总览

```text
total cells: 126
passed:      88
failed:      38
errors:      0
```

| Path family | Cells | Pass | Fail | 结论 |
| --- | ---: | ---: | ---: | --- |
| RDB file load/save | 18 | 18 | 0 | 双向通过 |
| DUMP/RESTORE | 18 | 18 | 0 | 双向通过 |
| BF.SCANDUMP / BF.LOADCHUNK | 18 | 0 | 18 | 双向全部失败 |
| command-AOF rewrite | 18 | 0 | 18 | 双向全部失败 |
| RDB-preamble AOF rewrite | 18 | 18 | 0 | 双向通过 |
| live replication command stream | 18 | 16 | 2 | `expansion1` 双向 `BF.CARD` 分歧 |
| fullsync replication snapshot | 18 | 18 | 0 | 双向通过 |

补充审计摘要：

```text
incremental AOF command stream: 6 cells, 4 pass, 2 fail, 0 errors
MIGRATE + TTL restore:         2 directions, 2 pass, 0 fail, 0 errors
BF.DEBUG:                      RedisBloom present, gemini missing
readonly replica SCANDUMP:     RedisBloom OK, gemini READONLY error
LOADCHUNK header over old key: RedisBloom rejects, gemini replaces
```

## 关键正向结论

在 Redis 6.2.17 + RedisBloom v2.8.20 范围内，以下迁移方式通过 9 个 corpus 的双向验证：

- RDB 文件保存/加载。
- Redis `DUMP` / `RESTORE`。
- Redis `MIGRATE COPY REPLACE`，并保留目标端 TTL。
- `DUMP` / `RESTORE` 显式 TTL，双向保留 TTL。
- Redis 6.2 默认 `aof-use-rdb-preamble yes` 的 AOF rewrite。
- replication fullsync 产生的 RDB snapshot。

这说明当前 `MBbloom--` RDB encoding 在本轮 corpus 内与 RedisBloom v2.8.20 兼容。该结论不能外推到 RedisBloom 其他版本或 Redis 8 Bloom。

补充正向结果：

```text
MIGRATE rb -> gemini:
  target card=8, found=8/8, target PTTL ~= 599996 ms

MIGRATE gemini -> RedisBloom:
  target card=8, found=8/8, target PTTL ~= 599996 ms

DUMP/RESTORE with TTL:
  rb -> gemini restore PTTL ~= 599999 ms
  gemini -> RedisBloom restore PTTL = 600000 ms

module load args INITIAL_SIZE 7 ERROR_RATE 0.05:
  both modules start; BF.ADD default filter has Capacity=7, Expansion rate=2
```

## 关键失败结论

`BF.SCANDUMP` / `BF.LOADCHUNK` 18/18 失败。典型样例：

```text
multi_exp2, RedisBloom -> gemini
RedisBloom chunks: [[1, 179], [17, 16], [49, 32], [121, 72], [0, 0]]
LOADCHUNK:         OK, ERR cursor exceeds layer count, ERR cursor exceeds layer count, ERR cursor exceeds layer count
target check:      card=40, expected_card=40, found=0/40

multi_exp2, gemini -> RedisBloom
gemini chunks:     [[1, 179], [2, 128], [3, 128], [4, 128], [0, 0]]
LOADCHUNK:         OK, ERR received bad data, ERR received bad data, ERR received bad data
target check:      card=40, expected_card=40, found=0/40
```

即使 empty corpus 没有 membership 可查，也因为 data chunk load error 判为失败：

```text
empty_scaling, RedisBloom -> gemini
chunks:    [[1, 73], [145, 144], [0, 0]]
LOADCHUNK: OK, ERR cursor exceeds layer count

empty_scaling, gemini -> RedisBloom
chunks:    [[1, 73], [2, 144], [0, 0]]
LOADCHUNK: OK, ERR received bad data
```

大 filter 证明 RedisBloom v2.8.20 使用 16MiB split，而 gemini 仍输出整层私有 chunk：

```text
large_empty_16mb, RedisBloom chunks:
  [[1, 73], [16777217, 16777216], [20677041, 3899824], [0, 0]]

large_empty_16mb, gemini chunks:
  [[1, 73], [2, 20677040], [0, 0]]
```

`command-AOF rewrite` 18/18 失败。关闭 `aof-use-rdb-preamble` 后，AOF rewrite 使用 `BF.LOADCHUNK`，因此复现同一协议不兼容：

```text
non-empty corpus:
  target BF.CARD matches source card
  inserted items found=0/N
  Redis log has AOF-loading-client critical errors

empty / large-empty corpus:
  membership check cannot expose false negative
  Redis log still has AOF-loading-client critical errors
```

`live replication command stream` 有 2/18 失败，均来自 `expansion1`：

```text
RedisBloom master -> gemini replica:
  source card=19, found=20/20
  target card=20, found=20/20

gemini master -> RedisBloom replica:
  source card=20, found=20/20
  target card=19, found=20/20
```

这不是 membership false negative，但 `BF.CARD` 是 RedisBloom API，可观察状态在 live command replay 下不一致。RDB、DUMP/RESTORE、RDB-preamble AOF 和 fullsync replication 对同一 corpus 都通过。

`incremental AOF command stream` 复现同一类 `BF.CARD` 分歧。它不是 AOF rewrite；源端只把 `BF.RESERVE` / `BF.ADD` command stream 追加到 AOF：

```text
single_layer:
  rb -> gemini:       card=8, found=8/8
  gemini -> RedisBloom: card=8, found=8/8

fixed_full:
  rb -> gemini:       card=2, found=2/2
  gemini -> RedisBloom: card=2, found=2/2

expansion1:
  RedisBloom incremental AOF -> gemini:
    source card=19, target card=20, found=20/20
  gemini incremental AOF -> RedisBloom:
    source card=20, target card=19, found=20/20
```

`BF.SCANDUMP` 命令元数据与只读副本行为不兼容：

```text
COMMAND INFO BF.SCANDUMP
  gemini:     flags=[write]
  RedisBloom: flags=[readonly, fast]

gemini replica:
  BF.SCANDUMP key 0 -> READONLY You can't write against a read only replica.

RedisBloom replica:
  BF.SCANDUMP key 0 -> [1, <73-byte header>]
```

`BF.LOADCHUNK key 1 <header>` 覆盖已有 Bloom key 的语义不兼容：

```text
before:
  dst contains old=1, new=0, card=1

gemini:
  LOADCHUNK dst 1 <header> -> OK
  after header: old=0, new=0, card=1

RedisBloom:
  LOADCHUNK dst 1 <header> -> ERR received bad data
  after header: old=1, new=0, card=1
```

## 命令语义 oracle

同一 RESP2 命令 trace 下观察到以下差异：

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

BF.RESERVE reserve_exp0 0.01 10 EXPANSION 0
  gemini:     OK
  RedisBloom: ERR expansion should be greater or equal to 1

BF.INSERT insert_exp0 EXPANSION 0 ITEMS a
  gemini:     [1]
  RedisBloom: ERR Bad argument received

BF.INFO missing
  gemini:     ERR key does not exist
  RedisBloom: ERR not found

BF.DEBUG key
  gemini:     ERR unknown command
  RedisBloom: [size:..., bytes:...]

MODULE LIST
  gemini:     name=GeminiBloom, ver=1
  RedisBloom: name=bf, ver=20820
```

这些差异不影响已通过的 RDB 类迁移，但影响 RedisBloom client compatibility、监控口径和 live command replay 语义。
