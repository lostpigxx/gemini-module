#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Unified maximum nesting depth across Parse, Serialize, Path, Clone, and RDB.
constexpr int kMaxJsonDepth = 128;

enum class JsonType : uint8_t {
  kNull = 0,
  kBool = 1,
  kInteger = 2,
  kNumber = 3,
  kString = 4,
  kArray = 5,
  kObject = 6,
};

const char* JsonTypeName(JsonType t);

class JsonValue {
public:
  struct ObjEntry {
    char* key;
    uint32_t key_len;
    uint32_t key_hash;
    JsonValue* value;
  };

  struct ObjectData {
    ObjEntry* entries;
    int32_t* hash_idx;
    uint32_t len;          // live entry count
    uint32_t entries_len;  // total slots used in entries (including tombstones)
    uint32_t cap;
    uint32_t hash_cap;     // always power of 2 when non-zero
  };

  static constexpr uint32_t kInlineStrMaxLen = 21;  // 21 bytes + '\0' inside 24B node

  static JsonValue* CreateNull();
  static JsonValue* CreateBool(bool v);
  static JsonValue* CreateInteger(int64_t v);
  static JsonValue* CreateNumber(double v);
  static JsonValue* CreateString(const char* data, uint32_t len);
  static JsonValue* CreateArray();
  static JsonValue* CreateObject();

  static void Destroy(JsonValue* v);

  JsonValue* Clone() const;

  JsonType Type() const { return static_cast<JsonType>(gen_.tag & kTypeMask); }
  bool IsNull() const { return Type() == JsonType::kNull; }
  bool IsBool() const { return Type() == JsonType::kBool; }
  bool IsInteger() const { return Type() == JsonType::kInteger; }
  bool IsNumber() const {
    JsonType t = Type();
    return t == JsonType::kNumber || t == JsonType::kInteger;
  }
  bool IsString() const { return Type() == JsonType::kString; }
  bool IsArray() const { return Type() == JsonType::kArray; }
  bool IsObject() const { return Type() == JsonType::kObject; }
  bool IsInlineString() const { return (gen_.tag & kFlagInlineStr) != 0; }

  bool GetBool() const { return gen_.payload.bool_val; }
  int64_t GetInteger() const { return gen_.payload.int_val; }
  double GetNumber() const;
  std::string_view GetString() const {
    if (IsInlineString()) {
      return {sso_.data, sso_.len};
    }
    return {gen_.payload.str.data, gen_.payload.str.len};
  }
  bool StringAppend(std::string_view suffix);

  // Array operations
  uint32_t ArrayLen() const { return gen_.payload.arr.len; }
  uint32_t ArrayCap() const { return gen_.payload.arr.cap; }
  JsonValue* ArrayGet(uint32_t index) const { return gen_.payload.arr.items[index]; }
  JsonValue** ArrayData() const { return gen_.payload.arr.items; }
  bool ArrayReserve(uint32_t new_cap);
  bool ArrayAppend(JsonValue* child);
  bool ArrayInsert(uint32_t index, JsonValue* child);
  JsonValue* ArrayPop(int32_t index);
  void ArrayTrim(uint32_t start, uint32_t stop);

  // Object operations
  uint32_t ObjectLen() const { return gen_.payload.obj ? gen_.payload.obj->len : 0; }
  uint32_t ObjectCap() const { return gen_.payload.obj ? gen_.payload.obj->cap : 0; }
  bool ObjectReserve(uint32_t new_cap);
  JsonValue* ObjectGet(std::string_view key) const;
  bool ObjectSet(const char* key, uint32_t key_len, JsonValue* child);
  bool ObjectDelete(std::string_view key);
  ObjEntry* ObjectEntries() const;
  uint32_t ObjectEntryCount() const { return ObjectLen(); }

  void Clear();

  size_t MemoryUsage() const;

  bool DeepEqual(const JsonValue* other) const;

  // Relocate all heap allocations owned by this tree via `defrag_fn`.
  // Returns the (possibly relocated) root pointer.
  using DefragAllocFn = void* (*)(void* ctx, void* ptr);
  static JsonValue* DefragTree(JsonValue* root, DefragAllocFn defrag_fn, void* ctx);

private:
  JsonValue() = default;

  bool ArrayGrow();
  void ArrayShrinkIfNeeded();

  ObjectData* EnsureObjectData();
  bool ObjectGrow(uint32_t min_cap);
  void ObjectCompact() const;
  void ObjHashRebuild(uint32_t target_cap) const;
  void ObjHashClear() const;
  int32_t ObjHashProbe(const char* key, uint32_t key_len, uint32_t hash) const;
  void ObjHashInsertSlot(uint32_t slot, uint32_t hash) const;
  void ObjHashRemoveSlot(uint32_t slot, uint32_t hash) const;

  static constexpr uint8_t kTypeMask = 0x0F;
  static constexpr uint8_t kFlagInlineStr = 0x10;
  static constexpr uint32_t kObjHashThreshold = 8;

  struct HeapStr {
    char* data;
    uint32_t len;
    uint32_t cap;
  };

  struct ArrayStorage {
    JsonValue** items;
    uint32_t len;
    uint32_t cap;
  };

  union Payload {
    bool bool_val;
    int64_t int_val;
    double num_val;
    HeapStr str;
    ArrayStorage arr;
    ObjectData* obj;
  };

  struct GeneralNode {
    uint8_t tag;
    uint8_t pad[7];
    Payload payload;
  };

  struct InlineStrNode {
    uint8_t tag;
    uint8_t len;
    char data[kInlineStrMaxLen + 1];  // 22 bytes (21 chars + '\0')
  };

  union {
    GeneralNode gen_;
    InlineStrNode sso_;
  };
};

static_assert(sizeof(JsonValue) == 24, "JsonValue must remain compact (24 bytes)");
