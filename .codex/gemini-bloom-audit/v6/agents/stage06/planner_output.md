# Stage 06 Planner Output

## Stage Objective

Audit persistence and transport compatibility for gemini-bloom against the DESIGN.md contract: RDB, DUMP/RESTORE, MIGRATE, fullsync RDB snapshot, RDB-preamble AOF, command-AOF without preamble, and BF.SCANDUMP/BF.LOADCHUNK.

This stage should verify promised compatible paths, and separately verify DESIGN_INTENDED incompatible paths are clean, explicit, and non-corrupting.

## DESIGN Constraints

DESIGN.md is authoritative.

Compatible by design:

- RDB serialization via type name `MBbloom--`, encver 2/4, RedisBloom-compatible field order, MurmurHash2/64A seeds.
- RDB file loading both directions with RedisBloom v2.4.20.
- DUMP/RESTORE both directions.
- MIGRATE both directions, including TTL preservation.
- psync/fullsync replication via RDB snapshot both directions.
- AOF with `aof-use-rdb-preamble yes`, because Bloom data is stored as RDB payload.

DESIGN_INTENDED incompatibilities:

- BF.SCANDUMP/BF.LOADCHUNK is not RedisBloom-compatible. gemini uses private layer-index cursor protocol; RedisBloom v2.4.20 uses byte-offset chunks.
- command-AOF rewrite with `aof-use-rdb-preamble no` is not cross-implementation compatible because it emits implementation-private LOADCHUNK commands.
- gemini LOADCHUNK loading-state protection is intended: mid-load keys reject reads/writes; completed keys reject cursor>1 overwrite.
- BF.INFO Size, module name/version, BF.INFO field shape, parser strictness, BF.DEBUG, RESP3, and live command-stream cardinality drift are not Stage 06 transport failures unless they cause data loss or contradict persistence behavior.

## Required Matrix Paths And Corpora

Reuse Stage 05 paths as baseline evidence:

- `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_env.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/compat_matrix_results_redis62_redisbloom2420.json`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/extended_audit_results_redis62_redisbloom2420.json`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/redisbloom_compat_matrix.py`
- `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/redisbloom_extended_audit.py`

Required Stage 06 evidence roots:

- `.codex/gemini-bloom-audit/v6/evidence/stage06/rdb/`
- `.codex/gemini-bloom-audit/v6/evidence/stage06/dump_restore/`
- `.codex/gemini-bloom-audit/v6/evidence/stage06/migrate/`
- `.codex/gemini-bloom-audit/v6/evidence/stage06/replication/`
- `.codex/gemini-bloom-audit/v6/evidence/stage06/aof_preamble_yes/`
- `.codex/gemini-bloom-audit/v6/evidence/stage06/aof_preamble_no/`
- `.codex/gemini-bloom-audit/v6/evidence/stage06/scandump_loadchunk/`

Use the 9 Stage 05 corpora:

`binary_items`, `empty_scaling`, `expansion1`, `expansion4`, `fixed_full`, `large_empty_16mb`, `long_item`, `multi_exp2`, `single_layer`.

Also check DESIGN's fixture claim path `tests/compat/redisbloom-2.4.20/`; prior finding `GBV6-00-001` says it was absent, so do not rely on it unless reverified.

## Oracle And Build Reuse

Reuse Stage 05 oracle only after re-verifying:

- Redis server: Redis 6.2.17.
- RedisBloom: tag `v2.4.20`, `MODULE LIST` reports `bf ver=20420`.
- gemini module: `GeminiBloom ver=1`.
- RedisBloom module path from Stage 05: `/tmp/redisbloom-v2.4.20-stage05-v6/bin/linux-x64-release/redisbloom.so`.
- gemini audit module path from Stage 05: `/tmp/gemini-module-v6-stage05-build-workaround/redis_bloom.so`.

Important: Stage 05 found `GBV6-05-001`, default Linux/GCC build fails without `<climits>`. Stage 06 may use the audit-only `-include climits` build only if recorded clearly. If source changed since Stage 05, rebuild current gemini with the same workaround and preserve the build failure as an open finding, not a fix.

## Harness Shape

Use isolated Redis instances with separate ports/temp dirs, never load gemini and RedisBloom in one server. For every path, record source module/version, target module/version, Redis config, raw commands, normalized result, server logs, exit code, artifact sizes/hashes, and missing inserted items.

RDB:

- Populate source corpus.
- `SAVE`/`BGSAVE`, move/copy `dump.rdb` into target dir.
- Start target with target module.
- Verify all inserted items have no false negatives, source/target BF.CARD matches observed source card, stable BF.INFO fields match.

DUMP/RESTORE:

- Populate source corpus and optional TTL.
- `DUMP key`, `PTTL key`, `RESTORE target ttl serialized-value`.
- Test gemini->gemini, RedisBloom->gemini, gemini->RedisBloom.
- Verify TTL is preserved within timing tolerance.

MIGRATE:

- Start source/target with different modules.
- Populate source with TTL.
- Use MIGRATE both directions.
- Verify target membership, BF.CARD, stable BF.INFO, PTTL, and source/target key behavior according to MIGRATE options.

Fullsync replication:

- Populate source before replica attaches.
- Start target as replica with target module.
- Wait for fullsync complete and `master_link_status:up`.
- Verify target after snapshot. Keep live command-stream replay as supplemental only; do not confuse it with fullsync.

AOF preamble yes:

- Start source with `appendonly yes` and `aof-use-rdb-preamble yes`.
- Populate, rewrite AOF, restart target with same or opposite module.
- Verify RDB-preamble transport both directions.

AOF preamble no:

- Start with `appendonly yes` and `aof-use-rdb-preamble no`.
- gemini->gemini should round-trip.
- Cross implementation is DESIGN_INTENDED incompatible. Verify failure is clean: no crash, no silent false negatives, errors/logs explain LOADCHUNK/protocol mismatch.

SCANDUMP/LOADCHUNK:

- gemini self round-trip over all corpora.
- Verify loading-state behavior mid-load and final key correctness.
- Cross-load RedisBloom->gemini and gemini->RedisBloom must be classified DESIGN_INTENDED if failure is clean.
- Explicitly test existing-key protection, wrongtype behavior, and cursor>1 completed-key rejection for gemini.

## Required Evidence

At Stage 06 root include:

- `commands.txt`
- `stdout.log`
- `stderr.log`
- `exit_codes.txt`
- `env_snapshot.txt`
- `evidence_index.md`
- `oracle_env.txt`
- `transport_matrix.json`
- `transport_matrix.md`
- `artifact_manifest.txt` with RDB/AOF/DUMP sizes and hashes.

Each subdirectory should include path-specific raw RESP/logs, server logs, normalized summaries, and failures if any.

Classification rules:

- DESIGN-promised path pass: `PASS`.
- DESIGN-promised path fails with exact oracle: `FAIL`, likely P1.
- Environment/tool unavailable: `BLOCKED` with concrete evidence.
- SCANDUMP/LOADCHUNK cross incompatibility and command-AOF no-preamble cross incompatibility: `DESIGN_INTENDED` only if clean and non-corrupting.
- Not run without blocker proof: `NOT_VERIFIED`.

## Risks And False Positives

- Wrong RedisBloom version invalidates compatibility claims.
- Bloom false positives are acceptable; inserted-item false negatives are not.
- Source BF.CARD can be lower than item count due Bloom false positives; compare against observed source card.
- BF.INFO Size is implementation-specific and should not be exact cross-impl evidence.
- TTL checks need tolerance for elapsed time.
- Redis AOF layout differs by version; record Redis version and config.
- Live command replication is not equivalent to fullsync RDB snapshot.
- Existing Stage 05 gemini build workaround must not hide `GBV6-05-001`.

## PASS / BLOCKED / FAIL Criteria

PASS:

- Exact RedisBloom v2.4.20 oracle and gemini audit build are version-proven.
- All DESIGN-promised transport paths pass over required corpora or have justified corpus-specific exclusions.
- DESIGN_INTENDED incompatibilities are verified clean and non-corrupting.
- Evidence is complete and reviewer can trace every conclusion to files.

BLOCKED:

- Exact oracle, Redis server, docker/container, module build, MIGRATE, replication, or AOF environment is unavailable.
- Blocker has concrete logs/evidence.
- Final confidence must be downgraded for blocked critical transport paths.

FAIL:

- Any promised RDB/DUMP/RESTORE/MIGRATE/fullsync/RDB-preamble AOF path fails with exact oracle.
- Any transport causes false negatives for inserted items, data loss, crash, corrupt restart, or TTL loss.
- DESIGN_INTENDED incompatible paths silently corrupt existing keys or mislead as successful.
- Evidence is too incomplete for reviewer validation.
