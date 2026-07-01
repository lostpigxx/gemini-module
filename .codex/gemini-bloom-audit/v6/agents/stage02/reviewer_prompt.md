# Stage 02 Reviewer Prompt

You are the Stage 02 reviewer sub agent for the gemini-bloom v6 audit.

Read these files before judging:

- `modules/gemini-bloom/DESIGN.md`
- `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`
- `.codex/gemini-bloom-audit/v6/policies/00_design_first_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/01_rehydration_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/02_subagent_protocol.md`
- `.codex/gemini-bloom-audit/v6/policies/03_evidence_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/04_quality_gates.md`
- `.codex/gemini-bloom-audit/v6/policies/05_commit_push_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/06_final_report_policy.md`
- `.codex/gemini-bloom-audit/v6/stages/STAGE_02_BUILD_EXISTING_TESTS.md`
- `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`
- `.codex/gemini-bloom-audit/v6/agents/stage01/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage01/reviewer_output.md`
- `.codex/gemini-bloom-audit/v6/agents/stage02/planner_output.md`
- `.codex/gemini-bloom-audit/v6/agents/stage02/rehydrate_log.md`
- `.codex/gemini-bloom-audit/v6/agents/stage02/main_execution.md`
- `.codex/gemini-bloom-audit/v6/agents/stage02/findings.md`
- `.codex/gemini-bloom-audit/v6/agents/stage02/stage_result.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage02/**`

Task:

- Review whether Stage 02 satisfies the stage file, policies, and DESIGN-first boundaries.
- Verify that all build/GTest/TCL failures are classified and evidence-backed.
- Check that direct GTest and TCL claims are not overstated.
- Check that DESIGN_INTENDED expected gaps are not misclassified as product bugs.
- Do not modify production code.
- Write your result to `.codex/gemini-bloom-audit/v6/agents/stage02/reviewer_output.md`.

Required output structure:

```markdown
# Stage 02 Reviewer Output

## 1. Overall verdict
PASS / FAIL / BLOCKED

## 2. DESIGN.md 对齐检查

## 3. 证据完整性检查

## 4. 不支持或夸大的结论

## 5. 遗漏项

## 6. Finding 分类和 severity 检查

## 7. 是否允许进入下一 stage

## 8. 必须补跑/修正项
```
