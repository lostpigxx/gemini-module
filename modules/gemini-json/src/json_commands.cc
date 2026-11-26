#include "json_commands.h"
#include "json_type.h"
#include "json_value.h"
#include "json_parse.h"
#include "json_serialize.h"
#include "json_path.h"
#include "rm_alloc.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
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

static const JsonPath* ParsePathFromArgCached(RedisModuleCtx* ctx,
                                              RedisModuleString* arg) {
  auto sv = ArgView(arg);
  const char* err = nullptr;
  const JsonPath* cached = ParsePathCached(sv, &err);
  if (!cached) {
    RedisModule_ReplyWithError(ctx, err ? err : "ERR invalid path");
    return nullptr;
  }
  return cached;
}

static bool ParsePathFromArg(RedisModuleCtx* ctx, RedisModuleString* arg,
                              JsonPath& out) {
  const JsonPath* cached = ParsePathFromArgCached(ctx, arg);
  if (!cached) return false;
  out = *cached;
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

// Replace value at a path match location.
static bool SetAtMatch(PathMatch& m, JsonValue* new_val) {
  if (!m.parent) return false;

  if (m.parent->IsObject() && m.has_object_key) {
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

  if (m.parent->IsObject() && m.has_object_key) {
    return m.parent->ObjectDelete(m.object_key);
  }
  if (m.parent->IsArray() && m.array_index >= 0) {
    auto* popped = m.parent->ArrayPop(m.array_index);
    if (popped) { JsonValue::Destroy(popped); return true; }
  }
  return false;
}

// Deduplicate matches, prune descendants of already-matched containers (which
// prevents Use-After-Free when mutating/deleting overlapping recursive paths
// such as $..*), and sort with a valid strict weak ordering so that array
// mutations within the same parent proceed from highest index to lowest.
static void PruneAndSortMatchesForMutation(std::vector<PathMatch>& matches) {
  if (matches.size() <= 1) return;

  // Step 1: Collect matched container values and mark all their descendants.
  std::unordered_set<const JsonValue*> descendants;
  bool any_container = false;
  for (const auto& m : matches) {
    if (m.value && (m.value->IsArray() || m.value->IsObject())) {
      any_container = true;
      break;
    }
  }

  if (any_container) {
    std::vector<const JsonValue*> stack;
    for (const auto& m : matches) {
      const JsonValue* v = m.value;
      if (!v || (!v->IsArray() && !v->IsObject())) continue;
      if (descendants.count(v)) continue;

      // Push direct children of v (not v itself)
      if (v->IsArray()) {
        for (uint32_t i = 0; i < v->ArrayLen(); i++) {
          if (v->ArrayGet(i)) stack.push_back(v->ArrayGet(i));
        }
      } else {
        auto* entries = v->ObjectEntries();
        for (uint32_t i = 0; i < v->ObjectLen(); i++) {
          if (entries[i].value) stack.push_back(entries[i].value);
        }
      }

      while (!stack.empty()) {
        const JsonValue* cur = stack.back();
        stack.pop_back();
        if (!descendants.insert(cur).second) continue;
        if (cur->IsArray()) {
          for (uint32_t i = 0; i < cur->ArrayLen(); i++) {
            if (cur->ArrayGet(i)) stack.push_back(cur->ArrayGet(i));
          }
        } else if (cur->IsObject()) {
          auto* entries = cur->ObjectEntries();
          for (uint32_t i = 0; i < cur->ObjectLen(); i++) {
            if (entries[i].value) stack.push_back(entries[i].value);
          }
        }
      }
    }
  }

  // Step 2: Filter out descendants and exact duplicate value pointers.
  std::unordered_set<const JsonValue*> seen;
  seen.reserve(matches.size());
  std::vector<PathMatch> filtered;
  filtered.reserve(matches.size());
  for (auto& m : matches) {
    if (!m.value) continue;
    if (any_container && descendants.count(m.value)) continue;
    if (!seen.insert(m.value).second) continue;
    filtered.push_back(std::move(m));
  }
  matches.swap(filtered);

  // Step 3: Strict weak ordering: group by parent pointer, then descending
  // array_index for arrays, then object_key for objects.
  std::sort(matches.begin(), matches.end(), [](const PathMatch& a, const PathMatch& b) {
    if (a.parent != b.parent) return a.parent < b.parent;
    if (a.parent && a.parent->IsArray()) return a.array_index > b.array_index;
    if (a.has_object_key != b.has_object_key) return a.has_object_key < b.has_object_key;
    return a.object_key < b.object_key;
  });
}

// Single-pass helper: try to insert new_val into an object parent at the
// leaf key of the given path. Walks the path minus the last segment to find
// candidate parent objects, then inserts the key directly.
// Returns true if at least one insertion happened.
static bool TryCreateAtLeafKey(const JsonPath& path, JsonValue* root, JsonValue* new_val) {
  auto& segs = path.segments;
  if (segs.size() < 2) return false;

  auto& leaf = segs.back();
  if (leaf.type != PathSegType::kKey) return false;

  // Build a temporary path containing everything except the leaf segment
  JsonPath ancestor_path;
  ancestor_path.segments.assign(segs.begin(), segs.end() - 1);
  ancestor_path.is_legacy = path.is_legacy;

  auto candidates = EvaluatePath(ancestor_path, root);
  bool inserted = false;
  for (auto& c : candidates) {
    if (c.value && c.value->IsObject()) {
      auto* val = inserted ? new_val->Clone() : new_val;
      c.value->ObjectSet(leaf.key.c_str(),
                         static_cast<uint32_t>(leaf.key.size()), val);
      inserted = true;
    }
  }
  return inserted;
}

// --- JSON.SET ---
static void NotifyJsonEvent(RedisModuleCtx* ctx, const char* event,
                            RedisModuleString* key) {
  if (RedisModule_NotifyKeyspaceEvent) {
    RedisModule_NotifyKeyspaceEvent(ctx, REDISMODULE_NOTIFY_MODULE, event, key);
  }
}

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

  // --- Key already exists ---
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
      NotifyJsonEvent(ctx, "json.set", argv[1]);
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
      PruneAndSortMatchesForMutation(matches);
      for (size_t i = 0; i < matches.size(); i++) {
        auto* val_to_set = (i == matches.size() - 1) ? new_val : new_val->Clone();
        if (!matches[i].parent) {
          RedisModule_ModuleTypeSetValue(key, JsonModuleType, val_to_set);
        } else {
          SetAtMatch(matches[i], val_to_set);
        }
      }
    } else {
      if (!TryCreateAtLeafKey(path, existing, new_val)) {
        JsonValue::Destroy(new_val);
        return RedisModule_ReplyWithNull(ctx);
      }
    }

    NotifyJsonEvent(ctx, "json.set", argv[1]);
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
  }

  // --- Key is empty ---
  if (xx) {
    JsonValue::Destroy(new_val);
    return RedisModule_ReplyWithNull(ctx);
  }
  if (!IsRootOnlyPath(path)) {
    JsonValue::Destroy(new_val);
    return RedisModule_ReplyWithError(ctx, "ERR new objects must be created at the root");
  }

  RedisModule_ModuleTypeSetValue(key, JsonModuleType, new_val);
  NotifyJsonEvent(ctx, "json.set", argv[1]);
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
    } else if (MatchArg(sv, "NOESCAPE")) {
      opts.noescape = true;
    } else {
      path_args.push_back(sv);
    }
  }

  if (path_args.empty()) {
    ReplyWithJsonValue(ctx, root, opts);
    return REDISMODULE_OK;
  }

  if (path_args.size() == 1) {
    const char* err = nullptr;
    const JsonPath* path = ParsePathCached(path_args[0], &err);
    if (!path) return RedisModule_ReplyWithError(ctx, err);

    auto matches = EvaluatePath(*path, root);

    if (path->is_legacy) {
      if (matches.empty()) return RedisModule_ReplyWithNull(ctx);
      ReplyWithJsonValue(ctx, matches[0].value, opts);
    } else {
      std::vector<const JsonValue*> ptrs;
      ptrs.reserve(matches.size());
      for (const auto& m : matches) ptrs.push_back(m.value);
      auto json = JsonSerializeArray(ptrs.data(), ptrs.size(), opts);
      RedisModule_ReplyWithStringBuffer(ctx, json.data(), json.size());
    }
    return REDISMODULE_OK;
  }

  // Multiple paths: zero-copy streaming { path: [results] }
  std::vector<std::vector<const JsonValue*>> all_ptrs(path_args.size());
  std::vector<NamedMatchList> lists(path_args.size());
  for (size_t idx = 0; idx < path_args.size(); idx++) {
    const char* err = nullptr;
    const JsonPath* path = ParsePathCached(path_args[idx], &err);
    if (!path) {
      return RedisModule_ReplyWithError(ctx, err);
    }
    auto matches = EvaluatePath(*path, root);
    all_ptrs[idx].reserve(matches.size());
    for (const auto& m : matches) all_ptrs[idx].push_back(m.value);
    lists[idx] = {path_args[idx], all_ptrs[idx].data(), all_ptrs[idx].size()};
  }
  auto json = JsonSerializeObjectOfArrays(lists.data(), lists.size(), opts);
  RedisModule_ReplyWithStringBuffer(ctx, json.data(), json.size());
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
    NotifyJsonEvent(ctx, "json.del", argv[1]);
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithLongLong(ctx, 1);
  }

  auto matches = EvaluatePath(path, root);
  if (matches.empty()) return RedisModule_ReplyWithLongLong(ctx, 0);

  PruneAndSortMatchesForMutation(matches);

  int deleted = 0;
  for (auto& m : matches) {
    if (!m.parent) {
      RedisModule_DeleteKey(key);
      deleted++;
      break;
    }
    if (DeleteAtMatch(m)) deleted++;
  }

  if (deleted > 0) {
    NotifyJsonEvent(ctx, "json.del", argv[1]);
    RedisModule_ReplicateVerbatim(ctx);
  }
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
    NotifyJsonEvent(ctx, "json.arrappend", argv[1]);
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithLongLong(ctx, arr->ArrayLen());
  }

  // Find last matching array index so we can move `values` directly without Clone
  size_t last_arr_idx = matches.size();
  for (size_t i = 0; i < matches.size(); i++) {
    if (matches[i].value->IsArray()) last_arr_idx = i;
  }

  RedisModule_ReplyWithArray(ctx, matches.size());
  bool changed = false;
  for (size_t i = 0; i < matches.size(); i++) {
    auto& m = matches[i];
    if (!m.value->IsArray()) {
      RedisModule_ReplyWithNull(ctx);
      continue;
    }
    if (i == last_arr_idx) {
      for (auto* v : values) m.value->ArrayAppend(v);
      values.clear();
    } else {
      for (auto* v : values) m.value->ArrayAppend(v->Clone());
    }
    RedisModule_ReplyWithLongLong(ctx, m.value->ArrayLen());
    changed = true;
  }
  for (auto* v : values) JsonValue::Destroy(v);
  if (changed) {
    NotifyJsonEvent(ctx, "json.arrappend", argv[1]);
    RedisModule_ReplicateVerbatim(ctx);
  }
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
    int64_t len = static_cast<int64_t>(arr_val->ArrayLen());
    int64_t s = start_arg < 0 ? start_arg + len : start_arg;
    int64_t e = stop_set ? (stop_arg < 0 ? stop_arg + len : stop_arg) : len;
    if (s < 0) s = 0;
    if (e > len) e = len;
    for (int64_t i = s; i < e; i++) {
      if (i >= 0 && i < len &&
          arr_val->ArrayGet(static_cast<uint32_t>(i))->DeepEqual(search_val)) {
        return static_cast<long long>(i);
      }
    }
    return -1;
  };

  if (path.is_legacy) {
    if (matches.empty()) {
      JsonValue::Destroy(search_val);
      return RedisModule_ReplyWithError(ctx, "ERR path does not exist");
    }
    if (!matches[0].value->IsArray()) {
      JsonValue::Destroy(search_val);
      return RedisModule_ReplyWithError(ctx, "ERR path is not an array");
    }
    long long idx = FindIndex(matches[0].value);
    JsonValue::Destroy(search_val);
    return RedisModule_ReplyWithLongLong(ctx, idx);
  }

  RedisModule_ReplyWithArray(ctx, matches.size());
  for (auto& m : matches) {
    if (!m.value->IsArray()) {
      RedisModule_ReplyWithNull(ctx);
      continue;
    }
    RedisModule_ReplyWithLongLong(ctx, FindIndex(m.value));
  }
  JsonValue::Destroy(search_val);
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
    int64_t len = static_cast<int64_t>(arr->ArrayLen());
    int64_t idx = index_arg < 0 ? index_arg + len : index_arg;
    if (idx < 0 || idx > len) {
      for (auto* v : values) JsonValue::Destroy(v);
      return RedisModule_ReplyWithError(ctx, "ERR index out of range");
    }
    for (size_t i = 0; i < values.size(); i++) {
      arr->ArrayInsert(static_cast<uint32_t>(idx + static_cast<int64_t>(i)), values[i]);
    }
    NotifyJsonEvent(ctx, "json.arrinsert", argv[1]);
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
    int64_t len = static_cast<int64_t>(arr->ArrayLen());
    int64_t idx = index_arg < 0 ? index_arg + len : index_arg;
    if (idx < 0 || idx > len) {
      RedisModule_ReplyWithError(ctx, "ERR index out of range");
      continue;
    }
    for (size_t i = 0; i < values.size(); i++) {
      arr->ArrayInsert(static_cast<uint32_t>(idx + static_cast<int64_t>(i)),
                       values[i]->Clone());
    }
    RedisModule_ReplyWithLongLong(ctx, arr->ArrayLen());
    changed = true;
  }
  for (auto* v : values) JsonValue::Destroy(v);
  if (changed) {
    NotifyJsonEvent(ctx, "json.arrinsert", argv[1]);
    RedisModule_ReplicateVerbatim(ctx);
  }
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

  long long pop_index_arg = -1;
  if (argc == 4) {
    if (RedisModule_StringToLongLong(argv[3], &pop_index_arg) != REDISMODULE_OK)
      return RedisModule_ReplyWithError(ctx, "ERR invalid index");
  }

  auto PopResolved = [&](JsonValue* arr) -> JsonValue* {
    int64_t len = static_cast<int64_t>(arr->ArrayLen());
    if (len == 0) return nullptr;
    int64_t idx = pop_index_arg < 0 ? pop_index_arg + len : pop_index_arg;
    if (idx < 0) idx = 0;
    if (idx >= len) idx = len - 1;
    return arr->ArrayPop(static_cast<int32_t>(idx));
  };

  auto matches = EvaluatePath(path, root);

  if (path.is_legacy) {
    if (matches.empty() || !matches[0].value->IsArray())
      return RedisModule_ReplyWithNull(ctx);
    auto* arr = matches[0].value;
    if (arr->ArrayLen() == 0) return RedisModule_ReplyWithNull(ctx);
    auto* popped = PopResolved(arr);
    if (!popped) return RedisModule_ReplyWithNull(ctx);
    ReplyWithJsonValue(ctx, popped);
    JsonValue::Destroy(popped);
    NotifyJsonEvent(ctx, "json.arrpop", argv[1]);
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
    auto* popped = PopResolved(m.value);
    if (!popped) { RedisModule_ReplyWithNull(ctx); continue; }
    ReplyWithJsonValue(ctx, popped);
    JsonValue::Destroy(popped);
    changed = true;
  }
  if (changed) {
    NotifyJsonEvent(ctx, "json.arrpop", argv[1]);
    RedisModule_ReplicateVerbatim(ctx);
  }
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

  auto TrimResolved = [&](JsonValue* arr) {
    int64_t len = static_cast<int64_t>(arr->ArrayLen());
    int64_t s = start_arg < 0 ? start_arg + len : start_arg;
    int64_t e = stop_arg < 0 ? stop_arg + len : stop_arg;
    if (s < 0) s = 0;
    if (e < 0 || s >= len || s > e) {
      arr->Clear();
    } else {
      if (e >= len) e = len - 1;
      arr->ArrayTrim(static_cast<uint32_t>(s), static_cast<uint32_t>(e));
    }
  };

  if (path.is_legacy) {
    if (matches.empty() || !matches[0].value->IsArray())
      return RedisModule_ReplyWithNull(ctx);
    auto* arr = matches[0].value;
    TrimResolved(arr);
    NotifyJsonEvent(ctx, "json.arrtrim", argv[1]);
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithLongLong(ctx, arr->ArrayLen());
  }

  RedisModule_ReplyWithArray(ctx, matches.size());
  bool changed = false;
  for (auto& m : matches) {
    if (!m.value->IsArray()) { RedisModule_ReplyWithNull(ctx); continue; }
    auto* arr = m.value;
    TrimResolved(arr);
    RedisModule_ReplyWithLongLong(ctx, arr->ArrayLen());
    changed = true;
  }
  if (changed) {
    NotifyJsonEvent(ctx, "json.arrtrim", argv[1]);
    RedisModule_ReplicateVerbatim(ctx);
  }
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
    if (!str_val->StringAppend(append_sv)) {
      JsonValue::Destroy(append_val);
      return RedisModule_ReplyWithError(ctx, "ERR string append failed or overflow");
    }
    size_t new_len = str_val->GetString().size();
    JsonValue::Destroy(append_val);
    NotifyJsonEvent(ctx, "json.strappend", argv[1]);
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(new_len));
  }

  RedisModule_ReplyWithArray(ctx, matches.size());
  bool changed = false;
  for (auto& m : matches) {
    if (!m.value->IsString()) { RedisModule_ReplyWithNull(ctx); continue; }
    if (!m.value->StringAppend(append_sv)) {
      RedisModule_ReplyWithNull(ctx);
      continue;
    }
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(m.value->GetString().size()));
    changed = true;
  }
  JsonValue::Destroy(append_val);
  if (changed) {
    NotifyJsonEvent(ctx, "json.strappend", argv[1]);
    RedisModule_ReplicateVerbatim(ctx);
  }
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

  auto IncrementValue = [&](JsonValue* v, bool& overflow) -> JsonValue* {
    overflow = false;
    if (v->IsInteger() && incr_is_int) {
      int64_t old_val = v->GetInteger();
      if ((incr_int > 0 && old_val > INT64_MAX - incr_int) ||
          (incr_int < 0 && old_val < INT64_MIN - incr_int)) {
        double res = static_cast<double>(old_val) + incr;
        if (!std::isfinite(res)) { overflow = true; return nullptr; }
        return JsonValue::CreateNumber(res);
      }
      return JsonValue::CreateInteger(old_val + incr_int);
    }
    double res = v->GetNumber() + incr;
    if (!std::isfinite(res)) { overflow = true; return nullptr; }
    return JsonValue::CreateNumber(res);
  };

  if (path.is_legacy) {
    if (matches.empty() || !matches[0].value->IsNumber())
      return RedisModule_ReplyWithError(ctx, "ERR path does not exist or is not a number");

    bool overflow = false;
    auto* new_val = IncrementValue(matches[0].value, overflow);
    if (overflow || !new_val)
      return RedisModule_ReplyWithError(ctx, "ERR result is not a finite number");
    if (matches[0].parent) SetAtMatch(matches[0], new_val);
    else RedisModule_ModuleTypeSetValue(key, JsonModuleType, new_val);

    auto result = JsonSerialize(new_val);
    NotifyJsonEvent(ctx, "json.numincrby", argv[1]);
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithStringBuffer(ctx, result.data(), result.size());
  }

  // Pre-check overflow before mutating any match in JSONPath mode
  std::vector<JsonValue*> computed(matches.size(), nullptr);
  for (size_t i = 0; i < matches.size(); i++) {
    auto* v = matches[i].value;
    if (!v->IsNumber()) continue;
    bool overflow = false;
    computed[i] = IncrementValue(v, overflow);
    if (overflow || !computed[i]) {
      for (auto* c : computed) JsonValue::Destroy(c);
      return RedisModule_ReplyWithError(ctx, "ERR result is not a finite number");
    }
  }

  std::string reply = "[";
  bool changed = false;
  for (size_t i = 0; i < matches.size(); i++) {
    if (i > 0) reply += ",";
    if (!computed[i]) {
      reply += "null";
      continue;
    }
    reply += JsonSerialize(computed[i]);
    if (matches[i].parent) SetAtMatch(matches[i], computed[i]);
    else RedisModule_ModuleTypeSetValue(key, JsonModuleType, computed[i]);
    changed = true;
  }
  reply += "]";
  if (changed) {
    NotifyJsonEvent(ctx, "json.numincrby", argv[1]);
    RedisModule_ReplicateVerbatim(ctx);
  }
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
  bool mult_is_int = mult_val->IsInteger();
  int64_t mult_int = mult_is_int ? mult_val->GetInteger() : 0;
  JsonValue::Destroy(mult_val);

  auto matches = EvaluatePath(path, root);

  auto MultiplyValue = [&](JsonValue* v, bool& overflow) -> JsonValue* {
    overflow = false;
    if (v->IsInteger() && mult_is_int) {
      int64_t prod = 0;
      if (!__builtin_mul_overflow(v->GetInteger(), mult_int, &prod)) {
        return JsonValue::CreateInteger(prod);
      }
    }
    double res = v->GetNumber() * mult;
    if (!std::isfinite(res)) { overflow = true; return nullptr; }
    return JsonValue::CreateNumber(res);
  };

  if (path.is_legacy) {
    if (matches.empty() || !matches[0].value->IsNumber())
      return RedisModule_ReplyWithError(ctx, "ERR path does not exist or is not a number");

    bool overflow = false;
    auto* new_val = MultiplyValue(matches[0].value, overflow);
    if (overflow || !new_val)
      return RedisModule_ReplyWithError(ctx, "ERR result is not a finite number");
    if (matches[0].parent) SetAtMatch(matches[0], new_val);
    else RedisModule_ModuleTypeSetValue(key, JsonModuleType, new_val);
    auto reply = JsonSerialize(new_val);
    NotifyJsonEvent(ctx, "json.nummultby", argv[1]);
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithStringBuffer(ctx, reply.data(), reply.size());
  }

  std::vector<JsonValue*> computed(matches.size(), nullptr);
  for (size_t i = 0; i < matches.size(); i++) {
    auto* v = matches[i].value;
    if (!v->IsNumber()) continue;
    bool overflow = false;
    computed[i] = MultiplyValue(v, overflow);
    if (overflow || !computed[i]) {
      for (auto* c : computed) JsonValue::Destroy(c);
      return RedisModule_ReplyWithError(ctx, "ERR result is not a finite number");
    }
  }

  std::string reply = "[";
  bool changed = false;
  for (size_t i = 0; i < matches.size(); i++) {
    if (i > 0) reply += ",";
    if (!computed[i]) { reply += "null"; continue; }
    reply += JsonSerialize(computed[i]);
    if (matches[i].parent) SetAtMatch(matches[i], computed[i]);
    else RedisModule_ModuleTypeSetValue(key, JsonModuleType, computed[i]);
    changed = true;
  }
  reply += "]";
  if (changed) {
    NotifyJsonEvent(ctx, "json.nummultby", argv[1]);
    RedisModule_ReplicateVerbatim(ctx);
  }
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
      RedisModule_ReplyWithStringBuffer(ctx, entries[i].KeyData(), entries[i].KeyLen());
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
      RedisModule_ReplyWithStringBuffer(ctx, entries[i].KeyData(), entries[i].KeyLen());
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
  if (cleared > 0) {
    NotifyJsonEvent(ctx, "json.clear", argv[1]);
    RedisModule_ReplicateVerbatim(ctx);
  }
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
    NotifyJsonEvent(ctx, "json.toggle", argv[1]);
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
  if (changed) {
    NotifyJsonEvent(ctx, "json.toggle", argv[1]);
    RedisModule_ReplicateVerbatim(ctx);
  }
  return REDISMODULE_OK;
}

// --- JSON.MGET ---
static int CmdMGet(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

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
      std::vector<const JsonValue*> ptrs;
      ptrs.reserve(matches.size());
      for (const auto& m : matches) ptrs.push_back(m.value);
      auto json = JsonSerializeArray(ptrs.data(), ptrs.size());
      RedisModule_ReplyWithStringBuffer(ctx, json.data(), json.size());
    }
  }
  return REDISMODULE_OK;
}

// --- JSON.MSET ---
static int CmdMSet(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 4 || (argc - 1) % 3 != 0) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  int num_triplets = (argc - 1) / 3;

  struct SetOp {
    int key_idx;
    JsonPath path;
    JsonValue* value;
  };

  std::vector<SetOp> ops;

  for (int i = 0; i < num_triplets; i++) {
    int base = 1 + i * 3;
    SetOp op;
    op.key_idx = base;
    if (!ParsePathFromArg(ctx, argv[base + 1], op.path)) {
      for (auto& prev : ops) JsonValue::Destroy(prev.value);
      return REDISMODULE_OK;
    }
    op.value = ParseJsonArg(ctx, argv[base + 2]);
    if (!op.value) {
      for (auto& prev : ops) JsonValue::Destroy(prev.value);
      return REDISMODULE_OK;
    }
    ops.push_back(std::move(op));
  }

  // Phase 1: Validate all keys and paths before applying any mutation (atomic MSET).
  std::unordered_set<std::string_view> will_exist;
  for (auto& op : ops) {
    auto key_sv = ArgView(argv[op.key_idx]);
    auto* key = static_cast<RedisModuleKey*>(
      RedisModule_OpenKey(ctx, argv[op.key_idx], REDISMODULE_READ));
    int key_type = RedisModule_KeyType(key);

    if (key_type == REDISMODULE_KEYTYPE_EMPTY) {
      if (IsRootOnlyPath(op.path)) {
        will_exist.insert(key_sv);
      } else if (!will_exist.count(key_sv)) {
        for (auto& rem : ops) JsonValue::Destroy(rem.value);
        return RedisModule_ReplyWithError(ctx, "ERR new objects must be created at the root");
      }
    } else if (key_type == REDISMODULE_KEYTYPE_MODULE) {
      if (!GetJsonValue(key)) {
        for (auto& rem : ops) JsonValue::Destroy(rem.value);
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
      }
    } else {
      for (auto& rem : ops) JsonValue::Destroy(rem.value);
      return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    }
  }

  // Phase 2: All checks passed — apply mutations.
  for (auto& op : ops) {
    auto* key = static_cast<RedisModuleKey*>(
      RedisModule_OpenKey(ctx, argv[op.key_idx], REDISMODULE_READ | REDISMODULE_WRITE));
    int key_type = RedisModule_KeyType(key);

    if (key_type == REDISMODULE_KEYTYPE_EMPTY || IsRootOnlyPath(op.path)) {
      RedisModule_ModuleTypeSetValue(key, JsonModuleType, op.value);
      op.value = nullptr;
    } else {
      auto* existing = GetJsonValue(key);
      auto matches = EvaluatePath(op.path, existing);
      if (!matches.empty()) {
        PruneAndSortMatchesForMutation(matches);
        for (size_t j = 0; j < matches.size(); j++) {
          auto* val = (j == matches.size() - 1) ? op.value : op.value->Clone();
          if (!matches[j].parent) {
            RedisModule_ModuleTypeSetValue(key, JsonModuleType, val);
          } else {
            SetAtMatch(matches[j], val);
          }
        }
        op.value = nullptr;
      } else {
        if (TryCreateAtLeafKey(op.path, existing, op.value)) {
          op.value = nullptr;
        }
      }
    }
  }

  for (auto& op : ops) JsonValue::Destroy(op.value);
  for (int k = 1; k + 2 < argc; k += 3) {
    NotifyJsonEvent(ctx, "json.set", argv[k]);
  }
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
    auto pk = patch_entries[i].KeyView();
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
        child_match.has_object_key = true;
        MergePatch(existing, pv, target, child_match, key);
      } else {
        target->ObjectSet(patch_entries[i].KeyData(), patch_entries[i].KeyLen(), pv->Clone());
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
      NotifyJsonEvent(ctx, "json.merge", argv[1]);
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
    if (patch->IsNull()) {
      JsonValue::Destroy(patch);
      return RedisModule_ReplyWithSimpleString(ctx, "OK");
    }
    if (TryCreateAtLeafKey(path, root, patch)) {
      NotifyJsonEvent(ctx, "json.merge", argv[1]);
      RedisModule_ReplicateVerbatim(ctx);
      return RedisModule_ReplyWithSimpleString(ctx, "OK");
    }
    JsonValue::Destroy(patch);
    return RedisModule_ReplyWithError(ctx, "ERR path does not exist");
  }

  PruneAndSortMatchesForMutation(matches);
  for (auto& m : matches) {
    MergePatch(m.value, patch, m.parent, m, key);
  }

  JsonValue::Destroy(patch);
  NotifyJsonEvent(ctx, "json.merge", argv[1]);
  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// --- JSON.DEBUG ---
static size_t CalcDepth(const JsonValue* v, int depth = 0) {
  if (!v || depth > kMaxJsonDepth) return 0;
  size_t max_child = 0;
  if (v->IsArray()) {
    for (uint32_t i = 0; i < v->ArrayLen(); i++) {
      max_child = std::max(max_child, CalcDepth(v->ArrayGet(i), depth + 1));
    }
    return 1 + max_child;
  }
  if (v->IsObject()) {
    auto* entries = v->ObjectEntries();
    for (uint32_t i = 0; i < v->ObjectLen(); i++) {
      max_child = std::max(max_child, CalcDepth(entries[i].value, depth + 1));
    }
    return 1 + max_child;
  }
  return 0;
}

static size_t CalcFields(const JsonValue* v, int depth = 0) {
  if (!v || depth > kMaxJsonDepth) return 0;
  if (v->IsArray()) {
    size_t total = v->ArrayLen();
    for (uint32_t i = 0; i < v->ArrayLen(); i++) {
      total += CalcFields(v->ArrayGet(i), depth + 1);
    }
    return total;
  }
  if (v->IsObject()) {
    size_t total = v->ObjectLen();
    auto* entries = v->ObjectEntries();
    for (uint32_t i = 0; i < v->ObjectLen(); i++) {
      total += CalcFields(entries[i].value, depth + 1);
    }
    return total;
  }
  return 0;
}

static int CmdDebug(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto subcmd = ArgView(argv[1]);

  if (MatchArg(subcmd, "MEMORY") || MatchArg(subcmd, "DEPTH") || MatchArg(subcmd, "FIELDS")) {
    if (argc < 3 || argc > 4) return RedisModule_WrongArity(ctx);

    auto* root = OpenReadOnly(ctx, argv[2], nullptr);
    if (!root) return REDISMODULE_OK;

    JsonPath path;
    if (argc == 4) {
      if (!ParsePathFromArg(ctx, argv[3], path)) return REDISMODULE_OK;
    } else {
      path.segments.push_back({PathSegType::kRoot, {}, 0, {}, {}, {}});
      path.is_legacy = true;
    }

    auto matches = EvaluatePath(path, root);
    bool is_mem = MatchArg(subcmd, "MEMORY");
    bool is_depth = MatchArg(subcmd, "DEPTH");

    auto MetricOf = [&](const JsonValue* v) -> long long {
      if (is_mem) return static_cast<long long>(v->MemoryUsage());
      if (is_depth) return static_cast<long long>(CalcDepth(v));
      return static_cast<long long>(CalcFields(v));
    };

    if (path.is_legacy) {
      if (matches.empty()) return RedisModule_ReplyWithLongLong(ctx, 0);
      return RedisModule_ReplyWithLongLong(ctx, MetricOf(matches[0].value));
    }

    RedisModule_ReplyWithArray(ctx, matches.size());
    for (auto& m : matches) {
      RedisModule_ReplyWithLongLong(ctx, MetricOf(m.value));
    }
    return REDISMODULE_OK;
  }

  if (MatchArg(subcmd, "HELP")) {
    RedisModule_ReplyWithArray(ctx, 4);
    RedisModule_ReplyWithSimpleString(ctx, "JSON.DEBUG MEMORY <key> [path] - report memory usage");
    RedisModule_ReplyWithSimpleString(ctx, "JSON.DEBUG DEPTH <key> [path] - report nesting depth");
    RedisModule_ReplyWithSimpleString(ctx, "JSON.DEBUG FIELDS <key> [path] - report total field/element count");
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
        RedisModule_ReplyWithStringBuffer(ctx, entries[i].KeyData(), entries[i].KeyLen());
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
    int firstkey;
    int lastkey;
    int keystep;
  };

  CmdEntry commands[] = {
    {"JSON.SET",       CmdSet,       "write deny-oom", 1,  1, 1},
    {"JSON.GET",       CmdGet,       "readonly",       1,  1, 1},
    {"JSON.DEL",       CmdDel,       "write",          1,  1, 1},
    {"JSON.FORGET",    CmdDel,       "write",          1,  1, 1},
    {"JSON.TYPE",      CmdType,      "readonly",       1,  1, 1},
    {"JSON.ARRAPPEND", CmdArrAppend, "write deny-oom", 1,  1, 1},
    {"JSON.ARRINDEX",  CmdArrIndex,  "readonly",       1,  1, 1},
    {"JSON.ARRINSERT", CmdArrInsert, "write deny-oom", 1,  1, 1},
    {"JSON.ARRLEN",    CmdArrLen,    "readonly",       1,  1, 1},
    {"JSON.ARRPOP",    CmdArrPop,    "write",          1,  1, 1},
    {"JSON.ARRTRIM",   CmdArrTrim,   "write",          1,  1, 1},
    {"JSON.STRAPPEND", CmdStrAppend, "write deny-oom", 1,  1, 1},
    {"JSON.STRLEN",    CmdStrLen,    "readonly",       1,  1, 1},
    {"JSON.NUMINCRBY", CmdNumIncrBy, "write deny-oom", 1,  1, 1},
    {"JSON.NUMMULTBY", CmdNumMultBy, "write deny-oom", 1,  1, 1},
    {"JSON.OBJKEYS",   CmdObjKeys,   "readonly",       1,  1, 1},
    {"JSON.OBJLEN",    CmdObjLen,    "readonly",       1,  1, 1},
    {"JSON.CLEAR",     CmdClear,     "write",          1,  1, 1},
    {"JSON.TOGGLE",    CmdToggle,    "write",          1,  1, 1},
    {"JSON.MGET",      CmdMGet,      "readonly",       1, -2, 1},
    {"JSON.MSET",      CmdMSet,      "write deny-oom", 1, -1, 3},
    {"JSON.MERGE",     CmdMerge,     "write deny-oom", 1,  1, 1},
    {"JSON.DEBUG",     CmdDebug,     "readonly",       0,  0, 0},
    {"JSON.RESP",      CmdResp,      "readonly",       1,  1, 1},
  };

  for (auto& cmd : commands) {
    if (RedisModule_CreateCommand(ctx, cmd.name, cmd.handler, cmd.flags,
                                  cmd.firstkey, cmd.lastkey, cmd.keystep) ==
        REDISMODULE_ERR) {
      return REDISMODULE_ERR;
    }
  }
  return REDISMODULE_OK;
}
