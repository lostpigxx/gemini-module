#pragma once

#include "cluster_codec.h"
#include "cluster_topology.h"

#include <string>
#include <vector>

namespace gemini_search {

// Registers Phase 67+ RPC handlers (DDL broadcast + Schema Sync) on ClusterManager.
void RegisterClusterCoordinatorHandlers();

// Executes a DDL command across all healthy remote master shards + local Coordinator
// using the two-step broadcast protocol with compensation rollback on partial failure.
// Caller must have already pre-validated arguments on the main thread.
int ExecuteClusterDdl(RedisModuleCtx* ctx, RpcOpCode opcode, const std::string& cmd_name,
                      const std::vector<std::string>& args, bool reply_integer);

// Checks if any healthy peer advertises a higher schema_epoch and asynchronously
// pulls full Schema metadata via kRpcSchemaSyncReq/Resp.
void TriggerSchemaSyncIfNeeded(RedisModuleCtx* ctx);

}  // namespace gemini_search
