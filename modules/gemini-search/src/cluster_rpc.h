#pragma once

#include "cluster_codec.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gemini_search {

enum class RpcStatus {
  kOk = 0,
  kTimeout,
  kDisconnected,
  kConnectFailed,
  kProtocolError,
  kInvalidArgument,
  kShutdown,
};

const char* RpcStatusToString(RpcStatus st);

struct RpcResponse {
  RpcStatus status = RpcStatus::kOk;
  RpcOpCode opcode = RpcOpCode::kRpcPong;
  uint16_t flags = kRpcFlagNone;
  std::string payload;
  std::string error_msg;
  int64_t rtt_ms = 0;
};

struct RpcServerStats {
  uint64_t active_connections = 0;
  uint64_t total_connections = 0;
  uint64_t requests_handled = 0;
  uint64_t rejected_frames = 0;
  uint64_t bytes_received = 0;
  uint64_t bytes_sent = 0;
};

struct RpcClientStats {
  uint64_t active_connections = 0;
  uint64_t inflight_requests = 0;
  uint64_t requests_sent = 0;
  uint64_t requests_succeeded = 0;
  uint64_t requests_failed = 0;
  uint64_t requests_timed_out = 0;
};

// Monotonic clock in milliseconds (pure POSIX, no redismodule.h)
int64_t ClusterNowMs();

// Async non-blocking TCP binary RPC server running on its own I/O thread.
class ClusterRpcServer {
 public:
  using ResponseSender = std::function<void(RpcOpCode resp_op, uint16_t flags, std::string payload)>;
  using RequestHandler =
      std::function<void(const RpcFrame& req, const std::string& peer_ip, ResponseSender send_resp)>;
  using PeerValidator = std::function<bool(const std::string& peer_ip)>;

  ClusterRpcServer();
  ~ClusterRpcServer();

  ClusterRpcServer(const ClusterRpcServer&) = delete;
  ClusterRpcServer& operator=(const ClusterRpcServer&) = delete;

  void SetRequestHandler(RequestHandler handler);
  void SetPeerValidator(PeerValidator validator);

  // Starts listening on bind_ip:port. If port == 0, binds an ephemeral port.
  // If port != 0 and bind fails with EADDRINUSE and allow_ephemeral_fallback is true,
  // falls back to an OS-assigned ephemeral port.
  bool Start(const std::string& bind_ip, int port, bool allow_ephemeral_fallback = true);
  void Stop();

  bool IsRunning() const { return running_.load(std::memory_order_acquire); }
  int BoundPort() const { return bound_port_.load(std::memory_order_acquire); }
  RpcServerStats GetStats() const;

 private:
  struct OutboundMsg {
    uint64_t conn_id = 0;
    std::string encoded_frame;
  };

  struct Connection {
    uint64_t id = 0;
    int fd = -1;
    std::string peer_ip;
    std::string read_buf;
    size_t read_pos = 0;
    std::deque<std::string> write_queue;
    size_t write_offset = 0;
    bool closing = false;
  };

  void IoLoop();
  void AcceptPending();
  bool HandleRead(Connection& conn);
  bool HandleWrite(Connection& conn);
  void FlushPendingResponses();
  void EnqueueResponse(uint64_t conn_id, std::string encoded_frame);
  void WakeupIo();
  void DrainWakeupPipe();
  void CloseAllSockets();

  int listen_fd_ = -1;
  int wakeup_pipe_[2] = {-1, -1};
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<int> bound_port_{0};
  std::thread io_thread_;

  mutable std::mutex handler_mu_;
  RequestHandler request_handler_;
  PeerValidator peer_validator_;

  std::mutex resp_mu_;
  std::deque<OutboundMsg> pending_responses_;

  std::unordered_map<uint64_t, Connection> conns_;
  uint64_t next_conn_id_ = 1;

  // Stats
  std::atomic<uint64_t> active_connections_{0};
  std::atomic<uint64_t> total_connections_{0};
  std::atomic<uint64_t> requests_handled_{0};
  std::atomic<uint64_t> rejected_frames_{0};
  std::atomic<uint64_t> bytes_received_{0};
  std::atomic<uint64_t> bytes_sent_{0};
};

// Async multiplexed TCP RPC client connection pool keyed by node_id.
class ClusterRpcClient {
 public:
  using RpcCallback = std::function<void(RpcResponse)>;

  ClusterRpcClient();
  ~ClusterRpcClient();

  ClusterRpcClient(const ClusterRpcClient&) = delete;
  ClusterRpcClient& operator=(const ClusterRpcClient&) = delete;

  bool Start();
  void Stop();
  bool IsRunning() const { return running_.load(std::memory_order_acquire); }

  // Asynchronous RPC call. Returns request_id (> 0) on acceptance, or 0 if rejected synchronously.
  uint64_t SendAsync(const std::string& node_id, const std::string& ip, int port,
                     RpcOpCode opcode, std::string_view payload, int64_t timeout_ms,
                     RpcCallback cb, uint16_t flags = kRpcFlagNone);

  // Synchronous wrapper over SendAsync (exception-free, uses condition_variable).
  RpcResponse SendSync(const std::string& node_id, const std::string& ip, int port,
                       RpcOpCode opcode, std::string_view payload, int64_t timeout_ms,
                       uint16_t flags = kRpcFlagNone);

  bool CancelRequest(uint64_t request_id);
  void DisconnectPeer(const std::string& node_id);
  void PrunePeers(const std::unordered_set<std::string>& active_node_ids);

  RpcClientStats GetStats() const;

 private:
  enum class ConnState {
    kDisconnected = 0,
    kConnecting,
    kConnected,
  };

  struct PendingRequest {
    uint64_t request_id = 0;
    std::string node_id;
    int64_t sent_ms = 0;
    int64_t deadline_ms = 0;
    RpcCallback callback;
  };

  struct PeerConn {
    std::string node_id;
    std::string ip;
    int port = 0;
    int fd = -1;
    ConnState state = ConnState::kDisconnected;
    std::string read_buf;
    size_t read_pos = 0;
    std::deque<std::string> write_queue;
    size_t write_offset = 0;
    std::unordered_set<uint64_t> req_ids;
  };

  struct QueuedSend {
    uint64_t request_id = 0;
    std::string node_id;
    std::string ip;
    int port = 0;
    std::string encoded_frame;
    int64_t sent_ms = 0;
    int64_t deadline_ms = 0;
    RpcCallback callback;
  };

  struct CompletedCallback {
    RpcCallback cb;
    RpcResponse resp;
  };

  void IoLoop();
  void ProcessCommands(std::vector<CompletedCallback>& completions);
  bool StartConnect(PeerConn& peer, std::vector<CompletedCallback>& completions);
  void HandleConnectComplete(PeerConn& peer, std::vector<CompletedCallback>& completions);
  void HandlePeerRead(PeerConn& peer, std::vector<CompletedCallback>& completions);
  void HandlePeerWrite(PeerConn& peer, std::vector<CompletedCallback>& completions);
  void FailPeer(PeerConn& peer, RpcStatus status, const std::string& msg,
                std::vector<CompletedCallback>& completions);
  void SweepTimeouts(int64_t now_ms, std::vector<CompletedCallback>& completions);
  int ComputePollTimeoutMs(int64_t now_ms) const;
  void DispatchCompletions(std::vector<CompletedCallback>& completions);
  void WakeupIo();
  void DrainWakeupPipe();
  void UpdateActiveConnStat();

  int wakeup_pipe_[2] = {-1, -1};
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::thread io_thread_;
  std::atomic<uint64_t> next_request_id_{1};

  // Command queue from callers -> I/O thread
  std::mutex cmd_mu_;
  std::deque<QueuedSend> queued_sends_;
  std::vector<uint64_t> queued_cancels_;
  std::vector<std::string> queued_disconnects_;
  bool has_prune_ = false;
  std::unordered_set<std::string> prune_active_set_;

  // I/O thread owned state
  std::unordered_map<std::string, PeerConn> peers_;
  std::unordered_map<uint64_t, PendingRequest> inflight_;

  // Stats
  std::atomic<uint64_t> active_connections_{0};
  std::atomic<uint64_t> inflight_count_{0};
  std::atomic<uint64_t> requests_sent_{0};
  std::atomic<uint64_t> requests_succeeded_{0};
  std::atomic<uint64_t> requests_failed_{0};
  std::atomic<uint64_t> requests_timed_out_{0};
};

}  // namespace gemini_search
