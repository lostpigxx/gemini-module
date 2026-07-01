# Stage 05 Rehydrate Log

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
- `.codex/gemini-bloom-audit/v6/stages/STAGE_05_REDISBLOOM_COMPAT.md`
- `.codex/gemini-bloom-audit/v6/agents/stage04/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage04/reviewer_output.md`

## DESIGN.md constraints relevant to Stage 05

- DESIGN.md is the controlling standard, not RedisBloom protocol behavior in general.
- RedisBloom compatibility is claimed for RDB-level mechanisms only: RDB files, DUMP/RESTORE, MIGRATE, psync/fullsync, and RDB-preamble AOF.
- RedisBloom v2.4.20 with Redis 6.2.17 is the documented oracle baseline; other versions cannot be generalized without downgrading confidence.
- `BF.SCANDUMP` / `BF.LOADCHUNK` cross-implementation incompatibility is intentional because gemini uses a private layer-index cursor protocol.
- command-AOF rewrite without RDB preamble is intentionally not cross-compatible.
- RESP3 is unsupported; RESP3 shape differences must be classified as `DESIGN_INTENDED` when implementation remains well-formed.
- Command differences such as scalar `BF.INFO key FIELD`, strict parser behavior, `BF.INSERT EXPANSION 0`, module name, and metadata flags must be classified against DESIGN.md before judging compatibility.
- Missing RedisBloom fixture path from Stage 00 remains an open documentation/evidence finding and must be considered when judging reproducibility.

## Prior stage state

- Stage 04 status: `BLOCKED` for ACL DRYRUN verification, but reviewer verdict was `PASS` and Stage 04 allows continuation.
- Stage 04 pushed commit: `d884abb3e73c7ea4fa680c222bf1aa3a256b57c4`.
- Stage 04 verified all ten BF commands with raw RESP evidence.
- Stage 04 blocker `GBV6-04-BLOCK-001`: local Redis 6.2.16 lacks `ACL DRYRUN`.

## Stage 05 boundaries

- Do not modify production code.
- If RedisBloom v2.4.20 oracle cannot be obtained or loaded, record `BLOCKED` with evidence and degrade final confidence.
- If a different RedisBloom/Redis version is used, record the version precisely and do not claim RedisBloom v2.4.20 parity.
- DESIGN_INTENDED differences must not become product findings.
- DESIGN-claimed compatible transport failures must become P1/P0 findings with exact evidence.
