#include "json_value.h"
#include "rm_alloc.h"

#include <cmath>
#include <cstring>

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

JsonValue* JsonValue::CreateNull() {
  auto* v = AllocValue();
  if (!v) return nullptr;
  v->type_ = JsonType::kNull;
  return v;
}

JsonValue* JsonValue::CreateBool(bool b) {
  auto* v = AllocValue();
  if (!v) return nullptr;
  v->type_ = JsonType::kBool;
  v->bool_val_ = b;
  return v;
}

JsonValue* JsonValue::CreateInteger(int64_t n) {
  auto* v = AllocValue();
  if (!v) return nullptr;
  v->type_ = JsonType::kInteger;
  v->int_val_ = n;
  return v;
}

JsonValue* JsonValue::CreateNumber(double n) {
  auto* v = AllocValue();
  if (!v) return nullptr;
  v->type_ = JsonType::kNumber;
  v->num_val_ = n;
  return v;
}

JsonValue* JsonValue::CreateString(const char* data, uint32_t len) {
  auto* v = AllocValue();
  if (!v) return nullptr;
  v->type_ = JsonType::kString;
  v->str_.data = static_cast<char*>(RMAlloc(len + 1));
  if (!v->str_.data) { RMFree(v); return nullptr; }
  if (len > 0) std::memcpy(v->str_.data, data, len);
  v->str_.data[len] = '\0';
  v->str_.len = len;
  return v;
}

JsonValue* JsonValue::CreateArray() {
  auto* v = AllocValue();
  if (!v) return nullptr;
  v->type_ = JsonType::kArray;
  v->arr_.items = nullptr;
  v->arr_.len = 0;
  v->arr_.cap = 0;
  return v;
}

JsonValue* JsonValue::CreateObject() {
  auto* v = AllocValue();
  if (!v) return nullptr;
  v->type_ = JsonType::kObject;
  v->obj_.entries = nullptr;
  v->obj_.len = 0;
  v->obj_.cap = 0;
  return v;
}

void JsonValue::Destroy(JsonValue* v) {
  if (!v) return;

  switch (v->type_) {
    case JsonType::kString:
      RMFree(v->str_.data);
      break;
    case JsonType::kArray:
      for (uint32_t i = 0; i < v->arr_.len; i++) {
        Destroy(v->arr_.items[i]);
      }
      RMFree(v->arr_.items);
      break;
    case JsonType::kObject:
      for (uint32_t i = 0; i < v->obj_.len; i++) {
        RMFree(v->obj_.entries[i].key);
        Destroy(v->obj_.entries[i].value);
      }
      RMFree(v->obj_.entries);
      break;
    default:
      break;
  }
  RMFree(v);
}

JsonValue* JsonValue::Clone() const {
  switch (type_) {
    case JsonType::kNull:
      return CreateNull();
    case JsonType::kBool:
      return CreateBool(bool_val_);
    case JsonType::kInteger:
      return CreateInteger(int_val_);
    case JsonType::kNumber:
      return CreateNumber(num_val_);
    case JsonType::kString:
      return CreateString(str_.data, str_.len);
    case JsonType::kArray: {
      auto* a = CreateArray();
      if (!a) return nullptr;
      for (uint32_t i = 0; i < arr_.len; i++) {
        auto* child = arr_.items[i]->Clone();
        if (!child || !a->ArrayAppend(child)) {
          Destroy(child);
          Destroy(a);
          return nullptr;
        }
      }
      return a;
    }
    case JsonType::kObject: {
      auto* o = CreateObject();
      if (!o) return nullptr;
      for (uint32_t i = 0; i < obj_.len; i++) {
        auto* child = obj_.entries[i].value->Clone();
        if (!child || !o->ObjectSet(obj_.entries[i].key, obj_.entries[i].key_len, child)) {
          Destroy(child);
          Destroy(o);
          return nullptr;
        }
      }
      return o;
    }
  }
  return nullptr;
}

double JsonValue::GetNumber() const {
  if (type_ == JsonType::kInteger) return static_cast<double>(int_val_);
  return num_val_;
}

bool JsonValue::ArrayGrow() {
  uint32_t new_cap = arr_.cap == 0 ? 4 : arr_.cap * 2;
  auto* new_items = static_cast<JsonValue**>(
    RMRealloc(arr_.items, new_cap * sizeof(JsonValue*)));
  if (!new_items) return false;
  arr_.items = new_items;
  arr_.cap = new_cap;
  return true;
}

bool JsonValue::ArrayAppend(JsonValue* child) {
  if (arr_.len == arr_.cap && !ArrayGrow()) return false;
  arr_.items[arr_.len++] = child;
  return true;
}

bool JsonValue::ArrayInsert(uint32_t index, JsonValue* child) {
  if (index > arr_.len) return false;
  if (arr_.len == arr_.cap && !ArrayGrow()) return false;
  if (index < arr_.len) {
    std::memmove(&arr_.items[index + 1], &arr_.items[index],
                 (arr_.len - index) * sizeof(JsonValue*));
  }
  arr_.items[index] = child;
  arr_.len++;
  return true;
}

JsonValue* JsonValue::ArrayPop(int32_t index) {
  if (arr_.len == 0) return nullptr;
  int32_t resolved = index;
  if (resolved < 0) resolved += static_cast<int32_t>(arr_.len);
  if (resolved < 0 || static_cast<uint32_t>(resolved) >= arr_.len) return nullptr;
  auto idx = static_cast<uint32_t>(resolved);

  JsonValue* popped = arr_.items[idx];
  if (idx < arr_.len - 1) {
    std::memmove(&arr_.items[idx], &arr_.items[idx + 1],
                 (arr_.len - idx - 1) * sizeof(JsonValue*));
  }
  arr_.len--;
  return popped;
}

void JsonValue::ArrayTrim(uint32_t start, uint32_t stop) {
  if (start >= arr_.len || start > stop) {
    for (uint32_t i = 0; i < arr_.len; i++) Destroy(arr_.items[i]);
    arr_.len = 0;
    return;
  }
  if (stop >= arr_.len) stop = arr_.len - 1;

  for (uint32_t i = 0; i < start; i++) Destroy(arr_.items[i]);
  for (uint32_t i = stop + 1; i < arr_.len; i++) Destroy(arr_.items[i]);

  uint32_t new_len = stop - start + 1;
  if (start > 0) {
    std::memmove(&arr_.items[0], &arr_.items[start], new_len * sizeof(JsonValue*));
  }
  arr_.len = new_len;
}

JsonValue* JsonValue::ObjectGet(std::string_view key) const {
  for (uint32_t i = 0; i < obj_.len; i++) {
    if (key.size() == obj_.entries[i].key_len &&
        std::memcmp(key.data(), obj_.entries[i].key, key.size()) == 0) {
      return obj_.entries[i].value;
    }
  }
  return nullptr;
}

bool JsonValue::ObjectGrow() {
  uint32_t new_cap = obj_.cap == 0 ? 4 : obj_.cap * 2;
  auto* new_entries = static_cast<ObjEntry*>(
    RMRealloc(obj_.entries, new_cap * sizeof(ObjEntry)));
  if (!new_entries) return false;
  obj_.entries = new_entries;
  obj_.cap = new_cap;
  return true;
}

bool JsonValue::ObjectSet(const char* key, uint32_t key_len, JsonValue* child) {
  for (uint32_t i = 0; i < obj_.len; i++) {
    if (key_len == obj_.entries[i].key_len &&
        std::memcmp(key, obj_.entries[i].key, key_len) == 0) {
      Destroy(obj_.entries[i].value);
      obj_.entries[i].value = child;
      return true;
    }
  }

  if (obj_.len == obj_.cap && !ObjectGrow()) return false;

  auto* k = static_cast<char*>(RMAlloc(key_len + 1));
  if (!k) return false;
  std::memcpy(k, key, key_len);
  k[key_len] = '\0';

  obj_.entries[obj_.len] = {k, key_len, child};
  obj_.len++;
  return true;
}

bool JsonValue::ObjectDelete(std::string_view key) {
  for (uint32_t i = 0; i < obj_.len; i++) {
    if (key.size() == obj_.entries[i].key_len &&
        std::memcmp(key.data(), obj_.entries[i].key, key.size()) == 0) {
      RMFree(obj_.entries[i].key);
      Destroy(obj_.entries[i].value);
      if (i < obj_.len - 1) {
        std::memmove(&obj_.entries[i], &obj_.entries[i + 1],
                     (obj_.len - i - 1) * sizeof(ObjEntry));
      }
      obj_.len--;
      return true;
    }
  }
  return false;
}

void JsonValue::Clear() {
  switch (type_) {
    case JsonType::kArray:
      for (uint32_t i = 0; i < arr_.len; i++) Destroy(arr_.items[i]);
      arr_.len = 0;
      break;
    case JsonType::kObject:
      for (uint32_t i = 0; i < obj_.len; i++) {
        RMFree(obj_.entries[i].key);
        Destroy(obj_.entries[i].value);
      }
      obj_.len = 0;
      break;
    case JsonType::kInteger:
      int_val_ = 0;
      break;
    case JsonType::kNumber:
      num_val_ = 0.0;
      break;
    default:
      break;
  }
}

size_t JsonValue::MemoryUsage() const {
  size_t total = sizeof(JsonValue);
  switch (type_) {
    case JsonType::kString:
      total += str_.len + 1;
      break;
    case JsonType::kArray:
      total += arr_.cap * sizeof(JsonValue*);
      for (uint32_t i = 0; i < arr_.len; i++) {
        total += arr_.items[i]->MemoryUsage();
      }
      break;
    case JsonType::kObject:
      total += obj_.cap * sizeof(ObjEntry);
      for (uint32_t i = 0; i < obj_.len; i++) {
        total += obj_.entries[i].key_len + 1;
        total += obj_.entries[i].value->MemoryUsage();
      }
      break;
    default:
      break;
  }
  return total;
}

static bool NumbersEqual(double a, double b) {
  if (std::isnan(a) && std::isnan(b)) return true;
  return a == b;
}

bool JsonValue::DeepEqual(const JsonValue* other) const {
  if (!other) return false;

  if (IsNumber() && other->IsNumber()) {
    return NumbersEqual(GetNumber(), other->GetNumber());
  }

  if (type_ != other->type_) return false;

  switch (type_) {
    case JsonType::kNull:
      return true;
    case JsonType::kBool:
      return bool_val_ == other->bool_val_;
    case JsonType::kInteger:
      return int_val_ == other->int_val_;
    case JsonType::kNumber:
      return NumbersEqual(num_val_, other->num_val_);
    case JsonType::kString:
      return str_.len == other->str_.len &&
             std::memcmp(str_.data, other->str_.data, str_.len) == 0;
    case JsonType::kArray:
      if (arr_.len != other->arr_.len) return false;
      for (uint32_t i = 0; i < arr_.len; i++) {
        if (!arr_.items[i]->DeepEqual(other->arr_.items[i])) return false;
      }
      return true;
    case JsonType::kObject:
      if (obj_.len != other->obj_.len) return false;
      for (uint32_t i = 0; i < obj_.len; i++) {
        if (obj_.entries[i].key_len != other->obj_.entries[i].key_len) return false;
        if (std::memcmp(obj_.entries[i].key, other->obj_.entries[i].key,
                        obj_.entries[i].key_len) != 0) return false;
        if (!obj_.entries[i].value->DeepEqual(other->obj_.entries[i].value)) return false;
      }
      return true;
  }
  return false;
}
