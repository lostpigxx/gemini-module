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

  RedisModule_SaveUnsigned(rdb, spec.prefixes.size());
  for (auto& p : spec.prefixes) {
    RedisModule_SaveStringBuffer(rdb, p.c_str(), p.size());
  }

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
      RedisModule_SaveUnsigned(rdb, f.vector_params.m);
      RedisModule_SaveUnsigned(rdb, f.vector_params.ef_construction);
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
  if (encver != 1 && encver != 2 && encver != kSearchEncVer) return nullptr;

  std::string index_name = LoadString(rdb);

  std::vector<std::string> prefixes;
  if (encver >= 2) {
    uint64_t prefix_count = RedisModule_LoadUnsigned(rdb);
    for (uint64_t i = 0; i < prefix_count; i++) {
      prefixes.push_back(LoadString(rdb));
    }
  }

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
      if (encver >= 3) {
        f.vector_params.m = static_cast<size_t>(RedisModule_LoadUnsigned(rdb));
        f.vector_params.ef_construction = static_cast<size_t>(RedisModule_LoadUnsigned(rdb));
      }
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

  IndexSpec spec{index_name, std::move(fields), std::move(prefixes)};
  CreateIndexFromRdb(index_name, std::move(spec), docs);

  return new std::string(std::move(index_name));
}

// EmitAOF helper: since RedisModule_EmitAOF is variadic and C++ can't forward
// a dynamic vector, we dispatch by arg count up to 30 (covers schemas with
// up to ~12 fields including VECTOR params).
static void EmitAofCreate(RedisModuleIO* aof, RedisModuleString* key,
                          const std::vector<std::string>& args) {
  std::vector<const char*> a;
  a.reserve(args.size());
  for (auto& s : args) a.push_back(s.c_str());
  size_t n = a.size();
  std::string fmt = "s";
  for (size_t i = 0; i < n; i++) fmt += "c";
  const char* f = fmt.c_str();

  #define A(i) a[i]
  switch (n) {
    case 3: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2)); break;
    case 4: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3)); break;
    case 5: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4)); break;
    case 6: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5)); break;
    case 7: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6)); break;
    case 8: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7)); break;
    case 9: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8)); break;
    case 10: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9)); break;
    case 11: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10)); break;
    case 12: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11)); break;
    case 13: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12)); break;
    case 14: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13)); break;
    case 15: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14)); break;
    case 16: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14),A(15)); break;
    case 17: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14),A(15),A(16)); break;
    case 18: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14),A(15),A(16),A(17)); break;
    case 19: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14),A(15),A(16),A(17),A(18)); break;
    case 20: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14),A(15),A(16),A(17),A(18),A(19)); break;
    case 21: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14),A(15),A(16),A(17),A(18),A(19),A(20)); break;
    case 22: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14),A(15),A(16),A(17),A(18),A(19),A(20),A(21)); break;
    case 23: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14),A(15),A(16),A(17),A(18),A(19),A(20),A(21),A(22)); break;
    case 24: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14),A(15),A(16),A(17),A(18),A(19),A(20),A(21),A(22),A(23)); break;
    case 25: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14),A(15),A(16),A(17),A(18),A(19),A(20),A(21),A(22),A(23),A(24)); break;
    case 26: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14),A(15),A(16),A(17),A(18),A(19),A(20),A(21),A(22),A(23),A(24),A(25)); break;
    case 27: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14),A(15),A(16),A(17),A(18),A(19),A(20),A(21),A(22),A(23),A(24),A(25),A(26)); break;
    case 28: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14),A(15),A(16),A(17),A(18),A(19),A(20),A(21),A(22),A(23),A(24),A(25),A(26),A(27)); break;
    case 29: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14),A(15),A(16),A(17),A(18),A(19),A(20),A(21),A(22),A(23),A(24),A(25),A(26),A(27),A(28)); break;
    case 30: RedisModule_EmitAOF(aof,"FT.CREATE",f,key,A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),A(10),A(11),A(12),A(13),A(14),A(15),A(16),A(17),A(18),A(19),A(20),A(21),A(22),A(23),A(24),A(25),A(26),A(27),A(28),A(29)); break;
    default: break;
  }
  #undef A
}

void AofRewriteSearch(RedisModuleIO* aof, RedisModuleString* key,
                      void* value) {
  auto* name_ptr = static_cast<std::string*>(value);
  auto* entry = GetIndexEntry(*name_ptr);
  if (!entry) return;

  auto& spec = entry->spec;

  std::vector<std::string> create_args;
  if (!spec.prefixes.empty()) {
    create_args.push_back("ON");
    create_args.push_back("HASH");
    create_args.push_back("PREFIX");
    create_args.push_back(std::to_string(spec.prefixes.size()));
    for (auto& p : spec.prefixes) create_args.push_back(p);
  }
  create_args.push_back("SCHEMA");
  for (auto& f : spec.fields) {
    create_args.push_back(f.name);
    create_args.push_back(FieldTypeName(f.type));
    if (f.type == FieldType::kVector) {
      create_args.push_back(VectorAlgorithmName(f.vector_params.algorithm));
      create_args.push_back("DIM");
      create_args.push_back(std::to_string(f.vector_params.dim));
      create_args.push_back("DISTANCE_METRIC");
      create_args.push_back(DistanceMetricName(f.vector_params.metric));
      if (f.vector_params.algorithm == VectorAlgorithm::kHnsw) {
        create_args.push_back("M");
        create_args.push_back(std::to_string(f.vector_params.m));
        create_args.push_back("EF_CONSTRUCTION");
        create_args.push_back(std::to_string(f.vector_params.ef_construction));
      }
    }
  }
  EmitAofCreate(aof, key, create_args);

  auto all_ids = entry->doc_store.AllIds();
  for (auto& doc_id : all_ids) {
    const auto* doc = entry->doc_store.Get(doc_id);
    if (!doc) continue;

    std::vector<std::string> add_args;
    add_args.push_back("FIELDS");
    for (auto& [k, v] : doc->fields) {
      add_args.push_back(k);
      add_args.push_back(v);
    }

    // FT.ADD <index> <doc_id> FIELDS <k1> <v1> ...
    // Emit each doc: format "scb" for key + doc_id + field pairs as binary
    // Use "scc" + pairs of "cb" for each field
    // Simpler: emit FT.ADD with "sc" for key+doc_id, then "c" for FIELDS,
    // then pairs of "cb" for each field key + binary value
    std::string add_fmt = "scc";
    for (size_t i = 0; i < doc->fields.size(); i++) add_fmt += "cb";

    // Again, variadic limitation. For FT.ADD with up to 10 fields (23 args):
    std::vector<const char*> ak;
    std::vector<size_t> al;
    ak.reserve(add_args.size());
    al.reserve(add_args.size());
    for (auto& s : add_args) {
      ak.push_back(s.c_str());
      al.push_back(s.size());
    }

    // For FT.ADD, use a simplified approach: emit with "sccccc..." format
    // doc fields are always string, so "c" works for keys and "b" for values
    // that may contain binary data (vectors).
    // Actually for simplicity use "b" for all field values.
    size_t nf = doc->fields.size();
    if (nf == 0) {
      RedisModule_EmitAOF(aof, "FT.ADD", "scc", key, doc_id.c_str(), "FIELDS");
    } else {
      // Build the field pairs as interleaved key(c) + value(b)
      // We need 1(key) + 1(doc_id) + 1(FIELDS) + 2*nf args
      // Build a simpler approach: just iterate and build the format
      std::string af = "scc";
      for (size_t i = 0; i < nf; i++) af += "cb";

      // Dispatch by number of fields (up to 10 fields = 23 total args)
      size_t ai = 1;
      switch (nf) {
        #define F(i) ak[ai + (i)*2], ak[ai + (i)*2 + 1], al[ai + (i)*2 + 1]
        case 1: RedisModule_EmitAOF(aof,"FT.ADD",af.c_str(),key,doc_id.c_str(),ak[0],F(0)); break;
        case 2: RedisModule_EmitAOF(aof,"FT.ADD",af.c_str(),key,doc_id.c_str(),ak[0],F(0),F(1)); break;
        case 3: RedisModule_EmitAOF(aof,"FT.ADD",af.c_str(),key,doc_id.c_str(),ak[0],F(0),F(1),F(2)); break;
        case 4: RedisModule_EmitAOF(aof,"FT.ADD",af.c_str(),key,doc_id.c_str(),ak[0],F(0),F(1),F(2),F(3)); break;
        case 5: RedisModule_EmitAOF(aof,"FT.ADD",af.c_str(),key,doc_id.c_str(),ak[0],F(0),F(1),F(2),F(3),F(4)); break;
        case 6: RedisModule_EmitAOF(aof,"FT.ADD",af.c_str(),key,doc_id.c_str(),ak[0],F(0),F(1),F(2),F(3),F(4),F(5)); break;
        case 7: RedisModule_EmitAOF(aof,"FT.ADD",af.c_str(),key,doc_id.c_str(),ak[0],F(0),F(1),F(2),F(3),F(4),F(5),F(6)); break;
        case 8: RedisModule_EmitAOF(aof,"FT.ADD",af.c_str(),key,doc_id.c_str(),ak[0],F(0),F(1),F(2),F(3),F(4),F(5),F(6),F(7)); break;
        case 9: RedisModule_EmitAOF(aof,"FT.ADD",af.c_str(),key,doc_id.c_str(),ak[0],F(0),F(1),F(2),F(3),F(4),F(5),F(6),F(7),F(8)); break;
        case 10: RedisModule_EmitAOF(aof,"FT.ADD",af.c_str(),key,doc_id.c_str(),ak[0],F(0),F(1),F(2),F(3),F(4),F(5),F(6),F(7),F(8),F(9)); break;
        #undef F
        default: break;
      }
    }
  }
}

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
