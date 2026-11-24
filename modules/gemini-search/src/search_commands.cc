#include "search_internal.h"

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
      {"FT.CURSOR", FtCursorCommand, "readonly"},
      {"FT.SUGADD", FtSugAddCommand, "write deny-oom"},
      {"FT.SUGGET", FtSugGetCommand, "readonly"},
      {"FT.SUGDEL", FtSugDelCommand, "write"},
      {"FT.SUGLEN", FtSugLenCommand, "readonly"},
      {"FT.DICTADD", FtDictAddCommand, "write deny-oom"},
      {"FT.DICTDEL", FtDictDelCommand, "write"},
      {"FT.DICTDUMP", FtDictDumpCommand, "readonly"},
      {"FT.SYNUPDATE", FtSynUpdateCommand, "write"},
      {"FT.SYNDUMP", FtSynDumpCommand, "readonly"},
      {"FT.SPELLCHECK", FtSpellCheckCommand, "readonly"},
      {"FT.CONFIG", FtConfigCommand, "readonly"},
      {"FT.PROFILE", FtProfileCommand, "readonly"},
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
