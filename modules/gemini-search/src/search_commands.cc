#include "search_commands.h"
#include "document_store.h"
#include "index_spec.h"
#include "tag_index.h"
#include "tag_query.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

static IndexRegistry g_registry;

struct IndexData {
  DocumentStore doc_store;
  TagFieldIndices tag_indices;
};

static std::unordered_map<std::string, IndexData> g_index_data;

IndexRegistry& GetGlobalRegistry() { return g_registry; }

static std::string_view ArgView(RedisModuleString* s) {
  size_t len;
  const char* data = RedisModule_StringPtrLen(s, &len);
  return {data, len};
}

static bool MatchArg(std::string_view arg, const char* target) {
  if (arg.size() != strlen(target)) return false;
  return strncasecmp(arg.data(), target, arg.size()) == 0;
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
  if (!g_registry.Create(idx_str, std::move(fields))) {
    return RedisModule_ReplyWithError(ctx, "ERR index already exists");
  }

  g_index_data.emplace(idx_str, IndexData{});

  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.DROPINDEX <index_name>
static int FtDropIndexCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                              int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto index_name = ArgView(argv[1]);
  std::string idx_str(index_name);

  if (!g_registry.Drop(idx_str)) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }

  g_index_data.erase(idx_str);

  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.INFO <index_name>
static int FtInfoCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                         int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto index_name = ArgView(argv[1]);
  std::string idx_str(index_name);
  const auto* spec = g_registry.Get(idx_str);
  if (!spec) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }

  RedisModule_ReplyWithArray(ctx, 6);

  RedisModule_ReplyWithSimpleString(ctx, "index_name");
  RedisModule_ReplyWithCString(ctx, spec->name.c_str());

  RedisModule_ReplyWithSimpleString(ctx, "num_docs");
  auto dit = g_index_data.find(idx_str);
  long long doc_count =
      dit != g_index_data.end()
          ? static_cast<long long>(dit->second.doc_store.Size())
          : 0;
  RedisModule_ReplyWithLongLong(ctx, doc_count);

  RedisModule_ReplyWithSimpleString(ctx, "fields");
  RedisModule_ReplyWithArray(ctx, static_cast<long>(spec->fields.size()));
  for (auto& f : spec->fields) {
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

  auto names = g_registry.List();
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

  auto index_name = ArgView(argv[1]);
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

  std::string idx_str(index_name);
  const auto* spec = g_registry.Get(idx_str);
  if (!spec) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }

  auto dit = g_index_data.find(idx_str);
  if (dit == g_index_data.end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index data not found");
  }
  auto& data = dit->second;

  std::unordered_map<std::string, std::string> doc_fields;
  for (int i = 4; i < argc; i += 2) {
    auto fname = ArgView(argv[i]);
    auto fval = ArgView(argv[i + 1]);
    std::string fname_str(fname);

    if (!spec->FindField(fname_str)) {
      return RedisModule_ReplyWithError(ctx, "ERR field not in schema");
    }

    doc_fields[fname_str] = std::string(fval);
  }

  std::string doc_id_str(doc_id);

  const auto* old_doc = data.doc_store.Get(doc_id_str);
  if (old_doc) {
    for (auto& [fname, fval] : old_doc->fields) {
      const auto* fspec = spec->FindField(fname);
      if (fspec && fspec->type == FieldType::kTag) {
        data.tag_indices.GetOrCreate(fname).Remove(fval, doc_id_str);
      }
    }
  }

  data.doc_store.Add(doc_id_str, doc_fields);

  for (auto& [fname, fval] : doc_fields) {
    const auto* fspec = spec->FindField(fname);
    if (fspec && fspec->type == FieldType::kTag) {
      data.tag_indices.GetOrCreate(fname).Add(fval, doc_id_str);
    }
  }

  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.DEL <index> <doc_id>
static int FtDelCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                        int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto index_name = ArgView(argv[1]);
  auto doc_id = ArgView(argv[2]);

  std::string idx_str(index_name);
  const auto* spec = g_registry.Get(idx_str);
  if (!spec) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }

  auto dit = g_index_data.find(idx_str);
  if (dit == g_index_data.end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  auto& data = dit->second;

  std::string doc_id_str(doc_id);
  const auto* doc = data.doc_store.Get(doc_id_str);
  if (!doc) {
    return RedisModule_ReplyWithError(ctx, "ERR document not found");
  }

  for (auto& [fname, fval] : doc->fields) {
    const auto* fspec = spec->FindField(fname);
    if (fspec && fspec->type == FieldType::kTag) {
      data.tag_indices.GetOrCreate(fname).Remove(fval, doc_id_str);
    }
  }

  data.doc_store.Remove(doc_id_str);

  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.SEARCH <index> <query>
static int FtSearchCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto index_name = ArgView(argv[1]);
  auto query_str = ArgView(argv[2]);

  std::string idx_str(index_name);
  const auto* spec = g_registry.Get(idx_str);
  if (!spec) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }

  auto dit = g_index_data.find(idx_str);
  if (dit == g_index_data.end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  auto& data = dit->second;

  TagQuery query;
  std::string parse_error;
  if (!ParseTagQuery(std::string(query_str), query, parse_error)) {
    return RedisModule_ReplyWithError(ctx, parse_error.c_str());
  }

  std::vector<std::string> result_ids;

  if (query.type == TagQuery::Type::kMatchAll) {
    result_ids = data.doc_store.AllIds();
  } else {
    const auto* fspec = spec->FindField(query.field_name);
    if (!fspec) {
      return RedisModule_ReplyWithError(ctx, "ERR query field not in schema");
    }
    if (fspec->type != FieldType::kTag) {
      return RedisModule_ReplyWithError(ctx, "ERR field is not a TAG field");
    }

    const auto* tag_idx = data.tag_indices.Get(query.field_name);
    if (tag_idx) {
      if (query.tag_values.size() == 1) {
        result_ids = tag_idx->Lookup(query.tag_values[0]);
      } else {
        result_ids = tag_idx->LookupOr(query.tag_values);
      }
    }
  }

  long total = static_cast<long>(result_ids.size());
  RedisModule_ReplyWithArray(ctx, 1 + total * 2);
  RedisModule_ReplyWithLongLong(ctx, total);

  for (auto& rid : result_ids) {
    RedisModule_ReplyWithCString(ctx, rid.c_str());
    const auto* doc = data.doc_store.Get(rid);
    if (doc) {
      RedisModule_ReplyWithArray(ctx,
                                 static_cast<long>(doc->fields.size() * 2));
      // Sort field names for deterministic output
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
