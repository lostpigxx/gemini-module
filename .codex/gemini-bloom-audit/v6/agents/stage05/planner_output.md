# Stage 05 Planner Output

## Stage Objective

Validate gemini-bloom against a RedisBloom v2.4.20 oracle, prioritizing the DESIGN.md compatibility contract: RDB-level transport paths must interoperate, while RESP3, BF.DEBUG, SCANDUMP/LOADCHUNK, and command-AOF rewrite cross-compatibility are explicitly out of scope or intentionally different.

## DESIGN Constraints And DESIGN_INTENDED Differences

`modules/gemini-bloom/DESIGN.md` is the authority. Do not classify these as bugs:

- gemini-bloom is not a RedisBloom drop-in replacement.
- Compatible paths: RDB file load, DUMP/RESTORE, MIGRATE, psync/fullsync RDB snapshot, and RDB-preamble AOF.
- Shared RDB contract: type name `MBbloom--`, encver 2/4, same field order, same MurmurHash2/64A seeds.
- Incompatible by design: BF.SCANDUMP/BF.LOADCHUNK cross-implementation protocol, command-AOF rewrite when `aof-use-rdb-preamble no`, RESP3-specific behavior, BF.DEBUG.
- Known command differences: BF.INFO field returns scalar in gemini vs singleton array in RedisBloom; BF.INFO Size differs; parser strictness differs for unknown/repeated options; BF.INSERT EXPANSION 0 differs; NOCREATE+CAPACITY error ordering/message differs; module name/version and some command flags differ.
- LOADCHUNK behavior differs by design: gemini uses loading-state protection and rejects cursor>1 overwrite of completed keys.

## Oracle Discovery / Acquisition Plan

First discover whether an exact oracle already exists locally:

- Check repo evidence/fixtures paths, especially `tests/compat/redisbloom-2.4.20/`; note prior finding GBV6-00-001 says this path was absent.
- Check local build artifacts, docker images, RedisStack installs, package caches, or tool scripts for Redis 6.2.17 and RedisBloom v2.4.20.
- Verify oracle with `redis-server --version`, `MODULE LIST`, and RedisBloom module version `20420`.
- Prefer Redis 6.2.17 + RedisBloom v2.4.20. If only Redis 6.2.16 or a non-2.4.20 RedisBloom is available, record it as degraded supplemental evidence and do not extrapolate to v2.4.20.

If unavailable, obtain from trusted local/package/docker source if allowed by the main agent environment. Do not load gemini and RedisBloom in the same Redis instance because command/type registration conflicts are DESIGN-documented.

## BLOCKED Evidence If Oracle Is Unavailable

To mark Stage 05 `BLOCKED`, evidence must prove the blocker, not just state it:

- `oracle_env.txt`: OS, Redis binaries found, module paths checked, docker/package availability, exact versions.
- `blocked_oracle.md`: every attempted discovery/acquisition path and why it failed.
- Raw command logs showing missing binary/module, download/network restriction, docker unavailable, module load failure, ABI mismatch, or wrong RedisBloom version.
- Server logs for failed `--loadmodule` attempts.
- Impact statement: RedisBloom v2.4.20 oracle comparison is unavailable, final confidence cannot be High per Policy 04.

## Two-Instance Comparison Plan If Oracle Is Available

Start two isolated Redis instances with separate ports and temp dirs:

- gemini instance: load `./build/redis_bloom.so`.
- oracle instance: load official RedisBloom v2.4.20.
- Capture port map, module list, server logs, raw RESP, normalized replies, and exit codes.

Corpus:

- empty reserve
- single-layer
- multi-layer
- fixed/NONSCALING
- expansion 1/2/4
- binary items including NUL and empty item
- long item
- large item if environment permits
- high false-positive scenario, avoiding strict negative-membership assumptions unless both bit arrays are expected identical

Compare:

- Command surface: supported BF commands, module metadata, COMMAND INFO/GETKEYS; classify BF.DEBUG absence as DESIGN_INTENDED.
- RESP2: raw and normalized replies for BF.RESERVE/ADD/MADD/INSERT/EXISTS/MEXISTS/INFO/CARD/SCANDUMP/LOADCHUNK.
- RESP3: capture raw replies; classify shape differences per DESIGN, not as failures.
- BF.INFO/parser: explicitly cover Size, field shape, missing key, duplicate options, unknown options, NOCREATE+CAPACITY, BF.INSERT EXPANSION 0, BF.RESERVE EXPANSION 0.
- SCANDUMP/LOADCHUNK: same-implementation round trips should work; cross-load gemini->RedisBloom and RedisBloom->gemini is expected incompatible. Verify failures are clean and do not corrupt existing keys.
- Transport compatibility, both directions where applicable: RDB file load, DUMP/RESTORE, MIGRATE with TTL preservation, fullsync replication via RDB snapshot, and RDB-preamble AOF with `aof-use-rdb-preamble yes`.
- Do not treat command-AOF rewrite with `aof-use-rdb-preamble no` as required compatibility.

## Required Evidence Files / Matrix Structure

Required Stage 05 files:

- `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_env.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_raw_resp.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_normalized.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/blocked_oracle.md` if blocked
- Policy files: `commands.txt`, `stdout.log`, `stderr.log`, `exit_codes.txt`, `env_snapshot.txt`, `evidence_index.md`

Matrix columns should include:

`ID | Area | Corpus | Source impl/version | Target impl/version | Redis version | Protocol/transport | Expected per DESIGN | Gemini actual | RedisBloom actual | Normalized verdict | Classification | Evidence path | Finding ID`

## Risks / False Positives

- Wrong RedisBloom version or RedisStack bundled version can invalidate v2.4.20 conclusions.
- Redis 6.2.16 vs 6.2.17 differences must be recorded; do not overclaim.
- Bloom false positives are not membership failures; inserted-item false negatives after transport are failures.
- BF.INFO Size, scalar-vs-array field replies, parser strictness, and error message differences are expected.
- SCANDUMP/LOADCHUNK cross-implementation failures are expected unless they crash or corrupt existing data.
- Redis 7 AOF multipart behavior can complicate RDB-preamble evidence; prefer Redis 6.2.17 where possible.
- Live command-stream replication is not the same as fullsync RDB compatibility; DESIGN notes BF.CARD can differ in high false-positive command replay scenarios.

## PASS / BLOCKED / FAIL Criteria

PASS:

- Exact RedisBloom v2.4.20 oracle is loaded and version-proven.
- Two-instance comparison evidence exists.
- All DESIGN-promised paths pass in required directions: RDB, DUMP/RESTORE, MIGRATE, fullsync, RDB-preamble AOF.
- All intentional differences are classified `DESIGN_INTENDED`.
- No unexplained raw/normalized diff remains.

BLOCKED:

- Exact v2.4.20 oracle cannot be obtained or loaded, with concrete evidence.
- Optional alternate-version results may be recorded only as degraded supplemental evidence.
- Final confidence must be downgraded.

FAIL:

- Any DESIGN-promised compatibility path fails with exact oracle.
- Transport causes false negatives, data loss, corrupt load, crash, or TTL loss where TTL is part of the tested path.
- An unexplained command/protocol difference outside DESIGN_INTENDED boundaries remains after normalization.
- Evidence is too incomplete for reviewer validation.
