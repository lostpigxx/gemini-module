## Stage objective

Stage 04 must run a real Redis runtime command-semantics audit for gemini-bloom, without using RedisBloom as an oracle. The goal is to prove, with raw Redis protocol evidence, whether gemini-bloom's 10 supported `BF.*` commands behave consistently with `modules/gemini-bloom/DESIGN.md` under normal, edge, and error conditions.

This stage should classify outcomes as:

- `PASS` when runtime behavior matches DESIGN.md or a Redis/Redis Module standard expectation.
- `FAIL` when behavior violates DESIGN.md, corrupts data, breaks command semantics, or exposes unsafe runtime behavior.
- `DESIGN_INTENDED` when behavior differs from RedisBloom or RESP3-native expectations but matches DESIGN.md.
- `BLOCKED` only when the local runtime environment prevents verification and the blocker has evidence.
- `NOT_VERIFIED` only for explicitly deferred areas.

## DESIGN.md constraints for this stage

- `modules/gemini-bloom/DESIGN.md` is the controlling standard. RedisBloom compatibility is not the oracle for this stage.
- gemini-bloom is not a full RedisBloom protocol compatibility layer.
- Supported command surface is exactly the 10 listed commands:
  - `BF.RESERVE`
  - `BF.ADD`
  - `BF.MADD`
  - `BF.INSERT`
  - `BF.EXISTS`
  - `BF.MEXISTS`
  - `BF.INFO`
  - `BF.CARD`
  - `BF.SCANDUMP`
  - `BF.LOADCHUNK`
- `RESP3` is explicitly unsupported. Commands are expected to keep RESP2-style return shapes. RESP3 differences must be classified as `DESIGN_INTENDED` if replies are well-formed and consistent with DESIGN.md.
- `BF.DEBUG` is out of scope and unsupported.
- `BF.SCANDUMP` / `BF.LOADCHUNK` use gemini's private layer-index cursor protocol, not RedisBloom's byte-offset cursor protocol.
- `BF.LOADCHUNK` loading state is an intentional integrity guard:
  - partially loaded keys reject normal reads/writes with `ERR filter is being loaded`;
  - completed Bloom keys reject `LOADCHUNK cursor>1`;
  - `cursor=1` against existing Bloom keys returns `ERR received bad data` and preserves the old key.
- Parser strictness is intentional:
  - unknown options rejected;
  - duplicate options rejected;
  - `NOCREATE` with `CAPACITY` or `ERROR` rejected before key lookup;
  - `NONSCALING` and `EXPANSION > 0` are mutually exclusive;
  - `EXPANSION 0` maps to `NONSCALING`.
- `BF.INFO key FIELD` returns a scalar in gemini, not a singleton array. This is `DESIGN_INTENDED`.
- `BF.INFO Size` uses gemini's own memory accounting and must not be compared to RedisBloom.
- `MADD` / `INSERT` partial failure on fixed-size full filters must stop at first full error and return a truncated postponed array containing prior successes plus the first error.
- Resource limits relevant to command semantics:
  - `capacity`: `1 .. 2^30`
  - `error_rate`: finite and `(0.0, 1.0)`
  - `expansion`: `0 .. 32768`, where `0` means non-scaling
  - write commands are expected to be `deny-oom`.
- Stage 03 open findings affect interpretation but should not be re-proven here:
  - `GBV6-03-001`: RDB/wire deserialization lacks DESIGN's per-layer 2GB cap enforcement.
  - `GBV6-03-002`: RDB/wire deserialization accepts expansion values above `kMaxExpansion`.
  - These are static/resource-boundary findings; Stage 04 should avoid huge allocation probes and leave malicious wire-scale validation to later fuzz/safety stages unless a small runtime reproduction naturally appears.

## Suggested runtime harness approach

Use a small stage-local Python or TCL harness under `.codex/gemini-bloom-audit/v6/evidence/stage04/` or `.codex/gemini-bloom-audit/v6/agents/stage04/`.

Recommended harness properties:

- Start one isolated `redis-server` with the built `redis_bloom.so`, on an unused local port, with a temporary dir under `/private/tmp` or `.codex/gemini-bloom-audit/v6/evidence/stage04/`.
- Record server version, module path, module load result, port, Redis protocol mode, and cleanup status.
- Use a raw socket RESP client, not only `redis-cli`, so binary items, empty bulk strings, raw RESP2/RESP3 frames, and per-element errors inside arrays are captured exactly.
- Capture both:
  - raw request/reply protocol frames in `runtime_matrix/raw_resp.log`;
  - normalized semantic results in `runtime_matrix/normalized_results.md`.
- Run `HELLO 2` before RESP2 matrix cases and `HELLO 3` before RESP3 checks.
- Keep each test case isolated by using unique keys or `FLUSHALL` between case groups.
- For every command, capture:
  - command arguments;
  - raw RESP;
  - normalized reply;
  - expected classification;
  - actual classification;
  - evidence path.
- Do not rely on RedisBloom oracle in this stage.
- Existing TCL tests can inform expectations, but Stage 04 should produce its own evidence matrix and raw RESP capture.

## Matrix cases to include

### 1. Command coverage baseline

Include happy-path RESP2 evidence for all 10 commands:

| Command | Minimum happy-path case |
|---|---|
| `BF.RESERVE` | create a new filter with explicit `error_rate`, `capacity`, and optional `EXPANSION` |
| `BF.ADD` | add new item returns `1`; duplicate returns `0` |
| `BF.MADD` | add multiple items, including duplicate behavior |
| `BF.INSERT` | create-with-options and add items after `ITEMS` |
| `BF.EXISTS` | existing item returns `1`; absent item returns `0` |
| `BF.MEXISTS` | mixed present/absent result array |
| `BF.INFO` | full shape and scalar field shapes |
| `BF.CARD` | cardinality after unique and duplicate inserts |
| `BF.SCANDUMP` | cursor sequence from `0` through final `0` |
| `BF.LOADCHUNK` | load a complete dump into a new key and verify membership/cardinality |

### 2. RESP2 raw protocol

For every happy-path command, capture raw RESP2 requests and replies.

Required details:

- Include integer, simple string, bulk string, array, nil/error shapes as applicable.
- Include at least one per-element error array case for `BF.MADD` or `BF.INSERT` partial failure.
- Include binary-safe raw payload capture for empty, NUL-containing, and long items.

### 3. RESP3 behavior

Run a focused subset after `HELLO 3`:

- `BF.ADD`
- `BF.EXISTS`
- `BF.MEXISTS`
- `BF.INFO` full shape
- `BF.INFO key FIELD`
- `BF.SCANDUMP`

Expected interpretation:

- Well-formed replies using RESP2-style shapes are `DESIGN_INTENDED`.
- Protocol corruption, server disconnect, inconsistent state, or command failure not explained by DESIGN.md is `FAIL`.

### 4. Wrong type behavior

Create a string key with `SET wrongtype value`, then run wrong-type checks for all commands where a key is read or mutated:

- `BF.RESERVE wrongtype ...`
- `BF.ADD wrongtype item`
- `BF.MADD wrongtype item`
- `BF.INSERT wrongtype ITEMS item`
- `BF.EXISTS wrongtype item`
- `BF.MEXISTS wrongtype item`
- `BF.INFO wrongtype`
- `BF.CARD wrongtype`
- `BF.SCANDUMP wrongtype 0`
- `BF.LOADCHUNK wrongtype 1 <valid_header_blob>`

Expected: standard `WRONGTYPE` behavior where applicable, and key preservation.

### 5. Missing key behavior

Cover missing-key semantics separately:

- `BF.RESERVE missing ...` creates.
- `BF.ADD missing item` auto-creates.
- `BF.MADD missing a b` auto-creates if implementation/DESIGN expects bulk add create behavior; otherwise classify actual behavior against command contract and existing tests.
- `BF.INSERT missing NOCREATE ITEMS a` returns missing-key error.
- `BF.INSERT missing ITEMS a` creates.
- `BF.EXISTS missing item` returns `0`.
- `BF.MEXISTS missing a b` returns zeros.
- `BF.INFO missing` returns DESIGN-documented `ERR key does not exist`.
- `BF.CARD missing` should be verified and classified from actual runtime/design alignment.
- `BF.SCANDUMP missing 0` should be verified and classified.
- `BF.LOADCHUNK missing 1 <header>` creates loading shell; `cursor>1` on missing key rejects.

### 6. Duplicate item behavior

Verify duplicates do not increase cardinality:

- `BF.ADD key a` then `BF.ADD key a`.
- `BF.MADD key a a b`.
- `BF.INSERT key ITEMS b b c`.
- Check `BF.CARD`.
- Check `BF.INFO ITEMS`.

Expected: first unique insert returns `1`, duplicates return `0`, cardinality counts unique accepted inserts only.

### 7. Binary, empty, and long items

Use raw RESP bulk strings, not shell quoting:

- empty item: `""`
- binary item with embedded NUL: e.g. `b"a\x00b"`
- binary item with non-UTF8 bytes
- long item: at least 10KB, matching DESIGN test coverage
- optional larger-but-safe long item if runtime cost is low

For each, verify:

- `BF.ADD`
- `BF.EXISTS`
- `BF.MADD` or `BF.MEXISTS`
- persistence is not required in Stage 04 unless the main agent chooses a small smoke check; full persistence belongs to Stage 06.

### 8. Capacity, error, and expansion boundaries

Parser and boundary cases:

- `capacity=0` rejected.
- `capacity=1` accepted.
- `capacity=2^30` accepted only if the environment can safely attempt it without dangerous allocation; otherwise document as `NOT_VERIFIED` or run a parser-only equivalent if available.
- `capacity=2^30+1` rejected.
- `error_rate=0`, negative, `1`, greater than `1`, `NaN`, `Inf`, non-numeric rejected.
- small valid finite error rate accepted if it does not create unsafe memory pressure.
- `EXPANSION 0` maps to non-scaling.
- `EXPANSION 1`, `2`, `32768` accepted where safe.
- `EXPANSION 32769` rejected.
- duplicate `EXPANSION`, `ERROR`, `CAPACITY`, and `NONSCALING` rejected.
- unknown option rejected.
- `NONSCALING` plus `EXPANSION > 0` rejected.
- `NOCREATE` plus `CAPACITY` or `ERROR` rejected before key lookup.

Avoid tests that intentionally allocate huge memory. Parser boundaries that may allocate too much should be handled with care and classified honestly.

### 9. NONSCALING full behavior

Create a small fixed-size filter:

- `BF.RESERVE fixed 0.01 2 NONSCALING`
- fill with two unique items;
- verify third unique insert via `BF.ADD` returns full error;
- duplicate insert after full returns duplicate/not-added behavior without consuming capacity;
- `BF.CARD` remains stable;
- `BF.INFO` indicates fixed/non-scaling shape via expansion/capacity metadata where exposed.

### 10. MADD / INSERT partial failure

For fixed-size capacity `2`:

- `BF.MADD fixed a b c d`
  - expected array length `3`: success, success, first error.
  - subsequent items are not processed.
- `BF.INSERT fixed ITEMS a b c d`
  - same truncation semantics.
- Verify successful prior inserts remain and are replicated into state:
  - `BF.CARD`
  - `BF.EXISTS` for processed and unprocessed items.
- Capture raw RESP because per-element error arrays are easy to normalize incorrectly.

### 11. BF.INFO field shapes and full shape

Full shape:

- Capture full `BF.INFO key`.
- Verify field names and order if DESIGN/tests imply stable order.
- Verify values for:
  - capacity
  - size
  - filters/layers
  - items
  - expansion rate

Field shape:

- `BF.INFO key CAPACITY`
- `BF.INFO key SIZE`
- `BF.INFO key FILTERS`
- `BF.INFO key ITEMS`
- `BF.INFO key EXPANSION`
- invalid field rejected.

Expected: scalar field replies are `DESIGN_INTENDED`, not RedisBloom singleton arrays.

### 12. COMMAND metadata and ACL

Capture:

- `COMMAND INFO BF.RESERVE BF.ADD BF.MADD BF.INSERT BF.EXISTS BF.MEXISTS BF.INFO BF.CARD BF.SCANDUMP BF.LOADCHUNK`
- `COMMAND GETKEYS` for representative invocations of each command.
- `ACL DRYRUN` for a user with restricted permissions:
  - write commands rejected without write permission;
  - readonly commands allowed with read permissions;
  - module command categories/flags match DESIGN where specified.

Classify `BF.INFO` / `BF.CARD` lacking `fast` as `DESIGN_INTENDED` if observed, because DESIGN lists this as a RedisBloom difference.

### 13. SCANDUMP / LOADCHUNK private cursor protocol

Use a multi-layer filter so more than one layer chunk exists.

Verify:

- `BF.SCANDUMP key 0` returns `[1, header_blob]`.
- `BF.SCANDUMP key 1` returns `[2, layer0_full_bits]`.
- `BF.SCANDUMP key 2` returns next layer if present.
- Final cursor returns `[0, ""]`.
- Cursor sequence is layer-index based, not byte-offset based.
- `BF.LOADCHUNK newkey 1 <header>` creates loading shell.
- Sequential `LOADCHUNK` calls complete the filter.
- After completion:
  - membership has no false negatives for inserted corpus;
  - `BF.CARD` and `BF.INFO` match expected metadata;
  - `BF.SCANDUMP` on the loaded key produces a consistent sequence.

Classify RedisBloom byte-offset incompatibility as `DESIGN_INTENDED`; do not fail it.

### 14. Loading-state rejection

Create a key in loading state by applying only `LOADCHUNK cursor=1`.

While loading, verify all normal operations reject with `ERR filter is being loaded`:

- `BF.ADD`
- `BF.MADD`
- `BF.INSERT`
- `BF.EXISTS`
- `BF.MEXISTS`
- `BF.INFO`
- `BF.CARD`
- `BF.SCANDUMP`

Also verify:

- `BF.LOADCHUNK cursor>1` is accepted only for the loading key and only with exact expected chunk length/order.
- completed key rejects `LOADCHUNK cursor>1` with `ERR received bad data`.
- `LOADCHUNK cursor=1` against existing Bloom key rejects and preserves old data.
- malformed chunk length rejects and leaves the key in a well-defined state.

## Required evidence files

Stage 04 must produce at minimum:

- `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/commands.txt`
  - every runtime command executed, including server start/stop, harness invocation, Redis commands, and cleanup.
- `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/raw_resp.log`
  - raw RESP2/RESP3 request and reply frames, including binary-safe encoding.
- `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/normalized_results.md`
  - table of cases, expected behavior, actual behavior, classification, and evidence offsets/labels.
- `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/failures.md`
  - every failed, blocked, or surprising case with reproduction commands and raw evidence reference.

Also recommended by evidence policy:

- `.codex/gemini-bloom-audit/v6/evidence/stage04/commands.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/stdout.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/stderr.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/exit_codes.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/env_snapshot.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/evidence_index.md`

Stage result should also persist:

- `.codex/gemini-bloom-audit/v6/agents/stage04/main_execution.md`
- `.codex/gemini-bloom-audit/v6/agents/stage04/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage04/reviewer_output.md`

## Risks and likely false positives

- RESP3 native-shape expectations are a likely false positive. DESIGN says RESP3 is unsupported and commands use RESP2 return formats.
- RedisBloom SCANDUMP/LOADCHUNK byte-offset expectations are a likely false positive. gemini's layer-index cursor protocol is intentional.
- `BF.INFO key FIELD` scalar replies are intentional and must not be failed for not returning singleton arrays.
- `BF.INFO Size` numeric differences must not be compared to RedisBloom.
- Strict parser rejection of unknown or duplicate options is intentional.
- `BF.INSERT EXPANSION 0` mapping to `NONSCALING` is intentional.
- Error-message text should not be overfit unless DESIGN explicitly specifies the message. Prefer checking error class and important stable substrings.
- Binary item tests can be invalid if implemented through shell quoting. Use raw RESP bulk strings.
- Large boundary values can accidentally create unsafe allocations. Do not force dangerous allocations just to prove parser limits.
- Bloom filters can have false positives. Runtime membership checks must only treat false negatives for inserted items as failures.
- `BF.CARD` counts accepted unique insertions, not exact set cardinality in theoretical Bloom-filter terms under all possible false-positive scenarios. Avoid overclaiming exactness beyond tested corpus.
- Stage 03 RDB/wire findings may appear during LOADCHUNK validation. If reproduced with small payloads, record them; otherwise do not block Stage 04 on huge malicious payload probes.
- Existing TCL expected gaps should not be treated as Stage 04 failures unless the new runtime matrix independently shows DESIGN.md violation.

## PASS/BLOCKED criteria

PASS requires:

- All 10 BF commands have real Redis runtime evidence.
- RESP2 happy paths are covered with raw RESP capture.
- RESP3 behavior is tested and classified against DESIGN's unsupported RESP3 boundary.
- Wrong type and missing key behavior are covered.
- Duplicate, binary, empty, and long item behavior are covered.
- Capacity/error/expansion parser and safe boundary cases are covered.
- NONSCALING full behavior is covered.
- `MADD` and `INSERT` partial failure behavior is covered with raw per-element error evidence.
- `BF.INFO` full and field shapes are covered.
- `COMMAND INFO`, `COMMAND GETKEYS`, and `ACL DRYRUN` are covered.
- SCANDUMP/LOADCHUNK private layer-index cursor protocol is covered.
- Loading-state rejection is covered.
- Every `DESIGN_INTENDED` difference is explicitly labeled.
- Any `FAIL` has raw RESP evidence and a minimal reproduction.
- Reviewer confirms no omission of RESP3, LOADCHUNK/loading, wrong type, or required command coverage.

BLOCKED is acceptable only if:

- Redis server, module artifact, build artifact, port allocation, or platform permissions prevent runtime verification.
- The blocker is captured in evidence files.
- The stage result identifies exactly which matrix rows are `BLOCKED` or `NOT_VERIFIED`.
- The final report confidence impact is stated.
- Because Stage 04 is allowed to continue after `BLOCKED`, later stages may proceed only after LOOP_STATE records the degraded coverage.

## Items to defer to later stages

- RedisBloom v2.4.20 oracle comparison: Stage 05.
- RDB, DUMP/RESTORE, MIGRATE, fullsync replication, RDB-preamble AOF, and command-AOF transport validation: Stage 06.
- Malicious payload fuzzing and large malformed LOADCHUNK/RDB resource-boundary attacks: Stage 07.
- ASAN/UBSAN and memory-safety stress validation: Stage 08.
- Replica, cluster, broader operational metadata, and deployment behavior beyond the Stage 04 command metadata smoke checks: Stage 09.
- Performance, resource consumption, memory-accounting scale, and large-capacity stress: Stage 10.
- Final Chinese human-readable synthesis and confidence rating: Stage 11.
- Final report self-audit: Stage 12.
