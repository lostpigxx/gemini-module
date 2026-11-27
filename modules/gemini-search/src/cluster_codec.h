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

// Standard Redis Cluster CRC16-CCITT hash slot (0..16383) with {hashtag} support
uint16_t ComputeClusterHashSlot(std::string_view key);

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

// Phase 68: FT.SEARCH Query-Then-Fetch payloads
struct RpcSearchNumericFilter {
  std::string field;
  double min = 0.0;
  double max = 0.0;
  bool min_exclusive = false;
  bool max_exclusive = false;
};

// Phase 69: Per-field BM25 term stats for 2-Phase DFS exact global scoring
struct RpcFieldBm25Stats {
  std::string field_name;
  uint64_t live_docs = 0;
  uint64_t total_doc_len = 0;
  std::vector<std::pair<std::string, uint64_t>> term_df;
};

struct RpcSearchPhase1Request {
  std::string index_name;
  std::string query_str;
  std::vector<std::pair<std::string, std::string>> params;
  int64_t remaining_timeout_ms = 5000;
  uint64_t limit_k = 0;
  bool count_only = false;
  bool withscores = false;
  bool verbatim = false;
  bool nostopwords = false;
  bool inorder = false;
  int32_t slop = 0;
  int32_t dialect = 2;
  uint8_t scorer = 0;
  bool has_language = false;
  std::string language;
  std::vector<std::string> infields;
  bool has_inkeys = false;
  std::vector<std::string> inkeys;
  bool has_sortby = false;
  std::string sortby_field;
  bool sortby_desc = false;
  std::vector<RpcSearchNumericFilter> numeric_filters;
  bool has_geofilter = false;
  std::string geofilter_field;
  double geofilter_lon = 0.0;
  double geofilter_lat = 0.0;
  double geofilter_radius = 0.0;
  uint8_t geofilter_unit = 0;
  // Phase 69: Optional global BM25 statistics (non-empty when CLUSTER_BM25_DFS == 1)
  std::vector<RpcFieldBm25Stats> global_bm25_stats;
};

struct RpcSearchPhase1Item {
  std::string doc_id;
  double score = 0.0;
  double sort_num = 0.0;
  std::string sort_str;
  bool has_yield_dist = false;
};

struct RpcSearchPhase1Response {
  bool ok = false;
  std::string error_msg;
  uint64_t total_hits = 0;
  uint8_t sort_mode = 0;
  bool is_knn = false;
  std::string score_alias;
  std::vector<RpcSearchPhase1Item> items;
};

struct RpcFetchDocsRequest {
  std::string index_name;
  std::vector<std::string> doc_ids;
  bool nocontent = false;
  bool withpayloads = false;
  bool has_return = false;
  std::vector<std::string> return_fields;
  std::vector<std::pair<std::string, std::string>> return_aliases;
  bool has_highlight = false;
  std::vector<std::string> highlight_fields;
  std::string highlight_open = "<b>";
  std::string highlight_close = "</b>";
  bool has_summarize = false;
  std::vector<std::string> summarize_fields;
  int32_t summarize_frags = 3;
  int32_t summarize_len = 20;
  std::string summarize_separator = "...";
  std::vector<std::string> query_terms;
};

struct RpcFetchedDoc {
  std::string doc_id;
  bool found = false;
  bool has_payload = false;
  std::string payload;
  std::vector<std::pair<std::string, std::string>> fields;
};

struct RpcFetchDocsResponse {
  bool ok = false;
  std::string error_msg;
  std::vector<RpcFetchedDoc> docs;
};

void EncodeSearchPhase1Request(const RpcSearchPhase1Request& req, std::string& out);
bool DecodeSearchPhase1Request(std::string_view data, RpcSearchPhase1Request& out);
void EncodeSearchPhase1Response(const RpcSearchPhase1Response& resp, std::string& out);
bool DecodeSearchPhase1Response(std::string_view data, RpcSearchPhase1Response& out);
void EncodeFetchDocsRequest(const RpcFetchDocsRequest& req, std::string& out);
bool DecodeFetchDocsRequest(std::string_view data, RpcFetchDocsRequest& out);
void EncodeFetchDocsResponse(const RpcFetchDocsResponse& resp, std::string& out);
bool DecodeFetchDocsResponse(std::string_view data, RpcFetchDocsResponse& out);

// Phase 69: 2-Phase BM25 DFS term stats payloads
struct RpcDfsTermStatsRequest {
  std::string index_name;
  std::string query_str;
  bool verbatim = false;
  bool nostopwords = false;
  int32_t dialect = 2;
  bool has_language = false;
  std::string language;
  std::vector<std::string> infields;
};

struct RpcDfsTermStatsResponse {
  bool ok = false;
  std::string error_msg;
  std::vector<RpcFieldBm25Stats> fields;
};

void EncodeDfsTermStatsRequest(const RpcDfsTermStatsRequest& req, std::string& out);
bool DecodeDfsTermStatsRequest(std::string_view data, RpcDfsTermStatsRequest& out);
void EncodeDfsTermStatsResponse(const RpcDfsTermStatsResponse& resp, std::string& out);
bool DecodeDfsTermStatsResponse(std::string_view data, RpcDfsTermStatsResponse& out);

// Phase 69: FT.HYBRID Phase 1 payloads (raw text leg + raw vector leg)
struct RpcHybridPhase1Request {
  std::string index_name;
  std::string text_query;
  uint8_t scorer = 0;
  int64_t text_limit = -1;
  std::string vec_field;
  std::string vec_param;
  uint64_t knn_k = 10;
  uint64_t ef_runtime = 0;
  uint64_t oversample = 0;
  std::string vec_filter_query;
  std::vector<std::pair<std::string, std::string>> params;
  int32_t dialect = 2;
  int64_t remaining_timeout_ms = 5000;
  std::vector<RpcFieldBm25Stats> global_bm25_stats;
};

struct RpcHybridPhase1Response {
  bool ok = false;
  std::string error_msg;
  std::vector<std::pair<std::string, double>> text_hits;
  std::vector<std::pair<std::string, float>> vec_hits;
};

void EncodeHybridPhase1Request(const RpcHybridPhase1Request& req, std::string& out);
bool DecodeHybridPhase1Request(std::string_view data, RpcHybridPhase1Request& out);
void EncodeHybridPhase1Response(const RpcHybridPhase1Response& resp, std::string& out);
bool DecodeHybridPhase1Response(std::string_view data, RpcHybridPhase1Response& out);

// Phase 69: FT.INFO cluster stats payloads
struct RpcInfoStatsRequest {
  std::string index_name;
};

struct RpcInfoStatsResponse {
  bool ok = false;
  std::string error_msg;
  uint64_t num_docs = 0;
  uint64_t num_terms = 0;
  uint64_t inverted_bytes = 0;
  uint64_t vector_bytes = 0;
  bool bg_indexing = false;
  uint64_t bg_scanned_keys = 0;
  uint64_t bg_total_keys = 0;
  double percent_indexed = 1.0;
  uint64_t cursor_idle = 0;
  uint64_t cursor_total_created = 0;
  uint64_t cursor_total_expired = 0;
};

void EncodeInfoStatsRequest(const RpcInfoStatsRequest& req, std::string& out);
bool DecodeInfoStatsRequest(std::string_view data, RpcInfoStatsRequest& out);
void EncodeInfoStatsResponse(const RpcInfoStatsResponse& resp, std::string& out);
bool DecodeInfoStatsResponse(std::string_view data, RpcInfoStatsResponse& out);

// Phase 69: FT.TAGVALS payloads
struct RpcTagValsRequest {
  std::string index_name;
  std::string field_name;
};

struct RpcTagValsResponse {
  bool ok = false;
  std::string error_msg;
  std::vector<std::string> tags;
};

void EncodeTagValsRequest(const RpcTagValsRequest& req, std::string& out);
bool DecodeTagValsRequest(std::string_view data, RpcTagValsRequest& out);
void EncodeTagValsResponse(const RpcTagValsResponse& resp, std::string& out);
bool DecodeTagValsResponse(std::string_view data, RpcTagValsResponse& out);

// Phase 69: FT.SPELLCHECK payloads
struct RpcSpellCheckRequest {
  std::string index_name;
  std::string query_str;
  int32_t max_dist = 1;
  std::vector<std::string> include_dicts;
  std::vector<std::string> exclude_dicts;
};

struct RpcSpellCheckTermSugs {
  std::string term;
  std::vector<std::pair<std::string, double>> suggestions;
};

struct RpcSpellCheckResponse {
  bool ok = false;
  std::string error_msg;
  std::vector<std::string> known_terms;
  std::vector<RpcSpellCheckTermSugs> term_suggestions;
};

void EncodeSpellCheckRequest(const RpcSpellCheckRequest& req, std::string& out);
bool DecodeSpellCheckRequest(std::string_view data, RpcSpellCheckRequest& out);
void EncodeSpellCheckResponse(const RpcSpellCheckResponse& resp, std::string& out);
bool DecodeSpellCheckResponse(std::string_view data, RpcSpellCheckResponse& out);

// Phase 69: FT.SUG* routing payloads
struct RpcSuggestRequest {
  std::string cmd_name;
  std::vector<std::string> args;
};

struct RpcSuggestEntry {
  std::string str;
  double score = 0.0;
  std::string payload;
};

struct RpcSuggestResponse {
  bool ok = false;
  std::string error_msg;
  int64_t int_reply = 0;
  std::vector<RpcSuggestEntry> entries;
};

void EncodeSuggestRequest(const RpcSuggestRequest& req, std::string& out);
bool DecodeSuggestRequest(std::string_view data, RpcSuggestRequest& out);
void EncodeSuggestResponse(const RpcSuggestResponse& resp, std::string& out);
bool DecodeSuggestResponse(std::string_view data, RpcSuggestResponse& out);

}  // namespace gemini_search
