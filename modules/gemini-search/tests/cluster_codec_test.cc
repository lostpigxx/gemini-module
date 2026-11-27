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

TEST(ClusterCodecTest, DdlAndSchemaSyncPayloadsRoundTrip) {
  RpcDdlRequest ddl_req;
  ddl_req.schema_epoch = 4242;
  ddl_req.coordinator_node_id = "0123456789abcdef0123456789abcdef01234567";
  ddl_req.cmd_name = "FT.CREATE";
  ddl_req.args = {"idx1", "ON", "HASH", "PREFIX", "1", "doc:", "SCHEMA", "title", "TEXT"};
  ddl_req.flags = kDdlFlagRollback;

  std::string ddl_req_buf;
  EncodeDdlRequest(ddl_req, ddl_req_buf);
  RpcDdlRequest ddl_req_out;
  ASSERT_TRUE(DecodeDdlRequest(ddl_req_buf, ddl_req_out));
  EXPECT_EQ(ddl_req_out.schema_epoch, ddl_req.schema_epoch);
  EXPECT_EQ(ddl_req_out.coordinator_node_id, ddl_req.coordinator_node_id);
  EXPECT_EQ(ddl_req_out.cmd_name, ddl_req.cmd_name);
  EXPECT_EQ(ddl_req_out.args, ddl_req.args);
  EXPECT_EQ(ddl_req_out.flags, ddl_req.flags);

  RpcDdlResponse ddl_resp{true, 3, 4242, ""};
  std::string ddl_resp_buf;
  EncodeDdlResponse(ddl_resp, ddl_resp_buf);
  RpcDdlResponse ddl_resp_out;
  ASSERT_TRUE(DecodeDdlResponse(ddl_resp_buf, ddl_resp_out));
  EXPECT_TRUE(ddl_resp_out.ok);
  EXPECT_EQ(ddl_resp_out.int_reply, 3);
  EXPECT_EQ(ddl_resp_out.applied_epoch, 4242u);
  EXPECT_TRUE(ddl_resp_out.error_msg.empty());

  // Full SchemaSync round-trip including all field types & vector HNSW BQ+RERANK
  RpcSchemaSyncRequest sync_req{"fedcba9876543210fedcba9876543210fedcba98", 10};
  std::string sync_req_buf;
  EncodeSchemaSyncRequest(sync_req, sync_req_buf);
  RpcSchemaSyncRequest sync_req_out;
  ASSERT_TRUE(DecodeSchemaSyncRequest(sync_req_buf, sync_req_out));
  EXPECT_EQ(sync_req_out.requester_node_id, sync_req.requester_node_id);
  EXPECT_EQ(sync_req_out.local_schema_epoch, 10u);

  RpcSchemaSyncResponse sync_resp;
  sync_resp.schema_epoch = 99;
  SchemaIndexSnapshot snap;
  snap.spec.name = "full_cluster_idx";
  snap.spec.prefixes = {"doc:", "item:"};
  snap.spec.source_type = IndexSourceType::kJson;
  snap.spec.language = "chinese";
  snap.spec.language_field = "lang";
  snap.spec.custom_dict = "cjk_dict";
  snap.spec.has_custom_stopwords = true;
  snap.spec.custom_stopwords = {"the", "a"};
  snap.spec.nofreqs = true;
  snap.spec.nooffsets = false;
  snap.spec.nohl = true;
  snap.spec.temporary_ttl = 3600;
  snap.spec.filter_expr = "@price > 10";
  snap.spec.skip_initial_scan = false;

  FieldSpec f_txt;
  f_txt.name = "title";
  f_txt.type = FieldType::kText;
  f_txt.json_path = "$.title";
  f_txt.sortable = true;
  f_txt.weight = 2.5;
  f_txt.nostem = true;
  f_txt.phonetic = "dm:en";
  f_txt.index_missing = true;
  snap.spec.fields.push_back(f_txt);

  FieldSpec f_vec;
  f_vec.name = "emb";
  f_vec.type = FieldType::kVector;
  f_vec.vector_params.algorithm = VectorAlgorithm::kHnsw;
  f_vec.vector_params.dim = 128;
  f_vec.vector_params.metric = DistanceMetric::kCosine;
  f_vec.vector_params.m = 24;
  f_vec.vector_params.ef_construction = 128;
  f_vec.vector_params.data_type = VectorDataType::kFloat16;
  f_vec.vector_params.quantization = VectorQuantization::kBq;
  f_vec.vector_params.rerank = VectorRerankType::kFp16;
  f_vec.vector_params.bq_oversample = 6;
  snap.spec.fields.push_back(f_vec);

  FieldSpec f_tag;
  f_tag.name = "tags";
  f_tag.type = FieldType::kTag;
  f_tag.tag_separator = '|';
  f_tag.case_sensitive = true;
  f_tag.index_empty = true;
  snap.spec.fields.push_back(f_tag);

  snap.synonyms = {{"g1", {"fast", "quick"}}, {"g2", {"big", "large"}}};
  sync_resp.indices.push_back(std::move(snap));
  sync_resp.aliases = {{"live_alias", "full_cluster_idx"}};
  sync_resp.term_dicts = {{"cjk_dict", {"redis", "cluster"}}};

  std::string sync_resp_buf;
  EncodeSchemaSyncResponse(sync_resp, sync_resp_buf);
  RpcSchemaSyncResponse sync_resp_out;
  ASSERT_TRUE(DecodeSchemaSyncResponse(sync_resp_buf, sync_resp_out));

  EXPECT_EQ(sync_resp_out.schema_epoch, 99u);
  ASSERT_EQ(sync_resp_out.indices.size(), 1u);
  const auto& os = sync_resp_out.indices[0].spec;
  EXPECT_EQ(os.name, "full_cluster_idx");
  EXPECT_EQ(os.prefixes, (std::vector<std::string>{"doc:", "item:"}));
  EXPECT_EQ(os.source_type, IndexSourceType::kJson);
  EXPECT_EQ(os.language, "chinese");
  EXPECT_EQ(os.language_field, "lang");
  EXPECT_EQ(os.custom_dict, "cjk_dict");
  EXPECT_TRUE(os.has_custom_stopwords);
  EXPECT_EQ(os.custom_stopwords, (std::vector<std::string>{"the", "a"}));
  EXPECT_EQ(os.temporary_ttl, 3600);
  EXPECT_EQ(os.filter_expr, "@price > 10");
  ASSERT_EQ(os.fields.size(), 3u);
  EXPECT_EQ(os.fields[0].json_path, "$.title");
  EXPECT_DOUBLE_EQ(os.fields[0].weight, 2.5);
  EXPECT_EQ(os.fields[1].vector_params.algorithm, VectorAlgorithm::kHnsw);
  EXPECT_EQ(os.fields[1].vector_params.dim, 128u);
  EXPECT_EQ(os.fields[1].vector_params.quantization, VectorQuantization::kBq);
  EXPECT_EQ(os.fields[1].vector_params.rerank, VectorRerankType::kFp16);
  EXPECT_EQ(os.fields[1].vector_params.bq_oversample, 6u);
  EXPECT_EQ(os.fields[2].tag_separator, '|');
  EXPECT_TRUE(os.fields[2].case_sensitive);
  EXPECT_EQ(sync_resp_out.indices[0].synonyms.size(), 2u);
  EXPECT_EQ(sync_resp_out.aliases.size(), 1u);
  EXPECT_EQ(sync_resp_out.term_dicts.size(), 1u);

  // Truncation fuzzing on DDL & SchemaSync decoders
  for (size_t cut = 0; cut < ddl_req_buf.size(); cut++) {
    RpcDdlRequest tmp;
    EXPECT_FALSE(DecodeDdlRequest(std::string_view(ddl_req_buf.data(), cut), tmp));
  }
  for (size_t cut = 0; cut < sync_resp_buf.size(); cut++) {
    RpcSchemaSyncResponse tmp;
    EXPECT_FALSE(DecodeSchemaSyncResponse(std::string_view(sync_resp_buf.data(), cut), tmp));
  }
}

TEST(ClusterCodecTest, SearchPhase1AndFetchDocsRoundTripAndFuzz) {
  RpcSearchPhase1Request p1_req;
  p1_req.index_name = "idx_prod";
  p1_req.query_str = "(@cat:{db})=>[KNN 10 @vec $qv EF_RUNTIME 64]";
  p1_req.params = {{"qv", std::string("\x00\x00\x80\x3f\x00\x00\x00\x00", 8)}};
  p1_req.remaining_timeout_ms = 2500;
  p1_req.limit_k = 30;
  p1_req.count_only = false;
  p1_req.withscores = true;
  p1_req.verbatim = false;
  p1_req.nostopwords = true;
  p1_req.inorder = true;
  p1_req.slop = 2;
  p1_req.dialect = 2;
  p1_req.scorer = 1;
  p1_req.has_language = true;
  p1_req.language = "english";
  p1_req.infields = {"title", "body"};
  p1_req.has_inkeys = true;
  p1_req.inkeys = {"doc:1", "doc:2"};
  p1_req.has_sortby = true;
  p1_req.sortby_field = "price";
  p1_req.sortby_desc = true;
  p1_req.numeric_filters = {{"price", -std::numeric_limits<double>::infinity(), 100.0, false, true}};
  p1_req.has_geofilter = true;
  p1_req.geofilter_field = "loc";
  p1_req.geofilter_lon = -122.4194;
  p1_req.geofilter_lat = 37.7749;
  p1_req.geofilter_radius = 10.5;
  p1_req.geofilter_unit = 1;

  std::string p1_req_buf;
  EncodeSearchPhase1Request(p1_req, p1_req_buf);
  RpcSearchPhase1Request p1_req_out;
  ASSERT_TRUE(DecodeSearchPhase1Request(p1_req_buf, p1_req_out));
  EXPECT_EQ(p1_req_out.index_name, p1_req.index_name);
  EXPECT_EQ(p1_req_out.query_str, p1_req.query_str);
  EXPECT_EQ(p1_req_out.params, p1_req.params);
  EXPECT_EQ(p1_req_out.limit_k, 30u);
  EXPECT_TRUE(p1_req_out.withscores);
  EXPECT_TRUE(p1_req_out.has_sortby);
  EXPECT_EQ(p1_req_out.sortby_field, "price");
  ASSERT_EQ(p1_req_out.numeric_filters.size(), 1u);
  EXPECT_TRUE(std::isinf(p1_req_out.numeric_filters[0].min));
  EXPECT_TRUE(p1_req_out.has_geofilter);
  EXPECT_DOUBLE_EQ(p1_req_out.geofilter_radius, 10.5);

  RpcSearchPhase1Response p1_resp;
  p1_resp.ok = true;
  p1_resp.total_hits = 1234;
  p1_resp.sort_mode = 2;
  p1_resp.is_knn = true;
  p1_resp.score_alias = "dist";
  p1_resp.items = {
      {"doc:1", 0.125, 10.0, "10", true},
      {"doc:2", 0.25, 20.0, "20", false},
  };
  std::string p1_resp_buf;
  EncodeSearchPhase1Response(p1_resp, p1_resp_buf);
  RpcSearchPhase1Response p1_resp_out;
  ASSERT_TRUE(DecodeSearchPhase1Response(p1_resp_buf, p1_resp_out));
  EXPECT_TRUE(p1_resp_out.ok);
  EXPECT_EQ(p1_resp_out.total_hits, 1234u);
  EXPECT_EQ(p1_resp_out.score_alias, "dist");
  ASSERT_EQ(p1_resp_out.items.size(), 2u);
  EXPECT_EQ(p1_resp_out.items[0].doc_id, "doc:1");
  EXPECT_TRUE(p1_resp_out.items[0].has_yield_dist);

  RpcFetchDocsRequest f_req;
  f_req.index_name = "idx_prod";
  f_req.doc_ids = {"doc:1", "doc:2"};
  f_req.nocontent = false;
  f_req.withpayloads = true;
  f_req.has_return = true;
  f_req.return_fields = {"title", "price"};
  f_req.return_aliases = {{"title", "t"}};
  f_req.has_highlight = true;
  f_req.highlight_fields = {"title"};
  f_req.highlight_open = "<em>";
  f_req.highlight_close = "</em>";
  f_req.has_summarize = true;
  f_req.summarize_fields = {"body"};
  f_req.summarize_frags = 2;
  f_req.summarize_len = 15;
  f_req.summarize_separator = " | ";
  f_req.query_terms = {"redis", "search"};

  std::string f_req_buf;
  EncodeFetchDocsRequest(f_req, f_req_buf);
  RpcFetchDocsRequest f_req_out;
  ASSERT_TRUE(DecodeFetchDocsRequest(f_req_buf, f_req_out));
  EXPECT_EQ(f_req_out.doc_ids, f_req.doc_ids);
  EXPECT_EQ(f_req_out.return_aliases, f_req.return_aliases);
  EXPECT_EQ(f_req_out.highlight_open, "<em>");
  EXPECT_EQ(f_req_out.summarize_separator, " | ");

  RpcFetchDocsResponse f_resp;
  f_resp.ok = true;
  f_resp.docs = {
      {"doc:1", true, true, std::string("p\0ay", 4), {{"t", "<em>redis</em>"}, {"price", "10"}}},
      {"doc:2", false, false, "", {}},
  };
  std::string f_resp_buf;
  EncodeFetchDocsResponse(f_resp, f_resp_buf);
  RpcFetchDocsResponse f_resp_out;
  ASSERT_TRUE(DecodeFetchDocsResponse(f_resp_buf, f_resp_out));
  EXPECT_TRUE(f_resp_out.ok);
  ASSERT_EQ(f_resp_out.docs.size(), 2u);
  EXPECT_TRUE(f_resp_out.docs[0].found);
  EXPECT_EQ(f_resp_out.docs[0].payload, std::string("p\0ay", 4));
  EXPECT_FALSE(f_resp_out.docs[1].found);

  // Truncation fuzzing on all 4 Search/Fetch codecs
  for (size_t cut = 0; cut < p1_req_buf.size(); cut++) {
    RpcSearchPhase1Request tmp;
    EXPECT_FALSE(DecodeSearchPhase1Request(std::string_view(p1_req_buf.data(), cut), tmp));
  }
  for (size_t cut = 0; cut < p1_resp_buf.size(); cut++) {
    RpcSearchPhase1Response tmp;
    EXPECT_FALSE(DecodeSearchPhase1Response(std::string_view(p1_resp_buf.data(), cut), tmp));
  }
  for (size_t cut = 0; cut < f_req_buf.size(); cut++) {
    RpcFetchDocsRequest tmp;
    EXPECT_FALSE(DecodeFetchDocsRequest(std::string_view(f_req_buf.data(), cut), tmp));
  }
  for (size_t cut = 0; cut < f_resp_buf.size(); cut++) {
    RpcFetchDocsResponse tmp;
    EXPECT_FALSE(DecodeFetchDocsResponse(std::string_view(f_resp_buf.data(), cut), tmp));
  }
}

TEST(ClusterCodecTest, Phase69PayloadsAndClusterHashSlotRoundTrip) {
  // Known Redis Cluster CRC16 slot test vectors
  EXPECT_EQ(ComputeClusterHashSlot("123456789"), 12739u);
  EXPECT_EQ(ComputeClusterHashSlot("foo"), 12182u);
  EXPECT_EQ(ComputeClusterHashSlot("user:{foo}:profile"), ComputeClusterHashSlot("foo"));
  EXPECT_EQ(ComputeClusterHashSlot("a{}b"), ComputeClusterHashSlot("a{}b"));

  // DFS Term Stats
  RpcDfsTermStatsRequest dfs_req;
  dfs_req.index_name = "idx_dfs";
  dfs_req.query_str = "@title:redis cluster";
  dfs_req.verbatim = true;
  dfs_req.nostopwords = false;
  dfs_req.dialect = 2;
  dfs_req.has_language = true;
  dfs_req.language = "english";
  dfs_req.infields = {"title", "body"};
  std::string dfs_req_buf;
  EncodeDfsTermStatsRequest(dfs_req, dfs_req_buf);
  RpcDfsTermStatsRequest dfs_req_out;
  ASSERT_TRUE(DecodeDfsTermStatsRequest(dfs_req_buf, dfs_req_out));
  EXPECT_EQ(dfs_req_out.index_name, dfs_req.index_name);
  EXPECT_EQ(dfs_req_out.query_str, dfs_req.query_str);
  EXPECT_EQ(dfs_req_out.infields, dfs_req.infields);

  RpcDfsTermStatsResponse dfs_resp;
  dfs_resp.ok = true;
  dfs_resp.fields = {{"body", 1000, 14500, {{"cluster", 25}, {"redis", 90}}}};
  std::string dfs_resp_buf;
  EncodeDfsTermStatsResponse(dfs_resp, dfs_resp_buf);
  RpcDfsTermStatsResponse dfs_resp_out;
  ASSERT_TRUE(DecodeDfsTermStatsResponse(dfs_resp_buf, dfs_resp_out));
  ASSERT_TRUE(dfs_resp_out.ok);
  ASSERT_EQ(dfs_resp_out.fields.size(), 1u);
  EXPECT_EQ(dfs_resp_out.fields[0].live_docs, 1000u);
  EXPECT_EQ(dfs_resp_out.fields[0].total_doc_len, 14500u);
  EXPECT_EQ(dfs_resp_out.fields[0].term_df.size(), 2u);

  // Hybrid Phase 1
  RpcHybridPhase1Request h_req;
  h_req.index_name = "idx_hyb";
  h_req.text_query = "distributed search";
  h_req.scorer = 0;
  h_req.text_limit = 50;
  h_req.vec_field = "emb";
  h_req.vec_param = "qvec";
  h_req.knn_k = 20;
  h_req.ef_runtime = 64;
  h_req.oversample = 4;
  h_req.vec_filter_query = "@cat:{tech}";
  h_req.params = {{"qvec", std::string("\x01\x02\x00\x03", 4)}};
  h_req.dialect = 2;
  h_req.remaining_timeout_ms = 3500;
  h_req.global_bm25_stats = dfs_resp.fields;
  std::string h_req_buf;
  EncodeHybridPhase1Request(h_req, h_req_buf);
  RpcHybridPhase1Request h_req_out;
  ASSERT_TRUE(DecodeHybridPhase1Request(h_req_buf, h_req_out));
  EXPECT_EQ(h_req_out.text_query, h_req.text_query);
  EXPECT_EQ(h_req_out.knn_k, 20u);
  EXPECT_EQ(h_req_out.global_bm25_stats.size(), 1u);

  RpcHybridPhase1Response h_resp;
  h_resp.ok = true;
  h_resp.text_hits = {{"doc:1", 4.5}, {"doc:2", 2.1}};
  h_resp.vec_hits = {{"doc:2", 0.12f}, {"doc:3", 0.34f}};
  std::string h_resp_buf;
  EncodeHybridPhase1Response(h_resp, h_resp_buf);
  RpcHybridPhase1Response h_resp_out;
  ASSERT_TRUE(DecodeHybridPhase1Response(h_resp_buf, h_resp_out));
  ASSERT_TRUE(h_resp_out.ok);
  EXPECT_EQ(h_resp_out.text_hits.size(), 2u);
  EXPECT_EQ(h_resp_out.vec_hits.size(), 2u);

  // InfoStats
  RpcInfoStatsRequest info_req{"idx_info"};
  std::string info_req_buf;
  EncodeInfoStatsRequest(info_req, info_req_buf);
  RpcInfoStatsRequest info_req_out;
  ASSERT_TRUE(DecodeInfoStatsRequest(info_req_buf, info_req_out));
  EXPECT_EQ(info_req_out.index_name, "idx_info");

  RpcInfoStatsResponse info_resp{true, "", 500, 1200, 65536, 131072, true, 250, 500, 0.50, 2, 10, 8};
  std::string info_resp_buf;
  EncodeInfoStatsResponse(info_resp, info_resp_buf);
  RpcInfoStatsResponse info_resp_out;
  ASSERT_TRUE(DecodeInfoStatsResponse(info_resp_buf, info_resp_out));
  EXPECT_EQ(info_resp_out.num_docs, 500u);
  EXPECT_TRUE(info_resp_out.bg_indexing);
  EXPECT_DOUBLE_EQ(info_resp_out.percent_indexed, 0.50);

  // TagVals
  RpcTagValsRequest tv_req{"idx_tv", "tags"};
  std::string tv_req_buf;
  EncodeTagValsRequest(tv_req, tv_req_buf);
  RpcTagValsRequest tv_req_out;
  ASSERT_TRUE(DecodeTagValsRequest(tv_req_buf, tv_req_out));
  EXPECT_EQ(tv_req_out.field_name, "tags");

  RpcTagValsResponse tv_resp{true, "", {"alpha", "beta", "gamma"}};
  std::string tv_resp_buf;
  EncodeTagValsResponse(tv_resp, tv_resp_buf);
  RpcTagValsResponse tv_resp_out;
  ASSERT_TRUE(DecodeTagValsResponse(tv_resp_buf, tv_resp_out));
  EXPECT_EQ(tv_resp_out.tags, tv_resp.tags);

  // SpellCheck
  RpcSpellCheckRequest sp_req{"idx_sp", "redsi serch", 2, {"inc_d"}, {"exc_d"}};
  std::string sp_req_buf;
  EncodeSpellCheckRequest(sp_req, sp_req_buf);
  RpcSpellCheckRequest sp_req_out;
  ASSERT_TRUE(DecodeSpellCheckRequest(sp_req_buf, sp_req_out));
  EXPECT_EQ(sp_req_out.max_dist, 2);
  EXPECT_EQ(sp_req_out.include_dicts, sp_req.include_dicts);

  RpcSpellCheckResponse sp_resp;
  sp_resp.ok = true;
  sp_resp.known_terms = {"redis"};
  sp_resp.term_suggestions = {{"serch", {{"doc:1", 2.5}, {"search", 1.0}}}};
  std::string sp_resp_buf;
  EncodeSpellCheckResponse(sp_resp, sp_resp_buf);
  RpcSpellCheckResponse sp_resp_out;
  ASSERT_TRUE(DecodeSpellCheckResponse(sp_resp_buf, sp_resp_out));
  EXPECT_EQ(sp_resp_out.known_terms, sp_resp.known_terms);
  ASSERT_EQ(sp_resp_out.term_suggestions.size(), 1u);

  // Suggest
  RpcSuggestRequest sug_req{"FT.SUGGET", {"ac", "red", "FUZZY", "MAX", "5"}};
  std::string sug_req_buf;
  EncodeSuggestRequest(sug_req, sug_req_buf);
  RpcSuggestRequest sug_req_out;
  ASSERT_TRUE(DecodeSuggestRequest(sug_req_buf, sug_req_out));
  EXPECT_EQ(sug_req_out.cmd_name, "FT.SUGGET");
  EXPECT_EQ(sug_req_out.args, sug_req.args);

  RpcSuggestResponse sug_resp{true, "", 3, {{"redis", 10.5, "p1"}, {"redwood", 3.0, ""}}};
  std::string sug_resp_buf;
  EncodeSuggestResponse(sug_resp, sug_resp_buf);
  RpcSuggestResponse sug_resp_out;
  ASSERT_TRUE(DecodeSuggestResponse(sug_resp_buf, sug_resp_out));
  EXPECT_EQ(sug_resp_out.int_reply, 3);
  ASSERT_EQ(sug_resp_out.entries.size(), 2u);
  EXPECT_EQ(sug_resp_out.entries[0].payload, "p1");

  // Truncation fuzzing on all Phase 69 codecs
  auto FuzzTrunc = [](const std::string& buf, auto decode_fn) {
    for (size_t cut = 0; cut < buf.size(); cut++) {
      EXPECT_FALSE(decode_fn(std::string_view(buf.data(), cut)));
    }
  };
  FuzzTrunc(dfs_req_buf, [](std::string_view v) { RpcDfsTermStatsRequest o; return DecodeDfsTermStatsRequest(v, o); });
  FuzzTrunc(dfs_resp_buf, [](std::string_view v) { RpcDfsTermStatsResponse o; return DecodeDfsTermStatsResponse(v, o); });
  FuzzTrunc(h_req_buf, [](std::string_view v) { RpcHybridPhase1Request o; return DecodeHybridPhase1Request(v, o); });
  FuzzTrunc(h_resp_buf, [](std::string_view v) { RpcHybridPhase1Response o; return DecodeHybridPhase1Response(v, o); });
  FuzzTrunc(info_req_buf, [](std::string_view v) { RpcInfoStatsRequest o; return DecodeInfoStatsRequest(v, o); });
  FuzzTrunc(info_resp_buf, [](std::string_view v) { RpcInfoStatsResponse o; return DecodeInfoStatsResponse(v, o); });
  FuzzTrunc(tv_req_buf, [](std::string_view v) { RpcTagValsRequest o; return DecodeTagValsRequest(v, o); });
  FuzzTrunc(tv_resp_buf, [](std::string_view v) { RpcTagValsResponse o; return DecodeTagValsResponse(v, o); });
  FuzzTrunc(sp_req_buf, [](std::string_view v) { RpcSpellCheckRequest o; return DecodeSpellCheckRequest(v, o); });
  FuzzTrunc(sp_resp_buf, [](std::string_view v) { RpcSpellCheckResponse o; return DecodeSpellCheckResponse(v, o); });
  FuzzTrunc(sug_req_buf, [](std::string_view v) { RpcSuggestRequest o; return DecodeSuggestRequest(v, o); });
  FuzzTrunc(sug_resp_buf, [](std::string_view v) { RpcSuggestResponse o; return DecodeSuggestResponse(v, o); });
}
