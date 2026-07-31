#pragma once

#ifndef REDISMODULE_API
#define REDISMODULE_API extern
#define _CF_RDB_API_DEFINED
#endif

extern "C" {
#include "redismodule.h"
}

#ifdef _CF_RDB_API_DEFINED
#undef REDISMODULE_API
#undef _CF_RDB_API_DEFINED
#endif

#include <cstddef>
#include <cstdint>

extern RedisModuleType* CuckooType;

// RDB encoding version. This is a private wire format (command-level
// compatibility only, per project decision — no byte-format compatibility
// with any external cuckoo filter implementation is required).
constexpr int kCfEncVerCurrent = 1;

// Redis Module type callbacks
void* RdbLoadCuckoo(RedisModuleIO* rdb, int encver);
void RdbSaveCuckoo(RedisModuleIO* rdb, void* value);
void AofRewriteCuckoo(RedisModuleIO* aof, RedisModuleString* key, void* value);
void FreeCuckoo(void* value);
size_t CuckooMemUsage(const void* value);
void DigestCuckoo(RedisModuleDigest* digest, void* value);
void* CopyCuckoo2(RedisModuleKeyOptCtx* ctx, const void* value);
size_t FreeEffortCuckoo2(RedisModuleKeyOptCtx* ctx, const void* value);
int DefragCuckoo(RedisModuleDefragCtx* ctx, RedisModuleString* key, void** value);

// --- SCANDUMP / LOADCHUNK wire format ---
// Private cursor protocol, structurally similar to BF.SCANDUMP/BF.LOADCHUNK
// but independently formatted (no byte-format compatibility requirement).
#pragma pack(push, 1)
struct CfWireHeader {
  uint64_t totalItems;
  uint64_t totalDeleted;
  uint32_t numLayers;
  uint32_t bucketSize;
  uint32_t maxIterations;
  uint32_t expansion;
  uint32_t maxExpansions;
};

struct CfWireLayerMeta {
  uint64_t numBuckets;
  uint64_t dataSize;
};
#pragma pack(pop)

size_t CfComputeHeaderSize(const class CuckooChain& chain);
size_t CfSerializeHeader(const class CuckooChain& chain, void* output);
CuckooChain* CfDeserializeHeader(const void* data, size_t length);
