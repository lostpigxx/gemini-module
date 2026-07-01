# Stage 04 Rehydrate Log

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
- `.codex/gemini-bloom-audit/v6/stages/STAGE_04_RUNTIME_COMMAND_SEMANTICS.md`
- `.codex/gemini-bloom-audit/v6/agents/stage03/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage03/reviewer_output.md`

## Previous stage state

- Stage 03 reviewer verdict: `PASS`.
- Stage 03 commit created and pushed after reviewer: `dc0a3a8ad8e97e6631028d4152804f68a923ecbf`.
- Open Stage 03 findings carried forward:
  - `GBV6-03-001`: RDB/wire deserialization does not enforce DESIGN's per-layer 2GB data-size cap.
  - `GBV6-03-002`: RDB/wire deserialization accepts expansionFactor values above `kMaxExpansion`.
  - `GBV6-03-003`: TCL per-layer data-size cap test name/comment do not match the assertion.

## DESIGN.md constraints relevant to Stage 04

- Runtime command checks must cover all 10 BF commands: `BF.RESERVE`, `BF.ADD`, `BF.MADD`, `BF.INSERT`, `BF.EXISTS`, `BF.MEXISTS`, `BF.INFO`, `BF.CARD`, `BF.SCANDUMP`, `BF.LOADCHUNK`.
- DESIGN_INTENDED differences must not be treated as bugs:
  - RESP3 is unsupported; runtime should show RESP3 behavior without claiming support.
  - `BF.INFO key FIELD` returns a scalar, not a RedisBloom singleton array.
  - `BF.INFO Size` uses gemini-specific memory accounting.
  - `SCANDUMP`/`LOADCHUNK` uses a private layer-index cursor protocol and does not interoperate with RedisBloom byte-offset chunks.
  - Loading-state protection intentionally rejects reads/writes against half-loaded keys.
- Command parser strictness must match DESIGN: unknown/duplicate options rejected, `NOCREATE + CAPACITY/ERROR` rejected before key access, `EXPANSION 0` maps to NONSCALING.
- Command resource limits must match DESIGN for command/config paths: capacity `1..2^30`, error rate `(0,1)`, expansion `0..32768`.
- Stage 04 must not use RedisBloom oracle; RedisBloom comparisons are Stage 05.

## Stage 04 limits

- Runtime verification may build and load gemini-bloom into a local Redis instance, but must not modify production code or existing tests.
- Temporary harnesses and logs must live under `.codex/gemini-bloom-audit/v6/evidence/stage04/` or `.codex/gemini-bloom-audit/v6/agents/stage04/`.
- Runtime results must include raw RESP and normalized outcomes. Any unrun case must be marked `NOT_VERIFIED` or `BLOCKED`.
