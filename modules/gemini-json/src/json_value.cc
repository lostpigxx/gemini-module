#include "json_value.h"
#include "rm_alloc.h"

#include <cmath>
#include <cstring>
#include <vector>

const char* JsonTypeName(JsonType t) {
  switch (t) {
    case JsonType::kNull:    return "null";
    case JsonType::kBool:    return "boolean";
    case JsonType::kInteger: return "integer";
    case JsonType::kNumber:  return "number";
    case JsonType::kString:  return "string";
    case JsonType::kArray:   return "array";
    case JsonType::kObject:  return "object";
  }
  return "unknown";
}

static JsonValue* AllocValue() {
  return static_cast<JsonValue*>(RMCalloc(1, sizeof(JsonValue)));
}

static inline uint32_t NextPow2(uint32_t v) {
  if (v <= 16) return 16;
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  return v + 1;
}

// Fast 64-bit mixing hash with process-unique seed (HashDoS resistant).
static uint64_t HashSeed() {
  static const uint64_t seed = []() -> uint64_t {
    uint64_t x = reinterpret_cast<uintptr_t>(&HashSeed) ^ 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
  }();
  return seed;
}

static uint32_t HashBytes(const char* data, uint32_t len) {
  uint64_t h = HashSeed() ^ (static_cast<uint64_t>(len) * 0x9e3779b97f4a7c15ULL);
  const auto* p = reinterpret_cast<const uint8_t*>(data);
  uint32_t i = 0;
  while (i + 8 <= len) {
    uint64_t k;
    std::memcpy(&k, p + i, 8);
    k *= 0xbf58476d1ce4e5b9ULL;
    k ^= k >> 31;
    h ^= k;
    h = (h * 0x94d049bb133111ebULL) + 0x52dce729ULL;
    i += 8;
  }
  uint64_t tail = 0;
  for (uint32_t rem = len - i, shift = 0; rem > 0; rem--, i++, shift += 8) {
    tail |= static_cast<uint64_t>(p[i]) << shift;
  }
  h ^= tail * 0xbf58476d1ce4e5b9ULL;
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  return static_cast<uint32_t>(h);
}

static inline uint8_t MakeTag(JsonType t, uint8_t flags = 0) {
  return static_cast<uint8_t>(static_cast<uint8_t>(t) | flags);
}

JsonValue* JsonValue::CreateNull() {
  auto* v = AllocValue();
  if (!v) return nullptr;
  v->gen_.tag = MakeTag(JsonType::kNull);
  return v;
}

JsonValue* JsonValue::CreateBool(bool b) {
  auto* v = AllocValue();
  if (!v) return nullptr;
  v->gen_.tag = MakeTag(JsonType::kBool);
  v->gen_.payload.bool_val = b;
  return v;
}

JsonValue* JsonValue::CreateInteger(int64_t n) {
  auto* v = AllocValue();
  if (!v) return nullptr;
  v->gen_.tag = MakeTag(JsonType::kInteger);
  v->gen_.payload.int_val = n;
  return v;
}

JsonValue* JsonValue::CreateNumber(double n) {
  auto* v = AllocValue();
  if (!v) return nullptr;
  v->gen_.tag = MakeTag(JsonType::kNumber);
  v->gen_.payload.num_val = n;
  return v;
}

JsonValue* JsonValue::CreateString(const char* data, uint32_t len) {
  auto* v = AllocValue();
  if (!v) return nullptr;
  if (len <= kInlineStrMaxLen) {
    v->sso_.tag = MakeTag(JsonType::kString, kFlagInlineStr);
    v->sso_.len = static_cast<uint8_t>(len);
    if (len > 0) std::memcpy(v->sso_.data, data, len);
    v->sso_.data[len] = '\0';
    return v;
  }
  v->gen_.tag = MakeTag(JsonType::kString);
  v->gen_.payload.str.data = static_cast<char*>(RMAlloc(len + 1));
  if (!v->gen_.payload.str.data) { RMFree(v); return nullptr; }
  std::memcpy(v->gen_.payload.str.data, data, len);
  v->gen_.payload.str.data[len] = '\0';
  v->gen_.payload.str.len = len;
  v->gen_.payload.str.cap = len;
  return v;
}

JsonValue* JsonValue::CreateArray() {
  auto* v = AllocValue();
  if (!v) return nullptr;
  v->gen_.tag = MakeTag(JsonType::kArray);
  return v;
}

JsonValue* JsonValue::CreateIntArrayFrom(const int64_t* data, uint32_t len) {
  auto* a = CreateArray();
  if (!a) return nullptr;
  if (len == 0) return a;
  if (!a->ArrayReserve(len)) {
    Destroy(a);
    return nullptr;
  }
  a->gen_.tag = MakeTag(JsonType::kArray, kFlagIntArray);
  std::memcpy(a->IntArrayDataMut(), data, static_cast<size_t>(len) * sizeof(int64_t));
  a->gen_.payload.arr.len = len;
  return a;
}

JsonValue* JsonValue::CreateDoubleArrayFrom(const double* data, uint32_t len) {
  auto* a = CreateArray();
  if (!a) return nullptr;
  if (len == 0) return a;
  if (!a->ArrayReserve(len)) {
    Destroy(a);
    return nullptr;
  }
  a->gen_.tag = MakeTag(JsonType::kArray, kFlagDoubleArray);
  std::memcpy(a->DoubleArrayDataMut(), data, static_cast<size_t>(len) * sizeof(double));
  a->gen_.payload.arr.len = len;
  return a;
}

JsonValue* JsonValue::CreateObject() {
  auto* v = AllocValue();
  if (!v) return nullptr;
  v->gen_.tag = MakeTag(JsonType::kObject);
  return v;
}

static inline void FreeEntryKey(JsonValue::ObjEntry& e) {
  if (!e.IsInlineKey()) {
    RMFree(e.heap_key);
    e.heap_key = nullptr;
  }
}

static inline bool InitEntryKey(JsonValue::ObjEntry& e, const char* key, uint32_t key_len) {
  if (key_len <= JsonValue::ObjEntry::kInlineKeyMaxLen) {
    e.key_len_flags = key_len | JsonValue::ObjEntry::kInlineKeyFlag;
    if (key_len > 0) std::memcpy(e.inline_key, key, key_len);
    e.inline_key[key_len] = '\0';
    return true;
  }
  auto* k = static_cast<char*>(RMAlloc(static_cast<size_t>(key_len) + 1));
  if (!k) return false;
  std::memcpy(k, key, key_len);
  k[key_len] = '\0';
  e.heap_key = k;
  e.key_len_flags = key_len;
  return true;
}

static inline void FreeLeaf(JsonValue* v) {
  if (v->IsString() && !v->IsInlineString()) {
    RMFree(const_cast<char*>(v->GetString().data()));
  }
  RMFree(v);
}

void JsonValue::Destroy(JsonValue* v) {
  if (!v) return;

  JsonType root_type = v->Type();
  if (root_type != JsonType::kArray && root_type != JsonType::kObject) {
    FreeLeaf(v);
    return;
  }

  std::vector<JsonValue*> stack;
  stack.push_back(v);

  while (!stack.empty()) {
    JsonValue* cur = stack.back();
    stack.pop_back();
    if (!cur) continue;

    switch (cur->Type()) {
      case JsonType::kString:
        if (!cur->IsInlineString()) RMFree(cur->gen_.payload.str.data);
        break;
      case JsonType::kArray: {
        auto& arr = cur->gen_.payload.arr;
        if (!cur->IsTypedArray()) {
          for (uint32_t i = 0; i < arr.len; i++) {
            JsonValue* child = arr.items[i];
            if (child) {
              if (child->IsArray() || child->IsObject()) {
                stack.push_back(child);
              } else {
                FreeLeaf(child);
              }
            }
          }
        }
        RMFree(arr.items);
        break;
      }
      case JsonType::kObject: {
        ObjectData* od = cur->gen_.payload.obj;
        if (od) {
          for (uint32_t i = 0; i < od->entries_len; i++) {
            if (!od->entries[i].IsLive()) continue;
            FreeEntryKey(od->entries[i]);
            JsonValue* child = od->entries[i].value;
            if (child) {
              if (child->IsArray() || child->IsObject()) {
                stack.push_back(child);
              } else {
                FreeLeaf(child);
              }
            }
          }
          RMFree(od->entries);
          RMFree(od->hash_idx);
          RMFree(od);
        }
        break;
      }
      default:
        break;
    }
    RMFree(cur);
  }
}

static JsonValue* CloneDepth(const JsonValue* v, int depth) {
  if (!v || depth > kMaxJsonDepth) return nullptr;
  switch (v->Type()) {
    case JsonType::kNull:
      return JsonValue::CreateNull();
    case JsonType::kBool:
      return JsonValue::CreateBool(v->GetBool());
    case JsonType::kInteger:
      return JsonValue::CreateInteger(v->GetInteger());
    case JsonType::kNumber:
      return JsonValue::CreateNumber(v->GetNumber());
    case JsonType::kString: {
      auto sv = v->GetString();
      return JsonValue::CreateString(sv.data(), static_cast<uint32_t>(sv.size()));
    }
    case JsonType::kArray: {
      uint32_t n = v->ArrayLen();
      if (v->IsIntArray()) {
        return JsonValue::CreateIntArrayFrom(v->IntArrayData(), n);
      }
      if (v->IsDoubleArray()) {
        return JsonValue::CreateDoubleArrayFrom(v->DoubleArrayData(), n);
      }
      auto* a = JsonValue::CreateArray();
      if (!a) return nullptr;
      if (n == 0) return a;
      if (!a->ArrayReserve(n)) {
        JsonValue::Destroy(a);
        return nullptr;
      }
      for (uint32_t i = 0; i < n; i++) {
        auto* child = CloneDepth(v->ArrayGet(i), depth + 1);
        if (!child || !a->ArrayAppend(child)) {
          JsonValue::Destroy(child);
          JsonValue::Destroy(a);
          return nullptr;
        }
      }
      return a;
    }
    case JsonType::kObject: {
      auto* o = JsonValue::CreateObject();
      if (!o) return nullptr;
      uint32_t n = v->ObjectLen();
      if (n > 0 && !o->ObjectReserve(n)) {
        JsonValue::Destroy(o);
        return nullptr;
      }
      auto* entries = v->ObjectEntries();
      for (uint32_t i = 0; i < n; i++) {
        auto* child = CloneDepth(entries[i].value, depth + 1);
        if (!child || !o->ObjectSet(entries[i].KeyData(), entries[i].KeyLen(), child)) {
          JsonValue::Destroy(child);
          JsonValue::Destroy(o);
          return nullptr;
        }
      }
      return o;
    }
  }
  return nullptr;
}

JsonValue* JsonValue::Clone() const {
  return CloneDepth(this, 0);
}

double JsonValue::GetNumber() const {
  if (Type() == JsonType::kInteger) return static_cast<double>(gen_.payload.int_val);
  return gen_.payload.num_val;
}

bool JsonValue::StringAppend(std::string_view suffix) {
  if (!IsString()) return false;
  if (suffix.empty()) return true;

  if (IsInlineString()) {
    uint32_t cur_len = sso_.len;
    if (suffix.size() > static_cast<size_t>(UINT32_MAX - cur_len)) return false;
    uint32_t new_len = cur_len + static_cast<uint32_t>(suffix.size());
    if (new_len <= kInlineStrMaxLen) {
      std::memcpy(sso_.data + cur_len, suffix.data(), suffix.size());
      sso_.data[new_len] = '\0';
      sso_.len = static_cast<uint8_t>(new_len);
      return true;
    }
    uint32_t new_cap = new_len;
    if (new_cap < 64) new_cap = 64;
    auto* buf = static_cast<char*>(RMAlloc(static_cast<size_t>(new_cap) + 1));
    if (!buf) return false;
    if (cur_len > 0) std::memcpy(buf, sso_.data, cur_len);
    std::memcpy(buf + cur_len, suffix.data(), suffix.size());
    buf[new_len] = '\0';
    gen_.tag = MakeTag(JsonType::kString);
    gen_.payload.str.data = buf;
    gen_.payload.str.len = new_len;
    gen_.payload.str.cap = new_cap;
    return true;
  }

  auto& s = gen_.payload.str;
  if (suffix.size() > static_cast<size_t>(UINT32_MAX - s.len)) return false;
  uint32_t new_len = s.len + static_cast<uint32_t>(suffix.size());
  if (new_len > s.cap) {
    uint64_t grown = static_cast<uint64_t>(s.cap) * 2;
    if (grown < new_len) grown = new_len;
    if (grown > UINT32_MAX) grown = new_len;
    auto* nd = static_cast<char*>(RMRealloc(s.data, static_cast<size_t>(grown) + 1));
    if (!nd) return false;
    s.data = nd;
    s.cap = static_cast<uint32_t>(grown);
  }
  std::memcpy(s.data + s.len, suffix.data(), suffix.size());
  s.len = new_len;
  s.data[new_len] = '\0';
  return true;
}

// --- Array operations ---

bool JsonValue::ArrayReserve(uint32_t new_cap) {
  auto& arr = gen_.payload.arr;
  if (new_cap <= arr.cap) return true;
  auto* new_items = static_cast<JsonValue**>(
      RMRealloc(arr.items, static_cast<size_t>(new_cap) * sizeof(JsonValue*)));
  if (!new_items) return false;
  arr.items = new_items;
  arr.cap = new_cap;
  return true;
}

bool JsonValue::ArrayGrow() {
  auto& arr = gen_.payload.arr;
  uint32_t new_cap = arr.cap == 0 ? 4 : arr.cap * 2;
  if (new_cap <= arr.cap) return false;
  return ArrayReserve(new_cap);
}

void JsonValue::ArrayShrinkIfNeeded() {
  auto& arr = gen_.payload.arr;
  if (arr.cap <= 64) return;
  if (arr.len == 0) {
    RMFree(arr.items);
    arr.items = nullptr;
    arr.cap = 0;
    return;
  }
  if (arr.len * 4 < arr.cap) {
    uint32_t new_cap = arr.len * 2;
    if (new_cap < 16) new_cap = 16;
    auto* shrunk = static_cast<JsonValue**>(
        RMRealloc(arr.items, static_cast<size_t>(new_cap) * sizeof(JsonValue*)));
    if (shrunk) {
      arr.items = shrunk;
      arr.cap = new_cap;
    }
  }
}

static inline bool IntFitsExactDouble(int64_t v) {
  constexpr int64_t kMaxExact = 1LL << 53;
  return v >= -kMaxExact && v <= kMaxExact;
}

bool JsonValue::EnsureBoxedArray() const {
  if (!IsTypedArray()) return true;
  auto* self = const_cast<JsonValue*>(this);
  auto& arr = self->gen_.payload.arr;
  uint32_t n = arr.len;
  if (n == 0) {
    self->gen_.tag = MakeTag(JsonType::kArray);
    return true;
  }

  uint32_t cap = arr.cap < n ? n : arr.cap;
  auto** boxed = static_cast<JsonValue**>(RMAlloc(static_cast<size_t>(cap) * sizeof(JsonValue*)));
  if (!boxed) return false;

  if (IsIntArray()) {
    const int64_t* src = IntArrayData();
    for (uint32_t i = 0; i < n; i++) {
      boxed[i] = CreateInteger(src[i]);
      if (!boxed[i]) {
        for (uint32_t j = 0; j < i; j++) Destroy(boxed[j]);
        RMFree(boxed);
        return false;
      }
    }
  } else {
    const double* src = DoubleArrayData();
    for (uint32_t i = 0; i < n; i++) {
      boxed[i] = CreateNumber(src[i]);
      if (!boxed[i]) {
        for (uint32_t j = 0; j < i; j++) Destroy(boxed[j]);
        RMFree(boxed);
        return false;
      }
    }
  }

  RMFree(arr.items);
  arr.items = boxed;
  arr.cap = cap;
  self->gen_.tag = MakeTag(JsonType::kArray);
  return true;
}

bool JsonValue::ArrayAppendInt(int64_t val) {
  auto& arr = gen_.payload.arr;
  if (arr.len == 0 && !IsTypedArray()) {
    gen_.tag = MakeTag(JsonType::kArray, kFlagIntArray);
  }
  if (IsIntArray()) {
    if (arr.len == arr.cap && !ArrayGrow()) return false;
    IntArrayDataMut()[arr.len++] = val;
    return true;
  }
  if (IsDoubleArray()) {
    if (IntFitsExactDouble(val)) {
      if (arr.len == arr.cap && !ArrayGrow()) return false;
      DoubleArrayDataMut()[arr.len++] = static_cast<double>(val);
      return true;
    }
    if (!EnsureBoxedArray()) return false;
  }
  auto* node = CreateInteger(val);
  if (!node) return false;
  if (!ArrayAppend(node)) {
    Destroy(node);
    return false;
  }
  return true;
}

bool JsonValue::ArrayAppendDouble(double val) {
  auto& arr = gen_.payload.arr;
  if (arr.len == 0 && !IsTypedArray()) {
    gen_.tag = MakeTag(JsonType::kArray, kFlagDoubleArray);
  }
  if (IsDoubleArray()) {
    if (arr.len == arr.cap && !ArrayGrow()) return false;
    DoubleArrayDataMut()[arr.len++] = val;
    return true;
  }
  if (IsIntArray()) {
    // Promote int64[] -> double[] in place if all existing ints are exact in IEEE-754 double
    const int64_t* src = IntArrayData();
    bool all_exact = true;
    for (uint32_t i = 0; i < arr.len; i++) {
      if (!IntFitsExactDouble(src[i])) {
        all_exact = false;
        break;
      }
    }
    if (all_exact) {
      if (arr.len == arr.cap && !ArrayGrow()) return false;
      int64_t* isrc = IntArrayDataMut();
      double* ddst = DoubleArrayDataMut();
      for (uint32_t i = 0; i < arr.len; i++) {
        ddst[i] = static_cast<double>(isrc[i]);
      }
      gen_.tag = MakeTag(JsonType::kArray, kFlagDoubleArray);
      ddst[arr.len++] = val;
      return true;
    }
    if (!EnsureBoxedArray()) return false;
  }
  auto* node = CreateNumber(val);
  if (!node) return false;
  if (!ArrayAppend(node)) {
    Destroy(node);
    return false;
  }
  return true;
}

bool JsonValue::ArrayAppend(JsonValue* child) {
  auto& arr = gen_.payload.arr;
  if (child && (arr.len == 0 || IsTypedArray())) {
    if (child->Type() == JsonType::kInteger) {
      int64_t v = child->GetInteger();
      if (ArrayAppendInt(v)) {
        Destroy(child);
        return true;
      }
      return false;
    }
    if (child->Type() == JsonType::kNumber) {
      double d = child->GetNumber();
      if (ArrayAppendDouble(d)) {
        Destroy(child);
        return true;
      }
      return false;
    }
    if (IsTypedArray() && !EnsureBoxedArray()) return false;
  }
  if (arr.len == arr.cap && !ArrayGrow()) return false;
  arr.items[arr.len++] = child;
  return true;
}

bool JsonValue::ArrayInsert(uint32_t index, JsonValue* child) {
  auto& arr = gen_.payload.arr;
  if (index > arr.len) return false;
  if (IsTypedArray()) {
    if (child && IsIntArray() && child->Type() == JsonType::kInteger) {
      if (arr.len == arr.cap && !ArrayGrow()) return false;
      int64_t* data = IntArrayDataMut();
      if (index < arr.len) {
        std::memmove(&data[index + 1], &data[index],
                     static_cast<size_t>(arr.len - index) * sizeof(int64_t));
      }
      data[index] = child->GetInteger();
      arr.len++;
      Destroy(child);
      return true;
    }
    if (child && IsDoubleArray() && child->Type() == JsonType::kNumber) {
      if (arr.len == arr.cap && !ArrayGrow()) return false;
      double* data = DoubleArrayDataMut();
      if (index < arr.len) {
        std::memmove(&data[index + 1], &data[index],
                     static_cast<size_t>(arr.len - index) * sizeof(double));
      }
      data[index] = child->GetNumber();
      arr.len++;
      Destroy(child);
      return true;
    }
    if (!EnsureBoxedArray()) return false;
  }
  if (arr.len == arr.cap && !ArrayGrow()) return false;
  if (index < arr.len) {
    std::memmove(&arr.items[index + 1], &arr.items[index],
                 static_cast<size_t>(arr.len - index) * sizeof(JsonValue*));
  }
  arr.items[index] = child;
  arr.len++;
  return true;
}

JsonValue* JsonValue::ArrayPop(int32_t index) {
  auto& arr = gen_.payload.arr;
  if (arr.len == 0) return nullptr;
  int32_t resolved = index;
  if (resolved < 0) resolved += static_cast<int32_t>(arr.len);
  if (resolved < 0 || static_cast<uint32_t>(resolved) >= arr.len) return nullptr;
  auto idx = static_cast<uint32_t>(resolved);

  if (IsIntArray()) {
    int64_t* data = IntArrayDataMut();
    int64_t val = data[idx];
    JsonValue* popped = CreateInteger(val);
    if (!popped) return nullptr;
    if (idx < arr.len - 1) {
      std::memmove(&data[idx], &data[idx + 1],
                   static_cast<size_t>(arr.len - idx - 1) * sizeof(int64_t));
    }
    arr.len--;
    ArrayShrinkIfNeeded();
    return popped;
  }
  if (IsDoubleArray()) {
    double* data = DoubleArrayDataMut();
    double val = data[idx];
    JsonValue* popped = CreateNumber(val);
    if (!popped) return nullptr;
    if (idx < arr.len - 1) {
      std::memmove(&data[idx], &data[idx + 1],
                   static_cast<size_t>(arr.len - idx - 1) * sizeof(double));
    }
    arr.len--;
    ArrayShrinkIfNeeded();
    return popped;
  }

  JsonValue* popped = arr.items[idx];
  if (idx < arr.len - 1) {
    std::memmove(&arr.items[idx], &arr.items[idx + 1],
                 static_cast<size_t>(arr.len - idx - 1) * sizeof(JsonValue*));
  }
  arr.len--;
  ArrayShrinkIfNeeded();
  return popped;
}

void JsonValue::ArrayTrim(uint32_t start, uint32_t stop) {
  auto& arr = gen_.payload.arr;
  if (IsTypedArray()) {
    if (start >= arr.len || start > stop) {
      arr.len = 0;
      gen_.tag = MakeTag(JsonType::kArray);
      ArrayShrinkIfNeeded();
      return;
    }
    if (stop >= arr.len) stop = arr.len - 1;
    uint32_t new_len = stop - start + 1;
    if (start > 0) {
      std::memmove(arr.items, reinterpret_cast<char*>(arr.items) + static_cast<size_t>(start) * 8,
                   static_cast<size_t>(new_len) * 8);
    }
    arr.len = new_len;
    ArrayShrinkIfNeeded();
    return;
  }

  if (start >= arr.len || start > stop) {
    for (uint32_t i = 0; i < arr.len; i++) Destroy(arr.items[i]);
    arr.len = 0;
    ArrayShrinkIfNeeded();
    return;
  }
  if (stop >= arr.len) stop = arr.len - 1;

  for (uint32_t i = 0; i < start; i++) Destroy(arr.items[i]);
  for (uint32_t i = stop + 1; i < arr.len; i++) Destroy(arr.items[i]);

  uint32_t new_len = stop - start + 1;
  if (start > 0) {
    std::memmove(&arr.items[0], &arr.items[start],
                 static_cast<size_t>(new_len) * sizeof(JsonValue*));
  }
  arr.len = new_len;
  ArrayShrinkIfNeeded();
}

// --- Hash-accelerated object operations (power-of-2 mask + cached hash + tombstones) ---

JsonValue::ObjectData* JsonValue::EnsureObjectData() {
  if (!gen_.payload.obj) {
    gen_.payload.obj = static_cast<ObjectData*>(RMCalloc(1, sizeof(ObjectData)));
  }
  return gen_.payload.obj;
}

void JsonValue::ObjHashClear() const {
  ObjectData* od = gen_.payload.obj;
  if (!od) return;
  RMFree(od->hash_idx);
  od->hash_idx = nullptr;
  od->hash_cap = 0;
}

void JsonValue::ObjHashInsertSlot(uint32_t slot, uint32_t hash) const {
  ObjectData* od = gen_.payload.obj;
  uint32_t mask = od->hash_cap - 1;
  uint32_t h = hash & mask;
  while (od->hash_idx[h] != -1) {
    h = (h + 1) & mask;
  }
  od->hash_idx[h] = static_cast<int32_t>(slot);
}

// Backward-shift deletion in open-addressed linear probing table (O(1) average).
void JsonValue::ObjHashRemoveSlot(uint32_t slot, uint32_t hash) const {
  ObjectData* od = gen_.payload.obj;
  if (!od || !od->hash_idx) return;
  uint32_t mask = od->hash_cap - 1;
  uint32_t h = hash & mask;
  while (od->hash_idx[h] != -1) {
    if (static_cast<uint32_t>(od->hash_idx[h]) == slot) {
      // Shift subsequent elements back to fill the hole
      uint32_t hole = h;
      uint32_t cur = (hole + 1) & mask;
      while (od->hash_idx[cur] != -1) {
        uint32_t cur_slot = static_cast<uint32_t>(od->hash_idx[cur]);
        uint32_t ideal = od->entries[cur_slot].key_hash & mask;
        // Check if `hole` lies cyclically in [ideal, cur)
        uint32_t dist_cur = (cur - ideal) & mask;
        uint32_t dist_hole = (hole - ideal) & mask;
        if (dist_hole < dist_cur) {
          od->hash_idx[hole] = od->hash_idx[cur];
          hole = cur;
        }
        cur = (cur + 1) & mask;
      }
      od->hash_idx[hole] = -1;
      return;
    }
    h = (h + 1) & mask;
  }
}

void JsonValue::ObjHashRebuild(uint32_t target_cap) const {
  ObjectData* od = gen_.payload.obj;
  if (!od) return;

  uint32_t new_cap = NextPow2(target_cap);
  if (od->hash_idx && od->hash_cap == new_cap) {
    std::memset(od->hash_idx, 0xFF, static_cast<size_t>(new_cap) * sizeof(int32_t));
  } else {
    auto* new_tbl = static_cast<int32_t*>(
        RMAlloc(static_cast<size_t>(new_cap) * sizeof(int32_t)));
    if (!new_tbl) {
      ObjHashClear();
      return;
    }
    RMFree(od->hash_idx);
    od->hash_idx = new_tbl;
    od->hash_cap = new_cap;
    std::memset(od->hash_idx, 0xFF, static_cast<size_t>(new_cap) * sizeof(int32_t));
  }

  uint32_t mask = od->hash_cap - 1;
  for (uint32_t i = 0; i < od->entries_len; i++) {
    if (!od->entries[i].IsLive()) continue;
    uint32_t h = od->entries[i].key_hash & mask;
    while (od->hash_idx[h] != -1) {
      h = (h + 1) & mask;
    }
    od->hash_idx[h] = static_cast<int32_t>(i);
  }
}

void JsonValue::ObjectCompact() const {
  ObjectData* od = gen_.payload.obj;
  if (!od || od->len == od->entries_len) return;

  uint32_t write = 0;
  for (uint32_t read = 0; read < od->entries_len; read++) {
    if (od->entries[read].IsLive()) {
      if (write != read) {
        od->entries[write] = od->entries[read];
      }
      write++;
    }
  }
  od->entries_len = write;

  if (od->len > kObjHashThreshold) {
    ObjHashRebuild(od->len * 2);
  } else {
    ObjHashClear();
  }
}

int32_t JsonValue::ObjHashProbe(const char* key, uint32_t key_len, uint32_t hash) const {
  ObjectData* od = gen_.payload.obj;
  if (!od || !od->hash_idx) return -1;

  uint32_t mask = od->hash_cap - 1;
  uint32_t h = hash & mask;
  while (od->hash_idx[h] != -1) {
    auto slot = static_cast<uint32_t>(od->hash_idx[h]);
    const ObjEntry& e = od->entries[slot];
    if (e.IsLive() && e.key_hash == hash && e.KeyLen() == key_len &&
        (key_len == 0 || std::memcmp(e.KeyData(), key, key_len) == 0)) {
      return od->hash_idx[h];
    }
    h = (h + 1) & mask;
  }
  return -1;
}

JsonValue::ObjEntry* JsonValue::ObjectEntries() const {
  ObjectData* od = gen_.payload.obj;
  if (!od) return nullptr;
  if (od->len != od->entries_len) {
    ObjectCompact();
  }
  return od->entries;
}

bool JsonValue::ObjectReserve(uint32_t new_cap) {
  ObjectData* od = EnsureObjectData();
  if (!od) return false;
  if (new_cap <= od->cap) return true;
  auto* new_entries = static_cast<ObjEntry*>(
      RMRealloc(od->entries, static_cast<size_t>(new_cap) * sizeof(ObjEntry)));
  if (!new_entries) return false;
  od->entries = new_entries;
  od->cap = new_cap;
  if (new_cap > kObjHashThreshold && (!od->hash_idx || od->hash_cap < new_cap * 2)) {
    ObjHashRebuild(new_cap * 2);
  }
  return true;
}

bool JsonValue::ObjectGrow(uint32_t min_cap) {
  ObjectData* od = EnsureObjectData();
  if (!od) return false;
  uint32_t new_cap = od->cap == 0 ? 4 : od->cap * 2;
  if (new_cap < min_cap) new_cap = min_cap;
  if (new_cap <= od->cap) return false;
  auto* new_entries = static_cast<ObjEntry*>(
      RMRealloc(od->entries, static_cast<size_t>(new_cap) * sizeof(ObjEntry)));
  if (!new_entries) return false;
  od->entries = new_entries;
  od->cap = new_cap;
  return true;
}

JsonValue* JsonValue::ObjectGet(std::string_view key) const {
  ObjectData* od = gen_.payload.obj;
  if (!od || od->len == 0) return nullptr;

  if (od->hash_idx) {
    uint32_t h = HashBytes(key.data(), static_cast<uint32_t>(key.size()));
    int32_t slot = ObjHashProbe(key.data(), static_cast<uint32_t>(key.size()), h);
    if (slot >= 0) return od->entries[slot].value;
    return nullptr;
  }

  for (uint32_t i = 0; i < od->entries_len; i++) {
    const ObjEntry& e = od->entries[i];
    if (e.IsLive() && key.size() == e.KeyLen() &&
        (key.size() == 0 || std::memcmp(key.data(), e.KeyData(), key.size()) == 0)) {
      return e.value;
    }
  }
  return nullptr;
}

bool JsonValue::ObjectSet(const char* key, uint32_t key_len, JsonValue* child) {
  ObjectData* od = EnsureObjectData();
  if (!od) return false;

  uint32_t hash = 0;
  bool hashed = false;

  // Check for existing key
  if (od->hash_idx) {
    hash = HashBytes(key, key_len);
    hashed = true;
    int32_t slot = ObjHashProbe(key, key_len, hash);
    if (slot >= 0) {
      Destroy(od->entries[slot].value);
      od->entries[slot].value = child;
      return true;
    }
  } else {
    for (uint32_t i = 0; i < od->entries_len; i++) {
      ObjEntry& e = od->entries[i];
      if (e.IsLive() && key_len == e.KeyLen() &&
          (key_len == 0 || std::memcmp(key, e.KeyData(), key_len) == 0)) {
        Destroy(e.value);
        e.value = child;
        return true;
      }
    }
  }

  if (od->entries_len == od->cap) {
    if (od->entries_len > od->len && (od->entries_len - od->len) * 2 >= od->cap) {
      ObjectCompact();
    } else if (!ObjectGrow(od->cap + 1)) {
      return false;
    }
  }

  uint32_t slot = od->entries_len;
  ObjEntry& entry = od->entries[slot];
  if (!InitEntryKey(entry, key, key_len)) return false;

  if (!hashed) hash = HashBytes(entry.KeyData(), key_len);
  entry.key_hash = hash;
  entry.value = child;
  od->entries_len++;
  od->len++;

  if (od->hash_idx) {
    if (od->len * 2 > od->hash_cap) {
      ObjHashRebuild(od->len * 2);
    } else {
      ObjHashInsertSlot(slot, hash);
    }
  } else if (od->len > kObjHashThreshold) {
    ObjHashRebuild(od->len * 2);
  }

  return true;
}

bool JsonValue::ObjectDelete(std::string_view key) {
  ObjectData* od = gen_.payload.obj;
  if (!od || od->len == 0) return false;

  int32_t found = -1;
  uint32_t hash = 0;

  if (od->hash_idx) {
    hash = HashBytes(key.data(), static_cast<uint32_t>(key.size()));
    found = ObjHashProbe(key.data(), static_cast<uint32_t>(key.size()), hash);
  } else {
    for (uint32_t i = 0; i < od->entries_len; i++) {
      const ObjEntry& e = od->entries[i];
      if (e.IsLive() && key.size() == e.KeyLen() &&
          (key.size() == 0 || std::memcmp(key.data(), e.KeyData(), key.size()) == 0)) {
        found = static_cast<int32_t>(i);
        break;
      }
    }
  }

  if (found < 0) return false;

  auto slot = static_cast<uint32_t>(found);
  FreeEntryKey(od->entries[slot]);
  Destroy(od->entries[slot].value);
  od->entries[slot].value = nullptr;
  od->entries[slot].key_len_flags = 0;
  od->len--;

  if (od->hash_idx) {
    ObjHashRemoveSlot(slot, hash);
  }

  // Compact when tombstones dominate or object shrinks below hash threshold
  if (od->len == 0) {
    od->entries_len = 0;
    ObjHashClear();
    if (od->cap > 64) {
      RMFree(od->entries);
      od->entries = nullptr;
      od->cap = 0;
    }
  } else if (od->len <= kObjHashThreshold && od->hash_idx) {
    ObjectCompact();
  } else if (od->entries_len > 64 && od->len * 4 < od->entries_len) {
    ObjectCompact();
    if (od->cap > 64 && od->len * 4 < od->cap) {
      uint32_t new_cap = od->len * 2;
      if (new_cap < 16) new_cap = 16;
      auto* shrunk = static_cast<ObjEntry*>(
          RMRealloc(od->entries, static_cast<size_t>(new_cap) * sizeof(ObjEntry)));
      if (shrunk) {
        od->entries = shrunk;
        od->cap = new_cap;
      }
    }
  }

  return true;
}

void JsonValue::Clear() {
  switch (Type()) {
    case JsonType::kArray: {
      auto& arr = gen_.payload.arr;
      if (!IsTypedArray()) {
        for (uint32_t i = 0; i < arr.len; i++) Destroy(arr.items[i]);
      } else {
        gen_.tag = MakeTag(JsonType::kArray);
      }
      arr.len = 0;
      ArrayShrinkIfNeeded();
      break;
    }
    case JsonType::kObject: {
      ObjectData* od = gen_.payload.obj;
      if (od) {
        for (uint32_t i = 0; i < od->entries_len; i++) {
          if (!od->entries[i].IsLive()) continue;
          FreeEntryKey(od->entries[i]);
          Destroy(od->entries[i].value);
        }
        od->len = 0;
        od->entries_len = 0;
        ObjHashClear();
        if (od->cap > 64) {
          RMFree(od->entries);
          od->entries = nullptr;
          od->cap = 0;
        }
      }
      break;
    }
    case JsonType::kInteger:
      gen_.payload.int_val = 0;
      break;
    case JsonType::kNumber:
      gen_.payload.num_val = 0.0;
      break;
    default:
      break;
  }
}

size_t JsonValue::MemoryUsage() const {
  size_t total = 0;
  std::vector<const JsonValue*> stack;
  stack.push_back(this);
  while (!stack.empty()) {
    const JsonValue* cur = stack.back();
    stack.pop_back();
    if (!cur) continue;
    total += sizeof(JsonValue);
    switch (cur->Type()) {
      case JsonType::kString:
        if (!cur->IsInlineString()) {
          total += static_cast<size_t>(cur->gen_.payload.str.cap) + 1;
        }
        break;
      case JsonType::kArray: {
        const auto& arr = cur->gen_.payload.arr;
        if (cur->IsTypedArray()) {
          total += static_cast<size_t>(arr.cap) * sizeof(int64_t);
        } else {
          total += static_cast<size_t>(arr.cap) * sizeof(JsonValue*);
          for (uint32_t i = 0; i < arr.len; i++) {
            stack.push_back(arr.items[i]);
          }
        }
        break;
      }
      case JsonType::kObject: {
        const ObjectData* od = cur->gen_.payload.obj;
        if (od) {
          total += sizeof(ObjectData);
          total += static_cast<size_t>(od->cap) * sizeof(ObjEntry);
          total += static_cast<size_t>(od->hash_cap) * sizeof(int32_t);
          for (uint32_t i = 0; i < od->entries_len; i++) {
            if (!od->entries[i].IsLive()) continue;
            if (!od->entries[i].IsInlineKey()) {
              total += static_cast<size_t>(od->entries[i].KeyLen()) + 1;
            }
            stack.push_back(od->entries[i].value);
          }
        }
        break;
      }
      default:
        break;
    }
  }
  return total;
}

static bool NumbersEqual(double a, double b) {
  if (std::isnan(a) && std::isnan(b)) return true;
  return a == b;
}

static bool IntEqualsDouble(int64_t i, double d) {
  if (!std::isfinite(d)) return false;
  if (std::trunc(d) != d) return false;
  constexpr double kMinInt64D = -9223372036854775808.0;
  constexpr double kMaxInt64ExclusiveD = 9223372036854775808.0;
  if (d < kMinInt64D || d >= kMaxInt64ExclusiveD) return false;
  return static_cast<int64_t>(d) == i;
}

static bool DeepEqualDepth(const JsonValue* a, const JsonValue* b, int depth) {
  if (!a || !b || depth > kMaxJsonDepth) return false;
  if (a == b) return true;

  if (a->Type() == JsonType::kInteger && b->Type() == JsonType::kInteger) {
    return a->GetInteger() == b->GetInteger();
  }
  if (a->Type() == JsonType::kInteger && b->Type() == JsonType::kNumber) {
    return IntEqualsDouble(a->GetInteger(), b->GetNumber());
  }
  if (a->Type() == JsonType::kNumber && b->Type() == JsonType::kInteger) {
    return IntEqualsDouble(b->GetInteger(), a->GetNumber());
  }

  if (a->Type() != b->Type()) return false;

  switch (a->Type()) {
    case JsonType::kNull:
      return true;
    case JsonType::kBool:
      return a->GetBool() == b->GetBool();
    case JsonType::kInteger:
      return a->GetInteger() == b->GetInteger();
    case JsonType::kNumber:
      return NumbersEqual(a->GetNumber(), b->GetNumber());
    case JsonType::kString:
      return a->GetString() == b->GetString();
    case JsonType::kArray: {
      uint32_t n = a->ArrayLen();
      if (n != b->ArrayLen()) return false;
      if (a->IsIntArray() && b->IsIntArray()) {
        return n == 0 || std::memcmp(a->IntArrayData(), b->IntArrayData(),
                                     static_cast<size_t>(n) * sizeof(int64_t)) == 0;
      }
      if (a->IsDoubleArray() && b->IsDoubleArray()) {
        const double* da = a->DoubleArrayData();
        const double* db = b->DoubleArrayData();
        for (uint32_t i = 0; i < n; i++) {
          if (!NumbersEqual(da[i], db[i])) return false;
        }
        return true;
      }
      if (a->IsTypedArray() || b->IsTypedArray()) {
        for (uint32_t i = 0; i < n; i++) {
          bool a_int = a->IsIntArray();
          bool a_dbl = a->IsDoubleArray();
          bool b_int = b->IsIntArray();
          bool b_dbl = b->IsDoubleArray();
          if (a_int && b_dbl) {
            if (!IntEqualsDouble(a->IntArrayData()[i], b->DoubleArrayData()[i])) return false;
          } else if (a_dbl && b_int) {
            if (!IntEqualsDouble(b->IntArrayData()[i], a->DoubleArrayData()[i])) return false;
          } else if (a_int) {
            const JsonValue* bv = b->ArrayGet(i);
            if (!bv) return false;
            if (bv->Type() == JsonType::kInteger) {
              if (a->IntArrayData()[i] != bv->GetInteger()) return false;
            } else if (bv->Type() == JsonType::kNumber) {
              if (!IntEqualsDouble(a->IntArrayData()[i], bv->GetNumber())) return false;
            } else {
              return false;
            }
          } else if (a_dbl) {
            const JsonValue* bv = b->ArrayGet(i);
            if (!bv) return false;
            if (bv->Type() == JsonType::kNumber) {
              if (!NumbersEqual(a->DoubleArrayData()[i], bv->GetNumber())) return false;
            } else if (bv->Type() == JsonType::kInteger) {
              if (!IntEqualsDouble(bv->GetInteger(), a->DoubleArrayData()[i])) return false;
            } else {
              return false;
            }
          } else if (b_int) {
            const JsonValue* av = a->ArrayGet(i);
            if (!av) return false;
            if (av->Type() == JsonType::kInteger) {
              if (av->GetInteger() != b->IntArrayData()[i]) return false;
            } else if (av->Type() == JsonType::kNumber) {
              if (!IntEqualsDouble(b->IntArrayData()[i], av->GetNumber())) return false;
            } else {
              return false;
            }
          } else {
            const JsonValue* av = a->ArrayGet(i);
            if (!av) return false;
            if (av->Type() == JsonType::kNumber) {
              if (!NumbersEqual(av->GetNumber(), b->DoubleArrayData()[i])) return false;
            } else if (av->Type() == JsonType::kInteger) {
              if (!IntEqualsDouble(av->GetInteger(), b->DoubleArrayData()[i])) return false;
            } else {
              return false;
            }
          }
        }
        return true;
      }
      for (uint32_t i = 0; i < n; i++) {
        if (!DeepEqualDepth(a->ArrayGet(i), b->ArrayGet(i), depth + 1)) return false;
      }
      return true;
    }
    case JsonType::kObject: {
      if (a->ObjectLen() != b->ObjectLen()) return false;
      auto* entries = a->ObjectEntries();
      for (uint32_t i = 0; i < a->ObjectLen(); i++) {
        auto* other_val = b->ObjectGet(entries[i].KeyView());
        if (!other_val || !DeepEqualDepth(entries[i].value, other_val, depth + 1)) return false;
      }
      return true;
    }
  }
  return false;
}

bool JsonValue::DeepEqual(const JsonValue* other) const {
  return DeepEqualDepth(this, other, 0);
}

JsonValue* JsonValue::DefragTree(JsonValue* root, DefragAllocFn defrag_fn, void* ctx) {
  if (!root || !defrag_fn) return root;

  auto Reloc = [&](auto*& ptr) {
    if (!ptr) return;
    void* moved = defrag_fn(ctx, ptr);
    if (moved) ptr = static_cast<std::remove_reference_t<decltype(ptr)>>(moved);
  };

  Reloc(root);

  std::vector<JsonValue*> stack;
  stack.push_back(root);

  while (!stack.empty()) {
    JsonValue* cur = stack.back();
    stack.pop_back();
    if (!cur) continue;

    switch (cur->Type()) {
      case JsonType::kString:
        if (!cur->IsInlineString()) {
          Reloc(cur->gen_.payload.str.data);
        }
        break;
      case JsonType::kArray: {
        auto& arr = cur->gen_.payload.arr;
        Reloc(arr.items);
        if (!cur->IsTypedArray()) {
          for (uint32_t i = 0; i < arr.len; i++) {
            Reloc(arr.items[i]);
            if (arr.items[i]) stack.push_back(arr.items[i]);
          }
        }
        break;
      }
      case JsonType::kObject: {
        Reloc(cur->gen_.payload.obj);
        ObjectData* od = cur->gen_.payload.obj;
        if (od) {
          Reloc(od->entries);
          Reloc(od->hash_idx);
          for (uint32_t i = 0; i < od->entries_len; i++) {
            if (!od->entries[i].IsLive()) continue;
            if (!od->entries[i].IsInlineKey()) {
              Reloc(od->entries[i].heap_key);
            }
            Reloc(od->entries[i].value);
            if (od->entries[i].value) stack.push_back(od->entries[i].value);
          }
        }
        break;
      }
      default:
        break;
    }
  }

  return root;
}
