# Stage 07 Main Execution

## Rehydration

Re-read DESIGN.md, LOOP_CONTROL_BATCH.md, all policies, LOOP_STATE.md, Stage 07 file, and Stage 06 result/reviewer before execution.

Rehydration log: `.codex/gemini-bloom-audit/v6/agents/stage07/rehydrate_log.md`.

Planner output reviewed: `.codex/gemini-bloom-audit/v6/agents/stage07/planner_output.md`.

## Planner Adoption

The planner's focus areas were adopted:

- malformed `BF.LOADCHUNK` headers and cursor sequences;
- RDB/wire numeric and resource-boundary cases;
- loading-state command blocking;
- half-loaded persistence/AOF replay fault injection;
- explicit preservation of DESIGN_INTENDED RedisBloom SCANDUMP/LOADCHUNK incompatibility.

The main execution added a targeted runtime check for saving/replaying a header-only Loading key because it is directly implied by the Stage 07 safety expectations.

## Execution

Runtime harness:

- Script: `.codex/gemini-bloom-audit/v6/evidence/stage07/stage07_fuzz_fault_safety.py`
- Result JSON: `.codex/gemini-bloom-audit/v6/evidence/stage07/stage07_fuzz_results.json`
- Seed: `2970124295` / `0xB100F007`
- Environment: Redis 6.2.17 and Stage 05 audit workaround gemini module `/tmp/gemini-module-v6-stage05-build-workaround/redis_bloom.so`

The harness covered:

- 94 LOADCHUNK header cases, including 64 seeded random payloads.
- Existing Bloom/string key preservation.
- Header-only Loading command blocking.
- Bad data chunk handling while Loading.
- Ordered LOADCHUNK completion.
- Missing/completed/bad cursor handling.
- Out-of-order and repeated same-sized cursor sequences.
- Header-only RDB `SAVE` restart and command-AOF `BGREWRITEAOF` restart.
- Static RDB/wire resource-boundary review for unsafe multi-GB metadata cases, backed by `.codex/gemini-bloom-audit/v6/evidence/stage07/static_inspection.log` and Stage 03 findings.

GTest build probe:

- `cmake --build /tmp/gemini-module-v6-stage05-build-workaround --target bloom_rdb_test -j2`
- Result: no such target in the Stage 05 module-only build, recorded in `.codex/gemini-bloom-audit/v6/evidence/stage07/env_snapshot.txt`.

## Outcome

Stage 07 main verdict before reviewer: audit PASS with findings.

No Redis process crash was observed and the server remained alive at the end of the fuzz run. The audit found data-integrity failures in malformed LOADCHUNK sequencing and half-loaded persistence/replay.

Evidence index: `.codex/gemini-bloom-audit/v6/evidence/stage07/evidence_index.md`.

## Findings

New:

- `GBV6-07-001` P1 OPEN: `BF.LOADCHUNK` accepts out-of-order or repeated data chunks and can complete a filter with false negatives.
- `GBV6-07-002` P1 OPEN: half-loaded `LOADCHUNK` keys persist/replay as completed filters with false negatives.

Confirmed:

- `GBV6-03-001`: per-layer 2GB data-size cap missing from shared RDB/wire validation.
- `GBV6-03-002`: runtime header fuzz confirms expansion factors above `kMaxExpansion` are accepted.

## Coverage Notes

- Runtime fuzz: PASS with findings.
- Loading-state normal command blocking: PASS.
- Existing key preservation: PASS.
- Header malformed rejection: mostly PASS; expansion over max is FAIL and mapped to `GBV6-03-002`.
- Fault injection: RDB and command-AOF half-loaded replay covered. Direct process-kill during BGSAVE was NOT_VERIFIED because the deterministic half-loaded persistence fault was sufficient and host-level child-kill behavior is nondeterministic.

## Agent Closure

Planner output saved. Planner will be considered closed after reviewer PASS and state update.

Reviewer output pending.
