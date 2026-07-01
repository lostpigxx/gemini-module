# Stage 06 Rehydrate Log

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
- `.codex/gemini-bloom-audit/v6/stages/STAGE_06_PERSISTENCE_TRANSPORT.md`
- `.codex/gemini-bloom-audit/v6/agents/stage05/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage05/reviewer_output.md`

## DESIGN.md constraints relevant to Stage 06

- RDB, DUMP/RESTORE, MIGRATE, psync/fullsync, and RDB-preamble AOF are compatibility commitments.
- Compatibility scope remains Redis 6.2.17 + RedisBloom v2.4.20 unless evidence says otherwise.
- `BF.SCANDUMP` / `BF.LOADCHUNK` cross-implementation incompatibility is `DESIGN_INTENDED`.
- command-AOF rewrite with `aof-use-rdb-preamble no` is not cross-compatible and must be classified `DESIGN_INTENDED` when failure is clean.
- gemini's own SCANDUMP/LOADCHUNK protocol must self round-trip and preserve inserted membership.
- AOF preamble disabled should at least be gemini self-compatible; cross-implementation incompatibility must not be misclassified as a bug.
- Transport checks must verify no false negatives for inserted items, stable `BF.CARD` where deterministic, TTL preservation where applicable, file load success, and absence/presence of expected Redis log errors.

## Prior stage state

- Stage 05 status: `PASS`.
- Stage 05 pushed commit: `6bb72cd73af7823b7872bb58edfa7e5317c8f53b`.
- Stage 05 exact oracle: Redis 6.2.17 + RedisBloom v2.4.20 (`MODULE LIST ver=20420`).
- Stage 05 finding `GBV6-05-001`: default Linux/GCC build fails due missing `<climits>` include; runtime oracle used audit-only `-include climits`.

## Stage 06 boundaries

- Do not modify production code.
- A clean transport harness may reuse the Stage 05 exact RedisBloom oracle and audit-only gemini module build, but must record this explicitly.
- DESIGN-promised transport failures are P1/P0 candidates.
- DESIGN-intended SCANDUMP/LOADCHUNK and command-AOF cross failures should be verified as safe/non-corrupting, not counted as product bugs.
