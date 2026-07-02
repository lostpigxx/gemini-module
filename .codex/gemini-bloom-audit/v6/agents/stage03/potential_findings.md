# Stage 03 Potential Findings

## New findings

### GBV6-03-001 — RDB/wire deserialization does not enforce DESIGN's per-layer 2GB data-size cap

- Severity: `P2`
- Status: `OPEN`
- Classification: `FAIL / STATIC_DESIGN_VIOLATION`
- Area: RDB/wire deserialization, malicious input, resource limits.
- Evidence:
  - DESIGN states single-layer max data size is `2 GB`: `modules/gemini-bloom/DESIGN.md:552-559`.
  - Runtime `BloomLayer::Create` enforces a local 2GB per-layer cap: `modules/gemini-bloom/src/bloom_filter.cc:130-131`.
  - RDB/wire shared validator checks many fields but has no per-layer cap check: `modules/gemini-bloom/src/bloom_rdb.cc:53-68`.
  - RDB load only checks total data-size <=4GB before accepting loaded layers: `modules/gemini-bloom/src/bloom_rdb.cc:203-209`.
  - Wire header load only checks total data-size <=4GB before allocating layer bit arrays: `modules/gemini-bloom/src/bloom_rdb.cc:295-317`.
  - `BloomLayer::FromWireMeta` allocates `meta.dataSize` directly after caller validation: `modules/gemini-bloom/src/bloom_rdb.cc:135-147`.
  - Tests cover wire total data-size >4GB but not one layer >2GB while total <=4GB: `modules/gemini-bloom/tests/bloom_rdb_test.cc:1306-1326`.
- Impact:
  - A crafted RDB or `BF.LOADCHUNK` header with a single layer between 2GB and 4GB can pass static validation where DESIGN says per-layer data size is capped at 2GB.
  - Runtime feasibility depends on Redis/protocol memory limits and host memory, so Stage 03 does not claim a demonstrated crash. It is still a resource-limit and untrusted-input boundary violation.
- Suggested fix direction:
  - Promote a shared `kMaxLayerDataSize` constant to a header or validation helper.
  - Add `f.dataSize <= kMaxLayerDataSize` to `ValidateLayerFields`.
  - Add RDB and wire unit tests for data-size just over 2GB with total <=4GB, using metadata-only wire tests where possible to avoid allocating huge buffers.

### GBV6-03-002 — RDB/wire deserialization accepts expansionFactor values above `kMaxExpansion`

- Severity: `P2`
- Status: `OPEN`
- Classification: `FAIL / STATIC_DESIGN_VIOLATION`
- Area: RDB/wire deserialization, resource amplification, malicious input.
- Evidence:
  - DESIGN states max expansion is `32768`: `modules/gemini-bloom/DESIGN.md:552-559`.
  - Command layer rejects expansion above `kMaxExpansion`: `modules/gemini-bloom/src/bloom_commands.cc:133-137`, `modules/gemini-bloom/src/bloom_commands.cc:300-304`.
  - Config layer rejects expansion above `kMaxExpansion`: `modules/gemini-bloom/src/bloom_config.cc:44-50`.
  - RDB load only rejects `rawExpansion > UINT_MAX` and scalable expansion zero; it does not reject `rawExpansion > kMaxExpansion`: `modules/gemini-bloom/src/bloom_rdb.cc:180-189`.
  - Wire load rejects scalable expansion zero but does not reject `hdr->expansionFactor > kMaxExpansion`: `modules/gemini-bloom/src/bloom_rdb.cc:281-305`.
  - Future growth multiplies prior capacity by `expansionFactor_`: `modules/gemini-bloom/src/sb_chain.cc:111-116`.
  - RDB/wire tests cover expansion zero only: `modules/gemini-bloom/tests/bloom_rdb_test.cc:1054-1074`, `modules/gemini-bloom/tests/bloom_rdb_test.cc:1251-1285`.
  - TCL tests cover command expansion overflow but not RDB/wire headers: `modules/gemini-bloom/tests/tcl/bloom_test.tcl:1015-1023`, `modules/gemini-bloom/tests/tcl/bloom_test.tcl:1226-1246`.
- Impact:
  - A crafted RDB or `BF.LOADCHUNK` header can import a scalable filter with an expansion factor outside the advertised resource boundary.
  - On later inserts, growth may request unexpectedly large layers. Overflow is guarded, but the accepted state still violates the DESIGN resource contract.
- Suggested fix direction:
  - During RDB and wire header validation, reject `expansionFactor > kMaxExpansion`.
  - Preserve existing fixed-size `expansionFactor == 0` compatibility if required by DESIGN, but still reject values above the maximum unless a narrower compatibility reason is documented.
  - Add RDB and wire tests for `32769`, `UINT32_MAX`, and fixed-size edge cases.

### GBV6-03-003 — TCL per-layer data-size cap test name/comment do not match the assertion

- Severity: `P3`
- Status: `OPEN`
- Classification: `FAIL / TEST_COVERAGE`
- Area: Test quality, evidence clarity.
- Evidence:
  - The TCL section is named `Per-layer data size cap (SAFE-06)` and the test is named `BloomLayer::Create rejects extremely large capacity`: `modules/gemini-bloom/tests/tcl/bloom_test.tcl:1583-1585`.
  - The comments claim a `512MB` per-layer cap, while DESIGN/code use `2GB`: `modules/gemini-bloom/tests/tcl/bloom_test.tcl:1586-1589`, `modules/gemini-bloom/DESIGN.md:552-559`, `modules/gemini-bloom/src/bloom_filter.cc:130-131`.
  - The actual assertion verifies that `BF.RESERVE` at `kMaxCapacity` succeeds, not that an excessive layer is rejected: `modules/gemini-bloom/tests/tcl/bloom_test.tcl:1590-1593`.
- Impact:
  - The test name/comment overstate coverage of per-layer cap rejection.
  - This contributed to Stage 03 finding `GBV6-03-001` remaining untested for RDB/wire input paths.
- Suggested fix direction:
  - Rename the TCL test to state that max command capacity succeeds, or add a separate true rejection test.
  - Update the stale `512MB` comment to `2GB` if the current design remains unchanged.
  - Add metadata-level RDB/wire tests for the per-layer cap without allocating multi-GB blobs.

## Inherited findings confirmed by Stage 03

### GBV6-00-001 — DESIGN.md claims RedisBloom v2.4.20 compat fixtures exist, but path is absent

- Stage 03 classification: still `OPEN`.
- Stage 03 note: source/test inspection did not find `modules/gemini-bloom/tests/compat/redisbloom-2.4.20/`. Runtime RedisBloom oracle remains Stage 05 scope.

### GBV6-00-002 — `sb_chain.h` SCANDUMP/LOADCHUNK comment contradicts DESIGN.md private protocol boundary

- Stage 03 classification: still `OPEN`.
- Stage 03 evidence: `modules/gemini-bloom/src/sb_chain.h:88-91` still says the wire-format structures match RedisBloom's binary protocol for cross-implementation compatibility, while DESIGN states SCANDUMP/LOADCHUNK are private and non-interoperable at `modules/gemini-bloom/DESIGN.md:686-688`.

### GBV6-02-001 — CMake `bloom_test` target fails to execute on macOS because GTest dylib RPATH is missing

- Stage 03 classification: still `OPEN`.
- Stage 03 note: CMake static inspection shows the `bloom_test` aggregate target at `modules/gemini-bloom/CMakeLists.txt:80-85`; Stage 02 runtime evidence remains authoritative for the RPATH failure.

### GBV6-02-002 — TCL expected gaps are counted as failures and make documented command exit nonzero

- Stage 03 classification: still `OPEN`.
- Stage 03 note: TCL expected gap tests are visible, including RedisBloom byte-offset SCANDUMP compatibility at `modules/gemini-bloom/tests/tcl/bloom_test.tcl:763-781`. Stage 02 runtime evidence remains authoritative for nonzero exit behavior.

## Non-findings / deferred items

| Item | Classification | Reason |
|---|---|---|
| RESP3 unsupported | `DESIGN_INTENDED` | Explicitly outside DESIGN scope. Runtime behavior already treated as expected gap in Stage 02. |
| RedisBloom SCANDUMP/LOADCHUNK non-interoperability | `DESIGN_INTENDED` | DESIGN explicitly uses private layer-index protocol. |
| command-AOF rewrite non-interoperability when RDB preamble is disabled | `DESIGN_INTENDED / VERIFY_LATER` | DESIGN documents limitation; Stage 06 should runtime-check persistence transport wording and default AOF-preamble path. |
| `BF.INFO key FIELD` scalar response | `DESIGN_INTENDED` | DESIGN explicitly lists scalar-vs-singleton-array difference. |
| Hash policy caps input length at `INT_MAX` | `NOT_VERIFIED / DEFER_STAGE10` | Redis bulk-string practical limits make >2GB command items unlikely; Stage 10 can revisit huge item/resource behavior. |
