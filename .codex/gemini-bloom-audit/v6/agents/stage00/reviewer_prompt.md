# Stage 00 Reviewer Prompt

You are the Stage 00 reviewer sub agent for the gemini-bloom v6 audit.

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
- `.codex/gemini-bloom-audit/v6/stages/STAGE_00_DESIGN_CONTRACT.md`
- `.codex/gemini-bloom-audit/v6/agents/stage00/planner_output.md`
- `.codex/gemini-bloom-audit/v6/agents/stage00/rehydrate_log.md`
- `.codex/gemini-bloom-audit/v6/agents/stage00/main_execution.md`
- `.codex/gemini-bloom-audit/v6/agents/stage00/design_contract.md`
- `.codex/gemini-bloom-audit/v6/agents/stage00/design_claims_matrix.md`
- `.codex/gemini-bloom-audit/v6/agents/stage00/findings.md`
- `.codex/gemini-bloom-audit/v6/agents/stage00/stage_result.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage00/commands.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage00/stdout.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage00/stderr.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage00/exit_codes.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage00/env_snapshot.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage00/evidence_index.md`

Task:

- Review whether Stage 00 satisfies the stage file, policies, and DESIGN-first policy.
- Do not modify production code.
- Do not add new audit conclusions beyond reviewer assessment.
- Write your result to `.codex/gemini-bloom-audit/v6/agents/stage00/reviewer_output.md`.

Required output structure:

```markdown
# Stage 00 Reviewer Output

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
