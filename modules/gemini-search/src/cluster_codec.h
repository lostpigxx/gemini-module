#pragma once

#include "index_spec.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gemini_search {

constexpr char kRpcMagic[4] = {'G', 'M', 'S', 'C'};
constexpr uint16_t kRpcProtocolVersion = 1;
constexpr size_t kRpcHeaderBytes = 30;
constexpr uint32_t kMaxRpcPayloadBytes = 32u * 1024u * 1024u;  // 32 MiB
constexpr size_t kMaxRpcVecItems = 1000000;
constexpr size_t kMaxClusterFanout = 256;

// Cluster Bus message types (control plane only, small packets)
constexpr uint8_t kClusterMsgHello = 0x01;
constexpr char kClusterBusHelloMagic[4] = {'G', 'M', 'C', 'H'};
constexpr uint16_t kClusterBusHelloVersion = 1;
constexpr size_t kClusterBusHelloBytes = 26;

// Frame flags
constexpr uint16_t kRpcFlagNone = 0;
constexpr uint16_t kRpcFlagResponse = 1u << 0;
constexpr uint16_t kRpcFlagError = 1u << 1;
constexpr uint16_t kRpcFlagHasAuth = 1u << 2;

enum class RpcOpCode : uint16_t {
  // Health & probing
  kRpcPing = 0x0001,
  kRpcPong = 0x8001,

  // DDL & Schema Sync
  kRpcDdlCreate = 0x0010,
  kRpcDdlDrop = 0x0011,
  kRpcDdlAlter = 0x0012,
  kRpcDdlAlias = 0x0013,
  kRpcDdlSyn = 0x0014,
  kRpcDdlDict = 0x0015,
  kRpcDdlResp = 0x8010,
  kRpcSchemaSyncReq = 0x0018,
  kRpcSchemaSyncResp = 0x8018,

  // Search Query-Then-Fetch & BM25 DFS
  kRpcSearchPhase1 = 0x0020,
  kRpcSearchPhase1Resp = 0x8020,
  kRpcFetchDocs = 0x0021,
  kRpcFetchDocsResp = 0x8021,
  kRpcDfsTermStats = 0x0022,
  kRpcDfsTermStatsResp = 0x8022,

  // Hybrid Search
  kRpcHybridPhase1 = 0x0030,
  kRpcHybridPhase1Resp = 0x8030,

  // Distributed Aggregation
  kRpcAggMap = 0x0040,
  kRpcAggMapResp = 0x8040,

  // Auxiliary cluster commands
  kRpcInfoStats = 0x0050,
  kRpcInfoStatsResp = 0x8050,
  kRpcTagVals = 0x0051,
  kRpcTagValsResp = 0x8051,
  kRpcSpellCheck = 0x0052,
  kRpcSpellCheckResp = 0x8052,
  kRpcSuggest = 0x0053,
  kRpcSuggestResp = 0x8053,
  kRpcCursor = 0x0054,
  kRpcCursorResp = 0x8054,
};

bool IsKnownRpcOpCode(uint16_t raw);

enum class FrameDecodeStatus {
  kOk = 0,
  kIncomplete,
  kInvalidMagic,
  kUnsupportedVersion,
  kUnknownOpCode,
  kPayloadTooLarge,
  kHeaderCrcMismatch,
  kPayloadCrcMismatch,
};

struct RpcFrameHeader {
  uint16_t version = kRpcProtocolVersion;
  RpcOpCode opcode = RpcOpCode::kRpcPing;
  uint16_t flags = kRpcFlagNone;
  uint64_t request_id = 0;
  uint32_t payload_len = 0;
  uint32_t header_crc = 0;
  uint32_t payload_crc = 0;
};

struct RpcFrame {
  RpcFrameHeader header;
  std::string payload;
};

// Standard IEEE 802.3 CRC32
uint32_t ComputeCrc32(const uint8_t* data, size_t len);
uint32_t ComputeCrc32(std::string_view data);

// Little-endian binary writer
class BinaryWriter {
 public:
  BinaryWriter() = default;
  explicit BinaryWriter(size_t reserve_bytes);

  void Reserve(size_t bytes) { buf_.reserve(bytes); }
  void Clear() { buf_.clear(); }

  void WriteU8(uint8_t v);
  void WriteU16(uint16_t v);
  void WriteU32(uint32_t v);
  void WriteU64(uint64_t v);
  void WriteI64(int64_t v);
  void WriteDouble(double v);
  void WriteBool(bool v);
  void WriteVarUint(uint64_t v);
  void WriteVarInt(int64_t v);
  void WriteRawBytes(const void* data, size_t len);
  void WriteString(std::string_view s);
  void WriteStringVec(const std::vector<std::string>& v);

  const std::string& Buffer() const { return buf_; }
  std::string TakeBuffer() { return std::move(buf_); }
  size_t Size() const { return buf_.size(); }

 private:
  std::string buf_;
};

// Bounds-checked little-endian binary reader (exception-free)
class BinaryReader {
 public:
  BinaryReader(const uint8_t* data, size_t len) : data_(data), len_(len), pos_(0) {}
  explicit BinaryReader(std::string_view view)
      : data_(reinterpret_cast<const uint8_t*>(view.data())), len_(view.size()), pos_(0) {}

  bool ReadU8(uint8_t& out);
  bool ReadU16(uint16_t& out);
  bool ReadU32(uint32_t& out);
  bool ReadU64(uint64_t& out);
  bool ReadI64(int64_t& out);
  bool ReadDouble(double& out, bool allow_non_finite = false);
  bool ReadBool(bool& out);
  bool ReadVarUint(uint64_t& out);
  bool ReadVarInt(int64_t& out);
  bool ReadRawBytes(uint8_t* dst, size_t len);
  bool ReadRawView(size_t len, std::string_view& out);
  bool ReadString(std::string& out, size_t max_len = kMaxRpcPayloadBytes);
  bool ReadStringVec(std::vector<std::string>& out, size_t max_count = kMaxRpcVecItems,
                     size_t max_item_len = kMaxRpcPayloadBytes);

  size_t Remaining() const { return pos_ <= len_ ? (len_ - pos_) : 0; }
  size_t Offset() const { return pos_; }
  bool AtEnd() const { return pos_ == len_; }

 private:
  const uint8_t* data_ = nullptr;
  size_t len_ = 0;
  size_t pos_ = 0;
};

// Frame encoding & decoding
bool EncodeRpcFrame(RpcOpCode opcode, uint16_t flags, uint64_t request_id,
                    std::string_view payload, std::string& out);
FrameDecodeStatus DecodeRpcHeader(const uint8_t* data, size_t len, RpcFrameHeader& out);
FrameDecodeStatus DecodeRpcFrame(const uint8_t* data, size_t len, RpcFrame& out, size_t& consumed);

// Standard error payload (used when kRpcFlagError is set)
struct RpcErrorPayload {
  uint32_t code = 0;
  std::string message;
};

void EncodeErrorPayload(const RpcErrorPayload& err, std::string& out);
bool DecodeErrorPayload(std::string_view data, RpcErrorPayload& out);

// Health check PING / PONG payloads
struct RpcPingPayload {
  std::string sender_node_id;
  uint64_t timestamp_ms = 0;
  uint64_t schema_epoch = 0;
  uint32_t rpc_port = 0;
};

struct RpcPongPayload {
  std::string responder_node_id;
  uint64_t echo_timestamp_ms = 0;
  uint64_t responder_time_ms = 0;
  uint64_t schema_epoch = 0;
  uint32_t rpc_port = 0;
};

void EncodePingPayload(const RpcPingPayload& ping, std::string& out);
bool DecodePingPayload(std::string_view data, RpcPingPayload& out);
void EncodePongPayload(const RpcPongPayload& pong, std::string& out);
bool DecodePongPayload(std::string_view data, RpcPongPayload& out);

// Cluster Bus Hello control message (fixed 26 bytes with CRC32)
struct ClusterBusHello {
  uint16_t version = kClusterBusHelloVersion;
  uint32_t rpc_port = 0;
  uint64_t schema_epoch = 0;
  uint16_t protocol_ver = kRpcProtocolVersion;
  uint16_t flags = 0;
};

void EncodeClusterBusHello(const ClusterBusHello& msg, std::string& out);
bool DecodeClusterBusHello(const uint8_t* data, size_t len, ClusterBusHello& out);

// DDL RPC flags
constexpr uint32_t kDdlFlagNone = 0;
constexpr uint32_t kDdlFlagRollback = 1u << 0;

struct RpcDdlRequest {
  uint64_t schema_epoch = 0;
  std::string coordinator_node_id;
  std::string cmd_name;
  std::vector<std::string> args;
  uint32_t flags = kDdlFlagNone;
};

struct RpcDdlResponse {
  bool ok = false;
  int64_t int_reply = 0;
  uint64_t applied_epoch = 0;
  std::string error_msg;
};

void EncodeDdlRequest(const RpcDdlRequest& req, std::string& out);
bool DecodeDdlRequest(std::string_view data, RpcDdlRequest& out);
void EncodeDdlResponse(const RpcDdlResponse& resp, std::string& out);
bool DecodeDdlResponse(std::string_view data, RpcDdlResponse& out);

// Serializable snapshot of one index's metadata (IndexSpec + synonym groups)
struct SchemaIndexSnapshot {
  IndexSpec spec;
  std::vector<std::pair<std::string, std::vector<std::string>>> synonyms;
};

void WriteIndexSpec(BinaryWriter& w, const IndexSpec& spec);
bool ReadIndexSpec(BinaryReader& r, IndexSpec& out);
void EncodeSchemaIndexSnapshot(const SchemaIndexSnapshot& snap, std::string& out);
bool DecodeSchemaIndexSnapshot(std::string_view data, SchemaIndexSnapshot& out);

struct RpcSchemaSyncRequest {
  std::string requester_node_id;
  uint64_t local_schema_epoch = 0;
};

struct RpcSchemaSyncResponse {
  uint64_t schema_epoch = 0;
  std::vector<SchemaIndexSnapshot> indices;
  std::vector<std::pair<std::string, std::string>> aliases;
  std::vector<std::pair<std::string, std::vector<std::string>>> term_dicts;
};

void EncodeSchemaSyncRequest(const RpcSchemaSyncRequest& req, std::string& out);
bool DecodeSchemaSyncRequest(std::string_view data, RpcSchemaSyncRequest& out);
void EncodeSchemaSyncResponse(const RpcSchemaSyncResponse& resp, std::string& out);
bool DecodeSchemaSyncResponse(std::string_view data, RpcSchemaSyncResponse& out);

}  // namespace gemini_search
