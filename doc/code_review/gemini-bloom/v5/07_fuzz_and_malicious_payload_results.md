# 07 - Fuzz 与恶意 RDB/wire payload 审计结果

本文件固化 v5 对随机 fuzz、恶意 RDB payload、恶意 `BF.LOADCHUNK` wire payload 的补充验证。兼容性基线仍固定为：

```text
gemini side:     Redis 6.2.17 + /tmp/gemini-module-v5-2420-build/redis_bloom.so
RedisBloom side: Redis 6.2.17 + RedisBloom v2.4.20, MODULE LIST ver=20420
container:       974d83bcff5c (strange_feynman)
workspace:       /workspace/projects/VibeCoding/gemini-module
```

原始结果：

```text
doc/code_review/gemini-bloom/v5/rdb_wire_fuzz_results_redis62_redisbloom2420.json
doc/code_review/gemini-bloom/v5/rdb_wire_fuzz_asan_results_redis62_redisbloom2420.json
doc/code_review/gemini-bloom/v5/malicious_wire_audit_results_redis62_redisbloom2420.json
```

## Harness

新增 harness：

```text
doc/code_review/gemini-bloom/v5/bloom_rdb_wire_fuzz_audit.cc
doc/code_review/gemini-bloom/v5/redisbloom_malicious_wire_audit.py
```

C++ harness 直接覆盖 decoder：

- `RdbLoadBloom()`，使用 safe mock `RedisModuleIO`，避免恶意长度先触发测试 harness 自身巨大分配。
- `DeserializeHeader()`，覆盖 wire header metadata。
- 结构化恶意字段：截断、未知 encver、0/过多 layer、未知 flag、RawBits flag、scaling expansion 0、total item mismatch、capacity 0、fp NaN/Inf/0/1/负数、`bitsPerEntry` 0/Inf/负数、`hashCount` 0/不一致、`totalBits` 0、`log2Bits` 64/不匹配、blob/data size 过短/过长、`itemCount > capacity`、fixed expansion 0。
- 资源炸弹：RDB 声明 3GB blob length、wire 声明 3GB single-layer allocation。该项在普通版 harness 中通过 `RLIMIT_AS=768MB` 安全执行。
- 随机 fuzz：每个 decoder 100000 次，固定 seed `9697955846`；一半为 valid payload byte mutation，一半为随机 bytes。

Python 黑盒 harness 覆盖 Redis 命令层：

- 每个模块先创建 native Bloom key，并通过本模块 `BF.SCANDUMP` 获取 native header/data chunks。
- 对 `BF.LOADCHUNK key 1 <header>` 跑 native truncation、extra byte、2500 个 native mutation、2500 个随机 header、25 个 gemini 结构化 header。
- 对 data chunk 跑 7 个结构化长度 case、1000 个 mutation、1000 个随机 payload。
- 每个 case 后执行 `PING`，命令成功的加载对象再执行 `BF.INFO`、`BF.CARD`、`BF.EXISTS`、`DEL`，记录连接死亡、探测失败和删除后存活。
- 对已有 Bloom key 单独测试 header 写入是否覆盖旧内容。

## 运行命令

普通 decoder fuzz，含 3GB 资源炸弹：

```text
docker exec 974d83bcff5c \
  g++ -std=c++20 -O1 -g -DREDIS_BLOOM_TESTING \
    -I/workspace/projects/VibeCoding/gemini-module/modules/gemini-bloom/src \
    -I/workspace/projects/VibeCoding/gemini-module/include \
    /workspace/projects/VibeCoding/gemini-module/doc/code_review/gemini-bloom/v5/bloom_rdb_wire_fuzz_audit.cc \
    /workspace/projects/VibeCoding/gemini-module/modules/gemini-bloom/src/bloom_rdb.cc \
    /workspace/projects/VibeCoding/gemini-module/modules/gemini-bloom/src/sb_chain.cc \
    /workspace/projects/VibeCoding/gemini-module/modules/gemini-bloom/src/bloom_filter.cc \
    /workspace/projects/VibeCoding/gemini-module/modules/gemini-bloom/src/murmur2.cc \
    -o /tmp/gemini-module-v5-2420-rdb-wire-fuzz-audit

docker exec 974d83bcff5c \
  /tmp/gemini-module-v5-2420-rdb-wire-fuzz-audit \
    --iterations 100000 \
    --memory-limit \
    --include-resource-bombs \
    --output /workspace/projects/VibeCoding/gemini-module/doc/code_review/gemini-bloom/v5/rdb_wire_fuzz_results_redis62_redisbloom2420.json
```

ASAN/UBSAN decoder fuzz：

```text
docker exec 974d83bcff5c \
  clang++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -DREDIS_BLOOM_TESTING \
    -I/workspace/projects/VibeCoding/gemini-module/modules/gemini-bloom/src \
    -I/workspace/projects/VibeCoding/gemini-module/include \
    /workspace/projects/VibeCoding/gemini-module/doc/code_review/gemini-bloom/v5/bloom_rdb_wire_fuzz_audit.cc \
    /workspace/projects/VibeCoding/gemini-module/modules/gemini-bloom/src/bloom_rdb.cc \
    /workspace/projects/VibeCoding/gemini-module/modules/gemini-bloom/src/sb_chain.cc \
    /workspace/projects/VibeCoding/gemini-module/modules/gemini-bloom/src/bloom_filter.cc \
    /workspace/projects/VibeCoding/gemini-module/modules/gemini-bloom/src/murmur2.cc \
    -o /tmp/gemini-module-v5-2420-rdb-wire-fuzz-audit-asan

docker exec \
  -e ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  -e UBSAN_OPTIONS=halt_on_error=1 \
  974d83bcff5c \
  /tmp/gemini-module-v5-2420-rdb-wire-fuzz-audit-asan \
    --iterations 100000 \
    --output /workspace/projects/VibeCoding/gemini-module/doc/code_review/gemini-bloom/v5/rdb_wire_fuzz_asan_results_redis62_redisbloom2420.json
```

黑盒恶意 wire payload：

```text
docker exec 974d83bcff5c \
  python3 /workspace/projects/VibeCoding/gemini-module/doc/code_review/gemini-bloom/v5/redisbloom_malicious_wire_audit.py \
    --gemini-module /tmp/gemini-module-v5-2420-build/redis_bloom.so \
    --redis-server /workspace/projects/Environments/OpenSourceRedis/redis-6.2-redisbloom/bin/redis-server \
    --redisbloom-module /tmp/redisbloom-v2.4.20/bin/linux-x64-release/redisbloom.so \
    --random-cases 5000 \
    --data-random-cases 2000 \
    --output /workspace/projects/VibeCoding/gemini-module/doc/code_review/gemini-bloom/v5/malicious_wire_audit_results_redis62_redisbloom2420.json
```

## Decoder fuzz 结果

普通版结果：

```text
seed:                    9697955846
iterations_per_decoder:  100000
memory_limit_enabled:    true
resource_bombs_enabled:  true

RDB structured:           35 cases, 3 expected accept, 32 expected reject
                          false reject=0, unsafe accept=2, invariant violation=2
Wire structured:          33 cases, 2 expected accept, 31 expected reject
                          false reject=0, unsafe accept=2, invariant violation=2
RDB random fuzz:          100000 cases, accepted=2930, invariant violation=294
Wire random fuzz:         100000 cases, accepted=4543, invariant violation=1287
```

结构化 unsafe accept 只有两类，并且 RDB 与 wire 都复现：

```text
bits_per_entry_zero
hash_count_inconsistent
```

其他结构化恶意字段均按预期拒绝，包括截断流、unknown encver、未知 flag、RawBits flag、NaN/Inf/非法 fp、0 capacity、0 hash、0 total bits、log2 mismatch、blob/data length mismatch、`itemCount > capacity`、RDB 3GB declared blob length、wire 3GB dataSize 资源炸弹。

ASAN/UBSAN 版结果：

```text
seed:                    9697955846
iterations_per_decoder:  100000
memory_limit_enabled:    false
resource_bombs_enabled:  false

RDB structured:           unsafe accept=2, invariant violation=2
Wire structured:          unsafe accept=2, invariant violation=2
RDB random fuzz:          100000 cases, accepted=2930, invariant violation=294
Wire random fuzz:         100000 cases, accepted=4543, invariant violation=1287
sanitizer result:         no ASAN/UBSAN abort
```

注意：随机 fuzz 的 `accepted` 不直接等于缺陷。这里作为缺陷计入的是 accepted 后违反不变量的 payload。随机 fuzz 进一步证明 `bitsPerEntry`、`hashCount`、极端 metadata 组合仍能被 decoder 接受并形成非法内部状态。

## 黑盒 `BF.LOADCHUNK` 结果

黑盒 harness seed 为 `9697955847`，每个模块执行：

```text
header cases: 5032
  native structured: 7
  native mutation:   2500
  random header:     2500
  gemini structured: 25

data cases:   2007
  structured length: 7
  data mutation:     1000
  random data:       1000

existing-key header cases: 12
```

gemini 结果：

```text
header:       5032 cases, ok=284, error=4748, connection_error=0
data:         2007 cases, ok=1002, error=1005, connection_error=0
existing key: 12 cases, ok=4, error=8, old_preserved=8, old_lost=4
critical log: 0
```

gemini 结构化 header 中被接受的 4 个 case：

```text
gemini_valid_empty_header
gemini_bits_per_entry_zero
gemini_hash_count_inconsistent
gemini_fixed_expansion_zero
```

其中 `bits_per_entry_zero` 与 `hash_count_inconsistent` 是 decoder 不变量缺陷；`valid_empty_header` 是合法空 header；`fixed_expansion_zero` 是 fixed-size filter 的允许语义。对已有 Bloom key 执行这些 header case 时，gemini 均返回 `OK` 并覆盖旧 key，`old` item 从 `BF.EXISTS == 1` 变为 `0`。这与 RedisBloom v2.4.20 的保护语义不同。

RedisBloom v2.4.20 结果：

```text
header:       5032 cases, ok=184, error=4845, connection_error=3
data:         2007 cases, ok=1036, error=971, connection_error=0
existing key: 12 cases, ok=1, error=11, old_preserved=12, old_lost=0
critical log: 0
```

RedisBloom v2.4.20 在 native header mutation 下出现 3 个连接死亡 case，脚本记录并重启后继续执行：

```text
native_mutation_00172
native_mutation_00974
native_mutation_01048
```

该问题属于 RedisBloom v2.4.20 oracle 自身在恶意 payload 下的稳定性现象，不是 gemini 的兼容性失败；但它说明 malicious wire payload 测试不能只看正常 RedisBloom 行为。对 gemini 的结论是：本轮黑盒恶意 `BF.LOADCHUNK` 没有发现进程死亡、probe 后死亡或 delete 后死亡，但发现了非法 header accepted 与已有 key 覆盖语义差异。

## 审计结论

基于本轮新增 fuzz，v5 可以声明覆盖了以下范围：

- 结构化恶意 RDB metadata。
- 结构化恶意 wire header metadata。
- RDB/wire decoder 100000 次随机 fuzz，各自含 valid mutation 与 random bytes。
- ASAN/UBSAN decoder fuzz。
- 3GB 声明长度/声明 dataSize 的资源炸弹，安全地在 harness/RLIMIT 下验证。
- Redis 命令层 `BF.LOADCHUNK` header/data payload 黑盒 fuzz。
- 已有 key 被 malicious header 写入时的保护/覆盖行为。

v5 不能声明“完全兼容 RedisBloom v2.4.20”。新增 fuzz 加强了原结论：

1. RDB / DUMP / RESTORE / RDB-preamble AOF / fullsync replication 在本轮 corpus 内双向通过。
2. `BF.SCANDUMP` / `BF.LOADCHUNK` public wire protocol 仍不兼容。
3. command-AOF rewrite 仍不兼容。
4. live replication / incremental AOF 在 `EXPANSION 1` 下仍有 `BF.CARD` 可观察分歧。
5. RDB/wire decoder 仍接受 `bitsPerEntry == 0` 和 `hashCount` 不一致的非法状态。
6. `BF.LOADCHUNK cursor=1` 对已有 key 的覆盖行为仍不同于 RedisBloom v2.4.20。

仍未被本轮覆盖、因此不能声称的范围：

- RedisBloom v2.4.20 之外的版本。
- Redis 8 内置 Bloom。
- RESP3。
- 长时间持续 fuzz、coverage-guided fuzz 或跨平台 sanitizer matrix。
- Redis process 级 3GB/更大 payload resource bomb；本轮只在 decoder harness + RLIMIT 下覆盖，避免把 Redis server 或宿主机拖入 OOM。
