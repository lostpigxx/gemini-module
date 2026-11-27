#pragma once

#ifndef REDISMODULE_API
#define REDISMODULE_API extern
#define _SEARCH_CMD_API_DEFINED
#endif

extern "C" {
#include "redismodule.h"
}

#ifdef _SEARCH_CMD_API_DEFINED
#undef REDISMODULE_API
#undef _SEARCH_CMD_API_DEFINED
#endif

#include "document_store.h"
#include "geo_index.h"
#include "geoshape_index.h"
#include "index_spec.h"
#include "numeric_index.h"
#include "sort_index.h"
#include "suggest_dict.h"
#include "tag_index.h"
#include "text_index.h"
#include "vector_index.h"

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct IndexEntry {
  IndexSpec spec;
  DocumentStore doc_store;
  TagFieldIndices tag_indices;
  NumericFieldIndices numeric_indices;
  VectorFieldIndices vector_indices;
  TextFieldIndices text_indices;
  GeoFieldIndices geo_indices;
  GeoShapeFieldIndices geoshape_indices;
  SortFieldIndices sort_indices;
  SynonymMap synonyms;
  int64_t last_access_ms = 0;
  uint64_t generation = 0;
  bool bg_indexing = false;
  uint64_t bg_scanned_keys = 0;
  uint64_t bg_total_keys = 0;
  double percent_indexed = 1.0;
  std::unique_ptr<std::shared_mutex> rw_mutex = std::make_unique<std::shared_mutex>();

  std::shared_mutex& Mutex() const { return *rw_mutex; }
};

int RegisterSearchCommands(RedisModuleCtx* ctx);

IndexEntry* GetIndexEntry(const std::string& name);
std::shared_ptr<IndexEntry> GetIndexEntryShared(const std::string& name);
bool CreateIndexEntry(const std::string& name, IndexEntry entry);
void EraseIndexEntry(const std::string& name);
bool CreateIndexFromRdb(
    const std::string& name, IndexSpec spec,
    const std::vector<
        std::pair<std::string, std::unordered_map<std::string, std::string>>>&
        docs,
    const std::unordered_map<std::string, std::string>* prebuilt_hnsw = nullptr);

std::unordered_map<std::string, std::string>& GetAliases();
std::unordered_map<std::string, SuggestDict>& GetSuggestDicts();
std::unordered_map<std::string, TermDict>& GetTermDicts();
std::unordered_map<std::string, std::string>& GetConfig();
void ClearAllGlobalState();
