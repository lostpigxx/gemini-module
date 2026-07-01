# Stage 10 Static Resource Audit

- `bloom_commands.cc` enforces `capacity` in `1..2^30`, `error_rate` in `(0,1)`, `EXPANSION` in `0..32768`, and registers write commands with `deny-oom`.
- `BloomLayer::Create()` rejects non-finite/invalid error rates, `bitsPerEntry > 1000`, oversized computed bit counts, zero data size, and runtime per-layer data size above 2GB before allocation.
- `ScalingBloomFilter::AppendLayer()` rejects total runtime bit-array data above 4GB.
- `BF.INFO Size` is backed by `ScalingBloomFilter::BytesUsed()` and module `mem_usage`; it accounts for the C++ object, reserved layer slots, and bit arrays, so it is not expected to equal Redis object overhead exactly.
- `BF.SCANDUMP` emits cursor `1` for the header and then one full bit-array chunk per layer; this is a DESIGN private protocol, not RedisBloom's byte-offset chunking.
- `BF.LOADCHUNK` orderly replay copies full layer bit arrays, but Stage 07 findings remain open: it does not enforce strict cursor order or reject half-loaded persisted states.
- RDB/wire deserialization validates total data size and layer count, but Stage 03 findings remain open for missing per-layer 2GB deserialization cap and expansion values above `kMaxExpansion`.
