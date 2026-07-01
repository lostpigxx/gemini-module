# Agent outputs

每个 stage 创建独立目录：

```text
agents/stageXX/
  rehydrate_log.md
  planner_prompt.md
  planner_output.md
  main_execution.md
  stage_result.md
  reviewer_prompt.md
  reviewer_output.md
  retry_1.md
  retry_2.md
```

这些文件是 agent 内部过程产物，必须留在 `.codex/` 下，不要放到最终报告目录。
