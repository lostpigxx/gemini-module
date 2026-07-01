# Stage 11 — FINAL_REPORT_SYNTHESIS


## Universal stage protocol

本 stage 必须遵守：

1. 重新读取 `modules/gemini-bloom/DESIGN.md`、`LOOP_CONTROL_BATCH.md`、所有 policies、`LOOP_STATE.md`、本 stage 文件。
2. 创建 `.codex/gemini-bloom-audit/v6/agents/stage11/rehydrate_log.md`。
3. 创建 planner 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage11/planner_output.md`。
4. 主 agent 审核 planner，然后执行。
5. 所有证据写入 `.codex/gemini-bloom-audit/v6/evidence/stage11/`。
6. 写 `.codex/gemini-bloom-audit/v6/agents/stage11/stage_result.md`。
7. 创建 reviewer 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage11/reviewer_output.md`。
8. reviewer 未 PASS 时必须补跑/修正/降级，然后重新 reviewer。
9. 更新 `LOOP_STATE.md`。
10. commit + push。
11. 标记 planner/reviewer closed。



## Goal

归纳 Stage 00-10 所有结果，生成面向人的中文最终报告，路径固定为 `doc/code_review/gemini-bloom/v6/`。

## Required inputs

必须重新读取：

- DESIGN.md。
- LOOP_STATE.md。
- 每个 stage 的 planner_output.md、stage_result.md、reviewer_output.md。
- 所有 evidence_index.md。
- 所有 findings。
- compatibility/coverage matrix。

## Required final files

生成：

```text
doc/code_review/gemini-bloom/v6/00_审计总览.md
doc/code_review/gemini-bloom/v6/01_DESIGN约束与结论对齐.md
doc/code_review/gemini-bloom/v6/02_源码实现审计.md
doc/code_review/gemini-bloom/v6/03_运行时测试结果.md
doc/code_review/gemini-bloom/v6/04_RedisBloom兼容性矩阵.md
doc/code_review/gemini-bloom/v6/05_持久化迁移复制审计.md
doc/code_review/gemini-bloom/v6/06_安全与资源边界.md
doc/code_review/gemini-bloom/v6/07_问题清单与复现.md
doc/code_review/gemini-bloom/v6/08_测试覆盖与未覆盖.md
doc/code_review/gemini-bloom/v6/09_最终结论与修复优先级.md
doc/code_review/gemini-bloom/v6/evidence_index.md
```

## Report rules

- 中文。
- 先说明 DESIGN.md 的兼容边界。
- 不写“完全兼容 RedisBloom”。
- 不把 DESIGN_INTENDED 差异写成 bug。
- 每个结论必须引用 `.codex/.../evidence/...` 路径。
- 每个未覆盖项写 `NOT_VERIFIED`。
- 每个环境阻塞写 `BLOCKED`。
- 结论必须降级可信度，如果 RedisBloom oracle、persistence、sanitizer、replica/cluster 关键项有 BLOCKED。

## Pass criteria

- 所有 required final files 存在。
- 每个 finding 有复现和证据。
- 报告没有未证实夸大结论。
- reviewer 确认可以进入 Stage 12 自审。

## Commit message

`audit(gemini-bloom): v6 stage 11 final report synthesis`

