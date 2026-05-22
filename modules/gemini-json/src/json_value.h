#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

enum class JsonType : uint8_t {
  kNull,
  kBool,
  kInteger,
  kNumber,
  kString,
  kArray,
  kObject
};

const char* JsonTypeName(JsonType t);

class JsonValue {
public:
  struct ObjEntry {
    char* key;
    uint32_t key_len;
    JsonValue* value;
  };

  static JsonValue* CreateNull();
  static JsonValue* CreateBool(bool v);
  static JsonValue* CreateInteger(int64_t v);
  static JsonValue* CreateNumber(double v);
  static JsonValue* CreateString(const char* data, uint32_t len);
  static JsonValue* CreateArray();
  static JsonValue* CreateObject();

  static void Destroy(JsonValue* v);

  JsonValue* Clone() const;

  JsonType Type() const { return type_; }
  bool IsNull() const { return type_ == JsonType::kNull; }
  bool IsBool() const { return type_ == JsonType::kBool; }
  bool IsInteger() const { return type_ == JsonType::kInteger; }
  bool IsNumber() const { return type_ == JsonType::kNumber || type_ == JsonType::kInteger; }
  bool IsString() const { return type_ == JsonType::kString; }
  bool IsArray() const { return type_ == JsonType::kArray; }
  bool IsObject() const { return type_ == JsonType::kObject; }

  bool GetBool() const { return bool_val_; }
  int64_t GetInteger() const { return int_val_; }
  double GetNumber() const;
  std::string_view GetString() const { return {str_.data, str_.len}; }

  // Array operations
  uint32_t ArrayLen() const { return arr_.len; }
  JsonValue* ArrayGet(uint32_t index) const { return arr_.items[index]; }
  JsonValue** ArrayData() const { return arr_.items; }
  bool ArrayAppend(JsonValue* child);
  bool ArrayInsert(uint32_t index, JsonValue* child);
  JsonValue* ArrayPop(int32_t index);
  void ArrayTrim(uint32_t start, uint32_t stop);

  // Object operations
  uint32_t ObjectLen() const { return obj_.len; }
  JsonValue* ObjectGet(std::string_view key) const;
  bool ObjectSet(const char* key, uint32_t key_len, JsonValue* child);
  bool ObjectDelete(std::string_view key);
  ObjEntry* ObjectEntries() const { return obj_.entries; }
  uint32_t ObjectEntryCount() const { return obj_.len; }

  void Clear();

  size_t MemoryUsage() const;

  bool DeepEqual(const JsonValue* other) const;

private:
  JsonValue() = default;

  bool ArrayGrow();
  bool ObjectGrow();
  void ObjHashRebuild();
  void ObjHashClear();
  int32_t ObjHashProbe(const char* key, uint32_t key_len) const;

  static constexpr uint32_t kObjHashThreshold = 8;

  JsonType type_;
  union {
    bool bool_val_;
    int64_t int_val_;
    double num_val_;
    struct { char* data; uint32_t len; } str_;
    struct { JsonValue** items; uint32_t len; uint32_t cap; } arr_;
    struct {
      ObjEntry* entries;
      uint32_t len;
      uint32_t cap;
      int32_t* hash_idx;
      uint32_t hash_cap;
    } obj_;
  };
};
