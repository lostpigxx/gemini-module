# Stage 06 Main Execution

## Rehydration

Re-read DESIGN.md, LOOP_CONTROL_BATCH.md, all policies, LOOP_STATE.md, Stage 06 file, and Stage 05 result/reviewer before execution. Rehydration log: `.codex/gemini-bloom-audit/v6/agents/stage06/rehydrate_log.md`. Planner output reviewed: `.codex/gemini-bloom-audit/v6/agents/stage06/planner_output.md`.

## Execution

- Reused Stage 05 exact oracle after verifying paths in Docker container `strange_feynman`.
- Recorded independent environment snapshot in `.codex/gemini-bloom-audit/v6/evidence/stage06/env_snapshot.txt`.
- Ran cross-implementation transport matrix and wrote `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix/cross_impl_transport_results.json`.
- Ran extended transport matrix for MIGRATE/TTL and supplemental AOF/live paths and wrote `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix/extended_transport_results.json`.
- Added and ran gemini self-transport harness `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix/stage06_gemini_self_transport.py`; raw result `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix/gemini_self_transport_results.json`.
- Generated combined matrix `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.json` and readable matrix `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md`.

## Planner Review

Planner requirements were satisfied: all required evidence roots exist, DESIGN-promised paths were classified separately from DESIGN-private SCANDUMP/LOADCHUNK and command-AOF paths, and Stage 05 build caveat `GBV6-05-001` was carried forward instead of hidden.

## Outcome

Stage 06 main verdict before reviewer: PASS.

- RDB, DUMP/RESTORE, MIGRATE, fullsync, and AOF RDB-preamble compatibility passed under Redis 6.2.17 + RedisBloom v2.4.20.
- gemini self transport passed for RDB restart, DUMP/RESTORE, fullsync, AOF preamble yes/no, and SCANDUMP/LOADCHUNK.
- Cross command-AOF no-preamble and cross SCANDUMP/LOADCHUNK were classified DESIGN_INTENDED_INCOMPATIBLE.
- No new production code changes were made.
