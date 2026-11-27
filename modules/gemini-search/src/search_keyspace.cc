#include "cluster_topology.h"
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

static void IndexHashKeyInternal(RedisModuleCtx* ctx, IndexEntry& entry,
                                 const std::string& key_name, bool preserve_existing_hnsw) {
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
    if (preserve_existing_hnsw) {
      AddDocToIndicesSkippingExistingHnsw(entry, key_name, doc_fields);
    } else {
      AddDocToIndices(entry, key_name, doc_fields);
    }
  }
}

void IndexHashKey(RedisModuleCtx* ctx, IndexEntry& entry,
                  const std::string& key_name) {
  IndexHashKeyInternal(ctx, entry, key_name, /*preserve_existing_hnsw=*/false);
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

static void IndexJsonKeyInternal(RedisModuleCtx* ctx, IndexEntry& entry,
                                 const std::string& key_name, bool preserve_existing_hnsw) {
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
    if (preserve_existing_hnsw) {
      AddDocToIndicesSkippingExistingHnsw(entry, key_name, doc_fields);
    } else {
      AddDocToIndices(entry, key_name, doc_fields);
    }
  }
}

void IndexJsonKey(RedisModuleCtx* ctx, IndexEntry& entry,
                  const std::string& key_name) {
  IndexJsonKeyInternal(ctx, entry, key_name, /*preserve_existing_hnsw=*/false);
}

struct ScanPrivdata {
  RedisModuleCtx* ctx;
  IndexEntry* entry;
  size_t keys_seen = 0;
  bool preserve_existing_hnsw = false;
};

static void ScanCallback(RedisModuleCtx* /*ctx*/, RedisModuleString* keyname,
                         RedisModuleKey* /*key*/, void* privdata) {
  auto* pd = static_cast<ScanPrivdata*>(privdata);
  pd->keys_seen++;
  size_t len = 0;
  const char* data = RedisModule_StringPtrLen(keyname, &len);
  std::string key_str(data, len);
  if (pd->entry->spec.MatchesPrefix(key_str)) {
    if (pd->entry->spec.source_type == IndexSourceType::kJson)
      IndexJsonKeyInternal(pd->ctx, *pd->entry, key_str, pd->preserve_existing_hnsw);
    else
      IndexHashKeyInternal(pd->ctx, *pd->entry, key_str, pd->preserve_existing_hnsw);
  }
}

void ScanExistingKeys(RedisModuleCtx* ctx, IndexEntry& entry) {
  auto* cursor = RedisModule_ScanCursorCreate();
  ScanPrivdata pd{ctx, &entry, 0, false};
  while (RedisModule_Scan(ctx, cursor, ScanCallback, &pd)) {}
  RedisModule_ScanCursorDestroy(cursor);
  entry.bg_indexing = false;
  entry.percent_indexed = 1.0;
}

void ScanExistingKeysPreservingHnsw(RedisModuleCtx* ctx, IndexEntry& entry) {
  auto* cursor = RedisModule_ScanCursorCreate();
  ScanPrivdata pd{ctx, &entry, 0, true};
  while (RedisModule_Scan(ctx, cursor, ScanCallback, &pd)) {}
  RedisModule_ScanCursorDestroy(cursor);
  entry.bg_indexing = false;
  entry.percent_indexed = 1.0;
}

namespace {

struct BgScanState {
  uint64_t generation = 0;
  int db_id = 0;
  RedisModuleScanCursor* cursor = nullptr;
  RedisModuleTimerID timer_id = 0;
};

struct BgTimerToken {
  std::string index_name;
  uint64_t generation = 0;
};

static std::unordered_map<std::string, BgScanState> g_bg_scans;

void BgScanTimerCallback(RedisModuleCtx* ctx, void* data) {
  std::unique_ptr<BgTimerToken> tok(static_cast<BgTimerToken*>(data));
  auto sit = g_bg_scans.find(tok->index_name);
  if (sit == g_bg_scans.end() || sit->second.generation != tok->generation) {
    return;
  }

  auto iit = GetIndices().find(tok->index_name);
  if (iit == GetIndices().end() || !iit->second || iit->second->generation != tok->generation) {
    if (sit->second.cursor) RedisModule_ScanCursorDestroy(sit->second.cursor);
    g_bg_scans.erase(sit);
    return;
  }

  auto& state = sit->second;
  auto& entry = *iit->second;
  RedisModule_SelectDb(ctx, state.db_id);

  const size_t batch_limit = GetBgIndexBatchKeys();
  const int64_t start_ms = NowMs();
  ScanPrivdata pd{ctx, &entry, 0};
  bool more = true;
  while (pd.keys_seen < batch_limit) {
    more = (RedisModule_Scan(ctx, state.cursor, ScanCallback, &pd) != 0);
    if (!more) break;
    if (NowMs() - start_ms >= 2) break;
  }

  entry.bg_scanned_keys += pd.keys_seen;
  if (!more) {
    entry.bg_indexing = false;
    entry.percent_indexed = 1.0;
    if (state.cursor) RedisModule_ScanCursorDestroy(state.cursor);
    g_bg_scans.erase(sit);
    return;
  }

  double denom = static_cast<double>(
      entry.bg_total_keys > 0 ? entry.bg_total_keys : 1);
  double pct = static_cast<double>(entry.bg_scanned_keys) / denom;
  if (pct > 0.99) pct = 0.99;
  entry.percent_indexed = pct;

  auto* next_tok = new BgTimerToken{tok->index_name, tok->generation};
  state.timer_id = RedisModule_CreateTimer(
      ctx, static_cast<mstime_t>(GetBgIndexTickMs()), BgScanTimerCallback, next_tok);
}

}  // namespace

void StopBackgroundIndexing(RedisModuleCtx* ctx, const std::string& index_name) {
  auto sit = g_bg_scans.find(index_name);
  if (sit == g_bg_scans.end()) return;
  if (ctx && sit->second.timer_id != 0) {
    void* timer_data = nullptr;
    if (RedisModule_StopTimer(ctx, sit->second.timer_id, &timer_data) == REDISMODULE_OK &&
        timer_data != nullptr) {
      delete static_cast<BgTimerToken*>(timer_data);
    }
  }
  if (sit->second.cursor) {
    RedisModule_ScanCursorDestroy(sit->second.cursor);
  }
  g_bg_scans.erase(sit);
}

void StopAllBackgroundIndexing(RedisModuleCtx* ctx) {
  for (auto& [name, state] : g_bg_scans) {
    if (ctx && state.timer_id != 0) {
      void* timer_data = nullptr;
      if (RedisModule_StopTimer(ctx, state.timer_id, &timer_data) == REDISMODULE_OK &&
          timer_data != nullptr) {
        delete static_cast<BgTimerToken*>(timer_data);
      }
    }
    if (state.cursor) {
      RedisModule_ScanCursorDestroy(state.cursor);
    }
  }
  g_bg_scans.clear();
}

void StartOrRunInitialScan(RedisModuleCtx* ctx, IndexEntry& entry) {
  unsigned long long db_size = RedisModule_DbSize(ctx);
  size_t min_keys = GetBgIndexMinKeys();
  if (db_size == 0 || db_size < min_keys) {
    ScanExistingKeys(ctx, entry);
    return;
  }

  StopBackgroundIndexing(ctx, entry.spec.name);
  entry.generation = NextIndexGeneration();
  entry.bg_indexing = true;
  entry.bg_scanned_keys = 0;
  entry.bg_total_keys = static_cast<uint64_t>(db_size);
  entry.percent_indexed = 0.0;

  BgScanState state;
  state.generation = entry.generation;
  state.db_id = RedisModule_GetSelectedDb(ctx);
  state.cursor = RedisModule_ScanCursorCreate();
  auto* tok = new BgTimerToken{entry.spec.name, entry.generation};
  state.timer_id = RedisModule_CreateTimer(
      ctx, static_cast<mstime_t>(GetBgIndexTickMs()), BgScanTimerCallback, tok);
  g_bg_scans[entry.spec.name] = state;
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

  for (auto& [idx_name, entry_ptr] : GetIndices()) {
    if (!entry_ptr) continue;
    auto& entry = *entry_ptr;
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

void OnFlushDbEvent(RedisModuleCtx* ctx, RedisModuleEvent /*eid*/,
                    uint64_t subevent, void* /*data*/) {
  if (subevent == REDISMODULE_SUBEVENT_FLUSHDB_START) {
    StopAllBackgroundIndexing(ctx);
  } else if (subevent == REDISMODULE_SUBEVENT_FLUSHDB_END) {
    StopAllBackgroundIndexing(ctx);
    GetCursors().clear();
    GetAliases().clear();
    GetSuggestDicts().clear();
    GetTermDicts().clear();
  }
}

static std::vector<std::string> g_pending_json_cluster_recoveries;

static bool IsClusterStateOk(RedisModuleCtx* ctx) {
  if (!ctx || !RedisModule_Call) return true;
  RedisModuleCallReply* rep = RedisModule_Call(ctx, "CLUSTER", "c", "INFO");
  if (!rep) return false;
  bool ok = false;
  if (RedisModule_CallReplyType(rep) == REDISMODULE_REPLY_STRING) {
    size_t len = 0;
    const char* data = RedisModule_CallReplyStringPtr(rep, &len);
    if (data && std::string_view(data, len).find("cluster_state:ok") != std::string_view::npos) {
      ok = true;
    }
  }
  RedisModule_FreeCallReply(rep);
  return ok;
}

static void ClusterJsonRecoveryTimerCallback(RedisModuleCtx* ctx, void* /*data*/) {
  if (g_pending_json_cluster_recoveries.empty()) return;
  if (!IsClusterStateOk(ctx)) {
    if (RedisModule_CreateTimer) {
      RedisModule_CreateTimer(ctx, 50, ClusterJsonRecoveryTimerCallback, nullptr);
    }
    return;
  }
  std::vector<std::string> pending;
  pending.swap(g_pending_json_cluster_recoveries);
  RedisModule_SelectDb(ctx, 0);
  for (const auto& idx_name : pending) {
    auto entry_ptr = GetIndexEntryShared(idx_name);
    if (entry_ptr && entry_ptr->spec.HasPrefixes() && !entry_ptr->spec.skip_initial_scan) {
      ScanExistingKeysPreservingHnsw(ctx, *entry_ptr);
    }
  }
}

void OnLoadingEvent(RedisModuleCtx* ctx, RedisModuleEvent /*eid*/,
                    uint64_t subevent, void* /*data*/) {
  if (subevent == REDISMODULE_SUBEVENT_LOADING_RDB_START ||
      subevent == REDISMODULE_SUBEVENT_LOADING_AOF_START ||
      subevent == REDISMODULE_SUBEVENT_LOADING_REPL_START) {
    g_is_loading = true;
  } else if (subevent == REDISMODULE_SUBEVENT_LOADING_ENDED) {
    g_is_loading = false;
    auto& mgr = gemini_search::ClusterManager::Instance();
    if (mgr.IsClusterMode()) {
      uint64_t persisted_epoch = GetPersistedSchemaEpoch();
      if (persisted_epoch > mgr.GetSchemaEpoch()) {
        mgr.SetSchemaEpoch(persisted_epoch);
      }
      std::vector<std::string> pending;
      pending.swap(GetPendingClusterRecoveries());
      if (!pending.empty() && ctx) {
        RedisModule_SelectDb(ctx, 0);
        bool cluster_ok = IsClusterStateOk(ctx);
        for (const auto& idx_name : pending) {
          auto entry_ptr = GetIndexEntryShared(idx_name);
          if (!entry_ptr || !entry_ptr->spec.HasPrefixes() || entry_ptr->spec.skip_initial_scan) {
            continue;
          }
          // ON JSON uses RM_Call("JSON.GET"), which Redis Cluster rejects with CLUSTERDOWN
          // until clusterUpdateState() marks cluster_state:ok after startup.
          if (entry_ptr->spec.source_type == IndexSourceType::kJson && !cluster_ok) {
            g_pending_json_cluster_recoveries.push_back(idx_name);
          } else {
            ScanExistingKeysPreservingHnsw(ctx, *entry_ptr);
          }
        }
        if (!g_pending_json_cluster_recoveries.empty() && RedisModule_CreateTimer) {
          RedisModule_CreateTimer(ctx, 50, ClusterJsonRecoveryTimerCallback, nullptr);
        }
      }
      if (ctx) {
        mgr.BroadcastHello(ctx, /*request_reply=*/false);
      }
    }
  } else if (subevent == REDISMODULE_SUBEVENT_LOADING_FAILED) {
    g_is_loading = false;
    GetPendingClusterRecoveries().clear();
    g_pending_json_cluster_recoveries.clear();
  }
}
