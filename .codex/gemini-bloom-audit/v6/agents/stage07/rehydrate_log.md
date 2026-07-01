# Stage 07 Rehydrate Log

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
- `.codex/gemini-bloom-audit/v6/stages/STAGE_07_FUZZ_FAULT_SAFETY.md`
- `.codex/gemini-bloom-audit/v6/agents/stage06/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage06/reviewer_output.md`

## DESIGN constraints relevant to Stage 07

- DESIGN.md is authoritative and distinguishes RDB-level compatibility from private SCANDUMP/LOADCHUNK protocol incompatibility.
- gemini's LOADCHUNK loading state is intended to reject reads/writes on half-loaded filters.
- Existing Bloom keys and wrongtype keys must not be corrupted by malformed or mismatched LOADCHUNK.
- Deserialization is documented as stricter than RedisBloom, including checks around unknown flags, item counts, hash parameters, numeric narrowing, and aggregate sizes.
- Stage 03 already found open validation gaps: per-layer 2GB data-size cap is not enforced on all wire/RDB paths and expansionFactor above `kMaxExpansion` can be accepted.

## Prior stage status

- Stage 06 reviewer verdict: PASS.
- Stage 06 commit pushed: `5dc3706`.
- Stage 06 scope: persistence/transport compatibility passed on Redis 6.2.17 + RedisBloom v2.4.20 with DESIGN-intended private-protocol gaps preserved.

## Stage 07 boundaries

- Do not modify production code.
- Audit malicious inputs, fuzz corpus, malformed RDB/wire/LOADCHUNK, and failure-safety behavior.
- Runtime fuzz must record seeds, corpus rules, commands, exit codes, logs, and any reduced repro.
- If environment prevents runtime fuzz, mark BLOCKED with concrete evidence and provide static corpus coverage.
