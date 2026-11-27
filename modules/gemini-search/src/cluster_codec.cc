#include "cluster_codec.h"

#include <cmath>
#include <cstring>

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
  static uint32_t table[256];
  static bool initialized = false;
  if (!initialized) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int bit = 0; bit < 8; bit++) {
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    initialized = true;
  }
  return table;
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

}  // namespace gemini_search
