#include "search_rdb.h"
#include "search_commands.h"

#include <cstdint>
#include <string>
#include <unordered_map>

RedisModuleType* SearchModuleType = nullptr;

void RdbSaveSearch(RedisModuleIO* rdb, void* value) {
  auto* name_ptr = static_cast<std::string*>(value);
  auto* entry = GetIndexEntry(*name_ptr);
  if (!entry) return;

  auto& spec = entry->spec;

  RedisModule_SaveStringBuffer(rdb, spec.name.c_str(), spec.name.size());

  RedisModule_SaveUnsigned(rdb, spec.fields.size());
  for (auto& f : spec.fields) {
    RedisModule_SaveStringBuffer(rdb, f.name.c_str(), f.name.size());
    RedisModule_SaveUnsigned(rdb, static_cast<uint64_t>(f.type));
    if (f.type == FieldType::kVector) {
      RedisModule_SaveUnsigned(rdb,
                                static_cast<uint64_t>(f.vector_params.algorithm));
      RedisModule_SaveUnsigned(rdb, f.vector_params.dim);
      RedisModule_SaveUnsigned(rdb,
                                static_cast<uint64_t>(f.vector_params.metric));
    }
  }

  auto all_ids = entry->doc_store.AllIds();
  RedisModule_SaveUnsigned(rdb, all_ids.size());
  for (auto& doc_id : all_ids) {
    RedisModule_SaveStringBuffer(rdb, doc_id.c_str(), doc_id.size());
    const auto* doc = entry->doc_store.Get(doc_id);
    if (doc) {
      RedisModule_SaveUnsigned(rdb, doc->fields.size());
      for (auto& [k, v] : doc->fields) {
        RedisModule_SaveStringBuffer(rdb, k.c_str(), k.size());
        RedisModule_SaveStringBuffer(rdb, v.c_str(), v.size());
      }
    } else {
      RedisModule_SaveUnsigned(rdb, 0);
    }
  }
}

static std::string LoadString(RedisModuleIO* rdb) {
  size_t len = 0;
  char* buf = RedisModule_LoadStringBuffer(rdb, &len);
  std::string s(buf, len);
  RedisModule_Free(buf);
  return s;
}

void* RdbLoadSearch(RedisModuleIO* rdb, int encver) {
  if (encver != kSearchEncVer) return nullptr;

  std::string index_name = LoadString(rdb);

  uint64_t field_count = RedisModule_LoadUnsigned(rdb);
  std::vector<FieldSpec> fields;
  for (uint64_t i = 0; i < field_count; i++) {
    FieldSpec f;
    f.name = LoadString(rdb);
    f.type = static_cast<FieldType>(RedisModule_LoadUnsigned(rdb));
    if (f.type == FieldType::kVector) {
      f.vector_params.algorithm =
          static_cast<VectorAlgorithm>(RedisModule_LoadUnsigned(rdb));
      f.vector_params.dim = static_cast<size_t>(RedisModule_LoadUnsigned(rdb));
      f.vector_params.metric =
          static_cast<DistanceMetric>(RedisModule_LoadUnsigned(rdb));
    }
    fields.push_back(std::move(f));
  }

  uint64_t doc_count = RedisModule_LoadUnsigned(rdb);
  std::vector<std::pair<std::string, std::unordered_map<std::string, std::string>>>
      docs;
  for (uint64_t i = 0; i < doc_count; i++) {
    std::string doc_id = LoadString(rdb);
    uint64_t nfields = RedisModule_LoadUnsigned(rdb);
    std::unordered_map<std::string, std::string> doc_fields;
    for (uint64_t j = 0; j < nfields; j++) {
      std::string k = LoadString(rdb);
      std::string v = LoadString(rdb);
      doc_fields[std::move(k)] = std::move(v);
    }
    docs.emplace_back(std::move(doc_id), std::move(doc_fields));
  }

  IndexSpec spec{index_name, std::move(fields)};
  CreateIndexFromRdb(index_name, std::move(spec), docs);

  return new std::string(std::move(index_name));
}

void AofRewriteSearch(RedisModuleIO* /*aof*/, RedisModuleString* /*key*/,
                      void* /*value*/) {}

void FreeSearch(void* value) {
  auto* name_ptr = static_cast<std::string*>(value);
  EraseIndexEntry(*name_ptr);
  delete name_ptr;
}

size_t SearchMemUsage(const void* value) {
  auto* name_ptr = static_cast<const std::string*>(value);
  auto* entry = GetIndexEntry(*name_ptr);
  if (!entry) return 0;
  return sizeof(*entry) + entry->doc_store.Size() * 256;
}
