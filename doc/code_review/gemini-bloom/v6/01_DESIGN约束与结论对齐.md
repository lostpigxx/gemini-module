# DESIGN 约束与结论对齐

## DESIGN 最高边界

`modules/gemini-bloom/DESIGN.md` 是本轮审计最高优先级标准。Stage 00 已把 DESIGN 约束抽取为审计契约，证据见 `.codex/gemini-bloom-audit/v6/agents/stage00/design_contract.md` 和 `.codex/gemini-bloom-audit/v6/agents/stage00/design_claims_matrix.md`。

## 设计承诺与审计结论

| DESIGN 项 | 审计结论 | 证据 |
|---|---|---|
| 不是 RedisBloom drop-in 替代品 | `PASS / DESIGN_BOUNDARY`，报告不得扩大兼容范围 | `.codex/gemini-bloom-audit/v6/agents/stage00/stage_result.md` |
| RedisBloom v2.4.20 RDB-family 互通 | `PASS`，限 Redis 6.2.17 + RedisBloom v2.4.20 + 9 corpus | `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md`, `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |
| DUMP/RESTORE/MIGRATE/fullsync/RDB-preamble AOF | `PASS`，本轮矩阵覆盖双向路径 | `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |
| `BF.SCANDUMP/BF.LOADCHUNK` 不与 RedisBloom 互通 | `DESIGN_INTENDED`，跨实现失败不算 bug | `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md`, `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |
| command-AOF no-preamble 不跨实现兼容 | `DESIGN_INTENDED`，gemini self PASS，cross 不是承诺 | `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |
| RESP3 不支持 | `DESIGN_INTENDED`，Stage 02/04 只记录 expected gap | `.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/tcl_summary.md`, `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/normalized_results.md` |
| BF.DEBUG 不支持 | `DESIGN_INTENDED` | `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_normalized.md` |
| capacity/error/expansion 命令边界 | `PASS` for command path | `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/normalized_results.md`, `.codex/gemini-bloom-audit/v6/evidence/stage10/resource_limits.log` |
| RDB/wire 非信任输入资源边界 | `FAIL` for two gaps | `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md`, `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md` |
| Loading runtime-only 状态保护 | `PARTIAL`，正常路径有保护，但异常序列/持久化有 P1 缺陷 | `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md`, `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/loading_partial_fullsync.log` |

## 设计内差异

下列差异符合 DESIGN，不应列为 product bug：

- RESP3 未支持：`.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/tcl_summary.md`
- BF.DEBUG 未支持：`.codex/gemini-bloom-audit/v6/evidence/stage05/diff_normalized.md`
- `BF.INFO key FIELD` 返回标量而非 RedisBloom singleton array：`.codex/gemini-bloom-audit/v6/evidence/stage05/diff_normalized.md`
- `BF.INFO Size` 统计口径不同：`.codex/gemini-bloom-audit/v6/evidence/stage10/memory_usage.md`
- RedisBloom SCANDUMP/LOADCHUNK byte-offset 协议不互通：`.codex/gemini-bloom-audit/v6/evidence/stage06/scandump_loadchunk/summary.md`
- command-AOF no-preamble cross replay 不互通：`.codex/gemini-bloom-audit/v6/evidence/stage06/aof_preamble_no/summary.md`
- live command-stream `EXPANSION 1` 的 `BF.CARD` drift：`.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md`

## DESIGN 自身/文档一致性问题

`GBV6-00-001`：DESIGN 声称 `tests/compat/redisbloom-2.4.20/` 有 checked-in fixtures，但审计树中不存在。证据：`.codex/gemini-bloom-audit/v6/evidence/stage00/stderr.log`。

`GBV6-00-002`：`sb_chain.h` 注释称 SCANDUMP/LOADCHUNK wire format 匹配 RedisBloom 以支持 cross-implementation compatibility，和 DESIGN 私有协议边界冲突。证据：`.codex/gemini-bloom-audit/v6/evidence/stage00/stdout.log`。
