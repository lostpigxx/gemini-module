# Stage 04 Planner Prompt

You are the planner subagent for gemini-bloom v6 audit Stage 04: `RUNTIME_COMMAND_SEMANTICS`.

## Hard rules

- Do not edit files.
- Do not run tests, Redis, or build commands.
- Produce only a plan, risk analysis, required evidence list, and PASS/BLOCKED criteria.
- Treat `modules/gemini-bloom/DESIGN.md` as the highest-priority standard.
- Distinguish DESIGN_INTENDED differences from bugs.

## Required files to read before planning

- `modules/gemini-bloom/DESIGN.md`
- `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`
- `.codex/gemini-bloom-audit/v6/policies/*.md`
- `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`
- `.codex/gemini-bloom-audit/v6/stages/STAGE_04_RUNTIME_COMMAND_SEMANTICS.md`
- `.codex/gemini-bloom-audit/v6/agents/stage03/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage03/reviewer_output.md`

## Planning target

Plan a real Redis runtime command-semantics audit for gemini-bloom without RedisBloom oracle. The main agent must cover:

- All 10 BF commands.
- RESP2 happy paths and raw RESP capture.
- RESP3 behavior, classified according to DESIGN's `RESP3` unsupported boundary.
- wrong type and missing key behavior.
- duplicate item behavior.
- binary, empty, and long item behavior.
- capacity/error/expansion boundaries.
- NONSCALING full behavior.
- MADD/INSERT partial failure behavior.
- BF.INFO field shapes and full shape.
- COMMAND INFO, COMMAND GETKEYS, ACL DRYRUN.
- SCANDUMP/LOADCHUNK private layer-index cursor protocol.
- loading-state rejection for half-loaded keys.

## Required output format

Return markdown with:

- Stage objective.
- DESIGN.md constraints for this stage.
- Suggested runtime harness approach.
- Matrix cases to include.
- Required evidence files.
- Risks and likely false positives.
- PASS/BLOCKED criteria.
- Items to defer to later stages.

The main agent will persist your final message verbatim to `.codex/gemini-bloom-audit/v6/agents/stage04/planner_output.md`.
