# Stage 07 Planner Prompt

You are the Stage 07 planner for the gemini-bloom v6 audit loop.

Read and use:

- `modules/gemini-bloom/DESIGN.md`
- `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`
- `.codex/gemini-bloom-audit/v6/policies/*.md`
- `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`
- `.codex/gemini-bloom-audit/v6/stages/STAGE_07_FUZZ_FAULT_SAFETY.md`
- Stage 03 and Stage 06 result/reviewer outputs as relevant prior evidence.

Produce a Stage 07 plan only. Do not execute tests and do not modify files.

The plan must cover:

- DESIGN.md constraints relevant to untrusted input, RDB/wire validation, and private LOADCHUNK boundaries.
- Files and commands the main agent should inspect/run.
- Fuzz corpus classes for malformed LOADCHUNK, RDB serialized payload, numeric edge cases, loading-state behavior, and fault injection.
- Required evidence files and how to classify PASS/FAIL/BLOCKED/NOT_VERIFIED/DESIGN_INTENDED.
- Risks of false positives, including Bloom false positives and DESIGN-intended private protocol incompatibility.
- Concrete PASS/BLOCKED criteria.

The main agent will save your answer to `.codex/gemini-bloom-audit/v6/agents/stage07/planner_output.md`.
