#define REDISMODULE_API
extern "C" {
#include "redismodule.h"
}

#include "bloom_rdb.h"
#include "mock_redismodule_io.h"
#include "sb_chain.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <numbers>
#include <random>
#include <string>
#include <sys/resource.h>
#include <vector>

namespace {

constexpr size_t kMaxMockBlob = 1 << 20;

void SafeSaveUnsigned(RedisModuleIO* io, uint64_t v) { StreamOf(io)->WriteBytes(&v, sizeof(v)); }
uint64_t SafeLoadUnsigned(RedisModuleIO* io) {
  uint64_t v = 0;
  StreamOf(io)->ReadBytes(&v, sizeof(v));
  return v;
}
void SafeSaveDouble(RedisModuleIO* io, double v) { StreamOf(io)->WriteBytes(&v, sizeof(v)); }
double SafeLoadDouble(RedisModuleIO* io) {
  double v = 0.0;
  StreamOf(io)->ReadBytes(&v, sizeof(v));
  return v;
}
void SafeSaveStringBuffer(RedisModuleIO* io, const char* data, size_t len) {
  uint64_t n = len;
  StreamOf(io)->WriteBytes(&n, sizeof(n));
  StreamOf(io)->WriteBytes(data, len);
}
char* SafeLoadStringBuffer(RedisModuleIO* io, size_t* lenptr) {
  uint64_t n = 0;
  auto* s = StreamOf(io);
  if (!s->ReadBytes(&n, sizeof(n))) {
    if (lenptr) *lenptr = 0;
    return nullptr;
  }
  if (lenptr) *lenptr = static_cast<size_t>(n);
  if (n > kMaxMockBlob || n > s->buf.size() - s->read_pos) {
    s->error = true;
    return nullptr;
  }
  auto* out = static_cast<char*>(std::malloc(static_cast<size_t>(n) + 1));
  if (!out) {
    s->error = true;
    return nullptr;
  }
  if (!s->ReadBytes(out, static_cast<size_t>(n))) {
    std::free(out);
    return nullptr;
  }
  out[n] = '\0';
  return out;
}
int SafeIsIOError(RedisModuleIO* io) { return StreamOf(io)->error ? 1 : 0; }
void SafeFree(void* p) { std::free(p); }

void InstallSafeMockRedisModuleIO() {
  RedisModule_SaveUnsigned = SafeSaveUnsigned;
  RedisModule_LoadUnsigned = SafeLoadUnsigned;
  RedisModule_SaveDouble = SafeSaveDouble;
  RedisModule_LoadDouble = SafeLoadDouble;
  RedisModule_SaveStringBuffer = SafeSaveStringBuffer;
  RedisModule_LoadStringBuffer = SafeLoadStringBuffer;
  RedisModule_Free = SafeFree;
  RedisModule_IsIOError = SafeIsIOError;
}

template <typename T>
void AppendRaw(std::vector<uint8_t>& out, const T& v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  out.insert(out.end(), p, p + sizeof(T));
}

void AppendBlob(std::vector<uint8_t>& out, const std::vector<uint8_t>& blob) {
  uint64_t n = blob.size();
  AppendRaw(out, n);
  out.insert(out.end(), blob.begin(), blob.end());
}

std::span<const std::byte> BytesOf(const std::string& s) {
  return AsBytes(s.data(), s.size());
}

BloomFlags DefaultFlags() {
  return BloomFlags::Use64Bit | BloomFlags::NoRound;
}

ScalingBloomFilter* CreateFilter(uint64_t cap, double fp, BloomFlags flags, unsigned exp,
                                 int items) {
  auto* mem = static_cast<ScalingBloomFilter*>(std::malloc(sizeof(ScalingBloomFilter)));
  new (mem) ScalingBloomFilter(cap, fp, flags, exp);
  for (int i = 0; i < items; ++i) {
    std::string item = "fuzz:" + std::to_string(i);
    mem->Put(BytesOf(item));
  }
  return mem;
}

void DestroyFilter(ScalingBloomFilter* f) {
  if (!f) return;
  f->~ScalingBloomFilter();
  std::free(f);
}

bool ValidateLoadedFilter(const ScalingBloomFilter* f) {
  if (!f || !f->IsValid()) return false;
  if (f->NumLayers() == 0 || f->NumLayers() > 1024) return false;
  uint64_t itemSum = 0;
  for (const auto& layer : f->Layers()) {
    if (layer.itemCount > layer.bloom.GetCapacity()) return false;
    if (layer.bloom.GetCapacity() == 0) return false;
    if (layer.bloom.GetHashCount() == 0) return false;
    if (layer.bloom.GetTotalBits() == 0) return false;
    if (layer.bloom.GetLog2Bits() >= 64) return false;
    if (layer.bloom.GetLog2Bits() > 0 &&
        layer.bloom.GetTotalBits() != (1ULL << layer.bloom.GetLog2Bits())) return false;
    if (layer.bloom.GetDataSize() != (layer.bloom.GetTotalBits() + 7) / 8) return false;
    if (!std::isfinite(layer.bloom.GetFpRate()) ||
        layer.bloom.GetFpRate() <= 0.0 || layer.bloom.GetFpRate() >= 1.0) return false;
    if (!std::isfinite(layer.bloom.GetBitsPerEntry()) ||
        layer.bloom.GetBitsPerEntry() <= 0.0) return false;
    if (layer.bloom.GetBitsPerEntry() >
        static_cast<double>(std::numeric_limits<uint32_t>::max()) / std::numbers::ln2) {
      return false;
    }
    auto expectedHashes = std::max(1u, static_cast<uint32_t>(
      std::ceil(std::numbers::ln2 * layer.bloom.GetBitsPerEntry())));
    if (layer.bloom.GetHashCount() != expectedHashes) return false;
    itemSum += layer.itemCount;
  }
  return itemSum == f->TotalItems();
}

struct LayerSpec {
  uint64_t capacity = 10;
  double fpRate = 0.01;
  uint32_t hashCount = 7;
  double bitsPerEntry = 9.585058377367439;
  uint64_t totalBits = 128;
  uint8_t log2Bits = 0;
  std::vector<uint8_t> bits = std::vector<uint8_t>(16, 0);
  uint64_t itemCount = 0;
};

struct FilterSpec {
  uint64_t totalItems = 0;
  uint64_t numLayers = 1;
  unsigned flags = ToUnderlying(DefaultFlags());
  uint64_t expansion = 2;
  std::vector<LayerSpec> layers;
};

LayerSpec LayerFromFilter(const ScalingBloomFilter& filter, size_t index) {
  const auto& src = filter.Layers()[index];
  LayerSpec s;
  s.capacity = src.bloom.GetCapacity();
  s.fpRate = src.bloom.GetFpRate();
  s.hashCount = src.bloom.GetHashCount();
  s.bitsPerEntry = src.bloom.GetBitsPerEntry();
  s.totalBits = src.bloom.GetTotalBits();
  s.log2Bits = src.bloom.GetLog2Bits();
  s.bits.assign(src.bloom.GetBitArray(), src.bloom.GetBitArray() + src.bloom.GetDataSize());
  s.itemCount = src.itemCount;
  return s;
}

FilterSpec SpecFromFilter(const ScalingBloomFilter& filter) {
  FilterSpec spec;
  spec.totalItems = filter.TotalItems();
  spec.numLayers = filter.NumLayers();
  spec.flags = ToUnderlying(filter.Flags());
  spec.expansion = filter.ExpansionFactor();
  for (size_t i = 0; i < filter.NumLayers(); ++i) spec.layers.push_back(LayerFromFilter(filter, i));
  return spec;
}

std::vector<uint8_t> EncodeRdb(const FilterSpec& spec, int encver) {
  std::vector<uint8_t> out;
  AppendRaw(out, spec.totalItems);
  AppendRaw(out, spec.numLayers);
  if (encver >= kEncVerWithFlags) {
    uint64_t flags = spec.flags;
    AppendRaw(out, flags);
  }
  if (encver >= kEncVerWithExpansion) {
    AppendRaw(out, spec.expansion);
  }
  for (const auto& layer : spec.layers) {
    AppendRaw(out, layer.capacity);
    AppendRaw(out, layer.fpRate);
    uint64_t hc = layer.hashCount;
    AppendRaw(out, hc);
    AppendRaw(out, layer.bitsPerEntry);
    AppendRaw(out, layer.totalBits);
    uint64_t log2 = layer.log2Bits;
    AppendRaw(out, log2);
    AppendBlob(out, layer.bits);
    AppendRaw(out, layer.itemCount);
  }
  return out;
}

std::vector<uint8_t> EncodeWire(const FilterSpec& spec) {
  std::vector<uint8_t> out(sizeof(WireFilterHeader) + spec.layers.size() * sizeof(WireLayerMeta));
  auto* hdr = reinterpret_cast<WireFilterHeader*>(out.data());
  hdr->totalItems = spec.totalItems;
  hdr->numLayers = static_cast<uint32_t>(spec.numLayers);
  hdr->flags = spec.flags;
  hdr->expansionFactor = static_cast<uint32_t>(spec.expansion);
  auto* meta = reinterpret_cast<WireLayerMeta*>(out.data() + sizeof(WireFilterHeader));
  for (size_t i = 0; i < spec.layers.size(); ++i) {
    const auto& layer = spec.layers[i];
    meta[i].dataSize = layer.bits.size();
    meta[i].totalBits = layer.totalBits;
    meta[i].itemCount = layer.itemCount;
    meta[i].fpRate = layer.fpRate;
    meta[i].bitsPerEntry = layer.bitsPerEntry;
    meta[i].hashCount = layer.hashCount;
    meta[i].capacity = layer.capacity;
    meta[i].log2Bits = layer.log2Bits;
  }
  return out;
}

void OverwriteU64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
  if (offset + sizeof(value) > bytes.size()) return;
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

struct TryResult {
  bool accepted = false;
  bool invariantOk = false;
};

TryResult TryRdb(const std::vector<uint8_t>& bytes, int encver) {
  MockRdbStream stream;
  stream.buf = bytes;
  stream.Rewind();
  auto* f = static_cast<ScalingBloomFilter*>(RdbLoadBloom(stream.IO(), encver));
  TryResult result;
  result.accepted = f != nullptr;
  result.invariantOk = f ? ValidateLoadedFilter(f) : true;
  DestroyFilter(f);
  return result;
}

TryResult TryWire(const std::vector<uint8_t>& bytes) {
  const void* ptr = bytes.empty() ? nullptr : bytes.data();
  auto* f = DeserializeHeader(ptr, bytes.size());
  TryResult result;
  result.accepted = f != nullptr;
  result.invariantOk = f ? ValidateLoadedFilter(f) : true;
  DestroyFilter(f);
  return result;
}

struct CaseResult {
  std::string name;
  bool expectedAccept;
  bool accepted;
  bool invariantOk;
};

struct StructuredResult {
  int total = 0;
  int expectedAccept = 0;
  int expectedReject = 0;
  int falseReject = 0;
  int unsafeAccept = 0;
  int invariantViolation = 0;
  std::vector<CaseResult> interesting;
};

void RecordCase(StructuredResult& out, const std::string& name, bool expectedAccept, TryResult actual) {
  out.total++;
  if (expectedAccept) out.expectedAccept++; else out.expectedReject++;
  bool falseReject = expectedAccept && !actual.accepted;
  bool unsafeAccept = !expectedAccept && actual.accepted;
  bool invariantBad = actual.accepted && !actual.invariantOk;
  if (falseReject) out.falseReject++;
  if (unsafeAccept) out.unsafeAccept++;
  if (invariantBad) out.invariantViolation++;
  if (falseReject || unsafeAccept || invariantBad) {
    out.interesting.push_back({name, expectedAccept, actual.accepted, actual.invariantOk});
  }
}

StructuredResult RunStructuredRdb(const FilterSpec& valid) {
  StructuredResult out;
  auto add = [&](const std::string& name, FilterSpec spec, bool expectedAccept, int encver = kCurrentEncVer) {
    RecordCase(out, name, expectedAccept, TryRdb(EncodeRdb(spec, encver), encver));
  };

  add("valid_current_encver4", valid, true);
  add("valid_encver2_no_expansion", valid, true, kEncVerWithFlags);
  RecordCase(out, "unknown_encver_5", false, TryRdb(EncodeRdb(valid, kCurrentEncVer), kCurrentEncVer + 1));

  auto base = EncodeRdb(valid, kCurrentEncVer);
  for (size_t len : {size_t{0}, size_t{1}, size_t{2}, size_t{8}, size_t{16}, size_t{32}, base.size() - 1}) {
    std::vector<uint8_t> truncated(base.begin(), base.begin() + std::min(len, base.size()));
    RecordCase(out, "truncated_" + std::to_string(len), false, TryRdb(truncated, kCurrentEncVer));
  }
  auto hugeBlobLen = base;
  OverwriteU64(hugeBlobLen, 80, 3ULL * 1024 * 1024 * 1024);
  RecordCase(out, "declared_blob_length_3gb", false, TryRdb(hugeBlobLen, kCurrentEncVer));

  auto s = valid;
  s.numLayers = 0; s.layers.clear(); add("zero_layers", s, false);
  s = valid; s.numLayers = 1025; add("too_many_layers", s, false);
  s = valid; s.flags = 0x80; add("unknown_flags", s, false);
  s = valid; s.flags = ToUnderlying(BloomFlags::RawBits); add("rawbits_flag", s, false);
  s = valid; s.expansion = 0; add("scaling_expansion_zero", s, false);
  s = valid; s.totalItems += 1; add("total_items_mismatch", s, false);
  s = valid; s.layers[0].capacity = 0; add("capacity_zero", s, false);
  s = valid; s.layers[0].fpRate = std::numeric_limits<double>::quiet_NaN(); add("fp_nan", s, false);
  s = valid; s.layers[0].fpRate = std::numeric_limits<double>::infinity(); add("fp_inf", s, false);
  s = valid; s.layers[0].fpRate = 0.0; add("fp_zero", s, false);
  s = valid; s.layers[0].fpRate = 1.0; add("fp_one", s, false);
  s = valid; s.layers[0].fpRate = -0.1; add("fp_negative", s, false);
  s = valid; s.layers[0].bitsPerEntry = 0.0; add("bits_per_entry_zero", s, false);
  s = valid; s.layers[0].bitsPerEntry = std::numeric_limits<double>::infinity(); add("bits_per_entry_inf", s, false);
  s = valid; s.layers[0].bitsPerEntry = -1.0; add("bits_per_entry_negative", s, false);
  s = valid; s.layers[0].hashCount = 0; add("hash_count_zero", s, false);
  s = valid; s.layers[0].hashCount += 3; add("hash_count_inconsistent", s, false);
  s = valid; s.layers[0].totalBits = 0; s.layers[0].bits.clear(); add("total_bits_zero", s, false);
  s = valid; s.layers[0].log2Bits = 64; add("log2_bits_64", s, false);
  s = valid; s.layers[0].log2Bits = 7; s.layers[0].totalBits = 256; s.layers[0].bits.resize(32); add("log2_mismatch", s, false);
  s = valid; s.layers[0].bits.pop_back(); add("blob_short", s, false);
  s = valid; s.layers[0].bits.push_back(0); add("blob_long", s, false);
  s = valid; s.layers[0].itemCount = s.layers[0].capacity + 1; s.totalItems = s.layers[0].itemCount; add("item_count_gt_capacity", s, false);
  s = valid; s.flags = ToUnderlying(DefaultFlags() | BloomFlags::FixedSize); s.expansion = 0; add("fixed_expansion_zero", s, true);

  return out;
}

StructuredResult RunStructuredWire(const FilterSpec& valid, bool includeResourceBombs) {
  StructuredResult out;
  auto add = [&](const std::string& name, FilterSpec spec, bool expectedAccept) {
    RecordCase(out, name, expectedAccept, TryWire(EncodeWire(spec)));
  };

  add("valid_wire_header", valid, true);
  auto base = EncodeWire(valid);
  for (size_t len : {size_t{0}, size_t{1}, size_t{2}, size_t{8}, size_t{16}, base.size() - 1}) {
    std::vector<uint8_t> truncated(base.begin(), base.begin() + std::min(len, base.size()));
    RecordCase(out, "truncated_" + std::to_string(len), false, TryWire(truncated));
  }
  auto extra = base;
  extra.push_back(0);
  RecordCase(out, "extra_byte", false, TryWire(extra));

  auto s = valid;
  s.numLayers = 0; s.layers.clear(); add("zero_layers", s, false);
  s = valid; s.numLayers = 1025; add("too_many_layers", s, false);
  s = valid; s.flags = 0x80; add("unknown_flags", s, false);
  s = valid; s.flags = ToUnderlying(BloomFlags::RawBits); add("rawbits_flag", s, false);
  s = valid; s.expansion = 0; add("scaling_expansion_zero", s, false);
  s = valid; s.totalItems += 1; add("total_items_mismatch", s, false);
  s = valid; s.layers[0].capacity = 0; add("capacity_zero", s, false);
  s = valid; s.layers[0].fpRate = std::numeric_limits<double>::quiet_NaN(); add("fp_nan", s, false);
  s = valid; s.layers[0].fpRate = std::numeric_limits<double>::infinity(); add("fp_inf", s, false);
  s = valid; s.layers[0].fpRate = 0.0; add("fp_zero", s, false);
  s = valid; s.layers[0].fpRate = 1.0; add("fp_one", s, false);
  s = valid; s.layers[0].fpRate = -0.1; add("fp_negative", s, false);
  s = valid; s.layers[0].bitsPerEntry = 0.0; add("bits_per_entry_zero", s, false);
  s = valid; s.layers[0].bitsPerEntry = std::numeric_limits<double>::infinity(); add("bits_per_entry_inf", s, false);
  s = valid; s.layers[0].bitsPerEntry = -1.0; add("bits_per_entry_negative", s, false);
  s = valid; s.layers[0].hashCount = 0; add("hash_count_zero", s, false);
  s = valid; s.layers[0].hashCount += 3; add("hash_count_inconsistent", s, false);
  s = valid; s.layers[0].totalBits = 0; s.layers[0].bits.clear(); add("total_bits_zero", s, false);
  s = valid; s.layers[0].log2Bits = 64; add("log2_bits_64", s, false);
  s = valid; s.layers[0].log2Bits = 7; s.layers[0].totalBits = 256; s.layers[0].bits.resize(32); add("log2_mismatch", s, false);
  s = valid; s.layers[0].bits.pop_back(); add("data_size_short", s, false);
  s = valid; s.layers[0].bits.push_back(0); add("data_size_long", s, false);
  s = valid; s.layers[0].itemCount = s.layers[0].capacity + 1; s.totalItems = s.layers[0].itemCount; add("item_count_gt_capacity", s, false);
  s = valid; s.flags = ToUnderlying(DefaultFlags() | BloomFlags::FixedSize); s.expansion = 0; add("fixed_expansion_zero", s, true);
  if (includeResourceBombs) {
    auto large = EncodeWire(valid);
    auto* meta = reinterpret_cast<WireLayerMeta*>(large.data() + sizeof(WireFilterHeader));
    meta[0].dataSize = 3ULL * 1024 * 1024 * 1024;
    meta[0].totalBits = meta[0].dataSize * 8;
    RecordCase(out, "large_3gb_single_layer_allocation", false, TryWire(large));
  }

  return out;
}

struct RandomResult {
  uint64_t cases = 0;
  uint64_t accepted = 0;
  uint64_t invariantViolation = 0;
};

std::vector<uint8_t> Mutate(std::vector<uint8_t> bytes, std::mt19937_64& rng) {
  if (bytes.empty()) return bytes;
  std::uniform_int_distribution<size_t> posDist(0, bytes.size() - 1);
  std::uniform_int_distribution<int> byteDist(0, 255);
  int flips = 1 + static_cast<int>(rng() % 8);
  for (int i = 0; i < flips; ++i) bytes[posDist(rng)] = static_cast<uint8_t>(byteDist(rng));
  return bytes;
}

std::vector<uint8_t> RandomBytes(std::mt19937_64& rng, size_t maxLen) {
  std::uniform_int_distribution<size_t> lenDist(0, maxLen);
  std::uniform_int_distribution<int> byteDist(0, 255);
  std::vector<uint8_t> bytes(lenDist(rng));
  for (auto& b : bytes) b = static_cast<uint8_t>(byteDist(rng));
  return bytes;
}

RandomResult RunRandomRdb(const std::vector<uint8_t>& valid, uint64_t iterations, uint64_t seed) {
  std::mt19937_64 rng(seed);
  RandomResult out;
  for (uint64_t i = 0; i < iterations; ++i) {
    auto bytes = (i % 2 == 0) ? Mutate(valid, rng) : RandomBytes(rng, 512);
    int encver = static_cast<int>(rng() % 7);
    auto r = TryRdb(bytes, encver);
    out.cases++;
    if (r.accepted) out.accepted++;
    if (r.accepted && !r.invariantOk) out.invariantViolation++;
  }
  return out;
}

RandomResult RunRandomWire(const std::vector<uint8_t>& valid, uint64_t iterations, uint64_t seed) {
  std::mt19937_64 rng(seed);
  RandomResult out;
  for (uint64_t i = 0; i < iterations; ++i) {
    auto bytes = (i % 2 == 0) ? Mutate(valid, rng) : RandomBytes(rng, 512);
    auto r = TryWire(bytes);
    out.cases++;
    if (r.accepted) out.accepted++;
    if (r.accepted && !r.invariantOk) out.invariantViolation++;
  }
  return out;
}

void PrintStructured(std::ostream& os, const StructuredResult& r) {
  os << "{\"total\":" << r.total
     << ",\"expected_accept\":" << r.expectedAccept
     << ",\"expected_reject\":" << r.expectedReject
     << ",\"false_reject\":" << r.falseReject
     << ",\"unsafe_accept\":" << r.unsafeAccept
     << ",\"invariant_violation\":" << r.invariantViolation
     << ",\"interesting\":[";
  for (size_t i = 0; i < r.interesting.size(); ++i) {
    const auto& c = r.interesting[i];
    if (i) os << ",";
    os << "{\"name\":\"" << c.name << "\",\"expected_accept\":"
       << (c.expectedAccept ? "true" : "false")
       << ",\"accepted\":" << (c.accepted ? "true" : "false")
       << ",\"invariant_ok\":" << (c.invariantOk ? "true" : "false") << "}";
  }
  os << "]}";
}

void PrintRandom(std::ostream& os, const RandomResult& r) {
  os << "{\"cases\":" << r.cases
     << ",\"accepted\":" << r.accepted
     << ",\"invariant_violation\":" << r.invariantViolation << "}";
}

void SetMemoryLimitIfRequested(bool enabled) {
  if (!enabled) return;
  rlimit lim{};
  lim.rlim_cur = 768ULL * 1024 * 1024;
  lim.rlim_max = 768ULL * 1024 * 1024;
  setrlimit(RLIMIT_AS, &lim);
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t iterations = 100000;
  uint64_t seed = 0x2420b1006ULL;
  std::string output;
  bool memoryLimit = false;
  bool includeResourceBombs = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--iterations" && i + 1 < argc) iterations = std::strtoull(argv[++i], nullptr, 10);
    else if (arg == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 0);
    else if (arg == "--output" && i + 1 < argc) output = argv[++i];
    else if (arg == "--memory-limit") memoryLimit = true;
    else if (arg == "--include-resource-bombs") includeResourceBombs = true;
    else {
      std::cerr << "unknown arg: " << arg << "\n";
      return 2;
    }
  }

  SetMemoryLimitIfRequested(memoryLimit);
  InstallSafeMockRedisModuleIO();

  auto* filter = CreateFilter(10, 0.01, DefaultFlags(), 2, 8);
  auto valid = SpecFromFilter(*filter);
  auto validRdb = EncodeRdb(valid, kCurrentEncVer);
  auto validWire = EncodeWire(valid);

  auto structuredRdb = RunStructuredRdb(valid);
  auto structuredWire = RunStructuredWire(valid, includeResourceBombs);
  auto randomRdb = RunRandomRdb(validRdb, iterations, seed ^ 0x1234);
  auto randomWire = RunRandomWire(validWire, iterations, seed ^ 0x5678);

  DestroyFilter(filter);

  std::ofstream file;
  std::ostream* os = &std::cout;
  if (!output.empty()) {
    file.open(output);
    os = &file;
  }

  *os << "{\n";
  *os << "  \"seed\":" << seed << ",\n";
  *os << "  \"iterations_per_decoder\":" << iterations << ",\n";
  *os << "  \"memory_limit_enabled\":" << (memoryLimit ? "true" : "false") << ",\n";
  *os << "  \"resource_bombs_enabled\":" << (includeResourceBombs ? "true" : "false") << ",\n";
  *os << "  \"rdb_structured\":";
  PrintStructured(*os, structuredRdb);
  *os << ",\n  \"wire_structured\":";
  PrintStructured(*os, structuredWire);
  *os << ",\n  \"rdb_random\":";
  PrintRandom(*os, randomRdb);
  *os << ",\n  \"wire_random\":";
  PrintRandom(*os, randomWire);
  *os << "\n}\n";

  return 0;
}
