# Stage 06 — PERSISTENCE_TRANSPORT


## Universal stage protocol

本 stage 必须遵守：

1. 重新读取 `modules/gemini-bloom/DESIGN.md`、`LOOP_CONTROL_BATCH.md`、所有 policies、`LOOP_STATE.md`、本 stage 文件。
2. 创建 `.codex/gemini-bloom-audit/v6/agents/stage06/rehydrate_log.md`。
3. 创建 planner 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage06/planner_output.md`。
4. 主 agent 审核 planner，然后执行。
5. 所有证据写入 `.codex/gemini-bloom-audit/v6/evidence/stage06/`。
6. 写 `.codex/gemini-bloom-audit/v6/agents/stage06/stage_result.md`。
7. 创建 reviewer 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage06/reviewer_output.md`。
8. reviewer 未 PASS 时必须补跑/修正/降级，然后重新 reviewer。
9. 更新 `LOOP_STATE.md`。
10. commit + push。
11. 标记 planner/reviewer closed。



## Goal

验证持久化、迁移和复制路径。此 stage 是 DESIGN.md 最关键的兼容性承诺验证阶段。

## Must verify or mark BLOCKED

- gemini → RDB save/restart → gemini
- RedisBloom → RDB → gemini
- gemini → RDB → RedisBloom
- gemini → DUMP/RESTORE → gemini
- RedisBloom ↔ gemini DUMP/RESTORE
- MIGRATE 双向，如环境支持
- fullsync replication / psync RDB snapshot，如环境支持
- AOF with `aof-use-rdb-preamble yes`
- AOF with `aof-use-rdb-preamble no`，预期仅 gemini 自身兼容，跨实现 DESIGN_INTENDED 不兼容
- gemini SCANDUMP/LOADCHUNK 自身 round-trip
- RedisBloom SCANDUMP/LOADCHUNK 跨实现不兼容，预期 DESIGN_INTENDED

## Main tasks

对每条路径记录：

- source module/version。
- target module/version。
- Redis 配置。
- 命令序列。
- membership no false negative。
- BF.CARD / BF.INFO 可确定字段。
- TTL 是否保留，适用于 DUMP/RESTORE/MIGRATE。
- RDB/AOF 文件是否能加载。
- 错误日志。

## Required evidence

- `evidence/stage06/rdb/`
- `evidence/stage06/dump_restore/`
- `evidence/stage06/migrate/`
- `evidence/stage06/replication/`
- `evidence/stage06/aof_preamble_yes/`
- `evidence/stage06/aof_preamble_no/`
- `evidence/stage06/scandump_loadchunk/`

## Pass criteria

- DESIGN.md 声明的兼容路径要么 PASS，要么 FAIL with finding，要么 BLOCKED with evidence。
- DESIGN.md 声明的不兼容路径必须验证其“不兼容但安全、不误导、不破坏数据”。
- reviewer 确认未把 SCANDUMP/LOADCHUNK 跨实现不兼容误判为 bug。

## Commit message

`audit(gemini-bloom): v6 stage 06 persistence transport`

