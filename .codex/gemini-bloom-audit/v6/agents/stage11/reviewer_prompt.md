# Stage 11 Reviewer Prompt

You are the Stage 11 reviewer for the gemini-bloom v6 audit loop.

Read:

- `modules/gemini-bloom/DESIGN.md`
- `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`
- all `.codex/gemini-bloom-audit/v6/policies/*.md`
- `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`
- `.codex/gemini-bloom-audit/v6/stages/STAGE_11_FINAL_REPORT_SYNTHESIS.md`
- `.codex/gemini-bloom-audit/v6/agents/stage11/planner_output.md`
- `.codex/gemini-bloom-audit/v6/agents/stage11/main_execution.md`
- `.codex/gemini-bloom-audit/v6/agents/stage11/stage_result.md`
- all `doc/code_review/gemini-bloom/v6/*.md`
- Stage 11 evidence files under `.codex/gemini-bloom-audit/v6/evidence/stage11/`
- Stage 00-10 outputs/evidence needed to verify report claims.

Review whether Stage 11 may PASS. Focus on:

- All required report files exist, including Policy 06 `10_报告自审结果.md`.
- Report is Chinese and evidence-backed.
- DESIGN_INTENDED differences are not misclassified as bugs.
- No broad RedisBloom/drop-in/RESP3/sanitizer/performance overclaim.
- All global findings and degraded coverage items are carried forward.
- The confidence rating is appropriately downgraded.
- Stage 12 can audit the report without rerunning product tests.

Write your full verdict to:

`.codex/gemini-bloom-audit/v6/agents/stage11/reviewer_output.md`

Do not edit production code. If FAIL, identify exact missing report/evidence/wording issues and minimal correction required.
