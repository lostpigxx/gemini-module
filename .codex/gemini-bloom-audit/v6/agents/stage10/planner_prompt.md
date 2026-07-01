# Stage 10 Planner Prompt

You are the planner sub-agent for gemini-bloom audit v6 Stage 10 (`PERF_RESOURCE`).

Read before planning:

- `modules/gemini-bloom/DESIGN.md`
- `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`
- `.codex/gemini-bloom-audit/v6/policies/*.md`
- `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`
- `.codex/gemini-bloom-audit/v6/stages/STAGE_10_PERF_RESOURCE.md`
- `.codex/gemini-bloom-audit/v6/agents/stage09/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage09/reviewer_output.md`

Plan only. Do not run tests and do not modify production code.

Your output must be written to `.codex/gemini-bloom-audit/v6/agents/stage10/planner_output.md` and include:

- Stage objective.
- Relevant DESIGN constraints and design-intended boundaries.
- Exact resource-limit, latency-sample, memory-accounting, SCANDUMP/LOADCHUNK, RDB/AOF-size, and extreme-parameter scenarios to run.
- Required evidence files.
- Rules for safe handling of capacity `2^30`, large items, and resource exhaustion.
- BLOCKED/NOT_VERIFIED classification rules.
- False PASS/false FAIL risks and pass criteria.

