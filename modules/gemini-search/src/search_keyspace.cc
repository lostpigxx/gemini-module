#include "search_internal.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

static bool g_is_loading = false;

static void RemoveIndexedKey(IndexEntry& entry, const std::string& key_name) {
  std::unique_lock<std::shared_mutex> idx_lk(entry.Mutex());
  if (!entry.doc_store.Contains(key_name)) return;
  RemoveDocFromIndices(entry, key_name);
  entry.doc_store.Remove(key_name);
}

void IndexHashKey(RedisModuleCtx* ctx, IndexEntry& entry,
                  const std::string& key_name) {
  RedisModuleString* rms_key =
      RedisModule_CreateString(ctx, key_name.c_str(), key_name.size());
  auto* rkey = static_cast<RedisModuleKey*>(
      RedisModule_OpenKey(ctx, rms_key, REDISMODULE_READ));
  if (!rkey || RedisModule_KeyType(rkey) != REDISMODULE_KEYTYPE_HASH) {
    if (rkey) RedisModule_CloseKey(rkey);
    RedisModule_FreeString(ctx, rms_key);
    RemoveIndexedKey(entry, key_name);
    return;
  }

  std::unordered_map<std::string, std::string> doc_fields;
  for (auto& fspec : entry.spec.fields) {
    if (fspec.type == FieldType::kVector) continue;
    RedisModuleString* val = nullptr;
    RedisModule_HashGet(rkey, REDISMODULE_HASH_CFIELDS, fspec.name.c_str(),
                        &val, NULL);
    if (val) {
      size_t len = 0;
      const char* data = RedisModule_StringPtrLen(val, &len);
      doc_fields[fspec.name] = std::string(data, len);
      RedisModule_FreeString(ctx, val);
    }
  }
  RedisModule_CloseKey(rkey);
  RedisModule_FreeString(ctx, rms_key);

  if (doc_fields.empty() || !PassesIndexFilter(entry.spec, doc_fields)) {
    RemoveIndexedKey(entry, key_name);
    return;
  }

  {
    std::unique_lock<std::shared_mutex> idx_lk(entry.Mutex());
    RemoveDocFromIndices(entry, key_name);
    entry.doc_store.Add(key_name, doc_fields);
    AddDocToIndices(entry, key_name, doc_fields);
  }
}

static bool IsJsonWs(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static std::string TrimJsonWs(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && IsJsonWs(s[b])) b++;
  size_t e = s.size();
  while (e > b && IsJsonWs(s[e - 1])) e--;
  return s.substr(b, e - b);
}

static std::string ParseJsonStringToken(const std::string& s, size_t& i) {
  std::string out;
  if (i >= s.size() || s[i] != '"') return out;
  i++;  // skip opening '"'
  while (i < s.size()) {
    char c = s[i++];
    if (c == '\\' && i < s.size()) {
      char esc = s[i++];
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default: out.push_back(esc); break;
      }
    } else if (c == '"') {
      break;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

static void ExtractJsonValuesRec(const std::string& s, size_t& i,
                                 std::vector<std::string>& out) {
  while (i < s.size() && IsJsonWs(s[i])) i++;
  if (i >= s.size()) return;

  if (s[i] == '[') {
    i++;  // skip '['
    while (i < s.size()) {
      while (i < s.size() && (IsJsonWs(s[i]) || s[i] == ',')) i++;
      if (i >= s.size() || s[i] == ']') {
        if (i < s.size()) i++;
        break;
      }
      ExtractJsonValuesRec(s, i, out);
    }
    return;
  }

  if (s[i] == '"') {
    out.push_back(ParseJsonStringToken(s, i));
    return;
  }

  if (s[i] == '{') {
    // Skip nested JSON object
    int depth = 0;
    bool in_str = false;
    while (i < s.size()) {
      char c = s[i++];
      if (in_str) {
        if (c == '\\' && i < s.size()) i++;
        else if (c == '"') in_str = false;
      } else if (c == '"') {
        in_str = true;
      } else if (c == '{') {
        depth++;
      } else if (c == '}') {
        depth--;
        if (depth == 0) break;
      }
    }
    return;
  }

  size_t start = i;
  while (i < s.size() && s[i] != ',' && s[i] != ']' && s[i] != '}') i++;
  std::string val = TrimJsonWs(s.substr(start, i - start));
  if (!val.empty() && val != "null") out.push_back(std::move(val));
}

static std::vector<std::string> ExtractJsonArray(const std::string& raw) {
  std::string trimmed = TrimJsonWs(raw);
  std::vector<std::string> results;
  if (trimmed.empty()) return results;
  size_t i = 0;
  ExtractJsonValuesRec(trimmed, i, results);
  return results;
}

static std::string ExtractJsonScalar(const std::string& raw) {
  auto vals = ExtractJsonArray(raw);
  return vals.empty() ? std::string() : vals.front();
}

void IndexJsonKey(RedisModuleCtx* ctx, IndexEntry& entry,
                  const std::string& key_name) {
  std::unordered_map<std::string, std::string> doc_fields;

  for (auto& fspec : entry.spec.fields) {
    if (fspec.type == FieldType::kVector) continue;
    const std::string& path =
        fspec.json_path.empty() ? fspec.name : fspec.json_path;

    RedisModuleString* rms_key =
        RedisModule_CreateString(ctx, key_name.c_str(), key_name.size());
    RedisModuleString* rms_path =
        RedisModule_CreateString(ctx, path.c_str(), path.size());
    RedisModuleCallReply* reply =
        RedisModule_Call(ctx, "JSON.GET", "ss", rms_key, rms_path);
    RedisModule_FreeString(ctx, rms_key);
    RedisModule_FreeString(ctx, rms_path);

    if (!reply) continue;
    if (RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_STRING) {
      RedisModule_FreeCallReply(reply);
      continue;
    }

    size_t len = 0;
    const char* data = RedisModule_CallReplyStringPtr(reply, &len);
    std::string raw = (data && len > 0) ? std::string(data, len) : std::string();
    RedisModule_FreeCallReply(reply);
    if (raw.empty()) continue;

    if (fspec.type == FieldType::kTag) {
      auto vals = ExtractJsonArray(raw);
      std::string sep(1, fspec.tag_separator);
      for (auto& v : vals) {
        if (!v.empty()) {
          if (doc_fields.find(fspec.name) == doc_fields.end())
            doc_fields[fspec.name] = v;
          else
            doc_fields[fspec.name] += sep + v;
        }
      }
    } else if (fspec.type == FieldType::kText) {
      auto vals = ExtractJsonArray(raw);
      for (auto& v : vals) {
        if (!v.empty()) {
          if (doc_fields.find(fspec.name) == doc_fields.end())
            doc_fields[fspec.name] = v;
          else
            doc_fields[fspec.name] += " " + v;
        }
      }
    } else if (fspec.type == FieldType::kNumeric) {
      auto vals = ExtractJsonArray(raw);
      for (auto& v : vals) {
        double num = 0;
        if (TryParseDouble(v, num)) {
          if (doc_fields.find(fspec.name) == doc_fields.end())
            doc_fields[fspec.name] = v;
          else
            doc_fields[fspec.name] += "," + v;
        }
      }
    } else {
      std::string scalar = ExtractJsonScalar(raw);
      if (!scalar.empty()) doc_fields[fspec.name] = std::move(scalar);
    }
  }

  if (doc_fields.empty() || !PassesIndexFilter(entry.spec, doc_fields)) {
    RemoveIndexedKey(entry, key_name);
    return;
  }

  {
    std::unique_lock<std::shared_mutex> idx_lk(entry.Mutex());
    RemoveDocFromIndices(entry, key_name);
    entry.doc_store.Add(key_name, doc_fields);
    AddDocToIndices(entry, key_name, doc_fields);
  }
}

struct ScanPrivdata {
  RedisModuleCtx* ctx;
  IndexEntry* entry;
};

static void ScanCallback(RedisModuleCtx* /*ctx*/, RedisModuleString* keyname,
                         RedisModuleKey* /*key*/, void* privdata) {
  auto* pd = static_cast<ScanPrivdata*>(privdata);
  size_t len = 0;
  const char* data = RedisModule_StringPtrLen(keyname, &len);
  std::string key_str(data, len);
  if (pd->entry->spec.MatchesPrefix(key_str)) {
    if (pd->entry->spec.source_type == IndexSourceType::kJson)
      IndexJsonKey(pd->ctx, *pd->entry, key_str);
    else
      IndexHashKey(pd->ctx, *pd->entry, key_str);
  }
}

void ScanExistingKeys(RedisModuleCtx* ctx, IndexEntry& entry) {
  auto* cursor = RedisModule_ScanCursorCreate();
  ScanPrivdata pd{ctx, &entry};
  while (RedisModule_Scan(ctx, cursor, ScanCallback, &pd)) {}
  RedisModule_ScanCursorDestroy(cursor);
}

int OnKeyspaceEvent(RedisModuleCtx* ctx, int type, const char* event,
                    RedisModuleString* key) {
  (void)type;
  if (g_is_loading) return REDISMODULE_OK;

  size_t len = 0;
  const char* data = RedisModule_StringPtrLen(key, &len);
  std::string key_str(data, len);

  bool is_del = (strcmp(event, "del") == 0 || strcmp(event, "unlink") == 0 ||
                 strcmp(event, "expired") == 0 || strcmp(event, "evicted") == 0 ||
                 strcmp(event, "rename_from") == 0);
  bool is_generic_reindex = (strcmp(event, "rename_to") == 0 ||
                             strcmp(event, "restore") == 0 ||
                             strcmp(event, "copy_to") == 0);
  bool is_hash_write = (strcmp(event, "hset") == 0 || strcmp(event, "hdel") == 0 ||
                        strcmp(event, "hincrby") == 0 ||
                        strcmp(event, "hincrbyfloat") == 0 ||
                        strcmp(event, "hmset") == 0 || strcmp(event, "hsetnx") == 0);
  bool is_json_write = (strcmp(event, "json.set") == 0 || strcmp(event, "json.del") == 0 ||
                        strcmp(event, "json.forget") == 0 ||
                        strcmp(event, "json.merge") == 0 ||
                        strcmp(event, "json.clear") == 0 ||
                        strcmp(event, "json.toggle") == 0 ||
                        strcmp(event, "json.arrappend") == 0 ||
                        strcmp(event, "json.arrinsert") == 0 ||
                        strcmp(event, "json.arrpop") == 0 ||
                        strcmp(event, "json.arrtrim") == 0 ||
                        strcmp(event, "json.numincrby") == 0 ||
                        strcmp(event, "json.nummultby") == 0 ||
                        strcmp(event, "json.strappend") == 0);

  for (auto& [idx_name, entry] : GetIndices()) {
    if (!entry.spec.HasPrefixes()) continue;
    if (!entry.spec.MatchesPrefix(key_str)) continue;

    if (is_del) {
      RemoveIndexedKey(entry, key_str);
    } else if (entry.spec.source_type == IndexSourceType::kJson) {
      if (is_json_write || is_generic_reindex) {
        IndexJsonKey(ctx, entry, key_str);
      }
    } else if (is_hash_write || is_generic_reindex) {
      IndexHashKey(ctx, entry, key_str);
    }
  }

  return REDISMODULE_OK;
}

void OnFlushDbEvent(RedisModuleCtx* /*ctx*/, RedisModuleEvent /*eid*/,
                    uint64_t subevent, void* /*data*/) {
  if (subevent == REDISMODULE_SUBEVENT_FLUSHDB_END) {
    GetCursors().clear();
    GetAliases().clear();
    GetSuggestDicts().clear();
    GetTermDicts().clear();
  }
}

void OnLoadingEvent(RedisModuleCtx* /*ctx*/, RedisModuleEvent /*eid*/,
                    uint64_t subevent, void* /*data*/) {
  if (subevent == REDISMODULE_SUBEVENT_LOADING_RDB_START ||
      subevent == REDISMODULE_SUBEVENT_LOADING_AOF_START ||
      subevent == REDISMODULE_SUBEVENT_LOADING_REPL_START) {
    g_is_loading = true;
  } else if (subevent == REDISMODULE_SUBEVENT_LOADING_ENDED ||
             subevent == REDISMODULE_SUBEVENT_LOADING_FAILED) {
    g_is_loading = false;
  }
}
