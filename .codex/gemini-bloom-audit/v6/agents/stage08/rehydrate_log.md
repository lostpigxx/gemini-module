# Stage 08 Rehydrate Log

## Files reread

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
- `.codex/gemini-bloom-audit/v6/stages/STAGE_08_SANITIZER_MEMORY.md`
- `.codex/gemini-bloom-audit/v6/agents/stage07/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage07/reviewer_output.md`

## DESIGN constraints relevant to Stage 08

- RDB and `BF.LOADCHUNK` payloads are untrusted input and must not cause UB, out-of-bounds access, leaks, or use-after-free.
- The implementation relies on RAII/move semantics, placement-new layer relocation, Redis Module allocation wrappers, and explicit validation before allocation/cast.
- DESIGN-private SCANDUMP/LOADCHUNK incompatibility remains `DESIGN_INTENDED`; sanitizer findings should focus on memory safety and UB, not RedisBloom private-protocol differences.
- Stage 07 found P1 data-integrity bugs without Redis process crashes; Stage 08 must distinguish sanitizer memory/UB defects from functional data-integrity failures.

## Prior stage status

- Stage 07 reviewer verdict: PASS.
- Stage 07 commit pushed: `f0d908cade71038cf7e59b7c783c536569fb05b9`.
- Stage 07 findings: `GBV6-07-001`, `GBV6-07-002` P1 OPEN.

## Stage 08 boundaries

- Do not modify production code.
- Attempt ASAN/UBSAN build and test first.
- If sanitizer module cannot load into Redis, record concrete BLOCKED evidence and run available alternatives: GTest sanitizer, valgrind if present, static fallback.
- Do not report an ASAN runtime-loading failure as test PASS.
