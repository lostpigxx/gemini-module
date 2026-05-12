# Code Review: cancer_redis Bloom Filter Module

**Date**: 2026-05-12
**Branch**: `redisbloom-main`
**Scope**: Full source review of the C++20 bloom filter Redis module

---

## Overall Assessment

Code quality is high: modern C++20 style, RAII lifetime management, academic formula
references, comprehensive test coverage (GTest unit + TCL integration). Issues found
below are organized by severity.

---

## CRITICAL — Crashes / Undefined Behavior / Data Corruption

### C1. `std::bit_ceil` UB for large capacities

**File**: `src/bloom_filter.cc:103`

```cpp
layer.totalBits_ = std::bit_ceil(layer.totalBits_);
```

C++20 spec: if the result of `std::bit_ceil(x)` is not representable in the type,
behavior is undefined. When `totalBits_ > 2^63` (e.g. `cap = 2^60, bitsPerEntry ~ 9.6`),
`bit_ceil` would need to return `2^64`, exceeding `uint64_t` range.

**Fix**: Guard before call:
```cpp
if (layer.totalBits_ > (1ULL << 63)) return std::nullopt;
```

### C2. `BF.SCANDUMP` allocation failure corrupts Redis protocol

**File**: `src/bloom_commands.cc:440-447`

```cpp
RedisModule_ReplyWithArray(ctx, 2);  // committed to 2-element array
// ...
if (!hdrBuf) {
    return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
    // client expects 2 array elements, gets error reply instead
}
```

After `ReplyWithArray(2)`, the client expects exactly 2 elements. An error reply here
corrupts the RESP stream.

**Fix**: Move allocation before `ReplyWithArray`, return error early if it fails.

### C3. `GrowIfNeeded` capacity multiplication overflow

**File**: `src/sb_chain.cc:97`

```cpp
uint64_t nextCap = top.bloom.GetCapacity() * expansionFactor_;
```

If `GetCapacity() = 2^62` and `expansionFactor_ = 4`, result wraps to 0 via unsigned
overflow, creating a degenerate layer.

**Fix**: Overflow check before multiplication:
```cpp
if (top.bloom.GetCapacity() > UINT64_MAX / expansionFactor_) return false;
```

### C4. Move assignment calls destructor then uses object (UB)

**File**: `src/sb_chain.cc:43-57`

```cpp
this->~ScalingBloomFilter();  // object lifetime ends
layers_ = other.layers_;      // writing to dead object
```

Per C++ standard, after explicit destructor call the object's lifetime has ended.
Subsequent member access is undefined behavior.

**Fix**: Inline the resource cleanup instead of calling the destructor:
```cpp
for (size_t i = 0; i < numLayers_; i++)
    layers_[i].~FilterLayer();
if (layers_) RMFree(layers_);
```

---

## HIGH — Functional Bugs

### H1. `BloomConfigLoad` prefix matching bug

**File**: `src/bloom_config.cc:13`

```cpp
if (strncasecmp(arg, "ERROR_RATE", len) == 0) {
```

`len` is the length of `arg`. If `arg = "ERROR"` (len=5), `strncasecmp("ERROR",
"ERROR_RATE", 5)` returns 0 — any prefix matches. Same issue affects `INITIAL_SIZE`
and `EXPANSION`.

**Fix**: Add exact length checks:
```cpp
if (len == 10 && strncasecmp(arg, "ERROR_RATE", 10) == 0) {
```

### H2. `BF.LOADCHUNK` silently accepts truncated layer data

**File**: `src/bloom_commands.cc:506-507`

```cpp
size_t copyLen = std::min(dataLen, static_cast<size_t>(layer.bloom.GetDataSize()));
std::memcpy(layer.bloom.GetBitArray(), data, copyLen);
```

If `dataLen < GetDataSize()`, partial data is copied; remaining bits stay zero. This
silently produces an incorrect bloom filter (false negatives for lost entries).

**Fix**: Strict length check:
```cpp
if (dataLen != layer.bloom.GetDataSize()) {
    return RedisModule_ReplyWithError(ctx, "ERR data length mismatch");
}
```

### H3. `CmdInsert` missing replication flag on key creation

**File**: `src/bloom_commands.cc:282-289`

```cpp
bool changed = false;  // does not track key creation
```

Compare with `CmdMadd` which correctly does `bool changed = created;`. If `CmdInsert`
creates a key but all `Put` calls return `nullopt` (fixed-size overflow), the key
creation is not replicated to replicas.

**Fix**: Track key creation:
```cpp
bool changed = (keyType == REDISMODULE_KEYTYPE_EMPTY);
```

---

## MEDIUM — Robustness / Defensive Coding

### M1. `DeserializeHeader` no `numLayers` upper bound

**File**: `src/bloom_rdb.cc:163-165`

`hdr->numLayers` comes from untrusted LOADCHUNK data. A huge value (e.g. `UINT32_MAX`)
causes multi-GB allocation attempts, enabling OOM denial-of-service.

**Fix**: Add reasonable cap:
```cpp
constexpr uint32_t kMaxLayers = 1024;
if (hdr->numLayers > kMaxLayers) return nullptr;
```

### M2. Hash function `data.size()` truncated to `int`

**File**: `src/bloom_filter.cc:18,25`

```cpp
auto len = static_cast<int>(data.size());
```

If `data.size() > INT_MAX` (~2GB), silent truncation produces wrong hash values.

**Fix**: Clamp or reject oversized input.

### M3. FP rate underflow in `GrowIfNeeded`

**File**: `src/sb_chain.cc:98`

After many scaling rounds, `nextRate` becomes a subnormal double, causing
`bitsPerEntry` to grow extremely large and requesting huge memory allocations.

**Fix**: Add minimum threshold:
```cpp
constexpr double kMinFpRate = 1e-15;
if (nextRate < kMinFpRate) return false;
```

### M4. `BloomConfigLoad` silently ignores unknown arguments

**File**: `src/bloom_config.cc:8-43`

Typos like `EROR_RATE` are silently ignored; module loads with defaults. This masks
configuration errors.

**Fix**: Return error on unrecognized arguments.

### M5. `FromRdbShell` wastefully constructs then destroys a default filter

**File**: `src/sb_chain.cc:138-149`

Constructs a full ScalingBloomFilter (allocating BloomLayer + bit array), then
immediately tears it down. Should use a lightweight empty-shell constructor.

---

## LOW — Code Quality

### L1. Destructor calls `~BloomLayer()` instead of `~FilterLayer()`

**File**: `src/sb_chain.cc:25`

While `FilterLayer`'s other member (`size_t`) is trivially destructible, semantically
the destructor should call `layers_[i].~FilterLayer()`.

### L2. Wire-format structs assume little-endian byte order

`WireLayerMeta` and `WireFilterHeader` use `#pragma pack(push, 1)` for binary
serialization. On big-endian architectures, SCANDUMP/LOADCHUNK data would be
incompatible. Known limitation in the Redis ecosystem.

### L3. Test code uses manual `malloc` + placement new

Tests in `test/sb_chain_test.cc` could use simpler stack allocation since
`ScalingBloomFilter` supports move semantics.

### L4. No unit tests for `SerializeHeader`/`DeserializeHeader`

RDB wire-format serialization is only covered by TCL integration tests. Adding
mock-free unit tests would catch regressions faster.
