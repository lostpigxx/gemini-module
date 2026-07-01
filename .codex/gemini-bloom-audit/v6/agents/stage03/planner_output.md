**Stage Objective**

Produce a source-only Stage 03 audit of `modules/gemini-bloom`, verifying implementation and tests against `modules/gemini-bloom/DESIGN.md` without edits, tests, or runtime probes. Classify deviations as `PASS`, `FAIL`, `BLOCKED`, `NOT_VERIFIED`, or `DESIGN_INTENDED`.

**DESIGN.md Constraints For This Stage**

- `DESIGN.md` is authoritative; RedisBloom differences explicitly documented there are not bugs.
- RDB compatibility is a design commitment: type name `MBbloom--`, encver `2/4`, field order, flags, hash seeds, double hashing, and RDB/DUMP/RESTORE/MIGRATE/fullsync/RDB-preamble AOF paths.
- SCANDUMP/LOADCHUNK is intentionally private layer-index protocol, not RedisBloom byte-offset compatible.
- command-AOF rewrite is intentionally gemini-only when `aof-use-rdb-preamble no`; default RDB-preamble AOF compatibility remains later runtime scope.
- Parser strictness is intended: unknown/repeated options rejected, `NOCREATE + CAPACITY/ERROR` rejected early, `EXPANSION 0` maps to NONSCALING.
- Resource limits must match design: capacity `1..2^30`, expansion `0..32768` for commands, config expansion `1..32768`, max layers `1024`, per-layer data `<=2GB`, total data `<=4GB`, `bitsPerEntry <= 1000`.
- `BF.INFO Size` uses gemini accounting including `ScalingBloomFilter`, reserved layer slots, and bit arrays.

**Required Source/Test Files To Inspect**

- `modules/gemini-bloom/DESIGN.md`
- `modules/gemini-bloom/CMakeLists.txt`
- `modules/gemini-bloom/src/redis_bloom_module.cc`
- `modules/gemini-bloom/src/bloom_commands.cc`
- `modules/gemini-bloom/src/bloom_commands.h`
- `modules/gemini-bloom/src/bloom_rdb.cc`
- `modules/gemini-bloom/src/bloom_rdb.h`
- `modules/gemini-bloom/src/bloom_filter.cc`
- `modules/gemini-bloom/src/bloom_filter.h`
- `modules/gemini-bloom/src/sb_chain.cc`
- `modules/gemini-bloom/src/sb_chain.h`
- `modules/gemini-bloom/src/bloom_config.cc`
- `modules/gemini-bloom/src/bloom_config.h`
- `modules/gemini-bloom/src/murmur2.cc`
- `modules/gemini-bloom/src/murmur2.h`
- `modules/gemini-bloom/src/rm_alloc.h`
- `modules/gemini-bloom/tests/bloom_filter_test.cc`
- `modules/gemini-bloom/tests/sb_chain_test.cc`
- `modules/gemini-bloom/tests/bloom_rdb_test.cc`
- `modules/gemini-bloom/tests/tcl/bloom_test.tcl`

**Suggested Evidence Files To Create**

- `.codex/gemini-bloom-audit/v6/agents/stage03/rehydrate_log.md`
- `.codex/gemini-bloom-audit/v6/agents/stage03/static_audit_by_file.md`
- `.codex/gemini-bloom-audit/v6/agents/stage03/invariant_map.md`
- `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md`
- `.codex/gemini-bloom-audit/v6/agents/stage03/design_vs_code_matrix.md`
- `.codex/gemini-bloom-audit/v6/agents/stage03/main_execution.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage03/commands.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage03/stdout.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage03/stderr.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage03/exit_codes.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage03/env_snapshot.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage03/evidence_index.md`

**Static Audit Method And Command Suggestions**

Use static-only inspection commands; do not build, run Redis, execute tests, or fuzz.

- Inventory files: `rg --files modules/gemini-bloom/src modules/gemini-bloom/tests modules/gemini-bloom`
- Locate invariants: `rg -n "kMax|BloomFlags|Validate|RdbLoad|RdbSave|AofRewrite|SCANDUMP|LOADCHUNK|ReplicateVerbatim|CreateCommand|OpenKey|ModuleTypeSetValue" modules/gemini-bloom`
- Review line evidence with `nl -ba <file> | sed -n '<start>,<end>p'`.
- Map DESIGN claims to implementation symbols:
  - Bloom math/hash: `BloomLayer::Create`, `OptimalBitsPerEntry`, `OptimalHashCount`, `Hash32Policy`, `Hash64Policy`, `ProbePosition`.
  - RAII/ownership: `BloomLayer` move/destructor, `ScalingBloomFilter` move/destructor, `AppendLayer`, `FromRdbShell`, `SetLayer`.
  - Commands/API: all `Cmd*` handlers and `RegisterBloomCommands`.
  - RDB/wire: `ValidateLayerFields`, `ReadFrom`, `WriteTo`, `SerializeHeader`, `DeserializeHeader`, callbacks.
  - Config: `BloomConfigLoad`.
  - Tests: compare GTest/TCL assertions against each DESIGN claim.

**Risk Points And Likely False Positives**

- High-risk static points:
  - Hash input length truncates to `INT_MAX`; classify whether this can create false negatives for items over 2GB or is unreachable/practical later-stage scope.
  - `ProbePosition` arithmetic wraps intentionally or accidentally; check C++ unsigned semantics and RedisBloom compatibility assumptions.
  - `BloomLayer::FromWireMeta` allocates before separate validation only if caller validates; verify all call paths.
  - `AppendLayer` expands array before validating next layer allocation; check failure leaves existing object coherent.
  - `Put()` treats Bloom false positive as duplicate, affecting cardinality by design of Bloom filters; do not misclassify unless DESIGN contradicts.
  - `ReplicateVerbatim` after partially successful MADD/INSERT must match reply and mutation semantics.
  - Loading state must block every read/write path except valid `LOADCHUNK` continuation.
  - RDB validation must reject unknown flags, RawBits, bad counts, narrowing bypasses, total data overflow, and malformed blobs.
  - `AofRewriteBloom` header allocation failure skips key after `LogIOError`; DESIGN documents this limitation, but severity depends on reporting.
  - `sb_chain.h` wire-format comment appears to claim RedisBloom cross-implementation compatibility; compare to existing `GBV6-00-002`.

- Likely false positives / `DESIGN_INTENDED`:
  - RESP3 unsupported.
  - SCANDUMP/LOADCHUNK not RedisBloom-compatible.
  - command-AOF rewrite not cross-implementation compatible.
  - `BF.INFO key FIELD` returns scalar rather than singleton array.
  - `BF.INFO Size` differs from RedisBloom.
  - Module name/version differs from RedisBloom.
  - Stricter parser behavior and loading-state protection.

**PASS/BLOCKED Criteria**

- `PASS` if every required file has a static audit entry, each DESIGN claim is mapped to code/test evidence or explicitly deferred, each potential issue has file/line evidence, and no conclusion depends on unrun runtime behavior.
- `PASS` may include open findings if they are evidence-backed and correctly classified.
- `BLOCKED` if required files are missing, static evidence artifacts cannot be written, source/test coverage cannot be inspected, or a required conclusion cannot be classified without runtime evidence and cannot be safely marked `NOT_VERIFIED`.
- `FAIL` findings require concrete source evidence and impact reasoning; avoid speculative bug labels.

**Defer To Later Stages**

- Runtime command semantics and Redis reply shape verification: Stage 04.
- RedisBloom v2.4.20 oracle and fixture compatibility: Stage 05.
- RDB/DUMP/RESTORE/MIGRATE/fullsync/AOF/SCANDUMP runtime transport validation: Stage 06.
- Malicious payload fuzzing and fault injection: Stage 07.
- ASAN/UBSAN/leak validation for move/placement-new/overflow concerns: Stage 08.
- Replica, cluster, ACL, and operational metadata behavior: Stage 09.
- Performance, memory pressure, huge item behavior, and resource-limit stress: Stage 10.
