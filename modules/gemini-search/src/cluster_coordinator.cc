#include "cluster_coordinator.h"
#include "search_internal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gemini_search {

namespace {

struct DdlPeerEndpoint {
  std::string node_id;
  std::string ip;
  int rpc_port = 0;
};

struct ClusterDdlContext {
  RedisModuleBlockedClient* bc = nullptr;
  RpcOpCode opcode = RpcOpCode::kRpcDdlCreate;
  std::string cmd_name;
  std::vector<std::string> args;
  uint64_t schema_epoch = 0;
  int64_t timeout_ms = 5000;
  bool reply_integer = false;

  std::mutex mu;
  size_t pending = 0;
  bool in_rollback = false;
  bool cancelled = false;
  bool finished = false;
  std::vector<DdlPeerEndpoint> succeeded_peers;
  std::string failed_node_id;
  std::string failed_reason;
};

std::mutex g_active_ddls_mu;
std::unordered_map<RedisModuleBlockedClient*, std::shared_ptr<ClusterDdlContext>> g_active_ddls;

std::atomic<bool> g_schema_sync_in_flight{false};
std::atomic<int64_t> g_last_schema_sync_ms{0};
constexpr int64_t kSchemaSyncRetryCooldownMs = 500;

bool BuildRollbackSpec(const std::string& cmd_name, const std::vector<std::string>& args,
                       RpcOpCode& rb_op, std::string& rb_cmd, std::vector<std::string>& rb_args) {
  if (MatchArg(cmd_name, "FT.CREATE") && !args.empty()) {
    rb_op = RpcOpCode::kRpcDdlDrop;
    rb_cmd = "FT.DROPINDEX";
    rb_args = {args[0]};  // Never pass DD on rollback
    return true;
  }
  if (MatchArg(cmd_name, "FT.ALIASADD") && !args.empty()) {
    rb_op = RpcOpCode::kRpcDdlAlias;
    rb_cmd = "FT.ALIASDEL";
    rb_args = {args[0]};
    return true;
  }
  return false;
}

void FireBestEffortRollback(const std::vector<DdlPeerEndpoint>& peers, RpcOpCode rb_op,
                            const std::string& rb_cmd, const std::vector<std::string>& rb_args,
                            uint64_t epoch, int64_t timeout_ms) {
  auto& mgr = ClusterManager::Instance();
  RpcDdlRequest rb_req;
  rb_req.schema_epoch = epoch;
  rb_req.coordinator_node_id = mgr.MyNodeId();
  rb_req.cmd_name = rb_cmd;
  rb_req.args = rb_args;
  rb_req.flags = kDdlFlagRollback;
  std::string encoded;
  EncodeDdlRequest(rb_req, encoded);

  for (const auto& peer : peers) {
    mgr.RpcClient().SendAsync(peer.node_id, peer.ip, peer.rpc_port, rb_op, encoded, timeout_ms,
                              [](RpcResponse) {});
  }
}

void UnblockDdlClient(const std::shared_ptr<ClusterDdlContext>& ddl_ctx,
                      RedisModuleBlockedClient* bc) {
  if (!bc) return;
  {
    std::lock_guard<std::mutex> lk(g_active_ddls_mu);
    g_active_ddls.erase(bc);
  }
  auto* holder = new std::shared_ptr<ClusterDdlContext>(ddl_ctx);
  RedisModule_UnblockClient(bc, holder);
}

int ClusterDdlReplyCallback(RedisModuleCtx* ctx, RedisModuleString** /*argv*/, int /*argc*/) {
  auto* holder = static_cast<std::shared_ptr<ClusterDdlContext>*>(
      RedisModule_GetBlockedClientPrivateData(ctx));
  if (!holder || !(*holder)) {
    return RedisModule_ReplyWithError(ctx, "ERR internal cluster DDL state missing");
  }
  auto& ddl_ctx = **holder;
  std::string failed_node;
  std::string failed_reason;
  std::vector<DdlPeerEndpoint> succeeded_peers;
  {
    std::lock_guard<std::mutex> lk(ddl_ctx.mu);
    failed_node = ddl_ctx.failed_node_id;
    failed_reason = ddl_ctx.failed_reason;
    succeeded_peers = ddl_ctx.succeeded_peers;
  }

  if (!failed_reason.empty()) {
    std::string msg = "ERR cluster DDL failed on " +
                      (failed_node.size() > 8 ? failed_node.substr(0, 8) : failed_node) + ": " +
                      failed_reason;
    return RedisModule_ReplyWithError(ctx, msg.c_str());
  }

  // All remote masters succeeded; apply locally on Coordinator main thread
  int64_t int_reply = 0;
  std::string local_err;
  if (!ApplyLocalDdlCommand(ctx, ddl_ctx.cmd_name, ddl_ctx.args, /*is_cluster_mode=*/true,
                            /*is_rollback=*/false, int_reply, local_err)) {
    RpcOpCode rb_op = RpcOpCode::kRpcDdlDrop;
    std::string rb_cmd;
    std::vector<std::string> rb_args;
    if (!succeeded_peers.empty() &&
        BuildRollbackSpec(ddl_ctx.cmd_name, ddl_ctx.args, rb_op, rb_cmd, rb_args)) {
      FireBestEffortRollback(succeeded_peers, rb_op, rb_cmd, rb_args, ddl_ctx.schema_epoch,
                             ddl_ctx.timeout_ms);
    }
    return RedisModule_ReplyWithError(ctx, local_err.c_str());
  }

  RedisModule_ReplicateVerbatim(ctx);
  auto& mgr = ClusterManager::Instance();
  SetPersistedSchemaEpoch(mgr.GetSchemaEpoch());
  mgr.BroadcastHello(ctx, /*request_reply=*/false);

  if (ddl_ctx.reply_integer) {
    return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(int_reply));
  }
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

int ClusterDdlTimeoutCallback(RedisModuleCtx* ctx, RedisModuleString** /*argv*/, int /*argc*/) {
  return RedisModule_ReplyWithError(ctx, "ERR cluster DDL timed out");
}

void ClusterDdlFreePrivData(RedisModuleCtx* /*ctx*/, void* privdata) {
  auto* holder = static_cast<std::shared_ptr<ClusterDdlContext>*>(privdata);
  delete holder;
}

void ClusterDdlDisconnectCallback(RedisModuleCtx* /*ctx*/, RedisModuleBlockedClient* bc) {
  std::shared_ptr<ClusterDdlContext> ddl_ctx;
  {
    std::lock_guard<std::mutex> lk(g_active_ddls_mu);
    auto it = g_active_ddls.find(bc);
    if (it != g_active_ddls.end()) {
      ddl_ctx = std::move(it->second);
      g_active_ddls.erase(it);
    }
  }
  if (ddl_ctx) {
    std::lock_guard<std::mutex> lk(ddl_ctx->mu);
    ddl_ctx->cancelled = true;
    ddl_ctx->bc = nullptr;
  }
}

void HandleRemoteDdlRequest(const RpcFrame& req_frame, const std::string& /*peer_ip*/,
                            ClusterRpcServer::ResponseSender send_resp) {
  RpcDdlRequest req;
  if (!DecodeDdlRequest(req_frame.payload, req)) {
    RpcDdlResponse resp;
    resp.ok = false;
    resp.error_msg = "malformed DDL request payload";
    std::string out;
    EncodeDdlResponse(resp, out);
    send_resp(RpcOpCode::kRpcDdlResp, kRpcFlagNone, std::move(out));
    return;
  }

  // Offload GIL-requiring DDL execution to SearchExecutor so RPC I/O thread never blocks
  GetSearchExecutor().Submit(
      [req = std::move(req), send_resp = std::move(send_resp)]() mutable {
        auto& mgr = ClusterManager::Instance();
        RpcDdlResponse resp;
        if (mgr.IsShuttingDown()) {
          resp.ok = false;
          resp.error_msg = "node shutting down";
          std::string out;
          EncodeDdlResponse(resp, out);
          send_resp(RpcOpCode::kRpcDdlResp, kRpcFlagNone, std::move(out));
          return;
        }

        std::lock_guard<std::mutex> ctx_lk(mgr.DetachedCtxMutex());
        RedisModuleCtx* tctx = mgr.DetachedCtx();
        if (!tctx || mgr.IsShuttingDown()) {
          resp.ok = false;
          resp.error_msg = "detached context unavailable";
          std::string out;
          EncodeDdlResponse(resp, out);
          send_resp(RpcOpCode::kRpcDdlResp, kRpcFlagNone, std::move(out));
          return;
        }

        RedisModule_ThreadSafeContextLock(tctx);
        RedisModule_SelectDb(tctx, 0);

        bool is_rollback = (req.flags & kDdlFlagRollback) != 0;
        int64_t int_reply = 0;
        std::string err;
        bool ok = ApplyLocalDdlCommand(tctx, req.cmd_name, req.args, /*is_cluster_mode=*/true,
                                       is_rollback, int_reply, err);
        if (ok) {
          ReplicateDdlCommand(tctx, req.cmd_name, req.args);
          if (!is_rollback && req.schema_epoch > mgr.GetSchemaEpoch()) {
            mgr.SetSchemaEpoch(req.schema_epoch);
            SetPersistedSchemaEpoch(req.schema_epoch);
          }
        }
        uint64_t cur_epoch = mgr.GetSchemaEpoch();
        RedisModule_ThreadSafeContextUnlock(tctx);

        resp.ok = ok;
        resp.int_reply = int_reply;
        resp.applied_epoch = cur_epoch;
        resp.error_msg = std::move(err);

        std::string out;
        EncodeDdlResponse(resp, out);
        send_resp(RpcOpCode::kRpcDdlResp, kRpcFlagNone, std::move(out));
      });
}

void HandleSchemaSyncRequest(const RpcFrame& req_frame, const std::string& /*peer_ip*/,
                             ClusterRpcServer::ResponseSender send_resp) {
  RpcSchemaSyncRequest req;
  if (!DecodeSchemaSyncRequest(req_frame.payload, req)) {
    RpcErrorPayload err{400, "malformed SchemaSync request"};
    std::string out;
    EncodeErrorPayload(err, out);
    send_resp(RpcOpCode::kRpcSchemaSyncResp, kRpcFlagError, std::move(out));
    return;
  }

  GetSearchExecutor().Submit([send_resp = std::move(send_resp)]() mutable {
    auto& mgr = ClusterManager::Instance();
    if (mgr.IsShuttingDown()) {
      RpcErrorPayload err{503, "node shutting down"};
      std::string out;
      EncodeErrorPayload(err, out);
      send_resp(RpcOpCode::kRpcSchemaSyncResp, kRpcFlagError, std::move(out));
      return;
    }

    std::lock_guard<std::mutex> ctx_lk(mgr.DetachedCtxMutex());
    RedisModuleCtx* tctx = mgr.DetachedCtx();
    if (!tctx || mgr.IsShuttingDown()) {
      RpcErrorPayload err{503, "detached context unavailable"};
      std::string out;
      EncodeErrorPayload(err, out);
      send_resp(RpcOpCode::kRpcSchemaSyncResp, kRpcFlagError, std::move(out));
      return;
    }

    RpcSchemaSyncResponse resp;
    RedisModule_ThreadSafeContextLock(tctx);
    resp.schema_epoch = mgr.GetSchemaEpoch();

    {
      std::shared_lock<std::shared_mutex> lk(GetIndicesMutex());
      std::vector<std::string> names;
      names.reserve(GetIndices().size());
      for (const auto& [name, ptr] : GetIndices()) {
        if (ptr) names.push_back(name);
      }
      std::sort(names.begin(), names.end());
      resp.indices.reserve(names.size());
      for (const auto& name : names) {
        const auto& entry = *GetIndices().at(name);
        std::shared_lock<std::shared_mutex> idx_lk(entry.Mutex());
        SchemaIndexSnapshot snap;
        snap.spec = entry.spec;
        auto dumped = entry.synonyms.Dump();
        snap.synonyms.reserve(dumped.size());
        for (auto& [gid, terms] : dumped) {
          snap.synonyms.emplace_back(std::move(gid), std::move(terms));
        }
        resp.indices.push_back(std::move(snap));
      }
    }

    for (const auto& [alias, target] : GetAliases()) {
      resp.aliases.emplace_back(alias, target);
    }
    std::sort(resp.aliases.begin(), resp.aliases.end());

    for (const auto& [dname, tdict] : GetTermDicts()) {
      resp.term_dicts.emplace_back(dname, tdict.Dump());
    }
    std::sort(resp.term_dicts.begin(), resp.term_dicts.end());
    RedisModule_ThreadSafeContextUnlock(tctx);

    std::string encoded;
    EncodeSchemaSyncResponse(resp, encoded);
    send_resp(RpcOpCode::kRpcSchemaSyncResp, kRpcFlagNone, std::move(encoded));
  });
}

void ApplySchemaSyncSnapshot(RpcSchemaSyncResponse resp) {
  auto& mgr = ClusterManager::Instance();
  if (mgr.IsShuttingDown()) {
    g_schema_sync_in_flight.store(false, std::memory_order_release);
    return;
  }

  std::lock_guard<std::mutex> ctx_lk(mgr.DetachedCtxMutex());
  RedisModuleCtx* tctx = mgr.DetachedCtx();
  if (!tctx || mgr.IsShuttingDown()) {
    g_schema_sync_in_flight.store(false, std::memory_order_release);
    return;
  }

  RedisModule_ThreadSafeContextLock(tctx);
  RedisModule_SelectDb(tctx, 0);

  if (resp.schema_epoch > mgr.GetSchemaEpoch()) {
    // 1. Sync term dictionaries first (indices may reference DICT)
    {
      std::unique_lock<std::shared_mutex> dlk(GetTermDictsMutex());
      GetTermDicts().clear();
      for (const auto& [dname, terms] : resp.term_dicts) {
        GetTermDicts()[dname].Add(terms);
      }
    }

    // 2. Sync indices: drop stale ones, create missing ones, update altered fields/synonyms
    std::unordered_set<std::string> remote_names;
    remote_names.reserve(resp.indices.size());
    for (const auto& snap : resp.indices) {
      remote_names.insert(snap.spec.name);
    }

    std::vector<std::shared_ptr<IndexEntry>> to_scan;
    {
      std::unique_lock<std::shared_mutex> lk(GetIndicesMutex());
      for (auto it = GetIndices().begin(); it != GetIndices().end();) {
        if (remote_names.count(it->first) == 0) {
          StopBackgroundIndexing(tctx, it->first);
          it = GetIndices().erase(it);
        } else {
          ++it;
        }
      }

      for (auto& snap : resp.indices) {
        auto it = GetIndices().find(snap.spec.name);
        if (it == GetIndices().end()) {
          auto entry = std::make_shared<IndexEntry>();
          entry->spec = std::move(snap.spec);
          for (const auto& [gid, terms] : snap.synonyms) {
            entry->synonyms.Update(gid, terms);
          }
          TouchIndex(*entry);
          GetIndices().emplace(entry->spec.name, entry);
          if (entry->spec.HasPrefixes() && !entry->spec.skip_initial_scan) {
            to_scan.push_back(entry);
          }
        } else {
          auto& entry = *it->second;
          std::unique_lock<std::shared_mutex> idx_lk(entry.Mutex());
          bool fields_added = false;
          for (const auto& f : snap.spec.fields) {
            if (!entry.spec.FindField(f.name)) {
              entry.spec.fields.push_back(f);
              fields_added = true;
            }
          }
          entry.synonyms.Clear();
          for (const auto& [gid, terms] : snap.synonyms) {
            entry.synonyms.Update(gid, terms);
          }
          if (fields_added && entry.spec.HasPrefixes()) {
            to_scan.push_back(it->second);
          }
        }
      }
    }

    // 3. Sync aliases
    GetAliases().clear();
    for (const auto& [alias, target] : resp.aliases) {
      GetAliases()[alias] = target;
    }

    // 4. Advance local epoch and trigger Phase 64 background incremental scan for new indices
    mgr.SetSchemaEpoch(resp.schema_epoch);
    SetPersistedSchemaEpoch(resp.schema_epoch);

    for (const auto& entry : to_scan) {
      StartOrRunInitialScan(tctx, *entry);
    }

    mgr.BroadcastHello(tctx, /*request_reply=*/false);
    RedisModule_Log(tctx, "notice",
                    "GeminiSearch cluster: synced schema to epoch %llu (%zu indices)",
                    static_cast<unsigned long long>(resp.schema_epoch), resp.indices.size());
  }

  RedisModule_ThreadSafeContextUnlock(tctx);
  g_schema_sync_in_flight.store(false, std::memory_order_release);
}

// ============================================================================
// Phase 68: FT.SEARCH Query-Then-Fetch Shard Handlers & Coordinator Machine
// ============================================================================

void HandleSearchPhase1Request(const RpcFrame& req_frame, const std::string& /*peer_ip*/,
                               ClusterRpcServer::ResponseSender send_resp) {
  RpcSearchPhase1Request req;
  if (!DecodeSearchPhase1Request(req_frame.payload, req)) {
    RpcSearchPhase1Response resp;
    resp.ok = false;
    resp.error_msg = "malformed SearchPhase1 request";
    std::string out;
    EncodeSearchPhase1Response(resp, out);
    send_resp(RpcOpCode::kRpcSearchPhase1Resp, kRpcFlagNone, std::move(out));
    return;
  }

  // Pure read-only offload on SearchExecutor: ZERO remote Redis main thread touch!
  GetSearchExecutor().Submit(
      [req = std::move(req), send_resp = std::move(send_resp)]() mutable {
        RpcSearchPhase1Response resp = ExecuteLocalSearchPhase1(req);
        std::string out;
        EncodeSearchPhase1Response(resp, out);
        send_resp(RpcOpCode::kRpcSearchPhase1Resp, kRpcFlagNone, std::move(out));
      });
}

void HandleFetchDocsRequest(const RpcFrame& req_frame, const std::string& /*peer_ip*/,
                            ClusterRpcServer::ResponseSender send_resp) {
  RpcFetchDocsRequest req;
  if (!DecodeFetchDocsRequest(req_frame.payload, req)) {
    RpcFetchDocsResponse resp;
    resp.ok = false;
    resp.error_msg = "malformed FetchDocs request";
    std::string out;
    EncodeFetchDocsResponse(resp, out);
    send_resp(RpcOpCode::kRpcFetchDocsResp, kRpcFlagNone, std::move(out));
    return;
  }

  // Pure read-only offload on SearchExecutor: ZERO remote Redis main thread touch!
  GetSearchExecutor().Submit(
      [req = std::move(req), send_resp = std::move(send_resp)]() mutable {
        RpcFetchDocsResponse resp = ExecuteLocalFetchDocs(req);
        std::string out;
        EncodeFetchDocsResponse(resp, out);
        send_resp(RpcOpCode::kRpcFetchDocsResp, kRpcFlagNone, std::move(out));
      });
}

struct SearchShardEndpoint {
  uint32_t shard_idx = 0;
  std::string node_id;
  std::string ip;
  int rpc_port = 0;
  bool is_myself = false;
};

struct ClusterSearchFinalRow {
  std::string doc_id;
  double score = 0.0;
  std::string sort_str;
  bool has_yield_dist = false;
  bool has_payload = false;
  std::string payload;
  std::vector<std::pair<std::string, std::string>> fields;
};

struct ClusterSearchContext {
  RedisModuleBlockedClient* bc = nullptr;
  ClusterSearchPlan plan;
  std::vector<SearchShardEndpoint> shards;
  int64_t start_ms = 0;
  bool on_timeout_return = false;

  std::mutex mu;
  size_t phase0_pending = 0;
  size_t phase1_pending = 0;
  size_t phase2_pending = 0;
  bool cancelled = false;
  bool finished = false;
  std::string error_msg;

  // Phase 0 collected DFS term stats (optional)
  std::vector<std::vector<RpcFieldBm25Stats>> dfs_shard_stats;

  // Phase 1 collected slices
  std::vector<ShardPhase1Slice> slices;
  MergeSortMode sort_mode = MergeSortMode::kDocIdAsc;
  bool sort_mode_set = false;
  std::string score_alias;

  // Final reply data
  bool count_only = false;
  uint64_t global_total = 0;
  std::vector<ClusterSearchFinalRow> final_rows;
};

std::mutex g_active_searches_mu;
std::unordered_map<RedisModuleBlockedClient*, std::shared_ptr<ClusterSearchContext>>
    g_active_searches;

void UnblockSearchClient(const std::shared_ptr<ClusterSearchContext>& sctx,
                         RedisModuleBlockedClient* bc) {
  if (!bc) return;
  {
    std::lock_guard<std::mutex> lk(g_active_searches_mu);
    g_active_searches.erase(bc);
  }
  auto* holder = new std::shared_ptr<ClusterSearchContext>(sctx);
  RedisModule_UnblockClient(bc, holder);
}

int ClusterSearchReplyCallback(RedisModuleCtx* ctx, RedisModuleString** /*argv*/, int /*argc*/) {
  auto* holder = static_cast<std::shared_ptr<ClusterSearchContext>*>(
      RedisModule_GetBlockedClientPrivateData(ctx));
  if (!holder || !(*holder)) {
    return RedisModule_ReplyWithError(ctx, "ERR internal cluster search state missing");
  }
  auto& sctx = **holder;
  std::lock_guard<std::mutex> lk(sctx.mu);

  if (!sctx.error_msg.empty()) {
    return RedisModule_ReplyWithError(ctx, sctx.error_msg.c_str());
  }

  if (sctx.count_only) {
    RedisModule_ReplyWithArray(ctx, 1);
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(sctx.global_total));
    return REDISMODULE_OK;
  }

  const auto& plan = sctx.plan;
  long total = static_cast<long>(sctx.final_rows.size());

  if (plan.is_knn) {
    long mult = 1 + (plan.withpayloads ? 1 : 0) + (plan.nocontent ? 0 : 1);
    RedisModule_ReplyWithArray(ctx, 1 + total * mult);
    RedisModule_ReplyWithLongLong(ctx, total);
    const std::string& score_field =
        sctx.score_alias.empty() ? plan.knn_score_alias : sctx.score_alias;
    for (const auto& row : sctx.final_rows) {
      RedisModule_ReplyWithStringBuffer(ctx, row.doc_id.data(), row.doc_id.size());
      if (plan.withpayloads) {
        if (row.has_payload) {
          RedisModule_ReplyWithStringBuffer(ctx, row.payload.data(), row.payload.size());
        } else {
          RedisModule_ReplyWithNull(ctx);
        }
      }
      if (!plan.nocontent) {
        std::string score_str = std::to_string(static_cast<float>(row.score));
        long flen = static_cast<long>(row.fields.size()) * 2 + 2 + (plan.explainscore ? 2 : 0);
        RedisModule_ReplyWithArray(ctx, flen);
        RedisModule_ReplyWithStringBuffer(ctx, score_field.data(), score_field.size());
        RedisModule_ReplyWithStringBuffer(ctx, score_str.data(), score_str.size());
        if (plan.explainscore) {
          std::string expl = plan.scorer_name + "(score=" + score_str + ")";
          RedisModule_ReplyWithCString(ctx, "__score_explanation");
          RedisModule_ReplyWithStringBuffer(ctx, expl.data(), expl.size());
        }
        for (const auto& [k, v] : row.fields) {
          RedisModule_ReplyWithStringBuffer(ctx, k.data(), k.size());
          RedisModule_ReplyWithStringBuffer(ctx, v.data(), v.size());
        }
      }
    }
    return REDISMODULE_OK;
  }

  bool emit_sortkey = !plan.withscores && plan.withsortkeys && plan.has_sortby;
  long mult = 1 + (emit_sortkey ? 1 : 0) + (plan.withpayloads ? 1 : 0) + (plan.nocontent ? 0 : 1);
  RedisModule_ReplyWithArray(ctx, 1 + total * mult);
  RedisModule_ReplyWithLongLong(ctx, total);

  for (const auto& row : sctx.final_rows) {
    RedisModule_ReplyWithStringBuffer(ctx, row.doc_id.data(), row.doc_id.size());
    if (emit_sortkey) {
      RedisModule_ReplyWithStringBuffer(ctx, row.sort_str.data(), row.sort_str.size());
    }
    if (plan.withpayloads) {
      if (row.has_payload) {
        RedisModule_ReplyWithStringBuffer(ctx, row.payload.data(), row.payload.size());
      } else {
        RedisModule_ReplyWithNull(ctx);
      }
    }
    if (!plan.nocontent) {
      if (plan.withscores) {
        std::string score_str = std::to_string(row.score);
        long extra = plan.explainscore ? 4 : 2;
        RedisModule_ReplyWithArray(ctx, static_cast<long>(row.fields.size()) * 2 + extra);
        RedisModule_ReplyWithCString(ctx, "__search_score");
        RedisModule_ReplyWithStringBuffer(ctx, score_str.data(), score_str.size());
        if (plan.explainscore) {
          std::string expl = plan.scorer_name + "(score=" + score_str + ")";
          RedisModule_ReplyWithCString(ctx, "__score_explanation");
          RedisModule_ReplyWithStringBuffer(ctx, expl.data(), expl.size());
        }
      } else {
        bool emit_yield = (!sctx.score_alias.empty() && row.has_yield_dist);
        long extra = emit_yield ? (plan.explainscore ? 4 : 2) : 0;
        RedisModule_ReplyWithArray(ctx, static_cast<long>(row.fields.size()) * 2 + extra);
        if (emit_yield) {
          std::string dstr = std::to_string(static_cast<float>(row.score));
          RedisModule_ReplyWithStringBuffer(ctx, sctx.score_alias.data(), sctx.score_alias.size());
          RedisModule_ReplyWithStringBuffer(ctx, dstr.data(), dstr.size());
          if (plan.explainscore) {
            std::string expl = plan.scorer_name + "(score=" + dstr + ")";
            RedisModule_ReplyWithCString(ctx, "__score_explanation");
            RedisModule_ReplyWithStringBuffer(ctx, expl.data(), expl.size());
          }
        }
      }
      for (const auto& [k, v] : row.fields) {
        RedisModule_ReplyWithStringBuffer(ctx, k.data(), k.size());
        RedisModule_ReplyWithStringBuffer(ctx, v.data(), v.size());
      }
    }
  }
  return REDISMODULE_OK;
}

int ClusterSearchTimeoutCallback(RedisModuleCtx* ctx, RedisModuleString** /*argv*/, int /*argc*/) {
  if (IsOnTimeoutReturn()) {
    RedisModule_ReplyWithArray(ctx, 1);
    RedisModule_ReplyWithLongLong(ctx, 0);
    return REDISMODULE_OK;
  }
  return RedisModule_ReplyWithError(ctx, "ERR cluster search timed out");
}

void ClusterSearchFreePrivData(RedisModuleCtx* /*ctx*/, void* privdata) {
  auto* holder = static_cast<std::shared_ptr<ClusterSearchContext>*>(privdata);
  delete holder;
}

void ClusterSearchDisconnectCallback(RedisModuleCtx* /*ctx*/, RedisModuleBlockedClient* bc) {
  std::shared_ptr<ClusterSearchContext> sctx;
  {
    std::lock_guard<std::mutex> lk(g_active_searches_mu);
    auto it = g_active_searches.find(bc);
    if (it != g_active_searches.end()) {
      sctx = std::move(it->second);
      g_active_searches.erase(it);
    }
  }
  if (sctx) {
    std::lock_guard<std::mutex> lk(sctx->mu);
    sctx->cancelled = true;
    sctx->bc = nullptr;
  }
}

void OnPhase2FetchDone(const std::shared_ptr<ClusterSearchContext>& sctx,
                       const std::vector<size_t>& winner_indices, const std::string& node_id,
                       bool ok, const std::string& err, RpcFetchDocsResponse&& resp) {
  RedisModuleBlockedClient* to_unblock = nullptr;
  {
    std::lock_guard<std::mutex> lk(sctx->mu);
    if (sctx->finished || sctx->cancelled) return;

    if (!ok || !resp.ok) {
      if (!sctx->on_timeout_return && sctx->error_msg.empty()) {
        std::string reason = !ok ? err : resp.error_msg;
        sctx->error_msg = "ERR cluster shard fetch failed (" +
                          (node_id.size() > 8 ? node_id.substr(0, 8) : node_id) + "): " + reason;
      }
    } else {
      size_t n = std::min(winner_indices.size(), resp.docs.size());
      for (size_t i = 0; i < n; i++) {
        size_t wi = winner_indices[i];
        if (wi < sctx->final_rows.size()) {
          auto& dst = sctx->final_rows[wi];
          auto& src = resp.docs[i];
          dst.has_payload = src.has_payload;
          dst.payload = std::move(src.payload);
          dst.fields = std::move(src.fields);
        }
      }
    }

    if (sctx->phase2_pending > 0) sctx->phase2_pending--;
    if (sctx->phase2_pending == 0 && !sctx->finished) {
      sctx->finished = true;
      to_unblock = sctx->bc;
      sctx->bc = nullptr;
    }
  }
  UnblockSearchClient(sctx, to_unblock);
}

void OnPhase1ShardDone(const std::shared_ptr<ClusterSearchContext>& sctx, uint32_t shard_idx,
                       const std::string& node_id, bool ok, const std::string& err,
                       RpcSearchPhase1Response&& resp) {
  bool start_phase2 = false;
  std::unordered_map<uint32_t, std::vector<size_t>> fetch_plan;
  RedisModuleBlockedClient* to_unblock = nullptr;

  {
    std::lock_guard<std::mutex> lk(sctx->mu);
    if (sctx->finished || sctx->cancelled) return;

    if (!ok || !resp.ok) {
      if (!sctx->on_timeout_return && sctx->error_msg.empty()) {
        std::string reason = !ok ? err : resp.error_msg;
        sctx->error_msg = "ERR cluster shard query failed (" +
                          (node_id.size() > 8 ? node_id.substr(0, 8) : node_id) + "): " + reason;
      }
    } else {
      if (!sctx->sort_mode_set) {
        sctx->sort_mode = static_cast<MergeSortMode>(resp.sort_mode);
        sctx->sort_mode_set = true;
      }
      if (sctx->score_alias.empty() && !resp.score_alias.empty()) {
        sctx->score_alias = resp.score_alias;
      }

      ShardPhase1Slice slice;
      slice.shard_idx = shard_idx;
      slice.total_hits = resp.total_hits;
      slice.items.reserve(resp.items.size());
      for (auto& it : resp.items) {
        MergeCandidate c;
        c.doc_id = std::move(it.doc_id);
        c.shard_idx = shard_idx;
        c.score = it.score;
        c.sort_num = it.sort_num;
        c.sort_str = std::move(it.sort_str);
        c.has_yield_dist = it.has_yield_dist;
        slice.items.push_back(std::move(c));
      }
      sctx->slices.push_back(std::move(slice));
    }

    if (sctx->phase1_pending > 0) sctx->phase1_pending--;
    if (sctx->phase1_pending == 0 && !sctx->finished) {
      if (!sctx->error_msg.empty()) {
        sctx->finished = true;
        to_unblock = sctx->bc;
        sctx->bc = nullptr;
      } else {
        const auto& plan = sctx->plan;
        size_t knn_cap = plan.is_knn ? plan.knn_k : 0;
        MergedSearchResult merged = MergeSearchShards(
            sctx->slices, sctx->sort_mode, plan.limit_offset, plan.limit_count, plan.has_limit,
            GetMaxSearchResults(), knn_cap);

        if (plan.phase1_req.count_only) {
          sctx->count_only = true;
          sctx->global_total = merged.global_total;
          sctx->finished = true;
          to_unblock = sctx->bc;
          sctx->bc = nullptr;
        } else {
          sctx->global_total = merged.global_total;
          sctx->final_rows.reserve(merged.winners.size());
          for (auto& w : merged.winners) {
            ClusterSearchFinalRow row;
            row.doc_id = std::move(w.doc_id);
            row.score = w.score;
            row.sort_str = std::move(w.sort_str);
            row.has_yield_dist = w.has_yield_dist;
            sctx->final_rows.push_back(std::move(row));
          }

          bool skip_fetch = sctx->final_rows.empty() || (plan.nocontent && !plan.withpayloads);
          if (skip_fetch || merged.winners_by_shard.empty()) {
            sctx->finished = true;
            to_unblock = sctx->bc;
            sctx->bc = nullptr;
          } else {
            fetch_plan = std::move(merged.winners_by_shard);
            sctx->phase2_pending = fetch_plan.size();
            start_phase2 = true;
          }
        }
      }
    }
  }

  if (to_unblock) {
    UnblockSearchClient(sctx, to_unblock);
    return;
  }

  if (!start_phase2) return;

  // Phase 2: Targeted Fetch of winning doc_ids per shard
  auto& mgr = ClusterManager::Instance();
  int64_t elapsed = ClusterNowMs() - sctx->start_ms;
  int64_t rem_timeout = std::max<int64_t>(200, sctx->plan.timeout_ms - elapsed);

  for (auto& [sidx, winner_indices] : fetch_plan) {
    if (sidx >= sctx->shards.size()) {
      OnPhase2FetchDone(sctx, winner_indices, "unknown", false, "invalid shard index", {});
      continue;
    }
    const auto& ep = sctx->shards[sidx];
    RpcFetchDocsRequest freq = sctx->plan.fetch_template;
    freq.doc_ids.reserve(winner_indices.size());
    for (size_t wi : winner_indices) {
      freq.doc_ids.push_back(sctx->final_rows[wi].doc_id);
    }

    if (ep.is_myself) {
      GetSearchExecutor().Submit(
          [sctx, winner_indices = std::move(winner_indices), nid = ep.node_id,
           freq = std::move(freq)]() mutable {
            RpcFetchDocsResponse fresp = ExecuteLocalFetchDocs(freq);
            OnPhase2FetchDone(sctx, winner_indices, nid, true, "", std::move(fresp));
          });
    } else {
      std::string encoded_fetch;
      EncodeFetchDocsRequest(freq, encoded_fetch);
      mgr.RpcClient().SendAsync(
          ep.node_id, ep.ip, ep.rpc_port, RpcOpCode::kRpcFetchDocs, encoded_fetch, rem_timeout,
          [sctx, winner_indices = std::move(winner_indices), nid = ep.node_id](RpcResponse r) {
            if (r.status != RpcStatus::kOk) {
              OnPhase2FetchDone(sctx, winner_indices, nid, false,
                                std::string(RpcStatusToString(r.status)) +
                                    (r.error_msg.empty() ? "" : (": " + r.error_msg)),
                                {});
              return;
            }
            if (r.flags & kRpcFlagError) {
              OnPhase2FetchDone(sctx, winner_indices, nid, false,
                                r.error_msg.empty() ? "remote fetch error" : r.error_msg, {});
              return;
            }
            RpcFetchDocsResponse fresp;
            if (!DecodeFetchDocsResponse(r.payload, fresp)) {
              OnPhase2FetchDone(sctx, winner_indices, nid, false, "malformed fetch response", {});
              return;
            }
            OnPhase2FetchDone(sctx, winner_indices, nid, true, "", std::move(fresp));
          });
    }
  }
}

void DispatchSearchPhase1(const std::shared_ptr<ClusterSearchContext>& sctx) {
  auto& mgr = ClusterManager::Instance();
  std::string encoded_p1;
  int64_t rem_timeout = 5000;
  {
    std::lock_guard<std::mutex> lk(sctx->mu);
    if (sctx->finished || sctx->cancelled) return;
    int64_t elapsed = ClusterNowMs() - sctx->start_ms;
    rem_timeout = std::max<int64_t>(200, (sctx->plan.timeout_ms > 0 ? sctx->plan.timeout_ms : 5000) -
                                             elapsed);
    sctx->plan.phase1_req.remaining_timeout_ms = rem_timeout;
    sctx->phase1_pending = sctx->shards.size();
    EncodeSearchPhase1Request(sctx->plan.phase1_req, encoded_p1);
  }

  for (const auto& ep : sctx->shards) {
    if (ep.is_myself) {
      GetSearchExecutor().Submit([sctx, sidx = ep.shard_idx, nid = ep.node_id]() {
        RpcSearchPhase1Request local_req;
        {
          std::lock_guard<std::mutex> lk(sctx->mu);
          local_req = sctx->plan.phase1_req;
        }
        RpcSearchPhase1Response resp = ExecuteLocalSearchPhase1(local_req);
        OnPhase1ShardDone(sctx, sidx, nid, true, "", std::move(resp));
      });
    } else {
      mgr.RpcClient().SendAsync(
          ep.node_id, ep.ip, ep.rpc_port, RpcOpCode::kRpcSearchPhase1, encoded_p1, rem_timeout,
          [sctx, sidx = ep.shard_idx, nid = ep.node_id](RpcResponse r) {
            if (r.status != RpcStatus::kOk) {
              OnPhase1ShardDone(sctx, sidx, nid, false,
                                std::string(RpcStatusToString(r.status)) +
                                    (r.error_msg.empty() ? "" : (": " + r.error_msg)),
                                {});
              return;
            }
            if (r.flags & kRpcFlagError) {
              OnPhase1ShardDone(sctx, sidx, nid, false,
                                r.error_msg.empty() ? "remote query error" : r.error_msg, {});
              return;
            }
            RpcSearchPhase1Response p1resp;
            if (!DecodeSearchPhase1Response(r.payload, p1resp)) {
              OnPhase1ShardDone(sctx, sidx, nid, false, "malformed Phase1 response", {});
              return;
            }
            OnPhase1ShardDone(sctx, sidx, nid, true, "", std::move(p1resp));
          });
    }
  }
}

void OnSearchPhase0DfsDone(const std::shared_ptr<ClusterSearchContext>& sctx,
                           const std::string& node_id, bool ok, const std::string& err,
                           RpcDfsTermStatsResponse&& resp) {
  bool launch_p1 = false;
  RedisModuleBlockedClient* to_unblock = nullptr;
  {
    std::lock_guard<std::mutex> lk(sctx->mu);
    if (sctx->finished || sctx->cancelled) return;
    if (!ok || !resp.ok) {
      if (!sctx->on_timeout_return && sctx->error_msg.empty()) {
        std::string reason = !ok ? err : resp.error_msg;
        sctx->error_msg = "ERR cluster shard DFS stats failed (" +
                          (node_id.size() > 8 ? node_id.substr(0, 8) : node_id) + "): " + reason;
      }
    } else {
      sctx->dfs_shard_stats.push_back(std::move(resp.fields));
    }

    if (sctx->phase0_pending > 0) sctx->phase0_pending--;
    if (sctx->phase0_pending == 0 && !sctx->finished) {
      if (!sctx->error_msg.empty()) {
        sctx->finished = true;
        to_unblock = sctx->bc;
        sctx->bc = nullptr;
      } else {
        sctx->plan.phase1_req.global_bm25_stats =
            AggregateGlobalBm25FieldStats(sctx->dfs_shard_stats);
        launch_p1 = true;
      }
    }
  }
  if (to_unblock) {
    UnblockSearchClient(sctx, to_unblock);
    return;
  }
  if (launch_p1) {
    DispatchSearchPhase1(sctx);
  }
}

// ============================================================================
// Phase 69: Generic R2-Safe Blocked Client Registry + Shard RPC Handlers
// ============================================================================

template <typename CtxT>
struct BlockedRegistry {
  std::mutex mu;
  std::unordered_map<RedisModuleBlockedClient*, std::shared_ptr<CtxT>> active;

  void Register(RedisModuleBlockedClient* bc, const std::shared_ptr<CtxT>& ctx) {
    std::lock_guard<std::mutex> lk(mu);
    active[bc] = ctx;
  }

  void Unblock(const std::shared_ptr<CtxT>& ctx, RedisModuleBlockedClient* bc) {
    if (!bc) return;
    {
      std::lock_guard<std::mutex> lk(mu);
      active.erase(bc);
    }
    auto* holder = new std::shared_ptr<CtxT>(ctx);
    RedisModule_UnblockClient(bc, holder);
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

template <typename CtxT>
void FreeSharedHolder(RedisModuleCtx* /*ctx*/, void* privdata) {
  delete static_cast<std::shared_ptr<CtxT>*>(privdata);
}

int GenericTimeoutCallback(RedisModuleCtx* ctx, RedisModuleString** /*argv*/, int /*argc*/) {
  return RedisModule_ReplyWithError(ctx, "ERR cluster request timed out");
}

bool CollectHealthyMasterShards(RedisModuleCtx* ctx, bool on_timeout_return,
                                std::vector<SearchShardEndpoint>& out_shards,
                                std::string& err_out) {
  auto& mgr = ClusterManager::Instance();
  mgr.RefreshTopology(ctx);
  auto topo = mgr.GetTopology();
  out_shards.clear();
  if (!topo) return true;
  uint32_t sidx = 0;
  for (const auto& m : topo->masters) {
    if (!m.is_healthy) continue;
    if (!m.is_myself && (!m.rpc_port_discovered || m.rpc_port <= 0 || m.ip.empty())) {
      if (!on_timeout_return) {
        err_out = "ERR cluster shard RPC endpoint not ready (" +
                  (m.node_id.size() > 8 ? m.node_id.substr(0, 8) : m.node_id) + ")";
        return false;
      }
      continue;
    }
    out_shards.push_back({sidx++, m.node_id, m.ip, m.rpc_port, m.is_myself});
    if (out_shards.size() >= kMaxClusterFanout) break;
  }
  return true;
}

int64_t MonotonicUs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000LL + static_cast<int64_t>(ts.tv_nsec) / 1000LL;
}

// ── Phase 69 Shard RPC Handlers ──

void HandleDfsTermStatsRequest(const RpcFrame& req_frame, const std::string& /*peer_ip*/,
                               ClusterRpcServer::ResponseSender send_resp) {
  RpcDfsTermStatsRequest req;
  if (!DecodeDfsTermStatsRequest(req_frame.payload, req)) {
    RpcDfsTermStatsResponse resp;
    resp.ok = false;
    resp.error_msg = "malformed DfsTermStats request";
    std::string out;
    EncodeDfsTermStatsResponse(resp, out);
    send_resp(RpcOpCode::kRpcDfsTermStatsResp, kRpcFlagNone, std::move(out));
    return;
  }
  GetSearchExecutor().Submit(
      [req = std::move(req), send_resp = std::move(send_resp)]() mutable {
        RpcDfsTermStatsResponse resp = ExecuteLocalDfsTermStats(req);
        std::string out;
        EncodeDfsTermStatsResponse(resp, out);
        send_resp(RpcOpCode::kRpcDfsTermStatsResp, kRpcFlagNone, std::move(out));
      });
}

void HandleHybridPhase1Request(const RpcFrame& req_frame, const std::string& /*peer_ip*/,
                               ClusterRpcServer::ResponseSender send_resp) {
  RpcHybridPhase1Request req;
  if (!DecodeHybridPhase1Request(req_frame.payload, req)) {
    RpcHybridPhase1Response resp;
    resp.ok = false;
    resp.error_msg = "malformed HybridPhase1 request";
    std::string out;
    EncodeHybridPhase1Response(resp, out);
    send_resp(RpcOpCode::kRpcHybridPhase1Resp, kRpcFlagNone, std::move(out));
    return;
  }
  GetSearchExecutor().Submit(
      [req = std::move(req), send_resp = std::move(send_resp)]() mutable {
        RpcHybridPhase1Response resp = ExecuteLocalHybridPhase1(req);
        std::string out;
        EncodeHybridPhase1Response(resp, out);
        send_resp(RpcOpCode::kRpcHybridPhase1Resp, kRpcFlagNone, std::move(out));
      });
}

void HandleInfoStatsRequest(const RpcFrame& req_frame, const std::string& /*peer_ip*/,
                            ClusterRpcServer::ResponseSender send_resp) {
  RpcInfoStatsRequest req;
  if (!DecodeInfoStatsRequest(req_frame.payload, req)) {
    RpcInfoStatsResponse resp;
    resp.ok = false;
    resp.error_msg = "malformed InfoStats request";
    std::string out;
    EncodeInfoStatsResponse(resp, out);
    send_resp(RpcOpCode::kRpcInfoStatsResp, kRpcFlagNone, std::move(out));
    return;
  }
  GetSearchExecutor().Submit(
      [req = std::move(req), send_resp = std::move(send_resp)]() mutable {
        RpcInfoStatsResponse resp = ExecuteLocalInfoStats(req);
        std::string out;
        EncodeInfoStatsResponse(resp, out);
        send_resp(RpcOpCode::kRpcInfoStatsResp, kRpcFlagNone, std::move(out));
      });
}

void HandleTagValsRequest(const RpcFrame& req_frame, const std::string& /*peer_ip*/,
                          ClusterRpcServer::ResponseSender send_resp) {
  RpcTagValsRequest req;
  if (!DecodeTagValsRequest(req_frame.payload, req)) {
    RpcTagValsResponse resp;
    resp.ok = false;
    resp.error_msg = "malformed TagVals request";
    std::string out;
    EncodeTagValsResponse(resp, out);
    send_resp(RpcOpCode::kRpcTagValsResp, kRpcFlagNone, std::move(out));
    return;
  }
  GetSearchExecutor().Submit(
      [req = std::move(req), send_resp = std::move(send_resp)]() mutable {
        RpcTagValsResponse resp = ExecuteLocalTagVals(req);
        std::string out;
        EncodeTagValsResponse(resp, out);
        send_resp(RpcOpCode::kRpcTagValsResp, kRpcFlagNone, std::move(out));
      });
}

void HandleSpellCheckRequest(const RpcFrame& req_frame, const std::string& /*peer_ip*/,
                             ClusterRpcServer::ResponseSender send_resp) {
  RpcSpellCheckRequest req;
  if (!DecodeSpellCheckRequest(req_frame.payload, req)) {
    RpcSpellCheckResponse resp;
    resp.ok = false;
    resp.error_msg = "malformed SpellCheck request";
    std::string out;
    EncodeSpellCheckResponse(resp, out);
    send_resp(RpcOpCode::kRpcSpellCheckResp, kRpcFlagNone, std::move(out));
    return;
  }
  GetSearchExecutor().Submit(
      [req = std::move(req), send_resp = std::move(send_resp)]() mutable {
        RpcSpellCheckResponse resp = ExecuteLocalSpellCheck(req);
        std::string out;
        EncodeSpellCheckResponse(resp, out);
        send_resp(RpcOpCode::kRpcSpellCheckResp, kRpcFlagNone, std::move(out));
      });
}

void HandleSuggestRequest(const RpcFrame& req_frame, const std::string& /*peer_ip*/,
                          ClusterRpcServer::ResponseSender send_resp) {
  RpcSuggestRequest req;
  if (!DecodeSuggestRequest(req_frame.payload, req)) {
    RpcSuggestResponse resp;
    resp.ok = false;
    resp.error_msg = "malformed Suggest request";
    std::string out;
    EncodeSuggestResponse(resp, out);
    send_resp(RpcOpCode::kRpcSuggestResp, kRpcFlagNone, std::move(out));
    return;
  }

  bool is_write = MatchArg(req.cmd_name, "FT.SUGADD") || MatchArg(req.cmd_name, "FT.SUGDEL");
  GetSearchExecutor().Submit(
      [req = std::move(req), is_write, send_resp = std::move(send_resp)]() mutable {
        RpcSuggestResponse resp;
        bool mutated = false;
        if (!is_write) {
          // Pure read: ZERO remote Redis main thread touch!
          ApplyLocalSuggestCommand(req.cmd_name, req.args, mutated, resp);
        } else {
          auto& mgr = ClusterManager::Instance();
          if (mgr.IsShuttingDown()) {
            resp.ok = false;
            resp.error_msg = "ERR node shutting down";
          } else {
            std::lock_guard<std::mutex> dlk(mgr.DetachedCtxMutex());
            RedisModuleCtx* dctx = mgr.DetachedCtx();
            if (!dctx || mgr.IsShuttingDown()) {
              resp.ok = false;
              resp.error_msg = "ERR node thread-safe context unavailable";
            } else {
              RedisModule_ThreadSafeContextLock(dctx);
              if (ApplyLocalSuggestCommand(req.cmd_name, req.args, mutated, resp) && mutated) {
                ReplicateDdlCommand(dctx, req.cmd_name, req.args);
              }
              RedisModule_ThreadSafeContextUnlock(dctx);
            }
          }
        }
        std::string out;
        EncodeSuggestResponse(resp, out);
        send_resp(RpcOpCode::kRpcSuggestResp, kRpcFlagNone, std::move(out));
      });
}

// ── Phase 69: FT.HYBRID Coordinator State Machine ──

struct ClusterHybridFinalRow {
  HybridHit hit;
  bool has_payload = false;
  std::string payload;
  std::vector<std::pair<std::string, std::string>> fields;
};

struct ClusterHybridContext {
  RedisModuleBlockedClient* bc = nullptr;
  ClusterHybridPlan plan;
  std::vector<SearchShardEndpoint> shards;
  int64_t start_ms = 0;
  bool on_timeout_return = false;

  std::mutex mu;
  size_t phase0_pending = 0;
  size_t phase1_pending = 0;
  size_t phase2_pending = 0;
  bool cancelled = false;
  bool finished = false;
  std::string error_msg;

  std::vector<std::vector<RpcFieldBm25Stats>> dfs_shard_stats;
  std::vector<ShardHybridSlice> slices;

  bool count_only = false;
  uint64_t total_fused = 0;
  std::vector<ClusterHybridFinalRow> final_rows;
};

BlockedRegistry<ClusterHybridContext> g_active_hybrids;

void ClusterHybridDisconnectCallback(RedisModuleCtx* /*ctx*/, RedisModuleBlockedClient* bc) {
  g_active_hybrids.OnDisconnect(bc);
}

int ClusterHybridReplyCallback(RedisModuleCtx* ctx, RedisModuleString** /*argv*/, int /*argc*/) {
  auto* holder = static_cast<std::shared_ptr<ClusterHybridContext>*>(
      RedisModule_GetBlockedClientPrivateData(ctx));
  if (!holder || !(*holder)) {
    return RedisModule_ReplyWithError(ctx, "ERR internal cluster hybrid state missing");
  }
  auto& hctx = **holder;
  std::lock_guard<std::mutex> lk(hctx.mu);
  if (!hctx.error_msg.empty()) {
    return RedisModule_ReplyWithError(ctx, hctx.error_msg.c_str());
  }
  if (hctx.count_only) {
    RedisModule_ReplyWithArray(ctx, 1);
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(hctx.total_fused));
    return REDISMODULE_OK;
  }

  const auto& plan = hctx.plan;
  long total = static_cast<long>(hctx.final_rows.size());
  long mult = 1 + (plan.withpayloads ? 1 : 0) + (plan.nocontent ? 0 : 1);
  RedisModule_ReplyWithArray(ctx, 1 + total * mult);
  RedisModule_ReplyWithLongLong(ctx, total);

  for (const auto& row : hctx.final_rows) {
    const auto& h = row.hit;
    RedisModule_ReplyWithStringBuffer(ctx, h.doc_id.data(), h.doc_id.size());
    if (plan.withpayloads) {
      if (row.has_payload) {
        RedisModule_ReplyWithStringBuffer(ctx, row.payload.data(), row.payload.size());
      } else {
        RedisModule_ReplyWithNull(ctx);
      }
    }
    if (!plan.nocontent) {
      long extra = plan.withscores ? (plan.explainscore ? 4 : 2) : 0;
      RedisModule_ReplyWithArray(ctx, static_cast<long>(row.fields.size()) * 2 + extra);
      if (plan.withscores) {
        std::string score_str = std::to_string(h.score);
        RedisModule_ReplyWithCString(ctx, "__hybrid_score");
        RedisModule_ReplyWithStringBuffer(ctx, score_str.data(), score_str.size());
        if (plan.explainscore) {
          std::string expl =
              (plan.hcfg.method == FusionMethod::kRrf)
                  ? ("RRF(k=" + std::to_string(plan.hcfg.rrf_k) +
                     ",text_rank=" + std::to_string(h.text_rank) +
                     ",vec_rank=" + std::to_string(h.vector_rank) + ")")
                  : ("LINEAR(alpha=" + std::to_string(plan.hcfg.alpha) +
                     ",text_score=" + std::to_string(h.text_score) +
                     ",vec_dist=" + std::to_string(h.vector_distance) + ")");
          RedisModule_ReplyWithCString(ctx, "__score_explanation");
          RedisModule_ReplyWithStringBuffer(ctx, expl.data(), expl.size());
        }
      }
      for (const auto& [k, v] : row.fields) {
        RedisModule_ReplyWithStringBuffer(ctx, k.data(), k.size());
        RedisModule_ReplyWithStringBuffer(ctx, v.data(), v.size());
      }
    }
  }
  return REDISMODULE_OK;
}

void OnHybridPhase2FetchDone(const std::shared_ptr<ClusterHybridContext>& hctx,
                             const std::vector<size_t>& winner_indices, const std::string& node_id,
                             bool ok, const std::string& err, RpcFetchDocsResponse&& resp) {
  RedisModuleBlockedClient* to_unblock = nullptr;
  {
    std::lock_guard<std::mutex> lk(hctx->mu);
    if (hctx->finished || hctx->cancelled) return;
    if (!ok || !resp.ok) {
      if (!hctx->on_timeout_return && hctx->error_msg.empty()) {
        std::string reason = !ok ? err : resp.error_msg;
        hctx->error_msg = "ERR cluster hybrid fetch failed (" +
                          (node_id.size() > 8 ? node_id.substr(0, 8) : node_id) + "): " + reason;
      }
    } else {
      size_t n = std::min(winner_indices.size(), resp.docs.size());
      for (size_t i = 0; i < n; i++) {
        size_t wi = winner_indices[i];
        if (wi < hctx->final_rows.size()) {
          hctx->final_rows[wi].has_payload = resp.docs[i].has_payload;
          hctx->final_rows[wi].payload = std::move(resp.docs[i].payload);
          hctx->final_rows[wi].fields = std::move(resp.docs[i].fields);
        }
      }
    }
    if (hctx->phase2_pending > 0) hctx->phase2_pending--;
    if (hctx->phase2_pending == 0 && !hctx->finished) {
      hctx->finished = true;
      to_unblock = hctx->bc;
      hctx->bc = nullptr;
    }
  }
  g_active_hybrids.Unblock(hctx, to_unblock);
}

void OnHybridPhase1ShardDone(const std::shared_ptr<ClusterHybridContext>& hctx, uint32_t shard_idx,
                             const std::string& node_id, bool ok, const std::string& err,
                             RpcHybridPhase1Response&& resp) {
  bool start_phase2 = false;
  std::unordered_map<uint32_t, std::vector<size_t>> fetch_plan;
  RedisModuleBlockedClient* to_unblock = nullptr;

  {
    std::lock_guard<std::mutex> lk(hctx->mu);
    if (hctx->finished || hctx->cancelled) return;
    if (!ok || !resp.ok) {
      if (!hctx->on_timeout_return && hctx->error_msg.empty()) {
        std::string reason = !ok ? err : resp.error_msg;
        hctx->error_msg = "ERR cluster hybrid shard query failed (" +
                          (node_id.size() > 8 ? node_id.substr(0, 8) : node_id) + "): " + reason;
      }
    } else {
      ShardHybridSlice slice;
      slice.shard_idx = shard_idx;
      slice.text_hits.reserve(resp.text_hits.size());
      for (auto& [id, sc] : resp.text_hits) slice.text_hits.push_back({std::move(id), sc});
      slice.vec_hits.reserve(resp.vec_hits.size());
      for (auto& [id, d] : resp.vec_hits) slice.vec_hits.push_back({std::move(id), d});
      hctx->slices.push_back(std::move(slice));
    }

    if (hctx->phase1_pending > 0) hctx->phase1_pending--;
    if (hctx->phase1_pending == 0 && !hctx->finished) {
      if (!hctx->error_msg.empty()) {
        hctx->finished = true;
        to_unblock = hctx->bc;
        hctx->bc = nullptr;
      } else {
        const auto& plan = hctx->plan;
        MergedHybridResult merged = MergeHybridShards(
            hctx->slices, plan.phase1_req.text_limit, static_cast<size_t>(plan.phase1_req.knn_k),
            plan.hcfg, plan.limit_offset, plan.limit_count, plan.has_limit);
        hctx->total_fused = merged.total_fused;
        if (plan.count_only) {
          hctx->count_only = true;
          hctx->finished = true;
          to_unblock = hctx->bc;
          hctx->bc = nullptr;
        } else {
          hctx->final_rows.reserve(merged.winners.size());
          for (auto& w : merged.winners) {
            ClusterHybridFinalRow row;
            row.hit = std::move(w);
            hctx->final_rows.push_back(std::move(row));
          }
          bool skip_fetch = hctx->final_rows.empty() || (plan.nocontent && !plan.withpayloads);
          if (skip_fetch || merged.winners_by_shard.empty()) {
            hctx->finished = true;
            to_unblock = hctx->bc;
            hctx->bc = nullptr;
          } else {
            fetch_plan = std::move(merged.winners_by_shard);
            hctx->phase2_pending = fetch_plan.size();
            start_phase2 = true;
          }
        }
      }
    }
  }

  if (to_unblock) {
    g_active_hybrids.Unblock(hctx, to_unblock);
    return;
  }
  if (!start_phase2) return;

  auto& mgr = ClusterManager::Instance();
  int64_t elapsed = ClusterNowMs() - hctx->start_ms;
  int64_t rem_timeout = std::max<int64_t>(200, hctx->plan.timeout_ms - elapsed);

  for (auto& [sidx, winner_indices] : fetch_plan) {
    if (sidx >= hctx->shards.size()) {
      OnHybridPhase2FetchDone(hctx, winner_indices, "unknown", false, "invalid shard index", {});
      continue;
    }
    const auto& ep = hctx->shards[sidx];
    RpcFetchDocsRequest freq = hctx->plan.fetch_template;
    freq.doc_ids.reserve(winner_indices.size());
    for (size_t wi : winner_indices) {
      freq.doc_ids.push_back(hctx->final_rows[wi].hit.doc_id);
    }

    if (ep.is_myself) {
      GetSearchExecutor().Submit(
          [hctx, winner_indices = std::move(winner_indices), nid = ep.node_id,
           freq = std::move(freq)]() mutable {
            RpcFetchDocsResponse fresp = ExecuteLocalFetchDocs(freq);
            OnHybridPhase2FetchDone(hctx, winner_indices, nid, true, "", std::move(fresp));
          });
    } else {
      std::string encoded_fetch;
      EncodeFetchDocsRequest(freq, encoded_fetch);
      mgr.RpcClient().SendAsync(
          ep.node_id, ep.ip, ep.rpc_port, RpcOpCode::kRpcFetchDocs, encoded_fetch, rem_timeout,
          [hctx, winner_indices = std::move(winner_indices), nid = ep.node_id](RpcResponse r) {
            if (r.status != RpcStatus::kOk) {
              OnHybridPhase2FetchDone(hctx, winner_indices, nid, false,
                                      std::string(RpcStatusToString(r.status)) +
                                          (r.error_msg.empty() ? "" : (": " + r.error_msg)),
                                      {});
              return;
            }
            if (r.flags & kRpcFlagError) {
              OnHybridPhase2FetchDone(hctx, winner_indices, nid, false,
                                      r.error_msg.empty() ? "remote fetch error" : r.error_msg, {});
              return;
            }
            RpcFetchDocsResponse fresp;
            if (!DecodeFetchDocsResponse(r.payload, fresp)) {
              OnHybridPhase2FetchDone(hctx, winner_indices, nid, false, "malformed fetch response",
                                      {});
              return;
            }
            OnHybridPhase2FetchDone(hctx, winner_indices, nid, true, "", std::move(fresp));
          });
    }
  }
}

void DispatchHybridPhase1(const std::shared_ptr<ClusterHybridContext>& hctx) {
  auto& mgr = ClusterManager::Instance();
  std::string encoded_p1;
  int64_t rem_timeout = 5000;
  {
    std::lock_guard<std::mutex> lk(hctx->mu);
    if (hctx->finished || hctx->cancelled) return;
    int64_t elapsed = ClusterNowMs() - hctx->start_ms;
    rem_timeout = std::max<int64_t>(200, (hctx->plan.timeout_ms > 0 ? hctx->plan.timeout_ms : 5000) -
                                             elapsed);
    hctx->plan.phase1_req.remaining_timeout_ms = rem_timeout;
    hctx->phase1_pending = hctx->shards.size();
    EncodeHybridPhase1Request(hctx->plan.phase1_req, encoded_p1);
  }

  for (const auto& ep : hctx->shards) {
    if (ep.is_myself) {
      GetSearchExecutor().Submit([hctx, sidx = ep.shard_idx, nid = ep.node_id]() {
        RpcHybridPhase1Request local_req;
        {
          std::lock_guard<std::mutex> lk(hctx->mu);
          local_req = hctx->plan.phase1_req;
        }
        RpcHybridPhase1Response resp = ExecuteLocalHybridPhase1(local_req);
        OnHybridPhase1ShardDone(hctx, sidx, nid, true, "", std::move(resp));
      });
    } else {
      mgr.RpcClient().SendAsync(
          ep.node_id, ep.ip, ep.rpc_port, RpcOpCode::kRpcHybridPhase1, encoded_p1, rem_timeout,
          [hctx, sidx = ep.shard_idx, nid = ep.node_id](RpcResponse r) {
            if (r.status != RpcStatus::kOk) {
              OnHybridPhase1ShardDone(hctx, sidx, nid, false,
                                      std::string(RpcStatusToString(r.status)) +
                                          (r.error_msg.empty() ? "" : (": " + r.error_msg)),
                                      {});
              return;
            }
            if (r.flags & kRpcFlagError) {
              OnHybridPhase1ShardDone(hctx, sidx, nid, false,
                                      r.error_msg.empty() ? "remote hybrid error" : r.error_msg,
                                      {});
              return;
            }
            RpcHybridPhase1Response hresp;
            if (!DecodeHybridPhase1Response(r.payload, hresp)) {
              OnHybridPhase1ShardDone(hctx, sidx, nid, false, "malformed hybrid response", {});
              return;
            }
            OnHybridPhase1ShardDone(hctx, sidx, nid, true, "", std::move(hresp));
          });
    }
  }
}

void OnHybridPhase0DfsDone(const std::shared_ptr<ClusterHybridContext>& hctx,
                           const std::string& node_id, bool ok, const std::string& err,
                           RpcDfsTermStatsResponse&& resp) {
  bool launch_p1 = false;
  RedisModuleBlockedClient* to_unblock = nullptr;
  {
    std::lock_guard<std::mutex> lk(hctx->mu);
    if (hctx->finished || hctx->cancelled) return;
    if (!ok || !resp.ok) {
      if (!hctx->on_timeout_return && hctx->error_msg.empty()) {
        std::string reason = !ok ? err : resp.error_msg;
        hctx->error_msg = "ERR cluster hybrid DFS failed (" +
                          (node_id.size() > 8 ? node_id.substr(0, 8) : node_id) + "): " + reason;
      }
    } else {
      hctx->dfs_shard_stats.push_back(std::move(resp.fields));
    }
    if (hctx->phase0_pending > 0) hctx->phase0_pending--;
    if (hctx->phase0_pending == 0 && !hctx->finished) {
      if (!hctx->error_msg.empty()) {
        hctx->finished = true;
        to_unblock = hctx->bc;
        hctx->bc = nullptr;
      } else {
        hctx->plan.phase1_req.global_bm25_stats =
            AggregateGlobalBm25FieldStats(hctx->dfs_shard_stats);
        launch_p1 = true;
      }
    }
  }
  if (to_unblock) {
    g_active_hybrids.Unblock(hctx, to_unblock);
    return;
  }
  if (launch_p1) {
    DispatchHybridPhase1(hctx);
  }
}

// ── Phase 69: FT.INFO Coordinator State Machine ──

struct ClusterInfoContext {
  RedisModuleBlockedClient* bc = nullptr;
  std::string index_name;
  bool on_timeout_return = false;

  std::mutex mu;
  size_t pending = 0;
  bool cancelled = false;
  bool finished = false;
  std::string error_msg;

  struct ShardInfoRow {
    std::string node_id;
    RpcInfoStatsResponse stats;
  };
  std::vector<ShardInfoRow> shard_rows;
};

BlockedRegistry<ClusterInfoContext> g_active_infos;

void ClusterInfoDisconnectCallback(RedisModuleCtx* /*ctx*/, RedisModuleBlockedClient* bc) {
  g_active_infos.OnDisconnect(bc);
}

int ClusterInfoReplyCallback(RedisModuleCtx* ctx, RedisModuleString** /*argv*/, int /*argc*/) {
  auto* holder = static_cast<std::shared_ptr<ClusterInfoContext>*>(
      RedisModule_GetBlockedClientPrivateData(ctx));
  if (!holder || !(*holder)) {
    return RedisModule_ReplyWithError(ctx, "ERR internal cluster info state missing");
  }
  auto& ictx = **holder;
  std::lock_guard<std::mutex> lk(ictx.mu);
  if (!ictx.error_msg.empty()) {
    return RedisModule_ReplyWithError(ctx, ictx.error_msg.c_str());
  }

  auto entry_ptr = GetIndexEntryShared(ictx.index_name);
  if (!entry_ptr) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }

  uint64_t total_docs = 0;
  uint64_t total_terms = 0;
  uint64_t total_inv_bytes = 0;
  uint64_t total_vec_bytes = 0;
  uint64_t total_cursor_idle = 0;
  uint64_t total_cursor_created = 0;
  uint64_t total_cursor_expired = 0;
  bool any_indexing = false;
  double weighted_done = 0.0;
  double weighted_total = 0.0;

  std::sort(ictx.shard_rows.begin(), ictx.shard_rows.end(),
            [](const auto& a, const auto& b) { return a.node_id < b.node_id; });

  std::vector<std::tuple<std::string, uint64_t, bool, double>> shard_summary;
  shard_summary.reserve(ictx.shard_rows.size());

  for (const auto& sr : ictx.shard_rows) {
    const auto& st = sr.stats;
    total_docs += st.num_docs;
    total_terms += st.num_terms;
    total_inv_bytes += st.inverted_bytes;
    total_vec_bytes += st.vector_bytes;
    total_cursor_idle += st.cursor_idle;
    total_cursor_created += st.cursor_total_created;
    total_cursor_expired += st.cursor_total_expired;
    if (st.bg_indexing) any_indexing = true;

    if (st.bg_indexing && st.bg_total_keys > 0) {
      weighted_done += static_cast<double>(std::min(st.bg_scanned_keys, st.bg_total_keys));
      weighted_total += static_cast<double>(st.bg_total_keys);
    } else if (!st.bg_indexing) {
      double w = static_cast<double>(std::max<uint64_t>(st.num_docs, 1));
      weighted_done += w;
      weighted_total += w;
    } else {
      weighted_done += std::clamp(st.percent_indexed, 0.0, 1.0);
      weighted_total += 1.0;
    }

    shard_summary.emplace_back(sr.node_id, st.num_docs, st.bg_indexing, st.percent_indexed);
  }

  double global_pct = 1.0;
  if (any_indexing) {
    global_pct = (weighted_total > 0.0) ? (weighted_done / weighted_total) : 0.0;
    if (global_pct < 0.0) global_pct = 0.0;
    if (global_pct > 0.99) global_pct = 0.99;
  }

  std::shared_lock<std::shared_mutex> idx_lk(entry_ptr->Mutex());
  ReplyWithIndexInfo(ctx, *entry_ptr, total_docs, total_terms, total_inv_bytes, total_vec_bytes,
                     any_indexing, global_pct, total_cursor_idle, total_cursor_created,
                     total_cursor_expired, &shard_summary);
  return REDISMODULE_OK;
}

// ── Phase 69: FT.TAGVALS Coordinator State Machine ──

struct ClusterTagValsContext {
  RedisModuleBlockedClient* bc = nullptr;
  bool on_timeout_return = false;

  std::mutex mu;
  size_t pending = 0;
  bool cancelled = false;
  bool finished = false;
  std::string error_msg;
  std::set<std::string> all_tags;
};

BlockedRegistry<ClusterTagValsContext> g_active_tagvals;

void ClusterTagValsDisconnectCallback(RedisModuleCtx* /*ctx*/, RedisModuleBlockedClient* bc) {
  g_active_tagvals.OnDisconnect(bc);
}

int ClusterTagValsReplyCallback(RedisModuleCtx* ctx, RedisModuleString** /*argv*/, int /*argc*/) {
  auto* holder = static_cast<std::shared_ptr<ClusterTagValsContext>*>(
      RedisModule_GetBlockedClientPrivateData(ctx));
  if (!holder || !(*holder)) {
    return RedisModule_ReplyWithError(ctx, "ERR internal cluster tagvals state missing");
  }
  auto& tctx = **holder;
  std::lock_guard<std::mutex> lk(tctx.mu);
  if (!tctx.error_msg.empty()) {
    return RedisModule_ReplyWithError(ctx, tctx.error_msg.c_str());
  }
  RedisModule_ReplyWithArray(ctx, static_cast<long>(tctx.all_tags.size()));
  for (const auto& t : tctx.all_tags) {
    RedisModule_ReplyWithStringBuffer(ctx, t.data(), t.size());
  }
  return REDISMODULE_OK;
}

// ── Phase 69: FT.SPELLCHECK Coordinator State Machine ──

struct ClusterSpellCheckContext {
  RedisModuleBlockedClient* bc = nullptr;
  RpcSpellCheckRequest req;
  bool on_timeout_return = false;

  std::mutex mu;
  size_t pending = 0;
  bool cancelled = false;
  bool finished = false;
  std::string error_msg;

  std::unordered_set<std::string> known_terms;
  std::unordered_map<std::string, std::unordered_map<std::string, double>> scores_by_term;
};

BlockedRegistry<ClusterSpellCheckContext> g_active_spellchecks;

void ClusterSpellCheckDisconnectCallback(RedisModuleCtx* /*ctx*/, RedisModuleBlockedClient* bc) {
  g_active_spellchecks.OnDisconnect(bc);
}

int ClusterSpellCheckReplyCallback(RedisModuleCtx* ctx, RedisModuleString** /*argv*/,
                                   int /*argc*/) {
  auto* holder = static_cast<std::shared_ptr<ClusterSpellCheckContext>*>(
      RedisModule_GetBlockedClientPrivateData(ctx));
  if (!holder || !(*holder)) {
    return RedisModule_ReplyWithError(ctx, "ERR internal cluster spellcheck state missing");
  }
  auto& sctx = **holder;
  std::lock_guard<std::mutex> lk(sctx.mu);
  if (!sctx.error_msg.empty()) {
    return RedisModule_ReplyWithError(ctx, sctx.error_msg.c_str());
  }

  auto terms = TextIndex::Tokenize(sctx.req.query_str);
  std::unordered_set<std::string> known = sctx.known_terms;
  std::unordered_set<std::string> excluded;
  {
    std::shared_lock<std::shared_mutex> dlk(GetTermDictsMutex());
    for (const auto& dname : sctx.req.include_dicts) {
      auto dit = GetTermDicts().find(dname);
      if (dit != GetTermDicts().end()) {
        for (const auto& term : terms) {
          if (dit->second.Contains(term)) known.insert(term);
        }
      }
    }
    for (const auto& dname : sctx.req.exclude_dicts) {
      auto dit = GetTermDicts().find(dname);
      if (dit != GetTermDicts().end()) {
        for (const auto& term : terms) {
          if (dit->second.Contains(term)) excluded.insert(term);
        }
      }
    }
  }

  struct SpellOut {
    std::string term;
    std::vector<std::pair<double, std::string>> suggestions;
  };
  std::vector<SpellOut> results;

  for (const auto& term : terms) {
    if (known.count(term) || excluded.count(term)) continue;
    std::unordered_map<std::string, double> sug_scores;
    auto tit = sctx.scores_by_term.find(term);
    if (tit != sctx.scores_by_term.end()) sug_scores = tit->second;

    // Add include_dicts candidates once on Coordinator
    if (!sctx.req.include_dicts.empty()) {
      std::shared_lock<std::shared_mutex> dlk(GetTermDictsMutex());
      for (const auto& dname : sctx.req.include_dicts) {
        auto dit = GetTermDicts().find(dname);
        if (dit == GetTermDicts().end()) continue;
        for (const auto& dt : dit->second.Dump()) {
          int dist = TextIndex::LevenshteinDistance(term, dt);
          if (dist <= sctx.req.max_dist && dist > 0) {
            sug_scores[dt] += 1.0 / static_cast<double>(dist);
          }
        }
      }
    }

    SpellOut out;
    out.term = term;
    for (auto& [sug, sc] : sug_scores) {
      if (!excluded.count(sug)) out.suggestions.emplace_back(sc, sug);
    }
    std::sort(out.suggestions.begin(), out.suggestions.end(), [](const auto& a, const auto& b) {
      if (a.first != b.first) return a.first > b.first;
      return a.second < b.second;
    });
    results.push_back(std::move(out));
  }

  RedisModule_ReplyWithArray(ctx, static_cast<long>(results.size()));
  for (const auto& ss : results) {
    RedisModule_ReplyWithArray(ctx, 2 + static_cast<long>(ss.suggestions.size()));
    RedisModule_ReplyWithSimpleString(ctx, "TERM");
    RedisModule_ReplyWithStringBuffer(ctx, ss.term.data(), ss.term.size());
    for (const auto& [sc, sug] : ss.suggestions) {
      (void)sc;
      RedisModule_ReplyWithStringBuffer(ctx, sug.data(), sug.size());
    }
  }
  return REDISMODULE_OK;
}

// ── Phase 69: FT.SUG* Coordinator Forwarding State Machine ──

struct ClusterSuggestContext {
  RedisModuleBlockedClient* bc = nullptr;
  ClusterSuggestPlan plan;

  std::mutex mu;
  bool cancelled = false;
  bool finished = false;
  std::string error_msg;
  RpcSuggestResponse resp;
};

BlockedRegistry<ClusterSuggestContext> g_active_suggests;

void ClusterSuggestDisconnectCallback(RedisModuleCtx* /*ctx*/, RedisModuleBlockedClient* bc) {
  g_active_suggests.OnDisconnect(bc);
}

int ClusterSuggestReplyCallback(RedisModuleCtx* ctx, RedisModuleString** /*argv*/, int /*argc*/) {
  auto* holder = static_cast<std::shared_ptr<ClusterSuggestContext>*>(
      RedisModule_GetBlockedClientPrivateData(ctx));
  if (!holder || !(*holder)) {
    return RedisModule_ReplyWithError(ctx, "ERR internal cluster suggest state missing");
  }
  auto& sctx = **holder;
  std::lock_guard<std::mutex> lk(sctx.mu);
  if (!sctx.error_msg.empty()) {
    return RedisModule_ReplyWithError(ctx, sctx.error_msg.c_str());
  }
  if (MatchArg(sctx.plan.cmd_name, "FT.SUGGET")) {
    long mult = 1 + (sctx.plan.sugget_withscores ? 1 : 0) +
                (sctx.plan.sugget_withpayloads ? 1 : 0);
    RedisModule_ReplyWithArray(ctx, static_cast<long>(sctx.resp.entries.size()) * mult);
    for (const auto& e : sctx.resp.entries) {
      RedisModule_ReplyWithStringBuffer(ctx, e.str.data(), e.str.size());
      if (sctx.plan.sugget_withscores) {
        std::string sc = std::to_string(e.score);
        RedisModule_ReplyWithStringBuffer(ctx, sc.data(), sc.size());
      }
      if (sctx.plan.sugget_withpayloads) {
        RedisModule_ReplyWithStringBuffer(ctx, e.payload.data(), e.payload.size());
      }
    }
    return REDISMODULE_OK;
  }
  return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(sctx.resp.int_reply));
}

// ── Phase 69: FT.PROFILE <idx> SEARCH Coordinator State Machine ──

struct ClusterProfileSearchContext {
  RedisModuleBlockedClient* bc = nullptr;
  ClusterProfileSearchPlan plan;
  std::vector<SearchShardEndpoint> shards;
  int64_t scatter_start_us = 0;
  bool on_timeout_return = false;

  std::mutex mu;
  size_t pending = 0;
  bool cancelled = false;
  bool finished = false;
  std::string error_msg;

  MergeSortMode sort_mode = MergeSortMode::kDocIdAsc;
  bool sort_mode_set = false;
  std::vector<ShardPhase1Slice> slices;

  struct ShardTiming {
    std::string node_id;
    uint64_t hits = 0;
    double elapsed_us = 0.0;
  };
  std::vector<ShardTiming> shard_timings;
  double scatter_time_us = 0.0;
  double merge_time_us = 0.0;
  uint64_t total_matched = 0;
  std::vector<std::string> winner_ids;
};

BlockedRegistry<ClusterProfileSearchContext> g_active_profiles;

void ClusterProfileDisconnectCallback(RedisModuleCtx* /*ctx*/, RedisModuleBlockedClient* bc) {
  g_active_profiles.OnDisconnect(bc);
}

int ClusterProfileReplyCallback(RedisModuleCtx* ctx, RedisModuleString** /*argv*/, int /*argc*/) {
  auto* holder = static_cast<std::shared_ptr<ClusterProfileSearchContext>*>(
      RedisModule_GetBlockedClientPrivateData(ctx));
  if (!holder || !(*holder)) {
    return RedisModule_ReplyWithError(ctx, "ERR internal cluster profile state missing");
  }
  auto& pctx = **holder;
  std::lock_guard<std::mutex> lk(pctx.mu);
  if (!pctx.error_msg.empty()) {
    return RedisModule_ReplyWithError(ctx, pctx.error_msg.c_str());
  }

  double total_us = pctx.plan.parse_time_us + pctx.scatter_time_us + pctx.merge_time_us;

  RedisModule_ReplyWithArray(ctx, 2);

  // Results (matching doc_ids)
  RedisModule_ReplyWithArray(ctx, static_cast<long>(pctx.winner_ids.size()));
  for (const auto& id : pctx.winner_ids) {
    RedisModule_ReplyWithStringBuffer(ctx, id.data(), id.size());
  }

  // Profile info (includes all standalone keys + cluster scatter/merge/per-shard breakdown)
  RedisModule_ReplyWithArray(ctx, 14);
  RedisModule_ReplyWithSimpleString(ctx, "parse_time_us");
  RedisModule_ReplyWithCString(ctx, std::to_string(pctx.plan.parse_time_us).c_str());
  RedisModule_ReplyWithSimpleString(ctx, "scatter_time_us");
  RedisModule_ReplyWithCString(ctx, std::to_string(pctx.scatter_time_us).c_str());
  RedisModule_ReplyWithSimpleString(ctx, "merge_time_us");
  RedisModule_ReplyWithCString(ctx, std::to_string(pctx.merge_time_us).c_str());
  RedisModule_ReplyWithSimpleString(ctx, "total_time_us");
  RedisModule_ReplyWithCString(ctx, std::to_string(total_us).c_str());
  RedisModule_ReplyWithSimpleString(ctx, "total_docs");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(pctx.total_matched));
  RedisModule_ReplyWithSimpleString(ctx, "result_count");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(pctx.winner_ids.size()));
  RedisModule_ReplyWithSimpleString(ctx, "shards");
  std::sort(pctx.shard_timings.begin(), pctx.shard_timings.end(),
            [](const auto& a, const auto& b) { return a.node_id < b.node_id; });
  RedisModule_ReplyWithArray(ctx, static_cast<long>(pctx.shard_timings.size()));
  for (const auto& st : pctx.shard_timings) {
    RedisModule_ReplyWithArray(ctx, 6);
    RedisModule_ReplyWithSimpleString(ctx, "node_id");
    RedisModule_ReplyWithCString(ctx, st.node_id.c_str());
    RedisModule_ReplyWithSimpleString(ctx, "hits");
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(st.hits));
    RedisModule_ReplyWithSimpleString(ctx, "time_us");
    RedisModule_ReplyWithCString(ctx, std::to_string(st.elapsed_us).c_str());
  }
  return REDISMODULE_OK;
}

// ============================================================================
// Phase 70: FT.AGGREGATE Two-Phase Map-Reduce & FT.CURSOR Forwarding Handlers
// ============================================================================

void HandleAggMapRequest(const RpcFrame& req_frame, const std::string& /*peer_ip*/,
                         ClusterRpcServer::ResponseSender send_resp) {
  RpcAggMapRequest req;
  if (!DecodeAggMapRequest(req_frame.payload, req)) {
    RpcAggMapResponse resp;
    resp.ok = false;
    resp.error_msg = "malformed AggMap request";
    std::string out;
    EncodeAggMapResponse(resp, out);
    send_resp(RpcOpCode::kRpcAggMapResp, kRpcFlagNone, std::move(out));
    return;
  }
  GetSearchExecutor().Submit(
      [req = std::move(req), send_resp = std::move(send_resp)]() mutable {
        RpcAggMapResponse resp = ExecuteLocalAggMap(req);
        std::string out;
        EncodeAggMapResponse(resp, out);
        send_resp(RpcOpCode::kRpcAggMapResp, kRpcFlagNone, std::move(out));
      });
}

void HandleCursorRequest(const RpcFrame& req_frame, const std::string& /*peer_ip*/,
                         ClusterRpcServer::ResponseSender send_resp) {
  RpcCursorRequest req;
  if (!DecodeCursorRequest(req_frame.payload, req)) {
    RpcCursorResponse resp;
    resp.ok = false;
    resp.error_msg = "malformed Cursor request";
    std::string out;
    EncodeCursorResponse(resp, out);
    send_resp(RpcOpCode::kRpcCursorResp, kRpcFlagNone, std::move(out));
    return;
  }
  GetSearchExecutor().Submit(
      [req = std::move(req), send_resp = std::move(send_resp)]() mutable {
        RpcCursorResponse resp = ExecuteLocalCursorOp(req);
        std::string out;
        EncodeCursorResponse(resp, out);
        send_resp(RpcOpCode::kRpcCursorResp, kRpcFlagNone, std::move(out));
      });
}

struct ClusterAggContext {
  RedisModuleBlockedClient* bc = nullptr;
  ClusterAggPlan plan;
  std::vector<SearchShardEndpoint> shards;
  int64_t start_ms = 0;
  bool on_timeout_return = false;

  std::mutex mu;
  size_t pending = 0;
  bool cancelled = false;
  bool finished = false;
  std::string error_msg;

  struct ShardAggResult {
    uint32_t shard_idx = 0;
    RpcAggMapResponse resp;
  };
  std::vector<ShardAggResult> shard_results;

  // Final post-Reduce rows
  std::vector<AggRow> final_rows;
};

BlockedRegistry<ClusterAggContext> g_active_aggs;

void ClusterAggDisconnectCallback(RedisModuleCtx* /*ctx*/, RedisModuleBlockedClient* bc) {
  g_active_aggs.OnDisconnect(bc);
}

int ClusterAggReplyCallback(RedisModuleCtx* ctx, RedisModuleString** /*argv*/, int /*argc*/) {
  auto* holder = static_cast<std::shared_ptr<ClusterAggContext>*>(
      RedisModule_GetBlockedClientPrivateData(ctx));
  if (!holder || !(*holder)) {
    return RedisModule_ReplyWithError(ctx, "ERR internal cluster aggregate state missing");
  }
  auto& actx = **holder;
  std::lock_guard<std::mutex> lk(actx.mu);
  if (!actx.error_msg.empty()) {
    return RedisModule_ReplyWithError(ctx, actx.error_msg.c_str());
  }

  if (actx.plan.with_cursor) {
    int64_t now_ms = NowMs();
    uint64_t cid = 0;
    {
      std::lock_guard<std::mutex> clk(GetCursorsMutex());
      PurgeExpiredCursorsLocked(now_ms);
      size_t idx_cursors = 0;
      for (const auto& [id, c] : GetCursors()) {
        if (c.index_name == actx.plan.map_req.index_name) idx_cursors++;
      }
      if (idx_cursors >= kMaxCursorsPerIndex) {
        return RedisModule_ReplyWithError(ctx, "ERR too many active cursors");
      }
      cid = MakeClusterCursorId(ClusterManager::Instance().MyNodeId(), NextCursorId());
      AggCursor cursor;
      cursor.index_name = actx.plan.map_req.index_name;
      cursor.rows = std::move(actx.final_rows);
      cursor.pos = 0;
      cursor.batch_size = actx.plan.cursor_count;
      cursor.last_access_ms = now_ms;
      cursor.max_idle_ms = actx.plan.cursor_max_idle_ms;
      GetCursors()[cid] = std::move(cursor);
    }

    RpcCursorRequest rreq;
    rreq.op = kRpcCursorOpRead;
    rreq.index_name = actx.plan.map_req.index_name;
    rreq.cursor_id = cid;
    auto bresp = ExecuteLocalCursorOp(rreq);
    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithArray(ctx, static_cast<long>(bresp.rows.size()));
    for (const auto& row : bresp.rows) {
      RedisModule_ReplyWithArray(ctx, static_cast<long>(row.size()) * 2);
      for (const auto& [k, v] : row) {
        RedisModule_ReplyWithStringBuffer(ctx, k.data(), k.size());
        RedisModule_ReplyWithStringBuffer(ctx, v.data(), v.size());
      }
    }
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(bresp.next_cursor_id));
    return REDISMODULE_OK;
  }

  RedisModule_ReplyWithArray(ctx, static_cast<long>(actx.final_rows.size()));
  for (const auto& row : actx.final_rows) {
    std::vector<std::string> keys;
    keys.reserve(row.size());
    for (const auto& [k, v] : row) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    RedisModule_ReplyWithArray(ctx, static_cast<long>(keys.size()) * 2);
    for (const auto& k : keys) {
      RedisModule_ReplyWithStringBuffer(ctx, k.data(), k.size());
      const std::string& val = row.at(k);
      RedisModule_ReplyWithStringBuffer(ctx, val.data(), val.size());
    }
  }
  return REDISMODULE_OK;
}

struct ClusterCursorContext {
  RedisModuleBlockedClient* bc = nullptr;
  uint8_t op = kRpcCursorOpRead;

  std::mutex mu;
  bool cancelled = false;
  bool finished = false;
  std::string error_msg;
  RpcCursorResponse resp;
};

BlockedRegistry<ClusterCursorContext> g_active_cursors;

void ClusterCursorDisconnectCallback(RedisModuleCtx* /*ctx*/, RedisModuleBlockedClient* bc) {
  g_active_cursors.OnDisconnect(bc);
}

int ClusterCursorReplyCallback(RedisModuleCtx* ctx, RedisModuleString** /*argv*/, int /*argc*/) {
  auto* holder = static_cast<std::shared_ptr<ClusterCursorContext>*>(
      RedisModule_GetBlockedClientPrivateData(ctx));
  if (!holder || !(*holder)) {
    return RedisModule_ReplyWithError(ctx, "ERR internal cluster cursor state missing");
  }
  auto& cctx = **holder;
  std::lock_guard<std::mutex> lk(cctx.mu);
  if (!cctx.error_msg.empty()) {
    return RedisModule_ReplyWithError(ctx, cctx.error_msg.c_str());
  }
  if (cctx.op == kRpcCursorOpDel) {
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
  }
  RedisModule_ReplyWithArray(ctx, 2);
  RedisModule_ReplyWithArray(ctx, static_cast<long>(cctx.resp.rows.size()));
  for (const auto& row : cctx.resp.rows) {
    RedisModule_ReplyWithArray(ctx, static_cast<long>(row.size()) * 2);
    for (const auto& [k, v] : row) {
      RedisModule_ReplyWithStringBuffer(ctx, k.data(), k.size());
      RedisModule_ReplyWithStringBuffer(ctx, v.data(), v.size());
    }
  }
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(cctx.resp.next_cursor_id));
  return REDISMODULE_OK;
}

}  // namespace

void RegisterClusterCoordinatorHandlers() {
  auto& mgr = ClusterManager::Instance();
  mgr.RegisterRpcHandler(RpcOpCode::kRpcDdlCreate, HandleRemoteDdlRequest);
  mgr.RegisterRpcHandler(RpcOpCode::kRpcDdlDrop, HandleRemoteDdlRequest);
  mgr.RegisterRpcHandler(RpcOpCode::kRpcDdlAlter, HandleRemoteDdlRequest);
  mgr.RegisterRpcHandler(RpcOpCode::kRpcDdlAlias, HandleRemoteDdlRequest);
  mgr.RegisterRpcHandler(RpcOpCode::kRpcDdlSyn, HandleRemoteDdlRequest);
  mgr.RegisterRpcHandler(RpcOpCode::kRpcDdlDict, HandleRemoteDdlRequest);
  mgr.RegisterRpcHandler(RpcOpCode::kRpcSchemaSyncReq, HandleSchemaSyncRequest);
  mgr.RegisterRpcHandler(RpcOpCode::kRpcSearchPhase1, HandleSearchPhase1Request);
  mgr.RegisterRpcHandler(RpcOpCode::kRpcFetchDocs, HandleFetchDocsRequest);
  mgr.RegisterRpcHandler(RpcOpCode::kRpcDfsTermStats, HandleDfsTermStatsRequest);
  mgr.RegisterRpcHandler(RpcOpCode::kRpcHybridPhase1, HandleHybridPhase1Request);
  mgr.RegisterRpcHandler(RpcOpCode::kRpcInfoStats, HandleInfoStatsRequest);
  mgr.RegisterRpcHandler(RpcOpCode::kRpcTagVals, HandleTagValsRequest);
  mgr.RegisterRpcHandler(RpcOpCode::kRpcSpellCheck, HandleSpellCheckRequest);
  mgr.RegisterRpcHandler(RpcOpCode::kRpcSuggest, HandleSuggestRequest);
  mgr.RegisterRpcHandler(RpcOpCode::kRpcAggMap, HandleAggMapRequest);
  mgr.RegisterRpcHandler(RpcOpCode::kRpcCursor, HandleCursorRequest);
}

int ExecuteClusterDdl(RedisModuleCtx* ctx, RpcOpCode opcode, const std::string& cmd_name,
                      const std::vector<std::string>& args, bool reply_integer) {
  auto& mgr = ClusterManager::Instance();
  mgr.RefreshTopology(ctx);
  auto topo = mgr.GetTopology();

  std::vector<DdlPeerEndpoint> remote_masters;
  if (topo) {
    for (const auto& m : topo->masters) {
      if (m.is_myself || !m.is_healthy) continue;
      if (!m.rpc_port_discovered || m.rpc_port <= 0 || m.ip.empty()) {
        std::string msg = "ERR cluster shard RPC endpoint not ready (" +
                          (m.node_id.size() > 8 ? m.node_id.substr(0, 8) : m.node_id) + ")";
        return RedisModule_ReplyWithError(ctx, msg.c_str());
      }
      remote_masters.push_back({m.node_id, m.ip, m.rpc_port});
    }
  }

  // Fallback to local execution if no remote healthy masters exist
  if (remote_masters.empty()) {
    int64_t int_reply = 0;
    std::string err;
    if (!ApplyLocalDdlCommand(ctx, cmd_name, args, /*is_cluster_mode=*/true,
                              /*is_rollback=*/false, int_reply, err)) {
      return RedisModule_ReplyWithError(ctx, err.c_str());
    }
    RedisModule_ReplicateVerbatim(ctx);
    SetPersistedSchemaEpoch(mgr.AdvanceSchemaEpoch());
    mgr.BroadcastHello(ctx, /*request_reply=*/false);
    if (reply_integer) {
      return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(int_reply));
    }
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
  }

  uint64_t epoch = mgr.AdvanceSchemaEpoch();
  SetPersistedSchemaEpoch(epoch);
  int64_t timeout_ms = static_cast<int64_t>(GetClusterQueryTimeoutMs());

  auto ddl_ctx = std::make_shared<ClusterDdlContext>();
  ddl_ctx->opcode = opcode;
  ddl_ctx->cmd_name = cmd_name;
  ddl_ctx->args = args;
  ddl_ctx->schema_epoch = epoch;
  ddl_ctx->timeout_ms = timeout_ms;
  ddl_ctx->reply_integer = reply_integer;
  ddl_ctx->pending = remote_masters.size();

  // Pass timeout=0 because ClusterRpcClient::SendAsync enforces timeout_ms on every peer
  RedisModuleBlockedClient* bc = RedisModule_BlockClient(
      ctx, ClusterDdlReplyCallback, ClusterDdlTimeoutCallback, ClusterDdlFreePrivData, 0);
  if (!bc) {
    return RedisModule_ReplyWithError(ctx, "ERR failed to block client for cluster DDL");
  }
  ddl_ctx->bc = bc;
  {
    std::lock_guard<std::mutex> lk(g_active_ddls_mu);
    g_active_ddls[bc] = ddl_ctx;
  }
  if (RedisModule_SetDisconnectCallback) {
    RedisModule_SetDisconnectCallback(bc, ClusterDdlDisconnectCallback);
  }

  RpcDdlRequest req;
  req.schema_epoch = epoch;
  req.coordinator_node_id = mgr.MyNodeId();
  req.cmd_name = cmd_name;
  req.args = args;
  req.flags = kDdlFlagNone;

  std::string encoded_req;
  EncodeDdlRequest(req, encoded_req);

  for (const auto& peer : remote_masters) {
    mgr.RpcClient().SendAsync(
        peer.node_id, peer.ip, peer.rpc_port, opcode, encoded_req, timeout_ms,
        [ddl_ctx, peer](RpcResponse resp) {
          bool peer_ok = false;
          std::string peer_err;
          if (resp.status != RpcStatus::kOk) {
            peer_err = std::string(RpcStatusToString(resp.status)) +
                       (resp.error_msg.empty() ? "" : (": " + resp.error_msg));
          } else if (resp.flags & kRpcFlagError) {
            peer_err = resp.error_msg.empty() ? "remote RPC error" : resp.error_msg;
          } else {
            RpcDdlResponse ddl_resp;
            if (!DecodeDdlResponse(resp.payload, ddl_resp)) {
              peer_err = "malformed DDL response";
            } else if (!ddl_resp.ok) {
              peer_err = ddl_resp.error_msg.empty() ? "remote DDL rejected" : ddl_resp.error_msg;
            } else {
              peer_ok = true;
            }
          }

          bool start_rollback = false;
          std::vector<DdlPeerEndpoint> rollback_targets;
          RedisModuleBlockedClient* to_unblock = nullptr;
          RpcOpCode rb_op = RpcOpCode::kRpcDdlDrop;
          std::string rb_cmd;
          std::vector<std::string> rb_args;

          {
            std::lock_guard<std::mutex> lk(ddl_ctx->mu);
            if (peer_ok) {
              ddl_ctx->succeeded_peers.push_back(peer);
            } else if (ddl_ctx->failed_reason.empty()) {
              ddl_ctx->failed_node_id = peer.node_id;
              ddl_ctx->failed_reason = peer_err;
            }
            if (ddl_ctx->pending > 0) ddl_ctx->pending--;
            if (ddl_ctx->pending == 0 && !ddl_ctx->finished) {
              if (!ddl_ctx->failed_reason.empty() && !ddl_ctx->succeeded_peers.empty() &&
                  BuildRollbackSpec(ddl_ctx->cmd_name, ddl_ctx->args, rb_op, rb_cmd, rb_args)) {
                ddl_ctx->in_rollback = true;
                ddl_ctx->pending = ddl_ctx->succeeded_peers.size();
                rollback_targets = ddl_ctx->succeeded_peers;
                start_rollback = true;
              } else {
                ddl_ctx->finished = true;
                if (!ddl_ctx->cancelled) {
                  to_unblock = ddl_ctx->bc;
                  ddl_ctx->bc = nullptr;
                }
              }
            }
          }

          if (start_rollback) {
            auto& cmgr = ClusterManager::Instance();
            RpcDdlRequest rb_req;
            rb_req.schema_epoch = ddl_ctx->schema_epoch;
            rb_req.coordinator_node_id = cmgr.MyNodeId();
            rb_req.cmd_name = rb_cmd;
            rb_req.args = rb_args;
            rb_req.flags = kDdlFlagRollback;
            std::string rb_encoded;
            EncodeDdlRequest(rb_req, rb_encoded);

            for (const auto& rb_peer : rollback_targets) {
              cmgr.RpcClient().SendAsync(
                  rb_peer.node_id, rb_peer.ip, rb_peer.rpc_port, rb_op, rb_encoded,
                  ddl_ctx->timeout_ms, [ddl_ctx](RpcResponse) {
                    RedisModuleBlockedClient* rb_unblock = nullptr;
                    {
                      std::lock_guard<std::mutex> lk(ddl_ctx->mu);
                      if (ddl_ctx->pending > 0) ddl_ctx->pending--;
                      if (ddl_ctx->pending == 0 && !ddl_ctx->finished) {
                        ddl_ctx->finished = true;
                        if (!ddl_ctx->cancelled) {
                          rb_unblock = ddl_ctx->bc;
                          ddl_ctx->bc = nullptr;
                        }
                      }
                    }
                    UnblockDdlClient(ddl_ctx, rb_unblock);
                  });
            }
            return;
          }

          UnblockDdlClient(ddl_ctx, to_unblock);
        });
  }

  return REDISMODULE_OK;
}

void TriggerSchemaSyncIfNeeded(RedisModuleCtx* /*ctx*/) {
  auto& mgr = ClusterManager::Instance();
  if (!mgr.IsClusterMode() || mgr.IsShuttingDown()) return;

  auto topo = mgr.GetTopology();
  if (!topo) return;

  uint64_t local_epoch = mgr.GetSchemaEpoch();
  const ShardNodeInfo* best_peer = nullptr;
  for (const auto& n : topo->nodes) {
    if (n.is_myself || !n.is_healthy || !n.rpc_port_discovered || n.rpc_port <= 0 || n.ip.empty()) {
      continue;
    }
    if (n.schema_epoch > local_epoch) {
      if (!best_peer || n.schema_epoch > best_peer->schema_epoch ||
          (n.schema_epoch == best_peer->schema_epoch && n.node_id < best_peer->node_id)) {
        best_peer = &n;
      }
    }
  }
  if (!best_peer) return;

  int64_t now = ClusterNowMs();
  int64_t last = g_last_schema_sync_ms.load(std::memory_order_acquire);
  if (now - last < kSchemaSyncRetryCooldownMs) return;

  bool expected = false;
  if (!g_schema_sync_in_flight.compare_exchange_strong(expected, true,
                                                       std::memory_order_acq_rel)) {
    return;
  }
  g_last_schema_sync_ms.store(now, std::memory_order_release);

  RpcSchemaSyncRequest req;
  req.requester_node_id = mgr.MyNodeId();
  req.local_schema_epoch = local_epoch;
  std::string encoded;
  EncodeSchemaSyncRequest(req, encoded);

  int64_t timeout_ms = static_cast<int64_t>(GetClusterQueryTimeoutMs());
  mgr.RpcClient().SendAsync(
      best_peer->node_id, best_peer->ip, best_peer->rpc_port, RpcOpCode::kRpcSchemaSyncReq,
      encoded, timeout_ms, [](RpcResponse resp) {
        if (resp.status != RpcStatus::kOk || (resp.flags & kRpcFlagError) != 0) {
          g_schema_sync_in_flight.store(false, std::memory_order_release);
          return;
        }
        RpcSchemaSyncResponse sync_resp;
        if (!DecodeSchemaSyncResponse(resp.payload, sync_resp)) {
          g_schema_sync_in_flight.store(false, std::memory_order_release);
          return;
        }
        GetSearchExecutor().Submit([sync_resp = std::move(sync_resp)]() mutable {
          ApplySchemaSyncSnapshot(std::move(sync_resp));
        });
      });
}

int ExecuteClusterSearch(RedisModuleCtx* ctx, ClusterSearchPlan plan) {
  auto& mgr = ClusterManager::Instance();
  mgr.RefreshTopology(ctx);
  auto topo = mgr.GetTopology();

  bool on_timeout_return = IsOnTimeoutReturn();
  std::vector<SearchShardEndpoint> shards;
  if (topo) {
    uint32_t sidx = 0;
    for (const auto& m : topo->masters) {
      if (!m.is_healthy) continue;
      if (!m.is_myself && (!m.rpc_port_discovered || m.rpc_port <= 0 || m.ip.empty())) {
        if (!on_timeout_return) {
          std::string msg = "ERR cluster shard RPC endpoint not ready (" +
                            (m.node_id.size() > 8 ? m.node_id.substr(0, 8) : m.node_id) + ")";
          return RedisModule_ReplyWithError(ctx, msg.c_str());
        }
        continue;
      }
      shards.push_back({sidx++, m.node_id, m.ip, m.rpc_port, m.is_myself});
      if (shards.size() >= kMaxClusterFanout) break;
    }
  }

  if (shards.empty()) {
    RedisModule_ReplyWithArray(ctx, 1);
    RedisModule_ReplyWithLongLong(ctx, 0);
    return REDISMODULE_OK;
  }

  auto sctx = std::make_shared<ClusterSearchContext>();
  sctx->plan = std::move(plan);
  sctx->shards = std::move(shards);
  sctx->start_ms = ClusterNowMs();
  sctx->on_timeout_return = on_timeout_return;
  sctx->phase1_pending = sctx->shards.size();
  sctx->slices.reserve(sctx->shards.size());

  // Pass timeout=0 because ClusterRpcClient::SendAsync enforces timeout_ms on every peer
  RedisModuleBlockedClient* bc = RedisModule_BlockClient(
      ctx, ClusterSearchReplyCallback, ClusterSearchTimeoutCallback, ClusterSearchFreePrivData, 0);
  if (!bc) {
    return RedisModule_ReplyWithError(ctx, "ERR failed to block client for cluster search");
  }
  sctx->bc = bc;
  {
    std::lock_guard<std::mutex> lk(g_active_searches_mu);
    g_active_searches[bc] = sctx;
  }
  if (RedisModule_SetDisconnectCallback) {
    RedisModule_SetDisconnectCallback(bc, ClusterSearchDisconnectCallback);
  }

  // Phase 69: Optional 2-Phase BM25 DFS exact global IDF stats collection
  bool use_dfs = !sctx->plan.is_knn && !sctx->plan.phase1_req.count_only &&
                 sctx->plan.withscores && GetClusterBm25Dfs() &&
                 (sctx->plan.phase1_req.scorer == static_cast<uint8_t>(ScorerType::kBm25));

  if (use_dfs) {
    RpcDfsTermStatsRequest dfs_req;
    dfs_req.index_name = sctx->plan.phase1_req.index_name;
    dfs_req.query_str = sctx->plan.phase1_req.query_str;
    dfs_req.verbatim = sctx->plan.phase1_req.verbatim;
    dfs_req.nostopwords = sctx->plan.phase1_req.nostopwords;
    dfs_req.dialect = sctx->plan.phase1_req.dialect;
    dfs_req.has_language = sctx->plan.phase1_req.has_language;
    dfs_req.language = sctx->plan.phase1_req.language;
    dfs_req.infields = sctx->plan.phase1_req.infields;

    sctx->phase0_pending = sctx->shards.size();
    sctx->dfs_shard_stats.reserve(sctx->shards.size());
    std::string encoded_dfs;
    EncodeDfsTermStatsRequest(dfs_req, encoded_dfs);
    int64_t timeout_ms = sctx->plan.timeout_ms > 0 ? sctx->plan.timeout_ms : 5000;

    for (const auto& ep : sctx->shards) {
      if (ep.is_myself) {
        GetSearchExecutor().Submit([sctx, nid = ep.node_id, dfs_req]() {
          RpcDfsTermStatsResponse dresp = ExecuteLocalDfsTermStats(dfs_req);
          OnSearchPhase0DfsDone(sctx, nid, true, "", std::move(dresp));
        });
      } else {
        mgr.RpcClient().SendAsync(
            ep.node_id, ep.ip, ep.rpc_port, RpcOpCode::kRpcDfsTermStats, encoded_dfs, timeout_ms,
            [sctx, nid = ep.node_id](RpcResponse r) {
              if (r.status != RpcStatus::kOk) {
                OnSearchPhase0DfsDone(sctx, nid, false,
                                      std::string(RpcStatusToString(r.status)) +
                                          (r.error_msg.empty() ? "" : (": " + r.error_msg)),
                                      {});
                return;
              }
              if (r.flags & kRpcFlagError) {
                OnSearchPhase0DfsDone(sctx, nid, false,
                                      r.error_msg.empty() ? "remote DFS error" : r.error_msg, {});
                return;
              }
              RpcDfsTermStatsResponse dresp;
              if (!DecodeDfsTermStatsResponse(r.payload, dresp)) {
                OnSearchPhase0DfsDone(sctx, nid, false, "malformed DFS response", {});
                return;
              }
              OnSearchPhase0DfsDone(sctx, nid, true, "", std::move(dresp));
            });
      }
    }
    return REDISMODULE_OK;
  }

  DispatchSearchPhase1(sctx);
  return REDISMODULE_OK;
}

int ExecuteClusterHybrid(RedisModuleCtx* ctx, ClusterHybridPlan plan) {
  auto& mgr = ClusterManager::Instance();
  bool on_timeout_return = IsOnTimeoutReturn();
  std::vector<SearchShardEndpoint> shards;
  std::string err;
  if (!CollectHealthyMasterShards(ctx, on_timeout_return, shards, err)) {
    return RedisModule_ReplyWithError(ctx, err.c_str());
  }
  if (shards.empty()) {
    RedisModule_ReplyWithArray(ctx, 1);
    RedisModule_ReplyWithLongLong(ctx, 0);
    return REDISMODULE_OK;
  }

  auto hctx = std::make_shared<ClusterHybridContext>();
  hctx->plan = std::move(plan);
  hctx->shards = std::move(shards);
  hctx->start_ms = ClusterNowMs();
  hctx->on_timeout_return = on_timeout_return;
  hctx->slices.reserve(hctx->shards.size());

  RedisModuleBlockedClient* bc = RedisModule_BlockClient(
      ctx, ClusterHybridReplyCallback, ClusterSearchTimeoutCallback,
      FreeSharedHolder<ClusterHybridContext>, 0);
  if (!bc) {
    return RedisModule_ReplyWithError(ctx, "ERR failed to block client for cluster hybrid search");
  }
  hctx->bc = bc;
  g_active_hybrids.Register(bc, hctx);
  if (RedisModule_SetDisconnectCallback) {
    RedisModule_SetDisconnectCallback(bc, ClusterHybridDisconnectCallback);
  }

  bool use_dfs = GetClusterBm25Dfs() &&
                 (hctx->plan.phase1_req.scorer == static_cast<uint8_t>(ScorerType::kBm25));
  if (use_dfs) {
    RpcDfsTermStatsRequest dfs_req;
    dfs_req.index_name = hctx->plan.phase1_req.index_name;
    dfs_req.query_str = hctx->plan.phase1_req.text_query;
    dfs_req.dialect = hctx->plan.phase1_req.dialect;

    hctx->phase0_pending = hctx->shards.size();
    hctx->dfs_shard_stats.reserve(hctx->shards.size());
    std::string encoded_dfs;
    EncodeDfsTermStatsRequest(dfs_req, encoded_dfs);
    int64_t timeout_ms = hctx->plan.timeout_ms > 0 ? hctx->plan.timeout_ms : 5000;

    for (const auto& ep : hctx->shards) {
      if (ep.is_myself) {
        GetSearchExecutor().Submit([hctx, nid = ep.node_id, dfs_req]() {
          RpcDfsTermStatsResponse dresp = ExecuteLocalDfsTermStats(dfs_req);
          OnHybridPhase0DfsDone(hctx, nid, true, "", std::move(dresp));
        });
      } else {
        mgr.RpcClient().SendAsync(
            ep.node_id, ep.ip, ep.rpc_port, RpcOpCode::kRpcDfsTermStats, encoded_dfs, timeout_ms,
            [hctx, nid = ep.node_id](RpcResponse r) {
              if (r.status != RpcStatus::kOk) {
                OnHybridPhase0DfsDone(hctx, nid, false,
                                      std::string(RpcStatusToString(r.status)) +
                                          (r.error_msg.empty() ? "" : (": " + r.error_msg)),
                                      {});
                return;
              }
              if (r.flags & kRpcFlagError) {
                OnHybridPhase0DfsDone(hctx, nid, false,
                                      r.error_msg.empty() ? "remote DFS error" : r.error_msg, {});
                return;
              }
              RpcDfsTermStatsResponse dresp;
              if (!DecodeDfsTermStatsResponse(r.payload, dresp)) {
                OnHybridPhase0DfsDone(hctx, nid, false, "malformed DFS response", {});
                return;
              }
              OnHybridPhase0DfsDone(hctx, nid, true, "", std::move(dresp));
            });
      }
    }
    return REDISMODULE_OK;
  }

  DispatchHybridPhase1(hctx);
  return REDISMODULE_OK;
}

int ExecuteClusterInfo(RedisModuleCtx* ctx, const std::string& index_name) {
  auto& mgr = ClusterManager::Instance();
  bool on_timeout_return = IsOnTimeoutReturn();
  std::vector<SearchShardEndpoint> shards;
  std::string err;
  if (!CollectHealthyMasterShards(ctx, on_timeout_return, shards, err)) {
    return RedisModule_ReplyWithError(ctx, err.c_str());
  }
  if (shards.empty()) {
    return RedisModule_ReplyWithError(ctx, "ERR no healthy cluster shards available");
  }

  auto ictx = std::make_shared<ClusterInfoContext>();
  ictx->index_name = index_name;
  ictx->on_timeout_return = on_timeout_return;
  ictx->pending = shards.size();
  ictx->shard_rows.reserve(shards.size());

  RedisModuleBlockedClient* bc = RedisModule_BlockClient(
      ctx, ClusterInfoReplyCallback, GenericTimeoutCallback, FreeSharedHolder<ClusterInfoContext>,
      0);
  if (!bc) {
    return RedisModule_ReplyWithError(ctx, "ERR failed to block client for cluster info");
  }
  ictx->bc = bc;
  g_active_infos.Register(bc, ictx);
  if (RedisModule_SetDisconnectCallback) {
    RedisModule_SetDisconnectCallback(bc, ClusterInfoDisconnectCallback);
  }

  RpcInfoStatsRequest req{index_name};
  std::string encoded;
  EncodeInfoStatsRequest(req, encoded);
  int64_t timeout_ms = static_cast<int64_t>(GetClusterQueryTimeoutMs());

  auto OnDone = [](const std::shared_ptr<ClusterInfoContext>& c, const std::string& nid, bool ok,
                   const std::string& e, RpcInfoStatsResponse&& resp) {
    RedisModuleBlockedClient* to_unblock = nullptr;
    {
      std::lock_guard<std::mutex> lk(c->mu);
      if (c->finished || c->cancelled) return;
      if (!ok || !resp.ok) {
        if (!c->on_timeout_return && c->error_msg.empty()) {
          std::string reason = !ok ? e : resp.error_msg;
          c->error_msg = "ERR cluster shard info failed (" +
                         (nid.size() > 8 ? nid.substr(0, 8) : nid) + "): " + reason;
        }
      } else {
        c->shard_rows.push_back({nid, std::move(resp)});
      }
      if (c->pending > 0) c->pending--;
      if (c->pending == 0 && !c->finished) {
        c->finished = true;
        to_unblock = c->bc;
        c->bc = nullptr;
      }
    }
    g_active_infos.Unblock(c, to_unblock);
  };

  for (const auto& ep : shards) {
    if (ep.is_myself) {
      GetSearchExecutor().Submit([ictx, nid = ep.node_id, req, OnDone]() {
        RpcInfoStatsResponse resp = ExecuteLocalInfoStats(req);
        OnDone(ictx, nid, true, "", std::move(resp));
      });
    } else {
      mgr.RpcClient().SendAsync(
          ep.node_id, ep.ip, ep.rpc_port, RpcOpCode::kRpcInfoStats, encoded, timeout_ms,
          [ictx, nid = ep.node_id, OnDone](RpcResponse r) {
            if (r.status != RpcStatus::kOk) {
              OnDone(ictx, nid, false,
                     std::string(RpcStatusToString(r.status)) +
                         (r.error_msg.empty() ? "" : (": " + r.error_msg)),
                     {});
              return;
            }
            if (r.flags & kRpcFlagError) {
              OnDone(ictx, nid, false, r.error_msg.empty() ? "remote info error" : r.error_msg, {});
              return;
            }
            RpcInfoStatsResponse iresp;
            if (!DecodeInfoStatsResponse(r.payload, iresp)) {
              OnDone(ictx, nid, false, "malformed info response", {});
              return;
            }
            OnDone(ictx, nid, true, "", std::move(iresp));
          });
    }
  }
  return REDISMODULE_OK;
}

int ExecuteClusterTagVals(RedisModuleCtx* ctx, const std::string& index_name,
                          const std::string& field_name) {
  auto& mgr = ClusterManager::Instance();
  bool on_timeout_return = IsOnTimeoutReturn();
  std::vector<SearchShardEndpoint> shards;
  std::string err;
  if (!CollectHealthyMasterShards(ctx, on_timeout_return, shards, err)) {
    return RedisModule_ReplyWithError(ctx, err.c_str());
  }
  if (shards.empty()) {
    RedisModule_ReplyWithArray(ctx, 0);
    return REDISMODULE_OK;
  }

  auto tctx = std::make_shared<ClusterTagValsContext>();
  tctx->on_timeout_return = on_timeout_return;
  tctx->pending = shards.size();

  RedisModuleBlockedClient* bc = RedisModule_BlockClient(
      ctx, ClusterTagValsReplyCallback, GenericTimeoutCallback,
      FreeSharedHolder<ClusterTagValsContext>, 0);
  if (!bc) {
    return RedisModule_ReplyWithError(ctx, "ERR failed to block client for cluster tagvals");
  }
  tctx->bc = bc;
  g_active_tagvals.Register(bc, tctx);
  if (RedisModule_SetDisconnectCallback) {
    RedisModule_SetDisconnectCallback(bc, ClusterTagValsDisconnectCallback);
  }

  RpcTagValsRequest req{index_name, field_name};
  std::string encoded;
  EncodeTagValsRequest(req, encoded);
  int64_t timeout_ms = static_cast<int64_t>(GetClusterQueryTimeoutMs());

  auto OnDone = [](const std::shared_ptr<ClusterTagValsContext>& c, const std::string& nid,
                   bool ok, const std::string& e, RpcTagValsResponse&& resp) {
    RedisModuleBlockedClient* to_unblock = nullptr;
    {
      std::lock_guard<std::mutex> lk(c->mu);
      if (c->finished || c->cancelled) return;
      if (!ok || !resp.ok) {
        if (!c->on_timeout_return && c->error_msg.empty()) {
          std::string reason = !ok ? e : resp.error_msg;
          c->error_msg = "ERR cluster shard tagvals failed (" +
                         (nid.size() > 8 ? nid.substr(0, 8) : nid) + "): " + reason;
        }
      } else {
        for (auto& t : resp.tags) c->all_tags.insert(std::move(t));
      }
      if (c->pending > 0) c->pending--;
      if (c->pending == 0 && !c->finished) {
        c->finished = true;
        to_unblock = c->bc;
        c->bc = nullptr;
      }
    }
    g_active_tagvals.Unblock(c, to_unblock);
  };

  for (const auto& ep : shards) {
    if (ep.is_myself) {
      GetSearchExecutor().Submit([tctx, nid = ep.node_id, req, OnDone]() {
        RpcTagValsResponse resp = ExecuteLocalTagVals(req);
        OnDone(tctx, nid, true, "", std::move(resp));
      });
    } else {
      mgr.RpcClient().SendAsync(
          ep.node_id, ep.ip, ep.rpc_port, RpcOpCode::kRpcTagVals, encoded, timeout_ms,
          [tctx, nid = ep.node_id, OnDone](RpcResponse r) {
            if (r.status != RpcStatus::kOk) {
              OnDone(tctx, nid, false,
                     std::string(RpcStatusToString(r.status)) +
                         (r.error_msg.empty() ? "" : (": " + r.error_msg)),
                     {});
              return;
            }
            if (r.flags & kRpcFlagError) {
              OnDone(tctx, nid, false,
                     r.error_msg.empty() ? "remote tagvals error" : r.error_msg, {});
              return;
            }
            RpcTagValsResponse tresp;
            if (!DecodeTagValsResponse(r.payload, tresp)) {
              OnDone(tctx, nid, false, "malformed tagvals response", {});
              return;
            }
            OnDone(tctx, nid, true, "", std::move(tresp));
          });
    }
  }
  return REDISMODULE_OK;
}

int ExecuteClusterSpellCheck(RedisModuleCtx* ctx, RpcSpellCheckRequest req) {
  auto& mgr = ClusterManager::Instance();
  bool on_timeout_return = IsOnTimeoutReturn();
  std::vector<SearchShardEndpoint> shards;
  std::string err;
  if (!CollectHealthyMasterShards(ctx, on_timeout_return, shards, err)) {
    return RedisModule_ReplyWithError(ctx, err.c_str());
  }
  if (shards.empty()) {
    RedisModule_ReplyWithArray(ctx, 0);
    return REDISMODULE_OK;
  }

  auto sctx = std::make_shared<ClusterSpellCheckContext>();
  sctx->req = std::move(req);
  sctx->on_timeout_return = on_timeout_return;
  sctx->pending = shards.size();

  RedisModuleBlockedClient* bc = RedisModule_BlockClient(
      ctx, ClusterSpellCheckReplyCallback, GenericTimeoutCallback,
      FreeSharedHolder<ClusterSpellCheckContext>, 0);
  if (!bc) {
    return RedisModule_ReplyWithError(ctx, "ERR failed to block client for cluster spellcheck");
  }
  sctx->bc = bc;
  g_active_spellchecks.Register(bc, sctx);
  if (RedisModule_SetDisconnectCallback) {
    RedisModule_SetDisconnectCallback(bc, ClusterSpellCheckDisconnectCallback);
  }

  std::string encoded;
  EncodeSpellCheckRequest(sctx->req, encoded);
  int64_t timeout_ms = static_cast<int64_t>(GetClusterQueryTimeoutMs());

  auto OnDone = [](const std::shared_ptr<ClusterSpellCheckContext>& c, const std::string& nid,
                   bool ok, const std::string& e, RpcSpellCheckResponse&& resp) {
    RedisModuleBlockedClient* to_unblock = nullptr;
    {
      std::lock_guard<std::mutex> lk(c->mu);
      if (c->finished || c->cancelled) return;
      if (!ok || !resp.ok) {
        if (!c->on_timeout_return && c->error_msg.empty()) {
          std::string reason = !ok ? e : resp.error_msg;
          c->error_msg = "ERR cluster shard spellcheck failed (" +
                         (nid.size() > 8 ? nid.substr(0, 8) : nid) + "): " + reason;
        }
      } else {
        for (auto& kt : resp.known_terms) c->known_terms.insert(std::move(kt));
        for (auto& ts : resp.term_suggestions) {
          auto& dst = c->scores_by_term[ts.term];
          for (auto& [sug, sc] : ts.suggestions) {
            dst[sug] += sc;
          }
        }
      }
      if (c->pending > 0) c->pending--;
      if (c->pending == 0 && !c->finished) {
        c->finished = true;
        to_unblock = c->bc;
        c->bc = nullptr;
      }
    }
    g_active_spellchecks.Unblock(c, to_unblock);
  };

  for (const auto& ep : shards) {
    if (ep.is_myself) {
      GetSearchExecutor().Submit([sctx, nid = ep.node_id, OnDone]() {
        RpcSpellCheckResponse resp = ExecuteLocalSpellCheck(sctx->req);
        OnDone(sctx, nid, true, "", std::move(resp));
      });
    } else {
      mgr.RpcClient().SendAsync(
          ep.node_id, ep.ip, ep.rpc_port, RpcOpCode::kRpcSpellCheck, encoded, timeout_ms,
          [sctx, nid = ep.node_id, OnDone](RpcResponse r) {
            if (r.status != RpcStatus::kOk) {
              OnDone(sctx, nid, false,
                     std::string(RpcStatusToString(r.status)) +
                         (r.error_msg.empty() ? "" : (": " + r.error_msg)),
                     {});
              return;
            }
            if (r.flags & kRpcFlagError) {
              OnDone(sctx, nid, false,
                     r.error_msg.empty() ? "remote spellcheck error" : r.error_msg, {});
              return;
            }
            RpcSpellCheckResponse sresp;
            if (!DecodeSpellCheckResponse(r.payload, sresp)) {
              OnDone(sctx, nid, false, "malformed spellcheck response", {});
              return;
            }
            OnDone(sctx, nid, true, "", std::move(sresp));
          });
    }
  }
  return REDISMODULE_OK;
}

int ExecuteClusterSuggest(RedisModuleCtx* ctx, ClusterSuggestPlan plan) {
  auto& mgr = ClusterManager::Instance();
  mgr.RefreshTopology(ctx);
  auto topo = mgr.GetTopology();

  uint16_t slot = ComputeClusterHashSlot(plan.dict_name);
  const ShardNodeInfo* owner = topo ? topo->FindMasterForSlot(slot) : nullptr;

  // Local owner (or fallback if slots unmapped): execute directly on this node
  if (!owner || owner->is_myself) {
    bool mutated = false;
    RpcSuggestResponse resp;
    if (!ApplyLocalSuggestCommand(plan.cmd_name, plan.args, mutated, resp)) {
      return RedisModule_ReplyWithError(ctx, resp.error_msg.c_str());
    }
    if (plan.is_write && mutated) {
      RedisModule_ReplicateVerbatim(ctx);
    }
    if (MatchArg(plan.cmd_name, "FT.SUGGET")) {
      long mult = 1 + (plan.sugget_withscores ? 1 : 0) + (plan.sugget_withpayloads ? 1 : 0);
      RedisModule_ReplyWithArray(ctx, static_cast<long>(resp.entries.size()) * mult);
      for (const auto& e : resp.entries) {
        RedisModule_ReplyWithStringBuffer(ctx, e.str.data(), e.str.size());
        if (plan.sugget_withscores) {
          std::string sc = std::to_string(e.score);
          RedisModule_ReplyWithStringBuffer(ctx, sc.data(), sc.size());
        }
        if (plan.sugget_withpayloads) {
          RedisModule_ReplyWithStringBuffer(ctx, e.payload.data(), e.payload.size());
        }
      }
      return REDISMODULE_OK;
    }
    return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(resp.int_reply));
  }

  if (!owner->is_healthy || !owner->rpc_port_discovered || owner->rpc_port <= 0 ||
      owner->ip.empty()) {
    std::string msg = "ERR cluster suggest owner shard unavailable (" +
                      (owner->node_id.size() > 8 ? owner->node_id.substr(0, 8) : owner->node_id) +
                      ")";
    return RedisModule_ReplyWithError(ctx, msg.c_str());
  }

  auto sctx = std::make_shared<ClusterSuggestContext>();
  sctx->plan = std::move(plan);

  RedisModuleBlockedClient* bc = RedisModule_BlockClient(
      ctx, ClusterSuggestReplyCallback, GenericTimeoutCallback,
      FreeSharedHolder<ClusterSuggestContext>, 0);
  if (!bc) {
    return RedisModule_ReplyWithError(ctx, "ERR failed to block client for cluster suggest");
  }
  sctx->bc = bc;
  g_active_suggests.Register(bc, sctx);
  if (RedisModule_SetDisconnectCallback) {
    RedisModule_SetDisconnectCallback(bc, ClusterSuggestDisconnectCallback);
  }

  RpcSuggestRequest req;
  req.cmd_name = sctx->plan.cmd_name;
  req.args = sctx->plan.args;
  std::string encoded;
  EncodeSuggestRequest(req, encoded);
  int64_t timeout_ms = static_cast<int64_t>(GetClusterQueryTimeoutMs());

  std::string target_id = owner->node_id;
  mgr.RpcClient().SendAsync(
      owner->node_id, owner->ip, owner->rpc_port, RpcOpCode::kRpcSuggest, encoded, timeout_ms,
      [sctx, target_id](RpcResponse r) {
        RedisModuleBlockedClient* to_unblock = nullptr;
        {
          std::lock_guard<std::mutex> lk(sctx->mu);
          if (sctx->finished || sctx->cancelled) return;
          if (r.status != RpcStatus::kOk) {
            sctx->error_msg = "ERR cluster suggest RPC failed (" +
                              (target_id.size() > 8 ? target_id.substr(0, 8) : target_id) + "): " +
                              std::string(RpcStatusToString(r.status)) +
                              (r.error_msg.empty() ? "" : (": " + r.error_msg));
          } else if (r.flags & kRpcFlagError) {
            sctx->error_msg = r.error_msg.empty() ? "ERR remote suggest error" : r.error_msg;
          } else if (!DecodeSuggestResponse(r.payload, sctx->resp)) {
            sctx->error_msg = "ERR malformed suggest response";
          } else if (!sctx->resp.ok) {
            sctx->error_msg =
                sctx->resp.error_msg.empty() ? "ERR remote suggest failed" : sctx->resp.error_msg;
          }
          sctx->finished = true;
          to_unblock = sctx->bc;
          sctx->bc = nullptr;
        }
        g_active_suggests.Unblock(sctx, to_unblock);
      });
  return REDISMODULE_OK;
}

int ExecuteClusterProfileSearch(RedisModuleCtx* ctx, ClusterProfileSearchPlan plan) {
  auto& mgr = ClusterManager::Instance();
  bool on_timeout_return = IsOnTimeoutReturn();
  std::vector<SearchShardEndpoint> shards;
  std::string err;
  if (!CollectHealthyMasterShards(ctx, on_timeout_return, shards, err)) {
    return RedisModule_ReplyWithError(ctx, err.c_str());
  }
  if (shards.empty()) {
    return RedisModule_ReplyWithError(ctx, "ERR no healthy cluster shards available");
  }

  auto pctx = std::make_shared<ClusterProfileSearchContext>();
  pctx->plan = std::move(plan);
  pctx->shards = std::move(shards);
  pctx->scatter_start_us = MonotonicUs();
  pctx->on_timeout_return = on_timeout_return;
  pctx->pending = pctx->shards.size();
  pctx->slices.reserve(pctx->shards.size());
  pctx->shard_timings.reserve(pctx->shards.size());

  RedisModuleBlockedClient* bc = RedisModule_BlockClient(
      ctx, ClusterProfileReplyCallback, GenericTimeoutCallback,
      FreeSharedHolder<ClusterProfileSearchContext>, 0);
  if (!bc) {
    return RedisModule_ReplyWithError(ctx, "ERR failed to block client for cluster profile");
  }
  pctx->bc = bc;
  g_active_profiles.Register(bc, pctx);
  if (RedisModule_SetDisconnectCallback) {
    RedisModule_SetDisconnectCallback(bc, ClusterProfileDisconnectCallback);
  }

  std::string encoded_p1;
  EncodeSearchPhase1Request(pctx->plan.phase1_req, encoded_p1);
  int64_t timeout_ms = pctx->plan.timeout_ms > 0 ? pctx->plan.timeout_ms : 5000;

  auto OnDone = [](const std::shared_ptr<ClusterProfileSearchContext>& c, uint32_t sidx,
                   const std::string& nid, int64_t shard_start_us, bool ok, const std::string& e,
                   RpcSearchPhase1Response&& resp) {
    int64_t now_us = MonotonicUs();
    double shard_us = static_cast<double>(std::max<int64_t>(0, now_us - shard_start_us));
    RedisModuleBlockedClient* to_unblock = nullptr;
    {
      std::lock_guard<std::mutex> lk(c->mu);
      if (c->finished || c->cancelled) return;
      if (!ok || !resp.ok) {
        if (!c->on_timeout_return && c->error_msg.empty()) {
          std::string reason = !ok ? e : resp.error_msg;
          c->error_msg = "ERR cluster profile shard failed (" +
                         (nid.size() > 8 ? nid.substr(0, 8) : nid) + "): " + reason;
        }
      } else {
        if (!c->sort_mode_set) {
          c->sort_mode = static_cast<MergeSortMode>(resp.sort_mode);
          c->sort_mode_set = true;
        }
        c->shard_timings.push_back({nid, resp.total_hits, shard_us});
        ShardPhase1Slice slice;
        slice.shard_idx = sidx;
        slice.total_hits = resp.total_hits;
        slice.items.reserve(resp.items.size());
        for (auto& it : resp.items) {
          MergeCandidate mc;
          mc.doc_id = std::move(it.doc_id);
          mc.shard_idx = sidx;
          mc.score = it.score;
          mc.sort_num = it.sort_num;
          mc.sort_str = std::move(it.sort_str);
          mc.has_yield_dist = it.has_yield_dist;
          slice.items.push_back(std::move(mc));
        }
        c->slices.push_back(std::move(slice));
      }

      if (c->pending > 0) c->pending--;
      if (c->pending == 0 && !c->finished) {
        c->scatter_time_us =
            static_cast<double>(std::max<int64_t>(0, now_us - c->scatter_start_us));
        if (c->error_msg.empty()) {
          int64_t m_start = MonotonicUs();
          MergedSearchResult merged = MergeSearchShards(
              c->slices, c->sort_mode, c->plan.limit_offset, c->plan.limit_count, c->plan.has_limit,
              GetMaxSearchResults(), 0);
          c->merge_time_us = static_cast<double>(std::max<int64_t>(0, MonotonicUs() - m_start));
          c->total_matched = merged.global_total;
          c->winner_ids.reserve(merged.winners.size());
          for (auto& w : merged.winners) c->winner_ids.push_back(std::move(w.doc_id));
        }
        c->finished = true;
        to_unblock = c->bc;
        c->bc = nullptr;
      }
    }
    g_active_profiles.Unblock(c, to_unblock);
  };

  for (const auto& ep : pctx->shards) {
    int64_t st_us = MonotonicUs();
    if (ep.is_myself) {
      GetSearchExecutor().Submit([pctx, sidx = ep.shard_idx, nid = ep.node_id, st_us, OnDone]() {
        RpcSearchPhase1Response resp = ExecuteLocalSearchPhase1(pctx->plan.phase1_req);
        OnDone(pctx, sidx, nid, st_us, true, "", std::move(resp));
      });
    } else {
      mgr.RpcClient().SendAsync(
          ep.node_id, ep.ip, ep.rpc_port, RpcOpCode::kRpcSearchPhase1, encoded_p1, timeout_ms,
          [pctx, sidx = ep.shard_idx, nid = ep.node_id, st_us, OnDone](RpcResponse r) {
            if (r.status != RpcStatus::kOk) {
              OnDone(pctx, sidx, nid, st_us, false,
                     std::string(RpcStatusToString(r.status)) +
                         (r.error_msg.empty() ? "" : (": " + r.error_msg)),
                     {});
              return;
            }
            if (r.flags & kRpcFlagError) {
              OnDone(pctx, sidx, nid, st_us, false,
                     r.error_msg.empty() ? "remote profile error" : r.error_msg, {});
              return;
            }
            RpcSearchPhase1Response p1resp;
            if (!DecodeSearchPhase1Response(r.payload, p1resp)) {
              OnDone(pctx, sidx, nid, st_us, false, "malformed profile response", {});
              return;
            }
            OnDone(pctx, sidx, nid, st_us, true, "", std::move(p1resp));
          });
    }
  }
  return REDISMODULE_OK;
}

int ExecuteClusterAggregate(RedisModuleCtx* ctx, ClusterAggPlan plan) {
  auto& mgr = ClusterManager::Instance();
  bool on_timeout_return = IsOnTimeoutReturn();
  std::vector<SearchShardEndpoint> shards;
  std::string err;
  if (!CollectHealthyMasterShards(ctx, on_timeout_return, shards, err)) {
    return RedisModule_ReplyWithError(ctx, err.c_str());
  }
  if (shards.empty()) {
    if (plan.with_cursor) {
      RedisModule_ReplyWithArray(ctx, 2);
      RedisModule_ReplyWithArray(ctx, 0);
      RedisModule_ReplyWithLongLong(ctx, 0);
    } else {
      RedisModule_ReplyWithArray(ctx, 0);
    }
    return REDISMODULE_OK;
  }

  auto actx = std::make_shared<ClusterAggContext>();
  actx->plan = std::move(plan);
  actx->shards = std::move(shards);
  actx->start_ms = ClusterNowMs();
  actx->on_timeout_return = on_timeout_return;
  actx->pending = actx->shards.size();
  actx->shard_results.reserve(actx->shards.size());

  RedisModuleBlockedClient* bc = RedisModule_BlockClient(
      ctx, ClusterAggReplyCallback, GenericTimeoutCallback, FreeSharedHolder<ClusterAggContext>, 0);
  if (!bc) {
    return RedisModule_ReplyWithError(ctx, "ERR failed to block client for cluster aggregate");
  }
  actx->bc = bc;
  g_active_aggs.Register(bc, actx);
  if (RedisModule_SetDisconnectCallback) {
    RedisModule_SetDisconnectCallback(bc, ClusterAggDisconnectCallback);
  }

  std::string encoded;
  EncodeAggMapRequest(actx->plan.map_req, encoded);
  int64_t timeout_ms = actx->plan.timeout_ms > 0 ? actx->plan.timeout_ms : 5000;

  auto OnDone = [](const std::shared_ptr<ClusterAggContext>& c, uint32_t sidx,
                   const std::string& nid, bool ok, const std::string& e,
                   RpcAggMapResponse&& resp) {
    RedisModuleBlockedClient* to_unblock = nullptr;
    {
      std::lock_guard<std::mutex> lk(c->mu);
      if (c->finished || c->cancelled) return;
      if (!ok || !resp.ok) {
        if (!c->on_timeout_return && c->error_msg.empty()) {
          std::string reason = !ok ? e : resp.error_msg;
          c->error_msg = "ERR cluster aggregate shard failed (" +
                         (nid.size() > 8 ? nid.substr(0, 8) : nid) + "): " + reason;
        }
      } else {
        c->shard_results.push_back({sidx, std::move(resp)});
      }

      if (c->pending > 0) c->pending--;
      if (c->pending == 0 && !c->finished) {
        if (c->error_msg.empty()) {
          // Sort shard results deterministically by shard_idx
          std::sort(c->shard_results.begin(), c->shard_results.end(),
                    [](const auto& a, const auto& b) { return a.shard_idx < b.shard_idx; });

          std::vector<AggRow> rows;
          std::string reduce_err;
          if (c->plan.map_req.mode == 1) {
            std::vector<std::vector<PartialGroup>> all_groups;
            all_groups.reserve(c->shard_results.size());
            for (auto& sr : c->shard_results) {
              all_groups.push_back(std::move(sr.resp.partial_groups));
            }
            if (!MergeAndFinalizeGroups(all_groups, c->plan.map_req.groupby_stage.reducers,
                                        static_cast<size_t>(c->plan.map_req.max_groups), rows,
                                        reduce_err)) {
              c->error_msg = std::move(reduce_err);
            }
          } else {
            // Raw rows mode: deduplicate by doc_id (for live MIGRATE safety), optional KNN top-K
            struct RawItem {
              std::string doc_id;
              bool has_knn_dist = false;
              double knn_dist = 0.0;
              AggRow row;
            };
            std::unordered_set<std::string> seen_ids;
            std::vector<RawItem> items;
            for (auto& sr : c->shard_results) {
              for (auto& rr : sr.resp.raw_rows) {
                if (!rr.doc_id.empty() && !seen_ids.insert(rr.doc_id).second) continue;
                RawItem item;
                item.doc_id = std::move(rr.doc_id);
                item.has_knn_dist = rr.has_knn_dist;
                item.knn_dist = rr.knn_dist;
                for (auto& [k, v] : rr.fields) item.row[k] = std::move(v);
                items.push_back(std::move(item));
              }
            }
            if (c->plan.is_knn && c->plan.knn_k > 0) {
              std::sort(items.begin(), items.end(), [](const RawItem& a, const RawItem& b) {
                if (a.knn_dist != b.knn_dist) return a.knn_dist < b.knn_dist;
                return a.doc_id < b.doc_id;
              });
              if (items.size() > c->plan.knn_k) items.resize(c->plan.knn_k);
            }
            rows.reserve(items.size());
            for (auto& it : items) rows.push_back(std::move(it.row));
          }

          if (c->error_msg.empty() && !c->plan.reduce_stages.empty()) {
            int64_t deadline_ms =
                (c->plan.timeout_ms > 0) ? (c->start_ms + c->plan.timeout_ms) : 0;
            if (!ExecuteAggPipelineStages(rows, c->plan.reduce_stages, deadline_ms, reduce_err)) {
              c->error_msg = std::move(reduce_err);
            }
          }
          if (c->error_msg.empty()) {
            c->final_rows = std::move(rows);
          }
        }
        c->finished = true;
        to_unblock = c->bc;
        c->bc = nullptr;
      }
    }
    g_active_aggs.Unblock(c, to_unblock);
  };

  for (const auto& ep : actx->shards) {
    if (ep.is_myself) {
      GetSearchExecutor().Submit([actx, sidx = ep.shard_idx, nid = ep.node_id, OnDone]() {
        RpcAggMapResponse resp = ExecuteLocalAggMap(actx->plan.map_req);
        OnDone(actx, sidx, nid, true, "", std::move(resp));
      });
    } else {
      mgr.RpcClient().SendAsync(
          ep.node_id, ep.ip, ep.rpc_port, RpcOpCode::kRpcAggMap, encoded, timeout_ms,
          [actx, sidx = ep.shard_idx, nid = ep.node_id, OnDone](RpcResponse r) {
            if (r.status != RpcStatus::kOk) {
              OnDone(actx, sidx, nid, false,
                     std::string(RpcStatusToString(r.status)) +
                         (r.error_msg.empty() ? "" : (": " + r.error_msg)),
                     {});
              return;
            }
            if (r.flags & kRpcFlagError) {
              OnDone(actx, sidx, nid, false,
                     r.error_msg.empty() ? "remote aggregate error" : r.error_msg, {});
              return;
            }
            RpcAggMapResponse aresp;
            if (!DecodeAggMapResponse(r.payload, aresp)) {
              OnDone(actx, sidx, nid, false, "malformed aggregate response", {});
              return;
            }
            OnDone(actx, sidx, nid, true, "", std::move(aresp));
          });
    }
  }
  return REDISMODULE_OK;
}

int ExecuteClusterCursor(RedisModuleCtx* ctx, RpcCursorRequest req) {
  auto& mgr = ClusterManager::Instance();
  mgr.RefreshTopology(ctx);
  auto topo = mgr.GetTopology();

  uint32_t target_tag = ExtractCursorNodeTag(req.cursor_id);
  const ShardNodeInfo* owner = nullptr;
  if (topo && target_tag != 0) {
    for (const auto& n : topo->nodes) {
      if (n.is_myself) continue;
      if (ComputeCursorNodeTag(n.node_id) == target_tag) {
        owner = &n;
        break;
      }
    }
  }

  if (!owner) {
    return RedisModule_ReplyWithError(ctx, "ERR cursor not found");
  }
  if (!owner->is_healthy || !owner->rpc_port_discovered || owner->rpc_port <= 0 ||
      owner->ip.empty()) {
    return RedisModule_ReplyWithError(ctx, "ERR cluster cursor owner unavailable");
  }

  auto cctx = std::make_shared<ClusterCursorContext>();
  cctx->op = req.op;

  RedisModuleBlockedClient* bc = RedisModule_BlockClient(
      ctx, ClusterCursorReplyCallback, GenericTimeoutCallback,
      FreeSharedHolder<ClusterCursorContext>, 0);
  if (!bc) {
    return RedisModule_ReplyWithError(ctx, "ERR failed to block client for cluster cursor");
  }
  cctx->bc = bc;
  g_active_cursors.Register(bc, cctx);
  if (RedisModule_SetDisconnectCallback) {
    RedisModule_SetDisconnectCallback(bc, ClusterCursorDisconnectCallback);
  }

  std::string encoded;
  EncodeCursorRequest(req, encoded);
  int64_t timeout_ms = static_cast<int64_t>(GetClusterQueryTimeoutMs());
  std::string target_id = owner->node_id;

  mgr.RpcClient().SendAsync(
      owner->node_id, owner->ip, owner->rpc_port, RpcOpCode::kRpcCursor, encoded, timeout_ms,
      [cctx, target_id](RpcResponse r) {
        RedisModuleBlockedClient* to_unblock = nullptr;
        {
          std::lock_guard<std::mutex> lk(cctx->mu);
          if (cctx->finished || cctx->cancelled) return;
          if (r.status != RpcStatus::kOk) {
            cctx->error_msg = "ERR cluster cursor RPC failed (" +
                              (target_id.size() > 8 ? target_id.substr(0, 8) : target_id) + "): " +
                              std::string(RpcStatusToString(r.status)) +
                              (r.error_msg.empty() ? "" : (": " + r.error_msg));
          } else if (r.flags & kRpcFlagError) {
            cctx->error_msg = r.error_msg.empty() ? "ERR remote cursor error" : r.error_msg;
          } else if (!DecodeCursorResponse(r.payload, cctx->resp)) {
            cctx->error_msg = "ERR malformed cursor response";
          } else if (!cctx->resp.ok) {
            cctx->error_msg =
                cctx->resp.error_msg.empty() ? "ERR cursor not found" : cctx->resp.error_msg;
          }
          cctx->finished = true;
          to_unblock = cctx->bc;
          cctx->bc = nullptr;
        }
        g_active_cursors.Unblock(cctx, to_unblock);
      });
  return REDISMODULE_OK;
}

}  // namespace gemini_search
