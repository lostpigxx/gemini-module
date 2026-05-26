#include "search_commands.h"
#include "document_store.h"
#include "index_spec.h"
#include "numeric_index.h"
#include "tag_index.h"
#include "tag_query.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct IndexEntry {
  IndexSpec spec;
  DocumentStore doc_store;
  TagFieldIndices tag_indices;
  NumericFieldIndices numeric_indices;
};

static std::unordered_map<std::string, IndexEntry> g_indices;

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

static bool MatchArg(std::string_view arg, const char* target) {
  if (arg.size() != strlen(target)) return false;
  return strncasecmp(arg.data(), target, arg.size()) == 0;
}

static void RemoveDocFromIndices(IndexEntry& entry, const std::string& doc_id) {
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
    }
  }
}

static void AddDocToIndices(IndexEntry& entry, const std::string& doc_id,
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
    }
  }
}

// FT.CREATE <index_name> SCHEMA <field_name> <field_type> [...]
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

  int field_argc = argc - 3;
  if (field_argc < 2 || field_argc % 2 != 0) {
    return RedisModule_ReplyWithError(
        ctx, "ERR syntax error, SCHEMA requires pairs of <field> <type>");
  }

  std::vector<FieldSpec> fields;
  for (int i = 3; i < argc; i += 2) {
    auto fname = ArgView(argv[i]);
    auto ftype_str = ArgView(argv[i + 1]);

    FieldType ftype;
    if (MatchArg(ftype_str, "TAG")) {
      ftype = FieldType::kTag;
    } else if (MatchArg(ftype_str, "NUMERIC")) {
      ftype = FieldType::kNumeric;
    } else {
      return RedisModule_ReplyWithError(
          ctx, "ERR unknown field type, expected TAG or NUMERIC");
    }

    for (auto& existing : fields) {
      if (existing.name == fname) {
        return RedisModule_ReplyWithError(ctx,
                                          "ERR duplicate field name in schema");
      }
    }

    fields.push_back({std::string(fname), ftype});
  }

  std::string idx_str(index_name);
  auto [it, inserted] = g_indices.try_emplace(idx_str);
  if (!inserted) {
    return RedisModule_ReplyWithError(ctx, "ERR index already exists");
  }
  it->second.spec = IndexSpec{idx_str, std::move(fields)};

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
  RedisModule_ReplyWithLongLong(ctx,
                                 static_cast<long long>(entry.doc_store.Size()));

  RedisModule_ReplyWithSimpleString(ctx, "fields");
  RedisModule_ReplyWithArray(ctx, static_cast<long>(entry.spec.fields.size()));
  for (auto& f : entry.spec.fields) {
    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithCString(ctx, f.name.c_str());
    RedisModule_ReplyWithSimpleString(ctx, FieldTypeName(f.type));
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
    std::string fval_str(ArgView(argv[i + 1]));

    if (!entry.spec.FindField(fname_str)) {
      return RedisModule_ReplyWithError(ctx, "ERR field not in schema");
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

// FT.SEARCH <index> <query>
static int FtSearchCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str(ArgView(argv[1]));
  auto it = g_indices.find(idx_str);
  if (it == g_indices.end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  auto& entry = it->second;

  TagQuery query;
  std::string parse_error;
  if (!ParseTagQuery(std::string(ArgView(argv[2])), query, parse_error)) {
    return RedisModule_ReplyWithError(ctx, parse_error.c_str());
  }

  std::vector<std::string> result_ids;

  if (query.type == TagQuery::Type::kMatchAll) {
    result_ids = entry.doc_store.AllIds();
  } else if (query.type == TagQuery::Type::kTagMatch) {
    const auto* fspec = entry.spec.FindField(query.field_name);
    if (!fspec) {
      return RedisModule_ReplyWithError(ctx, "ERR query field not in schema");
    }
    if (fspec->type != FieldType::kTag) {
      return RedisModule_ReplyWithError(ctx, "ERR field is not a TAG field");
    }

    const auto* tag_idx = entry.tag_indices.Get(query.field_name);
    if (tag_idx) {
      if (query.tag_values.size() == 1) {
        result_ids = tag_idx->Lookup(query.tag_values[0]);
      } else {
        result_ids = tag_idx->LookupOr(query.tag_values);
      }
    }
  } else if (query.type == TagQuery::Type::kNumericRange) {
    const auto* fspec = entry.spec.FindField(query.field_name);
    if (!fspec) {
      return RedisModule_ReplyWithError(ctx, "ERR query field not in schema");
    }
    if (fspec->type != FieldType::kNumeric) {
      return RedisModule_ReplyWithError(ctx,
                                        "ERR field is not a NUMERIC field");
    }

    const auto* num_idx = entry.numeric_indices.Get(query.field_name);
    if (num_idx) {
      result_ids = num_idx->RangeQuery(query.range_min, query.min_exclusive,
                                        query.range_max, query.max_exclusive);
    }
  }

  long total = static_cast<long>(result_ids.size());
  RedisModule_ReplyWithArray(ctx, 1 + total * 2);
  RedisModule_ReplyWithLongLong(ctx, total);

  for (auto& rid : result_ids) {
    RedisModule_ReplyWithCString(ctx, rid.c_str());
    const auto* doc = entry.doc_store.Get(rid);
    if (doc) {
      RedisModule_ReplyWithArray(ctx,
                                 static_cast<long>(doc->fields.size() * 2));
      std::vector<std::string> sorted_keys;
      sorted_keys.reserve(doc->fields.size());
      for (auto& [k, v] : doc->fields) {
        sorted_keys.push_back(k);
      }
      std::sort(sorted_keys.begin(), sorted_keys.end());
      for (auto& k : sorted_keys) {
        RedisModule_ReplyWithCString(ctx, k.c_str());
        RedisModule_ReplyWithCString(ctx, doc->fields.at(k).c_str());
      }
    } else {
      RedisModule_ReplyWithEmptyArray(ctx);
    }
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
