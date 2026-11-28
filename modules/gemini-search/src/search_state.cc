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
static std::shared_mutex g_aliases_mutex;
static std::shared_mutex g_config_mutex;
static std::shared_mutex g_term_dicts_mutex;
static std::shared_mutex g_suggest_dicts_mutex;
static std::mutex g_cursors_mutex;
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
static bool g_in_forked_child = false;
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
std::shared_mutex& GetAliasesMutex() { return g_aliases_mutex; }
std::shared_mutex& GetConfigMutex() { return g_config_mutex; }
std::shared_mutex& GetTermDictsMutex() { return g_term_dicts_mutex; }
std::shared_mutex& GetSuggestDictsMutex() { return g_suggest_dicts_mutex; }
std::mutex& GetCursorsMutex() { return g_cursors_mutex; }
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

void TouchIndex(const IndexEntry& entry) { entry.SetLastAccessMs(NowMs()); }

static bool ReadConfigValue(const char* key, std::string& out) {
  std::shared_lock<std::shared_mutex> lk(g_config_mutex);
  auto it = g_config.find(key);
  if (it == g_config.end()) return false;
  out = it->second;
  return true;
}

int GetDefaultDialect() {
  std::string val;
  if (ReadConfigValue("DEFAULT_DIALECT", val)) {
    char* ep = nullptr;
    long v = std::strtol(val.c_str(), &ep, 10);
    if (*ep == '\0' && v >= 1) return static_cast<int>(v);
  }
  return 2;
}

size_t GetMaxExpansions() {
  std::string val;
  if (ReadConfigValue("MAXEXPANSIONS", val)) {
    char* ep = nullptr;
    long v = std::strtol(val.c_str(), &ep, 10);
    if (*ep == '\0' && v > 0) return static_cast<size_t>(v);
  }
  return 200;
}

size_t GetMinStemLen() {
  std::string val;
  if (ReadConfigValue("MINSTEMLEN", val)) {
    char* ep = nullptr;
    long v = std::strtol(val.c_str(), &ep, 10);
    if (*ep == '\0' && v >= 1) return static_cast<size_t>(v);
  }
  return 3;
}

size_t GetMaxSearchResults() {
  std::string val;
  if (ReadConfigValue("MAXSEARCHRESULTS", val)) {
    char* ep = nullptr;
    long long v = std::strtoll(val.c_str(), &ep, 10);
    if (*ep == '\0' && v >= 0) return static_cast<size_t>(v);
  }
  return 1000000;
}

size_t GetAsyncMinDocs() {
  std::string val;
  if (ReadConfigValue("ASYNC_MIN_DOCS", val)) {
    char* ep = nullptr;
    long long v = std::strtoll(val.c_str(), &ep, 10);
    if (*ep == '\0' && v >= 0) return static_cast<size_t>(v);
  }
  return 0;
}

size_t GetBgIndexMinKeys() {
  std::string val;
  if (ReadConfigValue("BG_INDEX_MIN_KEYS", val)) {
    char* ep = nullptr;
    long long v = std::strtoll(val.c_str(), &ep, 10);
    if (*ep == '\0' && v >= 0) return static_cast<size_t>(v);
  }
  return 1000;
}

size_t GetBgIndexBatchKeys() {
  std::string val;
  if (ReadConfigValue("BG_INDEX_BATCH_KEYS", val)) {
    char* ep = nullptr;
    long long v = std::strtoll(val.c_str(), &ep, 10);
    if (*ep == '\0' && v >= 1) return static_cast<size_t>(v);
  }
  return 128;
}

uint64_t GetBgIndexTickMs() {
  std::string val;
  if (ReadConfigValue("BG_INDEX_TICK_MS", val)) {
    char* ep = nullptr;
    long long v = std::strtoll(val.c_str(), &ep, 10);
    if (*ep == '\0' && v >= 0 && v <= 10000) return static_cast<uint64_t>(v);
  }
  return 1;
}

bool GetClusterEnabledConfig() {
  std::string val;
  if (ReadConfigValue("CLUSTER_ENABLED", val)) {
    return val != "0" && !MatchArg(val, "FALSE") && !MatchArg(val, "NO");
  }
  return true;
}

int GetClusterRpcPortOffset() {
  std::string val;
  if (ReadConfigValue("CLUSTER_RPC_PORT_OFFSET", val)) {
    char* ep = nullptr;
    long v = std::strtol(val.c_str(), &ep, 10);
    if (*ep == '\0' && v >= 0 && v <= 50000) return static_cast<int>(v);
  }
  return 20000;
}

uint64_t GetClusterQueryTimeoutMs() {
  std::string val;
  if (ReadConfigValue("CLUSTER_QUERY_TIMEOUT_MS", val)) {
    char* ep = nullptr;
    long long v = std::strtoll(val.c_str(), &ep, 10);
    if (*ep == '\0' && v > 0) return static_cast<uint64_t>(v);
  }
  return 5000;
}

size_t GetClusterMaxScatterLimit() {
  std::string val;
  if (ReadConfigValue("CLUSTER_MAX_SCATTER_LIMIT", val)) {
    char* ep = nullptr;
    long long v = std::strtoll(val.c_str(), &ep, 10);
    if (*ep == '\0' && v > 0) return static_cast<size_t>(v);
  }
  return 10000;
}

bool GetClusterBm25Dfs() {
  std::string val;
  if (ReadConfigValue("CLUSTER_BM25_DFS", val)) {
    return val == "1" || MatchArg(val, "TRUE") || MatchArg(val, "YES");
  }
  return false;
}

size_t GetClusterAggMaxGroups() {
  std::string val;
  if (ReadConfigValue("CLUSTER_AGG_MAX_GROUPS", val)) {
    char* ep = nullptr;
    long long v = std::strtoll(val.c_str(), &ep, 10);
    if (*ep == '\0' && v > 0) return static_cast<size_t>(v);
  }
  return 100000;
}

uint64_t NextIndexGeneration() { return g_next_index_generation++; }

bool IsOnTimeoutReturn() {
  std::string val;
  if (ReadConfigValue("ON_TIMEOUT", val)) {
    return MatchArg(val, "RETURN");
  }
  return false;
}

void PurgeExpiredCursorsLocked(int64_t now_ms) {
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

void PurgeExpiredCursors(int64_t now_ms) {
  std::lock_guard<std::mutex> lk(g_cursors_mutex);
  PurgeExpiredCursorsLocked(now_ms);
}

std::string ResolveAlias(const std::string& name) {
  std::shared_lock<std::shared_mutex> lk(g_aliases_mutex, std::defer_lock);
  if (!g_in_forked_child) lk.lock();
  auto it = g_aliases.find(name);
  if (it != g_aliases.end()) return it->second;
  return name;
}

IndexEntry* GetIndexEntry(const std::string& name) {
  auto resolved = ResolveAlias(name);
  std::shared_lock<std::shared_mutex> lk(g_indices_mutex, std::defer_lock);
  if (!g_in_forked_child) lk.lock();
  auto it = g_indices.find(resolved);
  if (it == g_indices.end() || !it->second) return nullptr;
  return it->second.get();
}

std::shared_ptr<IndexEntry> GetIndexEntryShared(const std::string& name) {
  auto resolved = ResolveAlias(name);
  std::shared_lock<std::shared_mutex> lk(g_indices_mutex, std::defer_lock);
  if (!g_in_forked_child) lk.lock();
  auto it = g_indices.find(resolved);
  if (it == g_indices.end()) return nullptr;
  return it->second;
}

bool CreateIndexEntry(const std::string& name, IndexEntry entry) {
  std::unique_lock<std::shared_mutex> lk(g_indices_mutex);
  auto ptr = std::make_shared<IndexEntry>(std::move(entry));
  auto [it, inserted] = g_indices.try_emplace(name, std::move(ptr));
  return inserted;
}

void EraseIndexEntry(const std::string& name) {
  std::unique_lock<std::shared_mutex> lk(g_indices_mutex);
  g_indices.erase(name);
}

std::unordered_map<std::string, std::string>& GetAliases() { return g_aliases; }
std::unordered_map<std::string, SuggestDict>& GetSuggestDicts() { return g_suggest_dicts; }
std::unordered_map<std::string, TermDict>& GetTermDicts() { return g_term_dicts; }
std::unordered_map<std::string, std::string>& GetConfig() { return g_config; }
bool IsClusterRdbMode() { return g_cluster_rdb_mode; }
void SetClusterRdbMode(bool enabled) { g_cluster_rdb_mode = enabled; }
bool IsInForkedChild() { return g_in_forked_child; }
void MarkForkedChild() { g_in_forked_child = true; }
uint64_t GetPersistedSchemaEpoch() { return g_persisted_schema_epoch; }
void SetPersistedSchemaEpoch(uint64_t epoch) { g_persisted_schema_epoch = epoch; }
std::vector<std::string>& GetPendingClusterRecoveries() { return g_pending_cluster_recoveries; }

void ClearAllGlobalState() {
  {
    std::unique_lock<std::shared_mutex> lk(g_indices_mutex);
    g_indices.clear();
  }
  {
    std::unique_lock<std::shared_mutex> lk(g_aliases_mutex);
    g_aliases.clear();
  }
  {
    std::lock_guard<std::mutex> lk(g_cursors_mutex);
    g_cursors.clear();
    g_next_cursor_id = 1;
    g_cursors_created = 0;
    g_cursors_expired = 0;
  }
  {
    std::unique_lock<std::shared_mutex> lk(g_suggest_dicts_mutex);
    g_suggest_dicts.clear();
  }
  {
    std::unique_lock<std::shared_mutex> lk(g_term_dicts_mutex);
    g_term_dicts.clear();
  }
  g_cluster_rdb_mode = false;
  g_persisted_schema_epoch = 0;
  g_pending_cluster_recoveries.clear();
  {
    std::unique_lock<std::shared_mutex> lk(g_config_mutex);
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
}

void AppendFieldSpecArgs(const FieldSpec& f, std::vector<std::string>& out) {
  if (!f.json_path.empty()) {
    out.push_back(f.json_path);
    out.push_back("AS");
  }
  out.push_back(f.name);
  out.push_back(FieldTypeName(f.type));
  if (f.type == FieldType::kVector) {
    out.push_back(VectorAlgorithmName(f.vector_params.algorithm));
    if (f.vector_params.data_type != VectorDataType::kFloat32) {
      out.push_back("TYPE");
      out.push_back(VectorDataTypeName(f.vector_params.data_type));
    }
    if (f.vector_params.quantization != VectorQuantization::kNone) {
      out.push_back("QUANTIZATION");
      out.push_back(VectorQuantizationName(f.vector_params.quantization));
    }
    if (f.vector_params.rerank != VectorRerankType::kNone) {
      out.push_back("RERANK");
      out.push_back(VectorRerankTypeName(f.vector_params.rerank));
      out.push_back("OVERSAMPLE");
      out.push_back(std::to_string(f.vector_params.bq_oversample));
    }
    out.push_back("DIM");
    out.push_back(std::to_string(f.vector_params.dim));
    out.push_back("DISTANCE_METRIC");
    out.push_back(DistanceMetricName(f.vector_params.metric));
    if (f.vector_params.algorithm == VectorAlgorithm::kHnsw) {
      out.push_back("M");
      out.push_back(std::to_string(f.vector_params.m));
      out.push_back("EF_CONSTRUCTION");
      out.push_back(std::to_string(f.vector_params.ef_construction));
    }
  } else if (f.type == FieldType::kGeoShape) {
    out.push_back(f.geoshape_coord == GeoShapeCoord::kFlat ? "FLAT" : "SPHERICAL");
  }
  if (f.nostem) out.push_back("NOSTEM");
  if (!f.phonetic.empty()) {
    out.push_back("PHONETIC");
    out.push_back(f.phonetic);
  }
  if (f.type == FieldType::kTag && f.tag_separator != ',') {
    out.push_back("SEPARATOR");
    out.emplace_back(1, f.tag_separator);
  }
  if (f.type == FieldType::kTag && f.case_sensitive) {
    out.push_back("CASESENSITIVE");
  }
  if (f.sortable) out.push_back("SORTABLE");
  if (f.noindex) out.push_back("NOINDEX");
  if (f.index_missing) out.push_back("INDEXMISSING");
  if (f.index_empty) out.push_back("INDEXEMPTY");
  if (f.weight != 1.0) {
    out.push_back("WEIGHT");
    out.push_back(std::to_string(f.weight));
  }
}

std::vector<std::string> BuildFtCreateArgs(const IndexSpec& spec) {
  std::vector<std::string> args;
  args.push_back(spec.name);
  if (!spec.prefixes.empty()) {
    args.push_back("ON");
    args.push_back(spec.source_type == IndexSourceType::kJson ? "JSON" : "HASH");
    args.push_back("PREFIX");
    args.push_back(std::to_string(spec.prefixes.size()));
    for (const auto& p : spec.prefixes) args.push_back(p);
  }
  if (!spec.language.empty() && spec.language != "english") {
    args.push_back("LANGUAGE");
    args.push_back(spec.language);
  }
  if (spec.has_custom_stopwords) {
    args.push_back("STOPWORDS");
    args.push_back(std::to_string(spec.custom_stopwords.size()));
    for (const auto& sw : spec.custom_stopwords) args.push_back(sw);
  }
  if (spec.nofreqs) args.push_back("NOFREQS");
  if (spec.nooffsets) args.push_back("NOOFFSETS");
  if (spec.nohl) args.push_back("NOHL");
  if (spec.skip_initial_scan) args.push_back("SKIPINITIALSCAN");
  if (spec.temporary_ttl > 0) {
    args.push_back("TEMPORARY");
    args.push_back(std::to_string(spec.temporary_ttl));
  }
  if (!spec.filter_expr.empty()) {
    args.push_back("FILTER");
    args.push_back(spec.filter_expr);
  }
  if (!spec.language_field.empty()) {
    args.push_back("LANGUAGE_FIELD");
    args.push_back(spec.language_field);
  }
  if (!spec.custom_dict.empty()) {
    args.push_back("DICT");
    args.push_back(spec.custom_dict);
  }
  args.push_back("SCHEMA");
  for (const auto& f : spec.fields) {
    AppendFieldSpecArgs(f, args);
  }
  return args;
}

// Internal helper declared in search_commands.h, defined below

bool CreateIndexFromRdb(
    const std::string& name, IndexSpec spec,
    const std::vector<
        std::pair<std::string, std::unordered_map<std::string, std::string>>>&
        docs,
    const std::unordered_map<std::string, std::string>* prebuilt_hnsw) {
  IndexEntry entry;
  entry.spec = std::move(spec);
  entry.SetLastAccessMs(NowMs());

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
  {
    std::unique_lock<std::shared_mutex> lk(g_indices_mutex);
    g_indices[name] = std::make_shared<IndexEntry>(std::move(entry));
  }
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

void AddDocToIndicesInternal(
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
      std::unordered_set<std::string> cdict_copy;
      const std::unordered_set<std::string>* cdict = nullptr;
      if (!entry.spec.custom_dict.empty()) {
        std::shared_lock<std::shared_mutex> dlk(g_term_dicts_mutex);
        auto dit = g_term_dicts.find(entry.spec.custom_dict);
        if (dit != g_term_dicts.end()) {
          cdict_copy = dit->second.Terms();
          cdict = &cdict_copy;
        }
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

