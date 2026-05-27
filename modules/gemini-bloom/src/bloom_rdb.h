#pragma once

#ifndef REDISMODULE_API
#define REDISMODULE_API extern
#define _BF_RDB_API_DEFINED
#endif

extern "C" {
#include "redismodule.h"
}

#ifdef _BF_RDB_API_DEFINED
#undef REDISMODULE_API
#undef _BF_RDB_API_DEFINED
#endif

#include <cstddef>
#include <cstdint>
#include <utility>

extern RedisModuleType* BloomType;

// RDB encoding version history (wire-format protocol versions, required for
// reading RDB files produced by any compatible bloom filter implementation):
//   encver 2: added BloomFlags field
//   encver 4: added expansion factor field
constexpr int kEncVerWithFlags = 2;
constexpr int kEncVerWithExpansion = 4;
constexpr int kCurrentEncVer = 4;

// Typed wrapper around RedisModuleIO for serialization.
// Encapsulates the Redis Module API calls behind a clean C++ interface,
// eliminating direct RedisModule_Save*/Load* call sequences.
class RdbWriter {
public:
  explicit RdbWriter(RedisModuleIO* io) : io_(io) {}
  void PutUint(uint64_t v);
  void PutFloat(double v);
  void PutBlob(const uint8_t* data, uint64_t len);
private:
  RedisModuleIO* io_;
};

class RdbReader {
public:
  explicit RdbReader(RedisModuleIO* io) : io_(io) {}
  uint64_t GetUint();
  double GetFloat();
  // Returns (buffer, length). Caller owns the buffer via RedisModule_Free.
  std::pair<char*, size_t> GetBlob();
  bool Ok() const { return ok_; }
private:
  RedisModuleIO* io_;
  bool ok_ = true;
};

// Redis Module type callbacks
void* RdbLoadBloom(RedisModuleIO* rdb, int encver);
void RdbSaveBloom(RedisModuleIO* rdb, void* value);
void AofRewriteBloom(RedisModuleIO* aof, RedisModuleString* key, void* value);
void FreeBloom(void* value);
size_t BloomMemUsage(const void* value);
