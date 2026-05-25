#include "search_commands.h"
#include "index_spec.h"

#include <cstring>
#include <string_view>

static IndexRegistry g_registry;

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

  if (!g_registry.Create(std::string(index_name), std::move(fields))) {
    return RedisModule_ReplyWithError(ctx, "ERR index already exists");
  }

  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.DROPINDEX <index_name>
static int FtDropIndexCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                              int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto index_name = ArgView(argv[1]);

  if (!g_registry.Drop(std::string(index_name))) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }

  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.INFO <index_name>
static int FtInfoCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                         int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto index_name = ArgView(argv[1]);
  const auto* spec = g_registry.Get(std::string(index_name));
  if (!spec) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }

  RedisModule_ReplyWithArray(ctx, 4);

  RedisModule_ReplyWithSimpleString(ctx, "index_name");
  RedisModule_ReplyWithCString(ctx, spec->name.c_str());

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
