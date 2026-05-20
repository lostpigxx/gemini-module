#include "json_type.h"
#include "json_value.h"
#include "json_parse.h"
#include "json_serialize.h"

#include <cstring>

RedisModuleType* JsonModuleType = nullptr;
bool JsonCompatMode = false;

// ======================================================================
// Binary RDB format (encver 2) — Gemini native
// ======================================================================
// Each node: type tag (uint64) followed by type-specific payload.
// Tag values: 0=null, 1=false, 2=true, 3=integer, 4=double,
//             5=string, 6=array, 7=object

enum : uint64_t {
  kTagNull    = 0,
  kTagFalse   = 1,
  kTagTrue    = 2,
  kTagInteger = 3,
  kTagDouble  = 4,
  kTagString  = 5,
  kTagArray   = 6,
  kTagObject  = 7,
};

static void BinarySave(RedisModuleIO* rdb, const JsonValue* v) {
  switch (v->Type()) {
    case JsonType::kNull:
      RedisModule_SaveUnsigned(rdb, kTagNull);
      break;
    case JsonType::kBool:
      RedisModule_SaveUnsigned(rdb, v->GetBool() ? kTagTrue : kTagFalse);
      break;
    case JsonType::kInteger:
      RedisModule_SaveUnsigned(rdb, kTagInteger);
      RedisModule_SaveSigned(rdb, v->GetInteger());
      break;
    case JsonType::kNumber:
      RedisModule_SaveUnsigned(rdb, kTagDouble);
      RedisModule_SaveDouble(rdb, v->GetNumber());
      break;
    case JsonType::kString: {
      RedisModule_SaveUnsigned(rdb, kTagString);
      auto sv = v->GetString();
      RedisModule_SaveStringBuffer(rdb, sv.data(), sv.size());
      break;
    }
    case JsonType::kArray:
      RedisModule_SaveUnsigned(rdb, kTagArray);
      RedisModule_SaveUnsigned(rdb, v->ArrayLen());
      for (uint32_t i = 0; i < v->ArrayLen(); i++) {
        BinarySave(rdb, v->ArrayGet(i));
      }
      break;
    case JsonType::kObject: {
      RedisModule_SaveUnsigned(rdb, kTagObject);
      RedisModule_SaveUnsigned(rdb, v->ObjectLen());
      auto* entries = v->ObjectEntries();
      for (uint32_t i = 0; i < v->ObjectLen(); i++) {
        RedisModule_SaveStringBuffer(rdb, entries[i].key, entries[i].key_len);
        BinarySave(rdb, entries[i].value);
      }
      break;
    }
  }
}

static JsonValue* BinaryLoad(RedisModuleIO* rdb) {
  uint64_t tag = RedisModule_LoadUnsigned(rdb);
  switch (tag) {
    case kTagNull:
      return JsonValue::CreateNull();
    case kTagFalse:
      return JsonValue::CreateBool(false);
    case kTagTrue:
      return JsonValue::CreateBool(true);
    case kTagInteger:
      return JsonValue::CreateInteger(RedisModule_LoadSigned(rdb));
    case kTagDouble:
      return JsonValue::CreateNumber(RedisModule_LoadDouble(rdb));
    case kTagString: {
      size_t len;
      char* buf = RedisModule_LoadStringBuffer(rdb, &len);
      if (!buf) return nullptr;
      auto* v = JsonValue::CreateString(buf, static_cast<uint32_t>(len));
      RedisModule_Free(buf);
      return v;
    }
    case kTagArray: {
      uint64_t count = RedisModule_LoadUnsigned(rdb);
      auto* arr = JsonValue::CreateArray();
      if (!arr) return nullptr;
      for (uint64_t i = 0; i < count; i++) {
        auto* child = BinaryLoad(rdb);
        if (!child || !arr->ArrayAppend(child)) {
          JsonValue::Destroy(child);
          JsonValue::Destroy(arr);
          return nullptr;
        }
      }
      return arr;
    }
    case kTagObject: {
      uint64_t count = RedisModule_LoadUnsigned(rdb);
      auto* obj = JsonValue::CreateObject();
      if (!obj) return nullptr;
      for (uint64_t i = 0; i < count; i++) {
        size_t key_len;
        char* key = RedisModule_LoadStringBuffer(rdb, &key_len);
        if (!key) { JsonValue::Destroy(obj); return nullptr; }
        auto* val = BinaryLoad(rdb);
        if (!val) {
          RedisModule_Free(key);
          JsonValue::Destroy(obj);
          return nullptr;
        }
        obj->ObjectSet(key, static_cast<uint32_t>(key_len), val);
        RedisModule_Free(key);
      }
      return obj;
    }
    default:
      return nullptr;
  }
}

// ======================================================================
// JSON text RDB format (encver 1 & 3) — compat with RedisJSON
// ======================================================================

static void TextSave(RedisModuleIO* rdb, const JsonValue* v) {
  auto json = JsonSerialize(v);
  RedisModule_SaveStringBuffer(rdb, json.data(), json.size());
}

static JsonValue* TextLoad(RedisModuleIO* rdb) {
  size_t len;
  char* buf = RedisModule_LoadStringBuffer(rdb, &len);
  if (!buf) return nullptr;
  auto result = JsonParse(std::string_view{buf, len});
  RedisModule_Free(buf);
  return result.value;
}

// ======================================================================
// Public interface — dispatches based on encver and compat mode
// ======================================================================

void* RdbLoadJson(RedisModuleIO* rdb, int encver) {
  if (encver == 1 || encver == 3) return TextLoad(rdb);
  if (encver == 2) return BinaryLoad(rdb);
  return nullptr;
}

void RdbSaveJson(RedisModuleIO* rdb, void* value) {
  auto* v = static_cast<JsonValue*>(value);
  if (JsonCompatMode) {
    TextSave(rdb, v);
  } else {
    BinarySave(rdb, v);
  }
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
