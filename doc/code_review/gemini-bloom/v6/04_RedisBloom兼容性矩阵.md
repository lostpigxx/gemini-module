# RedisBloom 兼容性矩阵

## Oracle 范围

本轮 RedisBloom 对比的精确范围是 Redis 6.2.17 + RedisBloom v2.4.20，`MODULE LIST ver=20420`。证据见 `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_env.txt`。gemini runtime 对比使用 audit-only `-include climits` workaround，因为默认 Linux/GCC build 存在 `GBV6-05-001`；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/gemini_default_build_stderr.log`。

## 9 个 corpus

Stage 05/06 覆盖 corpus：`binary_items`、`empty_scaling`、`expansion1`、`expansion4`、`fixed_full`、`large_empty_16mb`、`long_item`、`multi_exp2`、`single_layer`。证据见 `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md`。

## 兼容路径

| 路径 | 方向 | 结论 | 证据 |
|---|---|---|---|
| RDB file | RedisBloom -> gemini | PASS 9/9 | `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md` |
| RDB file | gemini -> RedisBloom | PASS 9/9 | `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md` |
| DUMP/RESTORE | 双向 | PASS 9/9 | `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |
| MIGRATE + TTL | 双向 | PASS | `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/extended_audit_results_redis62_redisbloom2420.json` |
| fullsync replication | 双向 | PASS 9/9 | `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |
| AOF RDB preamble yes | 双向 | PASS 9/9 | `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |

## 设计内不兼容路径

| 路径 | 结论 | 说明 | 证据 |
|---|---|---|---|
| RedisBloom SCANDUMP/LOADCHUNK -> gemini | DESIGN_INTENDED | gemini 使用私有 layer-index cursor，不是 RedisBloom byte-offset | `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md` |
| gemini SCANDUMP/LOADCHUNK -> RedisBloom | DESIGN_INTENDED | 同上 | `.codex/gemini-bloom-audit/v6/evidence/stage06/scandump_loadchunk/summary.md` |
| command-AOF no-preamble cross replay | DESIGN_INTENDED | 依赖私有 LOADCHUNK；生产应保持 RDB preamble | `.codex/gemini-bloom-audit/v6/evidence/stage06/aof_preamble_no/summary.md` |
| live command-stream `EXPANSION 1` `BF.CARD` drift | DESIGN_INTENDED_LIMITATION | membership 无 inserted-item false negative，但 CARD 不同 | `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md` |
| BF.DEBUG | DESIGN_INTENDED | gemini 不支持 | `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_normalized.md` |

## 命令语义差异

Stage 05 raw RESP 对比确认下列差异均符合 DESIGN：module name/version 不同、`BF.INFO FIELD` 返回标量、`BF.INFO Size` 数值不同、`BF.INSERT NOCREATE CAPACITY` 错误消息不同、BF.DEBUG 不支持。证据见 `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_normalized.md` 与 `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_raw_resp.log`。

## 不得外推

本报告不声明其他 RedisBloom 版本、Redis 8 内置 Bloom、RESP3、RedisBloom SCANDUMP/LOADCHUNK、或同实例 RedisBloom/gemini 共存兼容。Stage 00 的 DESIGN contract 已明确这些边界，证据见 `.codex/gemini-bloom-audit/v6/agents/stage00/design_contract.md`。
