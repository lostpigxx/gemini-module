# Stage 03 Rehydrate Log

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
- `.codex/gemini-bloom-audit/v6/stages/STAGE_03_STATIC_DEEP_AUDIT.md`
- `.codex/gemini-bloom-audit/v6/agents/stage02/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage02/reviewer_output.md`

## Previous stage state

- Stage 02 reviewer verdict: `PASS`.
- Stage 02 commit created and pushed after reviewer: `77566087a4a58a0ac3bd790b73e64276fb045a90`.
- Stage 02 established a clean fallback build and direct GTest baseline, but left two test-infrastructure findings open:
  - `GBV6-02-001`: CMake `bloom_test` target fails at runtime on this macOS host because GTest dylib RPATH is missing.
  - `GBV6-02-002`: TCL expected gaps are counted as failures, producing a nonzero documented integration command.

## DESIGN.md constraints relevant to Stage 03

- `DESIGN.md` is the highest-priority standard. Explicit design limits such as no RESP3 support, private SCANDUMP/LOADCHUNK, command-AOF non-interoperability, and RedisBloom/Redis 8 same-instance conflicts must not be misclassified as product bugs.
- RDB-level compatibility, DUMP/RESTORE, MIGRATE, fullsync replication, and RDB-preamble AOF are design commitments. Static audit must verify implementation structure against these commitments and mark runtime proof for later stages where needed.
- RDB and LOADCHUNK payloads are untrusted. Static audit must inspect field validation, narrowing checks, overflow guards, allocation limits, unknown flags, RawBits rejection, Loading flag stripping, and total data-size limits.
- Command parser behavior is intentionally stricter than RedisBloom v2.4.20 for unknown/duplicate options and some error ordering. Static audit must distinguish intended strictness from undocumented behavior or implementation drift.
- `BF.INFO Size` intentionally uses gemini's own accounting and may exceed RedisBloom. Static audit must check whether the implementation follows that accounting rather than expecting RedisBloom equality.
- `SCANDUMP`/`LOADCHUNK` uses a gemini-private layer-index cursor protocol. Static audit must verify source comments, wire structs, and AOF rewrite wording do not overclaim RedisBloom wire compatibility.
- Tests are part of the audit surface. Static audit must compare coverage claims in `DESIGN.md` against actual test files and Stage 02 evidence.

## Stage 03 scope limits

- This stage is a source-level deep audit. It may run read-only analysis commands and small compile/introspection commands if needed, but should not perform broad runtime matrices reserved for Stage 04 through Stage 10.
- Production code under `modules/gemini-bloom/src/**` must not be modified in this audit loop.
- Findings require concrete file/function/line evidence or explicit `NOT_VERIFIED` handoff to later stages.
- Stage 03 required outputs are:
  - `.codex/gemini-bloom-audit/v6/agents/stage03/static_audit_by_file.md`
  - `.codex/gemini-bloom-audit/v6/agents/stage03/invariant_map.md`
  - `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md`
  - `.codex/gemini-bloom-audit/v6/agents/stage03/design_vs_code_matrix.md`
