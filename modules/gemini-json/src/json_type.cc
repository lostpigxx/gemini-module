#include "json_type.h"
#include "json_value.h"
#include "json_parse.h"
#include "json_serialize.h"

#include <cstring>

RedisModuleType* JsonModuleType = nullptr;

void* RdbLoadJson(RedisModuleIO* rdb, int encver) {
  if (encver > kJsonEncVer) return nullptr;

  size_t len;
  char* buf = RedisModule_LoadStringBuffer(rdb, &len);
  if (!buf) return nullptr;

  auto result = JsonParse(std::string_view{buf, len});
  RedisModule_Free(buf);

  return result.value;
}

void RdbSaveJson(RedisModuleIO* rdb, void* value) {
  auto* root = static_cast<JsonValue*>(value);
  auto json = JsonSerialize(root);
  RedisModule_SaveStringBuffer(rdb, json.data(), json.size());
}

void AofRewriteJson(RedisModuleIO* aof, RedisModuleString* key, void* value) {
  auto* root = static_cast<JsonValue*>(value);
  auto json = JsonSerialize(root);
  RedisModule_EmitAOF(aof, "JSON.SET", "scc", key, "$", json.c_str());
}

void FreeJson(void* value) {
  JsonValue::Destroy(static_cast<JsonValue*>(value));
}

size_t JsonMemUsage(const void* value) {
  return static_cast<const JsonValue*>(value)->MemoryUsage();
}
