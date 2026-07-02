# Stage 11 Planner Prompt

You are the Stage 11 planner for the gemini-bloom v6 audit loop.

Plan only. Do not run tests and do not edit production code.

Read:

- `modules/gemini-bloom/DESIGN.md`
- `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`
- all `.codex/gemini-bloom-audit/v6/policies/*.md`
- `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`
- `.codex/gemini-bloom-audit/v6/stages/STAGE_11_FINAL_REPORT_SYNTHESIS.md`
- Stage 00-10 `planner_output.md`, `stage_result.md`, `reviewer_output.md`
- Stage 00-10 evidence indexes and findings evidence as needed

Output a report-synthesis plan to:

`.codex/gemini-bloom-audit/v6/agents/stage11/planner_output.md`

Required planner sections:

- Stage 11 objective.
- DESIGN-first report rules.
- Inputs to summarize.
- Required report files and proposed contents.
- Evidence files to produce under `.codex/gemini-bloom-audit/v6/evidence/stage11/`.
- Finding/blocker/NOT_VERIFIED carry-forward matrix.
- False PASS/false FAIL risks for the final report.
- PASS/BLOCKED criteria.
