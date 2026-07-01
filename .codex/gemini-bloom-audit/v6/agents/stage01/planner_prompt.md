# Stage 01 Planner Prompt

You are the Stage 01 planner sub agent for the gemini-bloom v6 audit.

Read these files before producing the plan:

- `modules/gemini-bloom/DESIGN.md`
- `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`
- `.codex/gemini-bloom-audit/v6/policies/00_design_first_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/01_rehydration_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/02_subagent_protocol.md`
- `.codex/gemini-bloom-audit/v6/policies/03_evidence_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/04_quality_gates.md`
- `.codex/gemini-bloom-audit/v6/policies/05_commit_push_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/06_final_report_policy.md`
- `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`
- `.codex/gemini-bloom-audit/v6/stages/STAGE_01_ENV_REPO_BASELINE.md`
- `.codex/gemini-bloom-audit/v6/agents/stage00/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage00/reviewer_output.md`

Task:

- Analyze what Stage 01 must do, how to gather reproducibility evidence, what risks exist, and what evidence is required.
- Do not run commands beyond reading files.
- Do not modify production code.
- Write the planner result to `.codex/gemini-bloom-audit/v6/agents/stage01/planner_output.md`.

Required output structure:

```markdown
# Stage 01 Planner Output

## 1. Stage 目标

## 2. DESIGN.md 相关约束

## 3. 必审对象

## 4. 运行/静态检查计划

## 5. 证据清单

## 6. 易误判点

## 7. PASS / FAIL / BLOCKED 判据

## 8. 对最终报告的影响
```
