# Stage 10 Planner Output — PERF_RESOURCE

Planner role: plan only. No tests were run and no production code was edited by this planner.

## 1. Stage Objective

Stage 10 audits performance and resource behavior as audit signals, not as a formal benchmark. The main agent should verify that gemini-bloom's runtime behavior, memory accounting, persistence size, SCANDUMP/LOADCHUNK chunking, and extreme-parameter handling stay within `DESIGN.md` boundaries, and should explicitly downgrade anything not run to `NOT_VERIFIED` or `BLOCKED`.

This stage must not claim production performance characteristics from small local samples. It should record latency/RSS/file-size observations only as bounded evidence for this audit environment.

## 2. DESIGN Constraints And Boundaries Relevant To Stage 10

Highest-priority source: `modules/gemini-bloom/DESIGN.md`.

Design constraints to verify or preserve:

- `capacity` valid range is `1 .. 2^30`.
- `error_rate` valid range is finite `(0.0, 1.0)`.
- `expansion` valid range is `0 .. 32768`; `0` means `NONSCALING` in command behavior.
- Per-layer data size is intended to be `<= 2GB` in `BloomLayer::Create()`.
- Total data size is intended to be `<= 4GB` in runtime `AppendLayer()` and RDB/wire deserialization.
- RDB/wire deserialization has `max layers <= 1024`.
- `bitsPerEntry <= 1000`.
- Write commands are registered with `deny-oom`; Redis may reject writes when already in OOM state.
- `BF.INFO Size` uses gemini's accounting: `sizeof(ScalingBloomFilter)` + reserved layer slots + all bit arrays. It is expected to differ from RedisBloom and may differ from Redis `MEMORY USAGE`.
- SCANDUMP/LOADCHUNK uses gemini's private layer-index cursor protocol:
  - cursor `0` returns `[1, header_blob]`;
  - cursor `1..N` return full per-layer bit arrays with next cursor incrementing by 1;
  - final cursor returns `[0, ""]`.
- SCANDUMP/LOADCHUNK is not RedisBloom-compatible. Any RedisBloom byte-offset or 16MB chunk expectation is a design-intended difference, not a bug.
- Default Redis 6/7 RDB-preamble AOF stores Bloom data via RDB and is the compatible AOF path. Command-AOF rewrite uses private `BF.LOADCHUNK` commands and is same-module only.
- `aof-use-rdb-preamble no` command-AOF is not cross-implementation compatible by design.
- Stage 07/09 open P1 findings about half-loaded or out-of-order LOADCHUNK (`GBV6-07-001`, `GBV6-07-002`) must be carried forward and not masked by orderly same-module LOADCHUNK success.

Design-intended differences that must not be false failures:

- `BF.INFO Size` can be larger than RedisBloom and need not equal Redis `MEMORY USAGE`.
- SCANDUMP chunks are full layers and can exceed RedisBloom's chunking expectations.
- command-AOF rewrite is not RedisBloom-compatible.
- This stage does not need RESP3 or Redis 8 coverage.

## 3. Files And Static Areas The Main Agent Should Inspect

The main agent should statically inspect these files before or during execution and record notes in evidence:

- `modules/gemini-bloom/src/bloom_commands.cc`: parser limits, `deny-oom` command handling paths, `BF.INFO` fields.
- `modules/gemini-bloom/src/bloom_filter.cc`: capacity/error-rate formula, per-layer allocation cap, bits-per-entry cap.
- `modules/gemini-bloom/src/sb_chain.cc`: expansion behavior, total data-size accounting, `BytesUsed()`, layer array growth.
- `modules/gemini-bloom/src/bloom_rdb.cc`: SCANDUMP/LOADCHUNK wire header, AOF rewrite, RDB save/load size behavior.
- `modules/gemini-bloom/src/redis_bloom_module.cc`: command flags and module type callbacks if memory usage is exposed there.

Static inspection is evidence only if the inspected paths and exact observations are written under `.codex/gemini-bloom-audit/v6/evidence/stage10/`.

## 4. Required Evidence Files

Stage file explicitly requires:

- `.codex/gemini-bloom-audit/v6/evidence/stage10/perf_matrix.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/resource_limits.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/memory_usage.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/latency_samples.csv`

Policy 03 also requires at minimum:

- `.codex/gemini-bloom-audit/v6/evidence/stage10/commands.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/stdout.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/stderr.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/exit_codes.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/env_snapshot.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/evidence_index.md`

Strongly recommended additional files:

- `.codex/gemini-bloom-audit/v6/evidence/stage10/static_resource_audit.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/scandump_loadchunk.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/persistence_size.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/large_items.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/server_logs/`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/blocked_or_not_verified.md`

## 5. Execution Safety Rules

Use isolated Redis instances, random/free local ports, temporary dirs under the evidence tree or `/private/tmp`, explicit timeouts, and cleanup logs. Do not run unbounded insert loops. Do not attempt to fill `capacity=2^30`. Do not SCANDUMP a multi-hundred-MB layer unless the main agent has first recorded an explicit local memory budget and an abort condition.

Capacity `2^30` must be handled safely:

- Runtime acceptance can be tested with a high error rate and `NONSCALING`, for example `BF.RESERVE cap_max_safe 0.99 1073741824 NONSCALING`, because the bit array should be small enough for an audit probe.
- Do not use `BF.RESERVE cap_max_default 0.01 1073741824` as a routine runtime test; that can allocate roughly GB-scale memory and is not needed to prove parser acceptance.
- `capacity=1073741825` should be tested as a rejection case because it should fail before allocation.
- Tiny-error cases that would imply `>2GB` per-layer data should only be run in an isolated process with timeout and resource guard, and expected behavior is graceful rejection without large allocation. If no guard is available, mark that runtime part `NOT_VERIFIED` and rely only on static evidence.

Large item handling:

- Use a binary-safe client or script, not ad hoc shell quoting, for empty, NUL-containing, 10KB, and 1MB items.
- Limit 1MB item operations to a tiny count, for example one `ADD`, one `EXISTS`, one `SCANDUMP` pass on a small-capacity filter.
- Do not append many 1MB commands to AOF; persistence-size tests should keep large-item count low.

Resource exhaustion handling:

- Treat Redis/server OOM, process kill, timeout, or missing resource guard as evidence. Do not retry with larger loads.
- If a command unexpectedly allocates heavily or stalls, stop that scenario, preserve logs, and classify narrowly.

## 6. Exact Scenario Matrix

### 6.1 Resource-Limit Scenarios

Write all raw commands, replies, elapsed time, Redis memory before/after, and exit status to `resource_limits.log`.

| ID | Scenario | Expected Classification |
|---|---|---|
| RL-01 | `BF.RESERVE cap_zero 0.01 0` | `PASS` if rejected as invalid capacity. |
| RL-02 | `BF.RESERVE cap_min 0.01 1` plus `BF.ADD`/`BF.EXISTS` one item | `PASS` if accepted and functional. |
| RL-03 | `BF.RESERVE cap_10 0.01 10`, `cap_100`, `cap_10000`, `cap_100000` | `PASS` if accepted with plausible `BF.INFO` capacity/size and no abnormal RSS spike. |
| RL-04 | `BF.RESERVE cap_max_safe 0.99 1073741824 NONSCALING` | `PASS` if accepted safely; `BLOCKED` if environment cannot allocate the small expected bitset; `NOT_VERIFIED` only if skipped for safety. |
| RL-05 | `BF.RESERVE cap_too_big 0.01 1073741825` | `PASS` if rejected before allocation. |
| RL-06 | `BF.RESERVE exp_1 0.01 10 EXPANSION 1`, `exp_2`, `exp_4`, `exp_32768` | `PASS` if accepted and `BF.INFO Expansion rate` matches. |
| RL-07 | `BF.RESERVE exp_too_big 0.01 10 EXPANSION 32769` | `PASS` if rejected before key creation. |
| RL-08 | `BF.RESERVE exp_zero_reserve 0.01 10 EXPANSION 0` and `BF.INSERT exp_zero_insert EXPANSION 0 ITEMS a` | `PASS` if behavior matches DESIGN: command-level `0` maps to non-scaling where documented. |
| RL-09 | Tiny-error per-layer cap attempt, e.g. capacity high enough with `error_rate` small enough to imply `>2GB` layer | `PASS` only if safely rejected without large allocation; otherwise `NOT_VERIFIED` if not safely runnable. |
| RL-10 | `bitsPerEntry > 1000` attempt using an extremely small finite `error_rate` | `PASS` if rejected gracefully; `BLOCKED` if parser/runtime cannot represent the input in this environment. |
| RL-11 | NONSCALING full: capacity 10, add 12 unique items via `MADD`; repeat with `BF.INSERT ... NONSCALING ITEMS ...` | `PASS` if reply truncates at first full error, `BF.CARD` stays at capacity, later items are not processed. |

### 6.2 Latency-Sample Scenarios

Record samples in `latency_samples.csv`. Recommended columns:

`scenario_id,command,capacity,expansion,item_class,item_size_bytes,iteration,elapsed_ms,reply_class,num_layers,bf_info_size,memory_usage,used_memory,used_memory_rss,notes`

Sample counts should be small and explicit. Suggested count: 30 iterations for normal item commands, 5 iterations for 10KB items, 1-3 iterations for 1MB items, and one pass per SCANDUMP/LOADCHUNK chunk.

| ID | Scenario | Commands |
|---|---|---|
| LAT-01 | Single-layer small filter baseline | `ADD`, `EXISTS`, `MADD`, `MEXISTS`, `INFO`, `CARD` on capacity 100 / expansion 2. |
| LAT-02 | Capacity scale smoke | Same command family on capacity 10, 100, 10000, 100000 with bounded item counts. |
| LAT-03 | Multi-layer query degradation | capacity 10 / expansion 1, insert enough unique items to create many layers, then sample `EXISTS` for newest item, oldest item, and missing item. |
| LAT-04 | Expansion comparison | capacity 10 with expansion 1, 2, 4, 32768; insert a bounded count and record number of filters and latency. |
| LAT-05 | Large item hashing | empty item, small item, binary NUL item, 10KB item, 1MB item with `ADD` and `EXISTS`. |
| LAT-06 | SCANDUMP/LOADCHUNK ordered round-trip | Time each `SCANDUMP` cursor and corresponding `LOADCHUNK` into a new key for a moderate multi-layer filter. |

Pass for latency means samples exist and anomalies are explained. It does not mean the module is benchmarked or production-performance-certified.

### 6.3 Memory-Accounting Scenarios

Record in `memory_usage.md`:

- `BF.INFO key` full output.
- `BF.INFO key Size`.
- `BF.INFO key Number of filters`.
- `BF.INFO key Capacity`, `Expansion rate`, `Number of items`.
- Redis `MEMORY USAGE key`.
- Redis `INFO memory` fields: at least `used_memory`, `used_memory_rss`, `mem_fragmentation_ratio`.
- OS RSS if available through `ps`.
- Notes on allocator/version effects.

| ID | Scenario | Expected Interpretation |
|---|---|---|
| MEM-01 | Empty reserved capacity 100 filter | `BF.INFO Size` includes struct + reserved layer slots + bit array; it need not match `MEMORY USAGE`. |
| MEM-02 | Same filter after inserts below capacity | Size should stay generally stable unless layer count changes. |
| MEM-03 | Multi-layer growth capacity 10 / expansion 2 | Size should increase as layers are appended. |
| MEM-04 | expansion 1 many-layer case | Size and latency may grow with number of filters; this is a known design limitation, not automatically a bug. |
| MEM-05 | capacity 10000 and 100000 | Size/MEMORY/RSS should be monotonic and plausible, not exact-equal. |
| MEM-06 | capacity `2^30` high-error NONSCALING | Verify `BF.INFO Capacity` reports boundary value and memory remains bounded by the high error-rate formula. |

`FAIL` only if accounting is internally inconsistent with DESIGN, such as impossible negative/zero sizes, size decreasing across added layers without explanation, overflow-looking values, or crashes. Differences between `BF.INFO Size` and Redis `MEMORY USAGE` should usually be `DESIGN_INTENDED` or `INFO`.

### 6.4 SCANDUMP/LOADCHUNK Scenarios

Record in `scandump_loadchunk.md` and link summaries from `perf_matrix.md`.

| ID | Scenario | Expected Behavior |
|---|---|---|
| SLC-01 | Single-layer filter, cursor sequence from 0 to finish | Cursors follow `0 -> 1 -> 2 -> 0`; header then one full layer chunk. |
| SLC-02 | Multi-layer filter from capacity 10 / expansion 2 | Cursors increment by layer index; number of data chunks equals `BF.INFO Number of filters`. |
| SLC-03 | expansion 1 many-layer filter | More chunks and slower query/scandump are expected; record number of layers. |
| SLC-04 | Moderate large layer, capacity 100000 | Data chunk length should equal the full layer bit-array size; it should not be split into RedisBloom-style 16MB chunks. |
| SLC-05 | Ordered same-module LOADCHUNK replay into a new key | All inserted sample members remain present, `BF.CARD`/`BF.INFO` are plausible, and copy key is normal after final chunk. |
| SLC-06 | Loading-state safety note | Do not re-open Stage 07 fuzz here unless intentionally scoped. If out-of-order/repeated/half-loaded behavior is observed, classify as carry-forward evidence for `GBV6-07-001` or `GBV6-07-002`. |

Do not test RedisBloom SCANDUMP/LOADCHUNK interoperability as a pass/fail criterion; DESIGN says it is not compatible.

### 6.5 RDB/AOF Size Scenarios

Record in `persistence_size.md`:

- Redis version and module artifact used.
- Key set and item counts.
- RDB filename and byte size.
- AOF configuration and byte size.
- Whether `aof-use-rdb-preamble` is `yes` or `no`.
- Any Redis logs from BGSAVE/BGREWRITEAOF/restart.

| ID | Scenario | Expected Interpretation |
|---|---|---|
| PA-01 | `SAVE`/`BGSAVE` after moderate filters: capacity 100, capacity 10000, multi-layer expansion 1, 10KB item, one 1MB item | File sizes recorded; no formal compression/size claim. |
| PA-02 | AOF with `aof-use-rdb-preamble yes` | AOF stores RDB preamble; same-module restart should preserve membership. Size is evidence for this environment only. |
| PA-03 | Optional command-AOF with `aof-use-rdb-preamble no` | Same-module replay may be tested and size recorded, but cross-implementation incompatibility is `DESIGN_INTENDED`. |
| PA-04 | Large-item persistence sanity | Since Bloom filters store bits, serialized size should depend on filter bit arrays rather than item bytes. A one-item 1MB input should not make RDB/AOF grow by 1MB except for command-AOF live append before rewrite. |

If the Redis version uses multi-part AOF files, record all relevant manifest/base/incremental file sizes rather than assuming a single `appendonly.aof`.

### 6.6 Extreme-Parameter And Resource-Exhaustion Scenarios

These are audit-signal checks, not stress tests:

- `capacity=2^30`, `error_rate=0.99`, `NONSCALING`: safe boundary accept.
- `capacity=2^30 + 1`: reject.
- `expansion=32768`: accept on tiny filter without forcing growth.
- `expansion=32769`: reject.
- `capacity=100000`, `error_rate=0.01`: moderate real allocation and SCANDUMP chunk sample.
- `error_rate` extremely small causing `bitsPerEntry > 1000`: reject.
- high capacity + tiny error causing computed layer size `>2GB`: reject if safely runnable, otherwise static/NOT_VERIFIED.
- NONSCALING full on `MADD` and `BF.INSERT`: truncate at first error and preserve earlier inserts.
- 1MB item `ADD`/`EXISTS`: should complete without storing the item payload in the filter.

## 7. Classification Rules

Use the LOOP_CONTROL status definitions exactly:

- `PASS`: scenario was run or statically verified with persisted evidence and matches DESIGN.
- `FAIL`: scenario reveals a concrete bug, crash, data loss, overflow, unbounded allocation, DESIGN violation, or unsupported behavior promoted beyond DESIGN.
- `BLOCKED`: verification could not proceed due to environment, missing Redis/module artifact, build failure, port/permission issue, resource guard absence when the scenario requires one, or tooling failure. Must include evidence.
- `NOT_VERIFIED`: the scenario was intentionally not covered in Stage 10, was skipped for safety, or lacks enough evidence. Must be listed in `blocked_or_not_verified.md` and final report confidence must be narrowed.
- `DESIGN_INTENDED`: behavior differs from RedisBloom/common expectation but is explicitly allowed by DESIGN, such as private SCANDUMP cursor semantics or BF.INFO Size accounting.

Concrete classification guidance:

- If `capacity=2^30` high-error NONSCALING cannot be run because the environment cannot safely allocate even the expected small bitset, classify `BLOCKED_RESOURCE` with logs.
- If default-error `capacity=2^30` is skipped to avoid GB-scale allocation, classify that specific default-error runtime check as `NOT_VERIFIED`, not FAIL.
- If `SCANDUMP` returns full-layer chunks instead of RedisBloom byte-offset chunks, classify `DESIGN_INTENDED/PASS` for gemini protocol.
- If ordered same-module LOADCHUNK succeeds, do not claim Stage 07 LOADCHUNK findings are resolved.
- If RDB/AOF sizes are collected but restart is not performed, file-size evidence may pass only as size recording; persistence correctness must be `NOT_VERIFIED`.

## 8. False PASS Risks

- Passing `capacity=2^30` with `error_rate=0.99 NONSCALING` proves safe boundary parsing/allocation only for that high-error configuration; it does not prove default-error or tiny-error GB-scale behavior.
- Small latency samples can miss production regressions, concurrency effects, allocator fragmentation, and long-tail behavior.
- Same-module SCANDUMP/LOADCHUNK round-trip does not prove RedisBloom SCANDUMP/LOADCHUNK interoperability.
- `aof-use-rdb-preamble yes` success does not prove command-AOF cross-implementation compatibility.
- Low RSS after small tests does not prove the 4GB total-data cap under all paths.
- A small number of large-item operations does not prove sustained throughput for large payloads.
- Ordered LOADCHUNK success must not be reported as fixing half-loaded/out-of-order integrity bugs from Stage 07.

## 9. False FAIL Risks

- `BF.INFO Size` being larger than RedisBloom, or not equal to Redis `MEMORY USAGE`, is design-intended unless internally inconsistent.
- RSS and `used_memory_rss` vary by allocator, OS, lazy pages, Redis overhead, and fragmentation; they should not be exact-match assertions.
- First-run or BGSAVE/BGREWRITEAOF latency spikes may be environmental.
- Redis `maxmemory`/OOM rejection can reflect server state at command entry, not a module allocation bug.
- SCANDUMP chunk sizes above RedisBloom expectations are not failures because gemini uses full-layer chunks.
- command-AOF private `BF.LOADCHUNK` output is not a failure unless same-module replay fails or DESIGN claims are contradicted.
- Cluster/replica limitations from Stage 09, such as ASK `NOT_VERIFIED`, should not be reclassified in Stage 10 unless this stage directly tests them.

## 10. Pass Criteria

Stage 10 may pass if all of the following are true:

- All required evidence files exist and include concrete commands, outputs, exit codes, environment details, and evidence index entries.
- Resource-limit checks either match DESIGN or are explicitly classified as `BLOCKED`/`NOT_VERIFIED` with evidence.
- Capacity `2^30` is handled safely, with no unbounded allocation attempt and no overbroad claim.
- Latency samples are recorded for the selected matrix and labeled as audit samples, not formal benchmarks.
- Memory-accounting evidence distinguishes `BF.INFO Size`, Redis `MEMORY USAGE`, Redis `INFO memory`, and RSS without requiring false equality.
- SCANDUMP/LOADCHUNK evidence verifies the private layer-index protocol and ordered same-module replay, while preserving Stage 07/09 LOADCHUNK findings.
- RDB/AOF size evidence records both file sizes and relevant `aof-use-rdb-preamble` mode; command-AOF, if tested, is scoped as same-module only.
- Large-item and NONSCALING-full scenarios are covered or explicitly downgraded.
- No production source code is modified and no existing tests are weakened.
- Final Stage 10 result states every scenario as `PASS`, `FAIL`, `BLOCKED`, `NOT_VERIFIED`, or `DESIGN_INTENDED`, and does not overstate small-sample performance.
