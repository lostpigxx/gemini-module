#include "search_internal.h"
#include "expr_engine.h"
#include "hnsw_index.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

static std::unordered_map<std::string, std::shared_ptr<IndexEntry>> g_indices;
static std::shared_mutex g_indices_mutex;
static std::shared_mutex g_term_dicts_mutex;
static std::shared_mutex g_suggest_dicts_mutex;
static SearchExecutor g_executor(2);
static std::unordered_map<std::string, std::string> g_aliases;
static std::unordered_map<uint64_t, AggCursor> g_cursors;
static uint64_t g_next_cursor_id = 1;
static uint64_t g_cursors_created = 0;
static uint64_t g_cursors_expired = 0;
static std::unordered_map<std::string, SuggestDict> g_suggest_dicts;
static std::unordered_map<std::string, TermDict> g_term_dicts;
static uint64_t g_next_index_generation = 1;
static bool g_cluster_rdb_mode = false;
static uint64_t g_persisted_schema_epoch = 0;
static std::vector<std::string> g_pending_cluster_recoveries;
static std::unordered_map<std::string, std::string> g_config = {
    {"DEFAULT_DIALECT", "2"},
    {"TIMEOUT", "500"},
    {"MAXEXPANSIONS", "200"},
    {"MINSTEMLEN", "3"},
    {"ON_TIMEOUT", "FAIL"},
    {"MAXSEARCHRESULTS", "1000000"},
    {"ASYNC_MIN_DOCS", "0"},
    {"BG_INDEX_MIN_KEYS", "1000"},
    {"BG_INDEX_BATCH_KEYS", "128"},
    {"BG_INDEX_TICK_MS", "1"},
    {"CLUSTER_ENABLED", "1"},
    {"CLUSTER_RPC_PORT_OFFSET", "20000"},
    {"CLUSTER_QUERY_TIMEOUT_MS", "5000"},
    {"CLUSTER_MAX_SCATTER_LIMIT", "10000"},
    {"CLUSTER_BM25_DFS", "0"},
    {"CLUSTER_AGG_MAX_GROUPS", "100000"},
};

std::unordered_map<std::string, std::shared_ptr<IndexEntry>>& GetIndices() { return g_indices; }
std::shared_mutex& GetIndicesMutex() { return g_indices_mutex; }
std::shared_mutex& GetTermDictsMutex() { return g_term_dicts_mutex; }
std::shared_mutex& GetSuggestDictsMutex() { return g_suggest_dicts_mutex; }
SearchExecutor& GetSearchExecutor() { return g_executor; }
std::unordered_map<uint64_t, AggCursor>& GetCursors() { return g_cursors; }
uint64_t NextCursorId() {
  g_cursors_created++;
  return g_next_cursor_id++;
}
uint64_t GetTotalCursorsCreated() { return g_cursors_created; }
uint64_t GetTotalCursorsExpired() { return g_cursors_expired; }

int64_t NowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000LL +
         static_cast<int64_t>(ts.tv_nsec) / 1000000LL;
}

void TouchIndex(IndexEntry& entry) { entry.last_access_ms = NowMs(); }

int GetDefaultDialect() {
  auto it = g_config.find("DEFAULT_DIALECT");
  if (it != g_config.end()) {
    char* ep = nullptr;
    long v = std::strtol(it->second.c_str(), &ep, 10);
    if (*ep == '\0' && v >= 1) return static_cast<int>(v);
  }
  return 2;
}

size_t GetMaxExpansions() {
  auto it = g_config.find("MAXEXPANSIONS");
  if (it != g_config.end()) {
    char* ep = nullptr;
    long v = std::strtol(it->second.c_str(), &ep, 10);
    if (*ep == '\0' && v > 0) return static_cast<size_t>(v);
  }
  return 200;
}

size_t GetMinStemLen() {
  auto it = g_config.find("MINSTEMLEN");
  if (it != g_config.end()) {
    char* ep = nullptr;
    long v = std::strtol(it->second.c_str(), &ep, 10);
    if (*ep == '\0' && v >= 1) return static_cast<size_t>(v);
  }
  return 3;
}

size_t GetMaxSearchResults() {
  auto it = g_config.find("MAXSEARCHRESULTS");
  if (it != g_config.end()) {
    char* ep = nullptr;
    long long v = std::strtoll(it->second.c_str(), &ep, 10);
    if (*ep == '\0' && v >= 0) return static_cast<size_t>(v);
  }
  return 1000000;
}

size_t GetAsyncMinDocs() {
  auto it = g_config.find("ASYNC_MIN_DOCS");
  if (it != g_config.end()) {
    char* ep = nullptr;
    long long v = std::strtoll(it->second.c_str(), &ep, 10);
    if (*ep == '\0' && v >= 0) return static_cast<size_t>(v);
  }
  return 0;
}

size_t GetBgIndexMinKeys() {
  auto it = g_config.find("BG_INDEX_MIN_KEYS");
  if (it != g_config.end()) {
    char* ep = nullptr;
    long long v = std::strtoll(it->second.c_str(), &ep, 10);
    if (*ep == '\0' && v >= 0) return static_cast<size_t>(v);
  }
  return 1000;
}

size_t GetBgIndexBatchKeys() {
  auto it = g_config.find("BG_INDEX_BATCH_KEYS");
  if (it != g_config.end()) {
    char* ep = nullptr;
    long long v = std::strtoll(it->second.c_str(), &ep, 10);
    if (*ep == '\0' && v >= 1) return static_cast<size_t>(v);
  }
  return 128;
}

uint64_t GetBgIndexTickMs() {
  auto it = g_config.find("BG_INDEX_TICK_MS");
  if (it != g_config.end()) {
    char* ep = nullptr;
    long long v = std::strtoll(it->second.c_str(), &ep, 10);
    if (*ep == '\0' && v >= 0 && v <= 10000) return static_cast<uint64_t>(v);
  }
  return 1;
}

bool GetClusterEnabledConfig() {
  auto it = g_config.find("CLUSTER_ENABLED");
  if (it != g_config.end()) {
    return it->second != "0" && !MatchArg(it->second, "FALSE") && !MatchArg(it->second, "NO");
  }
  return true;
}

int GetClusterRpcPortOffset() {
  auto it = g_config.find("CLUSTER_RPC_PORT_OFFSET");
  if (it != g_config.end()) {
    char* ep = nullptr;
    long v = std::strtol(it->second.c_str(), &ep, 10);
    if (*ep == '\0' && v >= 0 && v <= 50000) return static_cast<int>(v);
  }
  return 20000;
}

uint64_t GetClusterQueryTimeoutMs() {
  auto it = g_config.find("CLUSTER_QUERY_TIMEOUT_MS");
  if (it != g_config.end()) {
    char* ep = nullptr;
    long long v = std::strtoll(it->second.c_str(), &ep, 10);
    if (*ep == '\0' && v > 0) return static_cast<uint64_t>(v);
  }
  return 5000;
}

size_t GetClusterMaxScatterLimit() {
  auto it = g_config.find("CLUSTER_MAX_SCATTER_LIMIT");
  if (it != g_config.end()) {
    char* ep = nullptr;
    long long v = std::strtoll(it->second.c_str(), &ep, 10);
    if (*ep == '\0' && v > 0) return static_cast<size_t>(v);
  }
  return 10000;
}

bool GetClusterBm25Dfs() {
  auto it = g_config.find("CLUSTER_BM25_DFS");
  if (it != g_config.end()) {
    return it->second == "1" || MatchArg(it->second, "TRUE") || MatchArg(it->second, "YES");
  }
  return false;
}

size_t GetClusterAggMaxGroups() {
  auto it = g_config.find("CLUSTER_AGG_MAX_GROUPS");
  if (it != g_config.end()) {
    char* ep = nullptr;
    long long v = std::strtoll(it->second.c_str(), &ep, 10);
    if (*ep == '\0' && v > 0) return static_cast<size_t>(v);
  }
  return 100000;
}

uint64_t NextIndexGeneration() { return g_next_index_generation++; }

bool IsOnTimeoutReturn() {
  auto it = g_config.find("ON_TIMEOUT");
  if (it != g_config.end()) {
    return MatchArg(it->second, "RETURN");
  }
  return false;
}

void PurgeExpiredCursors(int64_t now_ms) {
  for (auto it = g_cursors.begin(); it != g_cursors.end(); ) {
    if (it->second.max_idle_ms > 0 && it->second.last_access_ms > 0 &&
        now_ms - it->second.last_access_ms >= it->second.max_idle_ms) {
      it = g_cursors.erase(it);
      g_cursors_expired++;
    } else {
      ++it;
    }
  }
}

std::string ResolveAlias(const std::string& name) {
  auto it = g_aliases.find(name);
  if (it != g_aliases.end()) return it->second;
  return name;
}

IndexEntry* GetIndexEntry(const std::string& name) {
  auto resolved = ResolveAlias(name);
  auto it = g_indices.find(resolved);
  if (it == g_indices.end() || !it->second) return nullptr;
  return it->second.get();
}

std::shared_ptr<IndexEntry> GetIndexEntryShared(const std::string& name) {
  std::shared_lock<std::shared_mutex> lk(g_indices_mutex);
  auto resolved = ResolveAlias(name);
  auto it = g_indices.find(resolved);
  if (it == g_indices.end()) return nullptr;
  return it->second;
}

bool CreateIndexEntry(const std::string& name, IndexEntry entry) {
  auto ptr = std::make_shared<IndexEntry>(std::move(entry));
  auto [it, inserted] = g_indices.try_emplace(name, std::move(ptr));
  return inserted;
}

void EraseIndexEntry(const std::string& name) { g_indices.erase(name); }

std::unordered_map<std::string, std::string>& GetAliases() { return g_aliases; }
std::unordered_map<std::string, SuggestDict>& GetSuggestDicts() { return g_suggest_dicts; }
std::unordered_map<std::string, TermDict>& GetTermDicts() { return g_term_dicts; }
std::unordered_map<std::string, std::string>& GetConfig() { return g_config; }
bool IsClusterRdbMode() { return g_cluster_rdb_mode; }
void SetClusterRdbMode(bool enabled) { g_cluster_rdb_mode = enabled; }
uint64_t GetPersistedSchemaEpoch() { return g_persisted_schema_epoch; }
void SetPersistedSchemaEpoch(uint64_t epoch) { g_persisted_schema_epoch = epoch; }
std::vector<std::string>& GetPendingClusterRecoveries() { return g_pending_cluster_recoveries; }

void ClearAllGlobalState() {
  g_indices.clear();
  g_aliases.clear();
  g_cursors.clear();
  g_next_cursor_id = 1;
  g_cursors_created = 0;
  g_cursors_expired = 0;
  g_suggest_dicts.clear();
  g_term_dicts.clear();
  g_cluster_rdb_mode = false;
  g_persisted_schema_epoch = 0;
  g_pending_cluster_recoveries.clear();
  g_config = {
      {"DEFAULT_DIALECT", "2"},
      {"TIMEOUT", "500"},
      {"MAXEXPANSIONS", "200"},
      {"MINSTEMLEN", "3"},
      {"ON_TIMEOUT", "FAIL"},
      {"MAXSEARCHRESULTS", "1000000"},
      {"ASYNC_MIN_DOCS", "0"},
      {"BG_INDEX_MIN_KEYS", "1000"},
      {"BG_INDEX_BATCH_KEYS", "128"},
      {"BG_INDEX_TICK_MS", "1"},
      {"CLUSTER_ENABLED", "1"},
      {"CLUSTER_RPC_PORT_OFFSET", "20000"},
      {"CLUSTER_QUERY_TIMEOUT_MS", "5000"},
      {"CLUSTER_MAX_SCATTER_LIMIT", "10000"},
      {"CLUSTER_BM25_DFS", "0"},
      {"CLUSTER_AGG_MAX_GROUPS", "100000"},
  };
}

// Forward declaration of internal helper defined below
static void AddDocToIndicesInternal(
    IndexEntry& entry, const std::string& doc_id,
    const std::unordered_map<std::string, std::string>& fields,
    const std::unordered_set<std::string>* skip_vector_fields);

bool CreateIndexFromRdb(
    const std::string& name, IndexSpec spec,
    const std::vector<
        std::pair<std::string, std::unordered_map<std::string, std::string>>>&
        docs,
    const std::unordered_map<std::string, std::string>* prebuilt_hnsw) {
  IndexEntry entry;
  entry.spec = std::move(spec);
  entry.last_access_ms = NowMs();

  std::unordered_set<std::string> skip_vector_fields;
  if (prebuilt_hnsw && !prebuilt_hnsw->empty()) {
    for (const auto& [fname, blob] : *prebuilt_hnsw) {
      const auto* fspec = entry.spec.FindField(fname);
      if (!fspec || fspec->type != FieldType::kVector ||
          fspec->vector_params.algorithm != VectorAlgorithm::kHnsw) {
        return false;
      }
      auto& vidx = entry.vector_indices.GetOrCreate(fname, fspec->vector_params);
      auto* hnsw = dynamic_cast<HnswIndex*>(&vidx);
      if (!hnsw || !hnsw->DeserializeGraph(blob.data(), blob.size())) {
        return false;
      }
      skip_vector_fields.insert(fname);
    }
  }

  const auto* skip_ptr = skip_vector_fields.empty() ? nullptr : &skip_vector_fields;
  for (auto& [doc_id, fields] : docs) {
    entry.doc_store.Add(doc_id, fields);
    AddDocToIndicesInternal(entry, doc_id, fields, skip_ptr);
  }
  g_indices[name] = std::make_shared<IndexEntry>(std::move(entry));
  return true;
}

bool TryParseDouble(const std::string& s, double& out) {
  char* endptr = nullptr;
  out = std::strtod(s.c_str(), &endptr);
  return endptr != s.c_str() && *endptr == '\0' && !std::isnan(out) &&
         !std::isinf(out);
}

std::string_view ArgView(RedisModuleString* s) {
  size_t len;
  const char* data = RedisModule_StringPtrLen(s, &len);
  return {data, len};
}

std::string ArgStr(RedisModuleString* s) {
  size_t len;
  const char* data = RedisModule_StringPtrLen(s, &len);
  return {data, len};
}

bool MatchArg(std::string_view arg, const char* target) {
  if (arg.size() != strlen(target)) return false;
  return strncasecmp(arg.data(), target, arg.size()) == 0;
}

template <typename Fn>
static void ForEachNumericValue(const std::string& fval, Fn&& fn) {
  double num = 0;
  if (TryParseDouble(fval, num)) {
    fn(num);
    return;
  }
  if (fval.find(',') == std::string::npos) return;
  size_t pos = 0;
  while (pos < fval.size()) {
    size_t comma = fval.find(',', pos);
    if (comma == std::string::npos) comma = fval.size();
    std::string tok = fval.substr(pos, comma - pos);
    if (TryParseDouble(tok, num)) fn(num);
    pos = comma + 1;
  }
}

void RemoveDocFromIndices(IndexEntry& entry,
                                 const std::string& doc_id) {
  const auto* doc = entry.doc_store.Get(doc_id);
  if (!doc) return;
  for (auto& [fname, fval] : doc->fields) {
    const auto* fspec = entry.spec.FindField(fname);
    if (!fspec || fspec->noindex) continue;
    if (fspec->sortable) {
      entry.sort_indices.GetOrCreate(fname).Remove(doc_id);
    }
    if (fspec->type == FieldType::kTag) {
      entry.tag_indices.GetOrCreate(fname).RemoveMulti(
          fval, doc_id, fspec->tag_separator, fspec->case_sensitive);
    } else if (fspec->type == FieldType::kNumeric) {
      auto& nidx = entry.numeric_indices.GetOrCreate(fname);
      ForEachNumericValue(fval, [&](double num) { nidx.Remove(num, doc_id); });
    } else if (fspec->type == FieldType::kVector) {
      entry.vector_indices.GetOrCreate(fname, fspec->vector_params).Remove(doc_id);
    } else if (fspec->type == FieldType::kText) {
      entry.text_indices.GetOrCreate(fname).Remove(doc_id);
    } else if (fspec->type == FieldType::kGeo) {
      entry.geo_indices.GetOrCreate(fname).Remove(doc_id);
    } else if (fspec->type == FieldType::kGeoShape) {
      entry.geoshape_indices.GetOrCreate(fname).Remove(doc_id);
    }
  }
}

static void AddDocToIndicesInternal(
    IndexEntry& entry, const std::string& doc_id,
    const std::unordered_map<std::string, std::string>& fields,
    const std::unordered_set<std::string>* skip_vector_fields) {
  for (auto& [fname, fval] : fields) {
    const auto* fspec = entry.spec.FindField(fname);
    if (!fspec || fspec->noindex) continue;
    if (fspec->type == FieldType::kNumeric) {
      auto& nidx = entry.numeric_indices.GetOrCreate(fname);
      bool sort_set = false;
      ForEachNumericValue(fval, [&](double num) {
        nidx.Add(num, doc_id);
        if (fspec->sortable && !sort_set) {
          entry.sort_indices.GetOrCreate(fname).SetNumeric(doc_id, num);
          sort_set = true;
        }
      });
    } else if (fspec->sortable && fspec->type != FieldType::kVector) {
      entry.sort_indices.GetOrCreate(fname).SetString(doc_id, fval);
    }
    if (fspec->type == FieldType::kTag) {
      entry.tag_indices.GetOrCreate(fname).AddMulti(
          fval, doc_id, fspec->tag_separator, fspec->case_sensitive);
    } else if (fspec->type == FieldType::kNumeric) {
      // already indexed above
    } else if (fspec->type == FieldType::kVector) {
      if (skip_vector_fields && skip_vector_fields->count(fname) > 0) {
        continue;
      }
      std::vector<float> vec;
      if (DecodeVectorBlob(fval, fspec->vector_params.dim,
                           fspec->vector_params.data_type, vec)) {
        auto& vidx = entry.vector_indices.GetOrCreate(fname, fspec->vector_params);
        vidx.Add(doc_id, vec.data());
      }
    } else if (fspec->type == FieldType::kText) {
      std::string doc_lang = entry.spec.language;
      if (!entry.spec.language_field.empty()) {
        auto lit = fields.find(entry.spec.language_field);
        if (lit != fields.end() && !lit->second.empty()) {
          doc_lang = lit->second;
        }
      }
      const std::unordered_set<std::string>* cdict = nullptr;
      if (!entry.spec.custom_dict.empty()) {
        auto dit = g_term_dicts.find(entry.spec.custom_dict);
        if (dit != g_term_dicts.end()) cdict = &dit->second.Terms();
      }
      entry.text_indices.GetOrCreate(fname).Add(
          doc_id, fval, !fspec->phonetic.empty(), doc_lang,
          entry.spec.nofreqs, entry.spec.nooffsets, cdict);
    } else if (fspec->type == FieldType::kGeo) {
      GeoCoord coord;
      if (ParseGeoCoord(fval, coord)) {
        entry.geo_indices.GetOrCreate(fname).Add(doc_id, coord.lon, coord.lat);
      }
    } else if (fspec->type == FieldType::kGeoShape) {
      GeoShape shape;
      if (ParseWkt(fval, shape)) {
        entry.geoshape_indices.GetOrCreate(fname).Add(doc_id, shape);
      }
    }
  }
}

void AddDocToIndices(
    IndexEntry& entry, const std::string& doc_id,
    const std::unordered_map<std::string, std::string>& fields) {
  AddDocToIndicesInternal(entry, doc_id, fields, nullptr);
}

void AddDocToIndicesSkippingExistingHnsw(
    IndexEntry& entry, const std::string& doc_id,
    const std::unordered_map<std::string, std::string>& fields) {
  std::unordered_set<std::string> skip_fields;
  for (const auto& f : entry.spec.fields) {
    if (f.type == FieldType::kVector &&
        f.vector_params.algorithm == VectorAlgorithm::kHnsw) {
      if (const auto* vidx = entry.vector_indices.Get(f.name)) {
        if (const auto* hnsw = dynamic_cast<const HnswIndex*>(vidx)) {
          if (hnsw->Contains(doc_id)) {
            skip_fields.insert(f.name);
          }
        }
      }
    }
  }
  AddDocToIndicesInternal(entry, doc_id, fields,
                          skip_fields.empty() ? nullptr : &skip_fields);
}

bool PassesIndexFilter(const IndexSpec& spec,
                       const std::unordered_map<std::string, std::string>& fields) {
  if (spec.filter_expr.empty()) return true;
  ExprNode expr;
  std::string err;
  if (!ParseExpr(spec.filter_expr, expr, err)) return false;
  return EvalExpr(expr, fields).Truthy();
}

