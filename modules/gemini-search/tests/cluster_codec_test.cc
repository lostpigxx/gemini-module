#include "cluster_codec.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace gemini_search;

TEST(ClusterCodecTest, Crc32KnownVectors) {
  EXPECT_EQ(ComputeCrc32(""), 0u);
  EXPECT_EQ(ComputeCrc32("123456789"), 0xCBF43926u);
  EXPECT_NE(ComputeCrc32("hello"), ComputeCrc32("hellp"));
}

TEST(ClusterCodecTest, BinaryWriterReaderPrimitives) {
  BinaryWriter w;
  w.WriteU8(0xAB);
  w.WriteU16(0x1234);
  w.WriteU32(0xDEADBEEFu);
  w.WriteU64(0x0123456789ABCDEFull);
  w.WriteI64(-9876543210123LL);
  w.WriteDouble(3.141592653589793);
  w.WriteBool(true);
  w.WriteBool(false);
  w.WriteVarUint(0);
  w.WriteVarUint(127);
  w.WriteVarUint(128);
  w.WriteVarUint(std::numeric_limits<uint64_t>::max());
  w.WriteVarInt(0);
  w.WriteVarInt(-1);
  w.WriteVarInt(1);
  w.WriteVarInt(std::numeric_limits<int64_t>::min());
  w.WriteVarInt(std::numeric_limits<int64_t>::max());
  w.WriteString("gemini-search-cluster");
  w.WriteString("");
  std::vector<std::string> vec = {"alpha", "", "gamma", std::string("a\0b", 3)};
  w.WriteStringVec(vec);

  BinaryReader r(w.Buffer());
  uint8_t u8 = 0;
  uint16_t u16 = 0;
  uint32_t u32 = 0;
  uint64_t u64 = 0;
  int64_t i64 = 0;
  double d = 0.0;
  bool b1 = false, b2 = true;
  uint64_t vu0 = 1, vu1 = 0, vu2 = 0, vumax = 0;
  int64_t vi0 = 1, vim1 = 0, vi1 = 0, vimin = 0, vimax = 0;
  std::string s1, s2;
  std::vector<std::string> vec_out;

  ASSERT_TRUE(r.ReadU8(u8));
  ASSERT_TRUE(r.ReadU16(u16));
  ASSERT_TRUE(r.ReadU32(u32));
  ASSERT_TRUE(r.ReadU64(u64));
  ASSERT_TRUE(r.ReadI64(i64));
  ASSERT_TRUE(r.ReadDouble(d));
  ASSERT_TRUE(r.ReadBool(b1));
  ASSERT_TRUE(r.ReadBool(b2));
  ASSERT_TRUE(r.ReadVarUint(vu0));
  ASSERT_TRUE(r.ReadVarUint(vu1));
  ASSERT_TRUE(r.ReadVarUint(vu2));
  ASSERT_TRUE(r.ReadVarUint(vumax));
  ASSERT_TRUE(r.ReadVarInt(vi0));
  ASSERT_TRUE(r.ReadVarInt(vim1));
  ASSERT_TRUE(r.ReadVarInt(vi1));
  ASSERT_TRUE(r.ReadVarInt(vimin));
  ASSERT_TRUE(r.ReadVarInt(vimax));
  ASSERT_TRUE(r.ReadString(s1));
  ASSERT_TRUE(r.ReadString(s2));
  ASSERT_TRUE(r.ReadStringVec(vec_out));
  EXPECT_TRUE(r.AtEnd());

  EXPECT_EQ(u8, 0xAB);
  EXPECT_EQ(u16, 0x1234);
  EXPECT_EQ(u32, 0xDEADBEEFu);
  EXPECT_EQ(u64, 0x0123456789ABCDEFull);
  EXPECT_EQ(i64, -9876543210123LL);
  EXPECT_DOUBLE_EQ(d, 3.141592653589793);
  EXPECT_TRUE(b1);
  EXPECT_FALSE(b2);
  EXPECT_EQ(vu0, 0u);
  EXPECT_EQ(vu1, 127u);
  EXPECT_EQ(vu2, 128u);
  EXPECT_EQ(vumax, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(vi0, 0);
  EXPECT_EQ(vim1, -1);
  EXPECT_EQ(vi1, 1);
  EXPECT_EQ(vimin, std::numeric_limits<int64_t>::min());
  EXPECT_EQ(vimax, std::numeric_limits<int64_t>::max());
  EXPECT_EQ(s1, "gemini-search-cluster");
  EXPECT_EQ(s2, "");
  EXPECT_EQ(vec_out, vec);
}

TEST(ClusterCodecTest, ReaderRejectsMalformedBounds) {
  // Non-finite double rejected by default
  BinaryWriter w;
  w.WriteDouble(std::numeric_limits<double>::quiet_NaN());
  w.WriteDouble(std::numeric_limits<double>::infinity());
  BinaryReader r(w.Buffer());
  double d = 0.0;
  EXPECT_FALSE(r.ReadDouble(d));
  EXPECT_FALSE(r.ReadDouble(d));

  // Invalid bool value (2)
  uint8_t bad_bool[1] = {2};
  BinaryReader rb(bad_bool, 1);
  bool bv = false;
  EXPECT_FALSE(rb.ReadBool(bv));

  // Overflow varint (10th byte > 1)
  uint8_t overflow_vi[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02};
  BinaryReader rvi(overflow_vi, sizeof(overflow_vi));
  uint64_t vu = 0;
  EXPECT_FALSE(rvi.ReadVarUint(vu));

  // Huge claimed string length with small buffer (must reject without OOM)
  BinaryWriter ws;
  ws.WriteVarUint(1000000000ULL);
  BinaryReader rs(ws.Buffer());
  std::string out_s;
  EXPECT_FALSE(rs.ReadString(out_s));

  // Huge claimed vector count with tiny buffer
  BinaryWriter wv;
  wv.WriteVarUint(999999ULL);
  BinaryReader rv(wv.Buffer());
  std::vector<std::string> out_v;
  EXPECT_FALSE(rv.ReadStringVec(out_v));
}

TEST(ClusterCodecTest, AllOpCodesFrameRoundTrip) {
  const std::vector<RpcOpCode> all_ops = {
      RpcOpCode::kRpcPing,           RpcOpCode::kRpcPong,
      RpcOpCode::kRpcDdlCreate,      RpcOpCode::kRpcDdlDrop,
      RpcOpCode::kRpcDdlAlter,       RpcOpCode::kRpcDdlAlias,
      RpcOpCode::kRpcDdlSyn,         RpcOpCode::kRpcDdlDict,
      RpcOpCode::kRpcDdlResp,        RpcOpCode::kRpcSchemaSyncReq,
      RpcOpCode::kRpcSchemaSyncResp, RpcOpCode::kRpcSearchPhase1,
      RpcOpCode::kRpcSearchPhase1Resp, RpcOpCode::kRpcFetchDocs,
      RpcOpCode::kRpcFetchDocsResp,  RpcOpCode::kRpcDfsTermStats,
      RpcOpCode::kRpcDfsTermStatsResp, RpcOpCode::kRpcHybridPhase1,
      RpcOpCode::kRpcHybridPhase1Resp, RpcOpCode::kRpcAggMap,
      RpcOpCode::kRpcAggMapResp,     RpcOpCode::kRpcInfoStats,
      RpcOpCode::kRpcInfoStatsResp,  RpcOpCode::kRpcTagVals,
      RpcOpCode::kRpcTagValsResp,    RpcOpCode::kRpcSpellCheck,
      RpcOpCode::kRpcSpellCheckResp, RpcOpCode::kRpcSuggest,
      RpcOpCode::kRpcSuggestResp,    RpcOpCode::kRpcCursor,
      RpcOpCode::kRpcCursorResp,
  };

  uint64_t req_id = 100;
  for (RpcOpCode op : all_ops) {
    std::string payload = "payload_for_" + std::to_string(static_cast<uint16_t>(op));
    std::string encoded;
    ASSERT_TRUE(EncodeRpcFrame(op, kRpcFlagResponse, req_id, payload, encoded));

    RpcFrame decoded;
    size_t consumed = 0;
    EXPECT_EQ(DecodeRpcFrame(reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size(),
                             decoded, consumed),
              FrameDecodeStatus::kOk);
    EXPECT_EQ(consumed, encoded.size());
    EXPECT_EQ(decoded.header.version, kRpcProtocolVersion);
    EXPECT_EQ(decoded.header.opcode, op);
    EXPECT_EQ(decoded.header.flags, kRpcFlagResponse);
    EXPECT_EQ(decoded.header.request_id, req_id);
    EXPECT_EQ(decoded.payload, payload);
    req_id++;
  }
}

TEST(ClusterCodecTest, FrameValidationAndStreaming) {
  std::string encoded;
  ASSERT_TRUE(EncodeRpcFrame(RpcOpCode::kRpcSearchPhase1, kRpcFlagNone, 42, "hello world", encoded));

  // Partial streaming byte-by-byte returns kIncomplete until final byte
  RpcFrame frame;
  size_t consumed = 0;
  for (size_t partial = 0; partial < encoded.size(); partial++) {
    EXPECT_EQ(DecodeRpcFrame(reinterpret_cast<const uint8_t*>(encoded.data()), partial, frame,
                             consumed),
              FrameDecodeStatus::kIncomplete);
    EXPECT_EQ(consumed, 0u);
  }
  EXPECT_EQ(DecodeRpcFrame(reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size(), frame,
                           consumed),
            FrameDecodeStatus::kOk);
  EXPECT_EQ(consumed, encoded.size());

  // Bad magic
  std::string bad_magic = encoded;
  bad_magic[0] = 'X';
  EXPECT_EQ(DecodeRpcFrame(reinterpret_cast<const uint8_t*>(bad_magic.data()), bad_magic.size(),
                           frame, consumed),
            FrameDecodeStatus::kInvalidMagic);

  // Header CRC mismatch
  std::string bad_hcrc = encoded;
  bad_hcrc[10] ^= 0x01;
  EXPECT_EQ(DecodeRpcFrame(reinterpret_cast<const uint8_t*>(bad_hcrc.data()), bad_hcrc.size(),
                           frame, consumed),
            FrameDecodeStatus::kHeaderCrcMismatch);

  // Payload CRC mismatch
  std::string bad_pcrc = encoded;
  bad_pcrc.back() ^= 0x80;
  EXPECT_EQ(DecodeRpcFrame(reinterpret_cast<const uint8_t*>(bad_pcrc.data()), bad_pcrc.size(),
                           frame, consumed),
            FrameDecodeStatus::kPayloadCrcMismatch);

  // Unsupported version (with valid header CRC)
  std::string bad_ver = encoded;
  bad_ver[4] = 99;
  bad_ver[5] = 0;
  uint32_t new_hcrc = ComputeCrc32(reinterpret_cast<const uint8_t*>(bad_ver.data()), 22);
  bad_ver[22] = static_cast<char>(new_hcrc & 0xFF);
  bad_ver[23] = static_cast<char>((new_hcrc >> 8) & 0xFF);
  bad_ver[24] = static_cast<char>((new_hcrc >> 16) & 0xFF);
  bad_ver[25] = static_cast<char>((new_hcrc >> 24) & 0xFF);
  EXPECT_EQ(DecodeRpcFrame(reinterpret_cast<const uint8_t*>(bad_ver.data()), bad_ver.size(), frame,
                           consumed),
            FrameDecodeStatus::kUnsupportedVersion);

  // Oversized payload_len (> 32 MiB) in header with valid header CRC
  std::string oversized = encoded;
  uint32_t huge_len = kMaxRpcPayloadBytes + 1;
  oversized[18] = static_cast<char>(huge_len & 0xFF);
  oversized[19] = static_cast<char>((huge_len >> 8) & 0xFF);
  oversized[20] = static_cast<char>((huge_len >> 16) & 0xFF);
  oversized[21] = static_cast<char>((huge_len >> 24) & 0xFF);
  uint32_t ov_hcrc = ComputeCrc32(reinterpret_cast<const uint8_t*>(oversized.data()), 22);
  oversized[22] = static_cast<char>(ov_hcrc & 0xFF);
  oversized[23] = static_cast<char>((ov_hcrc >> 8) & 0xFF);
  oversized[24] = static_cast<char>((ov_hcrc >> 16) & 0xFF);
  oversized[25] = static_cast<char>((ov_hcrc >> 24) & 0xFF);
  RpcFrameHeader hdr;
  EXPECT_EQ(DecodeRpcHeader(reinterpret_cast<const uint8_t*>(oversized.data()), oversized.size(),
                            hdr),
            FrameDecodeStatus::kPayloadTooLarge);
}

TEST(ClusterCodecTest, ControlPayloadsRoundTrip) {
  RpcErrorPayload err_in{503, "shard unavailable"};
  std::string err_buf;
  EncodeErrorPayload(err_in, err_buf);
  RpcErrorPayload err_out;
  ASSERT_TRUE(DecodeErrorPayload(err_buf, err_out));
  EXPECT_EQ(err_out.code, err_in.code);
  EXPECT_EQ(err_out.message, err_in.message);

  RpcPingPayload ping_in{"0123456789abcdef0123456789abcdef01234567", 123456789ULL, 77ULL, 26379};
  std::string ping_buf;
  EncodePingPayload(ping_in, ping_buf);
  RpcPingPayload ping_out;
  ASSERT_TRUE(DecodePingPayload(ping_buf, ping_out));
  EXPECT_EQ(ping_out.sender_node_id, ping_in.sender_node_id);
  EXPECT_EQ(ping_out.timestamp_ms, ping_in.timestamp_ms);
  EXPECT_EQ(ping_out.schema_epoch, ping_in.schema_epoch);
  EXPECT_EQ(ping_out.rpc_port, ping_in.rpc_port);

  RpcPongPayload pong_in{"fedcba9876543210fedcba9876543210fedcba98", 123456789ULL, 123456795ULL,
                         88ULL, 26380};
  std::string pong_buf;
  EncodePongPayload(pong_in, pong_buf);
  RpcPongPayload pong_out;
  ASSERT_TRUE(DecodePongPayload(pong_buf, pong_out));
  EXPECT_EQ(pong_out.responder_node_id, pong_in.responder_node_id);
  EXPECT_EQ(pong_out.echo_timestamp_ms, pong_in.echo_timestamp_ms);
  EXPECT_EQ(pong_out.responder_time_ms, pong_in.responder_time_ms);
  EXPECT_EQ(pong_out.schema_epoch, pong_in.schema_epoch);
  EXPECT_EQ(pong_out.rpc_port, pong_in.rpc_port);

  ClusterBusHello hello_in{kClusterBusHelloVersion, 27001, 9999ULL, kRpcProtocolVersion, 1};
  std::string hello_buf;
  EncodeClusterBusHello(hello_in, hello_buf);
  ASSERT_EQ(hello_buf.size(), kClusterBusHelloBytes);
  ClusterBusHello hello_out;
  ASSERT_TRUE(DecodeClusterBusHello(reinterpret_cast<const uint8_t*>(hello_buf.data()),
                                    hello_buf.size(), hello_out));
  EXPECT_EQ(hello_out.rpc_port, 27001u);
  EXPECT_EQ(hello_out.schema_epoch, 9999ULL);
  EXPECT_EQ(hello_out.protocol_ver, kRpcProtocolVersion);
  EXPECT_EQ(hello_out.flags, 1u);

  // Corrupt ClusterBusHello CRC
  hello_buf[10] ^= 0x04;
  EXPECT_FALSE(DecodeClusterBusHello(reinterpret_cast<const uint8_t*>(hello_buf.data()),
                                     hello_buf.size(), hello_out));
}

TEST(ClusterCodecTest, FuzzTruncationAndBitFlips10000Iterations) {
  RpcPingPayload ping{"0123456789abcdef0123456789abcdef01234567", 424242, 123, 27000};
  std::string ping_payload;
  EncodePingPayload(ping, ping_payload);
  std::string valid_frame;
  ASSERT_TRUE(EncodeRpcFrame(RpcOpCode::kRpcPing, kRpcFlagNone, 999, ping_payload, valid_frame));

  ClusterBusHello hello{kClusterBusHelloVersion, 27000, 123, kRpcProtocolVersion, 1};
  std::string valid_hello;
  EncodeClusterBusHello(hello, valid_hello);

  std::mt19937_64 rng(0xCAFEBABE12345678ULL);

  for (int iter = 0; iter < 10000; iter++) {
    int mode = iter % 4;
    if (mode == 0) {
      // Random truncation of valid frame
      size_t cut = rng() % valid_frame.size();
      RpcFrame out;
      size_t consumed = 0;
      FrameDecodeStatus st = DecodeRpcFrame(
          reinterpret_cast<const uint8_t*>(valid_frame.data()), cut, out, consumed);
      EXPECT_EQ(st, FrameDecodeStatus::kIncomplete);
    } else if (mode == 1) {
      // Bit flip in valid frame must NEVER decode as kOk
      std::string mutated = valid_frame;
      size_t pos = rng() % mutated.size();
      uint8_t mask = static_cast<uint8_t>(1u << (rng() % 8));
      mutated[pos] = static_cast<char>(static_cast<uint8_t>(mutated[pos]) ^ mask);
      RpcFrame out;
      size_t consumed = 0;
      FrameDecodeStatus st = DecodeRpcFrame(
          reinterpret_cast<const uint8_t*>(mutated.data()), mutated.size(), out, consumed);
      EXPECT_NE(st, FrameDecodeStatus::kOk);
    } else if (mode == 2) {
      // Completely random bytes fed to all decoders
      size_t rlen = rng() % 128;
      std::string rand_buf(rlen, '\0');
      for (size_t i = 0; i < rlen; i++) rand_buf[i] = static_cast<char>(rng() & 0xFF);
      RpcFrame out;
      size_t consumed = 0;
      (void)DecodeRpcFrame(reinterpret_cast<const uint8_t*>(rand_buf.data()), rand_buf.size(), out,
                           consumed);
      RpcPingPayload po;
      (void)DecodePingPayload(rand_buf, po);
      RpcPongPayload pgo;
      (void)DecodePongPayload(rand_buf, pgo);
      RpcErrorPayload eo;
      (void)DecodeErrorPayload(rand_buf, eo);
      ClusterBusHello ho;
      (void)DecodeClusterBusHello(reinterpret_cast<const uint8_t*>(rand_buf.data()),
                                  rand_buf.size(), ho);
    } else {
      // Bit flip or truncation on ClusterBusHello
      std::string mutated = valid_hello;
      if (rng() % 2 == 0) {
        mutated.resize(rng() % valid_hello.size());
      } else {
        size_t pos = rng() % mutated.size();
        mutated[pos] = static_cast<char>(static_cast<uint8_t>(mutated[pos]) ^ (1u << (rng() % 8)));
      }
      ClusterBusHello ho;
      EXPECT_FALSE(DecodeClusterBusHello(reinterpret_cast<const uint8_t*>(mutated.data()),
                                         mutated.size(), ho));
    }
  }
}
