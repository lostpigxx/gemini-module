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
| module load 配置差异 | DESIGN_INTENDED / PARTIAL | gemini 支持 `EXPANSION` load arg 并拒绝配置 `EXPANSION 0`；RedisBloom v2.4.20 不支持 gemini 的 Bloom `EXPANSION` 配置，`CF_MAX_EXPANSIONS` 是 RedisBloom Cuckoo Filter 参数 | `modules/gemini-bloom/DESIGN.md:672`, `.codex/gemini-bloom-audit/v6/agents/stage00/design_claims_matrix.md` |

## 命令语义差异

Stage 05 raw RESP 对比确认下列差异均符合 DESIGN：module name/version 不同、`BF.INFO FIELD` 返回标量、`BF.INFO Size` 数值不同、`BF.INSERT NOCREATE CAPACITY` 错误消息不同、`BF.INSERT EXPANSION 0` 映射为 NONSCALING 而 RedisBloom 拒绝、BF.DEBUG 不支持。证据见 `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_normalized.md` 与 `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_raw_resp.log`。

## 操作限制

- Bloom Filter 删除不是 gemini-bloom 的支持能力；如需删除语义，DESIGN 指向 Cuckoo Filter，但 gemini 当前未实现。
- `EXPANSION 1` 会产生更多子 filter，查询路径需逐层检查，存在查询成本上升风险；生产建议使用 `EXPANSION 2` 或更大。
- command-AOF rewrite 在 header buffer 分配失败时会记录 warning 并跳过该 key。该风险只属于极端 OOM 和非 RDB preamble rewrite 路径；默认 `aof-use-rdb-preamble yes` 不执行这段 command rewrite。
- gemini-bloom 与 RedisBloom module / Redis 8 内置 Bloom 是同实例互斥部署关系。它复用 `BF.*` 命令名和 `MBbloom--` type name，同一实例中后加载者会遇到 command 或 data type 注册冲突。

## 不得外推

本报告不声明其他 RedisBloom 版本、Redis 8 内置 Bloom、RESP3、RedisBloom SCANDUMP/LOADCHUNK、或同实例 RedisBloom/gemini 共存兼容。对 Redis 8 内置 Bloom 和同实例共存，本报告按 DESIGN 明确为互斥部署限制，而不是未测兼容能力。Stage 00 的 DESIGN contract 已明确这些边界，证据见 `.codex/gemini-bloom-audit/v6/agents/stage00/design_contract.md`。
