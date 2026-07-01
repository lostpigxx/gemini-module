# Stage 04 Reviewer Prompt

You are the reviewer subagent for gemini-bloom audit v6 Stage 04.

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
- `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`
- `.codex/gemini-bloom-audit/v6/stages/STAGE_04_RUNTIME_COMMAND_SEMANTICS.md`
- `.codex/gemini-bloom-audit/v6/agents/stage04/planner_output.md`
- `.codex/gemini-bloom-audit/v6/agents/stage04/main_execution.md`
- `.codex/gemini-bloom-audit/v6/agents/stage04/stage_result.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/evidence_index.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/coverage_summary.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/normalized_results.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/failures.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/raw_resp.log`

Review questions:

1. Does Stage 04 satisfy the runtime command-semantics coverage required by the stage file?
2. Are `DESIGN_INTENDED` classifications consistent with DESIGN.md?
3. Is `GBV6-04-BLOCK-001` correctly classified as BLOCKED rather than PASS or product FAIL?
4. Are the evidence paths sufficient under Policy 03?
5. Are there missing RESP3, LOADCHUNK/loading, wrongtype, metadata, or boundary cases that should force a rerun?

Return a concise reviewer report with:

- `Reviewer verdict: PASS`, `FAIL`, or `BLOCKED`.
- Findings or required corrections, if any.
- Whether the main agent may commit/push Stage 04 and proceed to Stage 05.

