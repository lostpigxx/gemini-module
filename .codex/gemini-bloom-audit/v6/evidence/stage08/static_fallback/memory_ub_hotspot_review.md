# Static Memory/UB Hotspot Review

Status: `PARTIAL / STATIC_FALLBACK`

Scope:

- RAII and move lifecycle for `BloomLayer`.
- Placement-new, manual destructor, and array growth for `ScalingBloomFilter`.
- RDB/wire deserialization allocations and arithmetic checks.
- `BF.LOADCHUNK` copy and loading-state behavior.

Observations:

- `BloomLayer` owns `bitArray_`, frees it in the destructor, and nulls the source pointer in move construction/assignment. Static review did not find a double-free path in `modules/gemini-bloom/src/bloom_filter.cc:39`, `:46`, or `:59`.
- `ScalingBloomFilter::AppendLayer` grows the `FilterLayer` array with placement-new move construction, destroys old elements after move, frees the old array, and increments `numLayers_` only after constructing the new slot. Static review did not find a clear UAF/double-destroy path in `modules/gemini-bloom/src/sb_chain.cc:64`.
- `ScalingBloomFilter::FromRdbShell` uses `RMCalloc` for layer slots and keeps `numLayers_ = 0` until `SetLayer` constructs each slot, so destructor cleanup covers only constructed entries. Static review did not find a destructor-over-uninitialized-slot issue in `modules/gemini-bloom/src/sb_chain.cc:162` or `:181`.
- `BloomLayer::ReadFrom` validates canonical fields before allocation, checks Redis blob length, allocates `dataSize_`, copies exactly `bufLen`, and frees the Redis buffer. Static review did not find an immediate OOB copy in `modules/gemini-bloom/src/bloom_rdb.cc:85`.
- Carry-forward resource-validation issue remains: `ValidateLayerFields` does not enforce DESIGN's per-layer 2GB cap, and the later RDB/wire total-data guard allows one layer up to `kMaxTotalDataSize`. This is `GBV6-03-001`, not a new Stage 08 sanitizer finding.
- Carry-forward expansion-validation issue remains: RDB/wire reads only reject values above `UINT_MAX` and zero for scaling filters, not values above DESIGN's `kMaxExpansion`. This is `GBV6-03-002`, not a new Stage 08 sanitizer finding.
- `BF.LOADCHUNK` copies `dataLen` into the selected layer only after checking `dataLen == layer.bloom.GetDataSize()`, so the direct copy is size-guarded. The ordering/completeness flaw in `modules/gemini-bloom/src/bloom_commands.cc:630` remains `GBV6-07-001`/`GBV6-07-002`, a data-integrity and persistence issue rather than a sanitizer-proven memory issue.

Conclusion:

Static fallback found no additional concrete memory-safety/UB finding beyond existing Stage 03 and Stage 07 carry-forward issues. Because runtime ASAN/UBSAN and valgrind were blocked, memory safety remains `NOT_VERIFIED` rather than PASS.

