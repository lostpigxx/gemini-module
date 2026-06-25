# gemini-bloom v5 正确性审计

本目录记录 v5 审计结果。审计背景：`gemini-bloom` 是一个需要与 RedisBloom 互通的 Bloom Filter 模块，核心目标包括 RedisBloom 数据迁入迁出；本轮不把 RESP3 作为必需兼容目标。

## 文件索引

- `00_scope_and_method.md`：审计范围、命令、测试环境和严重级别。
- `01_function_correctness_gaps.md`：命令语义和运行时功能正确性缺口。
- `02_redisbloom_interop_gaps.md`：RedisBloom RDB / DUMP/RESTORE / AOF / SCANDUMP / LOADCHUNK / replication 互通缺口。
- `03_persistence_and_safety_gaps.md`：RDB、wire header、LOADCHUNK 和资源失败安全缺口。
- `04_test_validation_gaps.md`：测试体系和验证方法缺口。
- `05_known_failing_cases.md`：本轮实际执行得到的失败用例和环境失败。
- `06_redis62_redisbloom2420_compat_results.md`：Redis 6.2 + RedisBloom v2.4.20 完整兼容性测试覆盖与结果。
- `07_fuzz_and_malicious_payload_results.md`：随机 fuzz、恶意 RDB/wire payload、黑盒 `BF.LOADCHUNK` payload 审计结果。
- `redisbloom_interop_probe.py`：单 corpus 双实例互通探针。
- `redisbloom_compat_matrix.py`：Redis 6.2 + RedisBloom v2.4.20 完整兼容性矩阵 runner。
- `redisbloom_extended_audit.py`：补充命令元数据、只读副本、增量 AOF、MIGRATE、TTL、module args、LOADCHUNK 边界的审计 runner。
- `redisbloom_malicious_wire_audit.py`：黑盒恶意 `BF.LOADCHUNK` wire payload 审计 runner。
- `bloom_rdb_wire_fuzz_audit.cc`：RDB/wire decoder 结构化 payload 与随机 fuzz harness。
- `compat_matrix_results_redis62_redisbloom2420.json`：Redis 6.2 + RedisBloom v2.4.20 正式矩阵结果。
- `extended_audit_results_redis62_redisbloom2420.json`：补充审计结果。
- `rdb_wire_fuzz_results_redis62_redisbloom2420.json`：RDB/wire decoder fuzz 普通版结果，含资源炸弹。
- `rdb_wire_fuzz_asan_results_redis62_redisbloom2420.json`：RDB/wire decoder ASAN/UBSAN fuzz 结果。
- `malicious_wire_audit_results_redis62_redisbloom2420.json`：黑盒恶意 `BF.LOADCHUNK` wire payload 结果。

## 顶层结论

当前 v5 兼容性审计环境固定为 `Redis 6.2.17 + gemini-bloom` 对比 `Redis 6.2.17 + RedisBloom v2.4.20`。在该范围内，主兼容性矩阵已覆盖 9 个 corpus、7 类路径、双向迁移/复制，共 126 个兼容性单元，并额外覆盖 RESP2 命令语义 oracle。结果：88 pass、38 fail、0 unknown/error。补充审计又覆盖命令注册元数据、只读副本 `BF.SCANDUMP`、`BF.LOADCHUNK` 覆盖已有 key、增量 AOF、`MIGRATE`、带 TTL 的 `DUMP/RESTORE`、module load args 和更多错误语义。新增 fuzz 审计覆盖 RDB/wire decoder 结构化恶意 payload、每个 decoder 100000 次随机 fuzz、ASAN/UBSAN fuzz、3GB 声明长度资源炸弹，以及 Redis 命令层 `BF.LOADCHUNK` header/data payload 黑盒 fuzz。完整测试项和结果固化在 `06_redis62_redisbloom2420_compat_results.md` 与 `07_fuzz_and_malicious_payload_results.md`，原始 JSON 固化在 `compat_matrix_results_redis62_redisbloom2420.json`、`extended_audit_results_redis62_redisbloom2420.json`、`rdb_wire_fuzz_results_redis62_redisbloom2420.json`、`rdb_wire_fuzz_asan_results_redis62_redisbloom2420.json` 和 `malicious_wire_audit_results_redis62_redisbloom2420.json`。

当前实现能证明 gemini-bloom 基础命令和自身 RDB 路径大体自洽；在 RedisBloom v2.4.20 的矩阵 corpus 上，RDB、`DUMP/RESTORE`、`MIGRATE`、带 TTL 的 `DUMP/RESTORE`、Redis 6.2 默认 AOF RDB-preamble、replication fullsync 双向迁移通过。但 RedisBloom 迁入迁出仍不能声明完整正确，因为 public `BF.SCANDUMP` / `BF.LOADCHUNK` 与 command-AOF rewrite 已被 Redis 6.2 + RedisBloom v2.4.20 oracle 证明会产生 false negative、chunk load error 或 AOF-loading-client critical error；live replication 和增量 AOF command stream 还存在 `EXPANSION 1` 下 `BF.CARD` 可观察分歧。

P0/P1 阻断项：

1. `BF.SCANDUMP` / `BF.LOADCHUNK` 使用 layer-index 私有协议，不是 RedisBloom 的 byte-offset chunk 协议；RedisBloom v2.4.20 -> gemini 与 gemini -> RedisBloom v2.4.20 在 18/18 个 SCANDUMP 单元上失败。
2. command-AOF rewrite 使用公共 `BF.LOADCHUNK` 承载私有 chunk 语义；关闭 `aof-use-rdb-preamble` 后 18/18 个 command-AOF 单元失败，并在加载日志中出现 AOF-loading-client critical errors。
3. `BF.SCANDUMP` 注册为 `write` 而不是 RedisBloom 的 `readonly fast`，因此 gemini 只读 replica 上 `BF.SCANDUMP` 返回 `READONLY`，RedisBloom v2.4.20 能正常导出 chunk。
4. live replication 与增量 AOF command stream 在 `EXPANSION 1` corpus 上双向出现 `BF.CARD` 分歧；membership 不丢失，但 RedisBloom API 可观察状态不一致。
5. `BF.LOADCHUNK key 1 <header>` 对已有 Bloom key 的行为不兼容：gemini 返回 `OK` 并立即替换已有 key；RedisBloom v2.4.20 返回 `ERR received bad data`，header 阶段不会删除旧内容。
6. 模块注册 RedisBloom data type name `MBbloom--`。Redis 6.2 + RedisBloom v2.4.20 的 RDB、`DUMP/RESTORE`、`MIGRATE`、TTL restore、RDB-preamble AOF、fullsync replication 双向通过是正向信号，但不能掩盖 SCANDUMP/LOADCHUNK 和 command-AOF 失败。
7. RDB / wire header 校验仍接受部分非法状态；GTest、结构化 fuzz、随机 fuzz 均复现 `bitsPerEntry == 0`、`hashCount` 与 `bitsPerEntry` 不一致。
8. 功能语义仍有 RedisBloom oracle 差异：缺少 `BF.DEBUG`、`BF.INFO key FIELD` RESP2 shape、`BF.INFO Size` 数值、`BF.INSERT EXPANSION 0`、`BF.RESERVE` 未知 option、missing-key 错误文案、fixed filter `BF.MADD` / `BF.INSERT` partial failure 数组长度、`BF.INSERT NOCREATE + CAPACITY` 错误语义。
9. 极端容量/误差率路径仍可触发巨大分配；`sb_chain_test` 的 `ExtremeParamsRejected` 在当前 Docker/Rosetta 环境中 abort。

## 本轮测试结果

容器：`974d83bcff5c` (`strange_feynman`)

```text
macOS workspace: /Users/liuyu/centos_ex/projects/VibeCoding/gemini-module
container path:  /workspace/projects/VibeCoding/gemini-module
build dir:       /tmp/gemini-module-v5-2420-build

redis-server:    6.2.17
RedisBloom:      v2.4.20, MODULE LIST ver=20420
                /tmp/redisbloom-v2.4.20/bin/linux-x64-release/redisbloom.so
gemini module:   /tmp/gemini-module-v5-2420-build/redis_bloom.so
```

```text
TCL integration:                137 passed, 6 failed
manual bloom_filter_test:        27 passed
manual sb_chain_test:            15 passed with ExtremeParamsRejected filtered;
                                 full run aborts in ExtremeParamsRejected
manual bloom_rdb_test:           54 passed, 4 failed
RedisBloom matrix:              126 compatibility cells: 88 passed, 38 failed, 0 errors
                                 9 corpora x 7 path families x 2 directions
                                 Redis 6.2.17 + RedisBloom v2.4.20
RedisBloom extended audit:       incremental AOF 4 pass / 2 fail / 0 errors
                                 MIGRATE + TTL restore 2 pass / 0 fail / 0 errors
                                 BF.DEBUG missing in gemini
                                 readonly replica SCANDUMP fails only on gemini
                                 LOADCHUNK header over existing key differs
RDB/wire decoder fuzz:           structured RDB 35 cases, unsafe accept=2
                                 structured wire 33 cases, unsafe accept=2
                                 random RDB 100000 cases, invariant violation=294
                                 random wire 100000 cases, invariant violation=1287
                                 ASAN/UBSAN run completed without sanitizer abort
malicious BF.LOADCHUNK fuzz:     gemini header 5032 cases, connection_error=0
                                 gemini data 2007 cases, connection_error=0
                                 gemini existing-key old_lost=4/12
                                 RedisBloom v2.4.20 header connection_error=3
```

RESP3 失败用例按当前产品目标不作为必修正确性问题，但保留在 `05_known_failing_cases.md`，避免后续误读测试红线。
