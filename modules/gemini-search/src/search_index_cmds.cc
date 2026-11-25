#include "search_internal.h"
#include "expr_engine.h"
#include "search_rdb.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>


// FT.CREATE <index_name> [ON HASH|JSON PREFIX <count> <prefix> ...] SCHEMA <field> <type> [params...]
int FtCreateCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
  if (argc < 5) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto index_name = ArgView(argv[1]);

  int i = 2;
  std::vector<std::string> prefixes;
  IndexSourceType source_type = IndexSourceType::kHash;

  if (i < argc && MatchArg(ArgView(argv[i]), "ON")) {
    i++;
    if (i >= argc) {
      return RedisModule_ReplyWithError(ctx, "ERR ON requires HASH or JSON");
    }
    if (MatchArg(ArgView(argv[i]), "HASH")) {
      source_type = IndexSourceType::kHash;
    } else if (MatchArg(ArgView(argv[i]), "JSON")) {
      source_type = IndexSourceType::kJson;
    } else {
      return RedisModule_ReplyWithError(ctx, "ERR ON requires HASH or JSON");
    }
    i++;
    if (i >= argc || !MatchArg(ArgView(argv[i]), "PREFIX")) {
      return RedisModule_ReplyWithError(ctx, "ERR expected PREFIX after ON HASH/JSON");
    }
    i++;
    if (i >= argc) {
      return RedisModule_ReplyWithError(ctx, "ERR PREFIX requires a count");
    }
    char* endptr = nullptr;
    long prefix_count = std::strtol(std::string(ArgView(argv[i])).c_str(), &endptr, 10);
    i++;
    if (*endptr != '\0' || prefix_count <= 0) {
      return RedisModule_ReplyWithError(ctx, "ERR PREFIX count must be a positive integer");
    }
    for (long p = 0; p < prefix_count; p++) {
      if (i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR not enough PREFIX values");
      }
      prefixes.emplace_back(ArgView(argv[i]));
      i++;
    }
  }

  std::string index_language = "english";
  std::vector<std::string> custom_stopwords;
  bool has_custom_stopwords = false;
  bool idx_nofreqs = false;
  bool idx_nooffsets = false;
  bool idx_nohl = false;
  int idx_temporary = 0;
  std::string idx_filter_expr;
  bool idx_skip_initial_scan = false;

  while (i < argc && !MatchArg(ArgView(argv[i]), "SCHEMA")) {
    auto opt = ArgView(argv[i]);
    if (MatchArg(opt, "LANGUAGE")) {
      i++;
      if (i >= argc) return RedisModule_ReplyWithError(ctx, "ERR LANGUAGE requires a value");
      index_language = std::string(ArgView(argv[i]));
      i++;
    } else if (MatchArg(opt, "STOPWORDS")) {
      i++;
      if (i >= argc) return RedisModule_ReplyWithError(ctx, "ERR STOPWORDS requires count");
      char* ep = nullptr;
      long sw_count = std::strtol(std::string(ArgView(argv[i])).c_str(), &ep, 10);
      i++;
      if (*ep != '\0' || sw_count < 0)
        return RedisModule_ReplyWithError(ctx, "ERR STOPWORDS count must be non-negative");
      has_custom_stopwords = true;
      for (long sw = 0; sw < sw_count; sw++) {
        if (i >= argc) return RedisModule_ReplyWithError(ctx, "ERR not enough STOPWORDS");
        custom_stopwords.emplace_back(ArgView(argv[i]));
        i++;
      }
    } else if (MatchArg(opt, "NOFREQS")) {
      idx_nofreqs = true;
      i++;
    } else if (MatchArg(opt, "NOOFFSETS")) {
      idx_nooffsets = true;
      i++;
    } else if (MatchArg(opt, "NOHL")) {
      idx_nohl = true;
      i++;
    } else if (MatchArg(opt, "TEMPORARY")) {
      i++;
      if (i >= argc) return RedisModule_ReplyWithError(ctx, "ERR TEMPORARY requires seconds");
      char* ep = nullptr;
      idx_temporary = static_cast<int>(std::strtol(std::string(ArgView(argv[i])).c_str(), &ep, 10));
      if (*ep != '\0' || idx_temporary < 0)
        return RedisModule_ReplyWithError(ctx, "ERR TEMPORARY must be non-negative");
      i++;
    } else if (MatchArg(opt, "SKIPINITIALSCAN")) {
      idx_skip_initial_scan = true;
      i++;
    } else if (MatchArg(opt, "FILTER")) {
      i++;
      if (i >= argc) return RedisModule_ReplyWithError(ctx, "ERR FILTER requires an expression");
      idx_filter_expr = std::string(ArgView(argv[i]));
      ExprNode test_expr;
      std::string expr_err;
      if (!ParseExpr(idx_filter_expr, test_expr, expr_err)) {
        return RedisModule_ReplyWithError(ctx, expr_err.c_str());
      }
      i++;
    } else if (MatchArg(opt, "MAXTEXTFIELDS")) {
      i++;
    } else {
      break;
    }
  }

  if (i >= argc || !MatchArg(ArgView(argv[i]), "SCHEMA")) {
    return RedisModule_ReplyWithError(ctx, "ERR syntax error, expected SCHEMA keyword");
  }
  i++;

  std::vector<FieldSpec> fields;
  while (i < argc) {
    if (i + 1 >= argc) {
      return RedisModule_ReplyWithError(
          ctx, "ERR syntax error, missing field type");
    }

    auto fname = ArgView(argv[i]);
    i++;

    FieldSpec fspec;
    fspec.name = std::string(fname);

    // JSON schema: $.path AS alias TYPE
    if (source_type == IndexSourceType::kJson && !fname.empty() && fname[0] == '$') {
      fspec.json_path = std::string(fname);
      if (i + 2 >= argc || !MatchArg(ArgView(argv[i]), "AS")) {
        return RedisModule_ReplyWithError(ctx, "ERR JSON field requires AS alias");
      }
      i++;
      fspec.name = std::string(ArgView(argv[i]));
      i++;
    }

    if (i >= argc) {
      return RedisModule_ReplyWithError(ctx, "ERR syntax error, missing field type");
    }
    auto ftype_str = ArgView(argv[i]);
    i++;

    if (MatchArg(ftype_str, "TAG")) {
      fspec.type = FieldType::kTag;
    } else if (MatchArg(ftype_str, "NUMERIC")) {
      fspec.type = FieldType::kNumeric;
    } else if (MatchArg(ftype_str, "VECTOR")) {
      fspec.type = FieldType::kVector;

      if (i >= argc) {
        return RedisModule_ReplyWithError(
            ctx, "ERR VECTOR field requires algorithm (FLAT)");
      }
      auto algo_str = ArgView(argv[i]);
      i++;
      if (MatchArg(algo_str, "FLAT")) {
        fspec.vector_params.algorithm = VectorAlgorithm::kFlat;
      } else if (MatchArg(algo_str, "HNSW")) {
        fspec.vector_params.algorithm = VectorAlgorithm::kHnsw;
      } else {
        return RedisModule_ReplyWithError(
            ctx, "ERR unknown VECTOR algorithm, expected FLAT or HNSW");
      }

      bool has_dim = false, has_metric = false;
      while (i < argc) {
        auto param = ArgView(argv[i]);
        if (MatchArg(param, "TYPE")) {
          if (i + 1 >= argc) {
            return RedisModule_ReplyWithError(ctx, "ERR TYPE requires a value");
          }
          auto dtype_str = ArgView(argv[i + 1]);
          if (MatchArg(dtype_str, "FLOAT32")) {
            fspec.vector_params.data_type = VectorDataType::kFloat32;
          } else if (MatchArg(dtype_str, "FLOAT64")) {
            fspec.vector_params.data_type = VectorDataType::kFloat64;
          } else {
            return RedisModule_ReplyWithError(
                ctx, "ERR unknown VECTOR TYPE, expected FLOAT32 or FLOAT64");
          }
          i += 2;
        } else if (MatchArg(param, "DIM")) {
          if (i + 1 >= argc) {
            return RedisModule_ReplyWithError(ctx, "ERR DIM requires a value");
          }
          auto dim_str = ArgView(argv[i + 1]);
          char* endptr = nullptr;
          long dim_val =
              std::strtol(std::string(dim_str).c_str(), &endptr, 10);
          if (*endptr != '\0' || dim_val <= 0) {
            return RedisModule_ReplyWithError(
                ctx, "ERR DIM must be a positive integer");
          }
          fspec.vector_params.dim = static_cast<size_t>(dim_val);
          has_dim = true;
          i += 2;
        } else if (MatchArg(param, "DISTANCE_METRIC")) {
          if (i + 1 >= argc) {
            return RedisModule_ReplyWithError(
                ctx, "ERR DISTANCE_METRIC requires a value");
          }
          auto metric_str = ArgView(argv[i + 1]);
          if (MatchArg(metric_str, "L2")) {
            fspec.vector_params.metric = DistanceMetric::kL2;
          } else if (MatchArg(metric_str, "COSINE")) {
            fspec.vector_params.metric = DistanceMetric::kCosine;
          } else if (MatchArg(metric_str, "IP")) {
            fspec.vector_params.metric = DistanceMetric::kIP;
          } else {
            return RedisModule_ReplyWithError(
                ctx, "ERR unknown DISTANCE_METRIC, expected L2, COSINE, or IP");
          }
          has_metric = true;
          i += 2;
        } else if (MatchArg(param, "M")) {
          if (i + 1 >= argc) {
            return RedisModule_ReplyWithError(ctx, "ERR M requires a value");
          }
          auto m_str = ArgView(argv[i + 1]);
          char* endptr = nullptr;
          long m_val = std::strtol(std::string(m_str).c_str(), &endptr, 10);
          if (*endptr != '\0' || m_val <= 0) {
            return RedisModule_ReplyWithError(ctx, "ERR M must be a positive integer");
          }
          fspec.vector_params.m = static_cast<size_t>(m_val);
          i += 2;
        } else if (MatchArg(param, "EF_CONSTRUCTION")) {
          if (i + 1 >= argc) {
            return RedisModule_ReplyWithError(ctx, "ERR EF_CONSTRUCTION requires a value");
          }
          auto ef_str = ArgView(argv[i + 1]);
          char* endptr = nullptr;
          long ef_val = std::strtol(std::string(ef_str).c_str(), &endptr, 10);
          if (*endptr != '\0' || ef_val <= 0) {
            return RedisModule_ReplyWithError(ctx, "ERR EF_CONSTRUCTION must be a positive integer");
          }
          fspec.vector_params.ef_construction = static_cast<size_t>(ef_val);
          i += 2;
        } else {
          break;
        }
      }

      if (!has_dim) {
        return RedisModule_ReplyWithError(
            ctx, "ERR VECTOR field requires DIM parameter");
      }
      if (!has_metric) {
        return RedisModule_ReplyWithError(
            ctx, "ERR VECTOR field requires DISTANCE_METRIC parameter");
      }
    } else if (MatchArg(ftype_str, "TEXT")) {
      fspec.type = FieldType::kText;
    } else if (MatchArg(ftype_str, "GEO")) {
      fspec.type = FieldType::kGeo;
    } else if (MatchArg(ftype_str, "GEOSHAPE")) {
      fspec.type = FieldType::kGeoShape;
    } else {
      return RedisModule_ReplyWithError(
          ctx, "ERR unknown field type, expected TAG, NUMERIC, TEXT, VECTOR, GEO, or GEOSHAPE");
    }

    // Parse field-level attributes: SORTABLE, NOINDEX, NOSTEM, WEIGHT
    while (i < argc) {
      auto attr = ArgView(argv[i]);
      if (MatchArg(attr, "SORTABLE")) {
        fspec.sortable = true;
        i++;
      } else if (MatchArg(attr, "NOINDEX")) {
        fspec.noindex = true;
        i++;
      } else if (MatchArg(attr, "NOSTEM") && fspec.type == FieldType::kText) {
        fspec.nostem = true;
        i++;
      } else if (MatchArg(attr, "WEIGHT") && fspec.type == FieldType::kText) {
        i++;
        if (i >= argc) return RedisModule_ReplyWithError(ctx, "ERR WEIGHT requires a value");
        char* ep = nullptr;
        fspec.weight = std::strtod(std::string(ArgView(argv[i])).c_str(), &ep);
        if (*ep != '\0' || fspec.weight < 0)
          return RedisModule_ReplyWithError(ctx, "ERR WEIGHT must be a non-negative number");
        i++;
      } else if (MatchArg(attr, "PHONETIC") && fspec.type == FieldType::kText) {
        i++;
        if (i >= argc) return RedisModule_ReplyWithError(ctx, "ERR PHONETIC requires a matcher");
        fspec.phonetic = std::string(ArgView(argv[i]));
        i++;
      } else if (MatchArg(attr, "SEPARATOR") && fspec.type == FieldType::kTag) {
        i++;
        if (i >= argc) return RedisModule_ReplyWithError(ctx, "ERR SEPARATOR requires a single character");
        auto sep = ArgView(argv[i]);
        if (sep.size() != 1) return RedisModule_ReplyWithError(ctx, "ERR SEPARATOR must be a single character");
        fspec.tag_separator = sep[0];
        i++;
      } else if (MatchArg(attr, "CASESENSITIVE") && fspec.type == FieldType::kTag) {
        fspec.case_sensitive = true;
        i++;
      } else if (MatchArg(attr, "WITHSUFFIXTRIE") &&
                 (fspec.type == FieldType::kText || fspec.type == FieldType::kTag)) {
        i++;
      } else {
        break;
      }
    }

    for (auto& existing : fields) {
      if (existing.name == fspec.name) {
        return RedisModule_ReplyWithError(ctx,
                                          "ERR duplicate field name in schema");
      }
    }

    fields.push_back(std::move(fspec));
  }

  if (fields.empty()) {
    return RedisModule_ReplyWithError(ctx, "ERR schema requires at least one field");
  }

  std::string idx_str(index_name);
  auto [it, inserted] = GetIndices().try_emplace(idx_str);
  if (!inserted) {
    return RedisModule_ReplyWithError(ctx, "ERR index already exists");
  }
  it->second.spec = IndexSpec{idx_str, std::move(fields), std::move(prefixes),
                              index_language, source_type,
                              std::move(custom_stopwords), has_custom_stopwords,
                              idx_nofreqs, idx_nooffsets, idx_nohl, idx_temporary,
                              std::move(idx_filter_expr), idx_skip_initial_scan};
  TouchIndex(it->second);

  if (SearchModuleType) {
    auto* key = static_cast<RedisModuleKey*>(RedisModule_OpenKey(
        ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
    RedisModule_ModuleTypeSetValue(key, SearchModuleType,
                                   new std::string(idx_str));
  }

  if (it->second.spec.HasPrefixes() && !it->second.spec.skip_initial_scan) {
    ScanExistingKeys(ctx, it->second);
  }

  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.DROPINDEX <index_name> [DD]
int FtDropIndexCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                              int argc) {
  if (argc != 2 && argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  bool delete_docs = false;
  if (argc == 3) {
    if (!MatchArg(ArgView(argv[2]), "DD")) {
      return RedisModule_ReplyWithError(ctx, "ERR unknown option, expected DD");
    }
    delete_docs = true;
  }

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  auto it = GetIndices().find(idx_str);
  if (it == GetIndices().end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  std::vector<std::string> doc_ids;
  if (delete_docs) {
    doc_ids = it->second.doc_store.AllIds();
  }
  GetIndices().erase(it);
  // Remove any aliases pointing to this index
  for (auto ait = GetAliases().begin(); ait != GetAliases().end(); ) {
    if (ait->second == idx_str) ait = GetAliases().erase(ait);
    else ++ait;
  }

  if (SearchModuleType) {
    auto* key = static_cast<RedisModuleKey*>(RedisModule_OpenKey(
        ctx, argv[1], REDISMODULE_WRITE));
    RedisModule_DeleteKey(key);
  }

  for (const auto& doc_id : doc_ids) {
    RedisModuleString* dk = RedisModule_CreateString(ctx, doc_id.data(), doc_id.size());
    auto* dkey = static_cast<RedisModuleKey*>(
        RedisModule_OpenKey(ctx, dk, REDISMODULE_WRITE));
    if (dkey) RedisModule_DeleteKey(dkey);
  }

  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.INFO <index_name>
int FtInfoCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                         int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  auto it = GetIndices().find(idx_str);
  if (it == GetIndices().end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  auto& entry = it->second;

  long info_len = entry.spec.HasPrefixes() ? 10 : 8;
  RedisModule_ReplyWithArray(ctx, info_len);

  RedisModule_ReplyWithSimpleString(ctx, "index_name");
  RedisModule_ReplyWithCString(ctx, entry.spec.name.c_str());

  RedisModule_ReplyWithSimpleString(ctx, "num_docs");
  RedisModule_ReplyWithLongLong(
      ctx, static_cast<long long>(entry.doc_store.Size()));

  RedisModule_ReplyWithSimpleString(ctx, "language");
  RedisModule_ReplyWithCString(ctx, entry.spec.language.c_str());

  if (entry.spec.HasPrefixes()) {
    RedisModule_ReplyWithSimpleString(ctx, "index_definition");
    long def_len = 2 + static_cast<long>(entry.spec.prefixes.size());
    RedisModule_ReplyWithArray(ctx, def_len);
    RedisModule_ReplyWithSimpleString(ctx, "key_type");
    RedisModule_ReplyWithSimpleString(
        ctx, entry.spec.source_type == IndexSourceType::kJson ? "JSON" : "HASH");
    for (auto& p : entry.spec.prefixes) {
      RedisModule_ReplyWithCString(ctx, p.c_str());
    }
  }

  RedisModule_ReplyWithSimpleString(ctx, "fields");
  RedisModule_ReplyWithArray(ctx,
                             static_cast<long>(entry.spec.fields.size()));
  for (auto& f : entry.spec.fields) {
    if (f.type == FieldType::kVector) {
      long vec_info_len = (f.vector_params.algorithm == VectorAlgorithm::kHnsw) ? 12 : 8;
      RedisModule_ReplyWithArray(ctx, vec_info_len);
      RedisModule_ReplyWithCString(ctx, f.name.c_str());
      RedisModule_ReplyWithSimpleString(ctx, "VECTOR");
      RedisModule_ReplyWithSimpleString(ctx, "algorithm");
      RedisModule_ReplyWithSimpleString(
          ctx, VectorAlgorithmName(f.vector_params.algorithm));
      RedisModule_ReplyWithSimpleString(ctx, "dim");
      RedisModule_ReplyWithLongLong(
          ctx, static_cast<long long>(f.vector_params.dim));
      RedisModule_ReplyWithSimpleString(ctx, "distance_metric");
      RedisModule_ReplyWithSimpleString(
          ctx, DistanceMetricName(f.vector_params.metric));
      if (f.vector_params.algorithm == VectorAlgorithm::kHnsw) {
        RedisModule_ReplyWithSimpleString(ctx, "m");
        RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(f.vector_params.m));
        RedisModule_ReplyWithSimpleString(ctx, "ef_construction");
        RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(f.vector_params.ef_construction));
      }
    } else {
      long attr_count = 2;
      if (f.nostem) attr_count++;
      if (f.sortable) attr_count++;
      if (f.noindex) attr_count++;
      if (f.weight != 1.0) attr_count += 2;
      if (!f.json_path.empty()) attr_count += 2;
      RedisModule_ReplyWithArray(ctx, attr_count);
      RedisModule_ReplyWithCString(ctx, f.name.c_str());
      RedisModule_ReplyWithSimpleString(ctx, FieldTypeName(f.type));
      if (f.nostem) RedisModule_ReplyWithSimpleString(ctx, "NOSTEM");
      if (f.sortable) RedisModule_ReplyWithSimpleString(ctx, "SORTABLE");
      if (f.noindex) RedisModule_ReplyWithSimpleString(ctx, "NOINDEX");
      if (f.weight != 1.0) {
        RedisModule_ReplyWithSimpleString(ctx, "WEIGHT");
        RedisModule_ReplyWithCString(ctx, std::to_string(f.weight).c_str());
      }
      if (!f.json_path.empty()) {
        RedisModule_ReplyWithSimpleString(ctx, "json_path");
        RedisModule_ReplyWithCString(ctx, f.json_path.c_str());
      }
    }
  }

  return REDISMODULE_OK;
}

// FT._LIST
int FtListCommand(RedisModuleCtx* ctx, RedisModuleString** /*argv*/,
                         int argc) {
  if (argc != 1) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::vector<std::string> names;
  names.reserve(GetIndices().size());
  for (auto& [k, v] : GetIndices()) {
    names.push_back(k);
  }
  std::sort(names.begin(), names.end());

  RedisModule_ReplyWithArray(ctx, static_cast<long>(names.size()));
  for (auto& n : names) {
    RedisModule_ReplyWithCString(ctx, n.c_str());
  }

  return REDISMODULE_OK;
}

// FT.ADD <index> <doc_id> FIELDS <field1> <val1> [field2 val2 ...]
int FtAddCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                        int argc) {
  if (argc < 6) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str(ArgView(argv[1]));
  auto doc_id = ArgView(argv[2]);
  auto fields_kw = ArgView(argv[3]);

  if (!MatchArg(fields_kw, "FIELDS")) {
    return RedisModule_ReplyWithError(
        ctx, "ERR syntax error, expected FIELDS keyword");
  }

  int field_argc = argc - 4;
  if (field_argc < 2 || field_argc % 2 != 0) {
    return RedisModule_ReplyWithError(
        ctx, "ERR syntax error, FIELDS requires pairs of <field> <value>");
  }

  auto it = GetIndices().find(idx_str);
  if (it == GetIndices().end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  auto& entry = it->second;

  std::unordered_map<std::string, std::string> doc_fields;
  for (int i = 4; i < argc; i += 2) {
    std::string fname_str(ArgView(argv[i]));

    const auto* fspec = entry.spec.FindField(fname_str);
    if (!fspec) {
      return RedisModule_ReplyWithError(ctx, "ERR field not in schema");
    }

    // For VECTOR fields, store raw binary; for others, store as string
    std::string fval_str = ArgStr(argv[i + 1]);

    if (fspec->type == FieldType::kVector) {
      size_t expected = fspec->vector_params.dim *
                        VectorElementSize(fspec->vector_params.data_type);
      if (fval_str.size() != expected) {
        return RedisModule_ReplyWithError(
            ctx, "ERR vector dimension mismatch");
      }
    }

    doc_fields[std::move(fname_str)] = std::move(fval_str);
  }

  std::string doc_id_str(doc_id);

  RemoveDocFromIndices(entry, doc_id_str);
  entry.doc_store.Add(doc_id_str, doc_fields);
  AddDocToIndices(entry, doc_id_str, doc_fields);

  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.DEL <index> <doc_id>
int FtDelCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                        int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  auto it = GetIndices().find(idx_str);
  if (it == GetIndices().end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  auto& entry = it->second;

  std::string doc_id_str(ArgView(argv[2]));
  if (!entry.doc_store.Contains(doc_id_str)) {
    return RedisModule_ReplyWithError(ctx, "ERR document not found");
  }

  RemoveDocFromIndices(entry, doc_id_str);
  entry.doc_store.Remove(doc_id_str);

  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}


// FT.ALTER <index> SCHEMA ADD <field> <type> [params...] [<field> <type> ...]
int FtAlterCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                          int argc) {
  if (argc < 6) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  auto it = GetIndices().find(idx_str);
  if (it == GetIndices().end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  auto& entry = it->second;

  if (!MatchArg(ArgView(argv[2]), "SCHEMA") || !MatchArg(ArgView(argv[3]), "ADD")) {
    return RedisModule_ReplyWithError(ctx, "ERR syntax error, expected SCHEMA ADD");
  }

  int i = 4;
  std::vector<FieldSpec> new_fields;
  while (i < argc) {
    if (i + 1 >= argc) {
      return RedisModule_ReplyWithError(ctx, "ERR syntax error, missing field type");
    }
    auto fname = ArgView(argv[i]);
    auto ftype_str = ArgView(argv[i + 1]);
    i += 2;

    FieldSpec fspec;
    fspec.name = std::string(fname);

    if (entry.spec.FindField(fspec.name)) {
      return RedisModule_ReplyWithError(ctx, "ERR field already exists in schema");
    }
    for (auto& nf : new_fields) {
      if (nf.name == fspec.name) {
        return RedisModule_ReplyWithError(ctx, "ERR duplicate field name");
      }
    }

    if (MatchArg(ftype_str, "TAG")) {
      fspec.type = FieldType::kTag;
    } else if (MatchArg(ftype_str, "NUMERIC")) {
      fspec.type = FieldType::kNumeric;
    } else if (MatchArg(ftype_str, "TEXT")) {
      fspec.type = FieldType::kText;
      if (i < argc && MatchArg(ArgView(argv[i]), "NOSTEM")) {
        fspec.nostem = true;
        i++;
      }
    } else if (MatchArg(ftype_str, "GEO")) {
      fspec.type = FieldType::kGeo;
    } else {
      return RedisModule_ReplyWithError(
          ctx, "ERR unknown field type in ALTER, expected TAG, NUMERIC, TEXT, or GEO");
    }

    new_fields.push_back(std::move(fspec));
  }

  if (new_fields.empty()) {
    return RedisModule_ReplyWithError(ctx, "ERR SCHEMA ADD requires at least one field");
  }

  for (auto& nf : new_fields) {
    entry.spec.fields.push_back(std::move(nf));
  }

  // Re-scan existing docs for new fields if ON HASH
  if (entry.spec.HasPrefixes()) {
    ScanExistingKeys(ctx, entry);
  }

  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.ALIASADD <alias> <index>
int FtAliasAddCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                             int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string alias(ArgView(argv[1]));
  std::string idx(ArgView(argv[2]));

  if (GetIndices().find(idx) == GetIndices().end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  if (GetAliases().find(alias) != GetAliases().end()) {
    return RedisModule_ReplyWithError(ctx, "ERR alias already exists");
  }
  GetAliases()[alias] = idx;
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.ALIASDEL <alias>
int FtAliasDelCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                             int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string alias(ArgView(argv[1]));
  if (GetAliases().erase(alias) == 0) {
    return RedisModule_ReplyWithError(ctx, "ERR alias not found");
  }
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.ALIASUPDATE <alias> <index>
int FtAliasUpdateCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                                int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string alias(ArgView(argv[1]));
  std::string idx(ArgView(argv[2]));

  if (GetIndices().find(idx) == GetIndices().end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  GetAliases()[alias] = idx;
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.TAGVALS <index> <field>
int FtTagvalsCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                            int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  auto it = GetIndices().find(idx_str);
  if (it == GetIndices().end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  auto& entry = it->second;

  std::string field_name(ArgView(argv[2]));
  const auto* fspec = entry.spec.FindField(field_name);
  if (!fspec) {
    return RedisModule_ReplyWithError(ctx, "ERR field not in schema");
  }
  if (fspec->type != FieldType::kTag) {
    return RedisModule_ReplyWithError(ctx, "ERR field is not a TAG field");
  }

  const auto* tidx = entry.tag_indices.Get(field_name);
  if (!tidx) {
    RedisModule_ReplyWithArray(ctx, 0);
    return REDISMODULE_OK;
  }

  auto tags = tidx->AllTags();
  RedisModule_ReplyWithArray(ctx, static_cast<long>(tags.size()));
  for (auto& t : tags) {
    RedisModule_ReplyWithCString(ctx, t.c_str());
  }
  return REDISMODULE_OK;
}

