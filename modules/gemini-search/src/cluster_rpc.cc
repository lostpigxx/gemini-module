#include "cluster_rpc.h"

#include <arpa/inet.h>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace gemini_search {

namespace {

constexpr size_t kReadChunkBytes = 65536;
constexpr size_t kMaxPendingOutboundBytes = 64u * 1024u * 1024u;
constexpr int kMaxPollTimeoutMs = 50;

bool SetNonBlockingAndCloexec(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
  int fd_flags = fcntl(fd, F_GETFD, 0);
  if (fd_flags >= 0) {
    (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
  }
  return true;
}

void ConfigureTcpSocket(int fd) {
  int one = 1;
  (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
  (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

ssize_t SendNoSigpipe(int fd, const void* buf, size_t len) {
#ifdef MSG_NOSIGNAL
  return send(fd, buf, len, MSG_NOSIGNAL);
#else
  return send(fd, buf, len, 0);
#endif
}

bool CreateWakeupPipe(int pipe_fds[2]) {
  if (pipe(pipe_fds) != 0) {
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
    return false;
  }
  if (!SetNonBlockingAndCloexec(pipe_fds[0]) || !SetNonBlockingAndCloexec(pipe_fds[1])) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
    return false;
  }
  return true;
}

void CloseWakeupPipe(int pipe_fds[2]) {
  if (pipe_fds[0] >= 0) {
    close(pipe_fds[0]);
    pipe_fds[0] = -1;
  }
  if (pipe_fds[1] >= 0) {
    close(pipe_fds[1]);
    pipe_fds[1] = -1;
  }
}

void CompactReadBuffer(std::string& buf, size_t& read_pos) {
  if (read_pos == 0) return;
  if (read_pos >= buf.size()) {
    buf.clear();
    read_pos = 0;
  } else if (read_pos >= 65536 && read_pos >= buf.size() / 2) {
    buf.erase(0, read_pos);
    read_pos = 0;
  }
}

int CreateListeningSocket(const std::string& bind_ip, int port, int& out_port, int& out_errno) {
  out_errno = 0;
  bool is_v6 = (bind_ip.find(':') != std::string::npos);
  int domain = is_v6 ? AF_INET6 : AF_INET;
  int fd = socket(domain, SOCK_STREAM, 0);
  if (fd < 0) {
    out_errno = errno;
    return -1;
  }

  if (!SetNonBlockingAndCloexec(fd)) {
    out_errno = errno;
    close(fd);
    return -1;
  }

  int one = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  ConfigureTcpSocket(fd);

  if (is_v6) {
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(static_cast<uint16_t>(port));
    if (bind_ip.empty() || bind_ip == "::" || bind_ip == "*") {
      addr.sin6_addr = in6addr_any;
    } else if (inet_pton(AF_INET6, bind_ip.c_str(), &addr.sin6_addr) != 1) {
      out_errno = EINVAL;
      close(fd);
      return -1;
    }
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      out_errno = errno;
      close(fd);
      return -1;
    }
    socklen_t alen = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &alen) != 0) {
      out_errno = errno;
      close(fd);
      return -1;
    }
    out_port = ntohs(addr.sin6_port);
  } else {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind_ip.empty() || bind_ip == "0.0.0.0" || bind_ip == "*") {
      addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr) != 1) {
      out_errno = EINVAL;
      close(fd);
      return -1;
    }
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      out_errno = errno;
      close(fd);
      return -1;
    }
    socklen_t alen = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &alen) != 0) {
      out_errno = errno;
      close(fd);
      return -1;
    }
    out_port = ntohs(addr.sin_port);
  }

  if (listen(fd, 128) != 0) {
    out_errno = errno;
    close(fd);
    return -1;
  }
  return fd;
}

std::string SockAddrToIp(const sockaddr_storage& ss) {
  char buf[INET6_ADDRSTRLEN] = {};
  if (ss.ss_family == AF_INET) {
    const auto* sin = reinterpret_cast<const sockaddr_in*>(&ss);
    if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) return std::string(buf);
  } else if (ss.ss_family == AF_INET6) {
    const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(&ss);
    if (inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf))) return std::string(buf);
  }
  return "";
}

}  // namespace

const char* RpcStatusToString(RpcStatus st) {
  switch (st) {
    case RpcStatus::kOk: return "OK";
    case RpcStatus::kTimeout: return "TIMEOUT";
    case RpcStatus::kDisconnected: return "DISCONNECTED";
    case RpcStatus::kConnectFailed: return "CONNECT_FAILED";
    case RpcStatus::kProtocolError: return "PROTOCOL_ERROR";
    case RpcStatus::kInvalidArgument: return "INVALID_ARGUMENT";
    case RpcStatus::kShutdown: return "SHUTDOWN";
  }
  return "UNKNOWN";
}

int64_t ClusterNowMs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000LL + static_cast<int64_t>(ts.tv_nsec) / 1000000LL;
}

// ============================================================================
// ClusterRpcServer
// ============================================================================

ClusterRpcServer::ClusterRpcServer() = default;

ClusterRpcServer::~ClusterRpcServer() { Stop(); }

void ClusterRpcServer::SetRequestHandler(RequestHandler handler) {
  std::lock_guard<std::mutex> lk(handler_mu_);
  request_handler_ = std::move(handler);
}

void ClusterRpcServer::SetPeerValidator(PeerValidator validator) {
  std::lock_guard<std::mutex> lk(handler_mu_);
  peer_validator_ = std::move(validator);
}

bool ClusterRpcServer::Start(const std::string& bind_ip, int port, bool allow_ephemeral_fallback) {
  if (running_.load(std::memory_order_acquire)) return true;
  if (port < 0 || port > 65535) return false;

  if (!CreateWakeupPipe(wakeup_pipe_)) return false;

  int actual_port = 0;
  int bind_err = 0;
  listen_fd_ = CreateListeningSocket(bind_ip, port, actual_port, bind_err);
  if (listen_fd_ < 0 && port != 0 && allow_ephemeral_fallback &&
      (bind_err == EADDRINUSE || bind_err == EACCES)) {
    listen_fd_ = CreateListeningSocket(bind_ip, 0, actual_port, bind_err);
  }
  if (listen_fd_ < 0) {
    CloseWakeupPipe(wakeup_pipe_);
    return false;
  }

  bound_port_.store(actual_port, std::memory_order_release);
  stop_requested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  io_thread_ = std::thread([this]() { IoLoop(); });
  return true;
}

void ClusterRpcServer::Stop() {
  if (!running_.load(std::memory_order_acquire) && !io_thread_.joinable()) {
    CloseAllSockets();
    return;
  }
  stop_requested_.store(true, std::memory_order_release);
  WakeupIo();
  if (io_thread_.joinable()) {
    io_thread_.join();
  }
  running_.store(false, std::memory_order_release);
  bound_port_.store(0, std::memory_order_release);
  CloseAllSockets();
  {
    std::lock_guard<std::mutex> lk(resp_mu_);
    pending_responses_.clear();
  }
}

RpcServerStats ClusterRpcServer::GetStats() const {
  RpcServerStats s;
  s.active_connections = active_connections_.load(std::memory_order_relaxed);
  s.total_connections = total_connections_.load(std::memory_order_relaxed);
  s.requests_handled = requests_handled_.load(std::memory_order_relaxed);
  s.rejected_frames = rejected_frames_.load(std::memory_order_relaxed);
  s.bytes_received = bytes_received_.load(std::memory_order_relaxed);
  s.bytes_sent = bytes_sent_.load(std::memory_order_relaxed);
  return s;
}

void ClusterRpcServer::CloseAllSockets() {
  for (auto& [id, conn] : conns_) {
    if (conn.fd >= 0) close(conn.fd);
  }
  conns_.clear();
  active_connections_.store(0, std::memory_order_relaxed);
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    listen_fd_ = -1;
  }
  CloseWakeupPipe(wakeup_pipe_);
}

void ClusterRpcServer::WakeupIo() {
  if (wakeup_pipe_[1] >= 0) {
    char b = 1;
    ssize_t r = write(wakeup_pipe_[1], &b, 1);
    (void)r;
  }
}

void ClusterRpcServer::DrainWakeupPipe() {
  char buf[64];
  while (wakeup_pipe_[0] >= 0) {
    ssize_t n = read(wakeup_pipe_[0], buf, sizeof(buf));
    if (n <= 0) break;
  }
}

void ClusterRpcServer::EnqueueResponse(uint64_t conn_id, std::string encoded_frame) {
  {
    std::lock_guard<std::mutex> lk(resp_mu_);
    if (stop_requested_.load(std::memory_order_acquire)) return;
    pending_responses_.push_back({conn_id, std::move(encoded_frame)});
  }
  WakeupIo();
}

void ClusterRpcServer::FlushPendingResponses() {
  std::deque<OutboundMsg> local;
  {
    std::lock_guard<std::mutex> lk(resp_mu_);
    local.swap(pending_responses_);
  }
  for (auto& msg : local) {
    auto it = conns_.find(msg.conn_id);
    if (it == conns_.end() || it->second.closing) continue;
    it->second.write_queue.push_back(std::move(msg.encoded_frame));
  }
}

void ClusterRpcServer::AcceptPending() {
  while (true) {
    sockaddr_storage ss{};
    socklen_t slen = sizeof(ss);
    int cfd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&ss), &slen);
    if (cfd < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (!SetNonBlockingAndCloexec(cfd)) {
      close(cfd);
      continue;
    }
    ConfigureTcpSocket(cfd);
    std::string peer_ip = SockAddrToIp(ss);

    PeerValidator validator;
    {
      std::lock_guard<std::mutex> lk(handler_mu_);
      validator = peer_validator_;
    }
    if (validator && !validator(peer_ip)) {
      rejected_frames_.fetch_add(1, std::memory_order_relaxed);
      close(cfd);
      continue;
    }

    uint64_t cid = next_conn_id_++;
    Connection conn;
    conn.id = cid;
    conn.fd = cfd;
    conn.peer_ip = std::move(peer_ip);
    conns_.emplace(cid, std::move(conn));
    total_connections_.fetch_add(1, std::memory_order_relaxed);
    active_connections_.store(conns_.size(), std::memory_order_relaxed);
  }
}

bool ClusterRpcServer::HandleRead(Connection& conn) {
  char tmp[kReadChunkBytes];
  while (true) {
    ssize_t n = recv(conn.fd, tmp, sizeof(tmp), 0);
    if (n > 0) {
      bytes_received_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
      conn.read_buf.append(tmp, static_cast<size_t>(n));
      if (conn.read_buf.size() - conn.read_pos > kMaxRpcPayloadBytes + kRpcHeaderBytes + 65536) {
        rejected_frames_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      // Parse as many frames as available
      while (true) {
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(conn.read_buf.data() + conn.read_pos);
        size_t avail = conn.read_buf.size() - conn.read_pos;
        if (avail < kRpcHeaderBytes) break;

        // Fast header validation before buffering full payload
        RpcFrameHeader hdr;
        FrameDecodeStatus hst = DecodeRpcHeader(ptr, avail, hdr);
        if (hst != FrameDecodeStatus::kOk) {
          rejected_frames_.fetch_add(1, std::memory_order_relaxed);
          return false;
        }
        if (avail < kRpcHeaderBytes + static_cast<size_t>(hdr.payload_len)) {
          break;
        }

        RpcFrame frame;
        size_t consumed = 0;
        FrameDecodeStatus st = DecodeRpcFrame(ptr, avail, frame, consumed);
        if (st == FrameDecodeStatus::kIncomplete) break;
        if (st != FrameDecodeStatus::kOk) {
          rejected_frames_.fetch_add(1, std::memory_order_relaxed);
          return false;
        }
        conn.read_pos += consumed;
        requests_handled_.fetch_add(1, std::memory_order_relaxed);

        RequestHandler handler;
        {
          std::lock_guard<std::mutex> lk(handler_mu_);
          handler = request_handler_;
        }

        uint64_t cid = conn.id;
        uint64_t req_id = frame.header.request_id;
        ResponseSender sender = [this, cid, req_id](RpcOpCode resp_op, uint16_t flags,
                                                    std::string payload) {
          std::string encoded;
          if (EncodeRpcFrame(resp_op, static_cast<uint16_t>(flags | kRpcFlagResponse), req_id,
                             payload, encoded)) {
            EnqueueResponse(cid, std::move(encoded));
          }
        };

        if (handler) {
          handler(frame, conn.peer_ip, std::move(sender));
        } else {
          RpcErrorPayload err{1, "no RPC handler registered"};
          std::string err_bytes;
          EncodeErrorPayload(err, err_bytes);
          sender(frame.header.opcode, kRpcFlagError, std::move(err_bytes));
        }
      }
      CompactReadBuffer(conn.read_buf, conn.read_pos);
    } else if (n == 0) {
      return false;  // Peer closed cleanly
    } else {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
      return false;
    }
  }
}

bool ClusterRpcServer::HandleWrite(Connection& conn) {
  while (!conn.write_queue.empty()) {
    const std::string& front = conn.write_queue.front();
    const char* ptr = front.data() + conn.write_offset;
    size_t rem = front.size() - conn.write_offset;
    ssize_t n = SendNoSigpipe(conn.fd, ptr, rem);
    if (n > 0) {
      bytes_sent_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
      conn.write_offset += static_cast<size_t>(n);
      if (conn.write_offset >= front.size()) {
        conn.write_queue.pop_front();
        conn.write_offset = 0;
      }
    } else if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
      return false;
    } else {
      return false;
    }
  }
  return true;
}

void ClusterRpcServer::IoLoop() {
  std::vector<pollfd> pfds;
  std::vector<uint64_t> conn_ids;

  while (!stop_requested_.load(std::memory_order_acquire)) {
    FlushPendingResponses();

    pfds.clear();
    conn_ids.clear();
    pfds.reserve(2 + conns_.size());
    conn_ids.reserve(conns_.size());

    pfds.push_back({wakeup_pipe_[0], POLLIN, 0});
    pfds.push_back({listen_fd_, POLLIN, 0});
    for (const auto& [id, conn] : conns_) {
      short ev = POLLIN;
      if (!conn.write_queue.empty()) ev |= POLLOUT;
      pfds.push_back({conn.fd, ev, 0});
      conn_ids.push_back(id);
    }

    int rc = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), kMaxPollTimeoutMs);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (rc == 0) continue;

    if (pfds[0].revents & POLLIN) {
      DrainWakeupPipe();
    }
    if (pfds[1].revents & POLLIN) {
      AcceptPending();
    }

    std::vector<uint64_t> to_close;
    for (size_t i = 0; i < conn_ids.size(); i++) {
      short rev = pfds[2 + i].revents;
      if (rev == 0) continue;
      uint64_t cid = conn_ids[i];
      auto it = conns_.find(cid);
      if (it == conns_.end()) continue;

      bool ok = true;
      if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
        // Still try reading any buffered data on POLLHUP
        if (rev & POLLIN) {
          ok = HandleRead(it->second);
        } else {
          ok = false;
        }
      } else {
        if (rev & POLLIN) {
          ok = HandleRead(it->second);
        }
        // Flush inline responses generated during HandleRead immediately
        if (ok) {
          FlushPendingResponses();
        }
        if (ok && (!it->second.write_queue.empty()) && ((rev & POLLOUT) || (rev & POLLIN))) {
          ok = HandleWrite(it->second);
        }
      }
      if (!ok) {
        it->second.closing = true;
        to_close.push_back(cid);
      }
    }

    if (!to_close.empty()) {
      for (uint64_t cid : to_close) {
        auto it = conns_.find(cid);
        if (it != conns_.end()) {
          if (it->second.fd >= 0) close(it->second.fd);
          conns_.erase(it);
        }
      }
      active_connections_.store(conns_.size(), std::memory_order_relaxed);
    }
  }

  for (auto& [id, conn] : conns_) {
    if (conn.fd >= 0) close(conn.fd);
  }
  conns_.clear();
  active_connections_.store(0, std::memory_order_relaxed);
}

// ============================================================================
// ClusterRpcClient
// ============================================================================

ClusterRpcClient::ClusterRpcClient() = default;

ClusterRpcClient::~ClusterRpcClient() { Stop(); }

bool ClusterRpcClient::Start() {
  if (running_.load(std::memory_order_acquire)) return true;
  if (!CreateWakeupPipe(wakeup_pipe_)) return false;
  stop_requested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  io_thread_ = std::thread([this]() { IoLoop(); });
  return true;
}

void ClusterRpcClient::Stop() {
  if (!running_.load(std::memory_order_acquire) && !io_thread_.joinable()) {
    CloseWakeupPipe(wakeup_pipe_);
    return;
  }
  stop_requested_.store(true, std::memory_order_release);
  WakeupIo();
  if (io_thread_.joinable()) {
    io_thread_.join();
  }
  running_.store(false, std::memory_order_release);
  CloseWakeupPipe(wakeup_pipe_);

  // Fail any queued sends that arrived concurrently with shutdown
  std::vector<CompletedCallback> leftover;
  {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    while (!queued_sends_.empty()) {
      auto& qs = queued_sends_.front();
      if (qs.callback) {
        RpcResponse resp;
        resp.status = RpcStatus::kShutdown;
        resp.error_msg = "RPC client shut down";
        leftover.push_back({std::move(qs.callback), std::move(resp)});
      }
      queued_sends_.pop_front();
    }
    queued_cancels_.clear();
    queued_disconnects_.clear();
    has_prune_ = false;
    prune_active_set_.clear();
  }
  DispatchCompletions(leftover);
}

uint64_t ClusterRpcClient::SendAsync(const std::string& node_id, const std::string& ip, int port,
                                     RpcOpCode opcode, std::string_view payload, int64_t timeout_ms,
                                     RpcCallback cb, uint16_t flags) {
  if (!running_.load(std::memory_order_acquire) || stop_requested_.load(std::memory_order_acquire)) {
    if (cb) {
      RpcResponse resp;
      resp.status = RpcStatus::kShutdown;
      resp.error_msg = "RPC client not running";
      cb(std::move(resp));
    }
    return 0;
  }

  if (node_id.empty() || ip.empty() || port <= 0 || port > 65535) {
    if (cb) {
      RpcResponse resp;
      resp.status = RpcStatus::kInvalidArgument;
      resp.error_msg = "invalid target endpoint";
      cb(std::move(resp));
    }
    return 0;
  }

  uint64_t req_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
  std::string encoded;
  if (!EncodeRpcFrame(opcode, flags, req_id, payload, encoded)) {
    if (cb) {
      RpcResponse resp;
      resp.status = RpcStatus::kInvalidArgument;
      resp.error_msg = "failed to encode RPC frame (payload too large or invalid opcode)";
      cb(std::move(resp));
    }
    return 0;
  }

  int64_t now = ClusterNowMs();
  int64_t eff_timeout = timeout_ms > 0 ? timeout_ms : 5000;

  {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    if (stop_requested_.load(std::memory_order_acquire)) {
      if (cb) {
        RpcResponse resp;
        resp.status = RpcStatus::kShutdown;
        resp.error_msg = "RPC client stopping";
        cb(std::move(resp));
      }
      return 0;
    }
    QueuedSend qs;
    qs.request_id = req_id;
    qs.node_id = node_id;
    qs.ip = ip;
    qs.port = port;
    qs.encoded_frame = std::move(encoded);
    qs.sent_ms = now;
    qs.deadline_ms = now + eff_timeout;
    qs.callback = std::move(cb);
    queued_sends_.push_back(std::move(qs));
  }
  requests_sent_.fetch_add(1, std::memory_order_relaxed);
  WakeupIo();
  return req_id;
}

RpcResponse ClusterRpcClient::SendSync(const std::string& node_id, const std::string& ip, int port,
                                       RpcOpCode opcode, std::string_view payload,
                                       int64_t timeout_ms, uint16_t flags) {
  struct SyncState {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    RpcResponse resp;
  };
  auto state = std::make_shared<SyncState>();
  SendAsync(
      node_id, ip, port, opcode, payload, timeout_ms,
      [state](RpcResponse r) {
        {
          std::lock_guard<std::mutex> lk(state->mu);
          state->resp = std::move(r);
          state->done = true;
        }
        state->cv.notify_one();
      },
      flags);

  std::unique_lock<std::mutex> lk(state->mu);
  state->cv.wait(lk, [&]() { return state->done; });
  return std::move(state->resp);
}

bool ClusterRpcClient::CancelRequest(uint64_t request_id) {
  if (request_id == 0 || !running_.load(std::memory_order_acquire)) return false;
  {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    queued_cancels_.push_back(request_id);
  }
  WakeupIo();
  return true;
}

void ClusterRpcClient::DisconnectPeer(const std::string& node_id) {
  if (!running_.load(std::memory_order_acquire)) return;
  {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    queued_disconnects_.push_back(node_id);
  }
  WakeupIo();
}

void ClusterRpcClient::PrunePeers(const std::unordered_set<std::string>& active_node_ids) {
  if (!running_.load(std::memory_order_acquire)) return;
  {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    has_prune_ = true;
    prune_active_set_ = active_node_ids;
  }
  WakeupIo();
}

RpcClientStats ClusterRpcClient::GetStats() const {
  RpcClientStats s;
  s.active_connections = active_connections_.load(std::memory_order_relaxed);
  s.inflight_requests = inflight_count_.load(std::memory_order_relaxed);
  s.requests_sent = requests_sent_.load(std::memory_order_relaxed);
  s.requests_succeeded = requests_succeeded_.load(std::memory_order_relaxed);
  s.requests_failed = requests_failed_.load(std::memory_order_relaxed);
  s.requests_timed_out = requests_timed_out_.load(std::memory_order_relaxed);
  return s;
}

void ClusterRpcClient::WakeupIo() {
  if (wakeup_pipe_[1] >= 0) {
    char b = 1;
    ssize_t r = write(wakeup_pipe_[1], &b, 1);
    (void)r;
  }
}

void ClusterRpcClient::DrainWakeupPipe() {
  char buf[64];
  while (wakeup_pipe_[0] >= 0) {
    ssize_t n = read(wakeup_pipe_[0], buf, sizeof(buf));
    if (n <= 0) break;
  }
}

void ClusterRpcClient::UpdateActiveConnStat() {
  uint64_t active = 0;
  for (const auto& [id, peer] : peers_) {
    if (peer.fd >= 0 && peer.state == ConnState::kConnected) active++;
  }
  active_connections_.store(active, std::memory_order_relaxed);
  inflight_count_.store(inflight_.size(), std::memory_order_relaxed);
}

void ClusterRpcClient::DispatchCompletions(std::vector<CompletedCallback>& completions) {
  for (auto& item : completions) {
    if (item.cb) item.cb(std::move(item.resp));
  }
  completions.clear();
}

void ClusterRpcClient::FailPeer(PeerConn& peer, RpcStatus status, const std::string& msg,
                                std::vector<CompletedCallback>& completions) {
  if (peer.fd >= 0) {
    close(peer.fd);
    peer.fd = -1;
  }
  peer.state = ConnState::kDisconnected;
  peer.read_buf.clear();
  peer.read_pos = 0;
  peer.write_queue.clear();
  peer.write_offset = 0;

  int64_t now = ClusterNowMs();
  for (uint64_t req_id : peer.req_ids) {
    auto it = inflight_.find(req_id);
    if (it == inflight_.end()) continue;
    requests_failed_.fetch_add(1, std::memory_order_relaxed);
    RpcResponse resp;
    resp.status = status;
    resp.error_msg = msg;
    resp.rtt_ms = now - it->second.sent_ms;
    if (it->second.callback) {
      completions.push_back({std::move(it->second.callback), std::move(resp)});
    }
    inflight_.erase(it);
  }
  peer.req_ids.clear();
  UpdateActiveConnStat();
}

bool ClusterRpcClient::StartConnect(PeerConn& peer, std::vector<CompletedCallback>& completions) {
  bool is_v6 = (peer.ip.find(':') != std::string::npos);
  int fd = socket(is_v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    FailPeer(peer, RpcStatus::kConnectFailed, "socket() failed", completions);
    return false;
  }
  if (!SetNonBlockingAndCloexec(fd)) {
    close(fd);
    FailPeer(peer, RpcStatus::kConnectFailed, "fcntl() failed", completions);
    return false;
  }
  ConfigureTcpSocket(fd);

  int rc = -1;
  if (is_v6) {
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(static_cast<uint16_t>(peer.port));
    if (inet_pton(AF_INET6, peer.ip.c_str(), &addr.sin6_addr) != 1) {
      close(fd);
      FailPeer(peer, RpcStatus::kInvalidArgument, "invalid IPv6 address", completions);
      return false;
    }
    rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  } else {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(peer.port));
    if (inet_pton(AF_INET, peer.ip.c_str(), &addr.sin_addr) != 1) {
      close(fd);
      FailPeer(peer, RpcStatus::kInvalidArgument, "invalid IPv4 address", completions);
      return false;
    }
    rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  }

  if (rc == 0) {
    peer.fd = fd;
    peer.state = ConnState::kConnected;
    UpdateActiveConnStat();
    return true;
  }
  if (errno == EINPROGRESS) {
    peer.fd = fd;
    peer.state = ConnState::kConnecting;
    return true;
  }
  std::string err = std::string("connect() failed: ") + std::strerror(errno);
  close(fd);
  FailPeer(peer, RpcStatus::kConnectFailed, err, completions);
  return false;
}

void ClusterRpcClient::HandleConnectComplete(PeerConn& peer,
                                             std::vector<CompletedCallback>& completions) {
  int err = 0;
  socklen_t len = sizeof(err);
  if (getsockopt(peer.fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
    std::string msg = std::string("async connect failed: ") + std::strerror(err != 0 ? err : errno);
    FailPeer(peer, RpcStatus::kConnectFailed, msg, completions);
    return;
  }
  peer.state = ConnState::kConnected;
  UpdateActiveConnStat();
}

void ClusterRpcClient::HandlePeerWrite(PeerConn& peer,
                                       std::vector<CompletedCallback>& completions) {
  while (!peer.write_queue.empty()) {
    const std::string& front = peer.write_queue.front();
    const char* ptr = front.data() + peer.write_offset;
    size_t rem = front.size() - peer.write_offset;
    ssize_t n = SendNoSigpipe(peer.fd, ptr, rem);
    if (n > 0) {
      peer.write_offset += static_cast<size_t>(n);
      if (peer.write_offset >= front.size()) {
        peer.write_queue.pop_front();
        peer.write_offset = 0;
      }
    } else if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      FailPeer(peer, RpcStatus::kDisconnected,
               std::string("send() failed: ") + std::strerror(errno), completions);
      return;
    } else {
      FailPeer(peer, RpcStatus::kDisconnected, "peer closed on write", completions);
      return;
    }
  }
}

void ClusterRpcClient::HandlePeerRead(PeerConn& peer,
                                      std::vector<CompletedCallback>& completions) {
  char tmp[kReadChunkBytes];
  while (true) {
    ssize_t n = recv(peer.fd, tmp, sizeof(tmp), 0);
    if (n > 0) {
      peer.read_buf.append(tmp, static_cast<size_t>(n));
      while (true) {
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(peer.read_buf.data() + peer.read_pos);
        size_t avail = peer.read_buf.size() - peer.read_pos;
        if (avail < kRpcHeaderBytes) break;

        RpcFrameHeader hdr;
        FrameDecodeStatus hst = DecodeRpcHeader(ptr, avail, hdr);
        if (hst != FrameDecodeStatus::kOk) {
          FailPeer(peer, RpcStatus::kProtocolError, "invalid RPC response header", completions);
          return;
        }
        if (avail < kRpcHeaderBytes + static_cast<size_t>(hdr.payload_len)) break;

        RpcFrame frame;
        size_t consumed = 0;
        FrameDecodeStatus st = DecodeRpcFrame(ptr, avail, frame, consumed);
        if (st == FrameDecodeStatus::kIncomplete) break;
        if (st != FrameDecodeStatus::kOk) {
          FailPeer(peer, RpcStatus::kProtocolError, "corrupt RPC response frame", completions);
          return;
        }
        peer.read_pos += consumed;

        uint64_t req_id = frame.header.request_id;
        peer.req_ids.erase(req_id);
        auto it = inflight_.find(req_id);
        if (it == inflight_.end()) {
          // Timed out or cancelled earlier; safely discard late response
          continue;
        }
        int64_t now = ClusterNowMs();
        RpcResponse resp;
        resp.status = RpcStatus::kOk;
        resp.opcode = frame.header.opcode;
        resp.flags = frame.header.flags;
        resp.rtt_ms = now - it->second.sent_ms;
        if (frame.header.flags & kRpcFlagError) {
          RpcErrorPayload err_p;
          if (DecodeErrorPayload(frame.payload, err_p)) {
            resp.error_msg = std::move(err_p.message);
          } else {
            resp.error_msg = "remote RPC error";
          }
          resp.payload = std::move(frame.payload);
        } else {
          resp.payload = std::move(frame.payload);
        }
        requests_succeeded_.fetch_add(1, std::memory_order_relaxed);
        if (it->second.callback) {
          completions.push_back({std::move(it->second.callback), std::move(resp)});
        }
        inflight_.erase(it);
        inflight_count_.store(inflight_.size(), std::memory_order_relaxed);
      }
      CompactReadBuffer(peer.read_buf, peer.read_pos);
    } else if (n == 0) {
      FailPeer(peer, RpcStatus::kDisconnected, "peer closed connection", completions);
      return;
    } else {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      FailPeer(peer, RpcStatus::kDisconnected,
               std::string("recv() failed: ") + std::strerror(errno), completions);
      return;
    }
  }
}

void ClusterRpcClient::ProcessCommands(std::vector<CompletedCallback>& completions) {
  std::deque<QueuedSend> sends;
  std::vector<uint64_t> cancels;
  std::vector<std::string> disconnects;
  bool do_prune = false;
  std::unordered_set<std::string> prune_set;

  {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    sends.swap(queued_sends_);
    cancels.swap(queued_cancels_);
    disconnects.swap(queued_disconnects_);
    if (has_prune_) {
      do_prune = true;
      prune_set.swap(prune_active_set_);
      has_prune_ = false;
    }
  }

  for (const auto& nid : disconnects) {
    auto it = peers_.find(nid);
    if (it != peers_.end()) {
      FailPeer(it->second, RpcStatus::kDisconnected, "peer disconnected by caller", completions);
      peers_.erase(it);
    }
  }

  if (do_prune) {
    for (auto it = peers_.begin(); it != peers_.end();) {
      if (prune_set.count(it->first) == 0) {
        FailPeer(it->second, RpcStatus::kDisconnected, "peer removed from topology", completions);
        it = peers_.erase(it);
      } else {
        ++it;
      }
    }
  }

  for (uint64_t req_id : cancels) {
    auto it = inflight_.find(req_id);
    if (it != inflight_.end()) {
      auto pit = peers_.find(it->second.node_id);
      if (pit != peers_.end()) pit->second.req_ids.erase(req_id);
      inflight_.erase(it);
    }
  }

  for (auto& qs : sends) {
    auto& peer = peers_[qs.node_id];
    if (peer.node_id.empty()) peer.node_id = qs.node_id;

    // Endpoint changed: reset old connection cleanly
    if (peer.fd >= 0 && (peer.ip != qs.ip || peer.port != qs.port)) {
      FailPeer(peer, RpcStatus::kDisconnected, "peer endpoint changed", completions);
    }
    peer.ip = qs.ip;
    peer.port = qs.port;

    // Protect against unbounded outbound queue growth
    size_t queued_bytes = 0;
    for (const auto& f : peer.write_queue) queued_bytes += f.size();
    if (queued_bytes + qs.encoded_frame.size() > kMaxPendingOutboundBytes) {
      requests_failed_.fetch_add(1, std::memory_order_relaxed);
      if (qs.callback) {
        RpcResponse resp;
        resp.status = RpcStatus::kDisconnected;
        resp.error_msg = "peer outbound queue limit exceeded";
        completions.push_back({std::move(qs.callback), std::move(resp)});
      }
      continue;
    }

    PendingRequest pr;
    pr.request_id = qs.request_id;
    pr.node_id = qs.node_id;
    pr.sent_ms = qs.sent_ms;
    pr.deadline_ms = qs.deadline_ms;
    pr.callback = std::move(qs.callback);
    inflight_.emplace(qs.request_id, std::move(pr));
    peer.req_ids.insert(qs.request_id);
    peer.write_queue.push_back(std::move(qs.encoded_frame));

    if (peer.state == ConnState::kDisconnected) {
      StartConnect(peer, completions);
    }
  }

  UpdateActiveConnStat();
}

void ClusterRpcClient::SweepTimeouts(int64_t now_ms,
                                     std::vector<CompletedCallback>& completions) {
  for (auto it = inflight_.begin(); it != inflight_.end();) {
    if (now_ms >= it->second.deadline_ms) {
      uint64_t req_id = it->first;
      auto pit = peers_.find(it->second.node_id);
      if (pit != peers_.end()) {
        pit->second.req_ids.erase(req_id);
      }
      requests_timed_out_.fetch_add(1, std::memory_order_relaxed);
      requests_failed_.fetch_add(1, std::memory_order_relaxed);
      if (it->second.callback) {
        RpcResponse resp;
        resp.status = RpcStatus::kTimeout;
        resp.error_msg = "RPC request timed out";
        resp.rtt_ms = now_ms - it->second.sent_ms;
        completions.push_back({std::move(it->second.callback), std::move(resp)});
      }
      it = inflight_.erase(it);
    } else {
      ++it;
    }
  }
  inflight_count_.store(inflight_.size(), std::memory_order_relaxed);
}

int ClusterRpcClient::ComputePollTimeoutMs(int64_t now_ms) const {
  int timeout = kMaxPollTimeoutMs;
  for (const auto& [id, req] : inflight_) {
    int64_t rem = req.deadline_ms - now_ms;
    if (rem <= 0) return 0;
    if (rem < timeout) timeout = static_cast<int>(rem);
  }
  return timeout;
}

void ClusterRpcClient::IoLoop() {
  std::vector<pollfd> pfds;
  std::vector<std::string> peer_keys;
  std::vector<CompletedCallback> completions;

  while (!stop_requested_.load(std::memory_order_acquire)) {
    ProcessCommands(completions);
    int64_t now = ClusterNowMs();
    SweepTimeouts(now, completions);
    DispatchCompletions(completions);

    pfds.clear();
    peer_keys.clear();
    pfds.reserve(1 + peers_.size());
    peer_keys.reserve(peers_.size());

    pfds.push_back({wakeup_pipe_[0], POLLIN, 0});
    for (const auto& [nid, peer] : peers_) {
      if (peer.fd < 0) continue;
      short ev = 0;
      if (peer.state == ConnState::kConnecting) {
        ev = POLLOUT;
      } else {
        ev = POLLIN;
        if (!peer.write_queue.empty()) ev |= POLLOUT;
      }
      pfds.push_back({peer.fd, ev, 0});
      peer_keys.push_back(nid);
    }

    int poll_ms = ComputePollTimeoutMs(ClusterNowMs());
    int rc = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), poll_ms);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if (rc > 0) {
      if (pfds[0].revents & POLLIN) {
        DrainWakeupPipe();
      }
      for (size_t i = 0; i < peer_keys.size(); i++) {
        short rev = pfds[1 + i].revents;
        if (rev == 0) continue;
        auto it = peers_.find(peer_keys[i]);
        if (it == peers_.end() || it->second.fd < 0) continue;

        auto& peer = it->second;
        if (peer.state == ConnState::kConnecting) {
          if (rev & (POLLOUT | POLLERR | POLLHUP)) {
            HandleConnectComplete(peer, completions);
            if (peer.state == ConnState::kConnected && !peer.write_queue.empty()) {
              HandlePeerWrite(peer, completions);
            }
          }
          continue;
        }

        if (rev & (POLLERR | POLLNVAL)) {
          FailPeer(peer, RpcStatus::kDisconnected, "socket poll error", completions);
          continue;
        }
        if (rev & POLLIN) {
          HandlePeerRead(peer, completions);
        }
        if (peer.fd >= 0 && (rev & POLLOUT) && !peer.write_queue.empty()) {
          HandlePeerWrite(peer, completions);
        }
        if (peer.fd >= 0 && (rev & POLLHUP)) {
          FailPeer(peer, RpcStatus::kDisconnected, "peer hangup", completions);
        }
      }
    }

    SweepTimeouts(ClusterNowMs(), completions);
    DispatchCompletions(completions);
  }

  // Shutdown: close all sockets and fail all inflight requests
  for (auto& [nid, peer] : peers_) {
    FailPeer(peer, RpcStatus::kShutdown, "RPC client shut down", completions);
  }
  peers_.clear();
  for (auto& [id, req] : inflight_) {
    if (req.callback) {
      RpcResponse resp;
      resp.status = RpcStatus::kShutdown;
      resp.error_msg = "RPC client shut down";
      completions.push_back({std::move(req.callback), std::move(resp)});
    }
  }
  inflight_.clear();
  UpdateActiveConnStat();
  DispatchCompletions(completions);
}

}  // namespace gemini_search
