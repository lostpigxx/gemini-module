#pragma once

#ifndef REDISMODULE_API
#define REDISMODULE_API extern
#define _CMS_RDB_API_DEFINED
#endif

extern "C" {
#include "redismodule.h"
}

#ifdef _CMS_RDB_API_DEFINED
#undef REDISMODULE_API
#undef _CMS_RDB_API_DEFINED
#endif

#include <cstddef>
#include <cstdint>

extern RedisModuleType* CmsType;

// RDB encoding version. This is a private wire format (command-level
// compatibility only, per project decision — no byte-format compatibility
// with any external Count-Min Sketch implementation is required).
constexpr int kCmsEncVerCurrent = 1;

// Redis Module type callbacks
void* RdbLoadCms(RedisModuleIO* rdb, int encver);
void RdbSaveCms(RedisModuleIO* rdb, void* value);
void AofRewriteCms(RedisModuleIO* aof, RedisModuleString* key, void* value);
void FreeCms(void* value);
size_t CmsMemUsage(const void* value);
void DigestCms(RedisModuleDigest* digest, void* value);
void* CopyCms2(RedisModuleKeyOptCtx* ctx, const void* value);
size_t FreeEffortCms2(RedisModuleKeyOptCtx* ctx, const void* value);
int DefragCms(RedisModuleDefragCtx* ctx, RedisModuleString* key, void** value);

// --- LOADCHUNK wire format (private, single-shot, AOF-only) ---
// Not exposed via SCANDUMP — CMS has no public incremental-serialization
// command (confirmed against redis.io command docs). CMS.LOADCHUNK exists
// solely so AOF rewrite can replay a complete sketch in one command.
size_t CmsComputeChunkSize(const class CmsSketch& sketch);
size_t CmsSerializeChunk(const CmsSketch& sketch, void* output);
CmsSketch* CmsDeserializeChunk(const void* data, size_t length);
