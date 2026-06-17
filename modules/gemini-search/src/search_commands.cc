#include "search_commands.h"
#include "expr_engine.h"
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
static std::unordered_map<std::string, std::string> g_aliases;

static std::string ResolveAlias(const std::string& name) {
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
    }
  }
}

static void AddDocToIndices(
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
      entry.text_indices.GetOrCreate(fname).Add(doc_id, fval);
    } else if (fspec->type == FieldType::kGeo) {
      GeoCoord coord;
      if (ParseGeoCoord(fval, coord)) {
        entry.geo_indices.GetOrCreate(fname).Add(doc_id, coord.lon, coord.lat);
      }
    }
  }
}

static void ScanExistingKeys(RedisModuleCtx* ctx, IndexEntry& entry);
static void IndexHashKey(RedisModuleCtx* ctx, IndexEntry& entry,
                         const std::string& key_name);
static void IndexJsonKey(RedisModuleCtx* ctx, IndexEntry& entry,
                         const std::string& key_name);

// FT.CREATE <index_name> [ON HASH|JSON PREFIX <count> <prefix> ...] SCHEMA <field> <type> [params...]
static int FtCreateCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
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
    } else if (MatchArg(ftype_str, "GEO")) {
      fspec.type = FieldType::kGeo;
    } else {
      return RedisModule_ReplyWithError(
          ctx, "ERR unknown field type, expected TAG, NUMERIC, TEXT, VECTOR, or GEO");
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
  auto [it, inserted] = g_indices.try_emplace(idx_str);
  if (!inserted) {
    return RedisModule_ReplyWithError(ctx, "ERR index already exists");
  }
  it->second.spec = IndexSpec{idx_str, std::move(fields), std::move(prefixes),
                              index_language, source_type,
                              std::move(custom_stopwords), has_custom_stopwords,
                              idx_nofreqs, idx_nooffsets, idx_nohl, idx_temporary};

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

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  if (g_indices.erase(idx_str) == 0) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  // Remove any aliases pointing to this index
  for (auto ait = g_aliases.begin(); ait != g_aliases.end(); ) {
    if (ait->second == idx_str) ait = g_aliases.erase(ait);
    else ++ait;
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

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
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

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
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
  std::string geofilter_field;
  double geofilter_lon = 0;
  double geofilter_lat = 0;
  double geofilter_radius = 0;
  GeoUnit geofilter_unit = GeoUnit::kKm;
  bool has_geofilter = false;
  bool has_highlight = false;
  std::vector<std::string> highlight_fields;
  std::string highlight_open = "<b>";
  std::string highlight_close = "</b>";
  bool has_summarize = false;
  std::vector<std::string> summarize_fields;
  int summarize_frags = 3;
  int summarize_len = 20;
  std::string summarize_separator = "...";
};

static void CollectQueryTerms(const QueryNode& node,
                              std::unordered_set<std::string>& terms) {
  if (node.type == QueryNode::Type::kTextMatch) {
    for (auto& t : node.text_terms) terms.insert(t);
  }
  for (auto& child : node.children) CollectQueryTerms(child, terms);
}

static std::string ApplyHighlight(const std::string& text,
                                  const std::unordered_set<std::string>& terms,
                                  const std::string& open_tag,
                                  const std::string& close_tag) {
  std::string result;
  for (size_t i = 0; i < text.size(); ) {
    if (std::isalnum(static_cast<unsigned char>(text[i]))) {
      size_t start = i;
      while (i < text.size() && std::isalnum(static_cast<unsigned char>(text[i]))) i++;
      std::string word = text.substr(start, i - start);
      std::string lower = word;
      for (auto& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (terms.count(lower)) {
        result += open_tag + word + close_tag;
      } else {
        result += word;
      }
    } else {
      result += text[i];
      i++;
    }
  }
  return result;
}

static std::string ApplySummarize(const std::string& text,
                                  const std::unordered_set<std::string>& terms,
                                  int max_frags, int frag_len,
                                  const std::string& separator) {
  // Tokenize into words preserving their positions
  std::vector<std::string> words;
  std::vector<bool> is_match;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && !std::isalnum(static_cast<unsigned char>(text[i]))) i++;
    if (i >= text.size()) break;
    size_t start = i;
    while (i < text.size() && std::isalnum(static_cast<unsigned char>(text[i]))) i++;
    std::string word = text.substr(start, i - start);
    std::string lower = word;
    for (auto& c : lower)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    words.push_back(word);
    is_match.push_back(terms.count(lower) > 0);
  }

  if (words.empty()) return text;

  // Find match positions, extract fragments around them
  std::vector<std::string> fragments;
  std::set<int> used_positions;

  for (int wi = 0; wi < static_cast<int>(words.size()) && static_cast<int>(fragments.size()) < max_frags; wi++) {
    if (!is_match[wi] || used_positions.count(wi)) continue;

    int half = frag_len / 2;
    int frag_start = std::max(0, wi - half);
    int frag_end = std::min(static_cast<int>(words.size()), frag_start + frag_len);
    if (frag_end - frag_start < frag_len && frag_end == static_cast<int>(words.size()))
      frag_start = std::max(0, frag_end - frag_len);

    std::string frag;
    for (int fi = frag_start; fi < frag_end; fi++) {
      if (fi > frag_start) frag += " ";
      frag += words[fi];
      used_positions.insert(fi);
    }
    fragments.push_back(std::move(frag));
  }

  if (fragments.empty()) {
    // No matches found — return first frag_len words
    std::string frag;
    int limit = std::min(frag_len, static_cast<int>(words.size()));
    for (int fi = 0; fi < limit; fi++) {
      if (fi > 0) frag += " ";
      frag += words[fi];
    }
    return frag + separator;
  }

  std::string result;
  for (size_t fi = 0; fi < fragments.size(); fi++) {
    if (fi > 0) result += separator;
    result += fragments[fi];
  }
  return result;
}

static void ReplyWithDocFields(RedisModuleCtx* ctx, const Document* doc,
                               const SearchOptions& opts,
                               const std::string* score_val,
                               const std::unordered_set<std::string>& query_terms,
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

  std::unordered_set<std::string> hl_set(opts.highlight_fields.begin(),
                                          opts.highlight_fields.end());
  std::unordered_set<std::string> sm_set(opts.summarize_fields.begin(),
                                          opts.summarize_fields.end());

  for (auto& k : keys) {
    RedisModule_ReplyWithCString(ctx, k.c_str());
    auto fit = doc->fields.find(k);
    if (fit != doc->fields.end()) {
      std::string val = fit->second;
      bool do_hl = opts.has_highlight && (hl_set.empty() || hl_set.count(k));
      bool do_sm = opts.has_summarize && (sm_set.empty() || sm_set.count(k));
      if (do_sm && !query_terms.empty()) {
        val = ApplySummarize(val, query_terms, opts.summarize_frags,
                             opts.summarize_len, opts.summarize_separator);
      }
      if (do_hl && !query_terms.empty()) {
        val = ApplyHighlight(val, query_terms, opts.highlight_open, opts.highlight_close);
      }
      RedisModule_ReplyWithCString(ctx, val.c_str());
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

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
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
    } else if (MatchArg(kw, "HIGHLIGHT")) {
      opts.has_highlight = true;
      arg_i++;
      if (arg_i < argc && MatchArg(ArgView(argv[arg_i]), "FIELDS")) {
        arg_i++;
        if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR HIGHLIGHT FIELDS requires count");
        char* endptr = nullptr;
        long hf_count = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
        arg_i++;
        if (*endptr != '\0' || hf_count < 0)
          return RedisModule_ReplyWithError(ctx, "ERR HIGHLIGHT FIELDS count must be non-negative");
        for (long hf = 0; hf < hf_count; hf++) {
          if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR not enough HIGHLIGHT FIELDS");
          opts.highlight_fields.emplace_back(ArgView(argv[arg_i]));
          arg_i++;
        }
      }
      if (arg_i < argc && MatchArg(ArgView(argv[arg_i]), "TAGS")) {
        arg_i++;
        if (arg_i + 1 >= argc)
          return RedisModule_ReplyWithError(ctx, "ERR HIGHLIGHT TAGS requires open and close tags");
        opts.highlight_open = std::string(ArgView(argv[arg_i]));
        arg_i++;
        opts.highlight_close = std::string(ArgView(argv[arg_i]));
        arg_i++;
      }
    } else if (MatchArg(kw, "SUMMARIZE")) {
      opts.has_summarize = true;
      arg_i++;
      if (arg_i < argc && MatchArg(ArgView(argv[arg_i]), "FIELDS")) {
        arg_i++;
        if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR SUMMARIZE FIELDS requires count");
        char* endptr = nullptr;
        long sf_count = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
        arg_i++;
        if (*endptr != '\0' || sf_count < 0)
          return RedisModule_ReplyWithError(ctx, "ERR SUMMARIZE FIELDS count must be non-negative");
        for (long sf = 0; sf < sf_count; sf++) {
          if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR not enough SUMMARIZE FIELDS");
          opts.summarize_fields.emplace_back(ArgView(argv[arg_i]));
          arg_i++;
        }
      }
      while (arg_i < argc) {
        auto sub = ArgView(argv[arg_i]);
        if (MatchArg(sub, "FRAGS")) {
          arg_i++;
          if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR SUMMARIZE FRAGS requires value");
          char* ep = nullptr;
          opts.summarize_frags = static_cast<int>(
              std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &ep, 10));
          arg_i++;
        } else if (MatchArg(sub, "LEN")) {
          arg_i++;
          if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR SUMMARIZE LEN requires value");
          char* ep = nullptr;
          opts.summarize_len = static_cast<int>(
              std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &ep, 10));
          arg_i++;
        } else if (MatchArg(sub, "SEPARATOR")) {
          arg_i++;
          if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR SUMMARIZE SEPARATOR requires value");
          opts.summarize_separator = std::string(ArgView(argv[arg_i]));
          arg_i++;
        } else {
          break;
        }
      }
    } else if (MatchArg(kw, "GEOFILTER")) {
      arg_i++;
      if (arg_i + 4 >= argc) {
        return RedisModule_ReplyWithError(
            ctx, "ERR GEOFILTER requires field lon lat radius unit");
      }
      opts.geofilter_field = std::string(ArgView(argv[arg_i]));
      arg_i++;
      char* ep1 = nullptr;
      char* ep2 = nullptr;
      char* ep3 = nullptr;
      opts.geofilter_lon = std::strtod(std::string(ArgView(argv[arg_i])).c_str(), &ep1);
      arg_i++;
      opts.geofilter_lat = std::strtod(std::string(ArgView(argv[arg_i])).c_str(), &ep2);
      arg_i++;
      opts.geofilter_radius = std::strtod(std::string(ArgView(argv[arg_i])).c_str(), &ep3);
      arg_i++;
      if (*ep1 != '\0' || *ep2 != '\0' || *ep3 != '\0' || opts.geofilter_radius < 0) {
        return RedisModule_ReplyWithError(
            ctx, "ERR GEOFILTER invalid numeric values");
      }
      std::string unit_s(ArgView(argv[arg_i]));
      if (!ParseGeoUnit(unit_s, opts.geofilter_unit)) {
        return RedisModule_ReplyWithError(
            ctx, "ERR GEOFILTER unknown unit, expected m|km|mi|ft");
      }
      opts.has_geofilter = true;
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

  // Collect query terms for HIGHLIGHT/SUMMARIZE
  std::unordered_set<std::string> query_terms;
  if (opts.has_highlight || opts.has_summarize) {
    CollectQueryTerms(parsed.root, query_terms);
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
                                        entry.text_indices, entry.geo_indices,
                                        filter_error, qopts);
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
                           &score_str, query_terms);
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
        entry.text_indices, entry.geo_indices, eval_error, qopts);
    if (!eval_error.empty()) {
      return RedisModule_ReplyWithError(ctx, eval_error.c_str());
    }
  } else {
    result_ids = EvaluateQuery(
        parsed.root, entry.spec, entry.doc_store,
        entry.tag_indices, entry.numeric_indices,
        entry.text_indices, entry.geo_indices, eval_error, qopts);
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

  // GEOFILTER (legacy option)
  if (opts.has_geofilter) {
    const auto* gf_fspec = entry.spec.FindField(opts.geofilter_field);
    if (!gf_fspec || gf_fspec->type != FieldType::kGeo) {
      return RedisModule_ReplyWithError(ctx, "ERR GEOFILTER field is not a GEO field");
    }
    const auto* gidx = entry.geo_indices.Get(opts.geofilter_field);
    if (gidx) {
      double radius_m = opts.geofilter_radius * GeoUnitToMeters(opts.geofilter_unit);
      auto geo_hits = gidx->RadiusQuery(opts.geofilter_lon, opts.geofilter_lat, radius_m);
      std::unordered_set<std::string> geo_set;
      for (auto& gr : geo_hits) geo_set.insert(gr.doc_id);
      if (opts.withscores) {
        scored_results.erase(
            std::remove_if(scored_results.begin(), scored_results.end(),
                           [&](const ScoredResult& r) {
                             return geo_set.find(r.doc_id) == geo_set.end();
                           }),
            scored_results.end());
      } else {
        result_ids.erase(
            std::remove_if(result_ids.begin(), result_ids.end(),
                           [&](const std::string& id) {
                             return geo_set.find(id) == geo_set.end();
                           }),
            result_ids.end());
      }
    } else {
      scored_results.clear();
      result_ids.clear();
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
                           &score_str, query_terms, "__search_score");
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
        ReplyWithDocFields(ctx, entry.doc_store.Get(rid), opts, nullptr, query_terms);
      }
    }
  }

  return REDISMODULE_OK;
}

// FT.ALTER <index> SCHEMA ADD <field> <type> [params...] [<field> <type> ...]
static int FtAlterCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                          int argc) {
  if (argc < 6) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  auto it = g_indices.find(idx_str);
  if (it == g_indices.end()) {
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
static int FtAliasAddCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                             int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string alias(ArgView(argv[1]));
  std::string idx(ArgView(argv[2]));

  if (g_indices.find(idx) == g_indices.end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  if (g_aliases.find(alias) != g_aliases.end()) {
    return RedisModule_ReplyWithError(ctx, "ERR alias already exists");
  }
  g_aliases[alias] = idx;
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.ALIASDEL <alias>
static int FtAliasDelCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                             int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string alias(ArgView(argv[1]));
  if (g_aliases.erase(alias) == 0) {
    return RedisModule_ReplyWithError(ctx, "ERR alias not found");
  }
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.ALIASUPDATE <alias> <index>
static int FtAliasUpdateCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                                int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string alias(ArgView(argv[1]));
  std::string idx(ArgView(argv[2]));

  if (g_indices.find(idx) == g_indices.end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  g_aliases[alias] = idx;
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.TAGVALS <index> <field>
static int FtTagvalsCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                            int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  auto it = g_indices.find(idx_str);
  if (it == g_indices.end()) {
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

// Query tree serializer for EXPLAIN
static std::string ExplainNode(const QueryNode& node, int indent) {
  std::string pad(static_cast<size_t>(indent * 2), ' ');
  std::string result;

  switch (node.type) {
    case QueryNode::Type::kMatchAll:
      result = pad + "*\n";
      break;
    case QueryNode::Type::kTagMatch:
      result = pad + "TAG @" + node.field_name + ":{";
      for (size_t i = 0; i < node.tag_values.size(); i++) {
        if (i > 0) result += " | ";
        result += node.tag_values[i];
      }
      result += "}\n";
      break;
    case QueryNode::Type::kNumericRange:
      result = pad + "NUMERIC @" + node.field_name + ":[" +
               (node.min_exclusive ? "(" : "") + std::to_string(node.range_min) +
               " " +
               (node.max_exclusive ? "(" : "") + std::to_string(node.range_max) +
               "]\n";
      break;
    case QueryNode::Type::kTextMatch:
      result = pad + "TEXT";
      if (!node.field_name.empty()) result += " @" + node.field_name;
      if (node.is_phrase) result += " PHRASE";
      result += ":";
      for (size_t i = 0; i < node.text_terms.size(); i++) {
        result += " " + node.text_terms[i];
      }
      result += "\n";
      break;
    case QueryNode::Type::kGeoFilter:
      result = pad + "GEO @" + node.field_name + ":[" +
               std::to_string(node.geo_lon) + " " +
               std::to_string(node.geo_lat) + " " +
               std::to_string(node.geo_radius) + "]\n";
      break;
    case QueryNode::Type::kAnd:
      result = pad + "AND\n";
      for (auto& c : node.children) result += ExplainNode(c, indent + 1);
      break;
    case QueryNode::Type::kOr:
      result = pad + "OR\n";
      for (auto& c : node.children) result += ExplainNode(c, indent + 1);
      break;
    case QueryNode::Type::kNot:
      result = pad + "NOT\n";
      for (auto& c : node.children) result += ExplainNode(c, indent + 1);
      break;
    case QueryNode::Type::kOptional:
      result = pad + "OPTIONAL\n";
      for (auto& c : node.children) result += ExplainNode(c, indent + 1);
      break;
  }
  return result;
}

// FT.EXPLAIN <index> <query>
static int FtExplainCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                            int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  if (g_indices.find(idx_str) == g_indices.end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }

  ParsedQuery parsed;
  std::string parse_error;
  if (!ParseQuery(std::string(ArgView(argv[2])), parsed, parse_error)) {
    return RedisModule_ReplyWithError(ctx, parse_error.c_str());
  }

  std::string plan = ExplainNode(parsed.root, 0);
  if (parsed.has_knn) {
    plan += "KNN k=" + std::to_string(parsed.knn_k) +
            " @" + parsed.knn_field +
            " $" + parsed.knn_param_name + "\n";
  }

  // Remove trailing newline
  while (!plan.empty() && plan.back() == '\n') plan.pop_back();

  return RedisModule_ReplyWithCString(ctx, plan.c_str());
}

// FT.EXPLAINCLI <index> <query>
static int FtExplainCliCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                               int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  if (g_indices.find(idx_str) == g_indices.end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }

  ParsedQuery parsed;
  std::string parse_error;
  if (!ParseQuery(std::string(ArgView(argv[2])), parsed, parse_error)) {
    return RedisModule_ReplyWithError(ctx, parse_error.c_str());
  }

  std::string plan = ExplainNode(parsed.root, 0);
  if (parsed.has_knn) {
    plan += "KNN k=" + std::to_string(parsed.knn_k) +
            " @" + parsed.knn_field +
            " $" + parsed.knn_param_name + "\n";
  }

  // Split into lines and reply as array
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < plan.size()) {
    size_t nl = plan.find('\n', pos);
    if (nl == std::string::npos) nl = plan.size();
    if (nl > pos) lines.push_back(plan.substr(pos, nl - pos));
    pos = nl + 1;
  }

  RedisModule_ReplyWithArray(ctx, static_cast<long>(lines.size()));
  for (auto& line : lines) {
    RedisModule_ReplyWithCString(ctx, line.c_str());
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

static std::string StripJsonQuotes(const std::string& s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
    return s.substr(1, s.size() - 2);
  return s;
}

static std::string ExtractJsonScalar(const std::string& raw) {
  std::string trimmed = raw;
  while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
    trimmed.erase(trimmed.begin());
  while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
    trimmed.pop_back();
  if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']')
    trimmed = trimmed.substr(1, trimmed.size() - 2);
  while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
    trimmed.erase(trimmed.begin());
  while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
    trimmed.pop_back();
  return StripJsonQuotes(trimmed);
}

static std::vector<std::string> ExtractJsonArray(const std::string& raw) {
  std::string trimmed = raw;
  while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
    trimmed.erase(trimmed.begin());
  while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
    trimmed.pop_back();
  if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']')
    return {StripJsonQuotes(trimmed)};

  std::vector<std::string> results;
  size_t i = 1;
  while (i < trimmed.size() - 1) {
    while (i < trimmed.size() - 1 && (trimmed[i] == ' ' || trimmed[i] == ',')) i++;
    if (i >= trimmed.size() - 1) break;
    if (trimmed[i] == '"') {
      i++;
      size_t start = i;
      while (i < trimmed.size() - 1 && trimmed[i] != '"') i++;
      results.push_back(trimmed.substr(start, i - start));
      if (i < trimmed.size() - 1) i++;
    } else {
      size_t start = i;
      while (i < trimmed.size() - 1 && trimmed[i] != ',' && trimmed[i] != ']') i++;
      std::string val = trimmed.substr(start, i - start);
      while (!val.empty() && val.back() == ' ') val.pop_back();
      if (val != "null") results.push_back(std::move(val));
    }
  }
  return results;
}

static void IndexJsonKey(RedisModuleCtx* ctx, IndexEntry& entry,
                         const std::string& key_name) {
  std::unordered_map<std::string, std::string> doc_fields;

  for (auto& fspec : entry.spec.fields) {
    if (fspec.type == FieldType::kVector) continue;
    const std::string& path = fspec.json_path.empty() ? fspec.name : fspec.json_path;

    RedisModuleString* rms_key = RedisModule_CreateString(ctx, key_name.c_str(), key_name.size());
    RedisModuleString* rms_path = RedisModule_CreateString(ctx, path.c_str(), path.size());
    RedisModuleCallReply* reply = RedisModule_Call(ctx, "JSON.GET", "ss", rms_key, rms_path);

    if (!reply || RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_STRING)
      continue;

    size_t len = 0;
    const char* data = RedisModule_CallReplyStringPtr(reply, &len);
    if (!data || len == 0) continue;
    std::string raw(data, len);

    if (fspec.type == FieldType::kTag) {
      auto vals = ExtractJsonArray(raw);
      for (auto& v : vals) {
        if (!v.empty()) {
          if (doc_fields.find(fspec.name) == doc_fields.end())
            doc_fields[fspec.name] = v;
          else
            doc_fields[fspec.name] += "," + v;
        }
      }
    } else {
      doc_fields[fspec.name] = ExtractJsonScalar(raw);
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
    if (pd->entry->spec.source_type == IndexSourceType::kJson)
      IndexJsonKey(pd->ctx, *pd->entry, key_str);
    else
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
  bool is_json_write = (strcmp(event, "json.set") == 0 || strcmp(event, "json.del") == 0 ||
                        strcmp(event, "json.arrappend") == 0 || strcmp(event, "json.arrinsert") == 0 ||
                        strcmp(event, "json.numincrby") == 0 || strcmp(event, "json.strappend") == 0);
  bool is_json_del = (strcmp(event, "json.del") == 0);

  for (auto& [idx_name, entry] : g_indices) {
    if (!entry.spec.HasPrefixes()) continue;
    if (!entry.spec.MatchesPrefix(key_str)) continue;

    if (is_del) {
      RemoveHashKey(entry, key_str);
    } else if (entry.spec.source_type == IndexSourceType::kJson) {
      if (is_json_del) {
        RemoveHashKey(entry, key_str);
      } else if (is_json_write) {
        IndexJsonKey(ctx, entry, key_str);
      }
    } else if (is_hash_write) {
      IndexHashKey(ctx, entry, key_str);
    }
  }

  return REDISMODULE_OK;
}

// =============================================================
// FT.AGGREGATE pipeline
// =============================================================

enum class ReduceFunc {
  kCount, kSum, kAvg, kMin, kMax, kCountDistinct,
  kStddev, kQuantile, kTolist, kFirstValue, kRandomSample, kCountDistinctish
};

struct ReduceOp {
  ReduceFunc func;
  std::string field;
  std::string alias;
  double quantile_pct = 0.0;
  std::string first_value_by;
  bool first_value_desc = false;
  long random_sample_count = 0;
};

enum class PipelineStageType {
  kLoad, kApply, kFilter, kGroupby, kSortby, kLimit
};

struct PipelineStage {
  PipelineStageType type;
  // LOAD
  std::vector<std::string> load_fields;
  // APPLY
  std::string apply_expr;
  std::string apply_alias;
  // FILTER
  std::string filter_expr;
  // GROUPBY + REDUCE
  std::vector<std::string> groupby_fields;
  std::vector<ReduceOp> reducers;
  // SORTBY
  std::string sortby_field;
  bool sortby_desc = false;
  // LIMIT
  long long limit_offset = 0;
  long long limit_count = 0;
};

using AggRow = std::unordered_map<std::string, std::string>;

static void ApplyLimit(std::vector<AggRow>& rows, long long offset, long long count) {
  long long total = static_cast<long long>(rows.size());
  if (offset >= total) { rows.clear(); return; }
  auto begin = rows.begin() + offset;
  auto end = (count >= 0 && offset + count < total)
                 ? rows.begin() + offset + count : rows.end();
  rows = std::vector<AggRow>(begin, end);
}

static void ApplySortby(std::vector<AggRow>& rows,
                        const std::string& field, bool desc) {
  std::sort(rows.begin(), rows.end(),
            [&](const AggRow& a, const AggRow& b) {
              auto ait = a.find(field);
              auto bit = b.find(field);
              std::string va = (ait != a.end()) ? ait->second : "";
              std::string vb = (bit != b.end()) ? bit->second : "";
              double na = 0, nb = 0;
              bool num = TryParseDouble(va, na) && TryParseDouble(vb, nb);
              if (num) return desc ? na > nb : na < nb;
              return desc ? va > vb : va < vb;
            });
}

static void ApplyGroupby(std::vector<AggRow>& rows,
                          const std::vector<std::string>& gb_fields,
                          const std::vector<ReduceOp>& reducers) {
  struct GroupAccum {
    long long count = 0;
    std::unordered_map<std::string, double> sum;
    std::unordered_map<std::string, double> sum_sq;
    std::unordered_map<std::string, long long> field_count;
    std::unordered_map<std::string, double> min_val;
    std::unordered_map<std::string, double> max_val;
    std::unordered_map<std::string, std::unordered_set<std::string>> distinct;
    std::unordered_map<std::string, std::vector<double>> values;
    std::unordered_map<std::string, std::vector<std::string>> str_values;
    std::unordered_map<std::string, std::string> first_val;
    std::unordered_map<std::string, double> first_sort_val;
    std::unordered_map<std::string, bool> first_set;
  };

  auto MakeKey = [&](const AggRow& row) -> std::string {
    std::string key;
    for (size_t i = 0; i < gb_fields.size(); i++) {
      if (i > 0) key += "\x01";
      auto fit = row.find(gb_fields[i]);
      if (fit != row.end()) key += fit->second;
    }
    return key;
  };

  std::unordered_map<std::string, GroupAccum> groups;
  std::unordered_map<std::string, AggRow> group_key_values;

  for (auto& row : rows) {
    std::string gk = MakeKey(row);
    auto& accum = groups[gk];
    accum.count++;

    if (group_key_values.find(gk) == group_key_values.end()) {
      AggRow kv;
      for (auto& gf : gb_fields) {
        auto fit = row.find(gf);
        kv[gf] = (fit != row.end()) ? fit->second : "";
      }
      group_key_values[gk] = std::move(kv);
    }

    for (auto& r : reducers) {
      if (r.func == ReduceFunc::kCount) continue;

      auto fit = row.find(r.field);
      if (fit == row.end()) continue;
      const auto& fval = fit->second;

      if (r.func == ReduceFunc::kCountDistinct || r.func == ReduceFunc::kCountDistinctish) {
        accum.distinct[r.alias].insert(fval);
        continue;
      }
      if (r.func == ReduceFunc::kTolist) {
        accum.str_values[r.alias].push_back(fval);
        continue;
      }
      if (r.func == ReduceFunc::kFirstValue) {
        if (!r.first_value_by.empty()) {
          auto sort_it = row.find(r.first_value_by);
          double sv = 0;
          if (sort_it != row.end()) TryParseDouble(sort_it->second, sv);
          if (!accum.first_set[r.alias] ||
              (r.first_value_desc ? sv > accum.first_sort_val[r.alias]
                                  : sv < accum.first_sort_val[r.alias])) {
            accum.first_val[r.alias] = fval;
            accum.first_sort_val[r.alias] = sv;
            accum.first_set[r.alias] = true;
          }
        } else if (!accum.first_set[r.alias]) {
          accum.first_val[r.alias] = fval;
          accum.first_set[r.alias] = true;
        }
        continue;
      }
      if (r.func == ReduceFunc::kRandomSample) {
        accum.str_values[r.alias].push_back(fval);
        continue;
      }

      double val = 0;
      if (!TryParseDouble(fval, val)) continue;

      accum.sum[r.alias] += val;
      accum.sum_sq[r.alias] += val * val;
      accum.field_count[r.alias]++;

      if (r.func == ReduceFunc::kQuantile) {
        accum.values[r.alias].push_back(val);
      }

      auto min_it = accum.min_val.find(r.alias);
      if (min_it == accum.min_val.end() || val < min_it->second)
        accum.min_val[r.alias] = val;
      auto max_it = accum.max_val.find(r.alias);
      if (max_it == accum.max_val.end() || val > max_it->second)
        accum.max_val[r.alias] = val;
    }
  }

  std::vector<AggRow> result;
  result.reserve(groups.size());
  for (auto& [gk, accum] : groups) {
    AggRow row = group_key_values[gk];
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
          if (sit != accum.sum.end() && cit != accum.field_count.end() && cit->second > 0)
            row[r.alias] = std::to_string(sit->second / static_cast<double>(cit->second));
          else
            row[r.alias] = "0";
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
        case ReduceFunc::kCountDistinct:
        case ReduceFunc::kCountDistinctish: {
          auto dit = accum.distinct.find(r.alias);
          long long dc = (dit != accum.distinct.end()) ? static_cast<long long>(dit->second.size()) : 0;
          row[r.alias] = std::to_string(dc);
          break;
        }
        case ReduceFunc::kStddev: {
          auto sit = accum.sum.find(r.alias);
          auto sqit = accum.sum_sq.find(r.alias);
          auto cit = accum.field_count.find(r.alias);
          if (sit != accum.sum.end() && sqit != accum.sum_sq.end() &&
              cit != accum.field_count.end() && cit->second > 1) {
            double n = static_cast<double>(cit->second);
            double mean = sit->second / n;
            double variance = (sqit->second / n) - (mean * mean);
            if (variance < 0) variance = 0;
            row[r.alias] = std::to_string(std::sqrt(variance));
          } else {
            row[r.alias] = "0";
          }
          break;
        }
        case ReduceFunc::kQuantile: {
          auto vit = accum.values.find(r.alias);
          if (vit != accum.values.end() && !vit->second.empty()) {
            auto& vals = vit->second;
            std::sort(vals.begin(), vals.end());
            size_t idx = static_cast<size_t>(r.quantile_pct * static_cast<double>(vals.size() - 1));
            if (idx >= vals.size()) idx = vals.size() - 1;
            row[r.alias] = std::to_string(vals[idx]);
          } else {
            row[r.alias] = "0";
          }
          break;
        }
        case ReduceFunc::kTolist: {
          auto lit = accum.str_values.find(r.alias);
          if (lit != accum.str_values.end()) {
            std::string joined;
            for (size_t i = 0; i < lit->second.size(); i++) {
              if (i > 0) joined += ",";
              joined += lit->second[i];
            }
            row[r.alias] = std::move(joined);
          } else {
            row[r.alias] = "";
          }
          break;
        }
        case ReduceFunc::kFirstValue: {
          auto fit = accum.first_val.find(r.alias);
          row[r.alias] = (fit != accum.first_val.end()) ? fit->second : "";
          break;
        }
        case ReduceFunc::kRandomSample: {
          auto lit = accum.str_values.find(r.alias);
          if (lit != accum.str_values.end() && !lit->second.empty()) {
            long count = r.random_sample_count;
            auto& vals = lit->second;
            // Deterministic sampling: take evenly spaced elements
            std::string sampled;
            for (long si = 0; si < count && si < static_cast<long>(vals.size()); si++) {
              size_t idx = static_cast<size_t>(
                  si * static_cast<long>(vals.size()) / count);
              if (si > 0) sampled += ",";
              sampled += vals[idx];
            }
            row[r.alias] = std::move(sampled);
          } else {
            row[r.alias] = "";
          }
          break;
        }
      }
    }
    result.push_back(std::move(row));
  }
  rows = std::move(result);
}

static int FtAggregateCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                              int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
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
                    entry.text_indices, entry.geo_indices, eval_error);
  if (!eval_error.empty()) {
    return RedisModule_ReplyWithError(ctx, eval_error.c_str());
  }

  // Parse pipeline stages
  std::vector<PipelineStage> stages;
  PipelineStage* pending_groupby = nullptr;

  int arg_i = 3;
  while (arg_i < argc) {
    auto kw = ArgView(argv[arg_i]);

    if (MatchArg(kw, "LOAD")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR LOAD requires count");
      char* endptr = nullptr;
      long ld_count = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || ld_count < 0)
        return RedisModule_ReplyWithError(ctx, "ERR LOAD count must be non-negative");
      PipelineStage st;
      st.type = PipelineStageType::kLoad;
      for (long li = 0; li < ld_count; li++) {
        if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR not enough LOAD fields");
        auto fv = ArgView(argv[arg_i]);
        arg_i++;
        if (fv.empty() || fv[0] != '@')
          return RedisModule_ReplyWithError(ctx, "ERR LOAD field must start with @");
        st.load_fields.emplace_back(fv.substr(1));
      }
      stages.push_back(std::move(st));
    } else if (MatchArg(kw, "APPLY")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR APPLY requires expression");
      std::string expr(ArgView(argv[arg_i]));
      arg_i++;
      if (arg_i >= argc || !MatchArg(ArgView(argv[arg_i]), "AS"))
        return RedisModule_ReplyWithError(ctx, "ERR APPLY requires AS alias");
      arg_i++;
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR APPLY AS requires alias name");
      std::string alias(ArgView(argv[arg_i]));
      arg_i++;
      PipelineStage st;
      st.type = PipelineStageType::kApply;
      st.apply_expr = std::move(expr);
      st.apply_alias = std::move(alias);
      stages.push_back(std::move(st));
    } else if (MatchArg(kw, "FILTER")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR FILTER requires expression");
      PipelineStage st;
      st.type = PipelineStageType::kFilter;
      st.filter_expr = std::string(ArgView(argv[arg_i]));
      arg_i++;
      stages.push_back(std::move(st));
    } else if (MatchArg(kw, "GROUPBY")) {
      arg_i++;
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR GROUPBY requires count");
      char* endptr = nullptr;
      long gb_count = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || gb_count < 0)
        return RedisModule_ReplyWithError(ctx, "ERR GROUPBY count must be non-negative");
      PipelineStage st;
      st.type = PipelineStageType::kGroupby;
      for (long g = 0; g < gb_count; g++) {
        if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR not enough GROUPBY fields");
        auto fv = ArgView(argv[arg_i]);
        arg_i++;
        if (fv.empty() || fv[0] != '@')
          return RedisModule_ReplyWithError(ctx, "ERR GROUPBY field must start with @");
        st.groupby_fields.emplace_back(fv.substr(1));
      }
      stages.push_back(std::move(st));
      pending_groupby = &stages.back();
    } else if (MatchArg(kw, "REDUCE")) {
      arg_i++;
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR REDUCE requires function name");
      auto func_str = ArgView(argv[arg_i]);
      arg_i++;

      ReduceOp rop;
      bool needs_field = true;
      if (MatchArg(func_str, "COUNT")) {
        rop.func = ReduceFunc::kCount; needs_field = false;
      } else if (MatchArg(func_str, "SUM")) {
        rop.func = ReduceFunc::kSum;
      } else if (MatchArg(func_str, "AVG")) {
        rop.func = ReduceFunc::kAvg;
      } else if (MatchArg(func_str, "MIN")) {
        rop.func = ReduceFunc::kMin;
      } else if (MatchArg(func_str, "MAX")) {
        rop.func = ReduceFunc::kMax;
      } else if (MatchArg(func_str, "COUNT_DISTINCT")) {
        rop.func = ReduceFunc::kCountDistinct;
      } else if (MatchArg(func_str, "STDDEV")) {
        rop.func = ReduceFunc::kStddev;
      } else if (MatchArg(func_str, "QUANTILE")) {
        rop.func = ReduceFunc::kQuantile;
      } else if (MatchArg(func_str, "TOLIST")) {
        rop.func = ReduceFunc::kTolist;
      } else if (MatchArg(func_str, "FIRST_VALUE")) {
        rop.func = ReduceFunc::kFirstValue;
      } else if (MatchArg(func_str, "RANDOM_SAMPLE")) {
        rop.func = ReduceFunc::kRandomSample;
      } else if (MatchArg(func_str, "COUNT_DISTINCTISH")) {
        rop.func = ReduceFunc::kCountDistinctish; needs_field = true;
      } else {
        return RedisModule_ReplyWithError(ctx, "ERR unknown REDUCE function");
      }

      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR REDUCE requires nargs");
      char* endptr = nullptr;
      long nargs = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || nargs < 0)
        return RedisModule_ReplyWithError(ctx, "ERR REDUCE nargs must be non-negative");

      long consumed = 0;
      if (needs_field && nargs >= 1) {
        if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR REDUCE missing field argument");
        auto rf = ArgView(argv[arg_i]);
        arg_i++; consumed++;
        if (rf.empty() || rf[0] != '@')
          return RedisModule_ReplyWithError(ctx, "ERR REDUCE field must start with @");
        rop.field = std::string(rf.substr(1));
      }

      // Parse extra args for specific reducers
      if (rop.func == ReduceFunc::kQuantile && consumed < nargs) {
        if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR QUANTILE requires percentile");
        char* ep = nullptr;
        rop.quantile_pct = std::strtod(std::string(ArgView(argv[arg_i])).c_str(), &ep);
        arg_i++; consumed++;
      }
      if (rop.func == ReduceFunc::kFirstValue && consumed < nargs) {
        if (arg_i < argc && MatchArg(ArgView(argv[arg_i]), "BY")) {
          arg_i++; consumed++;
          if (arg_i < argc && consumed < nargs) {
            auto by_f = ArgView(argv[arg_i]);
            if (!by_f.empty() && by_f[0] == '@')
              rop.first_value_by = std::string(by_f.substr(1));
            arg_i++; consumed++;
          }
          if (arg_i < argc && consumed < nargs) {
            auto dir = ArgView(argv[arg_i]);
            if (MatchArg(dir, "DESC")) { rop.first_value_desc = true; arg_i++; consumed++; }
            else if (MatchArg(dir, "ASC")) { arg_i++; consumed++; }
          }
        }
      }
      if (rop.func == ReduceFunc::kRandomSample && consumed < nargs) {
        if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR RANDOM_SAMPLE requires count");
        char* ep = nullptr;
        rop.random_sample_count = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &ep, 10);
        arg_i++; consumed++;
      }

      // Skip any remaining args
      for (long skip = consumed; skip < nargs; skip++) {
        if (arg_i < argc) arg_i++;
      }

      if (arg_i >= argc || !MatchArg(ArgView(argv[arg_i]), "AS"))
        return RedisModule_ReplyWithError(ctx, "ERR REDUCE requires AS alias");
      arg_i++;
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR REDUCE AS requires alias name");
      rop.alias = std::string(ArgView(argv[arg_i]));
      arg_i++;

      if (pending_groupby) {
        pending_groupby->reducers.push_back(std::move(rop));
      } else {
        // Implicit empty GROUPBY
        PipelineStage st;
        st.type = PipelineStageType::kGroupby;
        st.reducers.push_back(std::move(rop));
        stages.push_back(std::move(st));
        pending_groupby = &stages.back();
      }
    } else if (MatchArg(kw, "SORTBY")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR SORTBY requires count");
      char* endptr = nullptr;
      long sb_count = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || sb_count < 2)
        return RedisModule_ReplyWithError(ctx, "ERR SORTBY count must be >= 2");
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR SORTBY requires field");
      auto sf = ArgView(argv[arg_i]);
      arg_i++;
      if (sf.empty() || sf[0] != '@')
        return RedisModule_ReplyWithError(ctx, "ERR SORTBY field must start with @");
      PipelineStage st;
      st.type = PipelineStageType::kSortby;
      st.sortby_field = std::string(sf.substr(1));
      if (arg_i < argc) {
        auto dir = ArgView(argv[arg_i]);
        if (MatchArg(dir, "ASC")) { st.sortby_desc = false; arg_i++; }
        else if (MatchArg(dir, "DESC")) { st.sortby_desc = true; arg_i++; }
      }
      for (long skip = 2; skip < sb_count; skip++) {
        if (arg_i < argc) arg_i++;
      }
      stages.push_back(std::move(st));
    } else if (MatchArg(kw, "LIMIT")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i + 1 >= argc)
        return RedisModule_ReplyWithError(ctx, "ERR LIMIT requires offset and count");
      char* ep1 = nullptr;
      char* ep2 = nullptr;
      std::string off_s(ArgView(argv[arg_i]));
      std::string cnt_s(ArgView(argv[arg_i + 1]));
      long long off = std::strtoll(off_s.c_str(), &ep1, 10);
      long long cnt = std::strtoll(cnt_s.c_str(), &ep2, 10);
      if (*ep1 != '\0' || *ep2 != '\0' || off < 0 || cnt < 0)
        return RedisModule_ReplyWithError(ctx, "ERR LIMIT offset and count must be non-negative");
      PipelineStage st;
      st.type = PipelineStageType::kLimit;
      st.limit_offset = off;
      st.limit_count = cnt;
      stages.push_back(std::move(st));
      arg_i += 2;
    } else {
      return RedisModule_ReplyWithError(ctx, "ERR unknown AGGREGATE option");
    }
  }

  // Build initial rows from query results
  std::vector<AggRow> rows;
  rows.reserve(result_ids.size());
  for (auto& doc_id : result_ids) {
    const auto* doc = entry.doc_store.Get(doc_id);
    if (!doc) continue;
    rows.push_back(doc->fields);
  }

  // Execute pipeline stages
  for (auto& stage : stages) {
    switch (stage.type) {
      case PipelineStageType::kLoad:
        // LOAD adds fields from document store (for rows that still reference docs)
        // In our model, rows already contain all doc fields from initial load
        // LOAD is a no-op for additional fields already in the row
        break;

      case PipelineStageType::kApply: {
        ExprNode expr;
        std::string expr_err;
        if (!ParseExpr(stage.apply_expr, expr, expr_err)) {
          return RedisModule_ReplyWithError(ctx, expr_err.c_str());
        }
        for (auto& row : rows) {
          auto val = EvalExpr(expr, row);
          row[stage.apply_alias] = val.AsString();
        }
        break;
      }

      case PipelineStageType::kFilter: {
        ExprNode expr;
        std::string expr_err;
        if (!ParseExpr(stage.filter_expr, expr, expr_err)) {
          return RedisModule_ReplyWithError(ctx, expr_err.c_str());
        }
        rows.erase(
            std::remove_if(rows.begin(), rows.end(),
                           [&](const AggRow& row) {
                             return !EvalExpr(expr, row).Truthy();
                           }),
            rows.end());
        break;
      }

      case PipelineStageType::kGroupby:
        if (stage.groupby_fields.empty() && stage.reducers.empty()) break;
        ApplyGroupby(rows, stage.groupby_fields, stage.reducers);
        break;

      case PipelineStageType::kSortby:
        ApplySortby(rows, stage.sortby_field, stage.sortby_desc);
        break;

      case PipelineStageType::kLimit:
        ApplyLimit(rows, stage.limit_offset, stage.limit_count);
        break;
    }
  }

  // Reply
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
      {"FT.ALTER", FtAlterCommand, "write"},
      {"FT.ALIASADD", FtAliasAddCommand, "write"},
      {"FT.ALIASDEL", FtAliasDelCommand, "write"},
      {"FT.ALIASUPDATE", FtAliasUpdateCommand, "write"},
      {"FT.TAGVALS", FtTagvalsCommand, "readonly"},
      {"FT.EXPLAIN", FtExplainCommand, "readonly"},
      {"FT.EXPLAINCLI", FtExplainCliCommand, "readonly"},
      {"FT._DEBUG", FtDebugCommand, "readonly"},
  };

  for (auto& cmd : commands) {
    if (RedisModule_CreateCommand(ctx, cmd.name, cmd.handler, cmd.flags, 0, 0,
                                  0) == REDISMODULE_ERR) {
      return REDISMODULE_ERR;
    }
  }

  if (RedisModule_SubscribeToKeyspaceEvents(
          ctx,
          REDISMODULE_NOTIFY_HASH | REDISMODULE_NOTIFY_GENERIC |
          REDISMODULE_NOTIFY_MODULE,
          OnKeyspaceEvent) == REDISMODULE_ERR) {
    RedisModule_Log(ctx, "warning", "Failed to subscribe to keyspace events");
  }

  return REDISMODULE_OK;
}
