# Stage 05 Main Execution

## Planner review

Planner output reviewed: `.codex/gemini-bloom-audit/v6/agents/stage05/planner_output.md`.

The planner's approach was adopted: first prove or block exact RedisBloom v2.4.20 oracle availability, then run two-instance comparisons and classify results against DESIGN.md rather than generic RedisBloom compatibility expectations.

## Oracle discovery and build

Local repository and host search confirmed that the DESIGN.md fixture path `tests/compat/redisbloom-2.4.20/` remains absent.

The running Docker container `strange_feynman` had Redis 6.2.17 but only a prebuilt RedisBloom v2.4.9 environment. The exact RedisBloom v2.4.20 oracle was built from the official RedisBloom tag:

- Source: `https://github.com/RedisBloom/RedisBloom.git`
- Tag: `v2.4.20`
- Checkout commit: `c44f89506d6e4af6362190f7b2eb7df526489107`
- Module: `/tmp/redisbloom-v2.4.20-stage05-v6/bin/linux-x64-release/redisbloom.so`
- Version proof: `MODULE LIST` returned `name=bf ver=20420`

Evidence:

- `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_env.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/commands.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_raw_resp.log`

## Gemini build caveat

The default Linux/GCC build of current gemini-bloom failed in the container:

- Error: `UINT_MAX` not declared in `modules/gemini-bloom/src/bloom_rdb.cc`
- Compiler note: `UINT_MAX` is defined in `<climits>`
- Evidence: `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/gemini_default_build_stderr.log`

To continue RedisBloom runtime comparison without modifying production code, the audit built a gemini module with `-DCMAKE_CXX_FLAGS=-include climits`.

This workaround is explicitly recorded in:

- `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_env.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_normalized.md`

## Runtime comparison executed

Main matrix:

- Harness: `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/redisbloom_compat_matrix.py`
- Result JSON: `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/compat_matrix_results_redis62_redisbloom2420.json`
- Summary: 126 cells, 88 pass, 38 fail, 0 errors.

The 38 failures are all in DESIGN.md non-compatible or known-limited path families:

- `BF.SCANDUMP` / `BF.LOADCHUNK` cross-implementation paths: 18 failures, classified `DESIGN_INTENDED`.
- command-AOF rewrite without RDB preamble: 18 failures, classified `DESIGN_INTENDED`.
- live replication command stream on `expansion1`: 2 failures due `BF.CARD` drift with no false negatives, classified `DESIGN_INTENDED` / known limit.

DESIGN-promised paths passed:

- RDB file load/save: 18/18 pass.
- DUMP/RESTORE: 18/18 pass.
- RDB-preamble AOF rewrite: 18/18 pass.
- fullsync replication snapshot: 18/18 pass.

Extended audit:

- Harness: `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/redisbloom_extended_audit.py`
- Result JSON: `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/extended_audit_results_redis62_redisbloom2420.json`
- MIGRATE and DUMP/RESTORE with TTL: 2/2 pass.
- Incremental AOF command stream: 4 pass, 2 fail on `expansion1` `BF.CARD` drift, matching DESIGN.md known limit.
- `BF.DEBUG`: RedisBloom supports it; gemini returns unknown command, classified `DESIGN_INTENDED`.

Focused raw RESP diff:

- Harness: `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/stage05_raw_resp_diff.py`
- Evidence: `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_raw_resp.log`

## Key observed command differences

Classified `DESIGN_INTENDED`:

- Module identity: `GeminiBloom ver=1` vs RedisBloom `bf ver=20420`.
- `BF.INFO key FIELD`: gemini scalar vs RedisBloom singleton array.
- `BF.INFO SIZE`: gemini memory accounting differs from RedisBloom.
- `BF.INSERT NOCREATE CAPACITY`: stricter gemini parser/error path.
- `BF.INSERT EXPANSION 0`: gemini accepts/maps to non-scaling; RedisBloom rejects.
- `BF.DEBUG`: unsupported by gemini.
- SCANDUMP/LOADCHUNK layer-index vs RedisBloom byte-offset protocol.
- command-AOF rewrite without RDB preamble is not cross-compatible.
- live command replay can produce `BF.CARD` drift in high false-positive `EXPANSION 1` corpus, while inserted-item membership remains intact.

Matched RedisBloom:

- `BF.MADD` and `BF.INSERT` partial failure on fixed filters now both truncate at the first full error.
- RDB-family transport paths preserve inserted-item membership with zero false negatives over the tested corpus.

## Evidence files

Required Stage 05 evidence:

- `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_env.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_raw_resp.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_normalized.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md`

Evidence policy files:

- `.codex/gemini-bloom-audit/v6/evidence/stage05/commands.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/stdout.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/stderr.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/exit_codes.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/env_snapshot.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/evidence_index.md`

## New findings

`GBV6-05-001` P2 OPEN: Linux/GCC default build fails because `bloom_rdb.cc` uses `UINT_MAX` without including `<climits>`.

Evidence:

- `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/gemini_default_build_stderr.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/gemini_default_build_exit_code.txt`

## Final report impact

- Final report may state that Redis 6.2.17 + RedisBloom v2.4.20 RDB/DUMP/RESTORE/MIGRATE/RDB-preamble AOF/fullsync compatibility passed over the Stage 05 corpus.
- Final report must state that runtime comparison used an audit-only compile workaround due `GBV6-05-001`; default Linux/GCC build is a separate open issue.
- Final report must not claim SCANDUMP/LOADCHUNK or command-AOF rewrite cross-compatibility.
- Final report must carry forward live command replay `BF.CARD` drift as a DESIGN.md known limit, not a membership-data-loss failure.
