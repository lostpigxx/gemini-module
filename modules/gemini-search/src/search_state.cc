#include "search_internal.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

static std::unordered_map<std::string, IndexEntry> g_indices;
static std::unordered_map<std::string, std::string> g_aliases;
static std::unordered_map<uint64_t, AggCursor> g_cursors;
static uint64_t g_next_cursor_id = 1;
static std::unordered_map<std::string, SuggestDict> g_suggest_dicts;
static std::unordered_map<std::string, TermDict> g_term_dicts;
static std::unordered_map<std::string, std::string> g_config = {
    {"DEFAULT_DIALECT", "2"},
    {"TIMEOUT", "500"},
    {"MAXEXPANSIONS", "200"},
    {"MINSTEMLEN", "4"},
};

std::unordered_map<std::string, IndexEntry>& GetIndices() { return g_indices; }
std::unordered_map<uint64_t, AggCursor>& GetCursors() { return g_cursors; }
uint64_t NextCursorId() { return g_next_cursor_id++; }

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

void PurgeExpiredCursors(int64_t now_ms) {
  for (auto it = g_cursors.begin(); it != g_cursors.end(); ) {
    if (it->second.max_idle_ms > 0 && it->second.last_access_ms > 0 &&
        now_ms - it->second.last_access_ms >= it->second.max_idle_ms) {
      it = g_cursors.erase(it);
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
  if (it == g_indices.end()) return nullptr;
  return &it->second;
}

bool CreateIndexEntry(const std::string& name, IndexEntry entry) {
  auto [it, inserted] = g_indices.try_emplace(name, std::move(entry));
  return inserted;
}

void EraseIndexEntry(const std::string& name) { g_indices.erase(name); }

std::unordered_map<std::string, std::string>& GetAliases() { return g_aliases; }
std::unordered_map<std::string, SuggestDict>& GetSuggestDicts() { return g_suggest_dicts; }
std::unordered_map<std::string, TermDict>& GetTermDicts() { return g_term_dicts; }
std::unordered_map<std::string, std::string>& GetConfig() { return g_config; }

void ClearAllGlobalState() {
  g_indices.clear();
  g_aliases.clear();
  g_cursors.clear();
  g_next_cursor_id = 1;
  g_suggest_dicts.clear();
  g_term_dicts.clear();
  g_config = {
      {"DEFAULT_DIALECT", "2"},
      {"TIMEOUT", "500"},
      {"MAXEXPANSIONS", "200"},
      {"MINSTEMLEN", "4"},
  };
}

// Forward declaration of static helper defined below
void AddDocToIndices(
    IndexEntry& entry, const std::string& doc_id,
    const std::unordered_map<std::string, std::string>& fields);

void CreateIndexFromRdb(
    const std::string& name, IndexSpec spec,
    const std::vector<
        std::pair<std::string, std::unordered_map<std::string, std::string>>>&
        docs) {
  IndexEntry entry;
  entry.spec = std::move(spec);
  entry.last_access_ms = NowMs();
  for (auto& [doc_id, fields] : docs) {
    entry.doc_store.Add(doc_id, fields);
    AddDocToIndices(entry, doc_id, fields);
  }
  g_indices[name] = std::move(entry);
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

void RemoveDocFromIndices(IndexEntry& entry,
                                 const std::string& doc_id) {
  const auto* doc = entry.doc_store.Get(doc_id);
  if (!doc) return;
  for (auto& [fname, fval] : doc->fields) {
    const auto* fspec = entry.spec.FindField(fname);
    if (!fspec || fspec->noindex) continue;
    if (fspec->type == FieldType::kTag) {
      entry.tag_indices.GetOrCreate(fname).Remove(fval, doc_id);
    } else if (fspec->type == FieldType::kNumeric) {
      double num;
      if (TryParseDouble(fval, num)) {
        entry.numeric_indices.GetOrCreate(fname).Remove(num, doc_id);
      }
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

void AddDocToIndices(
    IndexEntry& entry, const std::string& doc_id,
    const std::unordered_map<std::string, std::string>& fields) {
  for (auto& [fname, fval] : fields) {
    const auto* fspec = entry.spec.FindField(fname);
    if (!fspec || fspec->noindex) continue;
    if (fspec->type == FieldType::kTag) {
      entry.tag_indices.GetOrCreate(fname).Add(fval, doc_id);
    } else if (fspec->type == FieldType::kNumeric) {
      double num;
      if (TryParseDouble(fval, num)) {
        entry.numeric_indices.GetOrCreate(fname).Add(num, doc_id);
      }
    } else if (fspec->type == FieldType::kVector) {
      size_t dim = fspec->vector_params.dim;
      if (fval.size() == dim * sizeof(float)) {
        auto& vidx = entry.vector_indices.GetOrCreate(fname, fspec->vector_params);
        vidx.Add(doc_id, reinterpret_cast<const float*>(fval.data()));
      }
    } else if (fspec->type == FieldType::kText) {
      entry.text_indices.GetOrCreate(fname).Add(doc_id, fval, !fspec->phonetic.empty());
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

