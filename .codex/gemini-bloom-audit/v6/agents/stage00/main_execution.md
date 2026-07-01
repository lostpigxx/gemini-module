# Stage 00 Main Execution

## Planner review

Planner output reviewed: `.codex/gemini-bloom-audit/v6/agents/stage00/planner_output.md`.

Adopted planner guidance:

- Stage 00 is a design-contract extraction stage, not a runtime/build stage.
- No GTest, TCL, Redis runtime, Docker, fuzz, sanitizer, or RedisBloom oracle commands should run here.
- DESIGN.md claims should be split into design commitments, design-intended differences, and verification claims.
- DESIGN.md's own fixture/CI/test claims should be audited as claims and not treated as already verified.

Main-agent additions:

- Recorded the branch/control-source nuance: target branch was created from `origin/main`; v6 control assets were restored from `codex/gemini-bloom-v6-review` after confirming no `modules/gemini-bloom` diff between that branch and `origin/main`.
- Performed static file inventory checks for module tests, claimed RedisBloom compat fixture path, and `.github`.
- Checked source comments for SCANDUMP/LOADCHUNK compatibility wording against DESIGN.md's private protocol boundary.

## Execution summary

Created required Stage 00 files:

- `.codex/gemini-bloom-audit/v6/agents/stage00/rehydrate_log.md`
- `.codex/gemini-bloom-audit/v6/agents/stage00/planner_prompt.md`
- `.codex/gemini-bloom-audit/v6/agents/stage00/planner_output.md`
- `.codex/gemini-bloom-audit/v6/agents/stage00/design_contract.md`
- `.codex/gemini-bloom-audit/v6/agents/stage00/design_claims_matrix.md`
- `.codex/gemini-bloom-audit/v6/agents/stage00/findings.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage00/commands.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage00/stdout.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage00/stderr.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage00/exit_codes.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage00/env_snapshot.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage00/evidence_index.md`

## Evidence gathered

- Branch and HEAD baseline: `.codex/gemini-bloom-audit/v6/evidence/stage00/env_snapshot.txt`
- Command list and exit codes: `.codex/gemini-bloom-audit/v6/evidence/stage00/commands.txt`, `.codex/gemini-bloom-audit/v6/evidence/stage00/exit_codes.txt`
- Module/test inventory: `.codex/gemini-bloom-audit/v6/evidence/stage00/stdout.log`
- Missing compat fixture and `.github`: `.codex/gemini-bloom-audit/v6/evidence/stage00/stderr.log`
- DESIGN.md line references and `sb_chain.h` comment mismatch: `.codex/gemini-bloom-audit/v6/evidence/stage00/stdout.log`

## Non-executed scope

The following were intentionally not run in Stage 00 and remain `VERIFY_LATER` or `NOT_VERIFIED` until their mapped stages:

- CMake build and existing tests.
- Redis runtime command semantics.
- RedisBloom v2.4.20 oracle.
- RDB/DUMP/RESTORE/MIGRATE/fullsync/AOF runtime checks.
- Fuzz/fault injection.
- ASAN/UBSAN or other sanitizer runs.
- Replica/cluster/ACL/COMMAND metadata runtime checks.
- Performance/resource stress testing.
