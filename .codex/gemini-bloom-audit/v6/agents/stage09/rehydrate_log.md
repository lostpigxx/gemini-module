# Stage 09 Rehydrate Log

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
- `.codex/gemini-bloom-audit/v6/stages/STAGE_09_REPLICA_CLUSTER_OPS.md`
- `.codex/gemini-bloom-audit/v6/agents/stage08/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage08/reviewer_output.md`

## Stage 09 DESIGN constraints

- `psync / fullsync replication` is a DESIGN-committed compatible path, based on RDB snapshot transfer.
- DESIGN explicitly warns that live replication command stream can have `BF.CARD` differences in high false-positive scenarios, while membership correctness must remain intact.
- `BF.EXISTS`, `BF.MEXISTS`, `BF.INFO`, `BF.CARD`, and `BF.SCANDUMP` are readonly; `BF.SCANDUMP` is also fast.
- Write commands are expected to carry `write deny-oom`; command metadata and key extraction must be verified at runtime.
- RESP3 unsupported and RedisBloom SCANDUMP/LOADCHUNK incompatibility are DESIGN_INTENDED, not Stage 09 bugs.
- Redis 8 built-in Bloom and RedisBloom same-instance coexistence conflicts are known deployment limits.

## Previous stage state

- Stage 08 reviewer verdict: `BLOCKED`.
- Stage 08 blocker: `GBV6-08-BLOCK-001` because ASAN/UBSAN runtime and valgrind coverage are unavailable/incomplete.
- Stage 08 is continuable; Stage 09 must not claim sanitizer memory PASS.

## Stage 09 boundaries

- Default remains audit-only: no production source changes.
- Evidence must be under `.codex/gemini-bloom-audit/v6/evidence/stage09/`.
- Cluster environment may be blocked, but command metadata must still be verified with runtime evidence.
- If ACL DRYRUN is unavailable on the Redis version under test, preserve the Stage 04-style degraded classification.

