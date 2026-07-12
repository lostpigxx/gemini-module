#define REDISMODULE_API
extern "C" {
#include "redismodule.h"
}

#include "sb_chain.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

namespace {

constexpr size_t kMaxInputSize = 1U << 20;
constexpr uint64_t kMaxFuzzAllocation = 1U << 20;

void DestroyFilter(ScalingBloomFilter* filter) {
  if (!filter) return;
  filter->~ScalingBloomFilter();
  free(filter);
}

bool IsWithinAllocationBudget(const uint8_t* data, size_t size) {
  if (size < sizeof(WireFilterHeader)) return true;

  WireFilterHeader header{};
  std::memcpy(&header, data, sizeof(header));
  if (header.numLayers == 0 || header.numLayers > 1024) return true;

  const size_t required = sizeof(WireFilterHeader) +
                          static_cast<size_t>(header.numLayers) * sizeof(WireLayerMeta);
  if (size != required) return true;

  uint64_t total = 0;
  for (uint32_t i = 0; i < header.numLayers; ++i) {
    WireLayerMeta meta{};
    const size_t offset = sizeof(WireFilterHeader) +
                          static_cast<size_t>(i) * sizeof(WireLayerMeta);
    std::memcpy(&meta, data + offset, sizeof(meta));
    if (meta.dataSize > kMaxFuzzAllocation ||
        total > kMaxFuzzAllocation - meta.dataSize) {
      return false;
    }
    total += meta.dataSize;
  }
  return true;
}

void FuzzWireHeader(const uint8_t* data, size_t size) {
  if (!IsWithinAllocationBudget(data, size)) return;
  auto* filter = DeserializeHeader(data, size);
  if (filter) {
    (void)filter->Contains(AsBytes(data, size));
    (void)filter->BytesUsed();
  }
  DestroyFilter(filter);
}

void FuzzStructuredFilter(const uint8_t* data, size_t size) {
  if (size == 0) return;

  static constexpr double kRates[] = {0.1, 0.01, 0.001, 0.0001};
  const uint64_t capacity = 1 + data[0] % 128;
  const double rate = kRates[size > 1 ? data[1] % std::size(kRates) : 0];
  BloomFlags flags = BloomFlags::Use64Bit | BloomFlags::NoRound;
  if (size > 2 && (data[2] & 1U)) flags = flags | BloomFlags::FixedSize;
  const unsigned expansion = 1 + (size > 3 ? data[3] % 4 : 1);

  ScalingBloomFilter filter(capacity, rate, flags, expansion);
  if (!filter.IsValid()) return;

  size_t cursor = std::min<size_t>(4, size);
  while (cursor < size) {
    const size_t item_size = std::min<size_t>(1 + data[cursor] % 32, size - cursor - 1);
    ++cursor;
    if (item_size == 0) break;
    (void)filter.Put(AsBytes(data + cursor, item_size));
    (void)filter.Contains(AsBytes(data + cursor, item_size));
    cursor += item_size;
  }

  std::vector<uint8_t> header(ComputeHeaderSize(filter));
  SerializeHeader(filter, header.data());
  FuzzWireHeader(header.data(), header.size());

  if (size > 4 && !header.empty()) {
    const size_t offset = data[4] % header.size();
    header[offset] ^= data[size - 1];
    FuzzWireHeader(header.data(), header.size());
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > kMaxInputSize) return 0;
  FuzzWireHeader(data, size);
  FuzzStructuredFilter(data, size);
  return 0;
}
