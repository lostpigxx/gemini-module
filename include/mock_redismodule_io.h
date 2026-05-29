#pragma once

// Mock RedisModuleIO for unit-testing RDB serialization without a running
// Redis server.  Backs the standard Save*/Load* function-pointer API with
// a simple in-memory byte buffer.
//
// Usage:
//   MockRdbStream stream;
//   RedisModuleIO* rdb = stream.IO();
//   RdbSaveXxx(rdb, value);        // writes into buffer
//   stream.Rewind();
//   void* loaded = RdbLoadXxx(rdb, encver);  // reads back

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

struct MockRdbStream {
  std::vector<uint8_t> buf;
  size_t read_pos = 0;
  bool io_error = false;

  // Treat the stream struct itself as RedisModuleIO* — the mock functions
  // receive it via the opaque pointer and static_cast back.
  RedisModuleIO* IO() { return reinterpret_cast<RedisModuleIO*>(this); }

  void Rewind() { read_pos = 0; io_error = false; }
  void Clear()  { buf.clear(); read_pos = 0; io_error = false; }

  // --- write helpers ---
  void WriteBytes(const void* data, size_t len) {
    auto* p = static_cast<const uint8_t*>(data);
    buf.insert(buf.end(), p, p + len);
  }

  // --- read helpers ---
  bool ReadBytes(void* dst, size_t len) {
    if (read_pos > buf.size() || len > buf.size() - read_pos) {
      io_error = true;
      return false;
    }
    std::memcpy(dst, buf.data() + read_pos, len);
    read_pos += len;
    return true;
  }
};

static inline MockRdbStream* StreamOf(RedisModuleIO* io) {
  return reinterpret_cast<MockRdbStream*>(io);
}

// --- Mock implementations of Redis Module IO API ---

static void Mock_SaveUnsigned(RedisModuleIO* io, uint64_t v) {
  StreamOf(io)->WriteBytes(&v, sizeof(v));
}

static uint64_t Mock_LoadUnsigned(RedisModuleIO* io) {
  uint64_t v = 0;
  StreamOf(io)->ReadBytes(&v, sizeof(v));
  return v;
}

static void Mock_SaveSigned(RedisModuleIO* io, int64_t v) {
  StreamOf(io)->WriteBytes(&v, sizeof(v));
}

static int64_t Mock_LoadSigned(RedisModuleIO* io) {
  int64_t v = 0;
  StreamOf(io)->ReadBytes(&v, sizeof(v));
  return v;
}

static void Mock_SaveDouble(RedisModuleIO* io, double v) {
  StreamOf(io)->WriteBytes(&v, sizeof(v));
}

static double Mock_LoadDouble(RedisModuleIO* io) {
  double v = 0.0;
  StreamOf(io)->ReadBytes(&v, sizeof(v));
  return v;
}

static void Mock_SaveStringBuffer(RedisModuleIO* io, const char* str, size_t len) {
  // length-prefixed: write len as uint64, then raw bytes
  uint64_t n = len;
  StreamOf(io)->WriteBytes(&n, sizeof(n));
  StreamOf(io)->WriteBytes(str, len);
}

static char* Mock_LoadStringBuffer(RedisModuleIO* io, size_t* lenptr) {
  auto* stream = StreamOf(io);
  uint64_t n = 0;
  if (!stream->ReadBytes(&n, sizeof(n)) ||
      n > static_cast<uint64_t>(std::numeric_limits<size_t>::max() - 1)) {
    if (lenptr) *lenptr = 0;
    stream->io_error = true;
    return nullptr;
  }
  size_t len = static_cast<size_t>(n);
  if (lenptr) *lenptr = len;
  if (stream->read_pos > stream->buf.size() || len > stream->buf.size() - stream->read_pos) {
    stream->io_error = true;
    return nullptr;
  }
  // Allocate with malloc — matches Mock_Free below and the TESTING alloc path
  auto* buf = static_cast<char*>(std::malloc(len + 1));
  if (buf) {
    stream->ReadBytes(buf, len);
    buf[len] = '\0';
  } else {
    stream->io_error = true;
  }
  return buf;
}

static int Mock_IsIOError(RedisModuleIO* io) {
  return StreamOf(io)->io_error ? 1 : 0;
}

static void Mock_Free(void* ptr) {
  std::free(ptr);
}

// Call this once at the start of each test binary (or in a global fixture)
// to wire up the function pointers that redismodule.h declares as extern.
static inline void InstallMockRedisModuleIO() {
  RedisModule_SaveUnsigned    = Mock_SaveUnsigned;
  RedisModule_LoadUnsigned    = Mock_LoadUnsigned;
  RedisModule_SaveSigned      = Mock_SaveSigned;
  RedisModule_LoadSigned      = Mock_LoadSigned;
  RedisModule_SaveDouble      = Mock_SaveDouble;
  RedisModule_LoadDouble      = Mock_LoadDouble;
  RedisModule_SaveStringBuffer = Mock_SaveStringBuffer;
  RedisModule_LoadStringBuffer = Mock_LoadStringBuffer;
  RedisModule_IsIOError       = Mock_IsIOError;
  RedisModule_Free            = Mock_Free;
}
