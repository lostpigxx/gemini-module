#include "cluster_rpc.h"

#include <arpa/inet.h>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <limits>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace gemini_search {

namespace {

constexpr size_t kReadChunkBytes = 65536;
constexpr size_t kMaxPendingOutboundBytes = 64u * 1024u * 1024u;
constexpr size_t kMaxPreAuthPayloadBytes = 1024u * 1024u;
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

int64_t SafeAddDeadlineMs(int64_t now_ms, int64_t timeout_ms) {
  if (timeout_ms <= 0) return now_ms;
  if (now_ms > std::numeric_limits<int64_t>::max() - timeout_ms) {
    return std::numeric_limits<int64_t>::max();
  }
  return now_ms + timeout_ms;
}

namespace {

// Self-contained FIPS 180-4 SHA-256 implementation (endian-independent, zero external deps).
class Sha256 {
 public:
  static constexpr size_t kBlockBytes = 64;
  static constexpr size_t kDigestBytes = 32;

  Sha256() { Reset(); }

  void Reset() {
    h_[0] = 0x6a09e667u;
    h_[1] = 0xbb67ae85u;
    h_[2] = 0x3c6ef372u;
    h_[3] = 0xa54ff53au;
    h_[4] = 0x510e527fu;
    h_[5] = 0x9b05688cu;
    h_[6] = 0x1f83d9abu;
    h_[7] = 0x5be0cd19u;
    total_bytes_ = 0;
    buf_len_ = 0;
  }

  void Update(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    total_bytes_ += len;
    size_t off = 0;
    if (buf_len_ > 0) {
      size_t take = std::min(kBlockBytes - buf_len_, len);
      std::memcpy(buf_ + buf_len_, data, take);
      buf_len_ += take;
      off += take;
      if (buf_len_ == kBlockBytes) {
        Transform(buf_);
        buf_len_ = 0;
      }
    }
    while (off + kBlockBytes <= len) {
      Transform(data + off);
      off += kBlockBytes;
    }
    if (off < len) {
      buf_len_ = len - off;
      std::memcpy(buf_, data + off, buf_len_);
    }
  }

  void Update(std::string_view sv) {
    Update(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
  }

  std::array<uint8_t, kDigestBytes> Final() {
    uint64_t total_bits = total_bytes_ * 8ULL;
    uint8_t pad = 0x80u;
    Update(&pad, 1);
    uint8_t zero = 0;
    while (buf_len_ != 56) {
      Update(&zero, 1);
    }
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) {
      len_be[i] = static_cast<uint8_t>((total_bits >> ((7 - i) * 8)) & 0xFFu);
    }
    Update(len_be, 8);

    std::array<uint8_t, kDigestBytes> digest{};
    for (size_t i = 0; i < 8; i++) {
      digest[i * 4 + 0] = static_cast<uint8_t>((h_[i] >> 24) & 0xFFu);
      digest[i * 4 + 1] = static_cast<uint8_t>((h_[i] >> 16) & 0xFFu);
      digest[i * 4 + 2] = static_cast<uint8_t>((h_[i] >> 8) & 0xFFu);
      digest[i * 4 + 3] = static_cast<uint8_t>(h_[i] & 0xFFu);
    }
    return digest;
  }

 private:
  static inline uint32_t Rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32u - n)); }

  void Transform(const uint8_t block[64]) {
    static constexpr uint32_t kK[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
        0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
        0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
        0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
        0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
        0xc67178f2u};

    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
      w[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) |
             (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
             static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];

    for (int i = 0; i < 64; i++) {
      uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
      uint32_t ch = (e & f) ^ ((~e) & g);
      uint32_t temp1 = h + s1 + ch + kK[i] + w[i];
      uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t temp2 = s0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
  }

  uint32_t h_[8]{};
  uint64_t total_bytes_ = 0;
  uint8_t buf_[kBlockBytes]{};
  size_t buf_len_ = 0;
};

bool ConstantTimeEqBytes(const uint8_t* a, const uint8_t* b, size_t n) {
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) {
    diff = static_cast<uint8_t>(diff | static_cast<uint8_t>(a[i] ^ b[i]));
  }
  return diff == 0;
}

bool EncodeAuthenticatedFrame(RpcOpCode opcode, uint16_t flags, uint64_t request_id,
                              std::string_view payload, std::string_view auth_token,
                              std::string& out) {
  if (!auth_token.empty()) {
    if (payload.size() > kMaxRpcPayloadBytes - kRpcAuthMacBytes) {
      return false;
    }
    uint16_t eff_flags = static_cast<uint16_t>(flags | kRpcFlagHasAuth);
    auto mac = ComputeRpcAuthMac(auth_token, opcode, eff_flags, request_id, payload);
    std::string auth_payload;
    auth_payload.reserve(kRpcAuthMacBytes + payload.size());
    auth_payload.append(reinterpret_cast<const char*>(mac.data()), mac.size());
    auth_payload.append(payload.data(), payload.size());
    return EncodeRpcFrame(opcode, eff_flags, request_id, auth_payload, out);
  }
  uint16_t eff_flags = static_cast<uint16_t>(flags & ~kRpcFlagHasAuth);
  return EncodeRpcFrame(opcode, eff_flags, request_id, payload, out);
}

bool IsExpectedResponseOpCode(RpcOpCode req_op, RpcOpCode resp_op, uint16_t resp_flags) {
  RpcOpCode expected = req_op;
  switch (req_op) {
    case RpcOpCode::kRpcPing: expected = RpcOpCode::kRpcPong; break;
    case RpcOpCode::kRpcDdlCreate:
    case RpcOpCode::kRpcDdlDrop:
    case RpcOpCode::kRpcDdlAlter:
    case RpcOpCode::kRpcDdlAlias:
    case RpcOpCode::kRpcDdlSyn:
    case RpcOpCode::kRpcDdlDict: expected = RpcOpCode::kRpcDdlResp; break;
    case RpcOpCode::kRpcSchemaSyncReq: expected = RpcOpCode::kRpcSchemaSyncResp; break;
    case RpcOpCode::kRpcSearchPhase1: expected = RpcOpCode::kRpcSearchPhase1Resp; break;
    case RpcOpCode::kRpcFetchDocs: expected = RpcOpCode::kRpcFetchDocsResp; break;
    case RpcOpCode::kRpcDfsTermStats: expected = RpcOpCode::kRpcDfsTermStatsResp; break;
    case RpcOpCode::kRpcHybridPhase1: expected = RpcOpCode::kRpcHybridPhase1Resp; break;
    case RpcOpCode::kRpcAggMap: expected = RpcOpCode::kRpcAggMapResp; break;
    case RpcOpCode::kRpcInfoStats: expected = RpcOpCode::kRpcInfoStatsResp; break;
    case RpcOpCode::kRpcTagVals: expected = RpcOpCode::kRpcTagValsResp; break;
    case RpcOpCode::kRpcSpellCheck: expected = RpcOpCode::kRpcSpellCheckResp; break;
    case RpcOpCode::kRpcSuggest: expected = RpcOpCode::kRpcSuggestResp; break;
    case RpcOpCode::kRpcCursor: expected = RpcOpCode::kRpcCursorResp; break;
    default: return false;
  }
  if (resp_op == expected) return true;
  if ((resp_flags & kRpcFlagError) != 0 && resp_op == req_op) return true;
  return false;
}

struct InflightSlotGuard {
  std::shared_ptr<std::atomic<uint64_t>> counter;
  std::atomic<bool> released{false};

  explicit InflightSlotGuard(std::shared_ptr<std::atomic<uint64_t>> c) : counter(std::move(c)) {}
  ~InflightSlotGuard() { Release(); }

  void Release() {
    if (!released.exchange(true, std::memory_order_acq_rel) && counter) {
      counter->fetch_sub(1, std::memory_order_acq_rel);
    }
  }
};

}  // namespace

std::array<uint8_t, kRpcAuthMacBytes> ComputeRpcAuthMac(std::string_view token, RpcOpCode opcode,
                                                        uint16_t flags, uint64_t request_id,
                                                        std::string_view payload) {
  // RFC 2104 HMAC-SHA256
  uint8_t key_block[Sha256::kBlockBytes] = {};
  if (token.size() > Sha256::kBlockBytes) {
    Sha256 kh;
    kh.Update(token);
    auto kd = kh.Final();
    std::memcpy(key_block, kd.data(), kd.size());
  } else if (!token.empty()) {
    std::memcpy(key_block, token.data(), token.size());
  }

  uint8_t ipad[Sha256::kBlockBytes];
  uint8_t opad[Sha256::kBlockBytes];
  for (size_t i = 0; i < Sha256::kBlockBytes; i++) {
    ipad[i] = static_cast<uint8_t>(key_block[i] ^ 0x36u);
    opad[i] = static_cast<uint8_t>(key_block[i] ^ 0x5cu);
  }

  // Build explicit little-endian authenticated header fields (covers opcode, flags/direction, request_id, length)
  uint8_t meta[2 + 2 + 8 + 8];
  uint16_t op_u16 = static_cast<uint16_t>(opcode);
  meta[0] = static_cast<uint8_t>(op_u16 & 0xFFu);
  meta[1] = static_cast<uint8_t>((op_u16 >> 8) & 0xFFu);
  meta[2] = static_cast<uint8_t>(flags & 0xFFu);
  meta[3] = static_cast<uint8_t>((flags >> 8) & 0xFFu);
  for (int i = 0; i < 8; i++) {
    meta[4 + i] = static_cast<uint8_t>((request_id >> (i * 8)) & 0xFFu);
  }
  uint64_t plen = static_cast<uint64_t>(payload.size());
  for (int i = 0; i < 8; i++) {
    meta[12 + i] = static_cast<uint8_t>((plen >> (i * 8)) & 0xFFu);
  }

  Sha256 inner;
  inner.Update(ipad, sizeof(ipad));
  inner.Update(std::string_view("GS-RPC-AUTH-v2", 14));
  inner.Update(meta, sizeof(meta));
  inner.Update(payload);
  auto inner_digest = inner.Final();

  Sha256 outer;
  outer.Update(opad, sizeof(opad));
  outer.Update(inner_digest.data(), inner_digest.size());
  return outer.Final();
}

uint64_t ComputeRpcAuthTag(std::string_view token, RpcOpCode opcode, uint64_t request_id,
                           std::string_view payload, uint16_t flags) {
  auto mac = ComputeRpcAuthMac(token, opcode, flags, request_id, payload);
  uint64_t tag = 0;
  for (size_t b = 0; b < 8; b++) {
    tag |= static_cast<uint64_t>(mac[b]) << (b * 8);
  }
  return tag;
}

// ============================================================================
// ClusterRpcServer
// ============================================================================

void ClusterRpcServer::OutboundSink::WakeupLocked() {
  if (wakeup_write_fd >= 0) {
    char b = 1;
    ssize_t r = write(wakeup_write_fd, &b, 1);
    (void)r;
  }
}

void ClusterRpcServer::OutboundSink::Enqueue(uint64_t conn_id, std::string encoded_frame) {
  std::lock_guard<std::mutex> lk(mu);
  if (!alive) return;
  pending_responses.push_back({conn_id, std::move(encoded_frame)});
  WakeupLocked();
}

ClusterRpcServer::ClusterRpcServer() = default;

ClusterRpcServer::~ClusterRpcServer() { Stop(); }

void ClusterRpcServer::SetRequestHandler(RequestHandler handler) {
  std::lock_guard<std::mutex> lk(handler_mu_);
  request_handler_ = std::move(handler);
}

void ClusterRpcServer::SetPeerValidator(PeerValidator validator) {
  {
    std::lock_guard<std::mutex> lk(handler_mu_);
    peer_validator_ = std::move(validator);
  }
  RevalidatePeers();
}

void ClusterRpcServer::RevalidatePeers() {
  revalidate_peers_.store(true, std::memory_order_release);
  WakeupIo();
}

void ClusterRpcServer::SetAuthToken(std::string token) {
  {
    std::lock_guard<std::mutex> lk(handler_mu_);
    auth_token_ = token;
  }
  {
    std::lock_guard<std::mutex> lk(outbound_sink_->mu);
    outbound_sink_->auth_token = std::move(token);
  }
}

bool ClusterRpcServer::HasAuthToken() const {
  std::lock_guard<std::mutex> lk(handler_mu_);
  return !auth_token_.empty();
}

void ClusterRpcServer::SetMaxInflightRequests(size_t max_inflight) {
  max_inflight_requests_.store(max_inflight, std::memory_order_release);
}

void ClusterRpcServer::SetMaxConnections(size_t max_connections) {
  max_connections_.store(max_connections, std::memory_order_release);
}

bool ClusterRpcServer::Start(const std::string& bind_ip, int port, bool allow_ephemeral_fallback) {
  if (running_.load(std::memory_order_acquire)) return true;
  if (port < 0 || port > 65535) return false;

  if (!CreateWakeupPipe(wakeup_pipe_)) return false;

  int actual_port = 0;
  int bind_err = 0;
  listen_fd_ = CreateListeningSocket(bind_ip, port, actual_port, bind_err);
  if (listen_fd_ < 0 && (bind_err == EADDRNOTAVAIL || bind_err == EINVAL) &&
      !bind_ip.empty() && bind_ip != "0.0.0.0" && bind_ip != "*" && bind_ip != "::") {
    // In Docker/NAT/K8s, cluster-announce-ip may not be a local interface address;
    // fall back to wildcard bind on the requested port (then IPv4 0.0.0.0, then ephemeral).
    std::string fallback_ip = (bind_ip.find(':') != std::string::npos) ? "::" : "0.0.0.0";
    listen_fd_ = CreateListeningSocket(fallback_ip, port, actual_port, bind_err);
    if (listen_fd_ < 0 && fallback_ip == "::") {
      listen_fd_ = CreateListeningSocket("0.0.0.0", port, actual_port, bind_err);
      fallback_ip = "0.0.0.0";
    }
    if (listen_fd_ < 0 && port != 0 && allow_ephemeral_fallback &&
        (bind_err == EADDRINUSE || bind_err == EACCES)) {
      listen_fd_ = CreateListeningSocket(fallback_ip, 0, actual_port, bind_err);
    }
  } else if (listen_fd_ < 0 && port != 0 && allow_ephemeral_fallback &&
             (bind_err == EADDRINUSE || bind_err == EACCES)) {
    listen_fd_ = CreateListeningSocket(bind_ip, 0, actual_port, bind_err);
  }
  if (listen_fd_ < 0) {
    CloseWakeupPipe(wakeup_pipe_);
    return false;
  }

  {
    std::lock_guard<std::mutex> lk(outbound_sink_->mu);
    outbound_sink_->alive = true;
    outbound_sink_->wakeup_write_fd = wakeup_pipe_[1];
    outbound_sink_->pending_responses.clear();
  }

  bound_port_.store(actual_port, std::memory_order_release);
  stop_requested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  io_thread_ = std::thread([this]() { IoLoop(); });
  return true;
}

void ClusterRpcServer::Stop() {
  if (!running_.load(std::memory_order_acquire) && !io_thread_.joinable()) {
    {
      std::lock_guard<std::mutex> lk(outbound_sink_->mu);
      outbound_sink_->alive = false;
      outbound_sink_->wakeup_write_fd = -1;
      outbound_sink_->pending_responses.clear();
    }
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
  // Invalidate outbound sink BEFORE closing wakeup_pipe_ so async ResponseSenders never race on fd.
  {
    std::lock_guard<std::mutex> lk(outbound_sink_->mu);
    outbound_sink_->alive = false;
    outbound_sink_->wakeup_write_fd = -1;
    outbound_sink_->pending_responses.clear();
  }
  CloseAllSockets();
}

RpcServerStats ClusterRpcServer::GetStats() const {
  RpcServerStats s;
  s.active_connections = active_connections_.load(std::memory_order_relaxed);
  s.total_connections = total_connections_.load(std::memory_order_relaxed);
  s.requests_handled = requests_handled_.load(std::memory_order_relaxed);
  s.rejected_frames = rejected_frames_.load(std::memory_order_relaxed);
  s.auth_rejected = auth_rejected_.load(std::memory_order_relaxed);
  s.peer_rejected = peer_rejected_.load(std::memory_order_relaxed);
  s.inflight_rejected = inflight_rejected_.load(std::memory_order_relaxed);
  s.inflight_requests = inflight_requests_->load(std::memory_order_relaxed);
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
  std::lock_guard<std::mutex> lk(outbound_sink_->mu);
  outbound_sink_->WakeupLocked();
}

void ClusterRpcServer::DrainWakeupPipe() {
  char buf[64];
  while (wakeup_pipe_[0] >= 0) {
    ssize_t n = read(wakeup_pipe_[0], buf, sizeof(buf));
    if (n <= 0) break;
  }
}

void ClusterRpcServer::EnqueueResponse(uint64_t conn_id, std::string encoded_frame) {
  outbound_sink_->Enqueue(conn_id, std::move(encoded_frame));
}

void ClusterRpcServer::FlushPendingResponses() {
  std::deque<OutboundMsg> local;
  {
    std::lock_guard<std::mutex> lk(outbound_sink_->mu);
    local.swap(outbound_sink_->pending_responses);
  }
  for (auto& msg : local) {
    auto it = conns_.find(msg.conn_id);
    if (it == conns_.end() || it->second.closing) continue;
    size_t sz = msg.encoded_frame.size();
    if (it->second.write_queue_bytes + sz > kMaxPendingOutboundBytes) {
      rejected_frames_.fetch_add(1, std::memory_order_relaxed);
      it->second.closing = true;
      continue;
    }
    it->second.write_queue_bytes += sz;
    it->second.write_queue.push_back(std::move(msg.encoded_frame));
  }
}

void ClusterRpcServer::RevalidateConnections() {
  PeerValidator validator;
  {
    std::lock_guard<std::mutex> lk(handler_mu_);
    validator = peer_validator_;
  }
  if (!validator) return;
  bool removed = false;
  for (auto it = conns_.begin(); it != conns_.end();) {
    if (!validator(it->second.peer_ip)) {
      peer_rejected_.fetch_add(1, std::memory_order_relaxed);
      if (it->second.fd >= 0) close(it->second.fd);
      it = conns_.erase(it);
      removed = true;
    } else {
      ++it;
    }
  }
  if (removed) {
    active_connections_.store(conns_.size(), std::memory_order_relaxed);
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
    size_t max_conns = max_connections_.load(std::memory_order_acquire);
    if (max_conns > 0 && conns_.size() >= max_conns) {
      rejected_frames_.fetch_add(1, std::memory_order_relaxed);
      close(cfd);
      continue;
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
      peer_rejected_.fetch_add(1, std::memory_order_relaxed);
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

      bool require_auth = HasAuthToken();
      size_t max_buf = (require_auth && !conn.authenticated)
                           ? (kMaxPreAuthPayloadBytes + kRpcHeaderBytes + 4096)
                           : (kMaxRpcPayloadBytes + kRpcHeaderBytes + 65536);
      if (conn.read_buf.size() - conn.read_pos > max_buf) {
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
        // Reject response frames sent to server or oversized pre-auth frames immediately
        if ((hdr.flags & kRpcFlagResponse) != 0) {
          rejected_frames_.fetch_add(1, std::memory_order_relaxed);
          return false;
        }
        if (require_auth && !conn.authenticated && hdr.payload_len > kMaxPreAuthPayloadBytes) {
          auth_rejected_.fetch_add(1, std::memory_order_relaxed);
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

        RequestHandler handler;
        std::string auth_token;
        {
          std::lock_guard<std::mutex> lk(handler_mu_);
          handler = request_handler_;
          auth_token = auth_token_;
        }

        // Verify HMAC-SHA256 MAC when CLUSTER_AUTH_TOKEN is configured, and reject
        // unexpected kRpcFlagHasAuth frames when server has no auth token configured.
        if (!auth_token.empty()) {
          bool auth_ok = false;
          if ((frame.header.flags & kRpcFlagHasAuth) != 0 &&
              frame.payload.size() >= kRpcAuthMacBytes) {
            const uint8_t* recv_mac = reinterpret_cast<const uint8_t*>(frame.payload.data());
            std::string_view body(frame.payload.data() + kRpcAuthMacBytes,
                                  frame.payload.size() - kRpcAuthMacBytes);
            auto expected_mac = ComputeRpcAuthMac(auth_token, frame.header.opcode,
                                                  frame.header.flags, frame.header.request_id, body);
            auth_ok = ConstantTimeEqBytes(recv_mac, expected_mac.data(), kRpcAuthMacBytes);
          }
          if (!auth_ok) {
            auth_rejected_.fetch_add(1, std::memory_order_relaxed);
            rejected_frames_.fetch_add(1, std::memory_order_relaxed);
            RpcErrorPayload err{401, "ERR unauthorized cluster RPC"};
            std::string err_bytes;
            EncodeErrorPayload(err, err_bytes);
            std::string encoded;
            if (EncodeRpcFrame(frame.header.opcode,
                               static_cast<uint16_t>(kRpcFlagResponse | kRpcFlagError),
                               frame.header.request_id, err_bytes, encoded)) {
              conn.write_queue_bytes += encoded.size();
              conn.write_queue.push_back(std::move(encoded));
              (void)HandleWrite(conn);
            }
            return false;
          }
          conn.authenticated = true;
          frame.payload.erase(0, kRpcAuthMacBytes);
        } else if ((frame.header.flags & kRpcFlagHasAuth) != 0) {
          auth_rejected_.fetch_add(1, std::memory_order_relaxed);
          rejected_frames_.fetch_add(1, std::memory_order_relaxed);
          RpcErrorPayload err{401, "ERR unexpected cluster RPC auth tag"};
          std::string err_bytes;
          EncodeErrorPayload(err, err_bytes);
          std::string encoded;
          if (EncodeRpcFrame(frame.header.opcode,
                             static_cast<uint16_t>(kRpcFlagResponse | kRpcFlagError),
                             frame.header.request_id, err_bytes, encoded)) {
            conn.write_queue_bytes += encoded.size();
            conn.write_queue.push_back(std::move(encoded));
            (void)HandleWrite(conn);
          }
          return false;
        }

        // Server-side inflight concurrency quota defense against DoS/OOM
        size_t max_inflight = max_inflight_requests_.load(std::memory_order_acquire);
        uint64_t prev_inflight = inflight_requests_->fetch_add(1, std::memory_order_acq_rel);
        if (max_inflight > 0 && prev_inflight >= max_inflight) {
          inflight_requests_->fetch_sub(1, std::memory_order_acq_rel);
          inflight_rejected_.fetch_add(1, std::memory_order_relaxed);
          RpcErrorPayload err{503, "ERR server busy: max inflight RPC quota exceeded"};
          std::string err_bytes;
          EncodeErrorPayload(err, err_bytes);
          std::string encoded;
          if (EncodeAuthenticatedFrame(frame.header.opcode,
                                       static_cast<uint16_t>(kRpcFlagResponse | kRpcFlagError),
                                       frame.header.request_id, err_bytes, auth_token, encoded)) {
            EnqueueResponse(conn.id, std::move(encoded));
          }
          continue;
        }

        requests_handled_.fetch_add(1, std::memory_order_relaxed);

        auto guard = std::make_shared<InflightSlotGuard>(inflight_requests_);
        std::weak_ptr<OutboundSink> weak_sink = outbound_sink_;
        uint64_t cid = conn.id;
        uint64_t req_id = frame.header.request_id;
        ResponseSender sender = [weak_sink, cid, req_id, guard](RpcOpCode resp_op, uint16_t flags,
                                                                std::string payload) {
          guard->Release();
          auto sink = weak_sink.lock();
          if (!sink) return;
          std::string tok;
          {
            std::lock_guard<std::mutex> lk(sink->mu);
            if (!sink->alive) return;
            tok = sink->auth_token;
          }
          std::string encoded;
          uint16_t resp_flags = static_cast<uint16_t>(flags | kRpcFlagResponse);
          if (EncodeAuthenticatedFrame(resp_op, resp_flags, req_id, payload, tok, encoded)) {
            sink->Enqueue(cid, std::move(encoded));
          } else {
            // Do not silently drop oversized (>32 MiB) responses; return an explicit error frame.
            RpcErrorPayload err{413, "ERR RPC response payload exceeds maximum frame size"};
            std::string err_bytes;
            EncodeErrorPayload(err, err_bytes);
            std::string err_encoded;
            if (EncodeAuthenticatedFrame(resp_op,
                                         static_cast<uint16_t>(kRpcFlagResponse | kRpcFlagError),
                                         req_id, err_bytes, tok, err_encoded)) {
              sink->Enqueue(cid, std::move(err_encoded));
            }
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
        size_t popped_sz = front.size();
        conn.write_queue.pop_front();
        conn.write_queue_bytes =
            (conn.write_queue_bytes >= popped_sz) ? (conn.write_queue_bytes - popped_sz) : 0;
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
    if (revalidate_peers_.exchange(false, std::memory_order_acq_rel)) {
      RevalidateConnections();
    }
    FlushPendingResponses();
    // Reap any connections marked closing (e.g., outbound write queue cap exceeded)
    bool reaped = false;
    for (auto it = conns_.begin(); it != conns_.end();) {
      if (it->second.closing) {
        if (it->second.fd >= 0) close(it->second.fd);
        it = conns_.erase(it);
        reaped = true;
      } else {
        ++it;
      }
    }
    if (reaped) {
      active_connections_.store(conns_.size(), std::memory_order_relaxed);
    }

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
  {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    if (!CreateWakeupPipe(wakeup_pipe_)) return false;
  }
  stop_requested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  io_thread_ = std::thread([this]() { IoLoop(); });
  return true;
}

void ClusterRpcClient::Stop() {
  if (!running_.load(std::memory_order_acquire) && !io_thread_.joinable()) {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    CloseWakeupPipe(wakeup_pipe_);
    return;
  }
  stop_requested_.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    WakeupIoLocked();
  }
  if (io_thread_.joinable()) {
    io_thread_.join();
  }
  running_.store(false, std::memory_order_release);

  // Close wakeup_pipe_ under cmd_mu_ and fail any queued sends that arrived concurrently with shutdown
  std::vector<CompletedCallback> leftover;
  {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    CloseWakeupPipe(wakeup_pipe_);
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

void ClusterRpcClient::SetAuthToken(std::string token) {
  std::lock_guard<std::mutex> lk(auth_mu_);
  auth_token_ = std::move(token);
}

void ClusterRpcClient::SetMaxInflightPerPeer(size_t max_inflight) {
  max_inflight_per_peer_.store(max_inflight, std::memory_order_release);
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

  std::string auth_token;
  {
    std::lock_guard<std::mutex> lk(auth_mu_);
    auth_token = auth_token_;
  }

  std::string encoded;
  uint16_t req_flags = static_cast<uint16_t>(flags & ~kRpcFlagResponse);
  bool enc_ok = EncodeAuthenticatedFrame(opcode, req_flags, req_id, payload, auth_token, encoded);

  if (!enc_ok) {
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
    if (stop_requested_.load(std::memory_order_acquire) || !running_.load(std::memory_order_acquire)) {
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
    qs.req_opcode = opcode;
    qs.node_id = node_id;
    qs.ip = ip;
    qs.port = port;
    qs.encoded_frame = std::move(encoded);
    qs.sent_ms = now;
    qs.deadline_ms = SafeAddDeadlineMs(now, eff_timeout);
    qs.callback = std::move(cb);
    queued_sends_.push_back(std::move(qs));
    requests_sent_.fetch_add(1, std::memory_order_relaxed);
    WakeupIoLocked();
  }
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
    WakeupIoLocked();
  }
  return true;
}

void ClusterRpcClient::DisconnectPeer(const std::string& node_id) {
  if (!running_.load(std::memory_order_acquire)) return;
  {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    queued_disconnects_.push_back(node_id);
    WakeupIoLocked();
  }
}

void ClusterRpcClient::PrunePeers(const std::unordered_set<std::string>& active_node_ids) {
  if (!running_.load(std::memory_order_acquire)) return;
  {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    has_prune_ = true;
    prune_active_set_ = active_node_ids;
    WakeupIoLocked();
  }
}

RpcClientStats ClusterRpcClient::GetStats() const {
  RpcClientStats s;
  s.active_connections = active_connections_.load(std::memory_order_relaxed);
  s.inflight_requests = inflight_count_.load(std::memory_order_relaxed);
  s.requests_sent = requests_sent_.load(std::memory_order_relaxed);
  s.requests_succeeded = requests_succeeded_.load(std::memory_order_relaxed);
  s.requests_failed = requests_failed_.load(std::memory_order_relaxed);
  s.requests_timed_out = requests_timed_out_.load(std::memory_order_relaxed);
  s.inflight_rejected = inflight_rejected_.load(std::memory_order_relaxed);
  return s;
}

void ClusterRpcClient::WakeupIoLocked() {
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

        // Validate response direction flag
        if ((frame.header.flags & kRpcFlagResponse) == 0) {
          FailPeer(peer, RpcStatus::kProtocolError, "RPC frame missing response flag", completions);
          return;
        }

        // Verify response HMAC-SHA256 when auth_token is configured, and reject unexpected auth tags
        std::string auth_token;
        {
          std::lock_guard<std::mutex> lk(auth_mu_);
          auth_token = auth_token_;
        }
        if (!auth_token.empty()) {
          if ((frame.header.flags & kRpcFlagHasAuth) == 0 ||
              frame.payload.size() < kRpcAuthMacBytes) {
            FailPeer(peer, RpcStatus::kProtocolError, "unauthenticated RPC response", completions);
            return;
          }
          const uint8_t* recv_mac = reinterpret_cast<const uint8_t*>(frame.payload.data());
          std::string_view body(frame.payload.data() + kRpcAuthMacBytes,
                                frame.payload.size() - kRpcAuthMacBytes);
          auto expected_mac = ComputeRpcAuthMac(auth_token, frame.header.opcode,
                                                frame.header.flags, frame.header.request_id, body);
          if (!ConstantTimeEqBytes(recv_mac, expected_mac.data(), kRpcAuthMacBytes)) {
            FailPeer(peer, RpcStatus::kProtocolError, "invalid RPC response MAC", completions);
            return;
          }
          frame.payload.erase(0, kRpcAuthMacBytes);
        } else if ((frame.header.flags & kRpcFlagHasAuth) != 0) {
          FailPeer(peer, RpcStatus::kProtocolError, "unexpected auth tag in RPC response",
                   completions);
          return;
        }

        // Prevent cross-peer response hijacking: verify req_id belongs to THIS peer
        uint64_t req_id = frame.header.request_id;
        auto it = inflight_.find(req_id);
        if (it == inflight_.end()) {
          peer.req_ids.erase(req_id);
          // Timed out or cancelled earlier; safely discard late response
          continue;
        }
        if (peer.req_ids.count(req_id) == 0 || it->second.node_id != peer.node_id) {
          FailPeer(peer, RpcStatus::kProtocolError, "cross-peer response request_id mismatch",
                   completions);
          return;
        }
        if (!IsExpectedResponseOpCode(it->second.req_opcode, frame.header.opcode,
                                      frame.header.flags)) {
          FailPeer(peer, RpcStatus::kProtocolError, "unexpected RPC response opcode", completions);
          return;
        }
        peer.req_ids.erase(req_id);

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

  // Build cancel set so requests cancelled immediately after SendAsync (before IoLoop woke up)
  // are never transmitted and existing inflight requests are cancelled cleanly.
  std::unordered_set<uint64_t> cancel_set(cancels.begin(), cancels.end());
  for (uint64_t req_id : cancels) {
    auto it = inflight_.find(req_id);
    if (it != inflight_.end()) {
      auto pit = peers_.find(it->second.node_id);
      if (pit != peers_.end()) pit->second.req_ids.erase(req_id);
      inflight_.erase(it);
    }
  }

  for (auto& qs : sends) {
    if (cancel_set.count(qs.request_id) > 0) {
      continue;
    }
    auto& peer = peers_[qs.node_id];
    if (peer.node_id.empty()) peer.node_id = qs.node_id;

    // Endpoint changed: reset old connection cleanly
    if (peer.fd >= 0 && (peer.ip != qs.ip || peer.port != qs.port)) {
      FailPeer(peer, RpcStatus::kDisconnected, "peer endpoint changed", completions);
    }
    peer.ip = qs.ip;
    peer.port = qs.port;

    // Protect against unbounded per-peer inflight concurrency & outbound queue growth
    size_t max_per_peer = max_inflight_per_peer_.load(std::memory_order_acquire);
    if (max_per_peer > 0 && peer.req_ids.size() >= max_per_peer) {
      inflight_rejected_.fetch_add(1, std::memory_order_relaxed);
      requests_failed_.fetch_add(1, std::memory_order_relaxed);
      if (qs.callback) {
        RpcResponse resp;
        resp.status = RpcStatus::kDisconnected;
        resp.error_msg = "peer max inflight RPC quota exceeded";
        completions.push_back({std::move(qs.callback), std::move(resp)});
      }
      continue;
    }

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
    pr.req_opcode = qs.req_opcode;
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

  // Shutdown (or fatal poll() exit): mark stopped, close all sockets, and fail all inflight & queued requests
  stop_requested_.store(true, std::memory_order_release);
  running_.store(false, std::memory_order_release);
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
  {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    while (!queued_sends_.empty()) {
      auto& qs = queued_sends_.front();
      if (qs.callback) {
        RpcResponse resp;
        resp.status = RpcStatus::kShutdown;
        resp.error_msg = "RPC client shut down";
        completions.push_back({std::move(qs.callback), std::move(resp)});
      }
      queued_sends_.pop_front();
    }
    queued_cancels_.clear();
    queued_disconnects_.clear();
    has_prune_ = false;
    prune_active_set_.clear();
  }
  UpdateActiveConnStat();
  DispatchCompletions(completions);
}

}  // namespace gemini_search
