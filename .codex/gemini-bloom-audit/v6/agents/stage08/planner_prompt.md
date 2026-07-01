# Stage 08 Planner Prompt

You are the Stage 08 planner for the gemini-bloom v6 audit loop.

Read and use:

- `modules/gemini-bloom/DESIGN.md`
- `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`
- `.codex/gemini-bloom-audit/v6/policies/*.md`
- `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`
- `.codex/gemini-bloom-audit/v6/stages/STAGE_08_SANITIZER_MEMORY.md`
- Stage 02, Stage 03, Stage 07 result/reviewer outputs as relevant prior evidence.

Produce a plan only. Do not execute tests and do not modify files.

The plan must include:

- Stage objective and DESIGN constraints for memory safety/UB.
- Build/test commands to attempt for ASAN/UBSAN.
- How to classify sanitizer build failures, GTest failures, TCL Redis module loading failures, valgrind availability, and static fallback.
- Required evidence files and directory layout.
- Risks of false positives/false negatives.
- PASS/BLOCKED criteria.

The main agent will save your answer to `.codex/gemini-bloom-audit/v6/agents/stage08/planner_output.md`.
