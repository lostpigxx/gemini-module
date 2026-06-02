#include "search_commands.h"
#include "query_parser.h"
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
      entry.vector_indices.GetOrCreate(
          fname, fspec->vector_params.dim, fspec->vector_params.metric)
          .Remove(doc_id);
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
        auto& vidx = entry.vector_indices.GetOrCreate(
            fname, dim, fspec->vector_params.metric);
        vidx.Add(doc_id, reinterpret_cast<const float*>(fval.data()));
      }
    }
  }
}

// FT.CREATE <index_name> SCHEMA <field> <type> [params...] [<field> <type> ...]
static int FtCreateCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
  if (argc < 5) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto index_name = ArgView(argv[1]);
  auto schema_kw = ArgView(argv[2]);

  if (!MatchArg(schema_kw, "SCHEMA")) {
    return RedisModule_ReplyWithError(
        ctx, "ERR syntax error, expected SCHEMA keyword");
  }

  std::vector<FieldSpec> fields;
  int i = 3;
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
      if (!MatchArg(algo_str, "FLAT")) {
        return RedisModule_ReplyWithError(
            ctx, "ERR unknown VECTOR algorithm, expected FLAT");
      }
      fspec.vector_params.algorithm = VectorAlgorithm::kFlat;

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
    } else {
      return RedisModule_ReplyWithError(
          ctx, "ERR unknown field type, expected TAG, NUMERIC, or VECTOR");
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
  it->second.spec = IndexSpec{idx_str, std::move(fields)};

  if (SearchModuleType) {
    auto* key = static_cast<RedisModuleKey*>(RedisModule_OpenKey(
        ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE));
    RedisModule_ModuleTypeSetValue(key, SearchModuleType,
                                   new std::string(idx_str));
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

  RedisModule_ReplyWithArray(ctx, 6);

  RedisModule_ReplyWithSimpleString(ctx, "index_name");
  RedisModule_ReplyWithCString(ctx, entry.spec.name.c_str());

  RedisModule_ReplyWithSimpleString(ctx, "num_docs");
  RedisModule_ReplyWithLongLong(
      ctx, static_cast<long long>(entry.doc_store.Size()));

  RedisModule_ReplyWithSimpleString(ctx, "fields");
  RedisModule_ReplyWithArray(ctx,
                             static_cast<long>(entry.spec.fields.size()));
  for (auto& f : entry.spec.fields) {
    if (f.type == FieldType::kVector) {
      RedisModule_ReplyWithArray(ctx, 8);
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
};

static void ReplyWithDocFields(RedisModuleCtx* ctx, const Document* doc,
                               const SearchOptions& opts,
                               const std::string* vec_score) {
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
  if (vec_score) count += 2;

  RedisModule_ReplyWithArray(ctx, count);
  if (vec_score) {
    RedisModule_ReplyWithCString(ctx, "__vec_score");
    RedisModule_ReplyWithCString(ctx, vec_score->c_str());
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

  ParsedQuery parsed;
  std::string parse_error;
  if (!ParseQuery(std::string(ArgView(argv[2])), parsed, parse_error)) {
    return RedisModule_ReplyWithError(ctx, parse_error.c_str());
  }

  // Parse optional blocks: PARAMS, RETURN, SORTBY, LIMIT
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
    } else {
      return RedisModule_ReplyWithError(ctx,
                                        "ERR unknown search option");
    }
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
      knn_results = vidx->KnnQuery(
          reinterpret_cast<const float*>(blob.data()), parsed.knn_k);
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
    RedisModule_ReplyWithArray(ctx, 1 + total * 2);
    RedisModule_ReplyWithLongLong(ctx, total);

    for (auto& kr : knn_results) {
      RedisModule_ReplyWithCString(ctx, kr.doc_id.c_str());
      auto score_str = std::to_string(kr.score);
      ReplyWithDocFields(ctx, entry.doc_store.Get(kr.doc_id), opts,
                         &score_str);
    }

    return REDISMODULE_OK;
  }

  // Non-KNN: evaluate query tree
  std::string eval_error;
  auto result_ids =
      EvaluateQuery(parsed.root, entry.spec, entry.doc_store,
                    entry.tag_indices, entry.numeric_indices, eval_error);
  if (!eval_error.empty()) {
    return RedisModule_ReplyWithError(ctx, eval_error.c_str());
  }

  // SORTBY
  if (opts.has_sortby) {
    const auto* sort_fspec = entry.spec.FindField(opts.sortby_field);
    if (!sort_fspec) {
      return RedisModule_ReplyWithError(ctx,
                                        "ERR SORTBY field not in schema");
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
  RedisModule_ReplyWithArray(ctx, 1 + total * 2);
  RedisModule_ReplyWithLongLong(ctx, total);

  for (auto& rid : result_ids) {
    RedisModule_ReplyWithCString(ctx, rid.c_str());
    ReplyWithDocFields(ctx, entry.doc_store.Get(rid), opts, nullptr);
  }

  return REDISMODULE_OK;
}

// FT._DEBUG
static int FtDebugCommand(RedisModuleCtx* ctx, RedisModuleString** /*argv*/,
                          int /*argc*/) {
  return RedisModule_ReplyWithSimpleString(ctx, "GeminiSearch OK");
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
      {"FT._DEBUG", FtDebugCommand, "readonly"},
  };

  for (auto& cmd : commands) {
    if (RedisModule_CreateCommand(ctx, cmd.name, cmd.handler, cmd.flags, 0, 0,
                                  0) == REDISMODULE_ERR) {
      return REDISMODULE_ERR;
    }
  }
  return REDISMODULE_OK;
}
