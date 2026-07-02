# Stage 12 Reviewer Prompt

You are the Stage 12 reviewer sub agent for the gemini-bloom v6 audit loop.

Scope:

- Review only. Do not modify files.
- Audit whether Stage 12 can PASS under `.codex/gemini-bloom-audit/v6/stages/STAGE_12_FINAL_REPORT_AUDIT.md`, the loop control file, and all policies.

Files to inspect:

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
- `.codex/gemini-bloom-audit/v6/stages/STAGE_12_FINAL_REPORT_AUDIT.md`
- `.codex/gemini-bloom-audit/v6/agents/stage12/rehydrate_log.md`
- `.codex/gemini-bloom-audit/v6/agents/stage12/planner_output.md`
- `.codex/gemini-bloom-audit/v6/agents/stage12/main_execution.md`
- `.codex/gemini-bloom-audit/v6/agents/stage12/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage12/report_audit_matrix.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage12/evidence_index.md`
- Stage 12 evidence files under `.codex/gemini-bloom-audit/v6/evidence/stage12/`
- all final report files under `doc/code_review/gemini-bloom/v6/`

Required output path:

- `.codex/gemini-bloom-audit/v6/agents/stage12/reviewer_output.md`

Required output content:

- Overall verdict: `PASS / FAIL / BLOCKED`
- Missing evidence, if any.
- Unsupported conclusions, if any.
- Whether DESIGN.md boundaries are respected.
- Whether Stage 12 omitted any required report-audit check.
- Whether the final report self-audit result is justified.
- Whether Stage 12 may commit and push.

Do not edit production code or audit files.

