#pragma once

#include "cluster_codec.h"
#include "cluster_merger.h"
#include "cluster_topology.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gemini_search {

// Registers Phase 67+ RPC handlers (DDL, Schema Sync, Search Phase 1, Fetch Docs) on ClusterManager.
void RegisterClusterCoordinatorHandlers();

// Executes a DDL command across all healthy remote master shards + local Coordinator
// using the two-step broadcast protocol with compensation rollback on partial failure.
// Caller must have already pre-validated arguments on the main thread.
int ExecuteClusterDdl(RedisModuleCtx* ctx, RpcOpCode opcode, const std::string& cmd_name,
                      const std::vector<std::string>& args, bool reply_integer);

// Checks if any healthy peer advertises a higher schema_epoch and asynchronously
// pulls full Schema metadata via kRpcSchemaSyncReq/Resp.
void TriggerSchemaSyncIfNeeded(RedisModuleCtx* ctx);

// Plan for Phase 68 FT.SEARCH Query-Then-Fetch Scatter-Gather execution.
struct ClusterSearchPlan {
  RpcSearchPhase1Request phase1_req;
  RpcFetchDocsRequest fetch_template;
  bool is_knn = false;
  size_t knn_k = 0;
  std::string knn_score_alias;
  bool has_limit = false;
  size_t limit_offset = 0;
  size_t limit_count = 0;
  bool withscores = false;
  bool explainscore = false;
  std::string scorer_name = "BM25";
  bool withsortkeys = false;
  bool has_sortby = false;
  bool withpayloads = false;
  bool nocontent = false;
  int64_t timeout_ms = 5000;
};

// Executes FT.SEARCH across all healthy Master shards using Query-Then-Fetch:
//   Phase 1: Scatter metadata-only query (local SearchExecutor + remote kRpcSearchPhase1)
//   Merge:   M-way heap merge + doc_id dedup + LIMIT slice
//   Phase 2: Targeted Fetch of winning doc_ids only (skipped for LIMIT 0 0 / NOCONTENT)
int ExecuteClusterSearch(RedisModuleCtx* ctx, ClusterSearchPlan plan);

// Phase 69: Plan for FT.HYBRID global Scatter-Gather + RRF/LINEAR fusion
struct ClusterHybridPlan {
  RpcHybridPhase1Request phase1_req;
  RpcFetchDocsRequest fetch_template;
  HybridConfig hcfg;
  bool has_limit = false;
  size_t limit_offset = 0;
  size_t limit_count = 0;
  bool count_only = false;
  bool withscores = false;
  bool explainscore = false;
  bool withpayloads = false;
  bool nocontent = false;
  int64_t timeout_ms = 5000;
};

int ExecuteClusterHybrid(RedisModuleCtx* ctx, ClusterHybridPlan plan);

// Phase 69: Cluster FT.INFO aggregation across all healthy master shards
int ExecuteClusterInfo(RedisModuleCtx* ctx, const std::string& index_name);

// Phase 69: Cluster FT.TAGVALS global union/dedup across all healthy master shards
int ExecuteClusterTagVals(RedisModuleCtx* ctx, const std::string& index_name,
                          const std::string& field_name);

// Phase 69: Cluster FT.SPELLCHECK global known-term union & suggestion score aggregation
int ExecuteClusterSpellCheck(RedisModuleCtx* ctx, RpcSpellCheckRequest req);

// Phase 69: Cluster FT.SUG* transparent forwarding to slot-owning Master
struct ClusterSuggestPlan {
  std::string cmd_name;
  std::vector<std::string> args;
  std::string dict_name;
  bool is_write = false;
  bool sugget_withscores = false;
  bool sugget_withpayloads = false;
};

int ExecuteClusterSuggest(RedisModuleCtx* ctx, ClusterSuggestPlan plan);

// Phase 69: Cluster FT.PROFILE <idx> SEARCH ... with Coordinator + per-shard timing breakdown
struct ClusterProfileSearchPlan {
  RpcSearchPhase1Request phase1_req;
  bool has_limit = false;
  size_t limit_offset = 0;
  size_t limit_count = 0;
  double parse_time_us = 0.0;
  int64_t timeout_ms = 5000;
};

int ExecuteClusterProfileSearch(RedisModuleCtx* ctx, ClusterProfileSearchPlan plan);

}  // namespace gemini_search
