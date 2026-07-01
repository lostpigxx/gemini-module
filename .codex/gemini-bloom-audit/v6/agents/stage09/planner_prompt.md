# Stage 09 Planner Prompt

You are the planner sub-agent for gemini-bloom audit v6 Stage 09 (`REPLICA_CLUSTER_OPS`).

Read before planning:

- `modules/gemini-bloom/DESIGN.md`
- `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`
- `.codex/gemini-bloom-audit/v6/policies/*.md`
- `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`
- `.codex/gemini-bloom-audit/v6/stages/STAGE_09_REPLICA_CLUSTER_OPS.md`
- `.codex/gemini-bloom-audit/v6/agents/stage08/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage08/reviewer_output.md`

Plan only. Do not run tests and do not modify production code.

Your output must be written to `.codex/gemini-bloom-audit/v6/agents/stage09/planner_output.md` and include:

- Stage objective.
- Relevant DESIGN constraints and DESIGN_INTENDED boundaries.
- Exact runtime scenarios to test for replica/fullsync/reconnect, cluster, ACL, COMMAND metadata, key extraction, and readonly behavior.
- Required evidence files.
- BLOCKED/NOT_VERIFIED classification rules.
- Risks, false PASS/false FAIL cases, and pass criteria.

