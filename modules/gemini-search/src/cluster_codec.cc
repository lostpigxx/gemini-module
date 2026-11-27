#include "cluster_codec.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace gemini_search {

namespace {

constexpr size_t kMaxNodeIdBytes = 64;
constexpr size_t kMaxErrorMsgBytes = 4096;

uint32_t LoadLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t LoadLe16(const uint8_t* p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

uint64_t LoadLe64(const uint8_t* p) {
  return static_cast<uint64_t>(LoadLe32(p)) | (static_cast<uint64_t>(LoadLe32(p + 4)) << 32);
}

void StoreLe16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFFu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void StoreLe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFFu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

void StoreLe64(uint8_t* p, uint64_t v) {
  StoreLe32(p, static_cast<uint32_t>(v & 0xFFFFFFFFu));
  StoreLe32(p + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

const uint32_t* GetCrc32Table() {
  static const std::array<uint32_t, 256> kTable = []() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int bit = 0; bit < 8; bit++) {
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    return table;
  }();
  return kTable.data();
}

}  // namespace

bool IsKnownRpcOpCode(uint16_t raw) {
  switch (static_cast<RpcOpCode>(raw)) {
    case RpcOpCode::kRpcPing:
    case RpcOpCode::kRpcPong:
    case RpcOpCode::kRpcDdlCreate:
    case RpcOpCode::kRpcDdlDrop:
    case RpcOpCode::kRpcDdlAlter:
    case RpcOpCode::kRpcDdlAlias:
    case RpcOpCode::kRpcDdlSyn:
    case RpcOpCode::kRpcDdlDict:
    case RpcOpCode::kRpcDdlResp:
    case RpcOpCode::kRpcSchemaSyncReq:
    case RpcOpCode::kRpcSchemaSyncResp:
    case RpcOpCode::kRpcSearchPhase1:
    case RpcOpCode::kRpcSearchPhase1Resp:
    case RpcOpCode::kRpcFetchDocs:
    case RpcOpCode::kRpcFetchDocsResp:
    case RpcOpCode::kRpcDfsTermStats:
    case RpcOpCode::kRpcDfsTermStatsResp:
    case RpcOpCode::kRpcHybridPhase1:
    case RpcOpCode::kRpcHybridPhase1Resp:
    case RpcOpCode::kRpcAggMap:
    case RpcOpCode::kRpcAggMapResp:
    case RpcOpCode::kRpcInfoStats:
    case RpcOpCode::kRpcInfoStatsResp:
    case RpcOpCode::kRpcTagVals:
    case RpcOpCode::kRpcTagValsResp:
    case RpcOpCode::kRpcSpellCheck:
    case RpcOpCode::kRpcSpellCheckResp:
    case RpcOpCode::kRpcSuggest:
    case RpcOpCode::kRpcSuggestResp:
    case RpcOpCode::kRpcCursor:
    case RpcOpCode::kRpcCursorResp:
      return true;
  }
  return false;
}

uint32_t ComputeCrc32(const uint8_t* data, size_t len) {
  if (!data || len == 0) return 0;
  const uint32_t* table = GetCrc32Table();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

uint32_t ComputeCrc32(std::string_view data) {
  return ComputeCrc32(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

uint16_t ComputeClusterHashSlot(std::string_view key) {
  // Redis Cluster hash-tag rule: hash content between first '{' and first '}' after it if non-empty
  std::string_view to_hash = key;
  size_t s = key.find('{');
  if (s != std::string_view::npos) {
    size_t e = key.find('}', s + 1);
    if (e != std::string_view::npos && e > s + 1) {
      to_hash = key.substr(s + 1, e - s - 1);
    }
  }
  uint16_t crc = 0;
  for (unsigned char ch : to_hash) {
    crc ^= static_cast<uint16_t>(ch) << 8;
    for (int bit = 0; bit < 8; bit++) {
      if (crc & 0x8000u) {
        crc = static_cast<uint16_t>((crc << 1) ^ 0x1021u);
      } else {
        crc = static_cast<uint16_t>(crc << 1);
      }
    }
  }
  return static_cast<uint16_t>(crc & 0x3FFFu);
}

uint32_t ComputeCursorNodeTag(std::string_view node_id) {
  uint32_t h = ComputeCrc32(node_id);
  return ((h >> 8) & 0xFFFFFFu) | 1u;  // 24-bit non-zero tag
}

uint64_t MakeClusterCursorId(std::string_view node_id, uint64_t local_seq) {
  uint64_t tag = static_cast<uint64_t>(ComputeCursorNodeTag(node_id)) & 0xFFFFFFULL;
  uint64_t seq = local_seq & 0xFFFFFFFFFULL;  // 36-bit local sequence
  if (seq == 0) seq = 1;
  return (tag << 36) | seq;
}

uint32_t ExtractCursorNodeTag(uint64_t cursor_id) {
  return static_cast<uint32_t>((cursor_id >> 36) & 0xFFFFFFULL);
}

BinaryWriter::BinaryWriter(size_t reserve_bytes) { buf_.reserve(reserve_bytes); }

void BinaryWriter::WriteU8(uint8_t v) { buf_.push_back(static_cast<char>(v)); }

void BinaryWriter::WriteU16(uint16_t v) {
  uint8_t tmp[2];
  StoreLe16(tmp, v);
  buf_.append(reinterpret_cast<const char*>(tmp), 2);
}

void BinaryWriter::WriteU32(uint32_t v) {
  uint8_t tmp[4];
  StoreLe32(tmp, v);
  buf_.append(reinterpret_cast<const char*>(tmp), 4);
}

void BinaryWriter::WriteU64(uint64_t v) {
  uint8_t tmp[8];
  StoreLe64(tmp, v);
  buf_.append(reinterpret_cast<const char*>(tmp), 8);
}

void BinaryWriter::WriteI64(int64_t v) { WriteU64(static_cast<uint64_t>(v)); }

void BinaryWriter::WriteDouble(double v) {
  uint64_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  WriteU64(bits);
}

void BinaryWriter::WriteBool(bool v) { WriteU8(v ? 1u : 0u); }

void BinaryWriter::WriteVarUint(uint64_t v) {
  while (v >= 0x80u) {
    buf_.push_back(static_cast<char>((v & 0x7Fu) | 0x80u));
    v >>= 7;
  }
  buf_.push_back(static_cast<char>(v));
}

void BinaryWriter::WriteVarInt(int64_t v) {
  uint64_t zigzag = (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
  WriteVarUint(zigzag);
}

void BinaryWriter::WriteRawBytes(const void* data, size_t len) {
  if (len == 0 || !data) return;
  buf_.append(static_cast<const char*>(data), len);
}

void BinaryWriter::WriteString(std::string_view s) {
  WriteVarUint(static_cast<uint64_t>(s.size()));
  if (!s.empty()) {
    buf_.append(s.data(), s.size());
  }
}

void BinaryWriter::WriteStringVec(const std::vector<std::string>& v) {
  WriteVarUint(static_cast<uint64_t>(v.size()));
  for (const auto& s : v) {
    WriteString(s);
  }
}

bool BinaryReader::ReadU8(uint8_t& out) {
  if (Remaining() < 1) return false;
  out = data_[pos_++];
  return true;
}

bool BinaryReader::ReadU16(uint16_t& out) {
  if (Remaining() < 2) return false;
  out = LoadLe16(data_ + pos_);
  pos_ += 2;
  return true;
}

bool BinaryReader::ReadU32(uint32_t& out) {
  if (Remaining() < 4) return false;
  out = LoadLe32(data_ + pos_);
  pos_ += 4;
  return true;
}

bool BinaryReader::ReadU64(uint64_t& out) {
  if (Remaining() < 8) return false;
  out = LoadLe64(data_ + pos_);
  pos_ += 8;
  return true;
}

bool BinaryReader::ReadI64(int64_t& out) {
  uint64_t u = 0;
  if (!ReadU64(u)) return false;
  out = static_cast<int64_t>(u);
  return true;
}

bool BinaryReader::ReadDouble(double& out, bool allow_non_finite) {
  uint64_t bits = 0;
  if (!ReadU64(bits)) return false;
  double val = 0.0;
  std::memcpy(&val, &bits, sizeof(val));
  if (!allow_non_finite && !std::isfinite(val)) return false;
  out = val;
  return true;
}

bool BinaryReader::ReadBool(bool& out) {
  uint8_t b = 0;
  if (!ReadU8(b)) return false;
  if (b != 0 && b != 1) return false;
  out = (b == 1);
  return true;
}

bool BinaryReader::ReadVarUint(uint64_t& out) {
  uint64_t result = 0;
  unsigned shift = 0;
  for (int i = 0; i < 10; i++) {
    if (pos_ >= len_) return false;
    uint8_t b = data_[pos_++];
    if (i == 9 && b > 1) return false;  // uint64 overflow
    result |= static_cast<uint64_t>(b & 0x7Fu) << shift;
    if ((b & 0x80u) == 0) {
      out = result;
      return true;
    }
    shift += 7;
  }
  return false;
}

bool BinaryReader::ReadVarInt(int64_t& out) {
  uint64_t zigzag = 0;
  if (!ReadVarUint(zigzag)) return false;
  out = static_cast<int64_t>((zigzag >> 1) ^ (~(zigzag & 1u) + 1u));
  return true;
}

bool BinaryReader::ReadRawBytes(uint8_t* dst, size_t len) {
  if (len > Remaining()) return false;
  if (len > 0) {
    std::memcpy(dst, data_ + pos_, len);
    pos_ += len;
  }
  return true;
}

bool BinaryReader::ReadRawView(size_t len, std::string_view& out) {
  if (len > Remaining()) return false;
  out = std::string_view(reinterpret_cast<const char*>(data_ + pos_), len);
  pos_ += len;
  return true;
}

bool BinaryReader::ReadString(std::string& out, size_t max_len) {
  uint64_t slen = 0;
  if (!ReadVarUint(slen)) return false;
  if (slen > max_len || slen > Remaining()) return false;
  out.assign(reinterpret_cast<const char*>(data_ + pos_), static_cast<size_t>(slen));
  pos_ += static_cast<size_t>(slen);
  return true;
}

bool BinaryReader::ReadStringVec(std::vector<std::string>& out, size_t max_count,
                                 size_t max_item_len) {
  uint64_t count = 0;
  if (!ReadVarUint(count)) return false;
  if (count > max_count || count > Remaining()) return false;
  out.clear();
  out.reserve(static_cast<size_t>(count));
  for (uint64_t i = 0; i < count; i++) {
    std::string item;
    if (!ReadString(item, max_item_len)) {
      out.clear();
      return false;
    }
    out.push_back(std::move(item));
  }
  return true;
}

bool EncodeRpcFrame(RpcOpCode opcode, uint16_t flags, uint64_t request_id,
                    std::string_view payload, std::string& out) {
  if (!IsKnownRpcOpCode(static_cast<uint16_t>(opcode))) return false;
  if (payload.size() > kMaxRpcPayloadBytes) return false;

  uint8_t hdr[kRpcHeaderBytes];
  std::memcpy(hdr, kRpcMagic, 4);
  StoreLe16(hdr + 4, kRpcProtocolVersion);
  StoreLe16(hdr + 6, static_cast<uint16_t>(opcode));
  StoreLe16(hdr + 8, flags);
  StoreLe64(hdr + 10, request_id);
  StoreLe32(hdr + 18, static_cast<uint32_t>(payload.size()));

  uint32_t header_crc = ComputeCrc32(hdr, 22);
  uint32_t payload_crc = ComputeCrc32(payload);
  StoreLe32(hdr + 22, header_crc);
  StoreLe32(hdr + 26, payload_crc);

  out.clear();
  out.reserve(kRpcHeaderBytes + payload.size());
  out.append(reinterpret_cast<const char*>(hdr), kRpcHeaderBytes);
  if (!payload.empty()) {
    out.append(payload.data(), payload.size());
  }
  return true;
}

FrameDecodeStatus DecodeRpcHeader(const uint8_t* data, size_t len, RpcFrameHeader& out) {
  if (len < kRpcHeaderBytes) return FrameDecodeStatus::kIncomplete;
  if (!data) return FrameDecodeStatus::kInvalidMagic;

  if (std::memcmp(data, kRpcMagic, 4) != 0) {
    return FrameDecodeStatus::kInvalidMagic;
  }

  uint32_t stored_hcrc = LoadLe32(data + 22);
  uint32_t calc_hcrc = ComputeCrc32(data, 22);
  if (stored_hcrc != calc_hcrc) {
    return FrameDecodeStatus::kHeaderCrcMismatch;
  }

  uint16_t version = LoadLe16(data + 4);
  if (version != kRpcProtocolVersion) {
    return FrameDecodeStatus::kUnsupportedVersion;
  }

  uint16_t raw_op = LoadLe16(data + 6);
  if (!IsKnownRpcOpCode(raw_op)) {
    return FrameDecodeStatus::kUnknownOpCode;
  }

  uint32_t payload_len = LoadLe32(data + 18);
  if (payload_len > kMaxRpcPayloadBytes) {
    return FrameDecodeStatus::kPayloadTooLarge;
  }

  out.version = version;
  out.opcode = static_cast<RpcOpCode>(raw_op);
  out.flags = LoadLe16(data + 8);
  out.request_id = LoadLe64(data + 10);
  out.payload_len = payload_len;
  out.header_crc = stored_hcrc;
  out.payload_crc = LoadLe32(data + 26);
  return FrameDecodeStatus::kOk;
}

FrameDecodeStatus DecodeRpcFrame(const uint8_t* data, size_t len, RpcFrame& out, size_t& consumed) {
  consumed = 0;
  RpcFrameHeader hdr;
  FrameDecodeStatus st = DecodeRpcHeader(data, len, hdr);
  if (st != FrameDecodeStatus::kOk) return st;

  size_t total_bytes = kRpcHeaderBytes + static_cast<size_t>(hdr.payload_len);
  if (len < total_bytes) return FrameDecodeStatus::kIncomplete;

  const uint8_t* payload_ptr = data + kRpcHeaderBytes;
  uint32_t calc_pcrc = ComputeCrc32(payload_ptr, hdr.payload_len);
  if (calc_pcrc != hdr.payload_crc) {
    return FrameDecodeStatus::kPayloadCrcMismatch;
  }

  out.header = hdr;
  out.payload.assign(reinterpret_cast<const char*>(payload_ptr), hdr.payload_len);
  consumed = total_bytes;
  return FrameDecodeStatus::kOk;
}

void EncodeErrorPayload(const RpcErrorPayload& err, std::string& out) {
  BinaryWriter w(16 + err.message.size());
  w.WriteU32(err.code);
  w.WriteString(err.message);
  out = w.TakeBuffer();
}

bool DecodeErrorPayload(std::string_view data, RpcErrorPayload& out) {
  BinaryReader r(data);
  if (!r.ReadU32(out.code)) return false;
  if (!r.ReadString(out.message, kMaxErrorMsgBytes)) return false;
  return r.AtEnd();
}

void EncodePingPayload(const RpcPingPayload& ping, std::string& out) {
  BinaryWriter w(32 + ping.sender_node_id.size());
  w.WriteString(ping.sender_node_id);
  w.WriteU64(ping.timestamp_ms);
  w.WriteU64(ping.schema_epoch);
  w.WriteU32(ping.rpc_port);
  out = w.TakeBuffer();
}

bool DecodePingPayload(std::string_view data, RpcPingPayload& out) {
  BinaryReader r(data);
  if (!r.ReadString(out.sender_node_id, kMaxNodeIdBytes)) return false;
  if (!r.ReadU64(out.timestamp_ms)) return false;
  if (!r.ReadU64(out.schema_epoch)) return false;
  if (!r.ReadU32(out.rpc_port)) return false;
  return r.AtEnd();
}

void EncodePongPayload(const RpcPongPayload& pong, std::string& out) {
  BinaryWriter w(40 + pong.responder_node_id.size());
  w.WriteString(pong.responder_node_id);
  w.WriteU64(pong.echo_timestamp_ms);
  w.WriteU64(pong.responder_time_ms);
  w.WriteU64(pong.schema_epoch);
  w.WriteU32(pong.rpc_port);
  out = w.TakeBuffer();
}

bool DecodePongPayload(std::string_view data, RpcPongPayload& out) {
  BinaryReader r(data);
  if (!r.ReadString(out.responder_node_id, kMaxNodeIdBytes)) return false;
  if (!r.ReadU64(out.echo_timestamp_ms)) return false;
  if (!r.ReadU64(out.responder_time_ms)) return false;
  if (!r.ReadU64(out.schema_epoch)) return false;
  if (!r.ReadU32(out.rpc_port)) return false;
  return r.AtEnd();
}

void EncodeClusterBusHello(const ClusterBusHello& msg, std::string& out) {
  uint8_t buf[kClusterBusHelloBytes];
  std::memcpy(buf, kClusterBusHelloMagic, 4);
  StoreLe16(buf + 4, msg.version);
  StoreLe32(buf + 6, msg.rpc_port);
  StoreLe64(buf + 10, msg.schema_epoch);
  StoreLe16(buf + 18, msg.protocol_ver);
  StoreLe16(buf + 20, msg.flags);
  uint32_t crc = ComputeCrc32(buf, 22);
  StoreLe32(buf + 22, crc);
  out.assign(reinterpret_cast<const char*>(buf), kClusterBusHelloBytes);
}

bool DecodeClusterBusHello(const uint8_t* data, size_t len, ClusterBusHello& out) {
  if (!data || len != kClusterBusHelloBytes) return false;
  if (std::memcmp(data, kClusterBusHelloMagic, 4) != 0) return false;

  uint32_t stored_crc = LoadLe32(data + 22);
  if (stored_crc != ComputeCrc32(data, 22)) return false;

  uint16_t ver = LoadLe16(data + 4);
  if (ver != kClusterBusHelloVersion) return false;

  out.version = ver;
  out.rpc_port = LoadLe32(data + 6);
  out.schema_epoch = LoadLe64(data + 10);
  out.protocol_ver = LoadLe16(data + 18);
  out.flags = LoadLe16(data + 20);
  return true;
}

namespace {

constexpr size_t kMaxDdlCmdBytes = 64;
constexpr size_t kMaxDdlArgs = 65536;
constexpr size_t kMaxSpecPrefixes = 1024;
constexpr size_t kMaxSpecFields = 4096;
constexpr size_t kMaxSpecStopwords = 100000;
constexpr size_t kMaxSpecSynGroups = 100000;
constexpr size_t kMaxSpecSynTerms = 100000;
constexpr size_t kMaxSchemaIndices = 65536;
constexpr size_t kMaxSchemaAliases = 100000;
constexpr size_t kMaxSchemaDicts = 65536;
constexpr size_t kMaxSpecVectorDim = 65536;

void WriteSchemaIndexSnapshot(BinaryWriter& w, const SchemaIndexSnapshot& snap) {
  WriteIndexSpec(w, snap.spec);
  w.WriteVarUint(static_cast<uint64_t>(snap.synonyms.size()));
  for (const auto& [gid, terms] : snap.synonyms) {
    w.WriteString(gid);
    w.WriteStringVec(terms);
  }
}

bool ReadSchemaIndexSnapshot(BinaryReader& r, SchemaIndexSnapshot& out) {
  if (!ReadIndexSpec(r, out.spec)) return false;
  uint64_t syn_count = 0;
  if (!r.ReadVarUint(syn_count) || syn_count > kMaxSpecSynGroups || syn_count > r.Remaining()) {
    return false;
  }
  out.synonyms.clear();
  out.synonyms.reserve(static_cast<size_t>(syn_count));
  for (uint64_t i = 0; i < syn_count; i++) {
    std::string gid;
    std::vector<std::string> terms;
    if (!r.ReadString(gid, kMaxErrorMsgBytes) ||
        !r.ReadStringVec(terms, kMaxSpecSynTerms, kMaxErrorMsgBytes)) {
      return false;
    }
    out.synonyms.emplace_back(std::move(gid), std::move(terms));
  }
  return true;
}

}  // namespace

void EncodeDdlRequest(const RpcDdlRequest& req, std::string& out) {
  BinaryWriter w(128);
  w.WriteU64(req.schema_epoch);
  w.WriteString(req.coordinator_node_id);
  w.WriteString(req.cmd_name);
  w.WriteStringVec(req.args);
  w.WriteU32(req.flags);
  out = w.TakeBuffer();
}

bool DecodeDdlRequest(std::string_view data, RpcDdlRequest& out) {
  BinaryReader r(data);
  if (!r.ReadU64(out.schema_epoch)) return false;
  if (!r.ReadString(out.coordinator_node_id, kMaxNodeIdBytes)) return false;
  if (!r.ReadString(out.cmd_name, kMaxDdlCmdBytes) || out.cmd_name.empty()) return false;
  if (!r.ReadStringVec(out.args, kMaxDdlArgs, kMaxRpcPayloadBytes)) return false;
  if (!r.ReadU32(out.flags)) return false;
  return r.AtEnd();
}

void EncodeDdlResponse(const RpcDdlResponse& resp, std::string& out) {
  BinaryWriter w(32 + resp.error_msg.size());
  w.WriteBool(resp.ok);
  w.WriteI64(resp.int_reply);
  w.WriteU64(resp.applied_epoch);
  w.WriteString(resp.error_msg);
  out = w.TakeBuffer();
}

bool DecodeDdlResponse(std::string_view data, RpcDdlResponse& out) {
  BinaryReader r(data);
  if (!r.ReadBool(out.ok)) return false;
  if (!r.ReadI64(out.int_reply)) return false;
  if (!r.ReadU64(out.applied_epoch)) return false;
  if (!r.ReadString(out.error_msg, kMaxErrorMsgBytes)) return false;
  return r.AtEnd();
}

void WriteIndexSpec(BinaryWriter& w, const IndexSpec& spec) {
  w.WriteU8(1);  // IndexSpec binary format version 1
  w.WriteString(spec.name);
  w.WriteStringVec(spec.prefixes);
  w.WriteU8(static_cast<uint8_t>(spec.source_type));
  w.WriteVarUint(static_cast<uint64_t>(spec.fields.size()));
  for (const auto& f : spec.fields) {
    w.WriteString(f.name);
    w.WriteU8(static_cast<uint8_t>(f.type));
    if (f.type == FieldType::kVector) {
      w.WriteU8(static_cast<uint8_t>(f.vector_params.algorithm));
      w.WriteVarUint(static_cast<uint64_t>(f.vector_params.dim));
      w.WriteU8(static_cast<uint8_t>(f.vector_params.metric));
      w.WriteVarUint(static_cast<uint64_t>(f.vector_params.m));
      w.WriteVarUint(static_cast<uint64_t>(f.vector_params.ef_construction));
      w.WriteU8(static_cast<uint8_t>(f.vector_params.data_type));
      w.WriteU8(static_cast<uint8_t>(f.vector_params.quantization));
      w.WriteU8(static_cast<uint8_t>(f.vector_params.rerank));
      w.WriteVarUint(static_cast<uint64_t>(f.vector_params.bq_oversample));
    }
    w.WriteString(f.json_path);
    w.WriteBool(f.sortable);
    w.WriteBool(f.noindex);
    w.WriteDouble(f.weight);
    w.WriteBool(f.nostem);
    w.WriteString(f.phonetic);
    w.WriteU8(static_cast<uint8_t>(static_cast<unsigned char>(f.tag_separator)));
    w.WriteBool(f.case_sensitive);
    w.WriteU8(static_cast<uint8_t>(f.geoshape_coord));
    w.WriteBool(f.index_missing);
    w.WriteBool(f.index_empty);
  }
  w.WriteBool(spec.has_custom_stopwords);
  if (spec.has_custom_stopwords) {
    w.WriteStringVec(spec.custom_stopwords);
  }
  w.WriteBool(spec.nofreqs);
  w.WriteBool(spec.nooffsets);
  w.WriteBool(spec.nohl);
  w.WriteVarInt(static_cast<int64_t>(spec.temporary_ttl));
  w.WriteString(spec.language);
  w.WriteString(spec.filter_expr);
  w.WriteBool(spec.skip_initial_scan);
  w.WriteString(spec.language_field);
  w.WriteString(spec.custom_dict);
}

bool ReadIndexSpec(BinaryReader& r, IndexSpec& out) {
  out = IndexSpec{};
  uint8_t ver = 0;
  if (!r.ReadU8(ver) || ver != 1) return false;
  if (!r.ReadString(out.name, kMaxErrorMsgBytes) || out.name.empty()) return false;
  if (!r.ReadStringVec(out.prefixes, kMaxSpecPrefixes, kMaxErrorMsgBytes)) return false;

  uint8_t raw_src = 0;
  if (!r.ReadU8(raw_src) || raw_src > static_cast<uint8_t>(IndexSourceType::kJson)) return false;
  out.source_type = static_cast<IndexSourceType>(raw_src);

  uint64_t field_count = 0;
  if (!r.ReadVarUint(field_count) || field_count > kMaxSpecFields || field_count > r.Remaining()) {
    return false;
  }
  out.fields.reserve(static_cast<size_t>(field_count));
  for (uint64_t i = 0; i < field_count; i++) {
    FieldSpec f;
    if (!r.ReadString(f.name, kMaxErrorMsgBytes) || f.name.empty()) return false;
    uint8_t raw_type = 0;
    if (!r.ReadU8(raw_type) || raw_type > static_cast<uint8_t>(FieldType::kGeoShape)) return false;
    f.type = static_cast<FieldType>(raw_type);

    if (f.type == FieldType::kVector) {
      uint8_t raw_algo = 0, raw_metric = 0, raw_dtype = 0, raw_quant = 0, raw_rerank = 0;
      uint64_t dim = 0, m = 0, ef = 0, os = 0;
      if (!r.ReadU8(raw_algo) || raw_algo > static_cast<uint8_t>(VectorAlgorithm::kHnsw)) {
        return false;
      }
      if (!r.ReadVarUint(dim) || dim == 0 || dim > kMaxSpecVectorDim) return false;
      if (!r.ReadU8(raw_metric) || raw_metric > static_cast<uint8_t>(DistanceMetric::kIP)) {
        return false;
      }
      if (!r.ReadVarUint(m) || m == 0 || m > 1024) return false;
      if (!r.ReadVarUint(ef) || ef == 0 || ef > 100000) return false;
      if (!r.ReadU8(raw_dtype) || raw_dtype > static_cast<uint8_t>(VectorDataType::kBFloat16)) {
        return false;
      }
      if (!r.ReadU8(raw_quant) || raw_quant > static_cast<uint8_t>(VectorQuantization::kBq)) {
        return false;
      }
      if (!r.ReadU8(raw_rerank) || raw_rerank > static_cast<uint8_t>(VectorRerankType::kSq8)) {
        return false;
      }
      if (!r.ReadVarUint(os) || os == 0 || os > 1024) return false;
      f.vector_params.algorithm = static_cast<VectorAlgorithm>(raw_algo);
      f.vector_params.dim = static_cast<size_t>(dim);
      f.vector_params.metric = static_cast<DistanceMetric>(raw_metric);
      f.vector_params.m = static_cast<size_t>(m);
      f.vector_params.ef_construction = static_cast<size_t>(ef);
      f.vector_params.data_type = static_cast<VectorDataType>(raw_dtype);
      f.vector_params.quantization = static_cast<VectorQuantization>(raw_quant);
      f.vector_params.rerank = static_cast<VectorRerankType>(raw_rerank);
      f.vector_params.bq_oversample = static_cast<size_t>(os);
    }

    uint8_t sep = 0, coord = 0;
    if (!r.ReadString(f.json_path, kMaxErrorMsgBytes)) return false;
    if (!r.ReadBool(f.sortable) || !r.ReadBool(f.noindex)) return false;
    if (!r.ReadDouble(f.weight) || f.weight < 0.0) return false;
    if (!r.ReadBool(f.nostem)) return false;
    if (!r.ReadString(f.phonetic, kMaxErrorMsgBytes)) return false;
    if (!r.ReadU8(sep)) return false;
    f.tag_separator = static_cast<char>(sep);
    if (!r.ReadBool(f.case_sensitive)) return false;
    if (!r.ReadU8(coord) || coord > static_cast<uint8_t>(GeoShapeCoord::kFlat)) return false;
    f.geoshape_coord = static_cast<GeoShapeCoord>(coord);
    if (!r.ReadBool(f.index_missing) || !r.ReadBool(f.index_empty)) return false;
    out.fields.push_back(std::move(f));
  }

  if (!r.ReadBool(out.has_custom_stopwords)) return false;
  if (out.has_custom_stopwords) {
    if (!r.ReadStringVec(out.custom_stopwords, kMaxSpecStopwords, kMaxErrorMsgBytes)) return false;
  }
  if (!r.ReadBool(out.nofreqs) || !r.ReadBool(out.nooffsets) || !r.ReadBool(out.nohl)) {
    return false;
  }
  int64_t ttl = 0;
  if (!r.ReadVarInt(ttl) || ttl < 0 || ttl > 2147483647LL) return false;
  out.temporary_ttl = static_cast<int>(ttl);
  if (!r.ReadString(out.language, kMaxErrorMsgBytes)) return false;
  if (!r.ReadString(out.filter_expr, kMaxErrorMsgBytes)) return false;
  if (!r.ReadBool(out.skip_initial_scan)) return false;
  if (!r.ReadString(out.language_field, kMaxErrorMsgBytes)) return false;
  if (!r.ReadString(out.custom_dict, kMaxErrorMsgBytes)) return false;
  return true;
}

void EncodeSchemaIndexSnapshot(const SchemaIndexSnapshot& snap, std::string& out) {
  BinaryWriter w(256);
  WriteSchemaIndexSnapshot(w, snap);
  out = w.TakeBuffer();
}

bool DecodeSchemaIndexSnapshot(std::string_view data, SchemaIndexSnapshot& out) {
  BinaryReader r(data);
  if (!ReadSchemaIndexSnapshot(r, out)) return false;
  return r.AtEnd();
}

void EncodeSchemaSyncRequest(const RpcSchemaSyncRequest& req, std::string& out) {
  BinaryWriter w(64);
  w.WriteString(req.requester_node_id);
  w.WriteU64(req.local_schema_epoch);
  out = w.TakeBuffer();
}

bool DecodeSchemaSyncRequest(std::string_view data, RpcSchemaSyncRequest& out) {
  BinaryReader r(data);
  if (!r.ReadString(out.requester_node_id, kMaxNodeIdBytes)) return false;
  if (!r.ReadU64(out.local_schema_epoch)) return false;
  return r.AtEnd();
}

void EncodeSchemaSyncResponse(const RpcSchemaSyncResponse& resp, std::string& out) {
  BinaryWriter w(512);
  w.WriteU64(resp.schema_epoch);

  w.WriteVarUint(static_cast<uint64_t>(resp.indices.size()));
  for (const auto& idx : resp.indices) {
    WriteSchemaIndexSnapshot(w, idx);
  }

  w.WriteVarUint(static_cast<uint64_t>(resp.aliases.size()));
  for (const auto& [k, v] : resp.aliases) {
    w.WriteString(k);
    w.WriteString(v);
  }

  w.WriteVarUint(static_cast<uint64_t>(resp.term_dicts.size()));
  for (const auto& [dname, terms] : resp.term_dicts) {
    w.WriteString(dname);
    w.WriteStringVec(terms);
  }
  out = w.TakeBuffer();
}

bool DecodeSchemaSyncResponse(std::string_view data, RpcSchemaSyncResponse& out) {
  BinaryReader r(data);
  if (!r.ReadU64(out.schema_epoch)) return false;

  uint64_t idx_count = 0;
  if (!r.ReadVarUint(idx_count) || idx_count > kMaxSchemaIndices || idx_count > r.Remaining()) {
    return false;
  }
  out.indices.clear();
  out.indices.reserve(static_cast<size_t>(idx_count));
  for (uint64_t i = 0; i < idx_count; i++) {
    SchemaIndexSnapshot snap;
    if (!ReadSchemaIndexSnapshot(r, snap)) return false;
    out.indices.push_back(std::move(snap));
  }

  uint64_t alias_count = 0;
  if (!r.ReadVarUint(alias_count) || alias_count > kMaxSchemaAliases ||
      alias_count > r.Remaining()) {
    return false;
  }
  out.aliases.clear();
  out.aliases.reserve(static_cast<size_t>(alias_count));
  for (uint64_t i = 0; i < alias_count; i++) {
    std::string k, v;
    if (!r.ReadString(k, kMaxErrorMsgBytes) || !r.ReadString(v, kMaxErrorMsgBytes)) return false;
    out.aliases.emplace_back(std::move(k), std::move(v));
  }

  uint64_t dict_count = 0;
  if (!r.ReadVarUint(dict_count) || dict_count > kMaxSchemaDicts || dict_count > r.Remaining()) {
    return false;
  }
  out.term_dicts.clear();
  out.term_dicts.reserve(static_cast<size_t>(dict_count));
  for (uint64_t i = 0; i < dict_count; i++) {
    std::string dname;
    std::vector<std::string> terms;
    if (!r.ReadString(dname, kMaxErrorMsgBytes) ||
        !r.ReadStringVec(terms, kMaxSpecSynTerms, kMaxErrorMsgBytes)) {
      return false;
    }
    out.term_dicts.emplace_back(std::move(dname), std::move(terms));
  }
  return r.AtEnd();
}

static void WriteFieldBm25StatsVec(BinaryWriter& w, const std::vector<RpcFieldBm25Stats>& stats) {
  w.WriteVarUint(static_cast<uint64_t>(stats.size()));
  for (const auto& st : stats) {
    w.WriteString(st.field_name);
    w.WriteVarUint(st.live_docs);
    w.WriteVarUint(st.total_doc_len);
    w.WriteVarUint(static_cast<uint64_t>(st.term_df.size()));
    for (const auto& [term, df] : st.term_df) {
      w.WriteString(term);
      w.WriteVarUint(df);
    }
  }
}

static bool ReadFieldBm25StatsVec(BinaryReader& r, std::vector<RpcFieldBm25Stats>& out) {
  uint64_t fcount = 0;
  if (!r.ReadVarUint(fcount) || fcount > 4096 || fcount > r.Remaining()) return false;
  out.clear();
  out.reserve(static_cast<size_t>(fcount));
  for (uint64_t i = 0; i < fcount; i++) {
    RpcFieldBm25Stats st;
    if (!r.ReadString(st.field_name, kMaxErrorMsgBytes)) return false;
    if (!r.ReadVarUint(st.live_docs) || !r.ReadVarUint(st.total_doc_len)) return false;
    uint64_t tcount = 0;
    if (!r.ReadVarUint(tcount) || tcount > kMaxRpcVecItems || tcount > r.Remaining()) return false;
    st.term_df.reserve(static_cast<size_t>(tcount));
    for (uint64_t j = 0; j < tcount; j++) {
      std::string term;
      uint64_t df = 0;
      if (!r.ReadString(term, kMaxErrorMsgBytes) || !r.ReadVarUint(df)) return false;
      st.term_df.emplace_back(std::move(term), df);
    }
    out.push_back(std::move(st));
  }
  return true;
}

void EncodeSearchPhase1Request(const RpcSearchPhase1Request& req, std::string& out) {
  BinaryWriter w(256);
  w.WriteString(req.index_name);
  w.WriteString(req.query_str);
  w.WriteVarUint(static_cast<uint64_t>(req.params.size()));
  for (const auto& [k, v] : req.params) {
    w.WriteString(k);
    w.WriteString(v);
  }
  w.WriteI64(req.remaining_timeout_ms);
  w.WriteVarUint(req.limit_k);
  w.WriteBool(req.count_only);
  w.WriteBool(req.withscores);
  w.WriteBool(req.verbatim);
  w.WriteBool(req.nostopwords);
  w.WriteBool(req.inorder);
  w.WriteVarInt(static_cast<int64_t>(req.slop));
  w.WriteVarInt(static_cast<int64_t>(req.dialect));
  w.WriteU8(req.scorer);
  w.WriteBool(req.has_language);
  if (req.has_language) w.WriteString(req.language);
  w.WriteStringVec(req.infields);
  w.WriteBool(req.has_inkeys);
  if (req.has_inkeys) w.WriteStringVec(req.inkeys);
  w.WriteBool(req.has_sortby);
  if (req.has_sortby) {
    w.WriteString(req.sortby_field);
    w.WriteBool(req.sortby_desc);
  }
  w.WriteVarUint(static_cast<uint64_t>(req.numeric_filters.size()));
  for (const auto& nf : req.numeric_filters) {
    w.WriteString(nf.field);
    w.WriteDouble(nf.min);
    w.WriteDouble(nf.max);
    w.WriteBool(nf.min_exclusive);
    w.WriteBool(nf.max_exclusive);
  }
  w.WriteBool(req.has_geofilter);
  if (req.has_geofilter) {
    w.WriteString(req.geofilter_field);
    w.WriteDouble(req.geofilter_lon);
    w.WriteDouble(req.geofilter_lat);
    w.WriteDouble(req.geofilter_radius);
    w.WriteU8(req.geofilter_unit);
  }
  WriteFieldBm25StatsVec(w, req.global_bm25_stats);
  out = w.TakeBuffer();
}

bool DecodeSearchPhase1Request(std::string_view data, RpcSearchPhase1Request& out) {
  out = RpcSearchPhase1Request{};
  BinaryReader r(data);
  if (!r.ReadString(out.index_name, kMaxErrorMsgBytes) || out.index_name.empty()) return false;
  if (!r.ReadString(out.query_str, kMaxRpcPayloadBytes)) return false;

  uint64_t param_count = 0;
  if (!r.ReadVarUint(param_count) || param_count > 10000 || param_count > r.Remaining()) {
    return false;
  }
  out.params.reserve(static_cast<size_t>(param_count));
  for (uint64_t i = 0; i < param_count; i++) {
    std::string k, v;
    if (!r.ReadString(k, kMaxErrorMsgBytes) || !r.ReadString(v, kMaxRpcPayloadBytes)) return false;
    out.params.emplace_back(std::move(k), std::move(v));
  }

  int64_t slop = 0, dialect = 0;
  if (!r.ReadI64(out.remaining_timeout_ms)) return false;
  if (!r.ReadVarUint(out.limit_k)) return false;
  if (!r.ReadBool(out.count_only) || !r.ReadBool(out.withscores) || !r.ReadBool(out.verbatim) ||
      !r.ReadBool(out.nostopwords) || !r.ReadBool(out.inorder)) {
    return false;
  }
  if (!r.ReadVarInt(slop) || slop < 0 || slop > 1000000) return false;
  if (!r.ReadVarInt(dialect) || dialect < 0 || dialect > 100) return false;
  out.slop = static_cast<int32_t>(slop);
  out.dialect = static_cast<int32_t>(dialect);
  if (!r.ReadU8(out.scorer)) return false;
  if (!r.ReadBool(out.has_language)) return false;
  if (out.has_language && !r.ReadString(out.language, 128)) return false;
  if (!r.ReadStringVec(out.infields, 4096, kMaxErrorMsgBytes)) return false;
  if (!r.ReadBool(out.has_inkeys)) return false;
  if (out.has_inkeys && !r.ReadStringVec(out.inkeys, kMaxRpcVecItems, kMaxErrorMsgBytes)) {
    return false;
  }
  if (!r.ReadBool(out.has_sortby)) return false;
  if (out.has_sortby) {
    if (!r.ReadString(out.sortby_field, kMaxErrorMsgBytes) || !r.ReadBool(out.sortby_desc)) {
      return false;
    }
  }
  uint64_t nf_count = 0;
  if (!r.ReadVarUint(nf_count) || nf_count > 1024 || nf_count > r.Remaining()) return false;
  out.numeric_filters.reserve(static_cast<size_t>(nf_count));
  for (uint64_t i = 0; i < nf_count; i++) {
    RpcSearchNumericFilter nf;
    // Allow +-inf for numeric FILTER bounds, disallow NaN
    if (!r.ReadString(nf.field, kMaxErrorMsgBytes)) return false;
    if (!r.ReadDouble(nf.min, /*allow_non_finite=*/true) || std::isnan(nf.min)) return false;
    if (!r.ReadDouble(nf.max, /*allow_non_finite=*/true) || std::isnan(nf.max)) return false;
    if (!r.ReadBool(nf.min_exclusive) || !r.ReadBool(nf.max_exclusive)) return false;
    out.numeric_filters.push_back(std::move(nf));
  }
  if (!r.ReadBool(out.has_geofilter)) return false;
  if (out.has_geofilter) {
    if (!r.ReadString(out.geofilter_field, kMaxErrorMsgBytes) ||
        !r.ReadDouble(out.geofilter_lon) || !r.ReadDouble(out.geofilter_lat) ||
        !r.ReadDouble(out.geofilter_radius) || !r.ReadU8(out.geofilter_unit)) {
      return false;
    }
  }
  if (!ReadFieldBm25StatsVec(r, out.global_bm25_stats)) return false;
  return r.AtEnd();
}

void EncodeSearchPhase1Response(const RpcSearchPhase1Response& resp, std::string& out) {
  BinaryWriter w(64 + resp.items.size() * 48);
  w.WriteBool(resp.ok);
  w.WriteString(resp.error_msg);
  w.WriteVarUint(resp.total_hits);
  w.WriteU8(resp.sort_mode);
  w.WriteBool(resp.is_knn);
  w.WriteString(resp.score_alias);
  w.WriteVarUint(static_cast<uint64_t>(resp.items.size()));
  for (const auto& item : resp.items) {
    w.WriteString(item.doc_id);
    w.WriteDouble(item.score);
    w.WriteDouble(item.sort_num);
    w.WriteString(item.sort_str);
    w.WriteBool(item.has_yield_dist);
  }
  out = w.TakeBuffer();
}

bool DecodeSearchPhase1Response(std::string_view data, RpcSearchPhase1Response& out) {
  out = RpcSearchPhase1Response{};
  BinaryReader r(data);
  if (!r.ReadBool(out.ok)) return false;
  if (!r.ReadString(out.error_msg, kMaxErrorMsgBytes)) return false;
  if (!r.ReadVarUint(out.total_hits)) return false;
  if (!r.ReadU8(out.sort_mode) || out.sort_mode > 6) return false;
  if (!r.ReadBool(out.is_knn)) return false;
  if (!r.ReadString(out.score_alias, kMaxErrorMsgBytes)) return false;

  uint64_t count = 0;
  if (!r.ReadVarUint(count) || count > kMaxRpcVecItems || count > r.Remaining()) return false;
  out.items.reserve(static_cast<size_t>(count));
  for (uint64_t i = 0; i < count; i++) {
    RpcSearchPhase1Item item;
    if (!r.ReadString(item.doc_id, kMaxErrorMsgBytes)) return false;
    if (!r.ReadDouble(item.score, /*allow_non_finite=*/true) || std::isnan(item.score)) {
      return false;
    }
    if (!r.ReadDouble(item.sort_num, /*allow_non_finite=*/true) || std::isnan(item.sort_num)) {
      return false;
    }
    if (!r.ReadString(item.sort_str, kMaxRpcPayloadBytes)) return false;
    if (!r.ReadBool(item.has_yield_dist)) return false;
    out.items.push_back(std::move(item));
  }
  return r.AtEnd();
}

void EncodeFetchDocsRequest(const RpcFetchDocsRequest& req, std::string& out) {
  BinaryWriter w(128 + req.doc_ids.size() * 24);
  w.WriteString(req.index_name);
  w.WriteStringVec(req.doc_ids);
  w.WriteBool(req.nocontent);
  w.WriteBool(req.withpayloads);
  w.WriteBool(req.has_return);
  if (req.has_return) {
    w.WriteStringVec(req.return_fields);
    w.WriteVarUint(static_cast<uint64_t>(req.return_aliases.size()));
    for (const auto& [k, v] : req.return_aliases) {
      w.WriteString(k);
      w.WriteString(v);
    }
  }
  w.WriteBool(req.has_highlight);
  if (req.has_highlight) {
    w.WriteStringVec(req.highlight_fields);
    w.WriteString(req.highlight_open);
    w.WriteString(req.highlight_close);
  }
  w.WriteBool(req.has_summarize);
  if (req.has_summarize) {
    w.WriteStringVec(req.summarize_fields);
    w.WriteVarInt(static_cast<int64_t>(req.summarize_frags));
    w.WriteVarInt(static_cast<int64_t>(req.summarize_len));
    w.WriteString(req.summarize_separator);
  }
  w.WriteStringVec(req.query_terms);
  out = w.TakeBuffer();
}

bool DecodeFetchDocsRequest(std::string_view data, RpcFetchDocsRequest& out) {
  out = RpcFetchDocsRequest{};
  BinaryReader r(data);
  if (!r.ReadString(out.index_name, kMaxErrorMsgBytes) || out.index_name.empty()) return false;
  if (!r.ReadStringVec(out.doc_ids, kMaxRpcVecItems, kMaxErrorMsgBytes)) return false;
  if (!r.ReadBool(out.nocontent) || !r.ReadBool(out.withpayloads) || !r.ReadBool(out.has_return)) {
    return false;
  }
  if (out.has_return) {
    if (!r.ReadStringVec(out.return_fields, 4096, kMaxErrorMsgBytes)) return false;
    uint64_t acount = 0;
    if (!r.ReadVarUint(acount) || acount > 4096 || acount > r.Remaining()) return false;
    out.return_aliases.reserve(static_cast<size_t>(acount));
    for (uint64_t i = 0; i < acount; i++) {
      std::string k, v;
      if (!r.ReadString(k, kMaxErrorMsgBytes) || !r.ReadString(v, kMaxErrorMsgBytes)) return false;
      out.return_aliases.emplace_back(std::move(k), std::move(v));
    }
  }
  if (!r.ReadBool(out.has_highlight)) return false;
  if (out.has_highlight) {
    if (!r.ReadStringVec(out.highlight_fields, 4096, kMaxErrorMsgBytes)) return false;
    if (!r.ReadString(out.highlight_open, 256) || !r.ReadString(out.highlight_close, 256)) {
      return false;
    }
  }
  if (!r.ReadBool(out.has_summarize)) return false;
  if (out.has_summarize) {
    int64_t frags = 0, slen = 0;
    if (!r.ReadStringVec(out.summarize_fields, 4096, kMaxErrorMsgBytes)) return false;
    if (!r.ReadVarInt(frags) || frags < 0 || frags > 10000) return false;
    if (!r.ReadVarInt(slen) || slen < 0 || slen > 100000) return false;
    out.summarize_frags = static_cast<int32_t>(frags);
    out.summarize_len = static_cast<int32_t>(slen);
    if (!r.ReadString(out.summarize_separator, 256)) return false;
  }
  if (!r.ReadStringVec(out.query_terms, 4096, kMaxErrorMsgBytes)) return false;
  return r.AtEnd();
}

void EncodeFetchDocsResponse(const RpcFetchDocsResponse& resp, std::string& out) {
  BinaryWriter w(128 + resp.docs.size() * 64);
  w.WriteBool(resp.ok);
  w.WriteString(resp.error_msg);
  w.WriteVarUint(static_cast<uint64_t>(resp.docs.size()));
  for (const auto& doc : resp.docs) {
    w.WriteString(doc.doc_id);
    w.WriteBool(doc.found);
    w.WriteBool(doc.has_payload);
    if (doc.has_payload) w.WriteString(doc.payload);
    w.WriteVarUint(static_cast<uint64_t>(doc.fields.size()));
    for (const auto& [k, v] : doc.fields) {
      w.WriteString(k);
      w.WriteString(v);
    }
  }
  out = w.TakeBuffer();
}

bool DecodeFetchDocsResponse(std::string_view data, RpcFetchDocsResponse& out) {
  out = RpcFetchDocsResponse{};
  BinaryReader r(data);
  if (!r.ReadBool(out.ok)) return false;
  if (!r.ReadString(out.error_msg, kMaxErrorMsgBytes)) return false;
  uint64_t dcount = 0;
  if (!r.ReadVarUint(dcount) || dcount > kMaxRpcVecItems || dcount > r.Remaining()) return false;
  out.docs.reserve(static_cast<size_t>(dcount));
  for (uint64_t i = 0; i < dcount; i++) {
    RpcFetchedDoc doc;
    if (!r.ReadString(doc.doc_id, kMaxErrorMsgBytes)) return false;
    if (!r.ReadBool(doc.found) || !r.ReadBool(doc.has_payload)) return false;
    if (doc.has_payload && !r.ReadString(doc.payload, kMaxRpcPayloadBytes)) return false;
    uint64_t fcount = 0;
    if (!r.ReadVarUint(fcount) || fcount > 65536 || fcount > r.Remaining()) return false;
    doc.fields.reserve(static_cast<size_t>(fcount));
    for (uint64_t f = 0; f < fcount; f++) {
      std::string k, v;
      if (!r.ReadString(k, kMaxErrorMsgBytes) || !r.ReadString(v, kMaxRpcPayloadBytes)) {
        return false;
      }
      doc.fields.emplace_back(std::move(k), std::move(v));
    }
    out.docs.push_back(std::move(doc));
  }
  return r.AtEnd();
}

// ============================================================================
// Phase 69: BM25 DFS, Hybrid Phase 1, Info, TagVals, SpellCheck, Suggest Codecs
// ============================================================================

void EncodeDfsTermStatsRequest(const RpcDfsTermStatsRequest& req, std::string& out) {
  BinaryWriter w(128);
  w.WriteString(req.index_name);
  w.WriteString(req.query_str);
  w.WriteBool(req.verbatim);
  w.WriteBool(req.nostopwords);
  w.WriteVarInt(static_cast<int64_t>(req.dialect));
  w.WriteBool(req.has_language);
  if (req.has_language) w.WriteString(req.language);
  w.WriteStringVec(req.infields);
  out = w.TakeBuffer();
}

bool DecodeDfsTermStatsRequest(std::string_view data, RpcDfsTermStatsRequest& out) {
  out = RpcDfsTermStatsRequest{};
  BinaryReader r(data);
  if (!r.ReadString(out.index_name, kMaxErrorMsgBytes) || out.index_name.empty()) return false;
  if (!r.ReadString(out.query_str, kMaxRpcPayloadBytes)) return false;
  if (!r.ReadBool(out.verbatim) || !r.ReadBool(out.nostopwords)) return false;
  int64_t dialect = 0;
  if (!r.ReadVarInt(dialect) || dialect < 0 || dialect > 100) return false;
  out.dialect = static_cast<int32_t>(dialect);
  if (!r.ReadBool(out.has_language)) return false;
  if (out.has_language && !r.ReadString(out.language, 128)) return false;
  if (!r.ReadStringVec(out.infields, 4096, kMaxErrorMsgBytes)) return false;
  return r.AtEnd();
}

void EncodeDfsTermStatsResponse(const RpcDfsTermStatsResponse& resp, std::string& out) {
  BinaryWriter w(128);
  w.WriteBool(resp.ok);
  w.WriteString(resp.error_msg);
  WriteFieldBm25StatsVec(w, resp.fields);
  out = w.TakeBuffer();
}

bool DecodeDfsTermStatsResponse(std::string_view data, RpcDfsTermStatsResponse& out) {
  out = RpcDfsTermStatsResponse{};
  BinaryReader r(data);
  if (!r.ReadBool(out.ok)) return false;
  if (!r.ReadString(out.error_msg, kMaxErrorMsgBytes)) return false;
  if (!ReadFieldBm25StatsVec(r, out.fields)) return false;
  return r.AtEnd();
}

void EncodeHybridPhase1Request(const RpcHybridPhase1Request& req, std::string& out) {
  BinaryWriter w(256);
  w.WriteString(req.index_name);
  w.WriteString(req.text_query);
  w.WriteU8(req.scorer);
  w.WriteVarInt(req.text_limit);
  w.WriteString(req.vec_field);
  w.WriteString(req.vec_param);
  w.WriteVarUint(req.knn_k);
  w.WriteVarUint(req.ef_runtime);
  w.WriteVarUint(req.oversample);
  w.WriteString(req.vec_filter_query);
  w.WriteVarUint(static_cast<uint64_t>(req.params.size()));
  for (const auto& [k, v] : req.params) {
    w.WriteString(k);
    w.WriteString(v);
  }
  w.WriteVarInt(static_cast<int64_t>(req.dialect));
  w.WriteI64(req.remaining_timeout_ms);
  WriteFieldBm25StatsVec(w, req.global_bm25_stats);
  out = w.TakeBuffer();
}

bool DecodeHybridPhase1Request(std::string_view data, RpcHybridPhase1Request& out) {
  out = RpcHybridPhase1Request{};
  BinaryReader r(data);
  if (!r.ReadString(out.index_name, kMaxErrorMsgBytes) || out.index_name.empty()) return false;
  if (!r.ReadString(out.text_query, kMaxRpcPayloadBytes)) return false;
  if (!r.ReadU8(out.scorer)) return false;
  if (!r.ReadVarInt(out.text_limit) || out.text_limit < -1) return false;
  if (!r.ReadString(out.vec_field, kMaxErrorMsgBytes) || out.vec_field.empty()) return false;
  if (!r.ReadString(out.vec_param, kMaxErrorMsgBytes) || out.vec_param.empty()) return false;
  if (!r.ReadVarUint(out.knn_k) || out.knn_k == 0 || out.knn_k > kMaxRpcVecItems) return false;
  if (!r.ReadVarUint(out.ef_runtime) || !r.ReadVarUint(out.oversample)) return false;
  if (!r.ReadString(out.vec_filter_query, kMaxRpcPayloadBytes)) return false;
  uint64_t pcount = 0;
  if (!r.ReadVarUint(pcount) || pcount > 10000 || pcount > r.Remaining()) return false;
  out.params.reserve(static_cast<size_t>(pcount));
  for (uint64_t i = 0; i < pcount; i++) {
    std::string k, v;
    if (!r.ReadString(k, kMaxErrorMsgBytes) || !r.ReadString(v, kMaxRpcPayloadBytes)) return false;
    out.params.emplace_back(std::move(k), std::move(v));
  }
  int64_t dialect = 0;
  if (!r.ReadVarInt(dialect) || dialect < 0 || dialect > 100) return false;
  out.dialect = static_cast<int32_t>(dialect);
  if (!r.ReadI64(out.remaining_timeout_ms)) return false;
  if (!ReadFieldBm25StatsVec(r, out.global_bm25_stats)) return false;
  return r.AtEnd();
}

void EncodeHybridPhase1Response(const RpcHybridPhase1Response& resp, std::string& out) {
  BinaryWriter w(64 + (resp.text_hits.size() + resp.vec_hits.size()) * 24);
  w.WriteBool(resp.ok);
  w.WriteString(resp.error_msg);
  w.WriteVarUint(static_cast<uint64_t>(resp.text_hits.size()));
  for (const auto& [doc_id, score] : resp.text_hits) {
    w.WriteString(doc_id);
    w.WriteDouble(score);
  }
  w.WriteVarUint(static_cast<uint64_t>(resp.vec_hits.size()));
  for (const auto& [doc_id, dist] : resp.vec_hits) {
    w.WriteString(doc_id);
    w.WriteDouble(static_cast<double>(dist));
  }
  out = w.TakeBuffer();
}

bool DecodeHybridPhase1Response(std::string_view data, RpcHybridPhase1Response& out) {
  out = RpcHybridPhase1Response{};
  BinaryReader r(data);
  if (!r.ReadBool(out.ok)) return false;
  if (!r.ReadString(out.error_msg, kMaxErrorMsgBytes)) return false;
  uint64_t tcount = 0;
  if (!r.ReadVarUint(tcount) || tcount > kMaxRpcVecItems || tcount > r.Remaining()) return false;
  out.text_hits.reserve(static_cast<size_t>(tcount));
  for (uint64_t i = 0; i < tcount; i++) {
    std::string doc_id;
    double score = 0.0;
    if (!r.ReadString(doc_id, kMaxErrorMsgBytes) || !r.ReadDouble(score)) return false;
    out.text_hits.emplace_back(std::move(doc_id), score);
  }
  uint64_t vcount = 0;
  if (!r.ReadVarUint(vcount) || vcount > kMaxRpcVecItems || vcount > r.Remaining()) return false;
  out.vec_hits.reserve(static_cast<size_t>(vcount));
  for (uint64_t i = 0; i < vcount; i++) {
    std::string doc_id;
    double dist = 0.0;
    if (!r.ReadString(doc_id, kMaxErrorMsgBytes) || !r.ReadDouble(dist)) return false;
    constexpr double kMaxFloat = static_cast<double>(std::numeric_limits<float>::max());
    if (dist > kMaxFloat || dist < -kMaxFloat) return false;
    out.vec_hits.emplace_back(std::move(doc_id), static_cast<float>(dist));
  }
  return r.AtEnd();
}

void EncodeInfoStatsRequest(const RpcInfoStatsRequest& req, std::string& out) {
  BinaryWriter w(64);
  w.WriteString(req.index_name);
  out = w.TakeBuffer();
}

bool DecodeInfoStatsRequest(std::string_view data, RpcInfoStatsRequest& out) {
  out = RpcInfoStatsRequest{};
  BinaryReader r(data);
  if (!r.ReadString(out.index_name, kMaxErrorMsgBytes) || out.index_name.empty()) return false;
  return r.AtEnd();
}

void EncodeInfoStatsResponse(const RpcInfoStatsResponse& resp, std::string& out) {
  BinaryWriter w(128);
  w.WriteBool(resp.ok);
  w.WriteString(resp.error_msg);
  w.WriteVarUint(resp.num_docs);
  w.WriteVarUint(resp.num_terms);
  w.WriteVarUint(resp.inverted_bytes);
  w.WriteVarUint(resp.vector_bytes);
  w.WriteBool(resp.bg_indexing);
  w.WriteVarUint(resp.bg_scanned_keys);
  w.WriteVarUint(resp.bg_total_keys);
  w.WriteDouble(resp.percent_indexed);
  w.WriteVarUint(resp.cursor_idle);
  w.WriteVarUint(resp.cursor_total_created);
  w.WriteVarUint(resp.cursor_total_expired);
  out = w.TakeBuffer();
}

bool DecodeInfoStatsResponse(std::string_view data, RpcInfoStatsResponse& out) {
  out = RpcInfoStatsResponse{};
  BinaryReader r(data);
  if (!r.ReadBool(out.ok)) return false;
  if (!r.ReadString(out.error_msg, kMaxErrorMsgBytes)) return false;
  if (!r.ReadVarUint(out.num_docs) || !r.ReadVarUint(out.num_terms) ||
      !r.ReadVarUint(out.inverted_bytes) || !r.ReadVarUint(out.vector_bytes) ||
      !r.ReadBool(out.bg_indexing) || !r.ReadVarUint(out.bg_scanned_keys) ||
      !r.ReadVarUint(out.bg_total_keys) || !r.ReadDouble(out.percent_indexed) ||
      !r.ReadVarUint(out.cursor_idle) || !r.ReadVarUint(out.cursor_total_created) ||
      !r.ReadVarUint(out.cursor_total_expired)) {
    return false;
  }
  return r.AtEnd();
}

void EncodeTagValsRequest(const RpcTagValsRequest& req, std::string& out) {
  BinaryWriter w(64);
  w.WriteString(req.index_name);
  w.WriteString(req.field_name);
  out = w.TakeBuffer();
}

bool DecodeTagValsRequest(std::string_view data, RpcTagValsRequest& out) {
  out = RpcTagValsRequest{};
  BinaryReader r(data);
  if (!r.ReadString(out.index_name, kMaxErrorMsgBytes) || out.index_name.empty()) return false;
  if (!r.ReadString(out.field_name, kMaxErrorMsgBytes) || out.field_name.empty()) return false;
  return r.AtEnd();
}

void EncodeTagValsResponse(const RpcTagValsResponse& resp, std::string& out) {
  BinaryWriter w(64 + resp.tags.size() * 16);
  w.WriteBool(resp.ok);
  w.WriteString(resp.error_msg);
  w.WriteStringVec(resp.tags);
  out = w.TakeBuffer();
}

bool DecodeTagValsResponse(std::string_view data, RpcTagValsResponse& out) {
  out = RpcTagValsResponse{};
  BinaryReader r(data);
  if (!r.ReadBool(out.ok)) return false;
  if (!r.ReadString(out.error_msg, kMaxErrorMsgBytes)) return false;
  if (!r.ReadStringVec(out.tags, kMaxRpcVecItems, kMaxErrorMsgBytes)) return false;
  return r.AtEnd();
}

void EncodeSpellCheckRequest(const RpcSpellCheckRequest& req, std::string& out) {
  BinaryWriter w(128);
  w.WriteString(req.index_name);
  w.WriteString(req.query_str);
  w.WriteVarInt(static_cast<int64_t>(req.max_dist));
  w.WriteStringVec(req.include_dicts);
  w.WriteStringVec(req.exclude_dicts);
  out = w.TakeBuffer();
}

bool DecodeSpellCheckRequest(std::string_view data, RpcSpellCheckRequest& out) {
  out = RpcSpellCheckRequest{};
  BinaryReader r(data);
  if (!r.ReadString(out.index_name, kMaxErrorMsgBytes) || out.index_name.empty()) return false;
  if (!r.ReadString(out.query_str, kMaxRpcPayloadBytes)) return false;
  int64_t md = 1;
  if (!r.ReadVarInt(md) || md < 0 || md > 10) return false;
  out.max_dist = static_cast<int32_t>(md);
  if (!r.ReadStringVec(out.include_dicts, 1024, kMaxErrorMsgBytes)) return false;
  if (!r.ReadStringVec(out.exclude_dicts, 1024, kMaxErrorMsgBytes)) return false;
  return r.AtEnd();
}

void EncodeSpellCheckResponse(const RpcSpellCheckResponse& resp, std::string& out) {
  BinaryWriter w(128);
  w.WriteBool(resp.ok);
  w.WriteString(resp.error_msg);
  w.WriteStringVec(resp.known_terms);
  w.WriteVarUint(static_cast<uint64_t>(resp.term_suggestions.size()));
  for (const auto& ts : resp.term_suggestions) {
    w.WriteString(ts.term);
    w.WriteVarUint(static_cast<uint64_t>(ts.suggestions.size()));
    for (const auto& [sug, score] : ts.suggestions) {
      w.WriteString(sug);
      w.WriteDouble(score);
    }
  }
  out = w.TakeBuffer();
}

bool DecodeSpellCheckResponse(std::string_view data, RpcSpellCheckResponse& out) {
  out = RpcSpellCheckResponse{};
  BinaryReader r(data);
  if (!r.ReadBool(out.ok)) return false;
  if (!r.ReadString(out.error_msg, kMaxErrorMsgBytes)) return false;
  if (!r.ReadStringVec(out.known_terms, kMaxRpcVecItems, kMaxErrorMsgBytes)) return false;
  uint64_t tcount = 0;
  if (!r.ReadVarUint(tcount) || tcount > 10000 || tcount > r.Remaining()) return false;
  out.term_suggestions.reserve(static_cast<size_t>(tcount));
  for (uint64_t i = 0; i < tcount; i++) {
    RpcSpellCheckTermSugs ts;
    if (!r.ReadString(ts.term, kMaxErrorMsgBytes)) return false;
    uint64_t scount = 0;
    if (!r.ReadVarUint(scount) || scount > kMaxRpcVecItems || scount > r.Remaining()) return false;
    ts.suggestions.reserve(static_cast<size_t>(scount));
    for (uint64_t j = 0; j < scount; j++) {
      std::string sug;
      double score = 0.0;
      if (!r.ReadString(sug, kMaxErrorMsgBytes) || !r.ReadDouble(score)) return false;
      ts.suggestions.emplace_back(std::move(sug), score);
    }
    out.term_suggestions.push_back(std::move(ts));
  }
  return r.AtEnd();
}

void EncodeSuggestRequest(const RpcSuggestRequest& req, std::string& out) {
  BinaryWriter w(128);
  w.WriteString(req.cmd_name);
  w.WriteStringVec(req.args);
  out = w.TakeBuffer();
}

bool DecodeSuggestRequest(std::string_view data, RpcSuggestRequest& out) {
  out = RpcSuggestRequest{};
  BinaryReader r(data);
  if (!r.ReadString(out.cmd_name, 64) || out.cmd_name.empty()) return false;
  if (!r.ReadStringVec(out.args, 1024, kMaxRpcPayloadBytes)) return false;
  return r.AtEnd();
}

void EncodeSuggestResponse(const RpcSuggestResponse& resp, std::string& out) {
  BinaryWriter w(64 + resp.entries.size() * 32);
  w.WriteBool(resp.ok);
  w.WriteString(resp.error_msg);
  w.WriteI64(resp.int_reply);
  w.WriteVarUint(static_cast<uint64_t>(resp.entries.size()));
  for (const auto& e : resp.entries) {
    w.WriteString(e.str);
    w.WriteDouble(e.score);
    w.WriteString(e.payload);
  }
  out = w.TakeBuffer();
}

bool DecodeSuggestResponse(std::string_view data, RpcSuggestResponse& out) {
  out = RpcSuggestResponse{};
  BinaryReader r(data);
  if (!r.ReadBool(out.ok)) return false;
  if (!r.ReadString(out.error_msg, kMaxErrorMsgBytes)) return false;
  if (!r.ReadI64(out.int_reply)) return false;
  uint64_t ecount = 0;
  if (!r.ReadVarUint(ecount) || ecount > kMaxRpcVecItems || ecount > r.Remaining()) return false;
  out.entries.reserve(static_cast<size_t>(ecount));
  for (uint64_t i = 0; i < ecount; i++) {
    RpcSuggestEntry e;
    if (!r.ReadString(e.str, kMaxRpcPayloadBytes) || !r.ReadDouble(e.score) ||
        !r.ReadString(e.payload, kMaxRpcPayloadBytes)) {
      return false;
    }
    out.entries.push_back(std::move(e));
  }
  return r.AtEnd();
}

// ============================================================================
// Phase 70: FT.AGGREGATE Map-Reduce & FT.CURSOR Forwarding Codecs
// ============================================================================

namespace {

void WriteReduceOp(BinaryWriter& w, const ReduceOp& op) {
  w.WriteU8(static_cast<uint8_t>(op.func));
  w.WriteString(op.field);
  w.WriteString(op.alias);
  w.WriteDouble(op.quantile_pct);
  w.WriteString(op.first_value_by);
  w.WriteBool(op.first_value_desc);
  w.WriteVarInt(static_cast<int64_t>(op.random_sample_count));
}

bool ReadReduceOp(BinaryReader& r, ReduceOp& out) {
  uint8_t raw_fn = 0;
  if (!r.ReadU8(raw_fn) || raw_fn > static_cast<uint8_t>(ReduceFunc::kCountDistinctish)) {
    return false;
  }
  out.func = static_cast<ReduceFunc>(raw_fn);
  if (!r.ReadString(out.field, kMaxErrorMsgBytes) || !r.ReadString(out.alias, kMaxErrorMsgBytes)) {
    return false;
  }
  if (!r.ReadDouble(out.quantile_pct)) return false;
  if (!r.ReadString(out.first_value_by, kMaxErrorMsgBytes)) return false;
  if (!r.ReadBool(out.first_value_desc)) return false;
  int64_t rs = 0;
  if (!r.ReadVarInt(rs) || rs < 0 || rs > 1000000) return false;
  out.random_sample_count = static_cast<long>(rs);
  return true;
}

void WritePipelineStage(BinaryWriter& w, const PipelineStage& st) {
  w.WriteU8(static_cast<uint8_t>(st.type));
  switch (st.type) {
    case PipelineStageType::kLoad:
      w.WriteStringVec(st.load_fields);
      break;
    case PipelineStageType::kApply:
      w.WriteString(st.apply_expr);
      w.WriteString(st.apply_alias);
      break;
    case PipelineStageType::kFilter:
      w.WriteString(st.filter_expr);
      break;
    case PipelineStageType::kGroupby:
      w.WriteStringVec(st.groupby_fields);
      w.WriteVarUint(static_cast<uint64_t>(st.reducers.size()));
      for (const auto& op : st.reducers) WriteReduceOp(w, op);
      break;
    case PipelineStageType::kSortby:
      w.WriteVarUint(static_cast<uint64_t>(st.sortby_keys.size()));
      for (const auto& [f, desc] : st.sortby_keys) {
        w.WriteString(f);
        w.WriteBool(desc);
      }
      w.WriteVarInt(static_cast<int64_t>(st.sortby_max));
      break;
    case PipelineStageType::kLimit:
      w.WriteVarInt(static_cast<int64_t>(st.limit_offset));
      w.WriteVarInt(static_cast<int64_t>(st.limit_count));
      break;
  }
}

bool ReadPipelineStage(BinaryReader& r, PipelineStage& out) {
  out = PipelineStage{};
  uint8_t raw_type = 0;
  if (!r.ReadU8(raw_type) || raw_type > static_cast<uint8_t>(PipelineStageType::kLimit)) {
    return false;
  }
  out.type = static_cast<PipelineStageType>(raw_type);
  switch (out.type) {
    case PipelineStageType::kLoad:
      return r.ReadStringVec(out.load_fields, 4096, kMaxErrorMsgBytes);
    case PipelineStageType::kApply:
      return r.ReadString(out.apply_expr, kMaxRpcPayloadBytes) &&
             r.ReadString(out.apply_alias, kMaxErrorMsgBytes);
    case PipelineStageType::kFilter:
      return r.ReadString(out.filter_expr, kMaxRpcPayloadBytes);
    case PipelineStageType::kGroupby: {
      if (!r.ReadStringVec(out.groupby_fields, 1024, kMaxErrorMsgBytes)) return false;
      uint64_t rc = 0;
      if (!r.ReadVarUint(rc) || rc > 1024 || rc > r.Remaining()) return false;
      out.reducers.reserve(static_cast<size_t>(rc));
      for (uint64_t i = 0; i < rc; i++) {
        ReduceOp op;
        if (!ReadReduceOp(r, op)) return false;
        out.reducers.push_back(std::move(op));
      }
      return true;
    }
    case PipelineStageType::kSortby: {
      uint64_t kc = 0;
      if (!r.ReadVarUint(kc) || kc > 256 || kc > r.Remaining()) return false;
      out.sortby_keys.reserve(static_cast<size_t>(kc));
      for (uint64_t i = 0; i < kc; i++) {
        std::string f;
        bool desc = false;
        if (!r.ReadString(f, kMaxErrorMsgBytes) || !r.ReadBool(desc)) return false;
        out.sortby_keys.emplace_back(std::move(f), desc);
      }
      int64_t mx = -1;
      if (!r.ReadVarInt(mx) || mx < -1) return false;
      out.sortby_max = static_cast<long long>(mx);
      return true;
    }
    case PipelineStageType::kLimit: {
      int64_t off = 0, cnt = 0;
      if (!r.ReadVarInt(off) || !r.ReadVarInt(cnt) || off < 0 || cnt < 0) return false;
      out.limit_offset = static_cast<long long>(off);
      out.limit_count = static_cast<long long>(cnt);
      return true;
    }
  }
  return false;
}

void WritePartialReducerState(BinaryWriter& w, const PartialReducerState& st) {
  w.WriteU8(static_cast<uint8_t>(st.func));
  switch (st.func) {
    case ReduceFunc::kCount:
      w.WriteVarUint(st.count);
      break;
    case ReduceFunc::kSum:
      w.WriteBool(st.has_val);
      if (st.has_val) w.WriteDouble(st.sum);
      break;
    case ReduceFunc::kAvg:
      w.WriteVarUint(st.count);
      w.WriteDouble(st.sum);
      break;
    case ReduceFunc::kMin:
      w.WriteBool(st.has_val);
      if (st.has_val) w.WriteDouble(st.min_val);
      break;
    case ReduceFunc::kMax:
      w.WriteBool(st.has_val);
      if (st.has_val) w.WriteDouble(st.max_val);
      break;
    case ReduceFunc::kStddev:
      w.WriteVarUint(st.count);
      w.WriteDouble(st.mean);
      w.WriteDouble(st.m2);
      break;
    case ReduceFunc::kCountDistinct:
    case ReduceFunc::kTolist:
      w.WriteStringVec(st.str_vals);
      break;
    case ReduceFunc::kRandomSample:
      w.WriteVarUint(st.count);
      w.WriteStringVec(st.str_vals);
      break;
    case ReduceFunc::kFirstValue:
      w.WriteBool(st.has_val);
      if (st.has_val) {
        w.WriteString(st.first_val);
        w.WriteDouble(st.first_sort_val);
      }
      break;
    case ReduceFunc::kCountDistinctish:
      w.WriteBool(st.hll.is_dense);
      if (st.hll.is_dense) {
        w.WriteVarUint(static_cast<uint64_t>(st.hll.dense.size()));
        if (!st.hll.dense.empty()) {
          w.WriteRawBytes(st.hll.dense.data(), st.hll.dense.size());
        }
      } else {
        w.WriteVarUint(static_cast<uint64_t>(st.hll.sparse.size()));
        for (const auto& [idx, rho] : st.hll.sparse) {
          w.WriteU16(idx);
          w.WriteU8(rho);
        }
      }
      break;
    case ReduceFunc::kQuantile:
      w.WriteBool(st.quantile.is_compressed);
      if (!st.quantile.is_compressed) {
        w.WriteVarUint(static_cast<uint64_t>(st.quantile.exact_vals.size()));
        for (double v : st.quantile.exact_vals) w.WriteDouble(v);
      } else {
        w.WriteVarUint(static_cast<uint64_t>(st.quantile.centroids.size()));
        for (const auto& [m, wt] : st.quantile.centroids) {
          w.WriteDouble(m);
          w.WriteDouble(wt);
        }
      }
      break;
  }
}

bool ReadPartialReducerState(BinaryReader& r, PartialReducerState& out) {
  out = PartialReducerState{};
  uint8_t raw_fn = 0;
  if (!r.ReadU8(raw_fn) || raw_fn > static_cast<uint8_t>(ReduceFunc::kCountDistinctish)) {
    return false;
  }
  out.func = static_cast<ReduceFunc>(raw_fn);
  switch (out.func) {
    case ReduceFunc::kCount:
      return r.ReadVarUint(out.count);
    case ReduceFunc::kSum:
      if (!r.ReadBool(out.has_val)) return false;
      return !out.has_val || r.ReadDouble(out.sum);
    case ReduceFunc::kAvg:
      return r.ReadVarUint(out.count) && r.ReadDouble(out.sum);
    case ReduceFunc::kMin:
      if (!r.ReadBool(out.has_val)) return false;
      return !out.has_val || r.ReadDouble(out.min_val);
    case ReduceFunc::kMax:
      if (!r.ReadBool(out.has_val)) return false;
      return !out.has_val || r.ReadDouble(out.max_val);
    case ReduceFunc::kStddev:
      return r.ReadVarUint(out.count) && r.ReadDouble(out.mean) && r.ReadDouble(out.m2) &&
             out.m2 >= 0.0;
    case ReduceFunc::kCountDistinct:
    case ReduceFunc::kTolist:
      return r.ReadStringVec(out.str_vals, kMaxRpcVecItems, kMaxRpcPayloadBytes);
    case ReduceFunc::kRandomSample:
      return r.ReadVarUint(out.count) &&
             r.ReadStringVec(out.str_vals, kMaxRpcVecItems, kMaxRpcPayloadBytes);
    case ReduceFunc::kFirstValue:
      if (!r.ReadBool(out.has_val)) return false;
      if (out.has_val) {
        if (!r.ReadString(out.first_val, kMaxRpcPayloadBytes) || !r.ReadDouble(out.first_sort_val)) {
          return false;
        }
      }
      return true;
    case ReduceFunc::kCountDistinctish: {
      if (!r.ReadBool(out.hll.is_dense)) return false;
      if (out.hll.is_dense) {
        uint64_t n = 0;
        if (!r.ReadVarUint(n) || n != HllSketch::kNumRegs || n > r.Remaining()) return false;
        out.hll.dense.resize(static_cast<size_t>(n));
        return r.ReadRawBytes(out.hll.dense.data(), static_cast<size_t>(n));
      }
      uint64_t n = 0;
      if (!r.ReadVarUint(n) || n > HllSketch::kNumRegs || n * 3 > r.Remaining()) return false;
      out.hll.sparse.reserve(static_cast<size_t>(n));
      for (uint64_t i = 0; i < n; i++) {
        uint16_t idx = 0;
        uint8_t rho = 0;
        constexpr uint8_t kMaxRho = static_cast<uint8_t>(64 - HllSketch::kPrecision + 1);
        if (!r.ReadU16(idx) || !r.ReadU8(rho) || idx >= HllSketch::kNumRegs || rho == 0 ||
            rho > kMaxRho) {
          return false;
        }
        out.hll.sparse.emplace_back(idx, rho);
      }
      return true;
    }
    case ReduceFunc::kQuantile: {
      if (!r.ReadBool(out.quantile.is_compressed)) return false;
      if (!out.quantile.is_compressed) {
        uint64_t n = 0;
        if (!r.ReadVarUint(n) || n > QuantileSketch::kExactLimit || n * 8 > r.Remaining()) {
          return false;
        }
        out.quantile.exact_vals.reserve(static_cast<size_t>(n));
        for (uint64_t i = 0; i < n; i++) {
          double v = 0.0;
          if (!r.ReadDouble(v)) return false;
          out.quantile.exact_vals.push_back(v);
        }
        return true;
      }
      uint64_t n = 0;
      if (!r.ReadVarUint(n) || n > 4096 || n * 16 > r.Remaining()) return false;
      out.quantile.centroids.reserve(static_cast<size_t>(n));
      for (uint64_t i = 0; i < n; i++) {
        double m = 0.0, wt = 0.0;
        if (!r.ReadDouble(m) || !r.ReadDouble(wt) || wt < 0.0) return false;
        out.quantile.centroids.emplace_back(m, wt);
      }
      return true;
    }
  }
  return false;
}

}  // namespace

void EncodeAggMapRequest(const RpcAggMapRequest& req, std::string& out) {
  BinaryWriter w(256);
  w.WriteString(req.index_name);
  w.WriteString(req.query_str);
  w.WriteVarUint(static_cast<uint64_t>(req.params.size()));
  for (const auto& [k, v] : req.params) {
    w.WriteString(k);
    w.WriteString(v);
  }
  w.WriteVarInt(static_cast<int64_t>(req.dialect));
  w.WriteI64(req.remaining_timeout_ms);
  w.WriteU8(req.mode);
  w.WriteVarUint(static_cast<uint64_t>(req.map_stages.size()));
  for (const auto& st : req.map_stages) WritePipelineStage(w, st);
  if (req.mode == 1) {
    WritePipelineStage(w, req.groupby_stage);
  }
  w.WriteVarUint(req.max_groups);
  w.WriteVarUint(req.raw_shard_limit);
  w.WriteBool(req.raw_has_sortby);
  if (req.raw_has_sortby) {
    w.WriteVarUint(static_cast<uint64_t>(req.raw_sortby_keys.size()));
    for (const auto& [f, desc] : req.raw_sortby_keys) {
      w.WriteString(f);
      w.WriteBool(desc);
    }
  }
  if (req.raw_zero_limit) {
    w.WriteBool(true);
  }
  out = w.TakeBuffer();
}

bool DecodeAggMapRequest(std::string_view data, RpcAggMapRequest& out) {
  out = RpcAggMapRequest{};
  BinaryReader r(data);
  if (!r.ReadString(out.index_name, kMaxErrorMsgBytes) || out.index_name.empty()) return false;
  if (!r.ReadString(out.query_str, kMaxRpcPayloadBytes)) return false;

  uint64_t pcount = 0;
  if (!r.ReadVarUint(pcount) || pcount > 10000 || pcount > r.Remaining()) return false;
  out.params.reserve(static_cast<size_t>(pcount));
  for (uint64_t i = 0; i < pcount; i++) {
    std::string k, v;
    if (!r.ReadString(k, kMaxErrorMsgBytes) || !r.ReadString(v, kMaxRpcPayloadBytes)) return false;
    out.params.emplace_back(std::move(k), std::move(v));
  }

  int64_t dialect = 2;
  if (!r.ReadVarInt(dialect) || dialect < 0 || dialect > 100) return false;
  out.dialect = static_cast<int32_t>(dialect);
  if (!r.ReadI64(out.remaining_timeout_ms)) return false;
  if (!r.ReadU8(out.mode) || out.mode > 1) return false;

  uint64_t scount = 0;
  if (!r.ReadVarUint(scount) || scount > 256 || scount > r.Remaining()) return false;
  out.map_stages.reserve(static_cast<size_t>(scount));
  for (uint64_t i = 0; i < scount; i++) {
    PipelineStage st;
    if (!ReadPipelineStage(r, st)) return false;
    out.map_stages.push_back(std::move(st));
  }

  if (out.mode == 1) {
    if (!ReadPipelineStage(r, out.groupby_stage) ||
        out.groupby_stage.type != PipelineStageType::kGroupby) {
      return false;
    }
  }

  if (!r.ReadVarUint(out.max_groups) || !r.ReadVarUint(out.raw_shard_limit)) return false;
  if (!r.ReadBool(out.raw_has_sortby)) return false;
  if (out.raw_has_sortby) {
    uint64_t kc = 0;
    if (!r.ReadVarUint(kc) || kc > 256 || kc > r.Remaining()) return false;
    out.raw_sortby_keys.reserve(static_cast<size_t>(kc));
    for (uint64_t i = 0; i < kc; i++) {
      std::string f;
      bool desc = false;
      if (!r.ReadString(f, kMaxErrorMsgBytes) || !r.ReadBool(desc)) return false;
      out.raw_sortby_keys.emplace_back(std::move(f), desc);
    }
  }
  if (!r.AtEnd()) {
    if (!r.ReadBool(out.raw_zero_limit)) return false;
  }
  return r.AtEnd();
}

void EncodeAggMapResponse(const RpcAggMapResponse& resp, std::string& out) {
  BinaryWriter w(256);
  w.WriteBool(resp.ok);
  w.WriteString(resp.error_msg);
  w.WriteU8(resp.mode);
  if (resp.mode == 0) {
    w.WriteVarUint(static_cast<uint64_t>(resp.raw_rows.size()));
    for (const auto& row : resp.raw_rows) {
      w.WriteString(row.doc_id);
      w.WriteBool(row.has_knn_dist);
      if (row.has_knn_dist) w.WriteDouble(row.knn_dist);
      w.WriteVarUint(static_cast<uint64_t>(row.fields.size()));
      for (const auto& [k, v] : row.fields) {
        w.WriteString(k);
        w.WriteString(v);
      }
    }
  } else {
    w.WriteVarUint(static_cast<uint64_t>(resp.partial_groups.size()));
    for (const auto& pg : resp.partial_groups) {
      w.WriteString(pg.group_key);
      w.WriteVarUint(static_cast<uint64_t>(pg.key_fields.size()));
      for (const auto& [k, v] : pg.key_fields) {
        w.WriteString(k);
        w.WriteString(v);
      }
      w.WriteVarUint(static_cast<uint64_t>(pg.reducers.size()));
      for (const auto& st : pg.reducers) WritePartialReducerState(w, st);
    }
  }
  out = w.TakeBuffer();
}

bool DecodeAggMapResponse(std::string_view data, RpcAggMapResponse& out) {
  out = RpcAggMapResponse{};
  BinaryReader r(data);
  if (!r.ReadBool(out.ok)) return false;
  if (!r.ReadString(out.error_msg, kMaxErrorMsgBytes)) return false;
  if (!r.ReadU8(out.mode) || out.mode > 1) return false;

  if (out.mode == 0) {
    uint64_t rcount = 0;
    if (!r.ReadVarUint(rcount) || rcount > kMaxRpcVecItems || rcount > r.Remaining()) return false;
    out.raw_rows.reserve(static_cast<size_t>(rcount));
    for (uint64_t i = 0; i < rcount; i++) {
      RpcAggRawRow row;
      if (!r.ReadString(row.doc_id, kMaxErrorMsgBytes)) return false;
      if (!r.ReadBool(row.has_knn_dist)) return false;
      if (row.has_knn_dist && !r.ReadDouble(row.knn_dist)) return false;
      uint64_t fcount = 0;
      if (!r.ReadVarUint(fcount) || fcount > 65536 || fcount > r.Remaining()) return false;
      row.fields.reserve(static_cast<size_t>(fcount));
      for (uint64_t f = 0; f < fcount; f++) {
        std::string k, v;
        if (!r.ReadString(k, kMaxErrorMsgBytes) || !r.ReadString(v, kMaxRpcPayloadBytes)) {
          return false;
        }
        row.fields.emplace_back(std::move(k), std::move(v));
      }
      out.raw_rows.push_back(std::move(row));
    }
  } else {
    uint64_t gcount = 0;
    if (!r.ReadVarUint(gcount) || gcount > kMaxRpcVecItems || gcount > r.Remaining()) return false;
    out.partial_groups.reserve(static_cast<size_t>(gcount));
    for (uint64_t i = 0; i < gcount; i++) {
      PartialGroup pg;
      if (!r.ReadString(pg.group_key, kMaxRpcPayloadBytes)) return false;
      uint64_t kfcount = 0;
      if (!r.ReadVarUint(kfcount) || kfcount > 1024 || kfcount > r.Remaining()) return false;
      pg.key_fields.reserve(static_cast<size_t>(kfcount));
      for (uint64_t kf = 0; kf < kfcount; kf++) {
        std::string k, v;
        if (!r.ReadString(k, kMaxErrorMsgBytes) || !r.ReadString(v, kMaxRpcPayloadBytes)) {
          return false;
        }
        pg.key_fields.emplace_back(std::move(k), std::move(v));
      }
      uint64_t rdc = 0;
      if (!r.ReadVarUint(rdc) || rdc > 1024 || rdc > r.Remaining()) return false;
      pg.reducers.reserve(static_cast<size_t>(rdc));
      for (uint64_t ri = 0; ri < rdc; ri++) {
        PartialReducerState st;
        if (!ReadPartialReducerState(r, st)) return false;
        pg.reducers.push_back(std::move(st));
      }
      out.partial_groups.push_back(std::move(pg));
    }
  }
  return r.AtEnd();
}

void EncodeCursorRequest(const RpcCursorRequest& req, std::string& out) {
  BinaryWriter w(64);
  w.WriteU8(req.op);
  w.WriteString(req.index_name);
  w.WriteU64(req.cursor_id);
  w.WriteBool(req.has_count);
  if (req.has_count) w.WriteVarUint(req.count);
  out = w.TakeBuffer();
}

bool DecodeCursorRequest(std::string_view data, RpcCursorRequest& out) {
  out = RpcCursorRequest{};
  BinaryReader r(data);
  if (!r.ReadU8(out.op) || out.op > kRpcCursorOpDel) return false;
  if (!r.ReadString(out.index_name, kMaxErrorMsgBytes)) return false;
  if (!r.ReadU64(out.cursor_id)) return false;
  if (!r.ReadBool(out.has_count)) return false;
  if (out.has_count && !r.ReadVarUint(out.count)) return false;
  return r.AtEnd();
}

void EncodeCursorResponse(const RpcCursorResponse& resp, std::string& out) {
  BinaryWriter w(128);
  w.WriteBool(resp.ok);
  w.WriteString(resp.error_msg);
  w.WriteU64(resp.next_cursor_id);
  w.WriteVarUint(static_cast<uint64_t>(resp.rows.size()));
  for (const auto& row : resp.rows) {
    w.WriteVarUint(static_cast<uint64_t>(row.size()));
    for (const auto& [k, v] : row) {
      w.WriteString(k);
      w.WriteString(v);
    }
  }
  out = w.TakeBuffer();
}

bool DecodeCursorResponse(std::string_view data, RpcCursorResponse& out) {
  out = RpcCursorResponse{};
  BinaryReader r(data);
  if (!r.ReadBool(out.ok)) return false;
  if (!r.ReadString(out.error_msg, kMaxErrorMsgBytes)) return false;
  if (!r.ReadU64(out.next_cursor_id)) return false;
  uint64_t rcount = 0;
  if (!r.ReadVarUint(rcount) || rcount > kMaxRpcVecItems || rcount > r.Remaining()) return false;
  out.rows.reserve(static_cast<size_t>(rcount));
  for (uint64_t i = 0; i < rcount; i++) {
    uint64_t fcount = 0;
    if (!r.ReadVarUint(fcount) || fcount > 65536 || fcount > r.Remaining()) return false;
    std::vector<std::pair<std::string, std::string>> row;
    row.reserve(static_cast<size_t>(fcount));
    for (uint64_t f = 0; f < fcount; f++) {
      std::string k, v;
      if (!r.ReadString(k, kMaxErrorMsgBytes) || !r.ReadString(v, kMaxRpcPayloadBytes)) {
        return false;
      }
      row.emplace_back(std::move(k), std::move(v));
    }
    out.rows.push_back(std::move(row));
  }
  return r.AtEnd();
}

}  // namespace gemini_search
