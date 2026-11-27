#pragma once

#include "search_commands.h"
#include "search_executor.h"

#include <cstdint>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct AggCursor {
  std::string index_name;
  std::vector<std::unordered_map<std::string, std::string>> rows;
  size_t pos = 0;
  size_t batch_size = 1000;
  int64_t last_access_ms = 0;
  int64_t max_idle_ms = 300000;
};

constexpr size_t kMaxCursorsPerIndex = 128;

// Global state accessors (defined in search_state.cc)
std::unordered_map<std::string, std::shared_ptr<IndexEntry>>& GetIndices();
std::shared_mutex& GetIndicesMutex();
SearchExecutor& GetSearchExecutor();
std::unordered_map<uint64_t, AggCursor>& GetCursors();
uint64_t NextCursorId();
std::string ResolveAlias(const std::string& name);
int64_t NowMs();
void TouchIndex(IndexEntry& entry);
int GetDefaultDialect();
size_t GetMaxExpansions();
size_t GetMinStemLen();
size_t GetMaxSearchResults();
size_t GetAsyncMinDocs();
size_t GetBgIndexMinKeys();
size_t GetBgIndexBatchKeys();
uint64_t GetBgIndexTickMs();
bool GetClusterEnabledConfig();
int GetClusterRpcPortOffset();
uint64_t GetClusterQueryTimeoutMs();
size_t GetClusterMaxScatterLimit();
bool GetClusterBm25Dfs();
size_t GetClusterAggMaxGroups();
uint64_t NextIndexGeneration();
bool IsOnTimeoutReturn();
uint64_t GetTotalCursorsCreated();
uint64_t GetTotalCursorsExpired();
void PurgeExpiredCursors(int64_t now_ms);

// Shared argument & indexing helpers (defined in search_state.cc)
bool TryParseDouble(const std::string& s, double& out);
std::string_view ArgView(RedisModuleString* s);
std::string ArgStr(RedisModuleString* s);
bool MatchArg(std::string_view arg, const char* target);
void RemoveDocFromIndices(IndexEntry& entry, const std::string& doc_id);
void AddDocToIndices(IndexEntry& entry, const std::string& doc_id,
                     const std::unordered_map<std::string, std::string>& fields);
bool PassesIndexFilter(const IndexSpec& spec,
                       const std::unordered_map<std::string, std::string>& fields);

// Keyspace & server event helpers (defined in search_keyspace.cc)
void ScanExistingKeys(RedisModuleCtx* ctx, IndexEntry& entry);
void StartOrRunInitialScan(RedisModuleCtx* ctx, IndexEntry& entry);
void StopBackgroundIndexing(RedisModuleCtx* ctx, const std::string& index_name);
void StopAllBackgroundIndexing(RedisModuleCtx* ctx);
void IndexHashKey(RedisModuleCtx* ctx, IndexEntry& entry, const std::string& key_name);
void IndexJsonKey(RedisModuleCtx* ctx, IndexEntry& entry, const std::string& key_name);
int OnKeyspaceEvent(RedisModuleCtx* ctx, int type, const char* event, RedisModuleString* key);
void OnFlushDbEvent(RedisModuleCtx* ctx, RedisModuleEvent eid, uint64_t subevent, void* data);
void OnLoadingEvent(RedisModuleCtx* ctx, RedisModuleEvent eid, uint64_t subevent, void* data);

// Aggregation cursor & profile helpers (defined in search_agg_cmds.cc)
void ReplyCursorBatch(RedisModuleCtx* ctx, uint64_t cursor_id, AggCursor& cursor);
bool RunAggregateForProfile(
    IndexEntry& entry, const std::vector<std::string>& result_ids,
    RedisModuleString** argv, int start_arg, int argc,
    std::vector<std::unordered_map<std::string, std::string>>& out_rows,
    std::string& error_msg, int64_t deadline_ms = 0);

// Index management commands (search_index_cmds.cc)
int FtCreateCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtDropIndexCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtInfoCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtListCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtAddCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtDelCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtAlterCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtAliasAddCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtAliasDelCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtAliasUpdateCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtTagvalsCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);

// Query commands (search_query_cmds.cc)
int FtSearchCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtHybridCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtExplainCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtExplainCliCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtProfileCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);

// Aggregation & cursor commands (search_agg_cmds.cc)
int FtAggregateCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtCursorCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);

// Auxiliary commands (search_aux_cmds.cc)
int FtSugAddCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtSugGetCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtSugDelCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtSugLenCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtDictAddCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtDictDelCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtDictDumpCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtSynUpdateCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtSynDumpCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtSpellCheckCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtConfigCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
int FtDebugCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);
