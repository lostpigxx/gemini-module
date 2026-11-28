#include "cluster_topology.h"
#include "search_internal.h"
#include "search_rdb.h"

#include <pthread.h>

#include <mutex>
#include <vector>

namespace {

// BGSAVE fork safety: lock all global & per-index shared_mutexes exclusively across fork()
// so no SearchExecutor reader thread can leave an internal shared_mutex lock permanently
// wedged in the forked child process when AuxSaveSearch / RdbSaveSearch runs.
std::vector<std::shared_ptr<IndexEntry>> g_fork_locked_entries;

void SearchAtForkPrepare() {
  GetIndicesMutex().lock();
  GetAliasesMutex().lock();
  GetTermDictsMutex().lock();
  GetSuggestDictsMutex().lock();
  GetConfigMutex().lock();
  g_fork_locked_entries.clear();
  for (const auto& [name, entry_ptr] : GetIndices()) {
    if (entry_ptr) {
      entry_ptr->Mutex().lock();
      g_fork_locked_entries.push_back(entry_ptr);
    }
  }
}

void SearchAtForkParent() {
  for (auto it = g_fork_locked_entries.rbegin(); it != g_fork_locked_entries.rend(); ++it) {
    (*it)->Mutex().unlock();
  }
  g_fork_locked_entries.clear();
  GetConfigMutex().unlock();
  GetSuggestDictsMutex().unlock();
  GetTermDictsMutex().unlock();
  GetAliasesMutex().unlock();
  GetIndicesMutex().unlock();
}

void SearchAtForkChild() {
  // Do NOT unlock std::shared_mutex (pthread_rwlock_t) or free heap memory in the forked child:
  // unlocking a parent-owned rwlock in a child is UB and deadlocks on Linux glibc.
  // The forked child is strictly single-threaded and holds a consistent point-in-time snapshot
  // guaranteed by SearchAtForkPrepare(); RDB/AOF save paths bypass shared_mutex locks when
  // IsInForkedChild() is true.
  MarkForkedChild();
}

}  // namespace

static void MaintenanceTimerCallback(RedisModuleCtx* ctx, void* /*data*/) {
  int64_t now_ms = NowMs();

  // 1. Purge idle aggregation cursors
  PurgeExpiredCursors(now_ms);

  // 2. Drop expired TEMPORARY indices
  std::vector<std::string> expired;
  {
    std::shared_lock<std::shared_mutex> lk(GetIndicesMutex());
    for (const auto& [name, entry_ptr] : GetIndices()) {
      if (!entry_ptr) continue;
      const auto& entry = *entry_ptr;
      int64_t last_access = entry.LastAccessMs();
      if (entry.spec.temporary_ttl > 0 && last_access > 0 &&
          now_ms - last_access >=
              static_cast<int64_t>(entry.spec.temporary_ttl) * 1000LL) {
        expired.push_back(name);
      }
    }
  }
  for (const auto& name : expired) {
    StopBackgroundIndexing(ctx, name);
    {
      std::unique_lock<std::shared_mutex> lk(GetIndicesMutex());
      GetIndices().erase(name);
    }
    {
      std::unique_lock<std::shared_mutex> alk(GetAliasesMutex());
      auto& aliases = GetAliases();
      for (auto ait = aliases.begin(); ait != aliases.end(); ) {
        if (ait->second == name) ait = aliases.erase(ait);
        else ++ait;
      }
    }
    if (SearchModuleType && !gemini_search::ClusterManager::Instance().IsClusterMode()) {
      RedisModuleString* kstr = RedisModule_CreateString(ctx, name.data(), name.size());
      auto* key = static_cast<RedisModuleKey*>(
          RedisModule_OpenKey(ctx, kstr, REDISMODULE_READ | REDISMODULE_WRITE));
      if (key) {
        if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_MODULE &&
            RedisModule_ModuleTypeGetType(key) == SearchModuleType) {
          RedisModule_DeleteKey(key);
        }
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
      {"FT.HYBRID", FtHybridCommand, "readonly"},
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

  static std::once_flag atfork_once;
  std::call_once(atfork_once, []() {
    pthread_atfork(SearchAtForkPrepare, SearchAtForkParent, SearchAtForkChild);
  });

  if (RedisModule_CreateTimer) {
    RedisModule_CreateTimer(ctx, 1000, MaintenanceTimerCallback, nullptr);
  }

  return REDISMODULE_OK;
}
