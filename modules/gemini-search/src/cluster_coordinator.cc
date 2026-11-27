#include "cluster_coordinator.h"
#include "search_internal.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
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
  size_t phase1_pending = 0;
  size_t phase2_pending = 0;
  bool cancelled = false;
  bool finished = false;
  std::string error_msg;

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

  std::string encoded_p1;
  EncodeSearchPhase1Request(sctx->plan.phase1_req, encoded_p1);
  int64_t timeout_ms = sctx->plan.timeout_ms > 0 ? sctx->plan.timeout_ms : 5000;

  for (const auto& ep : sctx->shards) {
    if (ep.is_myself) {
      GetSearchExecutor().Submit([sctx, sidx = ep.shard_idx, nid = ep.node_id]() {
        RpcSearchPhase1Response resp = ExecuteLocalSearchPhase1(sctx->plan.phase1_req);
        OnPhase1ShardDone(sctx, sidx, nid, true, "", std::move(resp));
      });
    } else {
      mgr.RpcClient().SendAsync(
          ep.node_id, ep.ip, ep.rpc_port, RpcOpCode::kRpcSearchPhase1, encoded_p1, timeout_ms,
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

  return REDISMODULE_OK;
}

}  // namespace gemini_search
