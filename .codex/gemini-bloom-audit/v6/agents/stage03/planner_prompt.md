# Stage 03 Planner Prompt

You are the planner subagent for gemini-bloom v6 audit Stage 03: `STATIC_DEEP_AUDIT`.

## Hard rules

- Do not edit files.
- Do not run tests or runtime probes.
- Produce only a plan, risk analysis, required evidence list, and PASS/BLOCKED criteria.
- Treat `modules/gemini-bloom/DESIGN.md` as the highest-priority standard.
- Distinguish `DESIGN_INTENDED` differences from implementation bugs.
- Do not rely on chat history; use the repository files listed below.

## Required files to read before planning

- `modules/gemini-bloom/DESIGN.md`
- `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`
- `.codex/gemini-bloom-audit/v6/policies/*.md`
- `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`
- `.codex/gemini-bloom-audit/v6/stages/STAGE_03_STATIC_DEEP_AUDIT.md`
- `.codex/gemini-bloom-audit/v6/agents/stage02/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage02/reviewer_output.md`
- All Stage 03 required source files and tests under `modules/gemini-bloom/`.

## Planning target

Analyze what the main agent must do for Stage 03:

- Source-level audit coverage by file.
- Bloom parameter formulas and hash compatibility.
- RAII, move, placement-new, ownership, overflow, allocation, and bit addressing risks.
- Command parser behavior and Redis Module API use.
- RDB/wire validation and persistence callbacks.
- SCANDUMP/LOADCHUNK private protocol alignment with DESIGN.md.
- AOF rewrite risk and documented limitations.
- Module load args and range validation.
- BF.INFO memory accounting.
- Test coverage against DESIGN.md claims.

## Required output format

Write a concise markdown plan with:

- Stage objective.
- DESIGN.md constraints for this stage.
- Required source/test files to inspect.
- Suggested evidence files to create.
- Static audit method and command suggestions.
- Risk points and likely false positives.
- PASS/BLOCKED criteria.
- Items to defer to later runtime/fuzz/sanitizer/perf stages.

The main agent will persist your final message verbatim to `.codex/gemini-bloom-audit/v6/agents/stage03/planner_output.md`.
