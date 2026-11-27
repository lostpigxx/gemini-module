#include "cluster_coordinator.h"
#include "cluster_topology.h"
#include "search_internal.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

namespace gemini_search {

namespace {

constexpr int64_t kFastDiscoveryTickMs = 200;
constexpr int64_t kSteadyDiscoveryTickMs = 1000;
constexpr int64_t kHelloBroadcastIntervalMs = 2000;

void ClusterDiscoveryTimerCallback(RedisModuleCtx* ctx, void* /*data*/) {
  auto& mgr = ClusterManager::Instance();
  if (!mgr.IsClusterMode()) return;
  mgr.OnClusterTick(ctx);
  if (RedisModule_CreateTimer) {
    mstime_t next_ms = mgr.HasUndiscoveredPeers() ? kFastDiscoveryTickMs : kSteadyDiscoveryTickMs;
    RedisModule_CreateTimer(ctx, next_ms, ClusterDiscoveryTimerCallback, nullptr);
  }
}

struct ClusterPingResult {
  std::string node_id;
  std::string ip;
  int rpc_port = 0;
  RpcStatus status = RpcStatus::kOk;
  int64_t rtt_ms = 0;
  uint32_t remote_rpc_port = 0;
  uint64_t remote_schema_epoch = 0;
  std::string error_msg;
};

struct ClusterPingContext {
  RedisModuleBlockedClient* bc = nullptr;
  std::mutex mu;
  std::vector<ClusterPingResult> results;
  size_t pending = 0;
  bool cancelled = false;
  bool finished = false;
};

std::mutex g_active_pings_mu;
std::unordered_map<RedisModuleBlockedClient*, std::shared_ptr<ClusterPingContext>> g_active_pings;

int ClusterPingReplyCallback(RedisModuleCtx* ctx, RedisModuleString** /*argv*/, int /*argc*/) {
  auto* holder = static_cast<std::shared_ptr<ClusterPingContext>*>(
      RedisModule_GetBlockedClientPrivateData(ctx));
  if (!holder || !(*holder)) {
    return RedisModule_ReplyWithError(ctx, "ERR internal cluster ping state missing");
  }
  auto& ping_ctx = **holder;
  std::lock_guard<std::mutex> lk(ping_ctx.mu);

  // Sort deterministically by node_id
  std::sort(ping_ctx.results.begin(), ping_ctx.results.end(),
            [](const ClusterPingResult& a, const ClusterPingResult& b) {
              return a.node_id < b.node_id;
            });

  RedisModule_ReplyWithArray(ctx, static_cast<long>(ping_ctx.results.size()));
  for (const auto& r : ping_ctx.results) {
    RedisModule_ReplyWithArray(ctx, 12);
    RedisModule_ReplyWithSimpleString(ctx, "node_id");
    RedisModule_ReplyWithStringBuffer(ctx, r.node_id.data(), r.node_id.size());
    RedisModule_ReplyWithSimpleString(ctx, "status");
    RedisModule_ReplyWithSimpleString(ctx, RpcStatusToString(r.status));
    RedisModule_ReplyWithSimpleString(ctx, "rtt_ms");
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(r.rtt_ms));
    RedisModule_ReplyWithSimpleString(ctx, "rpc_port");
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(r.remote_rpc_port));
    RedisModule_ReplyWithSimpleString(ctx, "schema_epoch");
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(r.remote_schema_epoch));
    RedisModule_ReplyWithSimpleString(ctx, "error");
    RedisModule_ReplyWithStringBuffer(ctx, r.error_msg.data(), r.error_msg.size());
  }
  return REDISMODULE_OK;
}

int ClusterPingTimeoutCallback(RedisModuleCtx* ctx, RedisModuleString** /*argv*/, int /*argc*/) {
  return RedisModule_ReplyWithError(ctx, "ERR cluster ping timed out");
}

void ClusterPingFreePrivData(RedisModuleCtx* /*ctx*/, void* privdata) {
  auto* holder = static_cast<std::shared_ptr<ClusterPingContext>*>(privdata);
  delete holder;
}

void ClusterPingDisconnectCallback(RedisModuleCtx* /*ctx*/, RedisModuleBlockedClient* bc) {
  std::shared_ptr<ClusterPingContext> ping_ctx;
  {
    std::lock_guard<std::mutex> lk(g_active_pings_mu);
    auto it = g_active_pings.find(bc);
    if (it != g_active_pings.end()) {
      ping_ctx = std::move(it->second);
      g_active_pings.erase(it);
    }
  }
  if (ping_ctx) {
    std::lock_guard<std::mutex> lk(ping_ctx->mu);
    ping_ctx->cancelled = true;
    ping_ctx->bc = nullptr;
  }
}

}  // namespace

ClusterManager& ClusterManager::Instance() {
  static ClusterManager instance;
  return instance;
}

void ClusterManager::ParseModuleArgs(RedisModuleString** argv, int argc) {
  if (!argv || argc <= 0) return;
  for (int i = 0; i < argc; i++) {
    auto arg = ArgView(argv[i]);
    if (MatchArg(arg, "CLUSTER_RPC_PORT") && i + 1 < argc) {
      i++;
      std::string val(ArgView(argv[i]));
      char* ep = nullptr;
      long p = std::strtol(val.c_str(), &ep, 10);
      if (*ep == '\0' && p > 0 && p <= 65535) {
        explicit_rpc_port_ = static_cast<int>(p);
      }
    } else if (MatchArg(arg, "CLUSTER_RPC_BIND") && i + 1 < argc) {
      i++;
      explicit_bind_ip_ = std::string(ArgView(argv[i]));
    } else if (MatchArg(arg, "CLUSTER_RPC_PORT_OFFSET") && i + 1 < argc) {
      i++;
      std::string val(ArgView(argv[i]));
      char* ep = nullptr;
      long off = std::strtol(val.c_str(), &ep, 10);
      if (*ep == '\0' && off >= 0 && off <= 50000) {
        GetConfig()["CLUSTER_RPC_PORT_OFFSET"] = val;
      }
    } else if (MatchArg(arg, "CLUSTER_ENABLED") && i + 1 < argc) {
      i++;
      std::string val(ArgView(argv[i]));
      if (val == "0" || MatchArg(val, "FALSE") || MatchArg(val, "NO")) {
        GetConfig()["CLUSTER_ENABLED"] = "0";
        cluster_enabled_.store(false, std::memory_order_release);
      } else if (val == "1" || MatchArg(val, "TRUE") || MatchArg(val, "YES")) {
        GetConfig()["CLUSTER_ENABLED"] = "1";
        cluster_enabled_.store(true, std::memory_order_release);
      }
    } else if (MatchArg(arg, "CLUSTER_AUTH_TOKEN") && i + 1 < argc) {
      i++;
      auth_token_ = std::string(ArgView(argv[i]));
    }
  }
}

std::string ClusterManager::DetectBindIp(RedisModuleCtx* ctx, const std::string& node_ip) {
  if (!explicit_bind_ip_.empty()) return explicit_bind_ip_;
  if (!node_ip.empty() && node_ip != "0.0.0.0" && node_ip != "::") {
    return node_ip;
  }

  // Query Redis CONFIG GET bind to stay consistent with Redis network binding
  RedisModuleCallReply* rep = RedisModule_Call(ctx, "CONFIG", "cc", "GET", "bind");
  if (rep) {
    if (RedisModule_CallReplyType(rep) == REDISMODULE_REPLY_ARRAY &&
        RedisModule_CallReplyLength(rep) >= 2) {
      RedisModuleCallReply* val_rep = RedisModule_CallReplyArrayElement(rep, 1);
      if (val_rep && RedisModule_CallReplyType(val_rep) == REDISMODULE_REPLY_STRING) {
        size_t len = 0;
        const char* s = RedisModule_CallReplyStringPtr(val_rep, &len);
        std::string bind_cfg(s, len);
        // Extract first non-wildcard address only if single bind address (e.g., "127.0.0.1")
        size_t first_non_sp = bind_cfg.find_first_not_of(" \t");
        if (first_non_sp != std::string::npos) {
          size_t next_sp = bind_cfg.find_first_of(" \t", first_non_sp);
          std::string first_addr = bind_cfg.substr(
              first_non_sp, next_sp == std::string::npos ? std::string::npos : next_sp - first_non_sp);
          bool has_more = (next_sp != std::string::npos &&
                           bind_cfg.find_first_not_of(" \t", next_sp) != std::string::npos);
          // Strip Redis 7 optional leading '-' prefix on bind addresses
          if (!first_addr.empty() && first_addr[0] == '-') first_addr.erase(0, 1);
          if (!has_more && first_addr != "*" && first_addr != "-*" && first_addr != "0.0.0.0" &&
              first_addr != "::") {
            RedisModule_FreeCallReply(rep);
            return first_addr;
          }
        }
      }
    }
    RedisModule_FreeCallReply(rep);
  }
  return "0.0.0.0";
}

int ClusterManager::Init(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (initialized_.exchange(true, std::memory_order_acq_rel)) {
    return REDISMODULE_OK;
  }

  ParseModuleArgs(argv, argc);
  cluster_enabled_.store(GetClusterEnabledConfig(), std::memory_order_release);

  int ctx_flags = RedisModule_GetContextFlags ? RedisModule_GetContextFlags(ctx) : 0;
  if ((ctx_flags & REDISMODULE_CTX_FLAGS_CLUSTER) == 0) {
    // Standalone mode: zero overhead — no listening socket, no threads, no bus registration
    cluster_mode_.store(false, std::memory_order_release);
    auto empty_topo = std::make_shared<ClusterTopology>();
    std::lock_guard<std::mutex> lk(topo_mu_);
    topology_ = std::move(empty_topo);
    return REDISMODULE_OK;
  }

  cluster_mode_.store(true, std::memory_order_release);
  shutting_down_.store(false, std::memory_order_release);
  SetClusterRdbMode(true);

  if (RedisModule_GetDetachedThreadSafeContext) {
    detached_ctx_ = RedisModule_GetDetachedThreadSafeContext(ctx);
  }

  const char* my_id = RedisModule_GetMyClusterID ? RedisModule_GetMyClusterID() : nullptr;
  if (my_id) {
    my_node_id_.assign(my_id, REDISMODULE_NODE_ID_LEN);
  }

  char ip_buf[64] = {};
  int redis_port = 0;
  int node_flags = 0;
  if (RedisModule_GetClusterNodeInfo && !my_node_id_.empty()) {
    RedisModule_GetClusterNodeInfo(ctx, my_node_id_.c_str(), ip_buf, nullptr, &redis_port,
                                   &node_flags);
  }
  my_redis_port_ = redis_port;

  std::string bind_ip = DetectBindIp(ctx, std::string(ip_buf));
  int target_rpc_port = explicit_rpc_port_;
  if (target_rpc_port <= 0) {
    int offset = GetClusterRpcPortOffset();
    if (redis_port > 0 && redis_port + offset <= 65535) {
      target_rpc_port = redis_port + offset;
    } else {
      target_rpc_port = 0;  // Ephemeral fallback
    }
  }

  rpc_server_.SetRequestHandler(
      [this](const RpcFrame& req, const std::string& peer_ip,
             ClusterRpcServer::ResponseSender send_resp) {
        DispatchRpcRequest(req, peer_ip, std::move(send_resp));
      });

  if (!rpc_server_.Start(bind_ip, target_rpc_port, /*allow_ephemeral_fallback=*/true)) {
    RedisModule_Log(ctx, "warning",
                    "GeminiSearch cluster: failed to start RPC server on %s:%d",
                    bind_ip.c_str(), target_rpc_port);
  } else {
    my_rpc_port_.store(rpc_server_.BoundPort(), std::memory_order_release);
    RedisModule_Log(ctx, "notice",
                    "GeminiSearch cluster: RPC server listening on %s:%d (node=%.8s)",
                    bind_ip.c_str(), my_rpc_port_.load(), my_node_id_.c_str());
  }

  if (!rpc_client_.Start()) {
    RedisModule_Log(ctx, "warning", "GeminiSearch cluster: failed to start RPC client");
  }

  if (RedisModule_RegisterClusterMessageReceiver) {
    RedisModule_RegisterClusterMessageReceiver(ctx, kClusterMsgHello, OnClusterBusHelloCallback);
  }

  if (RedisModule_SubscribeToServerEvent) {
    RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_Shutdown, OnClusterShutdownEvent);
  }

  RegisterClusterCoordinatorHandlers();
  RefreshTopology(ctx);
  BroadcastHello(ctx, /*request_reply=*/true);

  if (RedisModule_CreateTimer) {
    RedisModule_CreateTimer(ctx, kFastDiscoveryTickMs, ClusterDiscoveryTimerCallback, nullptr);
  }
  return REDISMODULE_OK;
}

void ClusterManager::Shutdown() {
  if (!initialized_.exchange(false, std::memory_order_acq_rel)) return;
  shutting_down_.store(true, std::memory_order_release);
  rpc_client_.Stop();
  rpc_server_.Stop();
  my_rpc_port_.store(0, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(detached_ctx_mu_);
    if (detached_ctx_ && RedisModule_FreeThreadSafeContext) {
      RedisModule_FreeThreadSafeContext(detached_ctx_);
      detached_ctx_ = nullptr;
    }
  }
}

void ClusterManager::RegisterRpcHandler(RpcOpCode opcode, ClusterRpcServer::RequestHandler handler) {
  std::lock_guard<std::mutex> lk(handlers_mu_);
  rpc_handlers_[static_cast<uint16_t>(opcode)] = std::move(handler);
}

void ClusterManager::DispatchRpcRequest(const RpcFrame& req, const std::string& peer_ip,
                                        ClusterRpcServer::ResponseSender send_resp) {
  if (req.header.opcode == RpcOpCode::kRpcPing) {
    HandlePingRequest(req, std::move(send_resp));
    return;
  }

  ClusterRpcServer::RequestHandler handler;
  {
    std::lock_guard<std::mutex> lk(handlers_mu_);
    auto it = rpc_handlers_.find(static_cast<uint16_t>(req.header.opcode));
    if (it != rpc_handlers_.end()) handler = it->second;
  }

  if (handler) {
    handler(req, peer_ip, std::move(send_resp));
    return;
  }

  RpcErrorPayload err{404, "unsupported RPC opcode"};
  std::string err_bytes;
  EncodeErrorPayload(err, err_bytes);
  send_resp(req.header.opcode, kRpcFlagError, std::move(err_bytes));
}

void ClusterManager::HandlePingRequest(const RpcFrame& req,
                                       ClusterRpcServer::ResponseSender send_resp) {
  RpcPingPayload ping;
  if (!DecodePingPayload(req.payload, ping)) {
    RpcErrorPayload err{400, "malformed PING payload"};
    std::string err_bytes;
    EncodeErrorPayload(err, err_bytes);
    send_resp(RpcOpCode::kRpcPong, kRpcFlagError, std::move(err_bytes));
    return;
  }

  RpcPongPayload pong;
  pong.responder_node_id = my_node_id_;
  pong.echo_timestamp_ms = ping.timestamp_ms;
  pong.responder_time_ms = static_cast<uint64_t>(ClusterNowMs());
  pong.schema_epoch = schema_epoch_.load(std::memory_order_acquire);
  pong.rpc_port = static_cast<uint32_t>(my_rpc_port_.load(std::memory_order_acquire));

  std::string out;
  EncodePongPayload(pong, out);
  send_resp(RpcOpCode::kRpcPong, kRpcFlagNone, std::move(out));
}

void ClusterManager::SendHelloTo(RedisModuleCtx* ctx, const char* target_id, bool request_reply) {
  if (!RedisModule_SendClusterMessage) return;
  int rpc_port = my_rpc_port_.load(std::memory_order_acquire);
  if (rpc_port <= 0) return;

  ClusterBusHello hello;
  hello.version = kClusterBusHelloVersion;
  hello.rpc_port = static_cast<uint32_t>(rpc_port);
  hello.schema_epoch = schema_epoch_.load(std::memory_order_acquire);
  hello.protocol_ver = kRpcProtocolVersion;
  hello.flags = request_reply ? kHelloFlagRequestReply : kHelloFlagNone;

  std::string encoded;
  EncodeClusterBusHello(hello, encoded);
  RedisModule_SendClusterMessage(ctx, target_id, kClusterMsgHello, encoded.data(),
                                 static_cast<uint32_t>(encoded.size()));
}

void ClusterManager::BroadcastHello(RedisModuleCtx* ctx, bool request_reply) {
  if (!IsClusterMode()) return;
  // target_id == nullptr broadcasts to all connected cluster peers
  SendHelloTo(ctx, nullptr, request_reply);
  last_broadcast_ms_ = ClusterNowMs();
}

void ClusterManager::OnBusHello(RedisModuleCtx* ctx, const char* sender_id,
                                const unsigned char* payload, uint32_t len) {
  if (!sender_id || !payload) return;
  ClusterBusHello hello;
  if (!DecodeClusterBusHello(payload, len, hello)) return;
  if (hello.rpc_port == 0 || hello.rpc_port > 65535) return;

  std::string sid(sender_id, REDISMODULE_NODE_ID_LEN);
  if (sid == my_node_id_) return;

  bool newly_discovered = false;
  bool epoch_changed = false;
  {
    std::lock_guard<std::mutex> lk(topo_mu_);
    auto it = peer_hellos_.find(sid);
    if (it == peer_hellos_.end() || it->second.rpc_port != static_cast<int>(hello.rpc_port)) {
      newly_discovered = true;
    }
    if (it == peer_hellos_.end() || it->second.schema_epoch != hello.schema_epoch) {
      epoch_changed = true;
    }
    PeerHelloInfo info;
    info.rpc_port = static_cast<int>(hello.rpc_port);
    info.schema_epoch = hello.schema_epoch;
    info.protocol_ver = hello.protocol_ver;
    info.last_hello_ms = ClusterNowMs();
    peer_hellos_[sid] = info;
  }

  // Reply directly if sender requested a reply or we hadn't discovered them yet
  if ((hello.flags & kHelloFlagRequestReply) != 0 || newly_discovered) {
    SendHelloTo(ctx, sid.c_str(), /*request_reply=*/false);
  }

  if (newly_discovered || epoch_changed) {
    RefreshTopology(ctx);
    if (hello.schema_epoch > schema_epoch_.load(std::memory_order_acquire)) {
      TriggerSchemaSyncIfNeeded(ctx);
    }
  }
}

void ClusterManager::RefreshTopology(RedisModuleCtx* ctx) {
  if (!IsClusterMode() || !RedisModule_GetClusterNodesList || !RedisModule_GetClusterNodeInfo) {
    return;
  }

  size_t num_nodes = 0;
  char** ids = RedisModule_GetClusterNodesList(ctx, &num_nodes);
  if (!ids) return;

  auto topo = std::make_shared<ClusterTopology>();
  topo->my_node_id = my_node_id_;
  topo->my_redis_port = my_redis_port_;
  topo->my_rpc_port = my_rpc_port_.load(std::memory_order_acquire);

  std::unordered_map<std::string, PeerHelloInfo> hellos_copy;
  {
    std::lock_guard<std::mutex> lk(topo_mu_);
    hellos_copy = peer_hellos_;
  }

  std::unordered_set<std::string> known_ids;
  known_ids.reserve(num_nodes);

  for (size_t i = 0; i < num_nodes; i++) {
    if (!ids[i]) continue;
    // Redis 7.2.11 RM_GetClusterNodesList allocates REDISMODULE_NODE_ID_LEN bytes without
    // a trailing NUL, whereas RM_GetClusterNodeInfo calls strlen(id). Always pass a NUL-terminated copy.
    std::string node_id_str(ids[i], REDISMODULE_NODE_ID_LEN);
    char ip[64] = {};
    char master_id[REDISMODULE_NODE_ID_LEN + 1] = {};
    int port = 0;
    int flags = 0;
    if (RedisModule_GetClusterNodeInfo(ctx, node_id_str.c_str(), ip, master_id, &port, &flags) !=
        REDISMODULE_OK) {
      continue;
    }

    ShardNodeInfo node;
    node.node_id = std::move(node_id_str);
    node.ip = std::string(ip);
    node.redis_port = port;
    node.flags = flags;
    node.is_myself = (flags & REDISMODULE_NODE_MYSELF) != 0;
    node.is_master = (flags & REDISMODULE_NODE_MASTER) != 0;
    node.is_healthy = (flags & (REDISMODULE_NODE_FAIL | REDISMODULE_NODE_PFAIL)) == 0;

    if (node.is_myself) {
      node.rpc_port = topo->my_rpc_port;
      node.schema_epoch = schema_epoch_.load(std::memory_order_acquire);
      node.rpc_port_discovered = (node.rpc_port > 0);
      if (node.redis_port > 0) {
        my_redis_port_ = node.redis_port;
        topo->my_redis_port = node.redis_port;
      }
    } else {
      auto hit = hellos_copy.find(node.node_id);
      if (hit != hellos_copy.end()) {
        node.rpc_port = hit->second.rpc_port;
        node.schema_epoch = hit->second.schema_epoch;
        node.last_hello_ms = hit->second.last_hello_ms;
        node.rpc_port_discovered = (node.rpc_port > 0);
      }
    }

    known_ids.insert(node.node_id);
    if (node.is_master) {
      topo->masters.push_back(node);
    }
    topo->nodes.push_back(std::move(node));
  }

  if (RedisModule_FreeClusterNodesList) {
    RedisModule_FreeClusterNodesList(ids);
  }

  auto by_id = [](const ShardNodeInfo& a, const ShardNodeInfo& b) { return a.node_id < b.node_id; };
  std::sort(topo->nodes.begin(), topo->nodes.end(), by_id);
  std::sort(topo->masters.begin(), topo->masters.end(), by_id);

  uint64_t new_ver = topology_version_.fetch_add(1, std::memory_order_acq_rel) + 1;
  topo->version = new_ver;
  master_count_.store(topo->masters.size(), std::memory_order_release);

  {
    std::lock_guard<std::mutex> lk(topo_mu_);
    topology_ = std::move(topo);
    // Prune stale peer_hellos for nodes no longer in cluster
    for (auto it = peer_hellos_.begin(); it != peer_hellos_.end();) {
      if (known_ids.count(it->first) == 0) {
        it = peer_hellos_.erase(it);
      } else {
        ++it;
      }
    }
  }

  if (rpc_client_.IsRunning()) {
    rpc_client_.PrunePeers(known_ids);
  }
}

void ClusterManager::OnClusterTick(RedisModuleCtx* ctx) {
  if (!IsClusterMode()) return;
  RefreshTopology(ctx);
  TriggerSchemaSyncIfNeeded(ctx);

  int64_t now = ClusterNowMs();
  bool missing_peers = HasUndiscoveredPeers();
  if (missing_peers || now - last_broadcast_ms_ >= kHelloBroadcastIntervalMs) {
    BroadcastHello(ctx, /*request_reply=*/missing_peers);
  }
}

std::shared_ptr<const ClusterTopology> ClusterManager::GetTopology() const {
  std::lock_guard<std::mutex> lk(topo_mu_);
  return topology_;
}

bool ClusterManager::HasUndiscoveredPeers() const {
  auto topo = GetTopology();
  if (!topo) return false;
  for (const auto& n : topo->nodes) {
    if (!n.is_myself && n.is_healthy && !n.rpc_port_discovered) {
      return true;
    }
  }
  return false;
}

uint64_t ClusterManager::AdvanceSchemaEpoch() {
  uint64_t cur = schema_epoch_.load(std::memory_order_acquire);
  uint64_t now = static_cast<uint64_t>(NowMs());
  uint64_t next = (now > cur) ? now : (cur + 1);
  while (!schema_epoch_.compare_exchange_weak(cur, next, std::memory_order_acq_rel)) {
    next = (now > cur) ? now : (cur + 1);
  }
  return next;
}

void OnClusterBusHelloCallback(RedisModuleCtx* ctx, const char* sender_id, uint8_t type,
                               const unsigned char* payload, uint32_t len) {
  if (type != kClusterMsgHello) return;
  ClusterManager::Instance().OnBusHello(ctx, sender_id, payload, len);
}

void OnClusterShutdownEvent(RedisModuleCtx* /*ctx*/, RedisModuleEvent /*eid*/,
                            uint64_t /*subevent*/, void* /*data*/) {
  ClusterManager::Instance().Shutdown();
}

int HandleDebugClusterCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  auto& mgr = ClusterManager::Instance();

  // Subcommand: FT._DEBUG CLUSTER PING [timeout_ms]
  if (argc >= 3 && MatchArg(ArgView(argv[2]), "PING")) {
    if (!mgr.IsClusterMode()) {
      return RedisModule_ReplyWithError(ctx, "ERR cluster mode is not enabled");
    }

    int64_t timeout_ms = static_cast<int64_t>(GetClusterQueryTimeoutMs());
    if (argc >= 4) {
      char* ep = nullptr;
      long long v = std::strtoll(ArgStr(argv[3]).c_str(), &ep, 10);
      if (*ep != '\0' || v <= 0) {
        return RedisModule_ReplyWithError(ctx, "ERR invalid ping timeout_ms");
      }
      timeout_ms = v;
    }

    mgr.RefreshTopology(ctx);
    auto topo = mgr.GetTopology();
    std::vector<ShardNodeInfo> targets;
    if (topo) {
      for (const auto& n : topo->nodes) {
        if (!n.is_myself && n.is_healthy && n.rpc_port_discovered && n.rpc_port > 0 &&
            !n.ip.empty()) {
          targets.push_back(n);
        }
      }
    }

    if (targets.empty()) {
      RedisModule_ReplyWithArray(ctx, 0);
      return REDISMODULE_OK;
    }

    auto ping_ctx = std::make_shared<ClusterPingContext>();
    ping_ctx->pending = targets.size();
    ping_ctx->results.reserve(targets.size());

    // Pass timeout=0 to RedisModule_BlockClient because ClusterRpcClient::SendAsync enforces
    // the exact timeout_ms deadline on every peer and guarantees callback completion.
    RedisModuleBlockedClient* bc = RedisModule_BlockClient(
        ctx, ClusterPingReplyCallback, ClusterPingTimeoutCallback, ClusterPingFreePrivData, 0);
    if (!bc) {
      return RedisModule_ReplyWithError(ctx, "ERR failed to block client for cluster ping");
    }
    ping_ctx->bc = bc;
    {
      std::lock_guard<std::mutex> lk(g_active_pings_mu);
      g_active_pings[bc] = ping_ctx;
    }
    if (RedisModule_SetDisconnectCallback) {
      RedisModule_SetDisconnectCallback(bc, ClusterPingDisconnectCallback);
    }

    RpcPingPayload ping_payload;
    ping_payload.sender_node_id = mgr.MyNodeId();
    ping_payload.timestamp_ms = static_cast<uint64_t>(ClusterNowMs());
    ping_payload.schema_epoch = mgr.GetSchemaEpoch();
    ping_payload.rpc_port = static_cast<uint32_t>(mgr.MyRpcPort());

    std::string encoded_ping;
    EncodePingPayload(ping_payload, encoded_ping);

    for (const auto& target : targets) {
      std::string tid = target.node_id;
      std::string tip = target.ip;
      int tport = target.rpc_port;
      mgr.RpcClient().SendAsync(
          tid, tip, tport, RpcOpCode::kRpcPing, encoded_ping, timeout_ms,
          [ping_ctx, tid, tip, tport](RpcResponse resp) {
            ClusterPingResult res;
            res.node_id = tid;
            res.ip = tip;
            res.rpc_port = tport;
            res.status = resp.status;
            res.rtt_ms = resp.rtt_ms;
            res.error_msg = resp.error_msg;
            if (resp.status == RpcStatus::kOk && (resp.flags & kRpcFlagError) == 0) {
              RpcPongPayload pong;
              if (DecodePongPayload(resp.payload, pong)) {
                res.remote_rpc_port = pong.rpc_port;
                res.remote_schema_epoch = pong.schema_epoch;
              } else {
                res.status = RpcStatus::kProtocolError;
                res.error_msg = "malformed PONG payload";
              }
            }

            RedisModuleBlockedClient* to_unblock = nullptr;
            {
              std::lock_guard<std::mutex> lk(ping_ctx->mu);
              ping_ctx->results.push_back(std::move(res));
              if (ping_ctx->pending > 0) ping_ctx->pending--;
              if (ping_ctx->pending == 0 && !ping_ctx->finished && !ping_ctx->cancelled) {
                ping_ctx->finished = true;
                to_unblock = ping_ctx->bc;
                ping_ctx->bc = nullptr;
              }
            }
            if (to_unblock) {
              {
                std::lock_guard<std::mutex> lk(g_active_pings_mu);
                g_active_pings.erase(to_unblock);
              }
              auto* holder = new std::shared_ptr<ClusterPingContext>(ping_ctx);
              RedisModule_UnblockClient(to_unblock, holder);
            }
          });
    }

    return REDISMODULE_OK;
  }

  // Default: FT._DEBUG CLUSTER (topology & RPC diagnostics)
  if (mgr.IsClusterMode()) {
    mgr.RefreshTopology(ctx);
  }
  auto topo = mgr.GetTopology();
  auto srv_stats = mgr.RpcServer().GetStats();
  auto cli_stats = mgr.RpcClient().GetStats();

  RedisModule_ReplyWithArray(ctx, 36);

  RedisModule_ReplyWithSimpleString(ctx, "mode");
  RedisModule_ReplyWithSimpleString(ctx, mgr.IsClusterMode() ? "cluster" : "standalone");

  RedisModule_ReplyWithSimpleString(ctx, "cluster_enabled");
  RedisModule_ReplyWithLongLong(ctx, mgr.IsClusterEnabled() ? 1 : 0);

  RedisModule_ReplyWithSimpleString(ctx, "scatter_enabled");
  RedisModule_ReplyWithLongLong(ctx, mgr.IsClusterScatterEnabled() ? 1 : 0);

  RedisModule_ReplyWithSimpleString(ctx, "my_node_id");
  RedisModule_ReplyWithStringBuffer(ctx, mgr.MyNodeId().data(), mgr.MyNodeId().size());

  RedisModule_ReplyWithSimpleString(ctx, "my_redis_port");
  RedisModule_ReplyWithLongLong(ctx, topo ? topo->my_redis_port : 0);

  RedisModule_ReplyWithSimpleString(ctx, "my_rpc_port");
  RedisModule_ReplyWithLongLong(ctx, mgr.MyRpcPort());

  RedisModule_ReplyWithSimpleString(ctx, "rpc_server_running");
  RedisModule_ReplyWithLongLong(ctx, mgr.RpcServer().IsRunning() ? 1 : 0);

  RedisModule_ReplyWithSimpleString(ctx, "schema_epoch");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(mgr.GetSchemaEpoch()));

  RedisModule_ReplyWithSimpleString(ctx, "topology_version");
  RedisModule_ReplyWithLongLong(ctx, topo ? static_cast<long long>(topo->version) : 0);

  RedisModule_ReplyWithSimpleString(ctx, "cluster_size");
  RedisModule_ReplyWithLongLong(ctx, topo ? static_cast<long long>(topo->nodes.size()) : 0);

  RedisModule_ReplyWithSimpleString(ctx, "master_count");
  RedisModule_ReplyWithLongLong(ctx, topo ? static_cast<long long>(topo->masters.size()) : 0);

  RedisModule_ReplyWithSimpleString(ctx, "rpc_server_active_conns");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(srv_stats.active_connections));

  RedisModule_ReplyWithSimpleString(ctx, "rpc_server_requests_handled");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(srv_stats.requests_handled));

  RedisModule_ReplyWithSimpleString(ctx, "rpc_server_rejected_frames");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(srv_stats.rejected_frames));

  RedisModule_ReplyWithSimpleString(ctx, "rpc_client_active_conns");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(cli_stats.active_connections));

  RedisModule_ReplyWithSimpleString(ctx, "rpc_client_requests_sent");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(cli_stats.requests_sent));

  RedisModule_ReplyWithSimpleString(ctx, "rpc_client_requests_succeeded");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(cli_stats.requests_succeeded));

  RedisModule_ReplyWithSimpleString(ctx, "nodes");
  if (!topo) {
    RedisModule_ReplyWithArray(ctx, 0);
  } else {
    RedisModule_ReplyWithArray(ctx, static_cast<long>(topo->nodes.size()));
    for (const auto& n : topo->nodes) {
      RedisModule_ReplyWithArray(ctx, 18);
      RedisModule_ReplyWithSimpleString(ctx, "node_id");
      RedisModule_ReplyWithStringBuffer(ctx, n.node_id.data(), n.node_id.size());
      RedisModule_ReplyWithSimpleString(ctx, "ip");
      RedisModule_ReplyWithStringBuffer(ctx, n.ip.data(), n.ip.size());
      RedisModule_ReplyWithSimpleString(ctx, "redis_port");
      RedisModule_ReplyWithLongLong(ctx, n.redis_port);
      RedisModule_ReplyWithSimpleString(ctx, "rpc_port");
      RedisModule_ReplyWithLongLong(ctx, n.rpc_port);
      RedisModule_ReplyWithSimpleString(ctx, "role");
      RedisModule_ReplyWithSimpleString(ctx, n.is_master ? "master" : "replica");
      RedisModule_ReplyWithSimpleString(ctx, "healthy");
      RedisModule_ReplyWithLongLong(ctx, n.is_healthy ? 1 : 0);
      RedisModule_ReplyWithSimpleString(ctx, "discovered");
      RedisModule_ReplyWithLongLong(ctx, n.rpc_port_discovered ? 1 : 0);
      RedisModule_ReplyWithSimpleString(ctx, "myself");
      RedisModule_ReplyWithLongLong(ctx, n.is_myself ? 1 : 0);
      RedisModule_ReplyWithSimpleString(ctx, "schema_epoch");
      RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(n.schema_epoch));
    }
  }
  return REDISMODULE_OK;
}

}  // namespace gemini_search
