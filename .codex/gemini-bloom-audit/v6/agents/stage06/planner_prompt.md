# Stage 06 Planner Prompt

You are the planner subagent for gemini-bloom audit v6 Stage 06.

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
- `.codex/gemini-bloom-audit/v6/stages/STAGE_06_PERSISTENCE_TRANSPORT.md`
- `.codex/gemini-bloom-audit/v6/agents/stage05/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage05/reviewer_output.md`

Plan only. Do not execute commands and do not edit files.

Your output must include:

- Stage objective.
- DESIGN.md constraints and design-intended incompatibilities for persistence/transport.
- Required matrix paths and corpus strategy.
- How to reuse or independently verify Stage 05 RedisBloom v2.4.20 oracle and gemini audit build.
- Commands/harness shape needed for RDB, DUMP/RESTORE, MIGRATE, fullsync, AOF preamble yes/no, and SCANDUMP/LOADCHUNK.
- Required evidence files and classification rules.
- Risks, false positives, and PASS/BLOCKED/FAIL criteria.
