# Stage 05 Planner Prompt

You are the planner subagent for gemini-bloom audit v6 Stage 05.

Read these files before planning:

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
- `.codex/gemini-bloom-audit/v6/agents/stage04/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage04/reviewer_output.md`

Plan only. Do not execute commands and do not edit files.

Your output must include:

- Stage objective.
- DESIGN.md constraints and DESIGN_INTENDED differences relevant to RedisBloom comparison.
- How to discover or obtain a RedisBloom v2.4.20 oracle in this repo/environment.
- If the oracle is unavailable, what evidence must prove `BLOCKED`.
- If the oracle is available, the two-instance comparison plan for command surface, RESP2/RESP3, BF.INFO/parser differences, SCANDUMP/LOADCHUNK expected incompatibility, and RDB/DUMP/RESTORE/MIGRATE/fullsync/RDB-preamble AOF compatibility.
- Required evidence files and matrix structure.
- Risks, false positives, and exact PASS/BLOCKED/FAIL criteria.

