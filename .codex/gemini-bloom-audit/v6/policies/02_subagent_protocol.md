# Policy 02 — 子 agent 协议

## 1. 每个 stage 必须有两个子 agent

每个 stage 必须创建两个逻辑子 agent：

1. Planner 子 agent：只做阶段计划和风险分析，不执行测试，不修改文件。
2. Reviewer 子 agent：只审计本 stage 输出、证据和结论，不做实现修改。

如果 Codex App 当前环境不支持真实并行子 agent，则必须以“隔离上下文的子任务”模拟：单独写 prompt 文件、单独产出 output 文件、主 agent 只通过该 output 文件消费结果。

## 2. Planner 子 agent 输出

路径：

- `.codex/gemini-bloom-audit/v6/agents/stageXX/planner_prompt.md`
- `.codex/gemini-bloom-audit/v6/agents/stageXX/planner_output.md`

planner_output.md 必须包含：

- 本 stage 目标。
- DESIGN.md 对本 stage 的约束。
- 应审计的文件/命令/运行场景。
- 需要收集的 evidence。
- 风险点和可能误判点。
- 预期 PASS/BLOCKED 判据。

## 3. 主 agent 执行输出

路径：

- `.codex/gemini-bloom-audit/v6/agents/stageXX/main_execution.md`
- `.codex/gemini-bloom-audit/v6/agents/stageXX/stage_result.md`

stage_result.md 必须包含：

- planner 输出如何被采纳或修正。
- 实际执行了什么。
- 证据路径。
- findings。
- PASS/FAIL/BLOCKED/NOT_VERIFIED/DESIGN_INTENDED 分类。
- 对最终报告的影响。

## 4. Reviewer 子 agent 输出

路径：

- `.codex/gemini-bloom-audit/v6/agents/stageXX/reviewer_prompt.md`
- `.codex/gemini-bloom-audit/v6/agents/stageXX/reviewer_output.md`

reviewer_output.md 必须包含：

- Overall verdict: `PASS / FAIL / BLOCKED`
- 缺失证据。
- 不支持的结论。
- 是否违反 DESIGN.md。
- 是否遗漏本 stage 必审项目。
- 是否需要补跑。
- 是否允许进入下一 stage。

## 5. Reviewer FAIL 处理

如果 reviewer 输出 FAIL：

1. 主 agent 必须补证据、补跑测试、修正结论或降级状态。
2. 写入 `.codex/gemini-bloom-audit/v6/agents/stageXX/retry_N.md`。
3. 重新运行 reviewer。
4. 直到 reviewer PASS 或明确 BLOCKED。

## 6. 子 agent 关闭

stage 结束前必须在 stage_result.md 中写：

```text
Planner closed: yes
Reviewer closed: yes
Next stage may only use persisted files, not live subagent context.
```

“关闭”含义：输出已落盘，主 agent 不再依赖子 agent 活跃上下文，下一 stage 必须重新读文件。
