# GeminiBloom Partial-Load AOF Rewrite Follow-up Review

## Scope and Conclusion

This document reviews commit `f6e961d` (`fix: emit partially loaded bloom
filters during AOF rewrite`), which is a follow-up to `a238160`.

The commit contains two valid findings:

1. `RedisModule_LogIOError()` only logs and does not set the
   `RedisModuleIO` error flag in Redis 5.0.14, 6.2.20, or 7.2.11. Returning
   early from a module AOF rewrite callback can therefore omit a key while the
   rewrite is still reported as successful.
2. The old `RMAlloc()` null-return branch is not naturally reachable with the
   stock Redis allocator. Redis 5/6 use `zmalloc()` and Redis 7 uses
   `zmalloc_usable()`; both panic or abort on allocation failure instead of
   returning null. The previous successful-rewrite reproduction depended on a
   fault-injection allocator.

Removing the `IsLoading()` early return does prevent a partially loaded key
from disappearing completely. It does **not**, however, preserve the Bloom
filter data. The persistence problem remains open.

## Real Restart Reproduction

The following sequence was run in isolated Docker containers with the exact
module produced from `f6e961d`:

1. Create a source Bloom filter and insert 100 items.
2. Obtain its header and bit-array chunk with `BF.SCANDUMP`.
3. Apply only `BF.LOADCHUNK target 1 <header>`, leaving `target` in `Loading`
   state with an empty bit array.
4. Add 30,000 padding keys so that the rewrite child remains active.
5. Start `BGREWRITEAOF`, then apply the real data chunk after the rewrite fork.
6. Verify that the live target contains an inserted item.
7. Wait for a successful rewrite, restart Redis from the rewritten AOF, and
   check the same item again.

Results:

| Redis | Live membership | Key after restart | Membership after restart | `BF.CARD` after restart |
|---|---:|---:|---:|---:|
| 5.0.14 | 1 | 1 | 0 | 100 |
| 6.2.20 | 1 | 1 | 0 | 100 |
| 7.2.11 | 1 | 1 | 0 | 100 |

Redis 6 and Redis 7 also logged the following while replaying the incremental
AOF:

```text
ERR received bad data
after processing the command 'BF.LOADCHUNK'
```

The rewritten key therefore survives, but its metadata claims that it contains
100 items while its bit array does not contain those items. This creates Bloom
filter false negatives and is a data-loss condition.

## Root Cause

Applying a header chunk creates every declared layer with a zero-filled bit
array, sets `Loading`, and resets `ChunksLoaded()` to zero. Commit `f6e961d`
then makes the rewrite callback emit the header and **every** layer, including
layers whose data has not arrived.

During AOF replay:

1. The header command creates a new loading filter.
2. The zero-filled layer commands increment `ChunksLoaded()` until it equals
   `NumLayers()`.
3. `BF.LOADCHUNK` clears `Loading`, so the replay now treats the filter as
   complete.
4. A real layer command recorded in the incremental AOF after the rewrite fork
   is rejected because the filter is no longer loading.
5. Redis continues loading the AOF, leaving the zero-filled layer in place.

The claim that this reproduces RDB behavior is accurate, but RDB equivalence is
not a correctness guarantee. The current RDB path also strips `Loading` and
serializes zero-filled, not-yet-loaded layers.

## Why the Added Unit Test Missed It

`AofRewriteEmitsPartiallyLoadedFilter` inserts data into a complete filter and
only then calls `SetLoading()`. All of its layers already contain valid Bloom
bits. That is not the observable state created by applying only a
`BF.LOADCHUNK` header.

The test verifies the number and size of emitted commands and deserializes only
the header. It does not:

- construct a real header-only or partially loaded filter;
- replay the emitted layer commands;
- append a post-fork `BF.LOADCHUNK` command;
- restart a real Redis server from the resulting AOF;
- verify membership and cardinality after restart.

The unit test passes, but it does not exercise the failing persistence
sequence.

## Recommended Fix Direction

For command-AOF rewriting, emit the header and only the prefix of layers that
has actually arrived:

```text
layer_count = filter.IsLoading() ? filter.ChunksLoaded() : filter.NumLayers()
emit header
emit layers [0, layer_count)
```

Replaying the header automatically enters `Loading`. Replaying only the loaded
prefix restores the same `ChunksLoaded()` value and leaves the filter loading.
Any real chunks written to the incremental AOF after the fork can then continue
from the correct cursor. If no later chunk exists, the restored filter remains
incomplete and continues rejecting normal Bloom operations instead of becoming
a queryable filter with missing bits.

The RDB path requires a separate design because its current encoding does not
preserve `Loading` or `ChunksLoaded()`. Options include extending the RDB
encoding to store partial-load progress or redesigning imports so an incomplete
object is not exposed as a completed key after persistence.

Required regression coverage should include Redis 5.0.14, 6.2.20, and 7.2.11
for all of the following:

- header-only state followed by command-AOF rewrite and restart;
- one or more loaded layers in a multi-layer filter;
- remaining chunks written after the rewrite fork and replayed from the AOF
  tail;
- membership, cardinality, loading state, and absence of AOF replay errors;
- `BGSAVE`/RDB restart and Redis 6/7 AOF RDB-preamble behavior.

## Resolution

Independently reproduced with `ci/bloom_partial_load_test.py`. Before the fix
all five cases failed on all three Redis versions, including the RDB-preamble
path that is the Redis 6/7 default:

| Case | Result before fix |
|---|---|
| command-AOF, header-only | 100 false negatives, `BF.CARD` 100 |
| command-AOF, multi-layer partial prefix | 80 false negatives, `BF.CARD` 100 |
| RDB-preamble AOF, header-only | 100 false negatives, `BF.CARD` 100 |
| RDB-preamble AOF, multi-layer partial prefix | 80 false negatives, `BF.CARD` 100 |
| `BGSAVE` snapshot, header-only | restored as a queryable filter, 100 false negatives |

The governing rule is that **an incomplete filter must never be restored as a
complete one**. A filter whose header promises items its zero-filled layers do
not contain silently breaks the Bloom no-false-negative contract. Staying in
`Loading` and rejecting operations is a visible, recoverable failure; becoming a
queryable filter that lies is not.

### Command-AOF

`AofRewriteBloom()` emits the header plus only the first `ChunksLoaded()`
layers. Replaying the header re-enters `Loading`, so the loaded prefix restores
the same cursor and post-fork `BF.LOADCHUNK` commands in the AOF tail continue
from the correct position. Complete filters still emit every layer.

### RDB

The RDB encoding now persists `Loading` and, only when it is set, one extra
`chunksLoaded` field written directly after the expansion factor. This is
required for correctness on the default Redis 6/7 configuration, where the AOF
base is an RDB preamble and the tail carries the post-fork chunks.

The encoding stays self-describing rather than bumping `kCurrentEncVer`, because
`ci/bloom_migrate_test.py` covers gemini-bloom → RedisBloom export and an
unconditional version bump would break it. Complete filters never set the bit
and write no extra field, so their bytes are unchanged; `ValidateRdbFlags()`
accepts the bit only on the RDB path, and the SCANDUMP wire header still strips
it because replaying that header re-enters `Loading` on its own.

`Clone()` (the `COPY` command's `copy2` callback) had the same defect: it
carried the `Loading` bit across but reset the cursor, so the copy rejected the
next chunk as out-of-order. It now preserves the cursor.

### Verification

`ci/bloom_partial_load_test.py` runs in `ci/run_tests.sh` and applies the
remaining chunks strictly inside the rewrite fork window, failing rather than
passing vacuously if that race is lost. All five cases pass on Redis 5.0.14,
6.2.20, and 7.2.11, with no `received bad data` in the replay logs. The full
suite — including all seven RedisBloom migration directions — passes on Redis 7;
on Redis 5 and 6 the only failure is the pre-existing Tcl harness issue below.

Unit coverage in `bloom_rdb_test.cc` builds the real header-only state from a
SCANDUMP header and replays through a mirror of the `CmdLoadchunk` cursor
protocol, including its rejections, rather than counting emitted commands.

### Unrelated pre-existing issue

The Tcl suite has never run on Redis 5 or 6: `start_redis` passes
`--enable-debug-command`, which is Redis 7-only, so the server refuses to start.
Confirmed identically on `aac2cc3`, so it is independent of this work and is not
addressed here.
