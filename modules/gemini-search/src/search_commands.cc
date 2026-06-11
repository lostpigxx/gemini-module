#include "search_commands.h"
#include "query_parser.h"
#include "search_rdb.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

static std::unordered_map<std::string, IndexEntry> g_indices;

IndexEntry* GetIndexEntry(const std::string& name) {
  auto it = g_indices.find(name);
  if (it == g_indices.end()) return nullptr;
  return &it->second;
}

bool CreateIndexEntry(const std::string& name, IndexEntry entry) {
  auto [it, inserted] = g_indices.try_emplace(name, std::move(entry));
  return inserted;
}

void EraseIndexEntry(const std::string& name) { g_indices.erase(name); }

// Forward declaration of static helper defined below
static void AddDocToIndices(
    IndexEntry& entry, const std::string& doc_id,
    const std::unordered_map<std::string, std::string>& fields);

void CreateIndexFromRdb(
    const std::string& name, IndexSpec spec,
    const std::vector<
        std::pair<std::string, std::unordered_map<std::string, std::string>>>&
        docs) {
  IndexEntry entry;
  entry.spec = std::move(spec);
  for (auto& [doc_id, fields] : docs) {
    entry.doc_store.Add(doc_id, fields);
    AddDocToIndices(entry, doc_id, fields);
  }
  g_indices[name] = std::move(entry);
}

static bool TryParseDouble(const std::string& s, double& out) {
  char* endptr = nullptr;
  out = std::strtod(s.c_str(), &endptr);
  return endptr != s.c_str() && *endptr == '\0' && !std::isnan(out) &&
         !std::isinf(out);
}

static std::string_view ArgView(RedisModuleString* s) {
  size_t len;
  const char* data = RedisModule_StringPtrLen(s, &len);
  return {data, len};
}

static std::string ArgStr(RedisModuleString* s) {
  size_t len;
  const char* data = RedisModule_StringPtrLen(s, &len);
  return {data, len};
}

static bool MatchArg(std::string_view arg, const char* target) {
  if (arg.size() != strlen(target)) return false;
  return strncasecmp(arg.data(), target, arg.size()) == 0;
}

static void RemoveDocFromIndices(IndexEntry& entry,
                                 const std::string& doc_id) {
  const auto* doc = entry.doc_store.Get(doc_id);
  if (!doc) return;
  for (auto& [fname, fval] : doc->fields) {
    const auto* fspec = entry.spec.FindField(fname);
    if (!fspec) continue;
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
    }
  }
}

static void AddDocToIndices(
    IndexEntry& entry, const std::string& doc_id,
    const std::unordered_map<std::string, std::string>& fields) {
  for (auto& [fname, fval] : fields) {
    const auto* fspec = entry.spec.FindField(fname);
    if (!fspec) continue;
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
      entry.text_indices.GetOrCreate(fname).Add(doc_id, fval);
    }
  }
}

static void ScanExistingKeys(RedisModuleCtx* ctx, IndexEntry& entry);
static void IndexHashKey(RedisModuleCtx* ctx, IndexEntry& entry,
                         const std::string& key_name);

// FT.CREATE <index_name> [ON HASH PREFIX <count> <prefix> ...] SCHEMA <field> <type> [params...]
static int FtCreateCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
  if (argc < 5) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto index_name = ArgView(argv[1]);

  int i = 2;
  std::vector<std::string> prefixes;

  if (i < argc && MatchArg(ArgView(argv[i]), "ON")) {
    i++;
    if (i >= argc || !MatchArg(ArgView(argv[i]), "HASH")) {
      return RedisModule_ReplyWithError(ctx, "ERR only HASH is supported for ON");
    }
    i++;
    if (i >= argc || !MatchArg(ArgView(argv[i]), "PREFIX")) {
      return RedisModule_ReplyWithError(ctx, "ERR expected PREFIX after ON HASH");
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
  if (i < argc && MatchArg(ArgView(argv[i]), "LANGUAGE")) {
    i++;
    if (i >= argc) {
      return RedisModule_ReplyWithError(ctx, "ERR LANGUAGE requires a value");
    }
    index_language = std::string(ArgView(argv[i]));
    i++;
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
    auto ftype_str = ArgView(argv[i + 1]);
    i += 2;

    FieldSpec fspec;
    fspec.name = std::string(fname);

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
        if (MatchArg(param, "DIM")) {
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
      if (i < argc && MatchArg(ArgView(argv[i]), "NOSTEM")) {
        fspec.nostem = true;
        i++;
      }
    } else {
      return RedisModule_ReplyWithError(
          ctx, "ERR unknown field type, expected TAG, NUMERIC, TEXT, or VECTOR");
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
  auto [it, inserted] = g_indices.try_emplace(idx_str);
  if (!inserted) {
    return RedisModule_ReplyWithError(ctx, "ERR index already exists");
  }
  it->second.spec = IndexSpec{idx_str, std::move(fields), std::move(prefixes), index_language};

  if (SearchModuleType) {
    auto* key = static_cast<RedisModuleKey*>(RedisModule_OpenKey(
        ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
    RedisModule_ModuleTypeSetValue(key, SearchModuleType,
                                   new std::string(idx_str));
  }

  if (it->second.spec.HasPrefixes()) {
    ScanExistingKeys(ctx, it->second);
  }

  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.DROPINDEX <index_name>
static int FtDropIndexCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                              int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str(ArgView(argv[1]));
  if (g_indices.erase(idx_str) == 0) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }

  if (SearchModuleType) {
    auto* key = static_cast<RedisModuleKey*>(RedisModule_OpenKey(
        ctx, argv[1], REDISMODULE_WRITE));
    RedisModule_DeleteKey(key);
  }

  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.INFO <index_name>
static int FtInfoCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                         int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str(ArgView(argv[1]));
  auto it = g_indices.find(idx_str);
  if (it == g_indices.end()) {
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
    RedisModule_ReplyWithSimpleString(ctx, "HASH");
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
    } else if (f.type == FieldType::kText && f.nostem) {
      RedisModule_ReplyWithArray(ctx, 3);
      RedisModule_ReplyWithCString(ctx, f.name.c_str());
      RedisModule_ReplyWithSimpleString(ctx, "TEXT");
      RedisModule_ReplyWithSimpleString(ctx, "NOSTEM");
    } else {
      RedisModule_ReplyWithArray(ctx, 2);
      RedisModule_ReplyWithCString(ctx, f.name.c_str());
      RedisModule_ReplyWithSimpleString(ctx, FieldTypeName(f.type));
    }
  }

  return REDISMODULE_OK;
}

// FT._LIST
static int FtListCommand(RedisModuleCtx* ctx, RedisModuleString** /*argv*/,
                         int argc) {
  if (argc != 1) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::vector<std::string> names;
  names.reserve(g_indices.size());
  for (auto& [k, v] : g_indices) {
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
static int FtAddCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
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

  auto it = g_indices.find(idx_str);
  if (it == g_indices.end()) {
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
      size_t expected = fspec->vector_params.dim * sizeof(float);
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
static int FtDelCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                        int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str(ArgView(argv[1]));
  auto it = g_indices.find(idx_str);
  if (it == g_indices.end()) {
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

struct SearchOptions {
  std::vector<std::string> return_fields;
  bool has_return = false;
  std::string sortby_field;
  bool sortby_desc = false;
  bool has_sortby = false;
  long long limit_offset = 0;
  long long limit_count = -1;
  bool has_limit = false;
  bool nocontent = false;
  bool withscores = false;
  bool verbatim = false;
  bool nostopwords = false;
  std::vector<std::string> infields;
  bool has_infields = false;
  std::vector<std::string> inkeys;
  bool has_inkeys = false;
  long long timeout_ms = 0;
  bool has_timeout = false;
  int slop = 0;
  bool has_slop = false;
  bool inorder = false;
  std::string language;
  bool has_language = false;
};

static void ReplyWithDocFields(RedisModuleCtx* ctx, const Document* doc,
                               const SearchOptions& opts,
                               const std::string* score_val,
                               const char* score_name = "__vec_score") {
  if (!doc) {
    RedisModule_ReplyWithEmptyArray(ctx);
    return;
  }

  std::vector<std::string> keys;
  if (opts.has_return) {
    keys = opts.return_fields;
  } else {
    keys.reserve(doc->fields.size());
    for (auto& [k, v] : doc->fields) {
      keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());
  }

  long count = static_cast<long>(keys.size()) * 2;
  if (score_val) count += 2;

  RedisModule_ReplyWithArray(ctx, count);
  if (score_val) {
    RedisModule_ReplyWithCString(ctx, score_name);
    RedisModule_ReplyWithCString(ctx, score_val->c_str());
  }
  for (auto& k : keys) {
    RedisModule_ReplyWithCString(ctx, k.c_str());
    auto fit = doc->fields.find(k);
    if (fit != doc->fields.end()) {
      RedisModule_ReplyWithCString(ctx, fit->second.c_str());
    } else {
      RedisModule_ReplyWithCString(ctx, "");
    }
  }
}

// FT.SEARCH <index> <query> [PARAMS ...] [RETURN ...] [SORTBY ...] [LIMIT ...]
static int FtSearchCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str(ArgView(argv[1]));
  auto it = g_indices.find(idx_str);
  if (it == g_indices.end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  auto& entry = it->second;

  std::string query_str(ArgView(argv[2]));

  // Parse optional blocks: PARAMS, RETURN, SORTBY, LIMIT, etc.
  std::unordered_map<std::string, std::string> params;
  SearchOptions opts;
  int arg_i = 3;

  while (arg_i < argc) {
    auto kw = ArgView(argv[arg_i]);

    if (MatchArg(kw, "PARAMS")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR PARAMS requires count");
      }
      char* endptr = nullptr;
      long param_count =
          std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || param_count < 0 || param_count % 2 != 0) {
        return RedisModule_ReplyWithError(
            ctx, "ERR PARAMS count must be a non-negative even integer");
      }
      long pairs = param_count / 2;
      for (long p = 0; p < pairs; p++) {
        if (arg_i + 1 >= argc) {
          return RedisModule_ReplyWithError(ctx,
                                            "ERR not enough PARAMS values");
        }
        std::string pname(ArgView(argv[arg_i]));
        std::string pval = ArgStr(argv[arg_i + 1]);
        arg_i += 2;
        params[std::move(pname)] = std::move(pval);
      }
    } else if (MatchArg(kw, "RETURN")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR RETURN requires count");
      }
      char* endptr = nullptr;
      long ret_count =
          std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || ret_count < 0) {
        return RedisModule_ReplyWithError(
            ctx, "ERR RETURN count must be a non-negative integer");
      }
      opts.has_return = true;
      for (long r = 0; r < ret_count; r++) {
        if (arg_i >= argc) {
          return RedisModule_ReplyWithError(ctx,
                                            "ERR not enough RETURN fields");
        }
        opts.return_fields.emplace_back(ArgView(argv[arg_i]));
        arg_i++;
      }
    } else if (MatchArg(kw, "SORTBY")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR SORTBY requires field");
      }
      opts.sortby_field = std::string(ArgView(argv[arg_i]));
      opts.has_sortby = true;
      arg_i++;
      if (arg_i < argc) {
        auto dir = ArgView(argv[arg_i]);
        if (MatchArg(dir, "ASC")) {
          opts.sortby_desc = false;
          arg_i++;
        } else if (MatchArg(dir, "DESC")) {
          opts.sortby_desc = true;
          arg_i++;
        }
      }
    } else if (MatchArg(kw, "LIMIT")) {
      arg_i++;
      if (arg_i + 1 >= argc) {
        return RedisModule_ReplyWithError(
            ctx, "ERR LIMIT requires offset and count");
      }
      char* ep1 = nullptr;
      char* ep2 = nullptr;
      std::string off_s(ArgView(argv[arg_i]));
      std::string cnt_s(ArgView(argv[arg_i + 1]));
      long long off = std::strtoll(off_s.c_str(), &ep1, 10);
      long long cnt = std::strtoll(cnt_s.c_str(), &ep2, 10);
      if (*ep1 != '\0' || *ep2 != '\0' || off < 0 || cnt < 0) {
        return RedisModule_ReplyWithError(
            ctx, "ERR LIMIT offset and count must be non-negative integers");
      }
      opts.limit_offset = off;
      opts.limit_count = cnt;
      opts.has_limit = true;
      arg_i += 2;
    } else if (MatchArg(kw, "NOCONTENT")) {
      opts.nocontent = true;
      arg_i++;
    } else if (MatchArg(kw, "WITHSCORES")) {
      opts.withscores = true;
      arg_i++;
    } else if (MatchArg(kw, "VERBATIM")) {
      opts.verbatim = true;
      arg_i++;
    } else if (MatchArg(kw, "NOSTOPWORDS")) {
      opts.nostopwords = true;
      arg_i++;
    } else if (MatchArg(kw, "INFIELDS")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR INFIELDS requires count");
      }
      char* endptr = nullptr;
      long inf_count =
          std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || inf_count < 0) {
        return RedisModule_ReplyWithError(
            ctx, "ERR INFIELDS count must be a non-negative integer");
      }
      opts.has_infields = true;
      for (long f = 0; f < inf_count; f++) {
        if (arg_i >= argc) {
          return RedisModule_ReplyWithError(ctx, "ERR not enough INFIELDS fields");
        }
        opts.infields.emplace_back(ArgView(argv[arg_i]));
        arg_i++;
      }
    } else if (MatchArg(kw, "INKEYS")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR INKEYS requires count");
      }
      char* endptr = nullptr;
      long ink_count =
          std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || ink_count < 0) {
        return RedisModule_ReplyWithError(
            ctx, "ERR INKEYS count must be a non-negative integer");
      }
      opts.has_inkeys = true;
      for (long k = 0; k < ink_count; k++) {
        if (arg_i >= argc) {
          return RedisModule_ReplyWithError(ctx, "ERR not enough INKEYS keys");
        }
        opts.inkeys.emplace_back(ArgView(argv[arg_i]));
        arg_i++;
      }
    } else if (MatchArg(kw, "SLOP")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR SLOP requires value");
      }
      char* endptr = nullptr;
      long slop_val =
          std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      if (*endptr != '\0' || slop_val < 0) {
        return RedisModule_ReplyWithError(
            ctx, "ERR SLOP must be a non-negative integer");
      }
      opts.slop = static_cast<int>(slop_val);
      opts.has_slop = true;
      arg_i++;
    } else if (MatchArg(kw, "INORDER")) {
      opts.inorder = true;
      arg_i++;
    } else if (MatchArg(kw, "LANGUAGE")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR LANGUAGE requires value");
      }
      opts.language = std::string(ArgView(argv[arg_i]));
      opts.has_language = true;
      arg_i++;
    } else if (MatchArg(kw, "TIMEOUT")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR TIMEOUT requires value");
      }
      char* endptr = nullptr;
      long long tms =
          std::strtoll(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      if (*endptr != '\0' || tms < 0) {
        return RedisModule_ReplyWithError(
            ctx, "ERR TIMEOUT must be a non-negative integer");
      }
      opts.timeout_ms = tms;
      opts.has_timeout = true;
      arg_i++;
    } else {
      return RedisModule_ReplyWithError(ctx,
                                        "ERR unknown search option");
    }
  }

  // Build query options
  QueryOptions qopts;
  qopts.nostopwords = opts.nostopwords || opts.verbatim;
  qopts.infields = opts.infields;
  qopts.slop = opts.slop;
  qopts.inorder = opts.inorder;
  qopts.stem = !opts.verbatim;
  qopts.language = opts.has_language ? opts.language : entry.spec.language;

  ParsedQuery parsed;
  std::string parse_error;
  if (!ParseQuery(query_str, parsed, parse_error, qopts)) {
    return RedisModule_ReplyWithError(ctx, parse_error.c_str());
  }

  // Record start time for TIMEOUT
  struct timespec start_time;
  if (opts.has_timeout) {
    clock_gettime(CLOCK_MONOTONIC, &start_time);
  }

  // KNN path
  if (parsed.has_knn) {
    const auto* fspec = entry.spec.FindField(parsed.knn_field);
    if (!fspec) {
      return RedisModule_ReplyWithError(ctx, "ERR KNN field not in schema");
    }
    if (fspec->type != FieldType::kVector) {
      return RedisModule_ReplyWithError(
          ctx, "ERR KNN field is not a VECTOR field");
    }

    auto pit = params.find(parsed.knn_param_name);
    if (pit == params.end()) {
      return RedisModule_ReplyWithError(
          ctx, "ERR KNN param not found in PARAMS");
    }
    auto& blob = pit->second;
    size_t expected_bytes = fspec->vector_params.dim * sizeof(float);
    if (blob.size() != expected_bytes) {
      return RedisModule_ReplyWithError(
          ctx, "ERR query vector dimension mismatch");
    }

    const auto* vidx = entry.vector_indices.Get(parsed.knn_field);
    std::vector<KnnResult> knn_results;
    if (vidx) {
      if (parsed.root.type != QueryNode::Type::kMatchAll) {
        std::string filter_error;
        auto candidates = EvaluateQuery(parsed.root, entry.spec, entry.doc_store,
                                        entry.tag_indices, entry.numeric_indices,
                                        entry.text_indices, filter_error,
                                        qopts);
        if (!filter_error.empty()) {
          return RedisModule_ReplyWithError(ctx, filter_error.c_str());
        }
        knn_results = vidx->KnnQueryFiltered(
            reinterpret_cast<const float*>(blob.data()), parsed.knn_k, candidates);
      } else {
        knn_results = vidx->KnnQuery(
            reinterpret_cast<const float*>(blob.data()), parsed.knn_k);
      }
    }

    // TIMEOUT check
    if (opts.has_timeout) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      long long elapsed_ms = (now.tv_sec - start_time.tv_sec) * 1000LL +
                             (now.tv_nsec - start_time.tv_nsec) / 1000000LL;
      if (elapsed_ms > opts.timeout_ms) {
        return RedisModule_ReplyWithError(ctx, "ERR query timed out");
      }
    }

    // INKEYS filter
    if (opts.has_inkeys) {
      std::unordered_set<std::string> allowed(opts.inkeys.begin(), opts.inkeys.end());
      knn_results.erase(
          std::remove_if(knn_results.begin(), knn_results.end(),
                         [&](const KnnResult& r) {
                           return allowed.find(r.doc_id) == allowed.end();
                         }),
          knn_results.end());
    }

    // Apply LIMIT to KNN results
    long long knn_total_before_limit = static_cast<long long>(knn_results.size());
    if (opts.has_limit) {
      if (opts.limit_offset >= knn_total_before_limit) {
        knn_results.clear();
      } else {
        auto begin = knn_results.begin() + opts.limit_offset;
        auto end = (opts.limit_count >= 0 &&
                    opts.limit_offset + opts.limit_count < knn_total_before_limit)
                       ? knn_results.begin() + opts.limit_offset + opts.limit_count
                       : knn_results.end();
        knn_results = std::vector<KnnResult>(begin, end);
      }
    }

    long total = static_cast<long>(knn_results.size());
    long multiplier = opts.nocontent ? 1 : 2;
    RedisModule_ReplyWithArray(ctx, 1 + total * multiplier);
    RedisModule_ReplyWithLongLong(ctx, total);

    for (auto& kr : knn_results) {
      RedisModule_ReplyWithCString(ctx, kr.doc_id.c_str());
      if (!opts.nocontent) {
        auto score_str = std::to_string(kr.score);
        ReplyWithDocFields(ctx, entry.doc_store.Get(kr.doc_id), opts,
                           &score_str);
      }
    }

    return REDISMODULE_OK;
  }

  // Non-KNN: evaluate query tree
  std::string eval_error;

  // Use scored evaluation when WITHSCORES is requested
  std::vector<ScoredResult> scored_results;
  std::vector<std::string> result_ids;

  if (opts.withscores) {
    scored_results = EvaluateQueryScored(
        parsed.root, entry.spec, entry.doc_store,
        entry.tag_indices, entry.numeric_indices,
        entry.text_indices, eval_error, qopts);
    if (!eval_error.empty()) {
      return RedisModule_ReplyWithError(ctx, eval_error.c_str());
    }
  } else {
    result_ids = EvaluateQuery(
        parsed.root, entry.spec, entry.doc_store,
        entry.tag_indices, entry.numeric_indices,
        entry.text_indices, eval_error, qopts);
    if (!eval_error.empty()) {
      return RedisModule_ReplyWithError(ctx, eval_error.c_str());
    }
  }

  // TIMEOUT check (coarse, post-evaluation)
  if (opts.has_timeout) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long elapsed_ms = (now.tv_sec - start_time.tv_sec) * 1000LL +
                           (now.tv_nsec - start_time.tv_nsec) / 1000000LL;
    if (elapsed_ms > opts.timeout_ms) {
      return RedisModule_ReplyWithError(ctx, "ERR query timed out");
    }
  }

  // INKEYS filter
  if (opts.has_inkeys) {
    std::unordered_set<std::string> allowed(opts.inkeys.begin(), opts.inkeys.end());
    if (opts.withscores) {
      scored_results.erase(
          std::remove_if(scored_results.begin(), scored_results.end(),
                         [&](const ScoredResult& r) {
                           return allowed.find(r.doc_id) == allowed.end();
                         }),
          scored_results.end());
    } else {
      result_ids.erase(
          std::remove_if(result_ids.begin(), result_ids.end(),
                         [&](const std::string& id) {
                           return allowed.find(id) == allowed.end();
                         }),
          result_ids.end());
    }
  }

  if (opts.withscores) {
    // SORTBY overrides score-based ordering; otherwise sort by score desc
    if (opts.has_sortby) {
      const auto* sort_fspec = entry.spec.FindField(opts.sortby_field);
      if (!sort_fspec) {
        return RedisModule_ReplyWithError(ctx, "ERR SORTBY field not in schema");
      }
      bool numeric_sort = (sort_fspec->type == FieldType::kNumeric);
      bool desc = opts.sortby_desc;
      std::sort(scored_results.begin(), scored_results.end(),
                [&](const ScoredResult& a, const ScoredResult& b) {
                  const auto* da = entry.doc_store.Get(a.doc_id);
                  const auto* db = entry.doc_store.Get(b.doc_id);
                  std::string va, vb;
                  if (da) {
                    auto fa = da->fields.find(opts.sortby_field);
                    if (fa != da->fields.end()) va = fa->second;
                  }
                  if (db) {
                    auto fb = db->fields.find(opts.sortby_field);
                    if (fb != db->fields.end()) vb = fb->second;
                  }
                  if (numeric_sort) {
                    double na = 0, nb = 0;
                    TryParseDouble(va, na);
                    TryParseDouble(vb, nb);
                    return desc ? na > nb : na < nb;
                  }
                  return desc ? va > vb : va < vb;
                });
    } else {
      std::sort(scored_results.begin(), scored_results.end(),
                [](const ScoredResult& a, const ScoredResult& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.doc_id < b.doc_id;
                });
    }

    // LIMIT
    if (opts.has_limit) {
      long long total_before = static_cast<long long>(scored_results.size());
      if (opts.limit_offset >= total_before) {
        scored_results.clear();
      } else {
        auto begin = scored_results.begin() + opts.limit_offset;
        auto end = (opts.limit_count >= 0 &&
                    opts.limit_offset + opts.limit_count < total_before)
                       ? scored_results.begin() + opts.limit_offset + opts.limit_count
                       : scored_results.end();
        scored_results = std::vector<ScoredResult>(begin, end);
      }
    }

    long total = static_cast<long>(scored_results.size());
    long multiplier = opts.nocontent ? 1 : 2;
    RedisModule_ReplyWithArray(ctx, 1 + total * multiplier);
    RedisModule_ReplyWithLongLong(ctx, total);

    for (auto& sr : scored_results) {
      RedisModule_ReplyWithCString(ctx, sr.doc_id.c_str());
      if (!opts.nocontent) {
        auto score_str = std::to_string(sr.score);
        ReplyWithDocFields(ctx, entry.doc_store.Get(sr.doc_id), opts,
                           &score_str, "__search_score");
      }
    }
  } else {
    // SORTBY
    if (opts.has_sortby) {
      const auto* sort_fspec = entry.spec.FindField(opts.sortby_field);
      if (!sort_fspec) {
        return RedisModule_ReplyWithError(ctx, "ERR SORTBY field not in schema");
      }
      bool numeric_sort = (sort_fspec->type == FieldType::kNumeric);
      bool desc = opts.sortby_desc;
      std::sort(result_ids.begin(), result_ids.end(),
                [&](const std::string& a, const std::string& b) {
                  const auto* da = entry.doc_store.Get(a);
                  const auto* db = entry.doc_store.Get(b);
                  std::string va, vb;
                  if (da) {
                    auto fa = da->fields.find(opts.sortby_field);
                    if (fa != da->fields.end()) va = fa->second;
                  }
                  if (db) {
                    auto fb = db->fields.find(opts.sortby_field);
                    if (fb != db->fields.end()) vb = fb->second;
                  }
                  if (numeric_sort) {
                    double na = 0, nb = 0;
                    TryParseDouble(va, na);
                    TryParseDouble(vb, nb);
                    return desc ? na > nb : na < nb;
                  }
                  return desc ? va > vb : va < vb;
                });
    }

    // LIMIT
    if (opts.has_limit) {
      long long total_before = static_cast<long long>(result_ids.size());
      if (opts.limit_offset >= total_before) {
        result_ids.clear();
      } else {
        auto begin = result_ids.begin() + opts.limit_offset;
        auto end = (opts.limit_count >= 0 &&
                    opts.limit_offset + opts.limit_count < total_before)
                       ? result_ids.begin() + opts.limit_offset + opts.limit_count
                       : result_ids.end();
        result_ids = std::vector<std::string>(begin, end);
      }
    }

    long total = static_cast<long>(result_ids.size());
    long multiplier = opts.nocontent ? 1 : 2;
    RedisModule_ReplyWithArray(ctx, 1 + total * multiplier);
    RedisModule_ReplyWithLongLong(ctx, total);

    for (auto& rid : result_ids) {
      RedisModule_ReplyWithCString(ctx, rid.c_str());
      if (!opts.nocontent) {
        ReplyWithDocFields(ctx, entry.doc_store.Get(rid), opts, nullptr);
      }
    }
  }

  return REDISMODULE_OK;
}

// FT._DEBUG
static int FtDebugCommand(RedisModuleCtx* ctx, RedisModuleString** /*argv*/,
                          int /*argc*/) {
  return RedisModule_ReplyWithSimpleString(ctx, "GeminiSearch OK");
}

static void IndexHashKey(RedisModuleCtx* ctx, IndexEntry& entry,
                         const std::string& key_name) {
  RedisModuleString* rms_key = RedisModule_CreateString(ctx, key_name.c_str(), key_name.size());
  auto* rkey = static_cast<RedisModuleKey*>(
      RedisModule_OpenKey(ctx, rms_key, REDISMODULE_READ));
  if (!rkey || RedisModule_KeyType(rkey) != REDISMODULE_KEYTYPE_HASH) return;

  std::unordered_map<std::string, std::string> doc_fields;
  for (auto& fspec : entry.spec.fields) {
    if (fspec.type == FieldType::kVector) continue;
    RedisModuleString* val = nullptr;
    RedisModule_HashGet(rkey, REDISMODULE_HASH_CFIELDS, fspec.name.c_str(), &val, NULL);
    if (val) {
      size_t len = 0;
      const char* data = RedisModule_StringPtrLen(val, &len);
      doc_fields[fspec.name] = std::string(data, len);
    }
  }

  if (doc_fields.empty()) return;

  RemoveDocFromIndices(entry, key_name);
  entry.doc_store.Add(key_name, doc_fields);
  AddDocToIndices(entry, key_name, doc_fields);
}

static void RemoveHashKey(IndexEntry& entry, const std::string& key_name) {
  if (!entry.doc_store.Contains(key_name)) return;
  RemoveDocFromIndices(entry, key_name);
  entry.doc_store.Remove(key_name);
}

struct ScanPrivdata {
  RedisModuleCtx* ctx;
  IndexEntry* entry;
};

static void ScanCallback(RedisModuleCtx* /*ctx*/, RedisModuleString* keyname,
                          RedisModuleKey* /*key*/, void* privdata) {
  auto* pd = static_cast<ScanPrivdata*>(privdata);
  size_t len = 0;
  const char* data = RedisModule_StringPtrLen(keyname, &len);
  std::string key_str(data, len);
  if (pd->entry->spec.MatchesPrefix(key_str)) {
    IndexHashKey(pd->ctx, *pd->entry, key_str);
  }
}

static void ScanExistingKeys(RedisModuleCtx* ctx, IndexEntry& entry) {
  auto* cursor = RedisModule_ScanCursorCreate();
  ScanPrivdata pd{ctx, &entry};
  while (RedisModule_Scan(ctx, cursor, ScanCallback, &pd)) {}
  RedisModule_ScanCursorDestroy(cursor);
}

static int OnKeyspaceEvent(RedisModuleCtx* ctx, int type, const char* event,
                           RedisModuleString* key) {
  (void)type;
  size_t len = 0;
  const char* data = RedisModule_StringPtrLen(key, &len);
  std::string key_str(data, len);

  bool is_del = (strcmp(event, "del") == 0 || strcmp(event, "expired") == 0 ||
                 strcmp(event, "evicted") == 0);
  bool is_hash_write = (strcmp(event, "hset") == 0 || strcmp(event, "hdel") == 0 ||
                        strcmp(event, "hincrby") == 0 || strcmp(event, "hincrbyfloat") == 0 ||
                        strcmp(event, "hmset") == 0 || strcmp(event, "hsetnx") == 0);

  for (auto& [idx_name, entry] : g_indices) {
    if (!entry.spec.HasPrefixes()) continue;
    if (!entry.spec.MatchesPrefix(key_str)) continue;

    if (is_del) {
      RemoveHashKey(entry, key_str);
    } else if (is_hash_write) {
      IndexHashKey(ctx, entry, key_str);
    }
  }

  return REDISMODULE_OK;
}

// FT.AGGREGATE <index> <query>
//   [GROUPBY <nargs> @field ...]
//   [REDUCE <func> <nargs> [@field ...] AS <alias>] ...
//   [SORTBY <nargs> @field ASC|DESC ...]
//   [LIMIT <offset> <count>]
static int FtAggregateCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                              int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str(ArgView(argv[1]));
  auto it = g_indices.find(idx_str);
  if (it == g_indices.end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  auto& entry = it->second;

  ParsedQuery parsed;
  std::string parse_error;
  if (!ParseQuery(std::string(ArgView(argv[2])), parsed, parse_error)) {
    return RedisModule_ReplyWithError(ctx, parse_error.c_str());
  }

  std::string eval_error;
  auto result_ids =
      EvaluateQuery(parsed.root, entry.spec, entry.doc_store,
                    entry.tag_indices, entry.numeric_indices,
                    entry.text_indices, eval_error);
  if (!eval_error.empty()) {
    return RedisModule_ReplyWithError(ctx, eval_error.c_str());
  }

  // Parse pipeline stages
  std::vector<std::string> groupby_fields;

  enum class ReduceFunc { kCount, kSum, kAvg, kMin, kMax, kCountDistinct };
  struct ReduceOp {
    ReduceFunc func;
    std::string field;
    std::string alias;
  };
  std::vector<ReduceOp> reducers;

  std::string sortby_field;
  bool sortby_desc = false;
  bool has_sortby = false;
  long long limit_offset = 0;
  long long limit_count = -1;
  bool has_limit = false;

  int arg_i = 3;
  while (arg_i < argc) {
    auto kw = ArgView(argv[arg_i]);

    if (MatchArg(kw, "GROUPBY")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR GROUPBY requires count");
      }
      char* endptr = nullptr;
      long gb_count = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || gb_count < 0) {
        return RedisModule_ReplyWithError(ctx, "ERR GROUPBY count must be non-negative");
      }
      for (long g = 0; g < gb_count; g++) {
        if (arg_i >= argc) {
          return RedisModule_ReplyWithError(ctx, "ERR not enough GROUPBY fields");
        }
        auto fv = ArgView(argv[arg_i]);
        arg_i++;
        if (fv.empty() || fv[0] != '@') {
          return RedisModule_ReplyWithError(ctx, "ERR GROUPBY field must start with @");
        }
        groupby_fields.emplace_back(fv.substr(1));
      }
    } else if (MatchArg(kw, "REDUCE")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR REDUCE requires function name");
      }
      auto func_str = ArgView(argv[arg_i]);
      arg_i++;

      ReduceFunc func;
      bool needs_field = true;
      if (MatchArg(func_str, "COUNT")) {
        func = ReduceFunc::kCount;
        needs_field = false;
      } else if (MatchArg(func_str, "SUM")) {
        func = ReduceFunc::kSum;
      } else if (MatchArg(func_str, "AVG")) {
        func = ReduceFunc::kAvg;
      } else if (MatchArg(func_str, "MIN")) {
        func = ReduceFunc::kMin;
      } else if (MatchArg(func_str, "MAX")) {
        func = ReduceFunc::kMax;
      } else if (MatchArg(func_str, "COUNT_DISTINCT")) {
        func = ReduceFunc::kCountDistinct;
      } else {
        return RedisModule_ReplyWithError(ctx, "ERR unknown REDUCE function");
      }

      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR REDUCE requires nargs");
      }
      char* endptr = nullptr;
      long nargs = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || nargs < 0) {
        return RedisModule_ReplyWithError(ctx, "ERR REDUCE nargs must be non-negative");
      }

      std::string reduce_field;
      if (needs_field && nargs >= 1) {
        if (arg_i >= argc) {
          return RedisModule_ReplyWithError(ctx, "ERR REDUCE missing field argument");
        }
        auto rf = ArgView(argv[arg_i]);
        arg_i++;
        if (rf.empty() || rf[0] != '@') {
          return RedisModule_ReplyWithError(ctx, "ERR REDUCE field must start with @");
        }
        reduce_field = std::string(rf.substr(1));
        for (long skip = 1; skip < nargs; skip++) {
          if (arg_i < argc) arg_i++;
        }
      } else {
        for (long skip = 0; skip < nargs; skip++) {
          if (arg_i < argc) arg_i++;
        }
      }

      if (arg_i >= argc || !MatchArg(ArgView(argv[arg_i]), "AS")) {
        return RedisModule_ReplyWithError(ctx, "ERR REDUCE requires AS alias");
      }
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR REDUCE AS requires alias name");
      }
      std::string alias(ArgView(argv[arg_i]));
      arg_i++;

      reducers.push_back({func, std::move(reduce_field), std::move(alias)});
    } else if (MatchArg(kw, "SORTBY")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR SORTBY requires count");
      }
      char* endptr = nullptr;
      long sb_count = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || sb_count < 2) {
        return RedisModule_ReplyWithError(ctx, "ERR SORTBY count must be >= 2");
      }
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR SORTBY requires field");
      }
      auto sf = ArgView(argv[arg_i]);
      arg_i++;
      if (sf.empty() || sf[0] != '@') {
        return RedisModule_ReplyWithError(ctx, "ERR SORTBY field must start with @");
      }
      sortby_field = std::string(sf.substr(1));
      has_sortby = true;
      if (arg_i < argc) {
        auto dir = ArgView(argv[arg_i]);
        if (MatchArg(dir, "ASC")) {
          sortby_desc = false;
          arg_i++;
        } else if (MatchArg(dir, "DESC")) {
          sortby_desc = true;
          arg_i++;
        }
      }
      for (long skip = 2; skip < sb_count; skip++) {
        if (arg_i < argc) arg_i++;
      }
    } else if (MatchArg(kw, "LIMIT")) {
      arg_i++;
      if (arg_i + 1 >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR LIMIT requires offset and count");
      }
      char* ep1 = nullptr;
      char* ep2 = nullptr;
      std::string off_s(ArgView(argv[arg_i]));
      std::string cnt_s(ArgView(argv[arg_i + 1]));
      long long off = std::strtoll(off_s.c_str(), &ep1, 10);
      long long cnt = std::strtoll(cnt_s.c_str(), &ep2, 10);
      if (*ep1 != '\0' || *ep2 != '\0' || off < 0 || cnt < 0) {
        return RedisModule_ReplyWithError(ctx, "ERR LIMIT offset and count must be non-negative");
      }
      limit_offset = off;
      limit_count = cnt;
      has_limit = true;
      arg_i += 2;
    } else {
      return RedisModule_ReplyWithError(ctx, "ERR unknown AGGREGATE option");
    }
  }

  // Execute aggregation pipeline
  using Row = std::unordered_map<std::string, std::string>;

  if (groupby_fields.empty() && reducers.empty()) {
    // No GROUPBY/REDUCE: return raw document rows
    std::vector<Row> rows;
    for (auto& doc_id : result_ids) {
      const auto* doc = entry.doc_store.Get(doc_id);
      if (!doc) continue;
      rows.push_back(doc->fields);
    }

    if (has_sortby) {
      std::sort(rows.begin(), rows.end(),
                [&](const Row& a, const Row& b) {
                  auto ait = a.find(sortby_field);
                  auto bit = b.find(sortby_field);
                  std::string va = (ait != a.end()) ? ait->second : "";
                  std::string vb = (bit != b.end()) ? bit->second : "";
                  double na = 0, nb = 0;
                  bool num = TryParseDouble(va, na) && TryParseDouble(vb, nb);
                  if (num) return sortby_desc ? na > nb : na < nb;
                  return sortby_desc ? va > vb : va < vb;
                });
    }

    if (has_limit) {
      long long total = static_cast<long long>(rows.size());
      if (limit_offset >= total) {
        rows.clear();
      } else {
        auto begin = rows.begin() + limit_offset;
        auto end = (limit_count >= 0 && limit_offset + limit_count < total)
                       ? rows.begin() + limit_offset + limit_count
                       : rows.end();
        rows = std::vector<Row>(begin, end);
      }
    }

    RedisModule_ReplyWithArray(ctx, static_cast<long>(rows.size()));
    for (auto& row : rows) {
      std::vector<std::string> keys;
      for (auto& [k, v] : row) keys.push_back(k);
      std::sort(keys.begin(), keys.end());
      RedisModule_ReplyWithArray(ctx, static_cast<long>(keys.size()) * 2);
      for (auto& k : keys) {
        RedisModule_ReplyWithCString(ctx, k.c_str());
        RedisModule_ReplyWithCString(ctx, row.at(k).c_str());
      }
    }
    return REDISMODULE_OK;
  }

  // GROUPBY + REDUCE
  struct GroupAccum {
    long long count = 0;
    std::unordered_map<std::string, double> sum;
    std::unordered_map<std::string, long long> field_count;
    std::unordered_map<std::string, double> min_val;
    std::unordered_map<std::string, double> max_val;
    std::unordered_map<std::string, std::unordered_set<std::string>> distinct;
  };

  auto MakeGroupKey = [&](const Document* doc) -> std::string {
    std::string key;
    for (size_t i = 0; i < groupby_fields.size(); i++) {
      if (i > 0) key += "\x01";
      auto fit = doc->fields.find(groupby_fields[i]);
      if (fit != doc->fields.end()) key += fit->second;
    }
    return key;
  };

  std::unordered_map<std::string, GroupAccum> groups;
  std::unordered_map<std::string, Row> group_key_values;

  for (auto& doc_id : result_ids) {
    const auto* doc = entry.doc_store.Get(doc_id);
    if (!doc) continue;

    std::string gk = MakeGroupKey(doc);
    auto& accum = groups[gk];
    accum.count++;

    if (group_key_values.find(gk) == group_key_values.end()) {
      Row kv;
      for (auto& gf : groupby_fields) {
        auto fit = doc->fields.find(gf);
        kv[gf] = (fit != doc->fields.end()) ? fit->second : "";
      }
      group_key_values[gk] = std::move(kv);
    }

    for (auto& r : reducers) {
      if (r.func == ReduceFunc::kCount) continue;
      auto fit = doc->fields.find(r.field);
      if (fit == doc->fields.end()) continue;

      if (r.func == ReduceFunc::kCountDistinct) {
        accum.distinct[r.alias].insert(fit->second);
        continue;
      }

      double val = 0;
      if (!TryParseDouble(fit->second, val)) continue;

      accum.sum[r.alias] += val;
      accum.field_count[r.alias]++;

      auto min_it = accum.min_val.find(r.alias);
      if (min_it == accum.min_val.end() || val < min_it->second) {
        accum.min_val[r.alias] = val;
      }
      auto max_it = accum.max_val.find(r.alias);
      if (max_it == accum.max_val.end() || val > max_it->second) {
        accum.max_val[r.alias] = val;
      }
    }
  }

  // Build result rows
  std::vector<Row> result_rows;
  result_rows.reserve(groups.size());
  for (auto& [gk, accum] : groups) {
    Row row = group_key_values[gk];
    for (auto& r : reducers) {
      switch (r.func) {
        case ReduceFunc::kCount:
          row[r.alias] = std::to_string(accum.count);
          break;
        case ReduceFunc::kSum: {
          auto sit = accum.sum.find(r.alias);
          row[r.alias] = (sit != accum.sum.end()) ? std::to_string(sit->second) : "0";
          break;
        }
        case ReduceFunc::kAvg: {
          auto sit = accum.sum.find(r.alias);
          auto cit = accum.field_count.find(r.alias);
          if (sit != accum.sum.end() && cit != accum.field_count.end() && cit->second > 0) {
            row[r.alias] = std::to_string(sit->second / static_cast<double>(cit->second));
          } else {
            row[r.alias] = "0";
          }
          break;
        }
        case ReduceFunc::kMin: {
          auto mit = accum.min_val.find(r.alias);
          row[r.alias] = (mit != accum.min_val.end()) ? std::to_string(mit->second) : "0";
          break;
        }
        case ReduceFunc::kMax: {
          auto mit = accum.max_val.find(r.alias);
          row[r.alias] = (mit != accum.max_val.end()) ? std::to_string(mit->second) : "0";
          break;
        }
        case ReduceFunc::kCountDistinct: {
          auto dit = accum.distinct.find(r.alias);
          long long dc = (dit != accum.distinct.end()) ? static_cast<long long>(dit->second.size()) : 0;
          row[r.alias] = std::to_string(dc);
          break;
        }
      }
    }
    result_rows.push_back(std::move(row));
  }

  // SORTBY
  if (has_sortby) {
    std::sort(result_rows.begin(), result_rows.end(),
              [&](const Row& a, const Row& b) {
                auto ait = a.find(sortby_field);
                auto bit = b.find(sortby_field);
                std::string va = (ait != a.end()) ? ait->second : "";
                std::string vb = (bit != b.end()) ? bit->second : "";
                double na = 0, nb = 0;
                bool num = TryParseDouble(va, na) && TryParseDouble(vb, nb);
                if (num) return sortby_desc ? na > nb : na < nb;
                return sortby_desc ? va > vb : va < vb;
              });
  }

  // LIMIT
  if (has_limit) {
    long long total = static_cast<long long>(result_rows.size());
    if (limit_offset >= total) {
      result_rows.clear();
    } else {
      auto begin = result_rows.begin() + limit_offset;
      auto end = (limit_count >= 0 && limit_offset + limit_count < total)
                     ? result_rows.begin() + limit_offset + limit_count
                     : result_rows.end();
      result_rows = std::vector<Row>(begin, end);
    }
  }

  // Reply
  RedisModule_ReplyWithArray(ctx, static_cast<long>(result_rows.size()));
  for (auto& row : result_rows) {
    std::vector<std::string> keys;
    for (auto& [k, v] : row) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    RedisModule_ReplyWithArray(ctx, static_cast<long>(keys.size()) * 2);
    for (auto& k : keys) {
      RedisModule_ReplyWithCString(ctx, k.c_str());
      RedisModule_ReplyWithCString(ctx, row.at(k).c_str());
    }
  }

  return REDISMODULE_OK;
}

int RegisterSearchCommands(RedisModuleCtx* ctx) {
  struct CmdEntry {
    const char* name;
    RedisModuleCmdFunc handler;
    const char* flags;
  };

  CmdEntry commands[] = {
      {"FT.CREATE", FtCreateCommand, "write deny-oom"},
      {"FT.DROPINDEX", FtDropIndexCommand, "write"},
      {"FT.INFO", FtInfoCommand, "readonly"},
      {"FT._LIST", FtListCommand, "readonly"},
      {"FT.ADD", FtAddCommand, "write deny-oom"},
      {"FT.DEL", FtDelCommand, "write"},
      {"FT.SEARCH", FtSearchCommand, "readonly"},
      {"FT.AGGREGATE", FtAggregateCommand, "readonly"},
      {"FT._DEBUG", FtDebugCommand, "readonly"},
  };

  for (auto& cmd : commands) {
    if (RedisModule_CreateCommand(ctx, cmd.name, cmd.handler, cmd.flags, 0, 0,
                                  0) == REDISMODULE_ERR) {
      return REDISMODULE_ERR;
    }
  }

  if (RedisModule_SubscribeToKeyspaceEvents(
          ctx, REDISMODULE_NOTIFY_HASH | REDISMODULE_NOTIFY_GENERIC,
          OnKeyspaceEvent) == REDISMODULE_ERR) {
    RedisModule_Log(ctx, "warning", "Failed to subscribe to keyspace events");
  }

  return REDISMODULE_OK;
}
