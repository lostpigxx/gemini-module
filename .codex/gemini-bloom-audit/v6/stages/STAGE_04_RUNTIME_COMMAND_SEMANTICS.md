# Stage 04 — RUNTIME_COMMAND_SEMANTICS


## Universal stage protocol

本 stage 必须遵守：

1. 重新读取 `modules/gemini-bloom/DESIGN.md`、`LOOP_CONTROL_BATCH.md`、所有 policies、`LOOP_STATE.md`、本 stage 文件。
2. 创建 `.codex/gemini-bloom-audit/v6/agents/stage04/rehydrate_log.md`。
3. 创建 planner 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage04/planner_output.md`。
4. 主 agent 审核 planner，然后执行。
5. 所有证据写入 `.codex/gemini-bloom-audit/v6/evidence/stage04/`。
6. 写 `.codex/gemini-bloom-audit/v6/agents/stage04/stage_result.md`。
7. 创建 reviewer 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage04/reviewer_output.md`。
8. reviewer 未 PASS 时必须补跑/修正/降级，然后重新 reviewer。
9. 更新 `LOOP_STATE.md`。
10. commit + push。
11. 标记 planner/reviewer closed。



## Goal

在真实 redis-server 中验证 gemini-bloom 的 BF 命令运行时语义，并与 DESIGN.md 的命令接口、参数校验、错误路径、已知限制对齐。

## Required coverage

所有命令必须覆盖：

- BF.RESERVE
- BF.ADD
- BF.MADD
- BF.INSERT
- BF.EXISTS
- BF.MEXISTS
- BF.INFO
- BF.CARD
- BF.SCANDUMP
- BF.LOADCHUNK

## Main tasks

在不依赖 RedisBloom oracle 的情况下，构建 gemini 自身 runtime matrix：

- RESP2 happy path。
- RESP3 行为验证，并按 DESIGN.md 标记“不支持 RESP3”是否符合预期。
- wrong type。
- missing key。
- duplicate item。
- binary item、empty item、long item。
- capacity/error/expansion 边界。
- NONSCALING full。
- MADD/INSERT partial failure。
- BF.INFO field shapes。
- command metadata：COMMAND INFO、COMMAND GETKEYS、ACL DRYRUN。
- SCANDUMP/LOADCHUNK 私有 layer-index 协议。
- loading 中 key 的读写拒绝。

## Required evidence

- `evidence/stage04/runtime_matrix/commands.txt`
- `evidence/stage04/runtime_matrix/raw_resp.log`
- `evidence/stage04/runtime_matrix/normalized_results.md`
- `evidence/stage04/runtime_matrix/failures.md`

可以新增临时 Python/TCL harness，但必须放在 `.codex/gemini-bloom-audit/v6/evidence/stage04/` 或 `.codex/gemini-bloom-audit/v6/agents/stage04/`，不得污染生产目录。

## Pass criteria

- 10 个 BF 命令全部有 runtime 证据。
- DESIGN_INTENDED 差异被正确标记。
- reviewer 确认没有遗漏 RESP3/LOADCHUNK/loading/wrongtype。

## Commit message

`audit(gemini-bloom): v6 stage 04 runtime command semantics`

