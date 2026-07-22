# Stage 00 Design Claims Matrix

Status legend:

- `DESIGN_INTENDED`: Explicit design boundary; not a bug if implementation and docs honor it.
- `VERIFY_LATER`: Requires later static/runtime evidence.
- `DOC_RISK`: DESIGN/source/test/docs evidence conflict found in Stage 00.
- `NOT_VERIFIED`: Not checked in Stage 00.

| ID | Source | Claim | Type | Later stage(s) | Required evidence | Stage 00 status |
|---|---|---|---|---|---|---|
| DC-001 | DESIGN.md:5, 11-14 | Module is C++20 Scalable Bloom Filter implementing mainstream `BF.*`. | Design scope | 01, 03, 04 | Build inventory, command registration, runtime command tests. | VERIFY_LATER |
| DC-002 | DESIGN.md:9, 16-19 | Not RedisBloom source port and not RedisBloom drop-in replacement. | Design boundary | 03, 05, 11, 12 | Source provenance review, final report wording check. | DESIGN_INTENDED |
| DC-003 | DESIGN.md:25, 398-421 | RDB type name `MBbloom--`, encver 2/4, and field order match RedisBloom-compatible RDB format. | Compatibility commitment | 03, 05, 06 | Source review, GTest RDB tests, RedisBloom fixture/oracle, RDB round-trip. | VERIFY_LATER |
| DC-004 | DESIGN.md:26 | DUMP/RESTORE are bidirectionally compatible. | Compatibility commitment | 05, 06 | Redis DUMP/RESTORE cross-implementation matrix. | VERIFY_LATER |
| DC-005 | DESIGN.md:27 | MIGRATE is bidirectionally compatible and preserves TTL. | Compatibility commitment | 06 | Redis MIGRATE test with TTL checks. | VERIFY_LATER |
| DC-006 | DESIGN.md:28 | psync/fullsync replication is bidirectionally compatible through RDB snapshot. | Compatibility commitment | 06, 09 | Master/replica fullsync test and membership checks. | VERIFY_LATER |
| DC-007 | DESIGN.md:29, 453-466 | RDB-preamble AOF is compatible in Redis 6/7 default mode. | Compatibility commitment | 06 | AOF preamble round-trip and cross-load evidence. | VERIFY_LATER |
| DC-008 | DESIGN.md:30, 164-177, 484, 526-536, 686 | SCANDUMP/LOADCHUNK do not interoperate with RedisBloom; protocol is private layer-index cursor. | Design intended difference | 04, 05, 06, 11, 12 | Runtime gemini protocol tests; RedisBloom mismatch classified as intended. | DESIGN_INTENDED |
| DC-009 | DESIGN.md:31, 451-472, 688 | command-AOF without RDB preamble is gemini-only and not cross-implementation compatible. | Design intended difference | 06, 11, 12 | AOF rewrite mode check and final report limit wording. | DESIGN_INTENDED |
| DC-010 | DESIGN.md:32 | RESP3 is unsupported; commands use RESP2 format. | Design intended difference | 04, 11, 12 | Runtime RESP2 evidence; final report avoids RESP3 support claim. | DESIGN_INTENDED |
| DC-011 | DESIGN.md:33, 303 | BF.DEBUG is unsupported. | Design intended difference | 04, 11, 12 | Command negative test or command inventory; final report boundary. | DESIGN_INTENDED |
| DC-012 | DESIGN.md:43 | RedisBloom compatibility claim is scoped to Redis 6.2.17 + RedisBloom v2.4.20 and 9 corpora. | Version boundary | 05, 11, 12 | RedisBloom version capture, corpus list, confidence downgrade if unavailable. | VERIFY_LATER |
| DC-013 | DESIGN.md:45 | RedisBloom fixture files and expected results are stored in `tests/compat/redisbloom-2.4.20/`. | Test evidence claim | 01, 02, 05 | Path/file inventory; fixture loading. | DOC_RISK: `GBV6-00-001` |
| DC-014 | DESIGN.md:45 | Compat corpus round-trip is not yet a CI blocking gate. | CI/test claim | 01, 02 | CI inventory and workflow review. | VERIFY_LATER |
| DC-015 | DESIGN.md:68-81 | Scalable insertion/lookup/growth behavior follows layered Bloom Filter semantics. | Algorithm contract | 03, 04, 10 | Source review, unit/runtime expansion tests, false-negative checks. | VERIFY_LATER |
| DC-016 | DESIGN.md:82-85 | Growth stops for FixedSize, fpRate below `1e-15`, or uint64 capacity overflow. | Algorithm/resource contract | 03, 07, 10 | Source review and boundary tests. | VERIFY_LATER |
| DC-017 | DESIGN.md:88-94 | Bloom parameter formulas and NoRound differences are as documented. | Algorithm contract | 02, 03, 05 | Unit tests and RedisBloom size/corpus comparison. | VERIFY_LATER |
| DC-018 | DESIGN.md:98-116 | MurmurHash2/64A seeds and h2 derivation match RedisBloom and golden vectors. | Persistence compatibility | 02, 03, 05 | Hash golden test output and source review. | VERIFY_LATER |
| DC-019 | DESIGN.md:122-131, 356-367 | RAII `BloomLayer` and dynamic-array `ScalingBloomFilter` ownership are safe. | Memory safety | 03, 08 | Static review, ASAN/UBSAN, move/lifetime tests. | VERIFY_LATER |
| DC-020 | DESIGN.md:135-143 | Default NoRound/Use64Bit alignment matches RedisBloom. | RDB compatibility | 02, 03, 05 | Unit tests, corpus comparison. | VERIFY_LATER |
| DC-021 | DESIGN.md:146-153 | Tightening ratio is 0.5 and fixed/scaling fpRate behavior matches RedisBloom. | Algorithm compatibility | 03, 05, 10 | Source review and parameter matrix. | VERIFY_LATER |
| DC-022 | DESIGN.md:156-162, 692 | BF.INFO Size accounting differs by design. | Design intended difference | 04, 05, 10, 11 | Runtime BF.INFO evidence and final report wording. | DESIGN_INTENDED |
| DC-023 | DESIGN.md:181-189 | Parser rejects unknown/repeated options and conflicting options more strictly than RedisBloom. | Design intended difference | 04, 05, 07 | Runtime command matrix; RedisBloom diff classification. | DESIGN_INTENDED |
| DC-024 | DESIGN.md:193-201, 519-524 | LOADCHUNK loading-state protection rejects completed-key overwrite and loading-key reads/writes. | Safety contract | 04, 06, 07 | Runtime LOADCHUNK tests and adversarial payload tests. | VERIFY_LATER |
| DC-025 | DESIGN.md:205-211, 285-291 | MADD/INSERT partial failure truncates array at first full error and keeps prior successes. | Command compatibility | 04, 05 | Runtime command matrix against RedisBloom v2.4.20. | VERIFY_LATER |
| DC-026 | DESIGN.md:228-241 | Deserialization is stricter than RedisBloom for malformed edge cases. | Security/design difference | 03, 06, 07 | RDB/wire malicious metadata tests. | VERIFY_LATER |
| DC-027 | DESIGN.md:258-267 | Ten listed BF commands are registered with documented flags. | Runtime command contract | 04, 09 | COMMAND INFO/GETKEYS/ACL and module command behavior. | VERIFY_LATER |
| DC-028 | DESIGN.md:271-278, 552-559 | capacity/error/expansion/data-size/layer/bitsPerEntry limits are enforced. | Resource safety | 02, 03, 04, 07, 10 | Unit/runtime boundary tests, static allocation review. | VERIFY_LATER |
| DC-029 | DESIGN.md:280-283 | Duplicate/conflicting parser options are rejected before key operations. | Command safety | 04, 07 | Runtime wrong-key/type preservation tests. | VERIFY_LATER |
| DC-030 | DESIGN.md:297-307 | Listed RedisBloom command semantic differences are intentional. | Design intended difference | 04, 05, 11 | Differential command matrix and final report wording. | DESIGN_INTENDED |
| DC-031 | DESIGN.md:320-344 | Source and test files are structured as documented. | Repo claim | 01, 03 | File inventory and build target review. | PARTIAL: inventory collected |
| DC-032 | DESIGN.md:343-350 | Core layers do not depend on `redismodule.h`; allocation abstraction isolates tests. | Architecture/testability | 03, 08 | Include graph/static review and build behavior. | VERIFY_LATER |
| DC-033 | DESIGN.md:372-382 | BloomFlags include runtime-only Loading stripped from persistence. | Persistence safety | 03, 06, 07 | Source review, RDB/wire serialization tests. | VERIFY_LATER |
| DC-034 | DESIGN.md:386-394 | Memory allocation uses RedisModule allocation in production and malloc in tests. | Memory/runtime contract | 02, 03, 08 | Build definitions and sanitizer evidence. | VERIFY_LATER |
| DC-035 | DESIGN.md:424-449 | `ValidateLayerFields()` and filter-level validations reject malformed metadata. | Security contract | 02, 03, 06, 07, 08 | GTest malicious metadata and static review. | VERIFY_LATER |
| DC-036 | DESIGN.md:470-472, 698 | AOF rewrite header allocation failure can skip key and logs IO error. | Known risk | 03, 06, 07, 10, 11 | Source review and possibly fault injection; final report risk disclosure. | VERIFY_LATER |
| DC-037 | DESIGN.md:561-568 | RDB and LOADCHUNK payloads are non-trusted inputs with overflow and length checks. | Security contract | 03, 07, 08 | Fuzz/fault/sanitizer evidence. | VERIFY_LATER |
| DC-038 | DESIGN.md:572 | Write commands are `deny-oom`. | Redis command contract | 04, 09 | COMMAND INFO and OOM behavior where feasible. | VERIFY_LATER |
| DC-039 | DESIGN.md:584-588 | Unit/integration test counts are 28/21/65/150. | Test claim | 02 | Build/test runner output and/or source test count audit. | VERIFY_LATER |
| DC-040 | DESIGN.md:592-596 | GTest is Redis-decoupled via `REDIS_BLOOM_TESTING`; TCL uses isolated Redis server. | Test architecture | 02 | Build logs and TCL process evidence. | VERIFY_LATER |
| DC-041 | DESIGN.md:602-608 | BloomLayer tests cover hashes, lifecycle, FP rate, parameter rejection, flags, helpers. | Test coverage claim | 02, 08 | GTest output and source coverage matrix. | VERIFY_LATER |
| DC-042 | DESIGN.md:612-616 | Scaling tests cover expansion, fixed size, RDB shell, move-vs-realloc UB regression, extreme parameters. | Test coverage claim | 02, 08 | GTest output and source coverage matrix. | VERIFY_LATER |
| DC-043 | DESIGN.md:620-623 | RDB tests cover round-trip, malicious metadata, narrowing attacks, wire header safety. | Test coverage claim | 02, 07, 08 | GTest output and source coverage matrix. | VERIFY_LATER |
| DC-044 | DESIGN.md:629-640 | TCL tests cover runtime commands, errors, type safety, resources, LOADCHUNK safety, persistence, config, metadata, expected gaps. | Test coverage claim | 02, 04, 06, 09 | TCL output and coverage matrix. | VERIFY_LATER |
| DC-045 | DESIGN.md:646-653 | Documented build/test commands work. | Build claim | 02 | CMake and TCL command logs. | VERIFY_LATER |
| DC-046 | DESIGN.md:664-670 | Module load args defaults/ranges and unknown-arg rejection work. | Runtime config | 04, 07 | Module load tests. | VERIFY_LATER |
| DC-047 | DESIGN.md:676-680 | Config differences versus RedisBloom are intentional. | Design intended difference | 04, 05, 11 | Differential config tests and final report wording. | DESIGN_INTENDED |
| DC-048 | DESIGN.md:690 | Live command replication may differ in BF.CARD but membership should not; fullsync is compatible route. | Known limit | 09, 11 | Replica command/fullsync tests and final report disclosure. | VERIFY_LATER |
| DC-049 | DESIGN.md:694 | Deletion unsupported because Bloom Filter does not support deletes. | Design intended difference | 04, 11 | Command inventory/final report. | DESIGN_INTENDED |
| DC-050 | DESIGN.md:696 | EXPANSION 1 can create more layers and slower queries. | Performance risk | 10, 11 | Perf/resource test and final report disclosure. | VERIFY_LATER |
| DC-051 | DESIGN.md:700 | gemini-bloom is mutually exclusive with RedisBloom/Redis 8 Bloom in same instance. | Operational limit | 09, 11 | Module load conflict evidence or documented NOT_VERIFIED. | VERIFY_LATER |
| DC-052 | modules/gemini-bloom/src/sb_chain.h:88-91 vs DESIGN.md:166, 484, 528-536 | Source comment claims SCANDUMP/LOADCHUNK wire format matches RedisBloom for cross-implementation compatibility. | Documentation/source consistency | 03, 05, 11 | Source review and final report/fix recommendation. | DOC_RISK: `GBV6-00-002` |

## Stage-to-Claim Routing

| Stage | Primary claim groups |
|---|---|
| 01 | Branch/repo baseline, file inventory, CI/test asset presence (`DC-013`, `DC-014`, `DC-031`). |
| 02 | CMake, GTest, TCL existing tests, test counts and coverage claims (`DC-039`-`DC-045`). |
| 03 | Source static audit: algorithms, memory ownership, parser/resource checks, RDB/wire validation (`DC-015`-`DC-038`, `DC-052`). |
| 04 | Runtime command semantics and parser behavior (`DC-023`-`DC-030`, `DC-038`, `DC-046`). |
| 05 | RedisBloom v2.4.20 oracle comparison and intended differences (`DC-003`, `DC-004`, `DC-008`, `DC-012`, `DC-023`, `DC-030`). |
| 06 | Persistence/transport: RDB, DUMP/RESTORE, MIGRATE, fullsync, AOF, SCANDUMP/LOADCHUNK (`DC-003`-`DC-009`, `DC-024`, `DC-036`). |
| 07 | Malicious input/fuzz/fault safety and resource boundaries (`DC-026`, `DC-028`, `DC-029`, `DC-035`-`DC-037`). |
| 08 | Sanitizer/UB/memory safety (`DC-019`, `DC-034`, `DC-035`, `DC-037`). |
| 09 | Replica, cluster, ACL, COMMAND metadata, operational limits (`DC-006`, `DC-027`, `DC-038`, `DC-048`, `DC-051`). |
| 10 | Performance and resource exhaustion (`DC-016`, `DC-028`, `DC-036`, `DC-050`). |
| 11 | Chinese final report synthesis; all DESIGN_INTENDED and NOT_VERIFIED boundaries. |
| 12 | Final report audit for evidence paths, overclaiming, and severity correctness. |
