#pragma once

#ifndef REDISMODULE_API
#define REDISMODULE_API extern
#define _TDIGEST_RDB_API_DEFINED
#endif

extern "C" {
#include "redismodule.h"
}

#ifdef _TDIGEST_RDB_API_DEFINED
#undef REDISMODULE_API
#undef _TDIGEST_RDB_API_DEFINED
#endif

#include <cstddef>
#include <cstdint>

extern RedisModuleType* TdigestType;

// RDB encoding version. This is a private wire format (command-level
// compatibility only, per project decision — no byte-format compatibility
// with any external T-Digest implementation is required).
constexpr int kTdigestEncVerCurrent = 1;

// Redis Module type callbacks
void* RdbLoadTdigest(RedisModuleIO* rdb, int encver);
void RdbSaveTdigest(RedisModuleIO* rdb, void* value);
void AofRewriteTdigest(RedisModuleIO* aof, RedisModuleString* key, void* value);
void FreeTdigest(void* value);
size_t TdigestMemUsage(const void* value);
void DigestTdigest(RedisModuleDigest* digest, void* value);
void* CopyTdigest2(RedisModuleKeyOptCtx* ctx, const void* value);
size_t FreeEffortTdigest2(RedisModuleKeyOptCtx* ctx, const void* value);
int DefragTdigest(RedisModuleDefragCtx* ctx, RedisModuleString* key, void** value);

// --- LOADCHUNK wire format (private, single-shot, AOF-only) ---
// Not exposed via SCANDUMP — T-Digest has no public incremental-serialization
// command (confirmed against redis.io command docs). TDIGEST.LOADCHUNK
// exists solely so AOF rewrite can replay a complete sketch in one command.
size_t TdigestComputeChunkSize(const class TdigestSketch& sketch);
size_t TdigestSerializeChunk(const TdigestSketch& sketch, void* output);
TdigestSketch* TdigestDeserializeChunk(const void* data, size_t length);
