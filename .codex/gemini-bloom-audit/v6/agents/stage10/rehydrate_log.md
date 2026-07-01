# Stage 10 Rehydrate Log

## Files reread

- `modules/gemini-bloom/DESIGN.md` (700 lines; read in chunks)
- `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`
- `.codex/gemini-bloom-audit/v6/policies/00_design_first_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/01_rehydration_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/02_subagent_protocol.md`
- `.codex/gemini-bloom-audit/v6/policies/03_evidence_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/04_quality_gates.md`
- `.codex/gemini-bloom-audit/v6/policies/05_commit_push_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/06_final_report_policy.md`
- `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`
- `.codex/gemini-bloom-audit/v6/stages/STAGE_10_PERF_RESOURCE.md`
- `.codex/gemini-bloom-audit/v6/agents/stage09/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage09/reviewer_output.md`

## Stage 10 DESIGN constraints

- Stage 10 is an audit-signal pass, not a formal benchmark.
- Resource limits from DESIGN.md are the main correctness criteria: capacity `1..2^30`, expansion `0..32768`, per-layer data size `<=2GB`, total data size `<=4GB`, max layers `<=1024`, bitsPerEntry `<=1000`.
- `BF.INFO Size` is intentionally different from RedisBloom and includes the C++ object plus preallocated layer slots. It must be compared only to Redis memory metrics as an accounting signal, not a RedisBloom equivalence claim.
- `EXPANSION 1` can create more layers and degrade query performance; this is a known limitation to measure and explain.
- `BF.SCANDUMP` uses the private layer-index cursor protocol and emits one full bit-array chunk per layer. Large-layer chunk size should match the layer data size, not RedisBloom's 16MB byte-offset protocol.
- `LOADCHUNK` half-loaded persistence remains an open P1 from Stage 07/09 and must not be confused with normal completed-filter performance.

## Previous stage state

- Stage 09 reviewer verdict: `PASS`.
- Completed-filter replica/cluster/metadata/ACL smoke passed.
- Carry-forward: `GBV6-07-002` remains P1 OPEN with Stage 09 fullsync evidence.
- `ACL DRYRUN` remains blocked in Redis 6.2.x, and cluster ASK remains `NOT_VERIFIED`.

## Stage 10 boundaries

- Default remains audit-only: no production source changes.
- Evidence must be under `.codex/gemini-bloom-audit/v6/evidence/stage10/`.
- Do not call small sampled latency results a formal benchmark.
- Avoid unsafe huge allocations; capacity `2^30` boundary can be verified by command acceptance/rejection and measured RSS only if the environment safely supports it.

