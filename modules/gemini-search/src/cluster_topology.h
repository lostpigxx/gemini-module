#pragma once

#ifndef REDISMODULE_API
#define REDISMODULE_API extern
#define _CLUSTER_TOPO_API_DEFINED
#endif

extern "C" {
#include "redismodule.h"
}

#ifdef _CLUSTER_TOPO_API_DEFINED
#undef REDISMODULE_API
#undef _CLUSTER_TOPO_API_DEFINED
#endif

#include "cluster_codec.h"
#include "cluster_rpc.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gemini_search {

constexpr uint16_t kHelloFlagNone = 0;
constexpr uint16_t kHelloFlagRequestReply = 1u << 0;

struct ShardNodeInfo {
  std::string node_id;      // 40-byte Redis Cluster node ID
  std::string ip;           // Node IP address
  int redis_port = 0;       // Redis client port
  int rpc_port = 0;         // Discovered internal RPC port (0 if not yet discovered)
  uint64_t schema_epoch = 0;
  int64_t last_hello_ms = 0;
  int flags = 0;            // REDISMODULE_NODE_*
  bool is_myself = false;
  bool is_master = false;
  bool is_healthy = false;  // !(flags & (REDISMODULE_NODE_FAIL | REDISMODULE_NODE_PFAIL))
  bool rpc_port_discovered = false;
};

struct ClusterTopology {
  uint64_t version = 0;
  std::string my_node_id;
  int my_redis_port = 0;
  int my_rpc_port = 0;
  std::vector<ShardNodeInfo> nodes;    // All known cluster nodes sorted by node_id
  std::vector<ShardNodeInfo> masters;  // All master nodes sorted by node_id
  std::array<int16_t, 16384> slot_to_master{};

  ClusterTopology() { slot_to_master.fill(-1); }

  const ShardNodeInfo* FindMasterForSlot(uint16_t slot) const {
    if (slot >= 16384) return nullptr;
    int16_t idx = slot_to_master[slot];
    if (idx < 0 || static_cast<size_t>(idx) >= masters.size()) return nullptr;
    return &masters[static_cast<size_t>(idx)];
  }
};

class ClusterManager {
 public:
  static ClusterManager& Instance();

  ClusterManager(const ClusterManager&) = delete;
  ClusterManager& operator=(const ClusterManager&) = delete;

  // Initializes cluster detection, optional module args, RPC server/client, and Cluster Bus.
  // In standalone mode (non-cluster), starts no threads and opens no listening sockets.
  int Init(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);

  // Cleanly stops RPC server/client threads (called on Redis shutdown).
  void Shutdown();

  // Refreshes topology snapshot from Redis Cluster APIs (must be called on Redis main thread).
  void RefreshTopology(RedisModuleCtx* ctx);

  // Broadcasts local Hello (rpc_port + schema_epoch) over Cluster Bus (main thread).
  void BroadcastHello(RedisModuleCtx* ctx, bool request_reply);

  // Handles incoming Cluster Bus Hello message (main thread callback).
  void OnBusHello(RedisModuleCtx* ctx, const char* sender_id, const unsigned char* payload,
                  uint32_t len);

  // Fast inline-friendly checks (zero allocation, safe from any thread)
  bool IsClusterMode() const { return cluster_mode_.load(std::memory_order_acquire); }
  bool IsClusterEnabled() const { return cluster_enabled_.load(std::memory_order_acquire); }
  void SetClusterEnabled(bool enabled) { cluster_enabled_.store(enabled, std::memory_order_release); }
  bool IsClusterScatterEnabled() const {
    return cluster_mode_.load(std::memory_order_acquire) &&
           cluster_enabled_.load(std::memory_order_acquire) &&
           master_count_.load(std::memory_order_acquire) > 1;
  }

  // Thread-safe immutable snapshot access
  std::shared_ptr<const ClusterTopology> GetTopology() const;

  // True if any known remote node has not yet advertised its rpc_port
  bool HasUndiscoveredPeers() const;

  // Schema epoch management (for Phase 67+)
  uint64_t GetSchemaEpoch() const { return schema_epoch_.load(std::memory_order_acquire); }
  uint64_t AdvanceSchemaEpoch();
  void SetSchemaEpoch(uint64_t epoch) { schema_epoch_.store(epoch, std::memory_order_release); }

  int MyRpcPort() const { return my_rpc_port_.load(std::memory_order_acquire); }
  const std::string& MyNodeId() const { return my_node_id_; }

  ClusterRpcServer& RpcServer() { return rpc_server_; }
  ClusterRpcClient& RpcClient() { return rpc_client_; }
  const ClusterRpcServer& RpcServer() const { return rpc_server_; }
  const ClusterRpcClient& RpcClient() const { return rpc_client_; }

  // Detached thread-safe context for background RPC handlers that need Redis GIL
  std::mutex& DetachedCtxMutex() { return detached_ctx_mu_; }
  RedisModuleCtx* DetachedCtx() const { return detached_ctx_; }
  bool IsShuttingDown() const { return shutting_down_.load(std::memory_order_acquire); }

  // Register custom RPC opcode handler (used by Phase 67-70)
  void RegisterRpcHandler(RpcOpCode opcode, ClusterRpcServer::RequestHandler handler);

  // Periodic tick invoked from cluster discovery timer & maintenance timer (main thread)
  void OnClusterTick(RedisModuleCtx* ctx);

 private:
  ClusterManager() = default;
  ~ClusterManager() { Shutdown(); }

  struct PeerHelloInfo {
    int rpc_port = 0;
    uint64_t schema_epoch = 0;
    uint16_t protocol_ver = 0;
    int64_t last_hello_ms = 0;
  };

  void ParseModuleArgs(RedisModuleString** argv, int argc);
  std::string DetectBindIp(RedisModuleCtx* ctx, const std::string& node_ip);
  void DispatchRpcRequest(const RpcFrame& req, const std::string& peer_ip,
                          ClusterRpcServer::ResponseSender send_resp);
  void HandlePingRequest(const RpcFrame& req, ClusterRpcServer::ResponseSender send_resp);
  void SendHelloTo(RedisModuleCtx* ctx, const char* target_id, bool request_reply);

  std::atomic<bool> initialized_{false};
  std::atomic<bool> shutting_down_{false};
  std::atomic<bool> cluster_mode_{false};
  std::atomic<bool> cluster_enabled_{true};
  std::atomic<size_t> master_count_{0};
  std::atomic<int> my_rpc_port_{0};
  std::atomic<uint64_t> schema_epoch_{0};
  std::atomic<uint64_t> topology_version_{0};

  std::mutex detached_ctx_mu_;
  RedisModuleCtx* detached_ctx_ = nullptr;

  // Module-arg overrides
  int explicit_rpc_port_ = 0;
  std::string explicit_bind_ip_;
  std::string auth_token_;

  std::string my_node_id_;
  int my_redis_port_ = 0;
  int64_t last_broadcast_ms_ = 0;

  mutable std::mutex topo_mu_;
  std::shared_ptr<const ClusterTopology> topology_;
  std::unordered_map<std::string, PeerHelloInfo> peer_hellos_;

  mutable std::mutex handlers_mu_;
  std::unordered_map<uint16_t, ClusterRpcServer::RequestHandler> rpc_handlers_;

  ClusterRpcServer rpc_server_;
  ClusterRpcClient rpc_client_;
};

// Redis Module callback hooks
void OnClusterBusHelloCallback(RedisModuleCtx* ctx, const char* sender_id, uint8_t type,
                               const unsigned char* payload, uint32_t len);
void OnClusterShutdownEvent(RedisModuleCtx* ctx, RedisModuleEvent eid, uint64_t subevent,
                            void* data);

// FT._DEBUG CLUSTER [PING [timeout_ms]]
int HandleDebugClusterCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc);

}  // namespace gemini_search
