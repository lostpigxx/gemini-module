# Stage 12 — FINAL_REPORT_AUDIT


## Universal stage protocol

本 stage 必须遵守：

1. 重新读取 `modules/gemini-bloom/DESIGN.md`、`LOOP_CONTROL_BATCH.md`、所有 policies、`LOOP_STATE.md`、本 stage 文件。
2. 创建 `.codex/gemini-bloom-audit/v6/agents/stage12/rehydrate_log.md`。
3. 创建 planner 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage12/planner_output.md`。
4. 主 agent 审核 planner，然后执行。
5. 所有证据写入 `.codex/gemini-bloom-audit/v6/evidence/stage12/`。
6. 写 `.codex/gemini-bloom-audit/v6/agents/stage12/stage_result.md`。
7. 创建 reviewer 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage12/reviewer_output.md`。
8. reviewer 未 PASS 时必须补跑/修正/降级，然后重新 reviewer。
9. 更新 `LOOP_STATE.md`。
10. commit + push。
11. 标记 planner/reviewer closed。



## Goal

审计 Stage 11 生成的最终中文报告本身，确保报告可追溯、不过度宣称、不遗漏关键领域。

## Main tasks

重新读取：

- DESIGN.md。
- 所有 final report markdown。
- 所有 stage_result.md。
- 所有 reviewer_output.md。
- evidence index。

逐条检查：

1. 每个结论是否有 evidence 路径。
2. 每个 FAIL 是否有复现命令、expected、actual。
3. 每个 BLOCKED 是否有阻塞证据和可信度影响。
4. 每个 NOT_VERIFIED 是否在最终报告中明确呈现。
5. 是否误判 DESIGN_INTENDED 为 bug。
6. 是否遗漏 10 个 BF 命令。
7. 是否遗漏 RDB/DUMP/RESTORE/MIGRATE/fullsync/RDB-preamble AOF。
8. 是否遗漏 SCANDUMP/LOADCHUNK 私有协议边界。
9. 是否遗漏 RESP3 不支持边界。
10. 是否遗漏 sanitizer/fuzz/资源/replica/cluster/perf。
11. severity 是否过高或过低。
12. 最终可信度评级是否匹配 evidence 覆盖。

## Required output

写入或更新：

```text
doc/code_review/gemini-bloom/v6/10_报告自审结果.md
.codex/gemini-bloom-audit/v6/agents/stage12/report_audit_matrix.md
```

如果发现报告问题，必须修正 Stage 11 生成的报告文件，然后重新 reviewer。

## Pass criteria

- 报告自审 verdict = PASS。
- 所有修正已落盘。
- LOOP_STATE.md 标记 Stage 12 PASS。
- commit + push 成功。

## Commit message

`audit(gemini-bloom): v6 stage 12 final report audit`

