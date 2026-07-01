# Stage 05 Reviewer Prompt

You are the reviewer subagent for gemini-bloom audit v6 Stage 05.

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
- `.codex/gemini-bloom-audit/v6/stages/STAGE_05_REDISBLOOM_COMPAT.md`
- `.codex/gemini-bloom-audit/v6/agents/stage05/planner_output.md`
- `.codex/gemini-bloom-audit/v6/agents/stage05/main_execution.md`
- `.codex/gemini-bloom-audit/v6/agents/stage05/stage_result.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_env.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_raw_resp.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_normalized.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/evidence_index.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/compat_matrix_results_redis62_redisbloom2420.json`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/extended_audit_results_redis62_redisbloom2420.json`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/gemini_default_build_stderr.log`

Review questions:

1. Is the exact RedisBloom v2.4.20 oracle proven?
2. Do the Stage 05 compatibility conclusions correctly distinguish DESIGN-promised paths from DESIGN_INTENDED incompatibilities?
3. Does the `-include climits` audit-build workaround undermine the runtime compatibility conclusion, or is it acceptable when paired with `GBV6-05-001`?
4. Is `GBV6-05-001` evidence-backed and reasonably classified?
5. Are required evidence files sufficient under Policy 03?
6. Should the main agent rerun, downgrade to BLOCKED, or proceed to commit/push?

Return a concise reviewer report with:

- `Reviewer verdict: PASS`, `FAIL`, or `BLOCKED`.
- Findings or required corrections, if any.
- Whether the main agent may commit/push Stage 05 and proceed to Stage 06.
