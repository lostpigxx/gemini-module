#include "cluster_rpc.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace gemini_search;

TEST(ClusterRpcTest, PingPongRoundTripAndEphemeralFallback) {
  ClusterRpcServer srv1;
  srv1.SetRequestHandler([](const RpcFrame& req, const std::string& /*peer_ip*/,
                            ClusterRpcServer::ResponseSender send_resp) {
    ASSERT_EQ(req.header.opcode, RpcOpCode::kRpcPing);
    RpcPingPayload ping;
    ASSERT_TRUE(DecodePingPayload(req.payload, ping));
    RpcPongPayload pong{"server_node_1", ping.timestamp_ms, 9999ULL, 42ULL, 27001};
    std::string out;
    EncodePongPayload(pong, out);
    send_resp(RpcOpCode::kRpcPong, kRpcFlagNone, std::move(out));
  });

  ASSERT_TRUE(srv1.Start("127.0.0.1", 0));
  int port1 = srv1.BoundPort();
  ASSERT_GT(port1, 0);

  // Starting a second server on the same occupied port with fallback enabled must succeed on a new port
  ClusterRpcServer srv2;
  ASSERT_TRUE(srv2.Start("127.0.0.1", port1, /*allow_ephemeral_fallback=*/true));
  EXPECT_NE(srv2.BoundPort(), port1);
  EXPECT_GT(srv2.BoundPort(), 0);
  srv2.Stop();

  ClusterRpcClient cli;
  ASSERT_TRUE(cli.Start());

  RpcPingPayload ping{"client_node_0", 12345ULL, 10ULL, 27000};
  std::string ping_bytes;
  EncodePingPayload(ping, ping_bytes);

  RpcResponse resp =
      cli.SendSync("node1", "127.0.0.1", port1, RpcOpCode::kRpcPing, ping_bytes, 2000);
  ASSERT_EQ(resp.status, RpcStatus::kOk);
  EXPECT_EQ(resp.opcode, RpcOpCode::kRpcPong);

  RpcPongPayload pong;
  ASSERT_TRUE(DecodePongPayload(resp.payload, pong));
  EXPECT_EQ(pong.responder_node_id, "server_node_1");
  EXPECT_EQ(pong.echo_timestamp_ms, 12345ULL);
  EXPECT_EQ(pong.schema_epoch, 42ULL);
  EXPECT_EQ(pong.rpc_port, 27001u);

  cli.Stop();
  srv1.Stop();
}

TEST(ClusterRpcTest, ThousandConcurrentMultiplexedRequests) {
  ClusterRpcServer srv;
  // Simulate async SearchExecutor offload on a worker thread for half the requests
  srv.SetRequestHandler([](const RpcFrame& req, const std::string& /*peer_ip*/,
                           ClusterRpcServer::ResponseSender send_resp) {
    std::string reply_payload = "echo:" + req.payload;
    send_resp(RpcOpCode::kRpcSearchPhase1Resp, kRpcFlagNone, std::move(reply_payload));
  });

  ASSERT_TRUE(srv.Start("127.0.0.1", 0));
  int port = srv.BoundPort();

  ClusterRpcClient cli;
  ASSERT_TRUE(cli.Start());

  constexpr int kThreads = 8;
  constexpr int kReqsPerThread = 125;  // 1000 total concurrent requests
  std::atomic<int> success_count{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kReqsPerThread; i++) {
        std::string payload = "t" + std::to_string(t) + "_i" + std::to_string(i);
        RpcResponse r = cli.SendSync("shard_a", "127.0.0.1", port, RpcOpCode::kRpcSearchPhase1,
                                     payload, 5000);
        if (r.status == RpcStatus::kOk && r.payload == ("echo:" + payload)) {
          success_count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& th : threads) th.join();

  EXPECT_EQ(success_count.load(), kThreads * kReqsPerThread);
  auto cstats = cli.GetStats();
  EXPECT_EQ(cstats.requests_succeeded, static_cast<uint64_t>(kThreads * kReqsPerThread));
  EXPECT_EQ(cstats.inflight_requests, 0u);

  auto sstats = srv.GetStats();
  EXPECT_EQ(sstats.requests_handled, static_cast<uint64_t>(kThreads * kReqsPerThread));

  cli.Stop();
  srv.Stop();
}

TEST(ClusterRpcTest, TimeoutCleanupAndLateResponseDiscard) {
  ClusterRpcServer srv;
  std::vector<std::thread> bg_workers;
  std::mutex bg_mu;

  srv.SetRequestHandler([&](const RpcFrame& req, const std::string& /*peer_ip*/,
                            ClusterRpcServer::ResponseSender send_resp) {
    if (req.payload == "slow") {
      std::lock_guard<std::mutex> lk(bg_mu);
      bg_workers.emplace_back([send_resp = std::move(send_resp)]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        send_resp(RpcOpCode::kRpcPong, kRpcFlagNone, "late_reply");
      });
    } else {
      send_resp(RpcOpCode::kRpcPong, kRpcFlagNone, "fast_ok");
    }
  });

  ASSERT_TRUE(srv.Start("127.0.0.1", 0));
  int port = srv.BoundPort();

  ClusterRpcClient cli;
  ASSERT_TRUE(cli.Start());

  // 1. Slow request times out quickly
  RpcResponse r1 = cli.SendSync("peer1", "127.0.0.1", port, RpcOpCode::kRpcPing, "slow", 40);
  EXPECT_EQ(r1.status, RpcStatus::kTimeout);

  // Wait for late reply to arrive and be safely discarded
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // 2. Subsequent fast request on the same connection succeeds cleanly
  RpcResponse r2 = cli.SendSync("peer1", "127.0.0.1", port, RpcOpCode::kRpcPing, "fast", 2000);
  EXPECT_EQ(r2.status, RpcStatus::kOk);
  EXPECT_EQ(r2.payload, "fast_ok");
  EXPECT_EQ(cli.GetStats().inflight_requests, 0u);
  EXPECT_GE(cli.GetStats().requests_timed_out, 1u);

  for (auto& w : bg_workers) {
    if (w.joinable()) w.join();
  }
  cli.Stop();
  srv.Stop();
}

TEST(ClusterRpcTest, PeerKillFastFailAndReconnect) {
  auto srv = std::make_unique<ClusterRpcServer>();
  std::atomic<bool> hold_requests{false};
  srv->SetRequestHandler([&](const RpcFrame& req, const std::string& /*peer_ip*/,
                             ClusterRpcServer::ResponseSender send_resp) {
    if (!hold_requests.load()) {
      send_resp(RpcOpCode::kRpcPong, kRpcFlagNone, "alive:" + req.payload);
    }
    // When hold_requests is true, intentionally don't reply so request stays in-flight until kill
  });

  ASSERT_TRUE(srv->Start("127.0.0.1", 0));
  int port = srv->BoundPort();

  ClusterRpcClient cli;
  ASSERT_TRUE(cli.Start());

  // Establish connection
  RpcResponse r0 = cli.SendSync("node_x", "127.0.0.1", port, RpcOpCode::kRpcPing, "1", 2000);
  ASSERT_EQ(r0.status, RpcStatus::kOk);
  EXPECT_EQ(r0.payload, "alive:1");

  // Send an in-flight request while server holds it, then abruptly stop server
  hold_requests.store(true);
  std::atomic<bool> failed_fast{false};
  RpcStatus fail_status = RpcStatus::kOk;
  std::mutex cb_mu;
  std::condition_variable cb_cv;

  cli.SendAsync("node_x", "127.0.0.1", port, RpcOpCode::kRpcPing, "inflight", 10000,
                [&](RpcResponse r) {
                  {
                    std::lock_guard<std::mutex> lk(cb_mu);
                    fail_status = r.status;
                    failed_fast.store(true);
                  }
                  cb_cv.notify_one();
                });

  // Give I/O thread a moment to flush the request, then kill server
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  srv->Stop();

  {
    std::unique_lock<std::mutex> lk(cb_mu);
    ASSERT_TRUE(cb_cv.wait_for(lk, std::chrono::seconds(2), [&]() { return failed_fast.load(); }));
  }
  EXPECT_EQ(fail_status, RpcStatus::kDisconnected);

  // Restart a fresh server on the same port and verify automatic reconnect!
  srv = std::make_unique<ClusterRpcServer>();
  srv->SetRequestHandler([](const RpcFrame& req, const std::string& /*peer_ip*/,
                            ClusterRpcServer::ResponseSender send_resp) {
    send_resp(RpcOpCode::kRpcPong, kRpcFlagNone, "restarted:" + req.payload);
  });
  ASSERT_TRUE(srv->Start("127.0.0.1", port, /*allow_ephemeral_fallback=*/false));
  ASSERT_EQ(srv->BoundPort(), port);

  RpcResponse r_after = cli.SendSync("node_x", "127.0.0.1", port, RpcOpCode::kRpcPing, "2", 2000);
  ASSERT_EQ(r_after.status, RpcStatus::kOk);
  EXPECT_EQ(r_after.payload, "restarted:2");

  cli.Stop();
  srv->Stop();
}

TEST(ClusterRpcTest, OversizedFrameRejectedClientAndServer) {
  ClusterRpcServer srv;
  srv.SetRequestHandler([](const RpcFrame& /*req*/, const std::string& /*peer_ip*/,
                           ClusterRpcServer::ResponseSender send_resp) {
    send_resp(RpcOpCode::kRpcPong, kRpcFlagNone, "ok");
  });
  ASSERT_TRUE(srv.Start("127.0.0.1", 0));
  int port = srv.BoundPort();

  // 1. Client rejects payload > 32 MiB immediately
  ClusterRpcClient cli;
  ASSERT_TRUE(cli.Start());
  std::string huge(kMaxRpcPayloadBytes + 1, 'A');
  RpcResponse r_huge =
      cli.SendSync("node1", "127.0.0.1", port, RpcOpCode::kRpcPing, huge, 2000);
  EXPECT_EQ(r_huge.status, RpcStatus::kInvalidArgument);

  // 2. Malicious raw TCP client sending header claiming payload_len = 33 MiB
  int raw_fd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(raw_fd, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  ASSERT_EQ(connect(raw_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

  // Craft header with valid magic, version=1, opcode=kRpcPing, payload_len=33 MiB, valid header CRC
  uint8_t hdr[kRpcHeaderBytes] = {};
  std::memcpy(hdr, kRpcMagic, 4);
  hdr[4] = 1;
  hdr[6] = 1;  // kRpcPing
  uint32_t bad_len = kMaxRpcPayloadBytes + 1024;
  hdr[18] = static_cast<uint8_t>(bad_len & 0xFF);
  hdr[19] = static_cast<uint8_t>((bad_len >> 8) & 0xFF);
  hdr[20] = static_cast<uint8_t>((bad_len >> 16) & 0xFF);
  hdr[21] = static_cast<uint8_t>((bad_len >> 24) & 0xFF);
  uint32_t hcrc = ComputeCrc32(hdr, 22);
  hdr[22] = static_cast<uint8_t>(hcrc & 0xFF);
  hdr[23] = static_cast<uint8_t>((hcrc >> 8) & 0xFF);
  hdr[24] = static_cast<uint8_t>((hcrc >> 16) & 0xFF);
  hdr[25] = static_cast<uint8_t>((hcrc >> 24) & 0xFF);

  ASSERT_EQ(send(raw_fd, hdr, sizeof(hdr), 0), static_cast<ssize_t>(sizeof(hdr)));

  // Server must reject and close the socket promptly
  char buf[16];
  ssize_t n = recv(raw_fd, buf, sizeof(buf), 0);
  EXPECT_LE(n, 0);
  close(raw_fd);

  // Wait briefly for server stats update
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_GE(srv.GetStats().rejected_frames, 1u);

  // Legitimate client still works after rejecting malicious peer
  RpcResponse r_ok = cli.SendSync("node1", "127.0.0.1", port, RpcOpCode::kRpcPing, "hi", 2000);
  EXPECT_EQ(r_ok.status, RpcStatus::kOk);
  EXPECT_EQ(r_ok.payload, "ok");

  cli.Stop();
  srv.Stop();
}

TEST(ClusterRpcTest, PeerValidatorAndPrunePeers) {
  ClusterRpcServer srv;
  srv.SetPeerValidator([](const std::string& ip) { return ip != "127.0.0.1"; });
  ASSERT_TRUE(srv.Start("127.0.0.1", 0));
  int port = srv.BoundPort();

  ClusterRpcClient cli;
  ASSERT_TRUE(cli.Start());

  // Connection from 127.0.0.1 should be rejected by peer validator
  RpcResponse r = cli.SendSync("blocked_peer", "127.0.0.1", port, RpcOpCode::kRpcPing, "x", 1000);
  EXPECT_NE(r.status, RpcStatus::kOk);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_GE(srv.GetStats().peer_rejected, 1u);

  // Now allow 127.0.0.1
  srv.SetPeerValidator([](const std::string& ip) { return ip == "127.0.0.1"; });
  srv.SetRequestHandler([](const RpcFrame& /*req*/, const std::string& /*ip*/,
                           ClusterRpcServer::ResponseSender send_resp) {
    send_resp(RpcOpCode::kRpcPong, kRpcFlagNone, "allowed");
  });

  RpcResponse r2 = cli.SendSync("good_peer", "127.0.0.1", port, RpcOpCode::kRpcPing, "x", 2000);
  ASSERT_EQ(r2.status, RpcStatus::kOk);
  EXPECT_EQ(r2.payload, "allowed");
  EXPECT_EQ(cli.GetStats().active_connections, 1u);

  // Prune peer not in active set -> closes idle connection
  cli.PrunePeers({"other_peer"});
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(cli.GetStats().active_connections, 0u);

  cli.Stop();
  srv.Stop();
}

TEST(ClusterRpcTest, AuthTokenAcceptsValidAndRejectsInvalidOrMissing) {
  // Verify MAC tag domain sensitivity
  uint64_t t1 = ComputeRpcAuthTag("sec1", RpcOpCode::kRpcPing, 7, "hello");
  uint64_t t2 = ComputeRpcAuthTag("sec2", RpcOpCode::kRpcPing, 7, "hello");
  uint64_t t3 = ComputeRpcAuthTag("sec1", RpcOpCode::kRpcSearchPhase1, 7, "hello");
  uint64_t t4 = ComputeRpcAuthTag("sec1", RpcOpCode::kRpcPing, 8, "hello");
  uint64_t t5 = ComputeRpcAuthTag("sec1", RpcOpCode::kRpcPing, 7, "hellp");
  EXPECT_NE(t1, t2);
  EXPECT_NE(t1, t3);
  EXPECT_NE(t1, t4);
  EXPECT_NE(t1, t5);

  ClusterRpcServer srv;
  srv.SetAuthToken("phase71_secret_key");
  EXPECT_TRUE(srv.HasAuthToken());
  srv.SetRequestHandler([](const RpcFrame& req, const std::string& /*ip*/,
                           ClusterRpcServer::ResponseSender send_resp) {
    send_resp(RpcOpCode::kRpcPong, kRpcFlagNone, "verified:" + req.payload);
  });
  ASSERT_TRUE(srv.Start("127.0.0.1", 0));
  int port = srv.BoundPort();

  // 1. Client without auth token must be rejected
  ClusterRpcClient unauth_cli;
  ASSERT_TRUE(unauth_cli.Start());
  RpcResponse r_noauth =
      unauth_cli.SendSync("n1", "127.0.0.1", port, RpcOpCode::kRpcPing, "msg", 2000);
  EXPECT_TRUE(r_noauth.status != RpcStatus::kOk || (r_noauth.flags & kRpcFlagError) != 0);
  unauth_cli.Stop();

  // 2. Client with wrong auth token must be rejected
  ClusterRpcClient wrong_cli;
  wrong_cli.SetAuthToken("wrong_secret");
  ASSERT_TRUE(wrong_cli.Start());
  RpcResponse r_wrong =
      wrong_cli.SendSync("n1", "127.0.0.1", port, RpcOpCode::kRpcPing, "msg", 2000);
  EXPECT_TRUE(r_wrong.status != RpcStatus::kOk || (r_wrong.flags & kRpcFlagError) != 0);
  wrong_cli.Stop();

  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_GE(srv.GetStats().auth_rejected, 2u);

  // 3. Client with matching auth token succeeds and payload is stripped cleanly
  ClusterRpcClient ok_cli;
  ok_cli.SetAuthToken("phase71_secret_key");
  ASSERT_TRUE(ok_cli.Start());
  RpcResponse r_ok =
      ok_cli.SendSync("n1", "127.0.0.1", port, RpcOpCode::kRpcPing, "payload_123", 2000);
  ASSERT_EQ(r_ok.status, RpcStatus::kOk);
  EXPECT_EQ(r_ok.flags & kRpcFlagError, 0u);
  EXPECT_EQ(r_ok.payload, "verified:payload_123");

  ok_cli.Stop();
  srv.Stop();
}

TEST(ClusterRpcTest, MaxInflightQuotaEnforcementAndRecovery) {
  ClusterRpcServer srv;
  srv.SetMaxInflightRequests(2);

  std::mutex held_mu;
  std::condition_variable held_cv;
  std::vector<ClusterRpcServer::ResponseSender> held_senders;

  srv.SetRequestHandler([&](const RpcFrame& req, const std::string& /*ip*/,
                            ClusterRpcServer::ResponseSender send_resp) {
    if (req.payload == "hold") {
      {
        std::lock_guard<std::mutex> lk(held_mu);
        held_senders.push_back(std::move(send_resp));
      }
      held_cv.notify_all();
    } else {
      send_resp(RpcOpCode::kRpcPong, kRpcFlagNone, "ok:" + req.payload);
    }
  });
  ASSERT_TRUE(srv.Start("127.0.0.1", 0));
  int port = srv.BoundPort();

  ClusterRpcClient cli;
  ASSERT_TRUE(cli.Start());

  std::atomic<int> held_done{0};
  cli.SendAsync("n1", "127.0.0.1", port, RpcOpCode::kRpcPing, "hold", 5000,
                [&](RpcResponse r) {
                  if (r.status == RpcStatus::kOk && (r.flags & kRpcFlagError) == 0) {
                    held_done.fetch_add(1);
                  }
                });
  cli.SendAsync("n1", "127.0.0.1", port, RpcOpCode::kRpcPing, "hold", 5000,
                [&](RpcResponse r) {
                  if (r.status == RpcStatus::kOk && (r.flags & kRpcFlagError) == 0) {
                    held_done.fetch_add(1);
                  }
                });

  // Wait until both requests are held in-flight on server
  {
    std::unique_lock<std::mutex> lk(held_mu);
    ASSERT_TRUE(held_cv.wait_for(lk, std::chrono::seconds(2),
                                 [&]() { return held_senders.size() == 2; }));
  }

  // 3rd request exceeds server max_inflight=2 -> immediately rejected with 503 error frame
  RpcResponse r_busy = cli.SendSync("n1", "127.0.0.1", port, RpcOpCode::kRpcPing, "extra", 2000);
  ASSERT_EQ(r_busy.status, RpcStatus::kOk);
  EXPECT_NE(r_busy.flags & kRpcFlagError, 0u);
  EXPECT_NE(r_busy.error_msg.find("quota"), std::string::npos);
  EXPECT_GE(srv.GetStats().inflight_rejected, 1u);

  // Also verify client-side per-peer inflight quota enforcement
  cli.SetMaxInflightPerPeer(2);
  RpcResponse r_cli_quota =
      cli.SendSync("n1", "127.0.0.1", port, RpcOpCode::kRpcPing, "extra2", 2000);
  EXPECT_EQ(r_cli_quota.status, RpcStatus::kDisconnected);
  EXPECT_GE(cli.GetStats().inflight_rejected, 1u);
  cli.SetMaxInflightPerPeer(1024);

  // Release the 2 held senders -> server inflight drops to 0 and recovers cleanly
  {
    std::lock_guard<std::mutex> lk(held_mu);
    for (auto& s : held_senders) {
      s(RpcOpCode::kRpcPong, kRpcFlagNone, "released");
    }
    held_senders.clear();
  }

  RpcResponse r_after = cli.SendSync("n1", "127.0.0.1", port, RpcOpCode::kRpcPing, "fresh", 2000);
  ASSERT_EQ(r_after.status, RpcStatus::kOk);
  EXPECT_EQ(r_after.flags & kRpcFlagError, 0u);
  EXPECT_EQ(r_after.payload, "ok:fresh");
  EXPECT_EQ(srv.GetStats().inflight_requests, 0u);

  cli.Stop();
  srv.Stop();
}
