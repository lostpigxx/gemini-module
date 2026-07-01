# Policy 01 — Rehydration / 抗 compact 策略

## 1. 每个 stage 开始必须重新读取

主 agent 在每个 stage 开始前必须重新读取：

1. `modules/gemini-bloom/DESIGN.md`
2. `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`
3. `.codex/gemini-bloom-audit/v6/policies/*.md`
4. `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`
5. 当前 stage 文件，例如 `stages/STAGE_04_RUNTIME_COMMAND_SEMANTICS.md`
6. 上一 stage 的 `stage_result.md` 和 `reviewer_output.md`

## 2. 禁止依赖聊天上下文

不得写：

- “如前所述”但没有文件路径。
- “上一阶段已经验证”但没有 evidence 路径。
- “planner 认为”但没有 planner_output.md。

所有跨 stage 信息必须通过文件读取。

## 3. Stage rehydrate 记录

每个 stage 必须创建：

- `.codex/gemini-bloom-audit/v6/agents/stageXX/rehydrate_log.md`

内容包括：

- 本 stage 读取了哪些文件。
- DESIGN.md 中与本 stage 相关的约束摘要。
- 从 LOOP_STATE.md 读取到的上一 stage 状态。
- 本 stage 不允许越界的限制。

## 4. 文件优先级

优先级从高到低：

1. 用户最新明确要求。
2. `modules/gemini-bloom/DESIGN.md`。
3. `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`。
4. `.codex/gemini-bloom-audit/v6/policies/*.md`。
5. 当前 stage 文件。
6. 历史 stage 输出。
7. Codex 对话上下文。
