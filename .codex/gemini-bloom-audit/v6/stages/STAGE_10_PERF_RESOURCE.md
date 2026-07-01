# Stage 10 — PERF_RESOURCE


## Universal stage protocol

本 stage 必须遵守：

1. 重新读取 `modules/gemini-bloom/DESIGN.md`、`LOOP_CONTROL_BATCH.md`、所有 policies、`LOOP_STATE.md`、本 stage 文件。
2. 创建 `.codex/gemini-bloom-audit/v6/agents/stage10/rehydrate_log.md`。
3. 创建 planner 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage10/planner_output.md`。
4. 主 agent 审核 planner，然后执行。
5. 所有证据写入 `.codex/gemini-bloom-audit/v6/evidence/stage10/`。
6. 写 `.codex/gemini-bloom-audit/v6/agents/stage10/stage_result.md`。
7. 创建 reviewer 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage10/reviewer_output.md`。
8. reviewer 未 PASS 时必须补跑/修正/降级，然后重新 reviewer。
9. 更新 `LOOP_STATE.md`。
10. commit + push。
11. 标记 planner/reviewer closed。



## Goal

审计性能、资源限制、内存统计和极限参数行为。该阶段不产出正式 benchmark，只产出审计信号。

## Main tasks

覆盖矩阵：

- capacity: 10 / 100 / 10000 / 100000 / 2^30 边界尝试。
- expansion: 1 / 2 / 4 / 32768。
- item: empty / small / binary / 10KB / 1MB。
- commands: ADD / MADD / EXISTS / MEXISTS / INFO / CARD / SCANDUMP / LOADCHUNK。
- NONSCALING full。
- multi-layer 查询退化。
- BF.INFO Size 与 Redis MEMORY USAGE / module mem_usage 关系。
- 大 filter 的 SCANDUMP chunk size 是否符合 DESIGN.md 私有协议。

记录：

- latency。
- RSS。
- MEMORY USAGE。
- BF.INFO Size。
- RDB/AOF 文件大小。
- 是否触发资源上限。

## Required evidence

- `evidence/stage10/perf_matrix.md`
- `evidence/stage10/resource_limits.log`
- `evidence/stage10/memory_usage.md`
- `evidence/stage10/latency_samples.csv`

## Pass criteria

- 资源限制符合 DESIGN.md。
- 性能异常有说明。
- 报告不把小样本写成正式 benchmark。

## Commit message

`audit(gemini-bloom): v6 stage 10 perf resource`

