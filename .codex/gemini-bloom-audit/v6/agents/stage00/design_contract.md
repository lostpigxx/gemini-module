# Stage 00 Design Contract

Source of truth: `modules/gemini-bloom/DESIGN.md`.

This file is the audit baseline for gemini-bloom v6. Later stages must treat these constraints as the first judgment layer, then continue auditing implementation, test, runtime, persistence, resource, and report risks that DESIGN.md does not cover.

## 1. Product Scope

| Contract | Classification | Source |
|---|---|---|
| gemini-bloom is a C++20 Redis Module implementing Scalable Bloom Filter and mainstream `BF.*` commands. | DESIGN_COMMITMENT | DESIGN.md:5, 11-14 |
| gemini-bloom is an independent clean-room implementation, not a RedisBloom source port. | DESIGN_COMMITMENT | DESIGN.md:16-18 |
| gemini-bloom is not a RedisBloom drop-in replacement and not a complete RedisBloom protocol compatibility layer. | DESIGN_INTENDED_LIMIT | DESIGN.md:9, 18-19 |
| RedisBloom compatibility is centered on RDB/native Redis transport paths, not on SCANDUMP/LOADCHUNK protocol compatibility. | DESIGN_INTENDED_LIMIT | DESIGN.md:19, 30, 478-484 |

Audit rule: later stages must not call unsupported RESP3, BF.DEBUG, RedisBloom SCANDUMP/LOADCHUNK incompatibility, or non-RDB-preamble command-AOF incompatibility a bug by itself. They are only bugs if implementation, tests, docs, or final report overstate the supported boundary.

## 2. Compatibility Commitments

| Area | Contract | Required verification |
|---|---|---|
| RDB serialization | Data type name `MBbloom--`; encver 2/4; RedisBloom v2.4.20 bidirectional compatibility claimed. | Stage 05, 06 |
| DUMP / RESTORE | Redis native serialization based on RDB object is claimed bidirectionally compatible. | Stage 05, 06 |
| MIGRATE | Redis native migration is claimed bidirectionally compatible and TTL-preserving. | Stage 06 |
| psync / fullsync replication | RDB snapshot transfer is claimed bidirectionally compatible. | Stage 06, 09 |
| RDB-preamble AOF | Redis 6/7 default `aof-use-rdb-preamble yes` is claimed bidirectionally compatible. | Stage 06 |
| Supported customer migration routes | psync fullsync, SCAN + DUMP/RESTORE, direct RDB load, and MIGRATE. | Stage 06, 09 |
| RedisBloom version boundary | Compatibility baseline is Redis 6.2.17 + RedisBloom v2.4.20 only; no extrapolation to other RedisBloom versions or Redis 8 built-in Bloom. | Stage 05, final report |

Stage 00 status: all above are `DESIGN_CLAIM_REQUIRES_VERIFICATION`. No runtime compatibility test was run in this stage.

## 3. Explicit Design-Intended Differences

| Difference | Expected classification | Source |
|---|---|---|
| BF.SCANDUMP/BF.LOADCHUNK use gemini private layer-index cursor protocol and do not interoperate with RedisBloom byte-offset chunks. | DESIGN_INTENDED | DESIGN.md:30, 164-177, 474-538, 686 |
| command-AOF rewrite emits gemini-private `BF.LOADCHUNK` commands and is not cross-implementation compatible when RDB preamble is disabled. | DESIGN_INTENDED | DESIGN.md:31, 451-472, 688 |
| RESP3 is not supported; commands use RESP2 return formats. | DESIGN_INTENDED | DESIGN.md:32 |
| BF.DEBUG is unsupported. | DESIGN_INTENDED | DESIGN.md:33, 303 |
| Parser is stricter than RedisBloom for unknown/repeated options and some conflicting option combinations. | DESIGN_INTENDED | DESIGN.md:181-189, 293-307 |
| BF.INFO single-field return shape differs from RedisBloom v2.4.20. | DESIGN_INTENDED | DESIGN.md:297 |
| BF.INFO Size uses gemini's RAII/dynamic-array accounting and can be larger than RedisBloom. | DESIGN_INTENDED | DESIGN.md:156-162, 298, 692 |
| BF.INSERT EXPANSION 0 maps to NONSCALING although RedisBloom v2.4.20 rejects it. | DESIGN_INTENDED | DESIGN.md:186, 299, 680 |
| LOADCHUNK cursor>1 refuses completed Bloom keys due to loading-state protection; RedisBloom allows overwrite. | DESIGN_INTENDED | DESIGN.md:193-201, 306, 519-524 |
| Module name/version and COMMAND flags differ in listed ways. | DESIGN_INTENDED | DESIGN.md:304-305 |
| RedisBloom / Redis 8 Bloom cannot co-exist in the same Redis instance with gemini-bloom due to command/type registration conflicts. | DESIGN_INTENDED_LIMIT | DESIGN.md:700 |

Audit rule: these rows should appear in the final report as design boundaries, not as defects, unless implementation or user-facing docs contradict them.

## 4. Algorithm and Data Structure Contract

| Area | Contract | Stage mapping |
|---|---|---|
| Scalable Bloom Filter behavior | Lookup checks newest to oldest layers; insert avoids duplicate count; new layer is created when top layer is full. | Stage 03, 04, 10 |
| Growth termination | FixedSize does not grow; growth stops when next fpRate < `1e-15` or capacity would overflow `uint64_t`. | Stage 03, 07, 10 |
| Parameter formula | bitsPerEntry, hashCount, and totalBits use standard Bloom formulas; NoRound 64-bit alignment must match RedisBloom for RDB compatibility. | Stage 03, 05, 06 |
| Hashing | MurmurHash2 32-bit seed `0x9747b28c`; MurmurHash64A seed `0xc6a4a7935bd1e995`; h2 computed from h1 seed; golden vectors protect persistence compatibility. | Stage 02, 03, 05 |
| Structure ownership | `BloomLayer` uses RAII; `ScalingBloomFilter` uses placement-new dynamic array with move migration. | Stage 03, 08 |
| Flags | `NoRound`, `RawBits`, `Use64Bit`, `FixedSize`, `Loading`; Loading is runtime-only and must not persist. | Stage 03, 06, 07 |

## 5. Command Contract

Supported command set from DESIGN.md: `BF.RESERVE`, `BF.ADD`, `BF.MADD`, `BF.INSERT`, `BF.EXISTS`, `BF.MEXISTS`, `BF.INFO`, `BF.CARD`, `BF.SCANDUMP`, `BF.LOADCHUNK`.

| Command family | Contract | Stage mapping |
|---|---|---|
| Write commands | `BF.RESERVE`, `BF.ADD`, `BF.MADD`, `BF.INSERT`, `BF.LOADCHUNK` are `write deny-oom`. | Stage 04, 09 |
| Read commands | `BF.EXISTS`, `BF.MEXISTS`, `BF.INFO`, `BF.CARD` are `readonly`; `BF.SCANDUMP` is `readonly fast`. | Stage 04, 09 |
| Auto-create | `BF.ADD` auto-creates missing keys using configured defaults. | Stage 04 |
| Batch partial failure | Fixed-size `MADD`/`INSERT` stops at first full error; array is truncated to processed elements and successful inserts remain. | Stage 04, 05 |
| Type safety | Non-Bloom keys should produce WRONGTYPE for Bloom operations where applicable. | Stage 04, 07 |
| Parser rules | Reject duplicate options; reject `NONSCALING` with `EXPANSION > 0`; reject `NOCREATE` with `CAPACITY`/`ERROR`. | Stage 04, 07 |

## 6. Parameter and Resource Limits

| Limit | Value | Required verification |
|---|---:|---|
| capacity | `1 .. 2^30` | Stage 02, 04, 07, 10 |
| error_rate | finite `(0.0, 1.0)` | Stage 02, 04, 07 |
| expansion | `0 .. 32768`; command `0` means NONSCALING; config `0` rejected | Stage 04, 07, 10 |
| per-layer data size | `<= 2 GB` | Stage 03, 07, 10 |
| total data size | `<= 4 GB` | Stage 03, 06, 07, 10 |
| max layers | `<= 1024` for RDB/wire deserialization | Stage 06, 07 |
| bitsPerEntry | `<= 1000` | Stage 02, 03, 06, 07 |

Resource exhaustion, OOM behavior, and adversarial input must still be audited even where DESIGN.md gives a limit.

## 7. RDB and Wire Contract

| Area | Contract | Source | Stage mapping |
|---|---|---|---|
| Data type | `MBbloom--` | DESIGN.md:398 | Stage 05, 06 |
| Encoding versions | encver 2 and 4 supported; encver 4 includes expansion factor. | DESIGN.md:400-402 | Stage 02, 06 |
| Filter-level field order | `totalItems`, `numLayers`, `flags`, `expansionFactor`. | DESIGN.md:407-411 | Stage 03, 05, 06 |
| Per-layer field order | `capacity`, `fpRate`, `hashCount`, `bitsPerEntry`, `totalBits`, `log2Bits`, `bitArray`, `itemCount`. | DESIGN.md:413-421 | Stage 03, 05, 06 |
| Validation | `ValidateLayerFields()` checks zero/finite/hash/dataSize/log2 constraints; filter-level checks layers, flags, expansion, item sums, data-size sum. | DESIGN.md:424-436, 552-568 | Stage 03, 06, 07, 08 |
| Narrowing casts | uint64 RDB fields must be range-checked before narrowing. | DESIGN.md:565, 621-622 | Stage 03, 07, 08 |
| Non-trusted inputs | RDB files and LOADCHUNK payloads are untrusted inputs. | DESIGN.md:561-568 | Stage 07, 08 |

## 8. Persistence, Transport, and AOF Contract

| Area | Contract | Stage mapping |
|---|---|---|
| RDB path | RDB/native Redis paths must use `RdbSaveBloom`/`RdbLoadBloom` and be compatible with RedisBloom v2.4.20 within the declared corpus/version scope. | Stage 05, 06 |
| SCANDUMP/LOADCHUNK | User-side export/import and command-AOF rewrite use gemini-private header and layer chunks. | Stage 04, 06, 07 |
| Loading protection | Loading keys reject normal read/write/scandump until all chunks load; completed keys cannot be overwritten by cursor>1 chunks. | Stage 04, 06, 07 |
| AOF default mode | `aof-use-rdb-preamble yes` is the production-required/default compatible path. | Stage 06 |
| AOF non-preamble mode | Non-RDB-preamble command-AOF is gemini-only and not cross-compatible. | Stage 06, final report |
| AOF rewrite OOM | Header allocation failure logs warning and skips the key; Redis may abort rewrite but module does not assume that. | Stage 03, 06, 07, 10 |

## 9. Test and Documentation Claims

DESIGN.md claims these test assets and counts:

- `bloom_filter_test`: 28 cases.
- `sb_chain_test`: 21 cases.
- `bloom_rdb_test`: 65 cases.
- `bloom_test.tcl`: 150 cases.
- Compat corpus at `tests/compat/redisbloom-2.4.20/`.
- RedisBloom v2.4.20 matrix over 9 corpora and multiple transport paths.
- TCL coverage for default RDB-preamble AOF, and no CI coverage for non-RDB-preamble AOF rewrite.
- Expected gaps for RESP3 and RedisBloom SCANDUMP byte-offset cursor.

Stage 00 static check found:

- `modules/gemini-bloom/tests/compat/redisbloom-2.4.20/` is missing in the audited tree. This is `GBV6-00-001`.
- `.github` is absent, so Stage 00 found no GitHub workflow CI gate. Whether another CI system exists is `NOT_VERIFIED` until Stage 01/02.
- `modules/gemini-bloom/src/sb_chain.h` comments claim SCANDUMP/LOADCHUNK wire structures match RedisBloom for cross-implementation compatibility, contradicting DESIGN.md's private non-interoperable boundary. This is `GBV6-00-002`.

## 10. Known Limits That Must Be Reflected in Final Report

1. SCANDUMP/LOADCHUNK are not RedisBloom-interoperable.
2. command-AOF without RDB preamble is not cross-implementation compatible.
3. Live command replication can produce BF.CARD differences in high false-positive/EXPANSION 1 cases; fullsync RDB is the compatible route.
4. BF.INFO Size uses different accounting.
5. Bloom Filter deletion is unsupported.
6. EXPANSION 1 can increase query cost due to many layers.
7. AOF rewrite OOM can skip keys in the module callback path.
8. gemini-bloom cannot co-exist in one Redis instance with RedisBloom or Redis 8 built-in Bloom.

## 11. Design Gaps and Non-DESIGN Risks for Later Stages

DESIGN.md does not eliminate the need to audit:

- UB and memory safety under sanitizer or equivalent evidence.
- Integer overflow and allocation-size safety under adversarial metadata.
- Runtime command behavior under actual Redis server versions.
- RedisBloom oracle comparisons with raw/normalized replies.
- RDB, DUMP/RESTORE, MIGRATE, fullsync, AOF, SCANDUMP/LOADCHUNK behavior.
- Replica, cluster, ACL, COMMAND INFO/GETKEYS, and operational metadata.
- Performance and resource exhaustion behavior at declared limits.
- Test-count truth, CI coverage truth, and fixture availability.
- Final report evidence completeness and confidence downgrades for `BLOCKED` or `NOT_VERIFIED` items.
