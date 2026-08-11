# GeminiBloom Small-Key Memory and Command-AOF OOM Fixes

## Summary

Focused testing confirmed two real issues in the GeminiBloom implementation:

1. Every single-layer Bloom key reserved four layer slots, causing memory waste that grew linearly with the number of keys.
2. The command-AOF rewrite callback returned early in two places, silently omitting the key while Redis still published the incomplete AOF as successful.

The small-key memory issue is fixed. Commit `f6e961d` removes the early return
that omitted a partially loaded key, but follow-up restart testing found that it
does not preserve the key's Bloom data when loading continues after the rewrite
fork. See [Partial-Load AOF Rewrite Follow-up Review](gemini-bloom-partial-load-aof-review.md).

## 1. Excess Memory for Single-Layer Small Keys

### Problem and Impact

`ScalingBloomFilter` stores layers in a dynamic array. The old `AppendLayer()` implementation allocated four `FilterLayer` slots when it created the first layer. Keys that always remained single-layer still permanently paid for three unused slots.

The issue did not change membership results or directly lose data, but it could:

- make the process reach `maxmemory` earlier;
- trigger eviction or OOM earlier;
- make `BF.INFO Size` differ between a newly created single-layer object and the same object after an RDB reload.

### How It Was Found

`ci/bloom_memory_profile_test.py` runs the following workload on GeminiBloom and RedisBloom:

- 1,000, 5,000, 20,000, and 50,000 small keys with capacity 100;
- three inserted items per key, with full membership verification before and after BGSAVE/restart;
- object-size curves for capacities 1, 100, 1,000, 10,000, 100,000, and 1,000,000;
- object-size and logical-metadata checks for 1, 5, and 10 layers;
- linear regression across multiple checkpoints to calculate `used_memory` bytes per key instead of comparing a single memory snapshot.

Pre-fix results on Redis 5:

| Metric | GeminiBloom | RedisBloom | Difference |
|---|---:|---:|---:|
| Memory slope | 554.425 B/key | 346.425 B/key | +208 B/key |
| Total delta at 50,000 keys | - | - | +10,400,000 B |
| Per-key `BF.INFO Size` | 448 B | 296 B | +152 B |

After an RDB reload, GeminiBloom's per-key `Size` fell from 448 B to 256 B. This localized the difference to the runtime layer reservation rather than the Bloom bit array.

### Fix

The first layer now allocates one slot. The array grows to four slots only when a second layer is needed, then continues growing geometrically. This preserves contiguous storage for multi-layer filters without charging every single-layer key for unused layers.

After the fix, GeminiBloom's memory slope on Redis 5/6/7 is only about 16 B/key above the corresponding RedisBloom implementation. The total difference at 50,000 keys is about 0.8 MB, a reduction of approximately 92.3%. The test enforces a regression limit of `<= 64 B/key`.

Multi-layer objects can still contain unused dynamic-array slots. For example, the 5-layer and 10-layer cases on Redis 6/7 use 208 B and 400 B more than RedisBloom, respectively. This is an explicit cost that appears only when a filter actually becomes multi-layer.

## 2. Silent Key Loss From Early Return in the Command-AOF Callback

### Problem and Impact

The root cause is a property of the Redis Module API, verified against `src/module.c` and `src/aof.c` in the vendored Redis 5.0.14, 6.2.20, and 7.2.11 sources:

- `RM_LogIOError()` only logs. Unlike `RM_EmitAOF()`, it does **not** set `io->error`.
- `rewriteModuleObject()` returns `io.error ? 0 : 1`.

So any `aof_rewrite` callback that logs and returns early produces a *successful* rewrite with the key missing. The live dataset stays intact, but restarting from the new AOF permanently loses every omitted key. Genuine write failures are safe, because `RM_EmitAOF()` does set `io->error` itself.

`AofRewriteBloom()` had two such early returns.

**Header allocation failure (latent).** The callback allocated a temporary wire-header buffer with `RMAlloc()` and bailed out if it returned null. This branch was unreachable in a production build: `RMAlloc()` resolves to `RedisModule_Alloc()`, which is `zmalloc()` on Redis 5/6 and `zmalloc_usable()` on Redis 7, and Redis's allocator calls `zmalloc_oom_handler` → `abort()` rather than returning null. Under real memory pressure the rewrite child dies, the parent reports `aof_last_bgrewrite_status: err`, and the old AOF is retained. The reproduction below produced key loss only because the allocation was fault-injected to return null.

**Partially loaded filter (reachable).** The callback also skipped any filter with the `Loading` flag set. `BF.LOADCHUNK key 1 <header>` sets that flag, and it stays set until the final chunk arrives. A `BGREWRITEAOF` in that window silently dropped the key from the new AOF while the live server still reported it via `EXISTS`, and `RdbSaveBloom()` persisted it normally — so RDB and AOF disagreed. No fault injection required.

Trigger conditions for both:

- AOF is enabled;
- `aof-use-rdb-preamble no` selects the module command-AOF callback path;
- `BGREWRITEAOF` runs.

The default RDB preamble on Redis 6/7 does not use this path, so natural occurrence is unlikely, but the consequence is data loss.

### How It Was Found

First, the test allocator gained a failpoint that rejects the next `RMAlloc`, and `MockAofStream` was added to record `RedisModule_EmitAOF` calls. Before the fix, the failpoint made the callback log an error and emit no chunks.

The allocation point was then fault-injected in a real redis-server running inside a 64 MiB Docker cgroup with swap disabled. The dataset contained 100 small keys and one large key with capacity 10,000,000. The reproduction showed:

- `BGREWRITEAOF` ended with status `ok`;
- the new AOF was only 23 B;
- all 101 Bloom keys were missing after restart from that AOF.

This confirmed the *consequence* of an early return in the callback. It did not show that the header allocation itself can fail on a stock Redis build; see the reachability note above.

### Fix

Both early returns are gone.

The wire header has a maximum of 1,024 layers and a maximum size of about 54 KiB. The callback now uses a stack buffer with that fixed bound, so normal command-AOF rewrites no longer depend on temporary heap allocation. The remaining size check exists only so a corrupted layer count cannot overflow that buffer; `kMaxLayers` is enforced on every construction path, and scaling stops near 50 layers because the per-layer rate halves against a 1e-15 floor.

Partially loaded filters are now emitted rather than skipped. The first attempt emitted *every* layer and justified that by matching the RDB path; follow-up validation showed the RDB path was itself wrong, so that reasoning did not hold. Emitting every layer finalizes the filter during replay, which makes post-fork chunks in the AOF tail fail and leaves metadata that disagrees with the bit array. The corrected rule — emit only the layers that have actually arrived, and preserve `Loading` through RDB — is described in `gemini-bloom-partial-load-aof-review.md`.

Regression coverage includes:

- complete header and data-chunk emission when the next `RMAlloc` is forced to fail;
- GeminiBloom and RedisBloom control runs;
- Redis 5/6/7;
- 64 MiB with `--memory-swap=64m`: rewrite status `err`, old AOF retained, complete data after restart;
- 128 MiB with `--memory-swap=128m`: rewrite status `ok`, complete data after restart from the new AOF.

That first partial-load unit test was replaced. It created a fully populated
filter and then set `Loading` manually, which does not represent the zero-filled
layers produced by `BF.LOADCHUNK key 1 <header>`, and it never replayed the
emitted commands. The replacements build the real header-only state and replay
through the actual cursor protocol; cross-version restart coverage lives in
`ci/bloom_partial_load_test.py`.

All OOM pressure runs inside a Docker container with a finite cgroup limit. The test reads `/sys/fs/cgroup` and refuses to run with an unlimited limit or a limit above 256 MiB, preventing uncontrolled memory pressure on the host.

## Reproduction

The memory profile requires both GeminiBloom and the corresponding RedisBloom module:

```bash
python3 ci/bloom_memory_profile_test.py ./build/redis_bloom.so /opt/redisbloom.so
```

The OOM matrix must be launched from the host so Docker can apply finite memory limits:

```bash
bash ci/run_bloom_oom_matrix.sh gemini-module:redis7
```
