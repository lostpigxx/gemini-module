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
#include "index_spec.h"
#include "numeric_index.h"
#include "suggest_dict.h"
#include "tag_index.h"
#include "text_index.h"
#include "vector_index.h"

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
  SynonymMap synonyms;
};

int RegisterSearchCommands(RedisModuleCtx* ctx);

IndexEntry* GetIndexEntry(const std::string& name);
bool CreateIndexEntry(const std::string& name, IndexEntry entry);
void EraseIndexEntry(const std::string& name);
void CreateIndexFromRdb(const std::string& name, IndexSpec spec,
                        const std::vector<std::pair<std::string,
                            std::unordered_map<std::string, std::string>>>& docs);
