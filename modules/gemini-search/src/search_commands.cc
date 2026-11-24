#include "search_internal.h"
#include "search_rdb.h"

#include <vector>

static void MaintenanceTimerCallback(RedisModuleCtx* ctx, void* /*data*/) {
  int64_t now_ms = NowMs();

  // 1. Purge idle aggregation cursors
  PurgeExpiredCursors(now_ms);

  // 2. Drop expired TEMPORARY indices
  std::vector<std::string> expired;
  for (const auto& [name, entry] : GetIndices()) {
    if (entry.spec.temporary_ttl > 0 && entry.last_access_ms > 0 &&
        now_ms - entry.last_access_ms >=
            static_cast<int64_t>(entry.spec.temporary_ttl) * 1000LL) {
      expired.push_back(name);
    }
  }
  for (const auto& name : expired) {
    GetIndices().erase(name);
    auto& aliases = GetAliases();
    for (auto ait = aliases.begin(); ait != aliases.end(); ) {
      if (ait->second == name) ait = aliases.erase(ait);
      else ++ait;
    }
    if (SearchModuleType) {
      RedisModuleString* kstr = RedisModule_CreateString(ctx, name.data(), name.size());
      auto* key = static_cast<RedisModuleKey*>(
          RedisModule_OpenKey(ctx, kstr, REDISMODULE_WRITE));
      if (key) {
        RedisModule_DeleteKey(key);
        RedisModule_CloseKey(key);
      }
      RedisModule_FreeString(ctx, kstr);
    }
  }

  if (RedisModule_CreateTimer) {
    RedisModule_CreateTimer(ctx, 1000, MaintenanceTimerCallback, nullptr);
  }
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
          REDISMODULE_NOTIFY_MODULE | REDISMODULE_NOTIFY_EXPIRED |
          REDISMODULE_NOTIFY_EVICTED,
          OnKeyspaceEvent) == REDISMODULE_ERR) {
    RedisModule_Log(ctx, "warning", "Failed to subscribe to keyspace events");
  }

  if (RedisModule_SubscribeToServerEvent) {
    RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_FlushDB, OnFlushDbEvent);
    RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_Loading, OnLoadingEvent);
  }

  if (RedisModule_CreateTimer) {
    RedisModule_CreateTimer(ctx, 1000, MaintenanceTimerCallback, nullptr);
  }

  return REDISMODULE_OK;
}
