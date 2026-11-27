#include "cluster_coordinator.h"
#include "cluster_topology.h"
#include "search_internal.h"
#include "expr_engine.h"
#include "search_rdb.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

bool ParseCreateSpec(const std::vector<std::string>& args, IndexSpec& out_spec,
                     std::string& err_out) {
  if (args.size() < 4) {
    err_out = "ERR wrong number of arguments for 'FT.CREATE' command";
    return false;
  }

  size_t argc = args.size();
  const std::string& index_name = args[0];

  size_t i = 1;
  std::vector<std::string> prefixes;
  IndexSourceType source_type = IndexSourceType::kHash;

  if (i < argc && MatchArg(args[i], "ON")) {
    i++;
    if (i >= argc) {
      err_out = "ERR ON requires HASH or JSON";
      return false;
    }
    if (MatchArg(args[i], "HASH")) {
      source_type = IndexSourceType::kHash;
    } else if (MatchArg(args[i], "JSON")) {
      source_type = IndexSourceType::kJson;
    } else {
      err_out = "ERR ON requires HASH or JSON";
      return false;
    }
    i++;
    if (i >= argc || !MatchArg(args[i], "PREFIX")) {
      err_out = "ERR expected PREFIX after ON HASH/JSON";
      return false;
    }
    i++;
    if (i >= argc) {
      err_out = "ERR PREFIX requires a count";
      return false;
    }
    char* endptr = nullptr;
    long prefix_count = std::strtol(args[i].c_str(), &endptr, 10);
    i++;
    if (*endptr != '\0' || prefix_count <= 0) {
      err_out = "ERR PREFIX count must be a positive integer";
      return false;
    }
    for (long p = 0; p < prefix_count; p++) {
      if (i >= argc) {
        err_out = "ERR not enough PREFIX values";
        return false;
      }
      prefixes.push_back(args[i]);
      i++;
    }
  }

  std::string index_language = "english";
  std::string index_language_field;
  std::string index_custom_dict;
  std::vector<std::string> custom_stopwords;
  bool has_custom_stopwords = false;
  bool idx_nofreqs = false;
  bool idx_nooffsets = false;
  bool idx_nohl = false;
  int idx_temporary = 0;
  std::string idx_filter_expr;
  bool idx_skip_initial_scan = false;

  while (i < argc && !MatchArg(args[i], "SCHEMA")) {
    std::string_view opt = args[i];
    if (MatchArg(opt, "LANGUAGE")) {
      i++;
      if (i >= argc) { err_out = "ERR LANGUAGE requires a value"; return false; }
      index_language = args[i];
      i++;
    } else if (MatchArg(opt, "LANGUAGE_FIELD")) {
      i++;
      if (i >= argc) { err_out = "ERR LANGUAGE_FIELD requires a field name"; return false; }
      index_language_field = args[i];
      i++;
    } else if (MatchArg(opt, "DICT")) {
      i++;
      if (i >= argc) { err_out = "ERR DICT requires a dictionary name"; return false; }
      index_custom_dict = args[i];
      i++;
    } else if (MatchArg(opt, "STOPWORDS")) {
      i++;
      if (i >= argc) { err_out = "ERR STOPWORDS requires count"; return false; }
      char* ep = nullptr;
      long sw_count = std::strtol(args[i].c_str(), &ep, 10);
      i++;
      if (*ep != '\0' || sw_count < 0) {
        err_out = "ERR STOPWORDS count must be non-negative";
        return false;
      }
      has_custom_stopwords = true;
      for (long sw = 0; sw < sw_count; sw++) {
        if (i >= argc) { err_out = "ERR not enough STOPWORDS"; return false; }
        custom_stopwords.push_back(args[i]);
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
      if (i >= argc) { err_out = "ERR TEMPORARY requires seconds"; return false; }
      char* ep = nullptr;
      idx_temporary = static_cast<int>(std::strtol(args[i].c_str(), &ep, 10));
      if (*ep != '\0' || idx_temporary < 0) {
        err_out = "ERR TEMPORARY must be non-negative";
        return false;
      }
      i++;
    } else if (MatchArg(opt, "SKIPINITIALSCAN")) {
      idx_skip_initial_scan = true;
      i++;
    } else if (MatchArg(opt, "FILTER")) {
      i++;
      if (i >= argc) { err_out = "ERR FILTER requires an expression"; return false; }
      idx_filter_expr = args[i];
      ExprNode test_expr;
      std::string expr_err;
      if (!ParseExpr(idx_filter_expr, test_expr, expr_err)) {
        err_out = expr_err;
        return false;
      }
      i++;
    } else if (MatchArg(opt, "MAXTEXTFIELDS") || MatchArg(opt, "NOFIELDS")) {
      i++;
    } else if (MatchArg(opt, "SCORE")) {
      i++;
      if (i >= argc) { err_out = "ERR SCORE requires a value"; return false; }
      char* ep = nullptr;
      double sc = std::strtod(args[i].c_str(), &ep);
      if (*ep != '\0' || !std::isfinite(sc) || sc < 0.0) {
        err_out = "ERR SCORE must be a non-negative finite number";
        return false;
      }
      i++;
    } else if (MatchArg(opt, "SCORE_FIELD") || MatchArg(opt, "PAYLOAD_FIELD")) {
      i++;
      if (i >= argc) { err_out = "ERR option requires a field name"; return false; }
      i++;
    } else {
      break;
    }
  }

  if (i >= argc || !MatchArg(args[i], "SCHEMA")) {
    err_out = "ERR syntax error, expected SCHEMA keyword";
    return false;
  }
  i++;

  std::vector<FieldSpec> fields;
  while (i < argc) {
    if (i + 1 >= argc) {
      err_out = "ERR syntax error, missing field type";
      return false;
    }

    std::string_view fname = args[i];
    i++;

    FieldSpec fspec;
    fspec.name = std::string(fname);

    // JSON schema: $.path AS alias TYPE
    if (source_type == IndexSourceType::kJson && !fname.empty() && fname[0] == '$') {
      fspec.json_path = std::string(fname);
      if (i + 2 >= argc || !MatchArg(args[i], "AS")) {
        err_out = "ERR JSON field requires AS alias";
        return false;
      }
      i++;
      fspec.name = args[i];
      i++;
    }

    if (i >= argc) {
      err_out = "ERR syntax error, missing field type";
      return false;
    }
    std::string_view ftype_str = args[i];
    i++;

    if (MatchArg(ftype_str, "TAG")) {
      fspec.type = FieldType::kTag;
    } else if (MatchArg(ftype_str, "NUMERIC")) {
      fspec.type = FieldType::kNumeric;
    } else if (MatchArg(ftype_str, "VECTOR")) {
      fspec.type = FieldType::kVector;

      if (i >= argc) {
        err_out = "ERR VECTOR field requires algorithm (FLAT)";
        return false;
      }
      std::string_view algo_str = args[i];
      i++;
      if (MatchArg(algo_str, "FLAT")) {
        fspec.vector_params.algorithm = VectorAlgorithm::kFlat;
      } else if (MatchArg(algo_str, "HNSW")) {
        fspec.vector_params.algorithm = VectorAlgorithm::kHnsw;
      } else {
        err_out = "ERR unknown VECTOR algorithm, expected FLAT or HNSW";
        return false;
      }

      size_t param_end = argc;
      if (i < argc) {
        std::string_view maybe_cnt = args[i];
        if (!maybe_cnt.empty() &&
            std::isdigit(static_cast<unsigned char>(maybe_cnt[0]))) {
          char* ep = nullptr;
          long attr_cnt = std::strtol(args[i].c_str(), &ep, 10);
          if (*ep == '\0' && attr_cnt >= 0) {
            i++;
            if (i + static_cast<size_t>(attr_cnt) > argc) {
              err_out = "ERR VECTOR attribute count exceeds arguments";
              return false;
            }
            param_end = i + static_cast<size_t>(attr_cnt);
          }
        }
      }

      bool has_dim = false, has_metric = false;
      while (i < param_end) {
        std::string_view param = args[i];
        if (MatchArg(param, "TYPE")) {
          if (i + 1 >= argc) { err_out = "ERR TYPE requires a value"; return false; }
          std::string_view dtype_str = args[i + 1];
          if (MatchArg(dtype_str, "FLOAT32")) {
            fspec.vector_params.data_type = VectorDataType::kFloat32;
          } else if (MatchArg(dtype_str, "FLOAT64")) {
            fspec.vector_params.data_type = VectorDataType::kFloat64;
          } else if (MatchArg(dtype_str, "FLOAT16")) {
            fspec.vector_params.data_type = VectorDataType::kFloat16;
          } else if (MatchArg(dtype_str, "BFLOAT16")) {
            fspec.vector_params.data_type = VectorDataType::kBFloat16;
          } else {
            err_out = "ERR unknown VECTOR TYPE, expected FLOAT32, FLOAT64, FLOAT16, or BFLOAT16";
            return false;
          }
          i += 2;
        } else if (MatchArg(param, "QUANTIZATION")) {
          if (i + 1 >= argc) { err_out = "ERR QUANTIZATION requires a value"; return false; }
          std::string_view qstr = args[i + 1];
          if (MatchArg(qstr, "NONE")) {
            fspec.vector_params.quantization = VectorQuantization::kNone;
          } else if (MatchArg(qstr, "SQ8")) {
            fspec.vector_params.quantization = VectorQuantization::kSq8;
          } else if (MatchArg(qstr, "BQ")) {
            fspec.vector_params.quantization = VectorQuantization::kBq;
          } else {
            err_out = "ERR unknown QUANTIZATION, expected NONE, SQ8, or BQ";
            return false;
          }
          i += 2;
        } else if (MatchArg(param, "RERANK")) {
          if (i + 1 >= argc) { err_out = "ERR RERANK requires a value"; return false; }
          std::string_view rstr = args[i + 1];
          if (MatchArg(rstr, "NONE")) {
            fspec.vector_params.rerank = VectorRerankType::kNone;
          } else if (MatchArg(rstr, "FP16")) {
            fspec.vector_params.rerank = VectorRerankType::kFp16;
          } else if (MatchArg(rstr, "SQ8")) {
            fspec.vector_params.rerank = VectorRerankType::kSq8;
          } else {
            err_out = "ERR unknown RERANK, expected NONE, FP16, or SQ8";
            return false;
          }
          i += 2;
        } else if (MatchArg(param, "OVERSAMPLE")) {
          if (i + 1 >= argc) { err_out = "ERR OVERSAMPLE requires a value"; return false; }
          char* endptr = nullptr;
          long os_val = std::strtol(args[i + 1].c_str(), &endptr, 10);
          if (*endptr != '\0' || os_val <= 0 || os_val > 1024) {
            err_out = "ERR OVERSAMPLE must be a positive integer (<= 1024)";
            return false;
          }
          fspec.vector_params.bq_oversample = static_cast<size_t>(os_val);
          i += 2;
        } else if (MatchArg(param, "DIM")) {
          if (i + 1 >= argc) { err_out = "ERR DIM requires a value"; return false; }
          char* endptr = nullptr;
          long dim_val = std::strtol(args[i + 1].c_str(), &endptr, 10);
          if (*endptr != '\0' || dim_val <= 0 || dim_val > 65536) {
            err_out = "ERR DIM must be a positive integer (<= 65536)";
            return false;
          }
          fspec.vector_params.dim = static_cast<size_t>(dim_val);
          has_dim = true;
          i += 2;
        } else if (MatchArg(param, "DISTANCE_METRIC")) {
          if (i + 1 >= argc) { err_out = "ERR DISTANCE_METRIC requires a value"; return false; }
          std::string_view metric_str = args[i + 1];
          if (MatchArg(metric_str, "L2")) {
            fspec.vector_params.metric = DistanceMetric::kL2;
          } else if (MatchArg(metric_str, "COSINE")) {
            fspec.vector_params.metric = DistanceMetric::kCosine;
          } else if (MatchArg(metric_str, "IP")) {
            fspec.vector_params.metric = DistanceMetric::kIP;
          } else {
            err_out = "ERR unknown DISTANCE_METRIC, expected L2, COSINE, or IP";
            return false;
          }
          has_metric = true;
          i += 2;
        } else if (MatchArg(param, "M")) {
          if (i + 1 >= argc) { err_out = "ERR M requires a value"; return false; }
          char* endptr = nullptr;
          long m_val = std::strtol(args[i + 1].c_str(), &endptr, 10);
          if (*endptr != '\0' || m_val <= 0 || m_val > 1024) {
            err_out = "ERR M must be a positive integer (<= 1024)";
            return false;
          }
          fspec.vector_params.m = static_cast<size_t>(m_val);
          i += 2;
        } else if (MatchArg(param, "EF_CONSTRUCTION")) {
          if (i + 1 >= argc) { err_out = "ERR EF_CONSTRUCTION requires a value"; return false; }
          char* endptr = nullptr;
          long ef_val = std::strtol(args[i + 1].c_str(), &endptr, 10);
          if (*endptr != '\0' || ef_val <= 0 || ef_val > 100000) {
            err_out = "ERR EF_CONSTRUCTION must be a positive integer (<= 100000)";
            return false;
          }
          fspec.vector_params.ef_construction = static_cast<size_t>(ef_val);
          i += 2;
        } else {
          break;
        }
      }

      if (!has_dim) {
        err_out = "ERR VECTOR field requires DIM parameter";
        return false;
      }
      if (!has_metric) {
        err_out = "ERR VECTOR field requires DISTANCE_METRIC parameter";
        return false;
      }
      if (fspec.vector_params.rerank != VectorRerankType::kNone &&
          fspec.vector_params.quantization != VectorQuantization::kBq) {
        err_out = "ERR RERANK requires QUANTIZATION BQ";
        return false;
      }
    } else if (MatchArg(ftype_str, "TEXT")) {
      fspec.type = FieldType::kText;
    } else if (MatchArg(ftype_str, "GEO")) {
      fspec.type = FieldType::kGeo;
    } else if (MatchArg(ftype_str, "GEOSHAPE")) {
      fspec.type = FieldType::kGeoShape;
      if (i < argc) {
        std::string_view cs = args[i];
        if (MatchArg(cs, "FLAT")) {
          fspec.geoshape_coord = GeoShapeCoord::kFlat;
          i++;
        } else if (MatchArg(cs, "SPHERICAL")) {
          fspec.geoshape_coord = GeoShapeCoord::kSpherical;
          i++;
        }
      }
    } else {
      err_out = "ERR unknown field type, expected TAG, NUMERIC, TEXT, VECTOR, GEO, or GEOSHAPE";
      return false;
    }

    while (i < argc) {
      std::string_view attr = args[i];
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
        if (i >= argc) { err_out = "ERR WEIGHT requires a value"; return false; }
        char* ep = nullptr;
        fspec.weight = std::strtod(args[i].c_str(), &ep);
        if (*ep != '\0' || !std::isfinite(fspec.weight) || fspec.weight < 0) {
          err_out = "ERR WEIGHT must be a non-negative finite number";
          return false;
        }
        i++;
      } else if (MatchArg(attr, "PHONETIC") && fspec.type == FieldType::kText) {
        i++;
        if (i >= argc) { err_out = "ERR PHONETIC requires a matcher"; return false; }
        fspec.phonetic = args[i];
        i++;
      } else if (MatchArg(attr, "SEPARATOR") && fspec.type == FieldType::kTag) {
        i++;
        if (i >= argc) { err_out = "ERR SEPARATOR requires a single character"; return false; }
        if (args[i].size() != 1) {
          err_out = "ERR SEPARATOR must be a single character";
          return false;
        }
        fspec.tag_separator = args[i][0];
        i++;
      } else if (MatchArg(attr, "CASESENSITIVE") && fspec.type == FieldType::kTag) {
        fspec.case_sensitive = true;
        i++;
      } else if (MatchArg(attr, "WITHSUFFIXTRIE") &&
                 (fspec.type == FieldType::kText || fspec.type == FieldType::kTag)) {
        i++;
      } else if (MatchArg(attr, "INDEXMISSING")) {
        fspec.index_missing = true;
        i++;
      } else if (MatchArg(attr, "INDEXEMPTY")) {
        fspec.index_empty = true;
        i++;
      } else {
        break;
      }
    }

    for (auto& existing : fields) {
      if (existing.name == fspec.name) {
        err_out = "ERR duplicate field name in schema";
        return false;
      }
    }

    fields.push_back(std::move(fspec));
  }

  if (fields.empty()) {
    err_out = "ERR schema requires at least one field";
    return false;
  }

  out_spec = IndexSpec{index_name, std::move(fields), std::move(prefixes),
                       index_language, source_type,
                       std::move(custom_stopwords), has_custom_stopwords,
                       idx_nofreqs, idx_nooffsets, idx_nohl, idx_temporary,
                       std::move(idx_filter_expr), idx_skip_initial_scan,
                       std::move(index_language_field),
                       std::move(index_custom_dict)};
  return true;
}

bool ApplyLocalCreate(RedisModuleCtx* ctx, const std::vector<std::string>& args,
                      bool is_cluster_mode, std::string& err_out) {
  IndexSpec spec;
  if (!ParseCreateSpec(args, spec, err_out)) return false;

  std::string idx_str = spec.name;
  std::shared_ptr<IndexEntry> entry_ptr;
  {
    std::unique_lock<std::shared_mutex> lk(GetIndicesMutex());
    auto [it, inserted] = GetIndices().try_emplace(idx_str, std::make_shared<IndexEntry>());
    if (!inserted) {
      err_out = "ERR index already exists";
      return false;
    }
    entry_ptr = it->second;
    entry_ptr->spec = std::move(spec);
    TouchIndex(*entry_ptr);
  }

  // Standalone mode persists per-index anchor key; Cluster mode persists via aux_save2 (encver 2)
  // to avoid Hash Slot ownership conflicts and accidental MIGRATE relocation.
  if (SearchModuleType && !is_cluster_mode) {
    RedisModuleString* key_rms = RedisModule_CreateString(ctx, idx_str.data(), idx_str.size());
    auto* key = static_cast<RedisModuleKey*>(RedisModule_OpenKey(
        ctx, key_rms, REDISMODULE_READ | REDISMODULE_WRITE));
    if (key) {
      RedisModule_ModuleTypeSetValue(key, SearchModuleType, new std::string(idx_str));
      RedisModule_CloseKey(key);
    }
    RedisModule_FreeString(ctx, key_rms);
  }

  if (entry_ptr->spec.HasPrefixes() && !entry_ptr->spec.skip_initial_scan) {
    StartOrRunInitialScan(ctx, *entry_ptr);
  }
  return true;
}

bool ApplyLocalDropIndex(RedisModuleCtx* ctx, const std::vector<std::string>& args,
                         bool is_cluster_mode, bool idempotent_missing, std::string& err_out) {
  if (args.empty() || args.size() > 2) {
    err_out = "ERR wrong number of arguments for 'FT.DROPINDEX' command";
    return false;
  }
  bool delete_docs = false;
  if (args.size() == 2) {
    if (!MatchArg(args[1], "DD")) {
      err_out = "ERR unknown option, expected DD";
      return false;
    }
    delete_docs = true;
  }

  std::string idx_str = ResolveAlias(args[0]);
  std::vector<std::string> doc_ids;
  {
    std::unique_lock<std::shared_mutex> lk(GetIndicesMutex());
    auto it = GetIndices().find(idx_str);
    if (it == GetIndices().end()) {
      if (idempotent_missing) return true;
      err_out = "ERR index not found";
      return false;
    }
    if (delete_docs) {
      doc_ids = it->second->doc_store.AllIds();
    }
    StopBackgroundIndexing(ctx, idx_str);
    GetIndices().erase(it);
    {
      std::unique_lock<std::shared_mutex> alk(GetAliasesMutex());
      for (auto ait = GetAliases().begin(); ait != GetAliases().end(); ) {
        if (ait->second == idx_str) ait = GetAliases().erase(ait);
        else ++ait;
      }
    }
  }

  if (SearchModuleType && !is_cluster_mode) {
    RedisModuleString* key_rms = RedisModule_CreateString(ctx, idx_str.data(), idx_str.size());
    auto* key = static_cast<RedisModuleKey*>(
        RedisModule_OpenKey(ctx, key_rms, REDISMODULE_WRITE));
    if (key) {
      RedisModule_DeleteKey(key);
      RedisModule_CloseKey(key);
    }
    RedisModule_FreeString(ctx, key_rms);
  }

  for (const auto& doc_id : doc_ids) {
    RedisModuleString* dk = RedisModule_CreateString(ctx, doc_id.data(), doc_id.size());
    auto* dkey = static_cast<RedisModuleKey*>(
        RedisModule_OpenKey(ctx, dk, REDISMODULE_WRITE));
    if (dkey) {
      RedisModule_DeleteKey(dkey);
      RedisModule_CloseKey(dkey);
    }
    RedisModule_FreeString(ctx, dk);
  }
  return true;
}

// FT.CREATE <index_name> [ON HASH|JSON PREFIX <count> <prefix> ...] SCHEMA <field> <type> [params...]
int FtCreateCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
  if (argc < 5) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc - 1));
  for (int i = 1; i < argc; i++) args.emplace_back(ArgView(argv[i]));

  IndexSpec spec_check;
  std::string err;
  if (!ParseCreateSpec(args, spec_check, err)) {
    return RedisModule_ReplyWithError(ctx, err.c_str());
  }

  {
    std::shared_lock<std::shared_mutex> lk(GetIndicesMutex());
    if (GetIndices().find(spec_check.name) != GetIndices().end()) {
      return RedisModule_ReplyWithError(ctx, "ERR index already exists");
    }
  }

  int ctx_flags = RedisModule_GetContextFlags ? RedisModule_GetContextFlags(ctx) : 0;
  bool is_replicated = (ctx_flags & REDISMODULE_CTX_FLAGS_REPLICATED) != 0;
  bool is_replica_or_replay = (ctx_flags & (REDISMODULE_CTX_FLAGS_REPLICATED |
                                            REDISMODULE_CTX_FLAGS_SLAVE |
                                            REDISMODULE_CTX_FLAGS_LOADING)) != 0;
  auto& mgr = gemini_search::ClusterManager::Instance();
  if (!is_replicated && mgr.IsClusterScatterEnabled()) {
    return gemini_search::ExecuteClusterDdl(
        ctx, gemini_search::RpcOpCode::kRpcDdlCreate, "FT.CREATE", args, /*reply_integer=*/false);
  }

  if (!ApplyLocalCreate(ctx, args, mgr.IsClusterMode(), err)) {
    return RedisModule_ReplyWithError(ctx, err.c_str());
  }
  RedisModule_ReplicateVerbatim(ctx);
  if (mgr.IsClusterMode() && !is_replica_or_replay) {
    SetPersistedSchemaEpoch(mgr.AdvanceSchemaEpoch());
  }
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.DROPINDEX <index_name> [DD]
int FtDropIndexCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                              int argc) {
  if (argc != 2 && argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc - 1));
  for (int i = 1; i < argc; i++) args.emplace_back(ArgView(argv[i]));

  if (args.size() == 2 && !MatchArg(args[1], "DD")) {
    return RedisModule_ReplyWithError(ctx, "ERR unknown option, expected DD");
  }

  std::string idx_str = ResolveAlias(args[0]);
  {
    std::shared_lock<std::shared_mutex> lk(GetIndicesMutex());
    if (GetIndices().find(idx_str) == GetIndices().end()) {
      return RedisModule_ReplyWithError(ctx, "ERR index not found");
    }
  }

  int ctx_flags = RedisModule_GetContextFlags ? RedisModule_GetContextFlags(ctx) : 0;
  bool is_replicated = (ctx_flags & REDISMODULE_CTX_FLAGS_REPLICATED) != 0;
  bool is_replica_or_replay = (ctx_flags & (REDISMODULE_CTX_FLAGS_REPLICATED |
                                            REDISMODULE_CTX_FLAGS_SLAVE |
                                            REDISMODULE_CTX_FLAGS_LOADING)) != 0;
  auto& mgr = gemini_search::ClusterManager::Instance();
  if (!is_replicated && mgr.IsClusterScatterEnabled()) {
    return gemini_search::ExecuteClusterDdl(
        ctx, gemini_search::RpcOpCode::kRpcDdlDrop, "FT.DROPINDEX", args, /*reply_integer=*/false);
  }

  std::string err;
  if (!ApplyLocalDropIndex(ctx, args, mgr.IsClusterMode(), /*idempotent_missing=*/false, err)) {
    return RedisModule_ReplyWithError(ctx, err.c_str());
  }
  RedisModule_ReplicateVerbatim(ctx);
  if (mgr.IsClusterMode() && !is_replica_or_replay) {
    SetPersistedSchemaEpoch(mgr.AdvanceSchemaEpoch());
  }
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// Shared FT.INFO reply formatter (standalone + cluster)
void ReplyWithIndexInfo(
    RedisModuleCtx* ctx, const IndexEntry& entry, uint64_t num_docs, uint64_t num_terms,
    uint64_t inverted_bytes, uint64_t vector_bytes, bool bg_indexing, double percent_indexed,
    uint64_t cursor_idle, uint64_t cursor_created, uint64_t cursor_expired,
    const std::vector<std::tuple<std::string, uint64_t, bool, double>>* cluster_shards) {
  long info_len = (entry.spec.HasPrefixes() ? 10 : 8) + 12 +
                  (!entry.spec.custom_dict.empty() ? 2 : 0) +
                  (cluster_shards != nullptr ? 2 : 0);
  RedisModule_ReplyWithArray(ctx, info_len);

  RedisModule_ReplyWithSimpleString(ctx, "index_name");
  RedisModule_ReplyWithCString(ctx, entry.spec.name.c_str());

  RedisModule_ReplyWithSimpleString(ctx, "num_docs");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(num_docs));

  RedisModule_ReplyWithSimpleString(ctx, "language");
  RedisModule_ReplyWithCString(ctx, entry.spec.language.c_str());

  if (!entry.spec.custom_dict.empty()) {
    RedisModule_ReplyWithSimpleString(ctx, "custom_dict");
    RedisModule_ReplyWithCString(ctx, entry.spec.custom_dict.c_str());
  }

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
  RedisModule_ReplyWithArray(ctx, static_cast<long>(entry.spec.fields.size()));
  for (auto& f : entry.spec.fields) {
    if (f.type == FieldType::kVector) {
      long vec_info_len = (f.vector_params.algorithm == VectorAlgorithm::kHnsw) ? 12 : 8;
      if (f.vector_params.rerank != VectorRerankType::kNone) vec_info_len += 4;
      RedisModule_ReplyWithArray(ctx, vec_info_len);
      RedisModule_ReplyWithCString(ctx, f.name.c_str());
      RedisModule_ReplyWithSimpleString(ctx, "VECTOR");
      RedisModule_ReplyWithSimpleString(ctx, "algorithm");
      RedisModule_ReplyWithSimpleString(ctx, VectorAlgorithmName(f.vector_params.algorithm));
      RedisModule_ReplyWithSimpleString(ctx, "dim");
      RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(f.vector_params.dim));
      RedisModule_ReplyWithSimpleString(ctx, "distance_metric");
      RedisModule_ReplyWithSimpleString(ctx, DistanceMetricName(f.vector_params.metric));
      if (f.vector_params.algorithm == VectorAlgorithm::kHnsw) {
        RedisModule_ReplyWithSimpleString(ctx, "m");
        RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(f.vector_params.m));
        RedisModule_ReplyWithSimpleString(ctx, "ef_construction");
        RedisModule_ReplyWithLongLong(ctx,
                                      static_cast<long long>(f.vector_params.ef_construction));
      }
      if (f.vector_params.rerank != VectorRerankType::kNone) {
        RedisModule_ReplyWithSimpleString(ctx, "rerank");
        RedisModule_ReplyWithSimpleString(ctx, VectorRerankTypeName(f.vector_params.rerank));
        RedisModule_ReplyWithSimpleString(ctx, "oversample");
        RedisModule_ReplyWithLongLong(ctx,
                                      static_cast<long long>(f.vector_params.bq_oversample));
      }
    } else {
      long attr_count = 2;
      if (f.nostem) attr_count++;
      if (f.sortable) attr_count++;
      if (f.noindex) attr_count++;
      if (f.index_missing) attr_count++;
      if (f.index_empty) attr_count++;
      if (f.weight != 1.0) attr_count += 2;
      if (!f.json_path.empty()) attr_count += 2;
      RedisModule_ReplyWithArray(ctx, attr_count);
      RedisModule_ReplyWithCString(ctx, f.name.c_str());
      RedisModule_ReplyWithSimpleString(ctx, FieldTypeName(f.type));
      if (f.nostem) RedisModule_ReplyWithSimpleString(ctx, "NOSTEM");
      if (f.sortable) RedisModule_ReplyWithSimpleString(ctx, "SORTABLE");
      if (f.noindex) RedisModule_ReplyWithSimpleString(ctx, "NOINDEX");
      if (f.index_missing) RedisModule_ReplyWithSimpleString(ctx, "INDEXMISSING");
      if (f.index_empty) RedisModule_ReplyWithSimpleString(ctx, "INDEXEMPTY");
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

  auto FormatMb = [](uint64_t bytes) -> std::string {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return std::string(buf);
  };

  RedisModule_ReplyWithSimpleString(ctx, "num_terms");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(num_terms));

  RedisModule_ReplyWithSimpleString(ctx, "inverted_sz_mb");
  RedisModule_ReplyWithCString(ctx, FormatMb(inverted_bytes).c_str());

  RedisModule_ReplyWithSimpleString(ctx, "vector_index_sz_mb");
  RedisModule_ReplyWithCString(ctx, FormatMb(vector_bytes).c_str());

  RedisModule_ReplyWithSimpleString(ctx, "cursor_stats");
  RedisModule_ReplyWithArray(ctx, 8);
  RedisModule_ReplyWithSimpleString(ctx, "global_idle");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(cursor_idle));
  RedisModule_ReplyWithSimpleString(ctx, "index_capacity");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(kMaxCursorsPerIndex));
  RedisModule_ReplyWithSimpleString(ctx, "total_created");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(cursor_created));
  RedisModule_ReplyWithSimpleString(ctx, "total_expired");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(cursor_expired));

  RedisModule_ReplyWithSimpleString(ctx, "indexing");
  RedisModule_ReplyWithLongLong(ctx, bg_indexing ? 1 : 0);

  char pct_buf[32];
  std::snprintf(pct_buf, sizeof(pct_buf), "%.2f", percent_indexed);
  RedisModule_ReplyWithSimpleString(ctx, "percent_indexed");
  RedisModule_ReplyWithCString(ctx, pct_buf);

  if (cluster_shards != nullptr) {
    RedisModule_ReplyWithSimpleString(ctx, "cluster_shards");
    RedisModule_ReplyWithArray(ctx, static_cast<long>(cluster_shards->size()));
    for (const auto& [nid, sdocs, sind, spct] : *cluster_shards) {
      RedisModule_ReplyWithArray(ctx, 8);
      RedisModule_ReplyWithSimpleString(ctx, "node_id");
      RedisModule_ReplyWithCString(ctx, nid.c_str());
      RedisModule_ReplyWithSimpleString(ctx, "num_docs");
      RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(sdocs));
      RedisModule_ReplyWithSimpleString(ctx, "indexing");
      RedisModule_ReplyWithLongLong(ctx, sind ? 1 : 0);
      char spbuf[32];
      std::snprintf(spbuf, sizeof(spbuf), "%.2f", spct);
      RedisModule_ReplyWithSimpleString(ctx, "percent_indexed");
      RedisModule_ReplyWithCString(ctx, spbuf);
    }
  }
}

gemini_search::RpcInfoStatsResponse ExecuteLocalInfoStats(
    const gemini_search::RpcInfoStatsRequest& req) {
  gemini_search::RpcInfoStatsResponse resp;
  auto entry_ptr = GetIndexEntryShared(req.index_name);
  if (!entry_ptr) {
    resp.ok = false;
    resp.error_msg = "ERR index not found";
    return resp;
  }

  const auto& entry = *entry_ptr;
  std::shared_lock<std::shared_mutex> idx_lk(entry.Mutex());

  uint64_t num_terms = 0;
  uint64_t inverted_bytes = static_cast<uint64_t>(entry.doc_store.MemoryUsage());
  uint64_t vector_bytes = 0;
  for (const auto& f : entry.spec.fields) {
    if (f.type == FieldType::kText) {
      if (const auto* tidx = entry.text_indices.Get(f.name)) {
        num_terms += static_cast<uint64_t>(tidx->NumTerms());
        inverted_bytes += static_cast<uint64_t>(tidx->NumTerms() * 64 + tidx->NumDocs() * 32);
      }
    } else if (f.type == FieldType::kVector) {
      if (const auto* vidx = entry.vector_indices.Get(f.name)) {
        if (f.vector_params.quantization == VectorQuantization::kBq) {
          size_t per_vec = BqWords(vidx->Dim()) * sizeof(uint64_t);
          if (f.vector_params.rerank == VectorRerankType::kFp16) {
            per_vec += vidx->Dim() * sizeof(uint16_t);
          } else if (f.vector_params.rerank == VectorRerankType::kSq8) {
            per_vec += vidx->Dim() * sizeof(uint8_t) + 2 * sizeof(float);
          }
          vector_bytes += static_cast<uint64_t>(vidx->Size() * per_vec);
        } else {
          size_t elem_b =
              (f.vector_params.quantization == VectorQuantization::kSq8) ? 1 : sizeof(float);
          vector_bytes += static_cast<uint64_t>(vidx->Size() * vidx->Dim() * elem_b);
        }
      }
    }
  }

  resp.ok = true;
  resp.num_docs = static_cast<uint64_t>(entry.doc_store.Size());
  resp.num_terms = num_terms;
  resp.inverted_bytes = inverted_bytes;
  resp.vector_bytes = vector_bytes;
  resp.bg_indexing = entry.bg_indexing;
  resp.bg_scanned_keys = static_cast<uint64_t>(entry.bg_scanned_keys);
  resp.bg_total_keys = static_cast<uint64_t>(entry.bg_total_keys);
  resp.percent_indexed = entry.percent_indexed;
  {
    std::lock_guard<std::mutex> clk(GetCursorsMutex());
    resp.cursor_idle = static_cast<uint64_t>(GetCursors().size());
    resp.cursor_total_created = GetTotalCursorsCreated();
    resp.cursor_total_expired = GetTotalCursorsExpired();
  }
  return resp;
}

// FT.INFO <index_name>
int FtInfoCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                         int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  {
    std::shared_lock<std::shared_mutex> lk(GetIndicesMutex());
    if (GetIndices().find(idx_str) == GetIndices().end()) {
      return RedisModule_ReplyWithError(ctx, "ERR index not found");
    }
  }

  if (gemini_search::ClusterManager::Instance().IsClusterScatterEnabled()) {
    return gemini_search::ExecuteClusterInfo(ctx, idx_str);
  }

  auto entry_ptr = GetIndexEntryShared(idx_str);
  if (!entry_ptr) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  gemini_search::RpcInfoStatsRequest req;
  req.index_name = idx_str;
  auto stats = ExecuteLocalInfoStats(req);
  if (!stats.ok) {
    return RedisModule_ReplyWithError(ctx, stats.error_msg.c_str());
  }

  std::shared_lock<std::shared_mutex> idx_lk(entry_ptr->Mutex());
  ReplyWithIndexInfo(ctx, *entry_ptr, stats.num_docs, stats.num_terms, stats.inverted_bytes,
                     stats.vector_bytes, stats.bg_indexing, stats.percent_indexed,
                     stats.cursor_idle, stats.cursor_total_created, stats.cursor_total_expired);
  return REDISMODULE_OK;
}

// FT._LIST
int FtListCommand(RedisModuleCtx* ctx, RedisModuleString** /*argv*/,
                         int argc) {
  if (argc != 1) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::vector<std::string> names;
  {
    std::shared_lock<std::shared_mutex> lk(GetIndicesMutex());
    names.reserve(GetIndices().size());
    for (auto& [k, v] : GetIndices()) {
      names.push_back(k);
    }
  }
  std::sort(names.begin(), names.end());

  RedisModule_ReplyWithArray(ctx, static_cast<long>(names.size()));
  for (auto& n : names) {
    RedisModule_ReplyWithCString(ctx, n.c_str());
  }

  return REDISMODULE_OK;
}

// FT.ADD <index> <doc_id> [PAYLOAD <payload>] FIELDS <field1> <val1> [field2 val2 ...]
int FtAddCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                        int argc) {
  if (argc < 6) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  int ctx_flags = RedisModule_GetContextFlags ? RedisModule_GetContextFlags(ctx) : 0;
  bool is_replay = (ctx_flags & (REDISMODULE_CTX_FLAGS_REPLICATED |
                                 REDISMODULE_CTX_FLAGS_LOADING)) != 0;
  if (!is_replay && gemini_search::ClusterManager::Instance().IsClusterMode()) {
    return RedisModule_ReplyWithError(
        ctx, "ERR FT.ADD/FT.DEL not supported in cluster mode, use HSET/JSON.SET");
  }

  std::string idx_str(ArgView(argv[1]));
  auto doc_id = ArgView(argv[2]);

  int arg_i = 3;
  std::string payload;
  bool has_payload = false;
  if (arg_i + 1 < argc && MatchArg(ArgView(argv[arg_i]), "PAYLOAD")) {
    payload = ArgStr(argv[arg_i + 1]);
    has_payload = true;
    arg_i += 2;
  }

  if (arg_i >= argc || !MatchArg(ArgView(argv[arg_i]), "FIELDS")) {
    return RedisModule_ReplyWithError(
        ctx, "ERR syntax error, expected FIELDS keyword");
  }
  arg_i++;

  int field_argc = argc - arg_i;
  if (field_argc < 2 || field_argc % 2 != 0) {
    return RedisModule_ReplyWithError(
        ctx, "ERR syntax error, FIELDS requires pairs of <field> <value>");
  }

  auto it = GetIndices().find(idx_str);
  if (it == GetIndices().end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  auto& entry = *it->second;

  std::unordered_map<std::string, std::string> doc_fields;
  for (int i = arg_i; i < argc; i += 2) {
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

  {
    std::shared_lock<std::shared_mutex> tbl_lk(GetIndicesMutex());
    std::unique_lock<std::shared_mutex> idx_lk(entry.Mutex());
    RemoveDocFromIndices(entry, doc_id_str);
    entry.doc_store.Add(doc_id_str, doc_fields, std::move(payload), has_payload);
    AddDocToIndices(entry, doc_id_str, doc_fields);
  }

  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.DEL <index> <doc_id>
int FtDelCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                        int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  int ctx_flags = RedisModule_GetContextFlags ? RedisModule_GetContextFlags(ctx) : 0;
  bool is_replay = (ctx_flags & (REDISMODULE_CTX_FLAGS_REPLICATED |
                                 REDISMODULE_CTX_FLAGS_LOADING)) != 0;
  if (!is_replay && gemini_search::ClusterManager::Instance().IsClusterMode()) {
    return RedisModule_ReplyWithError(
        ctx, "ERR FT.ADD/FT.DEL not supported in cluster mode, use HSET/JSON.SET");
  }

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  std::shared_lock<std::shared_mutex> tbl_lk(GetIndicesMutex());
  auto it = GetIndices().find(idx_str);
  if (it == GetIndices().end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  auto& entry = *it->second;
  std::unique_lock<std::shared_mutex> idx_lk(entry.Mutex());

  std::string doc_id_str(ArgView(argv[2]));
  if (!entry.doc_store.Contains(doc_id_str)) {
    return RedisModule_ReplyWithError(ctx, "ERR document not found");
  }

  RemoveDocFromIndices(entry, doc_id_str);
  entry.doc_store.Remove(doc_id_str);

  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}


bool ParseAlterFields(const IndexSpec& spec, const std::vector<std::string>& args,
                      std::vector<FieldSpec>& out_fields, std::string& err_out) {
  if (args.size() < 5) {
    err_out = "ERR wrong number of arguments for 'FT.ALTER' command";
    return false;
  }
  if (!MatchArg(args[1], "SCHEMA") || !MatchArg(args[2], "ADD")) {
    err_out = "ERR syntax error, expected SCHEMA ADD";
    return false;
  }

  out_fields.clear();
  size_t i = 3;
  while (i < args.size()) {
    if (i + 1 >= args.size()) {
      err_out = "ERR syntax error, missing field type";
      return false;
    }
    std::string_view fname = args[i];
    std::string_view ftype_str = args[i + 1];
    i += 2;

    FieldSpec fspec;
    fspec.name = std::string(fname);

    if (spec.FindField(fspec.name)) {
      err_out = "ERR field already exists in schema";
      return false;
    }
    for (auto& nf : out_fields) {
      if (nf.name == fspec.name) {
        err_out = "ERR duplicate field name";
        return false;
      }
    }

    if (MatchArg(ftype_str, "TAG")) {
      fspec.type = FieldType::kTag;
    } else if (MatchArg(ftype_str, "NUMERIC")) {
      fspec.type = FieldType::kNumeric;
    } else if (MatchArg(ftype_str, "TEXT")) {
      fspec.type = FieldType::kText;
      if (i < args.size() && MatchArg(args[i], "NOSTEM")) {
        fspec.nostem = true;
        i++;
      }
    } else if (MatchArg(ftype_str, "GEO")) {
      fspec.type = FieldType::kGeo;
    } else {
      err_out = "ERR unknown field type in ALTER, expected TAG, NUMERIC, TEXT, or GEO";
      return false;
    }

    out_fields.push_back(std::move(fspec));
  }

  if (out_fields.empty()) {
    err_out = "ERR SCHEMA ADD requires at least one field";
    return false;
  }
  return true;
}

bool ApplyLocalAlter(RedisModuleCtx* ctx, const std::vector<std::string>& args,
                     std::string& err_out) {
  if (args.size() < 5) {
    err_out = "ERR wrong number of arguments for 'FT.ALTER' command";
    return false;
  }
  std::string idx_str = ResolveAlias(args[0]);
  std::shared_ptr<IndexEntry> entry_ptr = GetIndexEntryShared(idx_str);
  if (!entry_ptr) {
    err_out = "ERR index not found";
    return false;
  }
  auto& entry = *entry_ptr;
  bool needs_scan = false;
  {
    std::unique_lock<std::shared_mutex> idx_lk(entry.Mutex());
    std::vector<FieldSpec> new_fields;
    if (!ParseAlterFields(entry.spec, args, new_fields, err_out)) return false;
    for (auto& nf : new_fields) {
      entry.spec.fields.push_back(std::move(nf));
    }
    needs_scan = entry.spec.HasPrefixes();
  }
  if (needs_scan) {
    ScanExistingKeys(ctx, entry);
  }
  return true;
}

bool ApplyLocalAliasAdd(const std::vector<std::string>& args, std::string& err_out) {
  if (args.size() != 2) {
    err_out = "ERR wrong number of arguments for 'FT.ALIASADD' command";
    return false;
  }
  const std::string& alias = args[0];
  const std::string& idx = args[1];
  std::shared_lock<std::shared_mutex> lk(GetIndicesMutex());
  if (GetIndices().find(idx) == GetIndices().end()) {
    err_out = "ERR index not found";
    return false;
  }
  std::unique_lock<std::shared_mutex> alk(GetAliasesMutex());
  if (GetAliases().find(alias) != GetAliases().end()) {
    err_out = "ERR alias already exists";
    return false;
  }
  GetAliases()[alias] = idx;
  return true;
}

bool ApplyLocalAliasDel(const std::vector<std::string>& args, bool idempotent_missing,
                        std::string& err_out) {
  if (args.size() != 1) {
    err_out = "ERR wrong number of arguments for 'FT.ALIASDEL' command";
    return false;
  }
  std::unique_lock<std::shared_mutex> alk(GetAliasesMutex());
  if (GetAliases().erase(args[0]) == 0) {
    if (idempotent_missing) return true;
    err_out = "ERR alias not found";
    return false;
  }
  return true;
}

bool ApplyLocalAliasUpdate(const std::vector<std::string>& args, std::string& err_out) {
  if (args.size() != 2) {
    err_out = "ERR wrong number of arguments for 'FT.ALIASUPDATE' command";
    return false;
  }
  const std::string& alias = args[0];
  const std::string& idx = args[1];
  std::shared_lock<std::shared_mutex> lk(GetIndicesMutex());
  if (GetIndices().find(idx) == GetIndices().end()) {
    err_out = "ERR index not found";
    return false;
  }
  std::unique_lock<std::shared_mutex> alk(GetAliasesMutex());
  GetAliases()[alias] = idx;
  return true;
}

bool ApplyLocalDdlCommand(RedisModuleCtx* ctx, const std::string& cmd_name,
                          const std::vector<std::string>& args, bool is_cluster_mode,
                          bool is_rollback, int64_t& out_int_reply, std::string& err_out) {
  out_int_reply = 0;
  if (MatchArg(cmd_name, "FT.CREATE")) {
    return ApplyLocalCreate(ctx, args, is_cluster_mode, err_out);
  }
  if (MatchArg(cmd_name, "FT.DROPINDEX")) {
    return ApplyLocalDropIndex(ctx, args, is_cluster_mode, is_rollback, err_out);
  }
  if (MatchArg(cmd_name, "FT.ALTER")) {
    return ApplyLocalAlter(ctx, args, err_out);
  }
  if (MatchArg(cmd_name, "FT.ALIASADD")) {
    return ApplyLocalAliasAdd(args, err_out);
  }
  if (MatchArg(cmd_name, "FT.ALIASDEL")) {
    return ApplyLocalAliasDel(args, is_rollback, err_out);
  }
  if (MatchArg(cmd_name, "FT.ALIASUPDATE")) {
    return ApplyLocalAliasUpdate(args, err_out);
  }
  if (MatchArg(cmd_name, "FT.SYNUPDATE")) {
    return ApplyLocalSynUpdate(args, err_out);
  }
  if (MatchArg(cmd_name, "FT.DICTADD")) {
    return ApplyLocalDictAdd(args, out_int_reply, err_out);
  }
  if (MatchArg(cmd_name, "FT.DICTDEL")) {
    return ApplyLocalDictDel(args, out_int_reply, err_out);
  }
  err_out = "ERR unsupported DDL command: " + cmd_name;
  return false;
}

void ReplicateDdlCommand(RedisModuleCtx* ctx, const std::string& cmd_name,
                         const std::vector<std::string>& args) {
  if (!ctx || !RedisModule_Replicate) return;
  std::vector<RedisModuleString*> rms;
  rms.reserve(args.size());
  for (const auto& a : args) {
    rms.push_back(RedisModule_CreateString(ctx, a.data(), a.size()));
  }
  RedisModule_Replicate(ctx, cmd_name.c_str(), "v", rms.data(), rms.size());
  for (auto* s : rms) RedisModule_FreeString(ctx, s);
}

// FT.ALTER <index> SCHEMA ADD <field> <type> [params...] [<field> <type> ...]
int FtAlterCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                          int argc) {
  if (argc < 6) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc - 1));
  for (int i = 1; i < argc; i++) args.emplace_back(ArgView(argv[i]));

  std::string idx_str = ResolveAlias(args[0]);
  std::string err;
  {
    auto entry_ptr = GetIndexEntryShared(idx_str);
    if (!entry_ptr) {
      return RedisModule_ReplyWithError(ctx, "ERR index not found");
    }
    std::shared_lock<std::shared_mutex> idx_lk(entry_ptr->Mutex());
    std::vector<FieldSpec> check_fields;
    if (!ParseAlterFields(entry_ptr->spec, args, check_fields, err)) {
      return RedisModule_ReplyWithError(ctx, err.c_str());
    }
  }

  int ctx_flags = RedisModule_GetContextFlags ? RedisModule_GetContextFlags(ctx) : 0;
  bool is_replicated = (ctx_flags & REDISMODULE_CTX_FLAGS_REPLICATED) != 0;
  bool is_replica_or_replay = (ctx_flags & (REDISMODULE_CTX_FLAGS_REPLICATED |
                                            REDISMODULE_CTX_FLAGS_SLAVE |
                                            REDISMODULE_CTX_FLAGS_LOADING)) != 0;
  auto& mgr = gemini_search::ClusterManager::Instance();
  if (!is_replicated && mgr.IsClusterScatterEnabled()) {
    return gemini_search::ExecuteClusterDdl(
        ctx, gemini_search::RpcOpCode::kRpcDdlAlter, "FT.ALTER", args, /*reply_integer=*/false);
  }

  if (!ApplyLocalAlter(ctx, args, err)) {
    return RedisModule_ReplyWithError(ctx, err.c_str());
  }
  RedisModule_ReplicateVerbatim(ctx);
  if (mgr.IsClusterMode() && !is_replica_or_replay) {
    SetPersistedSchemaEpoch(mgr.AdvanceSchemaEpoch());
  }
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.ALIASADD <alias> <index>
int FtAliasAddCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                             int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::vector<std::string> args = {ArgStr(argv[1]), ArgStr(argv[2])};
  {
    std::shared_lock<std::shared_mutex> lk(GetIndicesMutex());
    if (GetIndices().find(args[1]) == GetIndices().end()) {
      return RedisModule_ReplyWithError(ctx, "ERR index not found");
    }
  }
  {
    std::shared_lock<std::shared_mutex> alk(GetAliasesMutex());
    if (GetAliases().find(args[0]) != GetAliases().end()) {
      return RedisModule_ReplyWithError(ctx, "ERR alias already exists");
    }
  }

  int ctx_flags = RedisModule_GetContextFlags ? RedisModule_GetContextFlags(ctx) : 0;
  bool is_replicated = (ctx_flags & REDISMODULE_CTX_FLAGS_REPLICATED) != 0;
  bool is_replica_or_replay = (ctx_flags & (REDISMODULE_CTX_FLAGS_REPLICATED |
                                            REDISMODULE_CTX_FLAGS_SLAVE |
                                            REDISMODULE_CTX_FLAGS_LOADING)) != 0;
  auto& mgr = gemini_search::ClusterManager::Instance();
  if (!is_replicated && mgr.IsClusterScatterEnabled()) {
    return gemini_search::ExecuteClusterDdl(
        ctx, gemini_search::RpcOpCode::kRpcDdlAlias, "FT.ALIASADD", args, /*reply_integer=*/false);
  }

  std::string err;
  if (!ApplyLocalAliasAdd(args, err)) {
    return RedisModule_ReplyWithError(ctx, err.c_str());
  }
  RedisModule_ReplicateVerbatim(ctx);
  if (mgr.IsClusterMode() && !is_replica_or_replay) {
    SetPersistedSchemaEpoch(mgr.AdvanceSchemaEpoch());
  }
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.ALIASDEL <alias>
int FtAliasDelCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                             int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::vector<std::string> args = {ArgStr(argv[1])};
  {
    std::shared_lock<std::shared_mutex> alk(GetAliasesMutex());
    if (GetAliases().find(args[0]) == GetAliases().end()) {
      return RedisModule_ReplyWithError(ctx, "ERR alias not found");
    }
  }

  int ctx_flags = RedisModule_GetContextFlags ? RedisModule_GetContextFlags(ctx) : 0;
  bool is_replicated = (ctx_flags & REDISMODULE_CTX_FLAGS_REPLICATED) != 0;
  bool is_replica_or_replay = (ctx_flags & (REDISMODULE_CTX_FLAGS_REPLICATED |
                                            REDISMODULE_CTX_FLAGS_SLAVE |
                                            REDISMODULE_CTX_FLAGS_LOADING)) != 0;
  auto& mgr = gemini_search::ClusterManager::Instance();
  if (!is_replicated && mgr.IsClusterScatterEnabled()) {
    return gemini_search::ExecuteClusterDdl(
        ctx, gemini_search::RpcOpCode::kRpcDdlAlias, "FT.ALIASDEL", args, /*reply_integer=*/false);
  }

  std::string err;
  if (!ApplyLocalAliasDel(args, /*idempotent_missing=*/false, err)) {
    return RedisModule_ReplyWithError(ctx, err.c_str());
  }
  RedisModule_ReplicateVerbatim(ctx);
  if (mgr.IsClusterMode() && !is_replica_or_replay) {
    SetPersistedSchemaEpoch(mgr.AdvanceSchemaEpoch());
  }
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.ALIASUPDATE <alias> <index>
int FtAliasUpdateCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                                int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::vector<std::string> args = {ArgStr(argv[1]), ArgStr(argv[2])};
  {
    std::shared_lock<std::shared_mutex> lk(GetIndicesMutex());
    if (GetIndices().find(args[1]) == GetIndices().end()) {
      return RedisModule_ReplyWithError(ctx, "ERR index not found");
    }
  }

  int ctx_flags = RedisModule_GetContextFlags ? RedisModule_GetContextFlags(ctx) : 0;
  bool is_replicated = (ctx_flags & REDISMODULE_CTX_FLAGS_REPLICATED) != 0;
  bool is_replica_or_replay = (ctx_flags & (REDISMODULE_CTX_FLAGS_REPLICATED |
                                            REDISMODULE_CTX_FLAGS_SLAVE |
                                            REDISMODULE_CTX_FLAGS_LOADING)) != 0;
  auto& mgr = gemini_search::ClusterManager::Instance();
  if (!is_replicated && mgr.IsClusterScatterEnabled()) {
    return gemini_search::ExecuteClusterDdl(
        ctx, gemini_search::RpcOpCode::kRpcDdlAlias, "FT.ALIASUPDATE", args,
        /*reply_integer=*/false);
  }

  std::string err;
  if (!ApplyLocalAliasUpdate(args, err)) {
    return RedisModule_ReplyWithError(ctx, err.c_str());
  }
  RedisModule_ReplicateVerbatim(ctx);
  if (mgr.IsClusterMode() && !is_replica_or_replay) {
    SetPersistedSchemaEpoch(mgr.AdvanceSchemaEpoch());
  }
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

gemini_search::RpcTagValsResponse ExecuteLocalTagVals(
    const gemini_search::RpcTagValsRequest& req) {
  gemini_search::RpcTagValsResponse resp;
  auto entry_ptr = GetIndexEntryShared(req.index_name);
  if (!entry_ptr) {
    resp.ok = false;
    resp.error_msg = "ERR index not found";
    return resp;
  }

  const auto& entry = *entry_ptr;
  std::shared_lock<std::shared_mutex> idx_lk(entry.Mutex());
  const auto* fspec = entry.spec.FindField(req.field_name);
  if (!fspec) {
    resp.ok = false;
    resp.error_msg = "ERR field not in schema";
    return resp;
  }
  if (fspec->type != FieldType::kTag) {
    resp.ok = false;
    resp.error_msg = "ERR field is not a TAG field";
    return resp;
  }

  resp.ok = true;
  if (const auto* tidx = entry.tag_indices.Get(req.field_name)) {
    resp.tags = tidx->AllTags();
  }
  return resp;
}

// FT.TAGVALS <index> <field>
int FtTagvalsCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                            int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  std::string field_name(ArgView(argv[2]));

  auto entry_ptr = GetIndexEntryShared(idx_str);
  if (!entry_ptr) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  {
    std::shared_lock<std::shared_mutex> idx_lk(entry_ptr->Mutex());
    const auto* fspec = entry_ptr->spec.FindField(field_name);
    if (!fspec) {
      return RedisModule_ReplyWithError(ctx, "ERR field not in schema");
    }
    if (fspec->type != FieldType::kTag) {
      return RedisModule_ReplyWithError(ctx, "ERR field is not a TAG field");
    }
  }

  if (gemini_search::ClusterManager::Instance().IsClusterScatterEnabled()) {
    return gemini_search::ExecuteClusterTagVals(ctx, idx_str, field_name);
  }

  std::shared_lock<std::shared_mutex> idx_lk(entry_ptr->Mutex());
  const auto* tidx = entry_ptr->tag_indices.Get(field_name);
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

