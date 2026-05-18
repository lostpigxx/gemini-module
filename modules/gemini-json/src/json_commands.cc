#include "json_commands.h"
#include "json_type.h"
#include "json_value.h"
#include "json_parse.h"
#include "json_serialize.h"
#include "json_path.h"
#include "rm_alloc.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// --- Helpers ---

static JsonValue* GetJsonValue(RedisModuleKey* key) {
  if (RedisModule_ModuleTypeGetType(key) != JsonModuleType) return nullptr;
  return static_cast<JsonValue*>(RedisModule_ModuleTypeGetValue(key));
}

static bool MatchArg(std::string_view arg, std::string_view target) {
  if (arg.size() != target.size()) return false;
  return strncasecmp(arg.data(), target.data(), arg.size()) == 0;
}

static std::string_view ArgView(RedisModuleString* s) {
  size_t len;
  const char* data = RedisModule_StringPtrLen(s, &len);
  return {data, len};
}

static bool ParsePathFromArg(RedisModuleCtx* ctx, RedisModuleString* arg,
                              JsonPath& out) {
  auto sv = ArgView(arg);
  auto result = ParsePath(sv);
  if (result.error) {
    RedisModule_ReplyWithError(ctx, result.error);
    return false;
  }
  out = std::move(result.path);
  return true;
}

static JsonValue* ParseJsonArg(RedisModuleCtx* ctx, RedisModuleString* arg) {
  auto sv = ArgView(arg);
  auto result = JsonParse(sv);
  if (!result.value) {
    std::string err = "ERR ";
    err += result.error ? result.error : "parse error";
    RedisModule_ReplyWithError(ctx, err.c_str());
    return nullptr;
  }
  return result.value;
}

static JsonValue* OpenReadOnly(RedisModuleCtx* ctx, RedisModuleString* keyName,
                               RedisModuleKey** outKey) {
  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, keyName, REDISMODULE_READ));
  if (outKey) *outKey = key;

  int type = RedisModule_KeyType(key);
  if (type == REDISMODULE_KEYTYPE_EMPTY) {
    RedisModule_ReplyWithNull(ctx);
    return nullptr;
  }
  if (type != REDISMODULE_KEYTYPE_MODULE) {
    RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    return nullptr;
  }
  auto* val = GetJsonValue(key);
  if (!val) {
    RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    return nullptr;
  }
  return val;
}

static bool IsRootOnlyPath(const JsonPath& path) {
  return path.segments.size() == 1 && path.segments[0].type == PathSegType::kRoot;
}

static void ReplyWithJsonValue(RedisModuleCtx* ctx, const JsonValue* val,
                               const SerializeOptions& opts = {}) {
  auto json = JsonSerialize(val, opts);
  RedisModule_ReplyWithStringBuffer(ctx, json.data(), json.size());
}

// Set value at a path match location. Returns true if successfully set.
static bool SetAtMatch(PathMatch& m, JsonValue* new_val) {
  if (!m.parent) return false;

  if (m.parent->IsObject() && !m.object_key.empty()) {
    m.parent->ObjectSet(m.object_key.c_str(),
                        static_cast<uint32_t>(m.object_key.size()), new_val);
    return true;
  }
  if (m.parent->IsArray() && m.array_index >= 0) {
    auto idx = static_cast<uint32_t>(m.array_index);
    if (idx < m.parent->ArrayLen()) {
      JsonValue::Destroy(m.parent->ArrayData()[idx]);
      m.parent->ArrayData()[idx] = new_val;
      return true;
    }
  }
  return false;
}

// Delete value at a path match location.
static bool DeleteAtMatch(PathMatch& m) {
  if (!m.parent) return false;

  if (m.parent->IsObject() && !m.object_key.empty()) {
    return m.parent->ObjectDelete(m.object_key);
  }
  if (m.parent->IsArray() && m.array_index >= 0) {
    auto* popped = m.parent->ArrayPop(m.array_index);
    if (popped) { JsonValue::Destroy(popped); return true; }
  }
  return false;
}

// --- JSON.SET ---
static int CmdSet(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 4 || argc > 5) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  JsonPath path;
  if (!ParsePathFromArg(ctx, argv[2], path)) return REDISMODULE_OK;

  auto* new_val = ParseJsonArg(ctx, argv[3]);
  if (!new_val) return REDISMODULE_OK;

  bool nx = false, xx = false;
  if (argc == 5) {
    auto opt = ArgView(argv[4]);
    if (MatchArg(opt, "NX")) nx = true;
    else if (MatchArg(opt, "XX")) xx = true;
    else {
      JsonValue::Destroy(new_val);
      return RedisModule_ReplyWithError(ctx, "ERR syntax error");
    }
  }

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  int key_type = RedisModule_KeyType(key);

  if (key_type != REDISMODULE_KEYTYPE_EMPTY &&
      key_type != REDISMODULE_KEYTYPE_MODULE) {
    JsonValue::Destroy(new_val);
    return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
  }

  if (key_type == REDISMODULE_KEYTYPE_MODULE) {
    auto* existing = GetJsonValue(key);
    if (!existing) {
      JsonValue::Destroy(new_val);
      return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    }

    if (IsRootOnlyPath(path)) {
      if (nx) {
        JsonValue::Destroy(new_val);
        return RedisModule_ReplyWithNull(ctx);
      }
      RedisModule_ModuleTypeSetValue(key, JsonModuleType, new_val);
      RedisModule_ReplicateVerbatim(ctx);
      return RedisModule_ReplyWithSimpleString(ctx, "OK");
    }

    auto matches = EvaluatePath(path, existing);

    if (nx && !matches.empty()) {
      JsonValue::Destroy(new_val);
      return RedisModule_ReplyWithNull(ctx);
    }
    if (xx && matches.empty()) {
      JsonValue::Destroy(new_val);
      return RedisModule_ReplyWithNull(ctx);
    }

    if (!matches.empty()) {
      for (size_t i = 0; i < matches.size(); i++) {
        auto* val_to_set = (i == matches.size() - 1) ? new_val : new_val->Clone();
        SetAtMatch(matches[i], val_to_set);
      }
    } else {
      // Try to create new key in parent object
      auto& segs = path.segments;
      if (segs.size() >= 2 && segs.back().type == PathSegType::kKey) {
        JsonPath parent_path;
        parent_path.segments.assign(segs.begin(), segs.end() - 1);
        parent_path.is_legacy = path.is_legacy;
        auto parent_matches = EvaluatePath(parent_path, existing);
        bool created = false;
        for (size_t i = 0; i < parent_matches.size(); i++) {
          auto* pm = parent_matches[i].value;
          if (pm && pm->IsObject()) {
            auto* val_to_set = (i == parent_matches.size() - 1 && !created)
                                  ? new_val : new_val->Clone();
            pm->ObjectSet(segs.back().key.c_str(),
                          static_cast<uint32_t>(segs.back().key.size()), val_to_set);
            created = true;
          }
        }
        if (!created) {
          JsonValue::Destroy(new_val);
          return RedisModule_ReplyWithNull(ctx);
        }
      } else {
        JsonValue::Destroy(new_val);
        return RedisModule_ReplyWithNull(ctx);
      }
    }

    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
  }

  // Key is empty
  if (xx) {
    JsonValue::Destroy(new_val);
    return RedisModule_ReplyWithNull(ctx);
  }
  if (!IsRootOnlyPath(path)) {
    JsonValue::Destroy(new_val);
    return RedisModule_ReplyWithError(ctx, "ERR new objects must be created at the root");
  }

  RedisModule_ModuleTypeSetValue(key, JsonModuleType, new_val);
  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// --- JSON.GET ---
static int CmdGet(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* root = OpenReadOnly(ctx, argv[1], nullptr);
  if (!root) return REDISMODULE_OK;

  SerializeOptions opts{};
  std::vector<std::string_view> path_args;
  std::string indent_store, newline_store, space_store;

  for (int i = 2; i < argc; i++) {
    auto sv = ArgView(argv[i]);
    if (MatchArg(sv, "INDENT") && i + 1 < argc) {
      indent_store = std::string(ArgView(argv[++i]));
      opts.indent = indent_store.c_str();
    } else if (MatchArg(sv, "NEWLINE") && i + 1 < argc) {
      newline_store = std::string(ArgView(argv[++i]));
      opts.newline = newline_store.c_str();
    } else if (MatchArg(sv, "SPACE") && i + 1 < argc) {
      space_store = std::string(ArgView(argv[++i]));
      opts.space = space_store.c_str();
    } else {
      path_args.push_back(sv);
    }
  }

  if (path_args.empty()) {
    ReplyWithJsonValue(ctx, root, opts);
    return REDISMODULE_OK;
  }

  if (path_args.size() == 1) {
    auto pr = ParsePath(path_args[0]);
    if (pr.error) return RedisModule_ReplyWithError(ctx, pr.error);

    auto matches = EvaluatePath(pr.path, root);

    if (pr.path.is_legacy) {
      if (matches.empty()) return RedisModule_ReplyWithNull(ctx);
      ReplyWithJsonValue(ctx, matches[0].value, opts);
    } else {
      // JSONPath: wrap results in array
      auto* arr = JsonValue::CreateArray();
      for (auto& m : matches) arr->ArrayAppend(m.value->Clone());
      ReplyWithJsonValue(ctx, arr, opts);
      JsonValue::Destroy(arr);
    }
    return REDISMODULE_OK;
  }

  // Multiple paths: return object { path: [results] }
  auto* result_obj = JsonValue::CreateObject();
  for (auto& pa : path_args) {
    auto pr = ParsePath(pa);
    if (pr.error) {
      JsonValue::Destroy(result_obj);
      return RedisModule_ReplyWithError(ctx, pr.error);
    }
    auto matches = EvaluatePath(pr.path, root);
    auto* arr = JsonValue::CreateArray();
    for (auto& m : matches) arr->ArrayAppend(m.value->Clone());
    result_obj->ObjectSet(pa.data(), static_cast<uint32_t>(pa.size()), arr);
  }
  ReplyWithJsonValue(ctx, result_obj, opts);
  JsonValue::Destroy(result_obj);
  return REDISMODULE_OK;
}

// --- JSON.DEL / JSON.FORGET ---
static int CmdDel(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 2 || argc > 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  int key_type = RedisModule_KeyType(key);

  if (key_type == REDISMODULE_KEYTYPE_EMPTY)
    return RedisModule_ReplyWithLongLong(ctx, 0);
  if (key_type != REDISMODULE_KEYTYPE_MODULE)
    return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  auto* root = GetJsonValue(key);
  if (!root) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  JsonPath path;
  if (argc == 3) {
    if (!ParsePathFromArg(ctx, argv[2], path)) return REDISMODULE_OK;
  } else {
    path.segments.push_back({PathSegType::kRoot, {}, 0, {}});
    path.is_legacy = true;
  }

  if (IsRootOnlyPath(path)) {
    RedisModule_DeleteKey(key);
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithLongLong(ctx, 1);
  }

  auto matches = EvaluatePath(path, root);
  if (matches.empty()) return RedisModule_ReplyWithLongLong(ctx, 0);

  // Sort array deletes in descending order for index stability
  std::sort(matches.begin(), matches.end(), [](const PathMatch& a, const PathMatch& b) {
    if (a.parent == b.parent && a.parent && a.parent->IsArray())
      return a.array_index > b.array_index;
    return false;
  });

  int deleted = 0;
  for (auto& m : matches) {
    if (DeleteAtMatch(m)) deleted++;
  }

  if (deleted > 0) RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithLongLong(ctx, deleted);
}

// --- JSON.TYPE ---
static int CmdType(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 2 || argc > 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* root = OpenReadOnly(ctx, argv[1], nullptr);
  if (!root) return REDISMODULE_OK;

  JsonPath path;
  if (argc == 3) {
    if (!ParsePathFromArg(ctx, argv[2], path)) return REDISMODULE_OK;
  } else {
    path.segments.push_back({PathSegType::kRoot, {}, 0, {}});
    path.is_legacy = true;
  }

  auto matches = EvaluatePath(path, root);

  if (path.is_legacy) {
    if (matches.empty()) return RedisModule_ReplyWithNull(ctx);
    return RedisModule_ReplyWithSimpleString(ctx, JsonTypeName(matches[0].value->Type()));
  }

  RedisModule_ReplyWithArray(ctx, matches.size());
  for (auto& m : matches) {
    RedisModule_ReplyWithSimpleString(ctx, JsonTypeName(m.value->Type()));
  }
  return REDISMODULE_OK;
}

// --- JSON.ARRAPPEND ---
static int CmdArrAppend(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY)
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  auto* root = GetJsonValue(key);
  if (!root) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  JsonPath path;
  if (!ParsePathFromArg(ctx, argv[2], path)) return REDISMODULE_OK;

  std::vector<JsonValue*> values;
  for (int i = 3; i < argc; i++) {
    auto* v = ParseJsonArg(ctx, argv[i]);
    if (!v) {
      for (auto* prev : values) JsonValue::Destroy(prev);
      return REDISMODULE_OK;
    }
    values.push_back(v);
  }

  auto matches = EvaluatePath(path, root);

  if (path.is_legacy) {
    if (matches.empty() || !matches[0].value->IsArray()) {
      for (auto* v : values) JsonValue::Destroy(v);
      return RedisModule_ReplyWithError(ctx, "ERR path does not exist or is not an array");
    }
    auto* arr = matches[0].value;
    for (auto* v : values) arr->ArrayAppend(v);
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithLongLong(ctx, arr->ArrayLen());
  }

  RedisModule_ReplyWithArray(ctx, matches.size());
  bool changed = false;
  for (auto& m : matches) {
    if (!m.value->IsArray()) {
      RedisModule_ReplyWithNull(ctx);
      continue;
    }
    for (auto* v : values) m.value->ArrayAppend(v->Clone());
    RedisModule_ReplyWithLongLong(ctx, m.value->ArrayLen());
    changed = true;
  }
  for (auto* v : values) JsonValue::Destroy(v);
  if (changed) RedisModule_ReplicateVerbatim(ctx);
  return REDISMODULE_OK;
}

// --- JSON.ARRINDEX ---
static int CmdArrIndex(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 4 || argc > 6) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* root = OpenReadOnly(ctx, argv[1], nullptr);
  if (!root) return REDISMODULE_OK;

  JsonPath path;
  if (!ParsePathFromArg(ctx, argv[2], path)) return REDISMODULE_OK;

  auto* search_val = ParseJsonArg(ctx, argv[3]);
  if (!search_val) return REDISMODULE_OK;

  long long start_arg = 0, stop_arg = 0;
  bool stop_set = false;
  if (argc >= 5) {
    if (RedisModule_StringToLongLong(argv[4], &start_arg) != REDISMODULE_OK) {
      JsonValue::Destroy(search_val);
      return RedisModule_ReplyWithError(ctx, "ERR invalid start index");
    }
  }
  if (argc >= 6) {
    if (RedisModule_StringToLongLong(argv[5], &stop_arg) != REDISMODULE_OK) {
      JsonValue::Destroy(search_val);
      return RedisModule_ReplyWithError(ctx, "ERR invalid stop index");
    }
    stop_set = true;
  }

  auto matches = EvaluatePath(path, root);

  auto FindIndex = [&](JsonValue* arr_val) -> long long {
    if (!arr_val->IsArray()) return -2; // sentinel: not an array
    int32_t len = static_cast<int32_t>(arr_val->ArrayLen());
    int32_t s = static_cast<int32_t>(start_arg);
    int32_t e = stop_set ? static_cast<int32_t>(stop_arg) : len;
    if (s < 0) s += len;
    if (e < 0) e += len;
    if (s < 0) s = 0;
    if (e > len) e = len;
    if (e == 0 && !stop_set) e = len;
    for (int32_t i = s; i < e; i++) {
      if (i >= 0 && i < len && arr_val->ArrayGet(static_cast<uint32_t>(i))->DeepEqual(search_val)) {
        JsonValue::Destroy(search_val);
        search_val = nullptr;
        return i;
      }
    }
    return -1;
  };

  if (path.is_legacy) {
    if (matches.empty()) {
      JsonValue::Destroy(search_val);
      return RedisModule_ReplyWithError(ctx, "ERR path does not exist");
    }
    long long idx = FindIndex(matches[0].value);
    if (search_val) JsonValue::Destroy(search_val);
    if (idx == -2) return RedisModule_ReplyWithError(ctx, "ERR path is not an array");
    return RedisModule_ReplyWithLongLong(ctx, idx);
  }

  RedisModule_ReplyWithArray(ctx, matches.size());
  for (auto& m : matches) {
    if (!m.value->IsArray()) {
      RedisModule_ReplyWithNull(ctx);
      continue;
    }
    // Re-implement search per match since search_val might have been freed
    int32_t len = static_cast<int32_t>(m.value->ArrayLen());
    int32_t s = static_cast<int32_t>(start_arg);
    int32_t e = stop_set ? static_cast<int32_t>(stop_arg) : len;
    if (s < 0) s += len;
    if (e < 0) e += len;
    if (s < 0) s = 0;
    if (e > len) e = len;
    long long found = -1;
    for (int32_t i = s; i < e && search_val; i++) {
      if (i >= 0 && i < len && m.value->ArrayGet(static_cast<uint32_t>(i))->DeepEqual(search_val)) {
        found = i;
        break;
      }
    }
    RedisModule_ReplyWithLongLong(ctx, found);
  }
  if (search_val) JsonValue::Destroy(search_val);
  return REDISMODULE_OK;
}

// --- JSON.ARRINSERT ---
static int CmdArrInsert(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 5) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY)
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  auto* root = GetJsonValue(key);
  if (!root) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  JsonPath path;
  if (!ParsePathFromArg(ctx, argv[2], path)) return REDISMODULE_OK;

  long long index_arg;
  if (RedisModule_StringToLongLong(argv[3], &index_arg) != REDISMODULE_OK)
    return RedisModule_ReplyWithError(ctx, "ERR invalid index");

  std::vector<JsonValue*> values;
  for (int i = 4; i < argc; i++) {
    auto* v = ParseJsonArg(ctx, argv[i]);
    if (!v) {
      for (auto* prev : values) JsonValue::Destroy(prev);
      return REDISMODULE_OK;
    }
    values.push_back(v);
  }

  auto matches = EvaluatePath(path, root);

  if (path.is_legacy) {
    if (matches.empty() || !matches[0].value->IsArray()) {
      for (auto* v : values) JsonValue::Destroy(v);
      return RedisModule_ReplyWithError(ctx, "ERR path does not exist or is not an array");
    }
    auto* arr = matches[0].value;
    int32_t idx = static_cast<int32_t>(index_arg);
    if (idx < 0) idx += static_cast<int32_t>(arr->ArrayLen());
    if (idx < 0 || static_cast<uint32_t>(idx) > arr->ArrayLen()) {
      for (auto* v : values) JsonValue::Destroy(v);
      return RedisModule_ReplyWithError(ctx, "ERR index out of range");
    }
    for (size_t i = 0; i < values.size(); i++) {
      arr->ArrayInsert(static_cast<uint32_t>(idx + static_cast<int32_t>(i)), values[i]);
    }
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithLongLong(ctx, arr->ArrayLen());
  }

  RedisModule_ReplyWithArray(ctx, matches.size());
  bool changed = false;
  for (auto& m : matches) {
    if (!m.value->IsArray()) {
      RedisModule_ReplyWithNull(ctx);
      continue;
    }
    auto* arr = m.value;
    int32_t idx = static_cast<int32_t>(index_arg);
    if (idx < 0) idx += static_cast<int32_t>(arr->ArrayLen());
    if (idx < 0 || static_cast<uint32_t>(idx) > arr->ArrayLen()) {
      RedisModule_ReplyWithError(ctx, "ERR index out of range");
      continue;
    }
    for (size_t i = 0; i < values.size(); i++) {
      arr->ArrayInsert(static_cast<uint32_t>(idx + static_cast<int32_t>(i)),
                       values[i]->Clone());
    }
    RedisModule_ReplyWithLongLong(ctx, arr->ArrayLen());
    changed = true;
  }
  for (auto* v : values) JsonValue::Destroy(v);
  if (changed) RedisModule_ReplicateVerbatim(ctx);
  return REDISMODULE_OK;
}

// --- JSON.ARRLEN ---
static int CmdArrLen(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 2 || argc > 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* root = OpenReadOnly(ctx, argv[1], nullptr);
  if (!root) return REDISMODULE_OK;

  JsonPath path;
  if (argc == 3) {
    if (!ParsePathFromArg(ctx, argv[2], path)) return REDISMODULE_OK;
  } else {
    path.segments.push_back({PathSegType::kRoot, {}, 0, {}});
    path.is_legacy = true;
  }

  auto matches = EvaluatePath(path, root);

  if (path.is_legacy) {
    if (matches.empty()) return RedisModule_ReplyWithNull(ctx);
    if (!matches[0].value->IsArray()) return RedisModule_ReplyWithNull(ctx);
    return RedisModule_ReplyWithLongLong(ctx, matches[0].value->ArrayLen());
  }

  RedisModule_ReplyWithArray(ctx, matches.size());
  for (auto& m : matches) {
    if (!m.value->IsArray()) RedisModule_ReplyWithNull(ctx);
    else RedisModule_ReplyWithLongLong(ctx, m.value->ArrayLen());
  }
  return REDISMODULE_OK;
}

// --- JSON.ARRPOP ---
static int CmdArrPop(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 2 || argc > 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY)
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  auto* root = GetJsonValue(key);
  if (!root) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  JsonPath path;
  if (argc >= 3) {
    if (!ParsePathFromArg(ctx, argv[2], path)) return REDISMODULE_OK;
  } else {
    path.segments.push_back({PathSegType::kRoot, {}, 0, {}});
    path.is_legacy = true;
  }

  int32_t pop_index = -1;
  if (argc == 4) {
    long long idx;
    if (RedisModule_StringToLongLong(argv[3], &idx) != REDISMODULE_OK)
      return RedisModule_ReplyWithError(ctx, "ERR invalid index");
    pop_index = static_cast<int32_t>(idx);
  }

  auto matches = EvaluatePath(path, root);

  if (path.is_legacy) {
    if (matches.empty() || !matches[0].value->IsArray())
      return RedisModule_ReplyWithNull(ctx);
    auto* arr = matches[0].value;
    if (arr->ArrayLen() == 0) return RedisModule_ReplyWithNull(ctx);
    auto* popped = arr->ArrayPop(pop_index);
    if (!popped) return RedisModule_ReplyWithNull(ctx);
    ReplyWithJsonValue(ctx, popped);
    JsonValue::Destroy(popped);
    RedisModule_ReplicateVerbatim(ctx);
    return REDISMODULE_OK;
  }

  RedisModule_ReplyWithArray(ctx, matches.size());
  bool changed = false;
  for (auto& m : matches) {
    if (!m.value->IsArray() || m.value->ArrayLen() == 0) {
      RedisModule_ReplyWithNull(ctx);
      continue;
    }
    auto* popped = m.value->ArrayPop(pop_index);
    if (!popped) { RedisModule_ReplyWithNull(ctx); continue; }
    ReplyWithJsonValue(ctx, popped);
    JsonValue::Destroy(popped);
    changed = true;
  }
  if (changed) RedisModule_ReplicateVerbatim(ctx);
  return REDISMODULE_OK;
}

// --- JSON.ARRTRIM ---
static int CmdArrTrim(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 5) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY)
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  auto* root = GetJsonValue(key);
  if (!root) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  JsonPath path;
  if (!ParsePathFromArg(ctx, argv[2], path)) return REDISMODULE_OK;

  long long start_arg, stop_arg;
  if (RedisModule_StringToLongLong(argv[3], &start_arg) != REDISMODULE_OK ||
      RedisModule_StringToLongLong(argv[4], &stop_arg) != REDISMODULE_OK)
    return RedisModule_ReplyWithError(ctx, "ERR invalid index");

  auto matches = EvaluatePath(path, root);

  if (path.is_legacy) {
    if (matches.empty() || !matches[0].value->IsArray())
      return RedisModule_ReplyWithNull(ctx);
    auto* arr = matches[0].value;
    int32_t s = static_cast<int32_t>(start_arg);
    int32_t e = static_cast<int32_t>(stop_arg);
    if (s < 0) s += static_cast<int32_t>(arr->ArrayLen());
    if (e < 0) e += static_cast<int32_t>(arr->ArrayLen());
    arr->ArrayTrim(s < 0 ? 0 : static_cast<uint32_t>(s),
                   e < 0 ? 0 : static_cast<uint32_t>(e));
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithLongLong(ctx, arr->ArrayLen());
  }

  RedisModule_ReplyWithArray(ctx, matches.size());
  bool changed = false;
  for (auto& m : matches) {
    if (!m.value->IsArray()) { RedisModule_ReplyWithNull(ctx); continue; }
    auto* arr = m.value;
    int32_t s = static_cast<int32_t>(start_arg);
    int32_t e = static_cast<int32_t>(stop_arg);
    if (s < 0) s += static_cast<int32_t>(arr->ArrayLen());
    if (e < 0) e += static_cast<int32_t>(arr->ArrayLen());
    arr->ArrayTrim(s < 0 ? 0 : static_cast<uint32_t>(s),
                   e < 0 ? 0 : static_cast<uint32_t>(e));
    RedisModule_ReplyWithLongLong(ctx, arr->ArrayLen());
    changed = true;
  }
  if (changed) RedisModule_ReplicateVerbatim(ctx);
  return REDISMODULE_OK;
}

// --- JSON.STRAPPEND ---
static int CmdStrAppend(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 3 || argc > 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY)
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  auto* root = GetJsonValue(key);
  if (!root) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  int path_idx, val_idx;
  if (argc == 4) { path_idx = 2; val_idx = 3; }
  else { path_idx = -1; val_idx = 2; }

  JsonPath path;
  if (path_idx >= 0) {
    if (!ParsePathFromArg(ctx, argv[path_idx], path)) return REDISMODULE_OK;
  } else {
    path.segments.push_back({PathSegType::kRoot, {}, 0, {}});
    path.is_legacy = true;
  }

  auto* append_val = ParseJsonArg(ctx, argv[val_idx]);
  if (!append_val) return REDISMODULE_OK;
  if (!append_val->IsString()) {
    JsonValue::Destroy(append_val);
    return RedisModule_ReplyWithError(ctx, "ERR value must be a JSON string");
  }
  auto append_sv = append_val->GetString();

  auto matches = EvaluatePath(path, root);

  if (path.is_legacy) {
    if (matches.empty() || !matches[0].value->IsString()) {
      JsonValue::Destroy(append_val);
      return RedisModule_ReplyWithNull(ctx);
    }
    auto* str_val = matches[0].value;
    auto existing = str_val->GetString();
    uint32_t new_len = static_cast<uint32_t>(existing.size() + append_sv.size());
    auto* new_str = JsonValue::CreateString("", 0);
    // Build concatenated string
    auto* buf = static_cast<char*>(RMAlloc(new_len + 1));
    std::memcpy(buf, existing.data(), existing.size());
    std::memcpy(buf + existing.size(), append_sv.data(), append_sv.size());
    buf[new_len] = '\0';
    JsonValue::Destroy(new_str);
    new_str = JsonValue::CreateString(buf, new_len);
    RMFree(buf);

    // Replace in parent
    if (matches[0].parent) {
      SetAtMatch(matches[0], new_str);
    } else {
      RedisModule_ModuleTypeSetValue(key, JsonModuleType, new_str);
    }
    JsonValue::Destroy(append_val);
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithLongLong(ctx, new_len);
  }

  RedisModule_ReplyWithArray(ctx, matches.size());
  bool changed = false;
  for (auto& m : matches) {
    if (!m.value->IsString()) { RedisModule_ReplyWithNull(ctx); continue; }
    auto existing = m.value->GetString();
    uint32_t new_len = static_cast<uint32_t>(existing.size() + append_sv.size());
    auto* buf = static_cast<char*>(RMAlloc(new_len + 1));
    std::memcpy(buf, existing.data(), existing.size());
    std::memcpy(buf + existing.size(), append_sv.data(), append_sv.size());
    buf[new_len] = '\0';
    auto* new_str = JsonValue::CreateString(buf, new_len);
    RMFree(buf);
    if (m.parent) SetAtMatch(m, new_str);
    else RedisModule_ModuleTypeSetValue(key, JsonModuleType, new_str);
    RedisModule_ReplyWithLongLong(ctx, new_len);
    changed = true;
  }
  JsonValue::Destroy(append_val);
  if (changed) RedisModule_ReplicateVerbatim(ctx);
  return REDISMODULE_OK;
}

// --- JSON.STRLEN ---
static int CmdStrLen(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 2 || argc > 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* root = OpenReadOnly(ctx, argv[1], nullptr);
  if (!root) return REDISMODULE_OK;

  JsonPath path;
  if (argc == 3) {
    if (!ParsePathFromArg(ctx, argv[2], path)) return REDISMODULE_OK;
  } else {
    path.segments.push_back({PathSegType::kRoot, {}, 0, {}});
    path.is_legacy = true;
  }

  auto matches = EvaluatePath(path, root);

  if (path.is_legacy) {
    if (matches.empty() || !matches[0].value->IsString())
      return RedisModule_ReplyWithNull(ctx);
    return RedisModule_ReplyWithLongLong(ctx, matches[0].value->GetString().size());
  }

  RedisModule_ReplyWithArray(ctx, matches.size());
  for (auto& m : matches) {
    if (!m.value->IsString()) RedisModule_ReplyWithNull(ctx);
    else RedisModule_ReplyWithLongLong(ctx, m.value->GetString().size());
  }
  return REDISMODULE_OK;
}

// --- JSON.NUMINCRBY ---
static int CmdNumIncrBy(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY)
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  auto* root = GetJsonValue(key);
  if (!root) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  JsonPath path;
  if (!ParsePathFromArg(ctx, argv[2], path)) return REDISMODULE_OK;

  auto* incr_val = ParseJsonArg(ctx, argv[3]);
  if (!incr_val) return REDISMODULE_OK;
  if (!incr_val->IsNumber()) {
    JsonValue::Destroy(incr_val);
    return RedisModule_ReplyWithError(ctx, "ERR value must be a number");
  }
  double incr = incr_val->GetNumber();
  bool incr_is_int = incr_val->IsInteger();
  int64_t incr_int = incr_is_int ? incr_val->GetInteger() : 0;
  JsonValue::Destroy(incr_val);

  auto matches = EvaluatePath(path, root);

  if (path.is_legacy) {
    if (matches.empty() || !matches[0].value->IsNumber())
      return RedisModule_ReplyWithError(ctx, "ERR path does not exist or is not a number");

    auto* v = matches[0].value;
    JsonValue* new_val;
    if (v->IsInteger() && incr_is_int) {
      int64_t old_val = v->GetInteger();
      if ((incr_int > 0 && old_val > INT64_MAX - incr_int) ||
          (incr_int < 0 && old_val < INT64_MIN - incr_int)) {
        new_val = JsonValue::CreateNumber(static_cast<double>(old_val) + incr);
      } else {
        new_val = JsonValue::CreateInteger(old_val + incr_int);
      }
    } else {
      new_val = JsonValue::CreateNumber(v->GetNumber() + incr);
    }

    if (matches[0].parent) SetAtMatch(matches[0], new_val);
    else RedisModule_ModuleTypeSetValue(key, JsonModuleType, new_val);

    auto result = JsonSerialize(new_val);
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithStringBuffer(ctx, result.data(), result.size());
  }

  // JSONPath: return array of results
  std::string reply = "[";
  bool changed = false;
  for (size_t i = 0; i < matches.size(); i++) {
    if (i > 0) reply += ",";
    auto* v = matches[i].value;
    if (!v->IsNumber()) {
      reply += "null";
      continue;
    }
    JsonValue* new_val;
    if (v->IsInteger() && incr_is_int) {
      int64_t old_val = v->GetInteger();
      if ((incr_int > 0 && old_val > INT64_MAX - incr_int) ||
          (incr_int < 0 && old_val < INT64_MIN - incr_int)) {
        new_val = JsonValue::CreateNumber(static_cast<double>(old_val) + incr);
      } else {
        new_val = JsonValue::CreateInteger(old_val + incr_int);
      }
    } else {
      new_val = JsonValue::CreateNumber(v->GetNumber() + incr);
    }
    reply += JsonSerialize(new_val);
    if (matches[i].parent) SetAtMatch(matches[i], new_val);
    else RedisModule_ModuleTypeSetValue(key, JsonModuleType, new_val);
    changed = true;
  }
  reply += "]";
  if (changed) RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithStringBuffer(ctx, reply.data(), reply.size());
}

// --- JSON.NUMMULTBY ---
static int CmdNumMultBy(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY)
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  auto* root = GetJsonValue(key);
  if (!root) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  JsonPath path;
  if (!ParsePathFromArg(ctx, argv[2], path)) return REDISMODULE_OK;

  auto* mult_val = ParseJsonArg(ctx, argv[3]);
  if (!mult_val) return REDISMODULE_OK;
  if (!mult_val->IsNumber()) {
    JsonValue::Destroy(mult_val);
    return RedisModule_ReplyWithError(ctx, "ERR value must be a number");
  }
  double mult = mult_val->GetNumber();
  JsonValue::Destroy(mult_val);

  auto matches = EvaluatePath(path, root);

  if (path.is_legacy) {
    if (matches.empty() || !matches[0].value->IsNumber())
      return RedisModule_ReplyWithError(ctx, "ERR path does not exist or is not a number");

    double result = matches[0].value->GetNumber() * mult;
    auto* new_val = JsonValue::CreateNumber(result);
    if (matches[0].parent) SetAtMatch(matches[0], new_val);
    else RedisModule_ModuleTypeSetValue(key, JsonModuleType, new_val);
    auto reply = JsonSerialize(new_val);
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithStringBuffer(ctx, reply.data(), reply.size());
  }

  std::string reply = "[";
  bool changed = false;
  for (size_t i = 0; i < matches.size(); i++) {
    if (i > 0) reply += ",";
    auto* v = matches[i].value;
    if (!v->IsNumber()) { reply += "null"; continue; }
    double result = v->GetNumber() * mult;
    auto* new_val = JsonValue::CreateNumber(result);
    reply += JsonSerialize(new_val);
    if (matches[i].parent) SetAtMatch(matches[i], new_val);
    else RedisModule_ModuleTypeSetValue(key, JsonModuleType, new_val);
    changed = true;
  }
  reply += "]";
  if (changed) RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithStringBuffer(ctx, reply.data(), reply.size());
}

// --- JSON.OBJKEYS ---
static int CmdObjKeys(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 2 || argc > 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* root = OpenReadOnly(ctx, argv[1], nullptr);
  if (!root) return REDISMODULE_OK;

  JsonPath path;
  if (argc == 3) {
    if (!ParsePathFromArg(ctx, argv[2], path)) return REDISMODULE_OK;
  } else {
    path.segments.push_back({PathSegType::kRoot, {}, 0, {}});
    path.is_legacy = true;
  }

  auto matches = EvaluatePath(path, root);

  if (path.is_legacy) {
    if (matches.empty() || !matches[0].value->IsObject())
      return RedisModule_ReplyWithNull(ctx);
    auto* obj = matches[0].value;
    RedisModule_ReplyWithArray(ctx, obj->ObjectLen());
    auto* entries = obj->ObjectEntries();
    for (uint32_t i = 0; i < obj->ObjectLen(); i++) {
      RedisModule_ReplyWithStringBuffer(ctx, entries[i].key, entries[i].key_len);
    }
    return REDISMODULE_OK;
  }

  RedisModule_ReplyWithArray(ctx, matches.size());
  for (auto& m : matches) {
    if (!m.value->IsObject()) { RedisModule_ReplyWithNull(ctx); continue; }
    auto* obj = m.value;
    RedisModule_ReplyWithArray(ctx, obj->ObjectLen());
    auto* entries = obj->ObjectEntries();
    for (uint32_t i = 0; i < obj->ObjectLen(); i++) {
      RedisModule_ReplyWithStringBuffer(ctx, entries[i].key, entries[i].key_len);
    }
  }
  return REDISMODULE_OK;
}

// --- JSON.OBJLEN ---
static int CmdObjLen(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 2 || argc > 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* root = OpenReadOnly(ctx, argv[1], nullptr);
  if (!root) return REDISMODULE_OK;

  JsonPath path;
  if (argc == 3) {
    if (!ParsePathFromArg(ctx, argv[2], path)) return REDISMODULE_OK;
  } else {
    path.segments.push_back({PathSegType::kRoot, {}, 0, {}});
    path.is_legacy = true;
  }

  auto matches = EvaluatePath(path, root);

  if (path.is_legacy) {
    if (matches.empty() || !matches[0].value->IsObject())
      return RedisModule_ReplyWithNull(ctx);
    return RedisModule_ReplyWithLongLong(ctx, matches[0].value->ObjectLen());
  }

  RedisModule_ReplyWithArray(ctx, matches.size());
  for (auto& m : matches) {
    if (!m.value->IsObject()) RedisModule_ReplyWithNull(ctx);
    else RedisModule_ReplyWithLongLong(ctx, m.value->ObjectLen());
  }
  return REDISMODULE_OK;
}

// --- JSON.CLEAR ---
static int CmdClear(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 2 || argc > 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY)
    return RedisModule_ReplyWithLongLong(ctx, 0);
  auto* root = GetJsonValue(key);
  if (!root) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  JsonPath path;
  if (argc == 3) {
    if (!ParsePathFromArg(ctx, argv[2], path)) return REDISMODULE_OK;
  } else {
    path.segments.push_back({PathSegType::kRoot, {}, 0, {}});
    path.is_legacy = true;
  }

  auto matches = EvaluatePath(path, root);
  int cleared = 0;
  for (auto& m : matches) {
    auto* v = m.value;
    if (v->IsArray() || v->IsObject() || v->IsNumber()) {
      v->Clear();
      cleared++;
    }
  }
  if (cleared > 0) RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithLongLong(ctx, cleared);
}

// --- JSON.TOGGLE ---
static int CmdToggle(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 2 || argc > 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY)
    return RedisModule_ReplyWithError(ctx, "ERR key does not exist");
  auto* root = GetJsonValue(key);
  if (!root) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

  JsonPath path;
  if (argc == 3) {
    if (!ParsePathFromArg(ctx, argv[2], path)) return REDISMODULE_OK;
  } else {
    path.segments.push_back({PathSegType::kRoot, {}, 0, {}});
    path.is_legacy = true;
  }

  auto matches = EvaluatePath(path, root);

  if (path.is_legacy) {
    if (matches.empty() || !matches[0].value->IsBool())
      return RedisModule_ReplyWithError(ctx, "ERR path does not exist or is not a boolean");
    bool old_val = matches[0].value->GetBool();
    auto* new_val = JsonValue::CreateBool(!old_val);
    if (matches[0].parent) SetAtMatch(matches[0], new_val);
    else RedisModule_ModuleTypeSetValue(key, JsonModuleType, new_val);
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithSimpleString(ctx, !old_val ? "true" : "false");
  }

  RedisModule_ReplyWithArray(ctx, matches.size());
  bool changed = false;
  for (auto& m : matches) {
    if (!m.value->IsBool()) { RedisModule_ReplyWithNull(ctx); continue; }
    bool old_val = m.value->GetBool();
    auto* new_val = JsonValue::CreateBool(!old_val);
    if (m.parent) SetAtMatch(m, new_val);
    else RedisModule_ModuleTypeSetValue(key, JsonModuleType, new_val);
    RedisModule_ReplyWithLongLong(ctx, !old_val ? 1 : 0);
    changed = true;
  }
  if (changed) RedisModule_ReplicateVerbatim(ctx);
  return REDISMODULE_OK;
}

// --- JSON.MGET ---
static int CmdMGet(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  // Last arg is path, rest are keys
  JsonPath path;
  if (!ParsePathFromArg(ctx, argv[argc - 1], path)) return REDISMODULE_OK;

  int num_keys = argc - 2;
  RedisModule_ReplyWithArray(ctx, num_keys);

  for (int i = 1; i <= num_keys; i++) {
    auto* key = static_cast<RedisModuleKey*>(
      RedisModule_OpenKey(ctx, argv[i], REDISMODULE_READ));
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY ||
        RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_MODULE) {
      RedisModule_ReplyWithNull(ctx);
      continue;
    }
    auto* root = GetJsonValue(key);
    if (!root) { RedisModule_ReplyWithNull(ctx); continue; }

    auto matches = EvaluatePath(path, root);
    if (matches.empty()) {
      RedisModule_ReplyWithNull(ctx);
    } else if (path.is_legacy) {
      ReplyWithJsonValue(ctx, matches[0].value);
    } else {
      auto* arr = JsonValue::CreateArray();
      for (auto& m : matches) arr->ArrayAppend(m.value->Clone());
      ReplyWithJsonValue(ctx, arr);
      JsonValue::Destroy(arr);
    }
  }
  return REDISMODULE_OK;
}

// --- JSON.MSET ---
static int CmdMSet(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 4 || (argc - 1) % 3 != 0) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  int num_triplets = (argc - 1) / 3;

  struct Triplet {
    int key_idx;
    JsonPath path;
    JsonValue* value;
  };

  std::vector<Triplet> triplets;

  // Phase 1: parse all triplets
  for (int i = 0; i < num_triplets; i++) {
    int base = 1 + i * 3;
    Triplet t;
    t.key_idx = base;
    if (!ParsePathFromArg(ctx, argv[base + 1], t.path)) {
      for (auto& prev : triplets) JsonValue::Destroy(prev.value);
      return REDISMODULE_OK;
    }
    t.value = ParseJsonArg(ctx, argv[base + 2]);
    if (!t.value) {
      for (auto& prev : triplets) JsonValue::Destroy(prev.value);
      return REDISMODULE_OK;
    }
    triplets.push_back(std::move(t));
  }

  // Phase 2: apply all sets
  for (auto& t : triplets) {
    auto* key = static_cast<RedisModuleKey*>(
      RedisModule_OpenKey(ctx, argv[t.key_idx], REDISMODULE_READ | REDISMODULE_WRITE));
    int key_type = RedisModule_KeyType(key);

    if (key_type == REDISMODULE_KEYTYPE_EMPTY) {
      if (IsRootOnlyPath(t.path)) {
        RedisModule_ModuleTypeSetValue(key, JsonModuleType, t.value);
        t.value = nullptr;
      } else {
        for (auto& rem : triplets) JsonValue::Destroy(rem.value);
        return RedisModule_ReplyWithError(ctx, "ERR new objects must be created at the root");
      }
    } else if (key_type == REDISMODULE_KEYTYPE_MODULE) {
      auto* existing = GetJsonValue(key);
      if (!existing) {
        for (auto& rem : triplets) JsonValue::Destroy(rem.value);
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
      }
      if (IsRootOnlyPath(t.path)) {
        RedisModule_ModuleTypeSetValue(key, JsonModuleType, t.value);
        t.value = nullptr;
      } else {
        auto matches = EvaluatePath(t.path, existing);
        if (!matches.empty()) {
          for (size_t j = 0; j < matches.size(); j++) {
            auto* val = (j == matches.size() - 1) ? t.value : t.value->Clone();
            SetAtMatch(matches[j], val);
          }
          t.value = nullptr;
        } else {
          auto& segs = t.path.segments;
          if (segs.size() >= 2 && segs.back().type == PathSegType::kKey) {
            JsonPath parent_path;
            parent_path.segments.assign(segs.begin(), segs.end() - 1);
            parent_path.is_legacy = t.path.is_legacy;
            auto parent_matches = EvaluatePath(parent_path, existing);
            bool created = false;
            for (size_t j = 0; j < parent_matches.size(); j++) {
              auto* pm = parent_matches[j].value;
              if (pm && pm->IsObject()) {
                auto* val = (j == parent_matches.size() - 1 && !created)
                              ? t.value : t.value->Clone();
                pm->ObjectSet(segs.back().key.c_str(),
                              static_cast<uint32_t>(segs.back().key.size()), val);
                created = true;
              }
            }
            if (created) t.value = nullptr;
          }
        }
      }
    } else {
      for (auto& rem : triplets) JsonValue::Destroy(rem.value);
      return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    }
  }

  for (auto& t : triplets) JsonValue::Destroy(t.value);
  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// --- JSON.MERGE (RFC 7396) ---
static void MergePatch(JsonValue* target, JsonValue* patch, JsonValue* /*parent*/,
                       PathMatch& match, RedisModuleKey* key) {
  if (!patch->IsObject()) {
    auto* cloned = patch->Clone();
    if (match.parent) {
      SetAtMatch(match, cloned);
      match.value = cloned;
    } else {
      RedisModule_ModuleTypeSetValue(key, JsonModuleType, cloned);
      match.value = cloned;
    }
    return;
  }

  if (!target->IsObject()) {
    auto* new_obj = JsonValue::CreateObject();
    if (match.parent) {
      SetAtMatch(match, new_obj);
      match.value = new_obj;
    } else {
      RedisModule_ModuleTypeSetValue(key, JsonModuleType, new_obj);
      match.value = new_obj;
    }
    target = new_obj;
  }

  auto* patch_entries = patch->ObjectEntries();
  for (uint32_t i = 0; i < patch->ObjectLen(); i++) {
    auto pk = std::string_view(patch_entries[i].key, patch_entries[i].key_len);
    auto* pv = patch_entries[i].value;

    if (pv->IsNull()) {
      target->ObjectDelete(pk);
    } else {
      auto* existing = target->ObjectGet(pk);
      if (existing) {
        PathMatch child_match;
        child_match.value = existing;
        child_match.parent = target;
        child_match.object_key = std::string(pk);
        MergePatch(existing, pv, target, child_match, key);
      } else {
        target->ObjectSet(patch_entries[i].key, patch_entries[i].key_len, pv->Clone());
      }
    }
  }
}

static int CmdMerge(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc != 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* key = static_cast<RedisModuleKey*>(
    RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));

  JsonPath path;
  if (!ParsePathFromArg(ctx, argv[2], path)) return REDISMODULE_OK;

  auto* patch = ParseJsonArg(ctx, argv[3]);
  if (!patch) return REDISMODULE_OK;

  int key_type = RedisModule_KeyType(key);

  if (key_type == REDISMODULE_KEYTYPE_EMPTY) {
    if (IsRootOnlyPath(path)) {
      RedisModule_ModuleTypeSetValue(key, JsonModuleType, patch);
      RedisModule_ReplicateVerbatim(ctx);
      return RedisModule_ReplyWithSimpleString(ctx, "OK");
    }
    JsonValue::Destroy(patch);
    return RedisModule_ReplyWithError(ctx, "ERR new objects must be created at the root");
  }

  if (key_type != REDISMODULE_KEYTYPE_MODULE) {
    JsonValue::Destroy(patch);
    return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
  }

  auto* root = GetJsonValue(key);
  if (!root) {
    JsonValue::Destroy(patch);
    return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
  }

  auto matches = EvaluatePath(path, root);
  if (matches.empty()) {
    JsonValue::Destroy(patch);
    return RedisModule_ReplyWithError(ctx, "ERR path does not exist");
  }

  for (auto& m : matches) {
    MergePatch(m.value, patch, m.parent, m, key);
  }

  JsonValue::Destroy(patch);
  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// --- JSON.DEBUG ---
static int CmdDebug(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto subcmd = ArgView(argv[1]);

  if (MatchArg(subcmd, "MEMORY")) {
    if (argc < 3 || argc > 4) return RedisModule_WrongArity(ctx);

    auto* root = OpenReadOnly(ctx, argv[2], nullptr);
    if (!root) return REDISMODULE_OK;

    JsonPath path;
    if (argc == 4) {
      if (!ParsePathFromArg(ctx, argv[3], path)) return REDISMODULE_OK;
    } else {
      path.segments.push_back({PathSegType::kRoot, {}, 0, {}});
      path.is_legacy = true;
    }

    auto matches = EvaluatePath(path, root);
    if (matches.empty()) return RedisModule_ReplyWithLongLong(ctx, 0);
    size_t total = 0;
    for (auto& m : matches) total += m.value->MemoryUsage();
    return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(total));
  }

  if (MatchArg(subcmd, "HELP")) {
    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithSimpleString(ctx, "JSON.DEBUG MEMORY <key> [path] - report memory usage");
    RedisModule_ReplyWithSimpleString(ctx, "JSON.DEBUG HELP - show help");
    return REDISMODULE_OK;
  }

  return RedisModule_ReplyWithError(ctx, "ERR unknown JSON.DEBUG subcommand");
}

// --- JSON.RESP ---
static void ReplyResp(RedisModuleCtx* ctx, const JsonValue* v) {
  switch (v->Type()) {
    case JsonType::kNull:
      RedisModule_ReplyWithNull(ctx);
      break;
    case JsonType::kBool:
      RedisModule_ReplyWithSimpleString(ctx, v->GetBool() ? "true" : "false");
      break;
    case JsonType::kInteger:
      RedisModule_ReplyWithLongLong(ctx, v->GetInteger());
      break;
    case JsonType::kNumber: {
      auto s = JsonSerialize(v);
      RedisModule_ReplyWithStringBuffer(ctx, s.data(), s.size());
      break;
    }
    case JsonType::kString: {
      auto sv = v->GetString();
      RedisModule_ReplyWithStringBuffer(ctx, sv.data(), sv.size());
      break;
    }
    case JsonType::kArray: {
      RedisModule_ReplyWithArray(ctx, v->ArrayLen() + 1);
      RedisModule_ReplyWithSimpleString(ctx, "[");
      for (uint32_t i = 0; i < v->ArrayLen(); i++) {
        ReplyResp(ctx, v->ArrayGet(i));
      }
      break;
    }
    case JsonType::kObject: {
      auto* entries = v->ObjectEntries();
      RedisModule_ReplyWithArray(ctx, v->ObjectLen() * 2 + 1);
      RedisModule_ReplyWithSimpleString(ctx, "{");
      for (uint32_t i = 0; i < v->ObjectLen(); i++) {
        RedisModule_ReplyWithStringBuffer(ctx, entries[i].key, entries[i].key_len);
        ReplyResp(ctx, entries[i].value);
      }
      break;
    }
  }
}

static int CmdResp(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 2 || argc > 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto* root = OpenReadOnly(ctx, argv[1], nullptr);
  if (!root) return REDISMODULE_OK;

  JsonPath path;
  if (argc == 3) {
    if (!ParsePathFromArg(ctx, argv[2], path)) return REDISMODULE_OK;
  } else {
    path.segments.push_back({PathSegType::kRoot, {}, 0, {}});
    path.is_legacy = true;
  }

  auto matches = EvaluatePath(path, root);

  if (path.is_legacy) {
    if (matches.empty()) return RedisModule_ReplyWithNull(ctx);
    ReplyResp(ctx, matches[0].value);
    return REDISMODULE_OK;
  }

  RedisModule_ReplyWithArray(ctx, matches.size());
  for (auto& m : matches) {
    ReplyResp(ctx, m.value);
  }
  return REDISMODULE_OK;
}

// --- Command Registration ---
int RegisterJsonCommands(RedisModuleCtx* ctx) {
  struct CmdEntry {
    const char* name;
    RedisModuleCmdFunc handler;
    const char* flags;
  };

  CmdEntry commands[] = {
    {"JSON.SET",       CmdSet,       "write deny-oom"},
    {"JSON.GET",       CmdGet,       "readonly"},
    {"JSON.DEL",       CmdDel,       "write"},
    {"JSON.FORGET",    CmdDel,       "write"},
    {"JSON.TYPE",      CmdType,      "readonly"},
    {"JSON.ARRAPPEND", CmdArrAppend, "write deny-oom"},
    {"JSON.ARRINDEX",  CmdArrIndex,  "readonly"},
    {"JSON.ARRINSERT", CmdArrInsert, "write deny-oom"},
    {"JSON.ARRLEN",    CmdArrLen,    "readonly"},
    {"JSON.ARRPOP",    CmdArrPop,    "write"},
    {"JSON.ARRTRIM",   CmdArrTrim,   "write"},
    {"JSON.STRAPPEND", CmdStrAppend, "write deny-oom"},
    {"JSON.STRLEN",    CmdStrLen,    "readonly"},
    {"JSON.NUMINCRBY", CmdNumIncrBy, "write deny-oom"},
    {"JSON.NUMMULTBY", CmdNumMultBy, "write deny-oom"},
    {"JSON.OBJKEYS",   CmdObjKeys,   "readonly"},
    {"JSON.OBJLEN",    CmdObjLen,    "readonly"},
    {"JSON.CLEAR",     CmdClear,     "write"},
    {"JSON.TOGGLE",    CmdToggle,    "write"},
    {"JSON.MGET",      CmdMGet,      "readonly"},
    {"JSON.MSET",      CmdMSet,      "write deny-oom"},
    {"JSON.MERGE",     CmdMerge,     "write deny-oom"},
    {"JSON.DEBUG",     CmdDebug,     "readonly"},
    {"JSON.RESP",      CmdResp,      "readonly"},
  };

  for (auto& cmd : commands) {
    if (RedisModule_CreateCommand(ctx, cmd.name, cmd.handler, cmd.flags, 1, 1, 1) ==
        REDISMODULE_ERR) {
      return REDISMODULE_ERR;
    }
  }
  return REDISMODULE_OK;
}
