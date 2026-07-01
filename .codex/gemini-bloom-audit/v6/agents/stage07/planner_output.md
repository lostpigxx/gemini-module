# Stage 07 Planner Output

## Stage Objective

Audit malicious/untrusted input handling, fuzz-style malformed payloads, abnormal LOADCHUNK/RDB flows, and fault-safety boundaries for gemini-bloom, with emphasis on:

- `BF.LOADCHUNK` private wire protocol
- RDB serialized payload validation
- numeric edge cases and narrowing/overflow guards
- loading-state protection against partial imports and false negatives
- fault injection around persistence/rewrite paths, if environment allows

Stage 07 should not classify RedisBloom SCANDUMP/LOADCHUNK incompatibility as a bug. That boundary is `DESIGN_INTENDED`.

## DESIGN Constraints

Relevant DESIGN constraints:

- RDB compatibility is promised for `MBbloom--`, encver 2/4, and Redis-native paths: RDB file load, DUMP/RESTORE, MIGRATE, fullsync, and RDB-preamble AOF.
- `BF.SCANDUMP` / `BF.LOADCHUNK` are explicitly private gemini protocols and are not RedisBloom-compatible.
- RDB files and LOADCHUNK payloads are untrusted input.
- Validation must happen before use, casts, or allocation.
- `ValidateLayerFields()` is shared by RDB and wire paths.
- Reject `numLayers == 0` or `numLayers > 1024`, unknown persistent flags, `RawBits`, non-fixed filter with `expansionFactor == 0`, `sum(itemCount) != totalItems`, `itemCount > capacity`, `sum(dataSize) > 4GB`, and invalid capacity/totalBits/hashCount/fpRate/bitsPerEntry/dataSize/log2Bits.
- DESIGN also claims per-layer data size `<= 2GB`, total data size `<= 4GB`, and max expansion `<= 32768`.
- Stage 03 already found likely gaps: `GBV6-03-001` for missing per-layer 2GB cap in RDB/wire deserialization and `GBV6-03-002` for accepting expansion factors above `kMaxExpansion`.
- Loading constraints: `LOADCHUNK key 1 <header>` creates a shell and marks Loading; `cursor > 1` only accepted for keys in Loading state; normal BF read/write commands on Loading keys return `ERR filter is being loaded`; completed Bloom keys reject `LOADCHUNK cursor>1`; malformed LOADCHUNK must not overwrite or corrupt existing keys; Loading flag is runtime-only and must not persist through RDB/wire.

## Files To Inspect

- `modules/gemini-bloom/src/bloom_rdb.cc`
- `modules/gemini-bloom/src/bloom_rdb.h`
- `modules/gemini-bloom/src/bloom_commands.cc`
- `modules/gemini-bloom/src/bloom_commands.h`
- `modules/gemini-bloom/src/sb_chain.cc`
- `modules/gemini-bloom/src/sb_chain.h`
- `modules/gemini-bloom/src/bloom_filter.cc`
- `modules/gemini-bloom/src/bloom_filter.h`
- `modules/gemini-bloom/tests/bloom_rdb_test.cc`
- `modules/gemini-bloom/tests/tcl/bloom_test.tcl`
- `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md`
- `.codex/gemini-bloom-audit/v6/agents/stage03/design_vs_code_matrix.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.json`

## Commands The Main Agent Should Run

Static inspection:

```bash
rg -n "DeserializeHeader|SerializeHeader|ValidateLayerFields|RdbLoadBloom|RdbSaveBloom|LoadChunk|ScanDump|Loading|RawBits|kMaxExpansion|kMaxLayers|4GB|2GB|dataSize|totalDataSize|expansionFactor|itemCount|totalItems" modules/gemini-bloom/src modules/gemini-bloom/tests
```

```bash
rg -n "LOADCHUNK|SCANDUMP|malformed|truncated|RawBits|unknown flags|numLayers|bitsPerEntry|hashCount|log2Bits|itemCount|totalItems|expansion|Loading" modules/gemini-bloom/tests
```

Runtime/fuzz commands must be recorded in `commands.txt`. Candidate commands:

```bash
cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) --target bloom_test
```

```bash
./build/modules/gemini-bloom/tests/bloom_rdb_test --gtest_filter='*Rdb*:*Wire*:*LoadChunk*:*Malformed*:*Invalid*'
```

```bash
tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl ./build/redis_bloom.so
```

If existing build targets are affected by known Stage 02/05 build issues, use the documented audit workaround path from Stage 05/06 and keep `GBV6-05-001` open.

## Fuzz Corpus Classes

### Malformed LOADCHUNK

Cover empty/truncated/overlong/random header blobs; valid header plus trailing junk; `numLayers = 0` and `1025`; unknown, `RawBits`, and `Loading` flags; expansion `0`, `1`, `32768`, `32769`, `UINT_MAX`; mismatched data sizes; huge per-layer or total data-size metadata; short/long data chunks; cursor `0`, negative, non-integer, skipped, repeated, after completion, missing key, completed Bloom key, existing Bloom key, and string key.

Expected safety: stable error, no crash, no key overwrite, existing membership preserved, no completed-key false negatives, no partial key exposed as queryable.

### RDB Serialized Payload

Cover truncated stream and blob body, encver 2/4 edge cases, invalid flags, `totalItems` mismatch, `itemCount > capacity`, zero capacity/totalBits, data-size mismatches, total data-size near/above 4GB, per-layer data-size near/above 2GB, and high-bit cast bypass cases.

Expected safety: malformed RDB returns load failure/null, Redis survives RESTORE/RDB load attempts, no partial key becomes normal usable Bloom, valid RedisBloom-compatible corpus remains accepted.

### Numeric Edge Cases

Cover NaN/Inf/zero/negative/out-of-range `fpRate` and `bitsPerEntry`, valid and off-by-one `hashCount`, `log2Bits` 63/64/high-bit cases, non-power-of-two `totalBits`, extreme `capacity`, `expansion` 0/1/32768/32769/UINT_MAX, extreme `itemCount`, and summed-field overflow.

Expected safety: no narrowing before range check, no unchecked arithmetic overflow, no impossible allocation attempt, invalid metadata rejected consistently.

### Loading-State Behavior

After valid header only and after one valid data chunk but before final layer, run `BF.ADD`, `BF.MADD`, `BF.INSERT`, `BF.EXISTS`, `BF.MEXISTS`, `BF.INFO`, `BF.CARD`, `BF.SCANDUMP`, and `BF.LOADCHUNK cursor>1`. After malformed data chunk during Loading, verify no normal query returns partial data. After final chunk, verify Loading clears, membership has no false negatives, `SCANDUMP` completes, and `LOADCHUNK cursor>1` rejects overwrite.

### Fault Injection

If feasible, cover kill during `BGSAVE`, kill during `BGREWRITEAOF`, restart after interrupted save/rewrite, malformed command-AOF replay with missing/corrupted/out-of-order LOADCHUNK, and low-memory/allocation-failure only if a safe harness exists.

## Required Evidence Files

- `commands.txt`
- `stdout.log`
- `stderr.log`
- `exit_codes.txt`
- `env_snapshot.txt`
- `evidence_index.md`
- `fuzz_seeds.txt`
- `fuzz_results.log`
- `safety_matrix.md`
- `corpus_manifest.md`
- `loadchunk_matrix.md`
- `rdb_payload_matrix.md`
- `numeric_edge_matrix.md`
- `loading_state_matrix.md`
- `fault_injection_matrix.md`
- `server_logs/`
- `crash_or_failure_repro/` if any failure occurs

For random/fuzz runs, record seed, corpus generator rules, exact command sequence, Redis/module version/path, raw transcript where practical, server log, and minimized repro for failures.

## Classification Rules

- `PASS`: case actually ran or was statically proven, with logs/matrix paths, and expected safety property held.
- `FAIL`: crash, abort, memory corruption symptom, Redis process exit, key corruption, false negative, invalid payload accepted contrary to DESIGN, existing key overwrite, or missing promised validation.
- `BLOCKED`: runtime/fuzz/fault check could not run due to environment/tooling/permission/dependency limitation with concrete evidence.
- `NOT_VERIFIED`: relevant area not covered and not blocked by concrete evidence.
- `DESIGN_INTENDED`: behavior differs from RedisBloom/general expectation but matches DESIGN, especially private SCANDUMP/LOADCHUNK incompatibility, command-AOF non-preamble cross incompatibility, loading-state rejection, and stricter malformed metadata rejection.

Severity: P0 for crash/process exit/memory corruption/persisted data loss/false negatives from malformed accepted payload; P1 for promised persistence or untrusted-input validation violation; P2 for missing resource-boundary enforcement or confidence-impacting fuzz gaps; P3 for misleading tests/docs; INFO for design-intended private protocol differences.

## Risks And False Positives

- Large-size metadata can accidentally attempt huge allocation; prefer mock/static or crafted headers that reject before allocation.
- Redis OOM/process-kill tests can be nondeterministic.
- Stage 03 findings are static until runtime-probed; do not overclaim exploitability without repro.
- RedisBloom behavior is not the oracle for private LOADCHUNK; DESIGN is.
- Stage 05 build workaround must be recorded and must not hide `GBV6-05-001`.
- Random fuzz without seed/corpus manifest is not evidence.
- Error string differences alone are not failures unless they affect safety.
- Half-loaded key inaccessibility is expected.

## PASS Criteria

Stage 07 can PASS if every required corpus class has evidence or justified `DESIGN_INTENDED`; `fuzz_seeds.txt`, `fuzz_results.log`, and `safety_matrix.md` are populated; malformed LOADCHUNK, malformed RDB payloads, numeric edge cases, loading-state behavior, and at least one feasible fault-injection path are covered; existing keys are not corrupted; loading commands are blocked until completion; no crash/process exit/false negative/accepted invalid untrusted payload is observed except documented open Stage 03 validation gaps; failures have minimized repros; and reviewer confirms no fuzz crash/failure was ignored.

## BLOCKED Criteria

Stage 07 should be `BLOCKED` if Redis/module runtime cannot start, module cannot be built or reused with a documented workaround, required runtime evidence cannot be logged, or crashes cannot be reproduced/minimized enough to classify. If only fault injection is blocked but core LOADCHUNK/RDB fuzz runs, classify specific rows `BLOCKED` and allow stage PASS with confidence caveat.
