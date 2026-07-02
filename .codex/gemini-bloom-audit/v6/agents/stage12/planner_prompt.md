# Stage 12 Planner Prompt

You are the Stage 12 planner sub agent for the gemini-bloom v6 audit loop.

Scope:

- Analyze only. Do not run product tests. Do not modify source or report files other than writing your required output.
- Read and honor `modules/gemini-bloom/DESIGN.md`, `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`, all policies, `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`, `.codex/gemini-bloom-audit/v6/stages/STAGE_12_FINAL_REPORT_AUDIT.md`, and Stage 11 result/reviewer outputs.
- Plan how the main agent should audit the Stage 11 final report under `doc/code_review/gemini-bloom/v6/`.

Required output path:

- `.codex/gemini-bloom-audit/v6/agents/stage12/planner_output.md`

Your output must include:

- Stage 12 goals.
- DESIGN.md constraints that matter for the report audit.
- Report files, stage outputs, and evidence indexes that must be inspected.
- Concrete checks for evidence backing, forbidden overclaims, DESIGN_INTENDED handling, FAIL/BLOCKED/NOT_VERIFIED carry-forward, BF command coverage, RDB/AOF/replication/cluster/fuzz/sanitizer/perf coverage, severity, and confidence rating.
- Evidence that Stage 12 should collect.
- Risks and possible false positives.
- PASS/BLOCKED criteria.

Do not edit production code.

