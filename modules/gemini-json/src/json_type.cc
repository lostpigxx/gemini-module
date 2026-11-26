#include "json_type.h"
#include "json_value.h"
#include "json_parse.h"
#include "json_serialize.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

RedisModuleType* JsonModuleType = nullptr;
bool JsonCompatMode = false;

namespace {

constexpr int kMaxRdbDepth = kMaxJsonDepth;
constexpr uint64_t kMaxRdbCollectionLen = 10000000;
constexpr size_t kLazyFreeEffortCutoff = 64;

bool HasIoError(RedisModuleIO* rdb) {
  return RedisModule_IsIOError && RedisModule_IsIOError(rdb) != 0;
}

size_t CountNodesBounded(const JsonValue* v, size_t limit, int depth = 0) {
  if (!v || depth > kMaxRdbDepth || limit == 0) return 0;
  size_t count = 1;
  if (count >= limit) return count;
  if (v->IsArray()) {
    if (v->IsTypedArray()) return 1;
    for (uint32_t i = 0; i < v->ArrayLen() && count < limit; i++) {
      count += CountNodesBounded(v->ArrayGet(i), limit - count, depth + 1);
    }
  } else if (v->IsObject()) {
    auto* entries = v->ObjectEntries();
    for (uint32_t i = 0; i < v->ObjectLen() && count < limit; i++) {
      count += CountNodesBounded(entries[i].value, limit - count, depth + 1);
    }
  }
  return count;
}

void DigestNode(RedisModuleDigest* digest, const JsonValue* v, int depth = 0) {
  if (!v || depth > kMaxRdbDepth) return;
  RedisModule_DigestAddLongLong(digest, static_cast<long long>(v->Type()));
  switch (v->Type()) {
    case JsonType::kNull:
      break;
    case JsonType::kBool:
      RedisModule_DigestAddLongLong(digest, v->GetBool() ? 1 : 0);
      break;
    case JsonType::kInteger:
      RedisModule_DigestAddLongLong(digest, v->GetInteger());
      break;
    case JsonType::kNumber: {
      double d = v->GetNumber();
      if (d == 0.0) d = 0.0;  // canonicalize -0.0
      uint64_t bits = 0;
      std::memcpy(&bits, &d, sizeof(bits));
      RedisModule_DigestAddLongLong(digest, static_cast<long long>(bits));
      break;
    }
    case JsonType::kString: {
      auto sv = v->GetString();
      RedisModule_DigestAddLongLong(digest, static_cast<long long>(sv.size()));
      RedisModule_DigestAddStringBuffer(digest, sv.data(), sv.size());
      break;
    }
    case JsonType::kArray:
      RedisModule_DigestAddLongLong(digest, static_cast<long long>(v->ArrayLen()));
      if (v->IsIntArray()) {
        const int64_t* data = v->IntArrayData();
        for (uint32_t i = 0; i < v->ArrayLen(); i++) {
          RedisModule_DigestAddLongLong(digest, static_cast<long long>(JsonType::kInteger));
          RedisModule_DigestAddLongLong(digest, data[i]);
        }
      } else if (v->IsDoubleArray()) {
        const double* data = v->DoubleArrayData();
        for (uint32_t i = 0; i < v->ArrayLen(); i++) {
          RedisModule_DigestAddLongLong(digest, static_cast<long long>(JsonType::kNumber));
          double d = data[i];
          if (d == 0.0) d = 0.0;
          uint64_t bits = 0;
          std::memcpy(&bits, &d, sizeof(bits));
          RedisModule_DigestAddLongLong(digest, static_cast<long long>(bits));
        }
      } else {
        for (uint32_t i = 0; i < v->ArrayLen(); i++) {
          DigestNode(digest, v->ArrayGet(i), depth + 1);
        }
      }
      break;
    case JsonType::kObject: {
      uint32_t n = v->ObjectLen();
      RedisModule_DigestAddLongLong(digest, static_cast<long long>(n));
      auto* entries = v->ObjectEntries();
      std::vector<uint32_t> order(n);
      for (uint32_t i = 0; i < n; i++) order[i] = i;
      std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return entries[a].KeyView() < entries[b].KeyView();
      });
      for (uint32_t idx : order) {
        RedisModule_DigestAddLongLong(digest, static_cast<long long>(entries[idx].KeyLen()));
        RedisModule_DigestAddStringBuffer(digest, entries[idx].KeyData(), entries[idx].KeyLen());
        DigestNode(digest, entries[idx].value, depth + 1);
      }
      break;
    }
  }
}

}  // namespace

// ======================================================================
// Binary RDB format (encver 2) — Gemini native
// ======================================================================

enum : uint64_t {
  kTagNull        = 0,
  kTagFalse       = 1,
  kTagTrue        = 2,
  kTagInteger     = 3,
  kTagDouble      = 4,
  kTagString      = 5,
  kTagArray       = 6,
  kTagObject      = 7,
  kTagIntArray    = 8,
  kTagDoubleArray = 9,
};

static void BinarySave(RedisModuleIO* rdb, const JsonValue* v, int depth = 0) {
  if (!v || depth > kMaxRdbDepth) {
    RedisModule_SaveUnsigned(rdb, kTagNull);
    return;
  }
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
      if (v->IsIntArray()) {
        uint32_t len = v->ArrayLen();
        RedisModule_SaveUnsigned(rdb, kTagIntArray);
        RedisModule_SaveUnsigned(rdb, len);
        if (len > 0) {
          RedisModule_SaveStringBuffer(
              rdb, reinterpret_cast<const char*>(v->IntArrayData()),
              static_cast<size_t>(len) * sizeof(int64_t));
        }
      } else if (v->IsDoubleArray()) {
        uint32_t len = v->ArrayLen();
        RedisModule_SaveUnsigned(rdb, kTagDoubleArray);
        RedisModule_SaveUnsigned(rdb, len);
        if (len > 0) {
          RedisModule_SaveStringBuffer(
              rdb, reinterpret_cast<const char*>(v->DoubleArrayData()),
              static_cast<size_t>(len) * sizeof(double));
        }
      } else {
        RedisModule_SaveUnsigned(rdb, kTagArray);
        RedisModule_SaveUnsigned(rdb, v->ArrayLen());
        for (uint32_t i = 0; i < v->ArrayLen(); i++) {
          BinarySave(rdb, v->ArrayGet(i), depth + 1);
        }
      }
      break;
    case JsonType::kObject: {
      RedisModule_SaveUnsigned(rdb, kTagObject);
      RedisModule_SaveUnsigned(rdb, v->ObjectLen());
      auto* entries = v->ObjectEntries();
      for (uint32_t i = 0; i < v->ObjectLen(); i++) {
        RedisModule_SaveStringBuffer(rdb, entries[i].KeyData(), entries[i].KeyLen());
        BinarySave(rdb, entries[i].value, depth + 1);
      }
      break;
    }
  }
}

static JsonValue* BinaryLoad(RedisModuleIO* rdb, int depth = 0) {
  if (depth > kMaxRdbDepth) return nullptr;
  uint64_t tag = RedisModule_LoadUnsigned(rdb);
  if (HasIoError(rdb)) return nullptr;

  switch (tag) {
    case kTagNull:
      return JsonValue::CreateNull();
    case kTagFalse:
      return JsonValue::CreateBool(false);
    case kTagTrue:
      return JsonValue::CreateBool(true);
    case kTagInteger: {
      int64_t val = RedisModule_LoadSigned(rdb);
      if (HasIoError(rdb)) return nullptr;
      return JsonValue::CreateInteger(val);
    }
    case kTagDouble: {
      double val = RedisModule_LoadDouble(rdb);
      if (HasIoError(rdb) || !std::isfinite(val)) return nullptr;
      return JsonValue::CreateNumber(val);
    }
    case kTagString: {
      size_t len = 0;
      char* buf = RedisModule_LoadStringBuffer(rdb, &len);
      if (HasIoError(rdb) || !buf) {
        if (buf) RedisModule_Free(buf);
        return nullptr;
      }
      auto* v = JsonValue::CreateString(buf, static_cast<uint32_t>(len));
      RedisModule_Free(buf);
      return v;
    }
    case kTagIntArray: {
      uint64_t count = RedisModule_LoadUnsigned(rdb);
      if (HasIoError(rdb) || count > kMaxRdbCollectionLen) return nullptr;
      auto* arr = JsonValue::CreateArray();
      if (!arr) return nullptr;
      if (count == 0) return arr;
      size_t bytes = 0;
      char* buf = RedisModule_LoadStringBuffer(rdb, &bytes);
      if (HasIoError(rdb) || !buf || bytes != static_cast<size_t>(count) * sizeof(int64_t)) {
        if (buf) RedisModule_Free(buf);
        JsonValue::Destroy(arr);
        return nullptr;
      }
      if (!arr->ArrayReserve(static_cast<uint32_t>(count))) {
        RedisModule_Free(buf);
        JsonValue::Destroy(arr);
        return nullptr;
      }
      for (uint64_t i = 0; i < count; i++) {
        int64_t val = 0;
        std::memcpy(&val, buf + static_cast<size_t>(i) * sizeof(int64_t), sizeof(int64_t));
        if (!arr->ArrayAppendInt(val)) {
          RedisModule_Free(buf);
          JsonValue::Destroy(arr);
          return nullptr;
        }
      }
      RedisModule_Free(buf);
      return arr;
    }
    case kTagDoubleArray: {
      uint64_t count = RedisModule_LoadUnsigned(rdb);
      if (HasIoError(rdb) || count > kMaxRdbCollectionLen) return nullptr;
      auto* arr = JsonValue::CreateArray();
      if (!arr) return nullptr;
      if (count == 0) return arr;
      size_t bytes = 0;
      char* buf = RedisModule_LoadStringBuffer(rdb, &bytes);
      if (HasIoError(rdb) || !buf || bytes != static_cast<size_t>(count) * sizeof(double)) {
        if (buf) RedisModule_Free(buf);
        JsonValue::Destroy(arr);
        return nullptr;
      }
      if (!arr->ArrayReserve(static_cast<uint32_t>(count))) {
        RedisModule_Free(buf);
        JsonValue::Destroy(arr);
        return nullptr;
      }
      for (uint64_t i = 0; i < count; i++) {
        double val = 0.0;
        std::memcpy(&val, buf + static_cast<size_t>(i) * sizeof(double), sizeof(double));
        if (!std::isfinite(val) || !arr->ArrayAppendDouble(val)) {
          RedisModule_Free(buf);
          JsonValue::Destroy(arr);
          return nullptr;
        }
      }
      RedisModule_Free(buf);
      return arr;
    }
    case kTagArray: {
      uint64_t count = RedisModule_LoadUnsigned(rdb);
      if (HasIoError(rdb) || count > kMaxRdbCollectionLen) return nullptr;
      auto* arr = JsonValue::CreateArray();
      if (!arr) return nullptr;
      if (count > 0 && !arr->ArrayReserve(static_cast<uint32_t>(count))) {
        JsonValue::Destroy(arr);
        return nullptr;
      }
      for (uint64_t i = 0; i < count; i++) {
        auto* child = BinaryLoad(rdb, depth + 1);
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
      if (HasIoError(rdb) || count > kMaxRdbCollectionLen) return nullptr;
      auto* obj = JsonValue::CreateObject();
      if (!obj) return nullptr;
      if (count > 0 && !obj->ObjectReserve(static_cast<uint32_t>(count))) {
        JsonValue::Destroy(obj);
        return nullptr;
      }
      for (uint64_t i = 0; i < count; i++) {
        size_t key_len = 0;
        char* key = RedisModule_LoadStringBuffer(rdb, &key_len);
        if (HasIoError(rdb) || !key) {
          if (key) RedisModule_Free(key);
          JsonValue::Destroy(obj);
          return nullptr;
        }
        auto* val = BinaryLoad(rdb, depth + 1);
        if (!val) {
          RedisModule_Free(key);
          JsonValue::Destroy(obj);
          return nullptr;
        }
        bool ok = obj->ObjectSet(key, static_cast<uint32_t>(key_len), val);
        RedisModule_Free(key);
        if (!ok) {
          JsonValue::Destroy(val);
          JsonValue::Destroy(obj);
          return nullptr;
        }
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
  size_t len = 0;
  char* buf = RedisModule_LoadStringBuffer(rdb, &len);
  if (HasIoError(rdb) || !buf) {
    if (buf) RedisModule_Free(buf);
    return nullptr;
  }
  auto result = JsonParse(std::string_view{buf, len});
  RedisModule_Free(buf);
  return result.value;
}

// ======================================================================
// Public interface — dispatches based on encver and compat mode
// ======================================================================

void* RdbLoadJson(RedisModuleIO* rdb, int encver) {
  if (encver == 1 || encver == 3) return TextLoad(rdb);
  if (encver == 2) return BinaryLoad(rdb, 0);
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
  RedisModule_EmitAOF(aof, "JSON.SET", "scb", key, "$", json.data(), json.size());
}

void FreeJson(void* value) {
  JsonValue::Destroy(static_cast<JsonValue*>(value));
}

size_t JsonMemUsage(const void* value) {
  return static_cast<const JsonValue*>(value)->MemoryUsage();
}

void DigestJson(RedisModuleDigest* digest, void* value) {
  DigestNode(digest, static_cast<const JsonValue*>(value), 0);
  RedisModule_DigestEndSequence(digest);
}

void* CopyJson2(RedisModuleKeyOptCtx* /*ctx*/, const void* value) {
  if (!value) return nullptr;
  return static_cast<const JsonValue*>(value)->Clone();
}

size_t FreeEffortJson2(RedisModuleKeyOptCtx* /*ctx*/, const void* value) {
  if (!value) return 1;
  return CountNodesBounded(static_cast<const JsonValue*>(value), kLazyFreeEffortCutoff, 0);
}

static void* DefragBridge(void* ctx, void* ptr) {
  return RedisModule_DefragAlloc(static_cast<RedisModuleDefragCtx*>(ctx), ptr);
}

int DefragJson(RedisModuleDefragCtx* ctx, RedisModuleString* /*key*/,
               void** value) {
  if (!value || !*value || !RedisModule_DefragAlloc) return 0;
  auto* root = static_cast<JsonValue*>(*value);
  *value = JsonValue::DefragTree(root, DefragBridge, ctx);
  return 0;
}
