# Stage 03 Static Audit By File

## Summary

Stage 03 reviewed every required `modules/gemini-bloom` source and test file statically. No production code was modified. The implementation largely matches `DESIGN.md`; the static audit identified two new RDB/wire input-boundary findings and one new test-quality finding. The inherited `GBV6-00-002` source-comment contradiction is still present.

## Source files

### `modules/gemini-bloom/DESIGN.md`

- Acts as the highest-priority contract for this stage.
- Relevant static constraints include source/test layout at `modules/gemini-bloom/DESIGN.md:313-331`, RDB validation claims at `modules/gemini-bloom/DESIGN.md:236-239`, resource limits at `modules/gemini-bloom/DESIGN.md:552-559`, untrusted RDB/LOADCHUNK handling at `modules/gemini-bloom/DESIGN.md:561-566`, and known SCANDUMP/AOF limitations at `modules/gemini-bloom/DESIGN.md:686-698`.
- Static finding impact: the design states max expansion is `32768` and single-layer max data size is `2 GB`; RDB/wire loaders do not fully enforce those boundaries.
- Inherited finding still relevant: Stage 00 already found the claimed RedisBloom fixture path absent (`GBV6-00-001`).

### `modules/gemini-bloom/CMakeLists.txt`

- Module source list includes all implementation files at `modules/gemini-bloom/CMakeLists.txt:1-9`.
- Shared module is built as `redis_bloom.so` at `modules/gemini-bloom/CMakeLists.txt:11-27`.
- GTest targets and `bloom_test` aggregate target are defined at `modules/gemini-bloom/CMakeLists.txt:29-86`.
- Static alignment: source/test targets match DESIGN source layout.
- Carry-forward: Stage 02 showed `bloom_test` target runtime failure on this macOS host due GTest dylib RPATH (`GBV6-02-001`).

### `modules/gemini-bloom/src/redis_bloom_module.cc`

- Module identity is `GeminiBloom` version 1 at `modules/gemini-bloom/src/redis_bloom_module.cc:9-12`, matching DESIGN's non-RedisBloom module identity difference.
- Config is parsed before datatype/command registration at `modules/gemini-bloom/src/redis_bloom_module.cc:15-17`.
- Data type name is `MBbloom--` with current encver callback registration at `modules/gemini-bloom/src/redis_bloom_module.cc:19-29`, matching RDB compatibility commitment.
- Command registration failure returns module load failure at `modules/gemini-bloom/src/redis_bloom_module.cc:35-38`, consistent with same-instance command/type conflict limitation.

### `modules/gemini-bloom/src/bloom_commands.cc`

- `AllocFilter` uses `Use64Bit | NoRound` plus optional `FixedSize`, matching DESIGN default mode at `modules/gemini-bloom/src/bloom_commands.cc:19-31`.
- Auto-create path rejects non-Bloom types and loading filters at `modules/gemini-bloom/src/bloom_commands.cc:39-78`.
- `BF.RESERVE` validates rate, capacity, unknown options, duplicate `EXPANSION`/`NONSCALING`, and expansion `0..32768` at `modules/gemini-bloom/src/bloom_commands.cc:100-178`.
- `BF.ADD`, `BF.MADD`, and `BF.INSERT` use postponed array lengths and `ReplicateVerbatim` only on mutation/creation at `modules/gemini-bloom/src/bloom_commands.cc:181-235` and `modules/gemini-bloom/src/bloom_commands.cc:345-404`, matching partial-failure semantics.
- `ParseInsertOptions` validates strict parser behavior and `NOCREATE + CAPACITY/ERROR` before key access at `modules/gemini-bloom/src/bloom_commands.cc:248-343`.
- Read paths block loading filters consistently for `EXISTS`, `MEXISTS`, `INFO`, `CARD`, and `SCANDUMP` at `modules/gemini-bloom/src/bloom_commands.cc:407-556`.
- `BF.INFO` returns scalar field values, not singleton arrays, at `modules/gemini-bloom/src/bloom_commands.cc:477-512`; this is a DESIGN_INTENDED RedisBloom difference.
- `BF.SCANDUMP` uses layer-index cursor protocol at `modules/gemini-bloom/src/bloom_commands.cc:534-587`, matching DESIGN private protocol.
- `BF.LOADCHUNK` rejects cursor <1, rejects cursor=1 on existing Bloom key, creates loading shells from validated headers, requires loading state for data chunks, checks data chunk length, and clears loading on final layer at `modules/gemini-bloom/src/bloom_commands.cc:590-649`.
- Command flags/key specs are registered uniformly at `modules/gemini-bloom/src/bloom_commands.cc:653-679`; DESIGN's `BF.INFO`/`BF.CARD` lack of `fast` is intentional.

### `modules/gemini-bloom/src/bloom_commands.h`

- Header only declares `RegisterBloomCommands` and handles RedisModule API include guards at `modules/gemini-bloom/src/bloom_commands.h:1-17`.
- No static issue found.

### `modules/gemini-bloom/src/bloom_rdb.cc`

- `RdbReader` tracks IO errors after loads at `modules/gemini-bloom/src/bloom_rdb.cc:20-38`.
- `ValidateLayerFields` rejects zero capacity/totalBits/hashCount, invalid fpRate/bitsPerEntry, bad log2Bits, dataSize mismatch, and hashCount mismatch at `modules/gemini-bloom/src/bloom_rdb.cc:53-68`.
- Static finding `GBV6-03-001`: `ValidateLayerFields` does not enforce DESIGN's single-layer 2GB data-size cap. `BloomLayer::ReadFrom` can allocate `layer.dataSize_` after blob load at `modules/gemini-bloom/src/bloom_rdb.cc:99-118`, while `DeserializeHeader` can allocate `meta.dataSize` through `FromWireMeta` at `modules/gemini-bloom/src/bloom_rdb.cc:135-147` and `modules/gemini-bloom/src/bloom_rdb.cc:309-317`.
- RDB field order matches DESIGN at `modules/gemini-bloom/src/bloom_rdb.cc:155-165`; persistent flags strip runtime-only Loading at `modules/gemini-bloom/src/bloom_rdb.cc:158`.
- RDB loader rejects unsupported encver at `modules/gemini-bloom/src/bloom_rdb.cc:330-333`.
- Static finding `GBV6-03-002`: RDB loader checks expansion only for UINT range and zero-on-scalable at `modules/gemini-bloom/src/bloom_rdb.cc:180-189`, not `> kMaxExpansion`.
- RDB loader enforces max layers, item count, item sum, and total data-size cap at `modules/gemini-bloom/src/bloom_rdb.cc:187-236`.
- Wire serializer strips Loading and writes filter metadata at `modules/gemini-bloom/src/bloom_rdb.cc:245-260`.
- Wire loader rejects zero/too many layers, unknown flags, expansion zero on scalable filters, item-count mismatch, and total data-size >4GB at `modules/gemini-bloom/src/bloom_rdb.cc:269-319`.
- Static finding `GBV6-03-002` also applies to wire header: `hdr->expansionFactor` is not checked against `kMaxExpansion` before `FromRdbShell` at `modules/gemini-bloom/src/bloom_rdb.cc:281-305`.
- AOF rewrite emits gemini-private `BF.LOADCHUNK` sequence and logs/skips key on header allocation failure at `modules/gemini-bloom/src/bloom_rdb.cc:341-361`; this is DESIGN_INTENDED but must be reported as a limitation.

### `modules/gemini-bloom/src/bloom_rdb.h`

- Encver constants are `2` and `4`, current `4`, matching DESIGN at `modules/gemini-bloom/src/bloom_rdb.h:23-30`.
- Redis Module callbacks are declared at `modules/gemini-bloom/src/bloom_rdb.h:57-62`.
- No standalone static issue found.

### `modules/gemini-bloom/src/bloom_filter.cc`

- Hash policies use RedisBloom-compatible seeds and `h2 = hash(data, seed=h1)` at `modules/gemini-bloom/src/bloom_filter.cc:23-35`.
- Hash input length is capped at `INT_MAX` at `modules/gemini-bloom/src/bloom_filter.cc:23-33`. Static classification: no Stage 03 finding because Redis command bulk-string practical limits are below `INT_MAX`; Stage 10 can revisit huge-item behavior if needed.
- RAII destructor and move semantics transfer/free bit arrays at `modules/gemini-bloom/src/bloom_filter.cc:39-74`.
- Bloom math uses `-log(fpRate)/(ln2)^2`, `ceil(ln2*bpe)`, minimum 1024 bits, and NoRound 64-bit alignment at `modules/gemini-bloom/src/bloom_filter.cc:81-127`.
- `BloomLayer::Create` enforces `kMaxBitsPerEntry` and local 2GB per-layer data-size cap at `modules/gemini-bloom/src/bloom_filter.cc:108-131`.
- Bit operations and probe positions are unsigned and bounded by `ProbePosition` modulo/mask paths at `modules/gemini-bloom/src/bloom_filter.cc:141-186`.

### `modules/gemini-bloom/src/bloom_filter.h`

- Supported persistent flags intentionally exclude `RawBits` and runtime-only `Loading` at `modules/gemini-bloom/src/bloom_filter.h:46-55`.
- Public constants define `kMaxCapacity`, `kMaxExpansion`, `kMaxBitsPerEntry`, and `kMaxTotalDataSize` at `modules/gemini-bloom/src/bloom_filter.h:57-60`.
- `ProbePosition` uses unsigned wrap then mask/modulo at `modules/gemini-bloom/src/bloom_filter.h:96-103`, compatible with deterministic double hashing.
- No standalone static issue found.

### `modules/gemini-bloom/src/sb_chain.cc`

- First layer fpRate is tightened for scalable filters and not tightened for fixed filters at `modules/gemini-bloom/src/sb_chain.cc:10-17`.
- Destructor and move assignment handle placement-new-owned layers at `modules/gemini-bloom/src/sb_chain.cc:25-60`.
- `AppendLayer` grows the `FilterLayer` array by allocating new storage, placement-new moving layers, destroying old layers, and freeing old storage at `modules/gemini-bloom/src/sb_chain.cc:64-77`, avoiding `realloc` on non-trivial objects.
- `AppendLayer` enforces total data-size cap before installing a new layer at `modules/gemini-bloom/src/sb_chain.cc:79-89`.
- `GrowIfNeeded` checks fixed-size, capacity multiplication overflow, and min fpRate at `modules/gemini-bloom/src/sb_chain.cc:106-117`.
- `Put` first treats any layer hit as duplicate, then grows and increments item counts only for new inserts at `modules/gemini-bloom/src/sb_chain.cc:121-131`. This is normal Bloom semantics, not a bug.
- `BytesUsed` includes `ScalingBloomFilter`, reserved `FilterLayer` slots, and bit arrays at `modules/gemini-bloom/src/sb_chain.cc:152-158`, matching DESIGN's gemini-specific size accounting.
- `FromRdbShell` and `SetLayer` use allocation plus placement new at `modules/gemini-bloom/src/sb_chain.cc:162-186`.

### `modules/gemini-bloom/src/sb_chain.h`

- Public API exposes layer spans, loading flag helpers, RDB shell construction, and wire header structures at `modules/gemini-bloom/src/sb_chain.h:45-114`.
- Inherited `GBV6-00-002` is confirmed: comment at `modules/gemini-bloom/src/sb_chain.h:88-91` says wire field order/types/packing match RedisBloom for cross-implementation compatibility, contradicting DESIGN private SCANDUMP/LOADCHUNK boundary.

### `modules/gemini-bloom/src/bloom_config.cc`

- Module load args validate `ERROR_RATE`, `INITIAL_SIZE`, `EXPANSION`, and reject unknown args at `modules/gemini-bloom/src/bloom_config.cc:10-57`.
- Config `EXPANSION` requires `1..32768` at `modules/gemini-bloom/src/bloom_config.cc:39-50`, matching DESIGN.
- No standalone static issue found.

### `modules/gemini-bloom/src/bloom_config.h`

- Default config values are `0.01`, `100`, and `2` at `modules/gemini-bloom/src/bloom_config.h:19-23`, matching DESIGN.
- No static issue found.

### `modules/gemini-bloom/src/murmur2.cc`

- Implements endian-stable MurmurHash2 and MurmurHash64A from bytes at `modules/gemini-bloom/src/murmur2.cc:6-86`.
- Static alignment: no platform-dependent unaligned load is used; bytes are assembled explicitly.
- No static issue found.

### `modules/gemini-bloom/src/murmur2.h`

- Hash function declarations use `int len`, matching implementation and current hash policy cap at `INT_MAX`.
- No static issue found.

### `modules/gemini-bloom/src/rm_alloc.h`

- Test builds use `malloc/calloc/free`; production builds use RedisModule allocation APIs at `modules/gemini-bloom/src/rm_alloc.h:5-31`, matching DESIGN.
- No static issue found.

## Test files

### `modules/gemini-bloom/tests/bloom_filter_test.cc`

- Covers deterministic Murmur hashes, BloomLayer lifecycle/move, create parameter rejection, flags, RawBits behavior, resource constants, golden vectors, bit addressing, and probe positioning at `modules/gemini-bloom/tests/bloom_filter_test.cc:14-274`.
- Static gap: no direct test for command/runtime item size above `INT_MAX`; classified as later-stage/perf-resource scope, not a Stage 03 finding.

### `modules/gemini-bloom/tests/sb_chain_test.cc`

- Covers construction, put/contains, 32-bit hash flag, no false negatives, expansion, fixed-size overflow, duplicate semantics, capacity, bytes used, move assignment, expansion=1, `FromRdbShell+SetLayer`, safe relocation, extreme parameter rejection, total data-size accounting, loading lifecycle, and span interface at `modules/gemini-bloom/tests/sb_chain_test.cc:15-370`.
- Static alignment: tests directly cover historical placement-new and relocation risks.

### `modules/gemini-bloom/tests/bloom_rdb_test.cc`

- Covers RDB round-trip, encver 2/4, unknown encvers, wire round-trip, full SCANDUMP/LOADCHUNK simulation, malformed headers, max layers, many invalid layer fields, narrowing bypasses, total/item count integrity, flags, wire data-size mismatch, total data-size >4GB, and Loading flag stripping at `modules/gemini-bloom/tests/bloom_rdb_test.cc:65-1386`.
- Static finding `GBV6-03-002`: tests only cover expansion zero for RDB/wire (`modules/gemini-bloom/tests/bloom_rdb_test.cc:1054-1074`, `modules/gemini-bloom/tests/bloom_rdb_test.cc:1251-1285`), not expansion >32768.
- Static finding `GBV6-03-001`: tests cover wire total data-size >4GB (`modules/gemini-bloom/tests/bloom_rdb_test.cc:1306-1326`) but not one layer with data-size >2GB and total <=4GB.

### `modules/gemini-bloom/tests/tcl/bloom_test.tcl`

- Provides self-contained RESP client, runtime BF command tests, SCANDUMP/LOADCHUNK round-trip, wrong type checks, parser/expansion checks, persistence, resource limits, partial failure, duplicate options, config load args, loading-state safety, cursor protocol, COMMAND metadata, ACL dryrun, and existing-key LOADCHUNK rejection at `modules/gemini-bloom/tests/tcl/bloom_test.tcl:1-1615`.
- Command-level expansion and capacity boundaries are covered at `modules/gemini-bloom/tests/tcl/bloom_test.tcl:1015-1023` and `modules/gemini-bloom/tests/tcl/bloom_test.tcl:1220-1246`.
- Static finding `GBV6-03-003`: the section named `Per-layer data size cap (SAFE-06)` and test named `BloomLayer::Create rejects extremely large capacity` actually assert that `BF.RESERVE` at `kMaxCapacity` succeeds; comments also say `512MB` while DESIGN/code say `2GB` (`modules/gemini-bloom/tests/tcl/bloom_test.tcl:1583-1593`).
