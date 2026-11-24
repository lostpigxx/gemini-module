#include "search_internal.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

void IndexHashKey(RedisModuleCtx* ctx, IndexEntry& entry,
                         const std::string& key_name) {
  RedisModuleString* rms_key = RedisModule_CreateString(ctx, key_name.c_str(), key_name.size());
  auto* rkey = static_cast<RedisModuleKey*>(
      RedisModule_OpenKey(ctx, rms_key, REDISMODULE_READ));
  if (!rkey || RedisModule_KeyType(rkey) != REDISMODULE_KEYTYPE_HASH) return;

  std::unordered_map<std::string, std::string> doc_fields;
  for (auto& fspec : entry.spec.fields) {
    if (fspec.type == FieldType::kVector) continue;
    RedisModuleString* val = nullptr;
    RedisModule_HashGet(rkey, REDISMODULE_HASH_CFIELDS, fspec.name.c_str(), &val, NULL);
    if (val) {
      size_t len = 0;
      const char* data = RedisModule_StringPtrLen(val, &len);
      doc_fields[fspec.name] = std::string(data, len);
    }
  }

  if (doc_fields.empty()) return;

  RemoveDocFromIndices(entry, key_name);
  entry.doc_store.Add(key_name, doc_fields);
  AddDocToIndices(entry, key_name, doc_fields);
}

static std::string StripJsonQuotes(const std::string& s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
    return s.substr(1, s.size() - 2);
  return s;
}

static std::string ExtractJsonScalar(const std::string& raw) {
  std::string trimmed = raw;
  while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
    trimmed.erase(trimmed.begin());
  while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
    trimmed.pop_back();
  if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']')
    trimmed = trimmed.substr(1, trimmed.size() - 2);
  while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
    trimmed.erase(trimmed.begin());
  while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
    trimmed.pop_back();
  return StripJsonQuotes(trimmed);
}

static std::vector<std::string> ExtractJsonArray(const std::string& raw) {
  std::string trimmed = raw;
  while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
    trimmed.erase(trimmed.begin());
  while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
    trimmed.pop_back();
  if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']')
    return {StripJsonQuotes(trimmed)};

  std::vector<std::string> results;
  size_t i = 1;
  while (i < trimmed.size() - 1) {
    while (i < trimmed.size() - 1 && (trimmed[i] == ' ' || trimmed[i] == ',')) i++;
    if (i >= trimmed.size() - 1) break;
    if (trimmed[i] == '"') {
      i++;
      size_t start = i;
      while (i < trimmed.size() - 1 && trimmed[i] != '"') i++;
      results.push_back(trimmed.substr(start, i - start));
      if (i < trimmed.size() - 1) i++;
    } else {
      size_t start = i;
      while (i < trimmed.size() - 1 && trimmed[i] != ',' && trimmed[i] != ']') i++;
      std::string val = trimmed.substr(start, i - start);
      while (!val.empty() && val.back() == ' ') val.pop_back();
      if (val != "null") results.push_back(std::move(val));
    }
  }
  return results;
}

void IndexJsonKey(RedisModuleCtx* ctx, IndexEntry& entry,
                         const std::string& key_name) {
  std::unordered_map<std::string, std::string> doc_fields;

  for (auto& fspec : entry.spec.fields) {
    if (fspec.type == FieldType::kVector) continue;
    const std::string& path = fspec.json_path.empty() ? fspec.name : fspec.json_path;

    RedisModuleString* rms_key = RedisModule_CreateString(ctx, key_name.c_str(), key_name.size());
    RedisModuleString* rms_path = RedisModule_CreateString(ctx, path.c_str(), path.size());
    RedisModuleCallReply* reply = RedisModule_Call(ctx, "JSON.GET", "ss", rms_key, rms_path);

    if (!reply || RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_STRING)
      continue;

    size_t len = 0;
    const char* data = RedisModule_CallReplyStringPtr(reply, &len);
    if (!data || len == 0) continue;
    std::string raw(data, len);

    if (fspec.type == FieldType::kTag) {
      auto vals = ExtractJsonArray(raw);
      for (auto& v : vals) {
        if (!v.empty()) {
          if (doc_fields.find(fspec.name) == doc_fields.end())
            doc_fields[fspec.name] = v;
          else
            doc_fields[fspec.name] += "," + v;
        }
      }
    } else {
      doc_fields[fspec.name] = ExtractJsonScalar(raw);
    }
  }

  if (doc_fields.empty()) return;

  RemoveDocFromIndices(entry, key_name);
  entry.doc_store.Add(key_name, doc_fields);
  AddDocToIndices(entry, key_name, doc_fields);
}

static void RemoveHashKey(IndexEntry& entry, const std::string& key_name) {
  if (!entry.doc_store.Contains(key_name)) return;
  RemoveDocFromIndices(entry, key_name);
  entry.doc_store.Remove(key_name);
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
  size_t len = 0;
  const char* data = RedisModule_StringPtrLen(key, &len);
  std::string key_str(data, len);

  bool is_del = (strcmp(event, "del") == 0 || strcmp(event, "expired") == 0 ||
                 strcmp(event, "evicted") == 0);
  bool is_hash_write = (strcmp(event, "hset") == 0 || strcmp(event, "hdel") == 0 ||
                        strcmp(event, "hincrby") == 0 || strcmp(event, "hincrbyfloat") == 0 ||
                        strcmp(event, "hmset") == 0 || strcmp(event, "hsetnx") == 0);
  bool is_json_write = (strcmp(event, "json.set") == 0 || strcmp(event, "json.del") == 0 ||
                        strcmp(event, "json.arrappend") == 0 || strcmp(event, "json.arrinsert") == 0 ||
                        strcmp(event, "json.numincrby") == 0 || strcmp(event, "json.strappend") == 0);
  bool is_json_del = (strcmp(event, "json.del") == 0);

  for (auto& [idx_name, entry] : GetIndices()) {
    if (!entry.spec.HasPrefixes()) continue;
    if (!entry.spec.MatchesPrefix(key_str)) continue;

    if (is_del) {
      RemoveHashKey(entry, key_str);
    } else if (entry.spec.source_type == IndexSourceType::kJson) {
      if (is_json_del) {
        RemoveHashKey(entry, key_str);
      } else if (is_json_write) {
        IndexJsonKey(ctx, entry, key_str);
      }
    } else if (is_hash_write) {
      IndexHashKey(ctx, entry, key_str);
    }
  }

  return REDISMODULE_OK;
}

