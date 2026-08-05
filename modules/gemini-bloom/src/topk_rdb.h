#pragma once

#ifndef REDISMODULE_API
#define REDISMODULE_API extern
#define _TOPK_RDB_API_DEFINED
#endif

extern "C" {
#include "redismodule.h"
}

#ifdef _TOPK_RDB_API_DEFINED
#undef REDISMODULE_API
#undef _TOPK_RDB_API_DEFINED
#endif

#include <cstddef>
#include <cstdint>

extern RedisModuleType* TopkType;

// RDB encoding version. This is a private wire format (command-level
// compatibility only, per project decision — no byte-format compatibility
// with any external Top-K implementation is required).
constexpr int kTopkEncVerCurrent = 1;

// Redis Module type callbacks
void* RdbLoadTopk(RedisModuleIO* rdb, int encver);
void RdbSaveTopk(RedisModuleIO* rdb, void* value);
void AofRewriteTopk(RedisModuleIO* aof, RedisModuleString* key, void* value);
void FreeTopk(void* value);
size_t TopkMemUsage(const void* value);
void DigestTopk(RedisModuleDigest* digest, void* value);
void* CopyTopk2(RedisModuleKeyOptCtx* ctx, const void* value);
size_t FreeEffortTopk2(RedisModuleKeyOptCtx* ctx, const void* value);
int DefragTopk(RedisModuleDefragCtx* ctx, RedisModuleString* key, void** value);

// --- LOADCHUNK wire format (private, single-shot, AOF-only) ---
// Not exposed via SCANDUMP — Top-K has no public incremental-serialization
// command (confirmed against redis.io command docs). TOPK.LOADCHUNK exists
// solely so AOF rewrite can replay a complete sketch in one command.
size_t TopkComputeChunkSize(const class TopkSketch& sketch);
size_t TopkSerializeChunk(const TopkSketch& sketch, void* output);
TopkSketch* TopkDeserializeChunk(const void* data, size_t length);
