#pragma once

#include "cluster_codec.h"
#include "search_commands.h"
#include "search_executor.h"

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <tuple>
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
std::shared_mutex& GetAliasesMutex();
std::shared_mutex& GetConfigMutex();
std::shared_mutex& GetTermDictsMutex();
std::shared_mutex& GetSuggestDictsMutex();
std::mutex& GetCursorsMutex();

// Race-free synchronization between worker/IO-thread RedisModule_UnblockClient and
// Redis main-thread DisconnectCallback: holds `mu` across RedisModule_UnblockClient so
// Redis cannot free `bc` in DisconnectCallback while UnblockClient is executing.
template <typename CtxT>
struct BlockedClientRegistry {
  std::mutex mu;
  std::unordered_map<RedisModuleBlockedClient*, std::shared_ptr<CtxT>> active;

  void Register(RedisModuleBlockedClient* bc, const std::shared_ptr<CtxT>& ctx) {
    if (!bc) return;
    std::lock_guard<std::mutex> lk(mu);
    active[bc] = ctx;
  }

  bool Unblock(const std::shared_ptr<CtxT>& ctx, RedisModuleBlockedClient* bc) {
    if (!bc) return false;
    std::lock_guard<std::mutex> lk(mu);
    auto it = active.find(bc);
    if (it == active.end()) {
      return false;
    }
    active.erase(it);
    auto* holder = new std::shared_ptr<CtxT>(ctx);
    RedisModule_UnblockClient(bc, holder);
    return true;
  }

  void OnDisconnect(RedisModuleBlockedClient* bc) {
    std::shared_ptr<CtxT> ctx;
    {
      std::lock_guard<std::mutex> lk(mu);
      auto it = active.find(bc);
      if (it != active.end()) {
        ctx = std::move(it->second);
        active.erase(it);
      }
    }
    if (ctx) {
      std::lock_guard<std::mutex> lk(ctx->mu);
      ctx->cancelled = true;
      ctx->bc = nullptr;
    }
  }
};

// Overflow-safe saturating deadline helper (returns INT64_MAX on overflow).
inline int64_t SaturatingAddMs(int64_t base_ms, int64_t delta_ms) {
  if (delta_ms <= 0) return base_ms;
  if (base_ms > INT64_MAX - delta_ms) return INT64_MAX;
  return base_ms + delta_ms;
}

// Reconstructs FT.CREATE / FT.ALTER field arguments from IndexSpec/FieldSpec (for SchemaSync & AOF).
void AppendFieldSpecArgs(const FieldSpec& f, std::vector<std::string>& out);
std::vector<std::string> BuildFtCreateArgs(const IndexSpec& spec);
SearchExecutor& GetSearchExecutor();
std::unordered_map<uint64_t, AggCursor>& GetCursors();
uint64_t NextCursorId();
std::string ResolveAlias(const std::string& name);
int64_t NowMs();
void TouchIndex(const IndexEntry& entry);
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
void PurgeExpiredCursorsLocked(int64_t now_ms);

// Shared argument & indexing helpers (defined in search_state.cc)
bool TryParseDouble(const std::string& s, double& out);
std::string_view ArgView(RedisModuleString* s);
std::string ArgStr(RedisModuleString* s);
bool MatchArg(std::string_view arg, const char* target);
void RemoveDocFromIndices(IndexEntry& entry, const std::string& doc_id);
void AddDocToIndices(IndexEntry& entry, const std::string& doc_id,
                     const std::unordered_map<std::string, std::string>& fields);
void AddDocToIndicesSkippingExistingHnsw(
    IndexEntry& entry, const std::string& doc_id,
    const std::unordered_map<std::string, std::string>& fields);
bool PassesIndexFilter(const IndexSpec& spec,
                       const std::unordered_map<std::string, std::string>& fields);

// Local DDL validation & execution helpers (shared by standalone, Coordinator, and Shard RPC)
bool ParseCreateSpec(const std::vector<std::string>& args, IndexSpec& out_spec,
                     std::string& err_out);
bool ApplyLocalCreate(RedisModuleCtx* ctx, const std::vector<std::string>& args,
                      bool is_cluster_mode, std::string& err_out);
bool ApplyLocalDropIndex(RedisModuleCtx* ctx, const std::vector<std::string>& args,
                         bool is_cluster_mode, bool idempotent_missing, std::string& err_out);
bool ParseAlterFields(const IndexSpec& spec, const std::vector<std::string>& args,
                      std::vector<FieldSpec>& out_fields, std::string& err_out);
bool ApplyLocalAlter(RedisModuleCtx* ctx, const std::vector<std::string>& args,
                     std::string& err_out);
bool ApplyLocalAliasAdd(const std::vector<std::string>& args, std::string& err_out);
bool ApplyLocalAliasDel(const std::vector<std::string>& args, bool idempotent_missing,
                        std::string& err_out);
bool ApplyLocalAliasUpdate(const std::vector<std::string>& args, std::string& err_out);
bool ApplyLocalSynUpdate(const std::vector<std::string>& args, std::string& err_out);
bool ApplyLocalDictAdd(const std::vector<std::string>& args, int64_t& out_added,
                       std::string& err_out);
bool ApplyLocalDictDel(const std::vector<std::string>& args, int64_t& out_removed,
                       std::string& err_out);
bool ApplyLocalDdlCommand(RedisModuleCtx* ctx, const std::string& cmd_name,
                          const std::vector<std::string>& args, bool is_cluster_mode,
                          bool is_rollback, int64_t& out_int_reply, std::string& err_out);
void ReplicateDdlCommand(RedisModuleCtx* ctx, const std::string& cmd_name,
                         const std::vector<std::string>& args);

// Keyspace & server event helpers (defined in search_keyspace.cc)
void ScanExistingKeys(RedisModuleCtx* ctx, IndexEntry& entry);
void ScanExistingKeysPreservingHnsw(RedisModuleCtx* ctx, IndexEntry& entry);
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
bool ParseAggArgsForProfile(
    RedisModuleString** argv, int start_arg, int argc,
    std::vector<gemini_search::PipelineStage>& stages_out,
    std::unordered_map<std::string, std::string>& params_out,
    int& dialect_inout, long long& timeout_ms_out, std::string& error_out);
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

// Lock-only shard search, doc fetch, DFS, hybrid & auxiliary helpers (no Redis GIL needed)
gemini_search::RpcSearchPhase1Response ExecuteLocalSearchPhase1(
    const gemini_search::RpcSearchPhase1Request& req);
gemini_search::RpcFetchDocsResponse ExecuteLocalFetchDocs(
    const gemini_search::RpcFetchDocsRequest& req);
gemini_search::RpcDfsTermStatsResponse ExecuteLocalDfsTermStats(
    const gemini_search::RpcDfsTermStatsRequest& req);
gemini_search::RpcHybridPhase1Response ExecuteLocalHybridPhase1(
    const gemini_search::RpcHybridPhase1Request& req);
gemini_search::RpcInfoStatsResponse ExecuteLocalInfoStats(
    const gemini_search::RpcInfoStatsRequest& req);
gemini_search::RpcTagValsResponse ExecuteLocalTagVals(
    const gemini_search::RpcTagValsRequest& req);
gemini_search::RpcSpellCheckResponse ExecuteLocalSpellCheck(
    const gemini_search::RpcSpellCheckRequest& req);
bool ApplyLocalSuggestCommand(const std::string& cmd_name,
                              const std::vector<std::string>& args,
                              bool& out_mutated,
                              gemini_search::RpcSuggestResponse& resp);
gemini_search::RpcAggMapResponse ExecuteLocalAggMap(
    const gemini_search::RpcAggMapRequest& req);
gemini_search::RpcCursorResponse ExecuteLocalCursorOp(
    const gemini_search::RpcCursorRequest& req);

// Shared FT.INFO reply formatter (used by standalone and cluster FT.INFO)
void ReplyWithIndexInfo(
    RedisModuleCtx* ctx, const IndexEntry& entry, uint64_t num_docs, uint64_t num_terms,
    uint64_t inverted_bytes, uint64_t vector_bytes, bool bg_indexing, double percent_indexed,
    uint64_t cursor_idle, uint64_t cursor_created, uint64_t cursor_expired,
    const std::vector<std::tuple<std::string, uint64_t, bool, double>>* cluster_shards = nullptr);

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
