#include "json_parse.h"
#include "json_simd.h"
#include "rm_alloc.h"

#include <cerrno>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

// Lookup table for fast whitespace check
static constexpr uint8_t kWsTable[256] = {
  0,0,0,0,0,0,0,0, 0,1,1,0,0,1,0,0,  // 0x09='\t', 0x0A='\n', 0x0D='\r'
  0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
  1,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,  // 0x20=' '
};

// Lookup table for hex digits (0..15, or 0xFF for invalid)
static constexpr uint8_t kHexVal[256] = {
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
     0,   1,   2,   3,   4,   5,   6,   7,   8,   9,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,  10,  11,  12,  13,  14,  15,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,  10,  11,  12,  13,  14,  15,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
};

// Stack-backed small buffer for escaped strings (avoids heap alloc for <= 256B)
struct SmallBuf {
  char inline_buf[256];
  char* data = inline_buf;
  size_t len = 0;
  size_t cap = sizeof(inline_buf);

  ~SmallBuf() {
    if (data != inline_buf) RMFree(data);
  }

  bool Reserve(size_t needed) {
    if (needed <= cap) return true;
    size_t nc = cap * 2;
    while (nc < needed) nc *= 2;
    if (data == inline_buf) {
      auto* nd = static_cast<char*>(RMAlloc(nc));
      if (!nd) return false;
      if (len > 0) std::memcpy(nd, inline_buf, len);
      data = nd;
    } else {
      auto* nd = static_cast<char*>(RMRealloc(data, nc));
      if (!nd) return false;
      data = nd;
    }
    cap = nc;
    return true;
  }

  bool Push(char c) {
    if (len == cap && !Reserve(len + 1)) return false;
    data[len++] = c;
    return true;
  }

  bool PushN(const char* s, size_t n) {
    if (n == 0) return true;
    if (len + n > cap && !Reserve(len + n)) return false;
    std::memcpy(data + len, s, n);
    len += n;
    return true;
  }
};

struct Parser {
  const char* pos;
  const char* end;
  const char* start;
  const char* error;
  int depth = 0;

  inline void SkipWhitespace() {
    while (pos < end && kWsTable[static_cast<uint8_t>(*pos)]) {
      pos++;
    }
  }

  inline bool HasMore() const { return pos < end; }
  inline char Peek() const { return *pos; }
  inline char Advance() { return *pos++; }

  inline bool Match(char c) {
    if (pos < end && *pos == c) { pos++; return true; }
    return false;
  }

  inline bool MatchLiteral(const char* lit, size_t len) {
    if (static_cast<size_t>(end - pos) >= len && std::memcmp(pos, lit, len) == 0) {
      pos += len;
      return true;
    }
    return false;
  }

  void SetError(const char* msg) { error = msg; }
  size_t Offset() const { return static_cast<size_t>(pos - start); }
};

// Validate a single UTF-8 sequence starting at `p` (where *p >= 0x80).
// Returns sequence length (2..4) on success, or 0 if invalid UTF-8.
static inline int ValidateUtf8Seq(const char* p, const char* end) {
  auto b0 = static_cast<uint8_t>(p[0]);
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    if (p + 2 > end) return 0;
    auto b1 = static_cast<uint8_t>(p[1]);
    return ((b1 & 0xC0) == 0x80) ? 2 : 0;
  }
  if (b0 >= 0xE0 && b0 <= 0xEF) {
    if (p + 3 > end) return 0;
    auto b1 = static_cast<uint8_t>(p[1]);
    auto b2 = static_cast<uint8_t>(p[2]);
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return 0;
    if (b0 == 0xE0 && b1 < 0xA0) return 0;  // overlong
    if (b0 == 0xED && b1 >= 0xA0) return 0; // UTF-16 surrogate
    return 3;
  }
  if (b0 >= 0xF0 && b0 <= 0xF4) {
    if (p + 4 > end) return 0;
    auto b1 = static_cast<uint8_t>(p[1]);
    auto b2 = static_cast<uint8_t>(p[2]);
    auto b3 = static_cast<uint8_t>(p[3]);
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) return 0;
    if (b0 == 0xF0 && b1 < 0x90) return 0;  // overlong
    if (b0 == 0xF4 && b1 >= 0x90) return 0; // > U+10FFFF
    return 4;
  }
  return 0;
}

static inline int ParseHex4(Parser& p) {
  if (p.end - p.pos < 4) return -1;
  uint8_t d0 = kHexVal[static_cast<uint8_t>(p.pos[0])];
  uint8_t d1 = kHexVal[static_cast<uint8_t>(p.pos[1])];
  uint8_t d2 = kHexVal[static_cast<uint8_t>(p.pos[2])];
  uint8_t d3 = kHexVal[static_cast<uint8_t>(p.pos[3])];
  if ((d0 | d1 | d2 | d3) & 0x80) return -1;
  p.pos += 4;
  return (d0 << 12) | (d1 << 8) | (d2 << 4) | d3;
}

static int EncodeUtf8(uint32_t cp, char* out) {
  if (cp <= 0x7F) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp <= 0x7FF) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp <= 0xFFFF) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  if (cp <= 0x10FFFF) {
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
  }
  return 0;
}

// Parse a JSON string into either a zero-copy view of the input (fast path,
// no escapes) or into `scratch` (escaped path). Sets `out_view` on success.
static bool ParseStringView(Parser& p, SmallBuf& scratch, std::string_view& out_view) {
  if (!p.Match('"')) { p.SetError("expected '\"'"); return false; }

  const char* chunk_start = p.pos;
  const char* cur = p.pos;
  const char* end = p.end;

  // Fast path: SIMD/SWAR scan unescaped string directly in input buffer
  while (cur < end) {
    cur = json_simd::FindNextSpecialInString(cur, end);
    if (cur >= end) break;
    auto c = static_cast<uint8_t>(*cur);
    if (c == '"') {
      out_view = std::string_view(chunk_start, static_cast<size_t>(cur - chunk_start));
      p.pos = cur + 1;
      return true;
    }
    if (c == '\\') {
      break;  // switch to escaped path
    }
    if (c < 0x20) {
      p.pos = cur;
      p.SetError("unescaped control character in string");
      return false;
    }
    // c >= 0x80: validate UTF-8 sequence
    int n = ValidateUtf8Seq(cur, end);
    if (n == 0) {
      p.pos = cur;
      p.SetError("invalid UTF-8 sequence in string");
      return false;
    }
    cur += n;
  }

  if (cur >= end) {
    p.pos = cur;
    p.SetError("unterminated string");
    return false;
  }

  // Escaped path: copy clean prefix scanned so far into scratch
  scratch.len = 0;
  if (!scratch.PushN(chunk_start, static_cast<size_t>(cur - chunk_start))) {
    p.SetError("allocation failure");
    return false;
  }
  p.pos = cur;

  while (p.pos < end) {
    // Scan next clean chunk via SIMD/SWAR
    const char* run_start = p.pos;
    while (p.pos < end) {
      p.pos = json_simd::FindNextSpecialInString(p.pos, end);
      if (p.pos >= end) break;
      auto c = static_cast<uint8_t>(*p.pos);
      if (c == '"' || c == '\\') break;
      if (c < 0x20) {
        p.SetError("unescaped control character in string");
        return false;
      }
      int n = ValidateUtf8Seq(p.pos, end);
      if (n == 0) {
        p.SetError("invalid UTF-8 sequence in string");
        return false;
      }
      p.pos += n;
    }

    if (p.pos > run_start) {
      if (!scratch.PushN(run_start, static_cast<size_t>(p.pos - run_start))) {
        p.SetError("allocation failure");
        return false;
      }
    }

    if (p.pos >= end) {
      p.SetError("unterminated string");
      return false;
    }

    char c = *p.pos++;
    if (c == '"') {
      out_view = std::string_view(scratch.data, scratch.len);
      return true;
    }

    // c == '\\'
    if (p.pos >= end) { p.SetError("unexpected end in string escape"); return false; }
    char esc = *p.pos++;
    bool ok = true;
    switch (esc) {
      case '"':  ok = scratch.Push('"'); break;
      case '\\': ok = scratch.Push('\\'); break;
      case '/':  ok = scratch.Push('/'); break;
      case 'b':  ok = scratch.Push('\b'); break;
      case 'f':  ok = scratch.Push('\f'); break;
      case 'n':  ok = scratch.Push('\n'); break;
      case 'r':  ok = scratch.Push('\r'); break;
      case 't':  ok = scratch.Push('\t'); break;
      case 'u': {
        int cp = ParseHex4(p);
        if (cp < 0) { p.SetError("invalid \\uXXXX escape"); return false; }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          if (p.end - p.pos < 2 || p.pos[0] != '\\' || p.pos[1] != 'u') {
            p.SetError("missing low surrogate"); return false;
          }
          p.pos += 2;
          int low = ParseHex4(p);
          if (low < 0xDC00 || low > 0xDFFF) {
            p.SetError("invalid low surrogate"); return false;
          }
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          p.SetError("unexpected low surrogate"); return false;
        }
        char utf8[4];
        int n = EncodeUtf8(static_cast<uint32_t>(cp), utf8);
        if (n == 0) { p.SetError("invalid codepoint"); return false; }
        ok = scratch.PushN(utf8, static_cast<size_t>(n));
        break;
      }
      default:
        p.SetError("invalid escape character");
        return false;
    }
    if (!ok) {
      p.SetError("allocation failure");
      return false;
    }
  }

  p.SetError("unterminated string");
  return false;
}

static JsonValue* ParseValue(Parser& p);

static JsonValue* ParseString(Parser& p) {
  SmallBuf scratch;
  std::string_view sv;
  if (!ParseStringView(p, scratch, sv)) return nullptr;
  return JsonValue::CreateString(sv.data(), static_cast<uint32_t>(sv.size()));
}

static bool ParseDoubleFast(const char* first, const char* last, double& out) {
  if constexpr (requires(const char* a, const char* b, double& d) {
                  std::from_chars(a, b, d);
                }) {
    auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc() && ptr == last && std::isfinite(out);
  } else {
    size_t len = static_cast<size_t>(last - first);
    char buf[64];
    char* heap_buf = nullptr;
    char* parse_buf = buf;
    if (len >= sizeof(buf)) {
      heap_buf = static_cast<char*>(RMAlloc(len + 1));
      if (!heap_buf) return false;
      parse_buf = heap_buf;
    }
    std::memcpy(parse_buf, first, len);
    parse_buf[len] = '\0';
    char* endptr = nullptr;
    errno = 0;
    out = std::strtod(parse_buf, &endptr);
    RMFree(heap_buf);
    return errno != ERANGE && endptr == parse_buf + len && std::isfinite(out);
  }
}

static JsonValue* ParseNumber(Parser& p) {
  const char* num_start = p.pos;
  bool neg = false;
  bool is_float = false;

  if (p.HasMore() && *p.pos == '-') { neg = true; p.pos++; }

  if (!p.HasMore() || *p.pos < '0' || *p.pos > '9') {
    p.SetError("invalid number");
    p.pos = num_start;
    return nullptr;
  }

  const char* digits_start = p.pos;
  if (*p.pos == '0') {
    p.pos++;
    if (p.HasMore() && *p.pos >= '0' && *p.pos <= '9') {
      p.SetError("leading zeros not allowed");
      p.pos = num_start;
      return nullptr;
    }
  } else {
    while (p.HasMore() && *p.pos >= '0' && *p.pos <= '9') p.pos++;
  }
  const char* digits_end = p.pos;

  if (p.HasMore() && *p.pos == '.') {
    is_float = true;
    p.pos++;
    if (!p.HasMore() || *p.pos < '0' || *p.pos > '9') {
      p.SetError("expected digit after decimal point");
      p.pos = num_start;
      return nullptr;
    }
    while (p.HasMore() && *p.pos >= '0' && *p.pos <= '9') p.pos++;
  }

  if (p.HasMore() && (*p.pos == 'e' || *p.pos == 'E')) {
    is_float = true;
    p.pos++;
    if (p.HasMore() && (*p.pos == '+' || *p.pos == '-')) p.pos++;
    if (!p.HasMore() || *p.pos < '0' || *p.pos > '9') {
      p.SetError("expected digit in exponent");
      p.pos = num_start;
      return nullptr;
    }
    while (p.HasMore() && *p.pos >= '0' && *p.pos <= '9') p.pos++;
  }

  if (!is_float) {
    size_t digit_count = static_cast<size_t>(digits_end - digits_start);
    if (digit_count <= 18) {
      uint64_t acc = 0;
      for (const char* d = digits_start; d < digits_end; d++) {
        acc = acc * 10 + static_cast<uint64_t>(*d - '0');
      }
      int64_t val = neg ? -static_cast<int64_t>(acc) : static_cast<int64_t>(acc);
      return JsonValue::CreateInteger(val);
    }
    if (digit_count == 19) {
      uint64_t acc = 0;
      for (const char* d = digits_start; d < digits_end; d++) {
        acc = acc * 10 + static_cast<uint64_t>(*d - '0');
      }
      if (!neg && acc <= static_cast<uint64_t>(INT64_MAX)) {
        return JsonValue::CreateInteger(static_cast<int64_t>(acc));
      }
      if (neg && acc <= static_cast<uint64_t>(INT64_MAX) + 1ULL) {
        if (acc == static_cast<uint64_t>(INT64_MAX) + 1ULL) {
          return JsonValue::CreateInteger(INT64_MIN);
        }
        return JsonValue::CreateInteger(-static_cast<int64_t>(acc));
      }
    }
  }

  double val = 0.0;
  if (!ParseDoubleFast(num_start, p.pos, val)) {
    p.SetError("invalid number");
    return nullptr;
  }
  return JsonValue::CreateNumber(val);
}

static JsonValue* ParseArray(Parser& p) {
  if (p.depth >= kMaxJsonDepth) {
    p.SetError("maximum nesting depth exceeded");
    return nullptr;
  }
  p.depth++;
  p.pos++; // skip '['
  p.SkipWhitespace();

  auto* arr = JsonValue::CreateArray();
  if (!arr) { p.depth--; p.SetError("allocation failure"); return nullptr; }

  if (p.HasMore() && p.Peek() == ']') {
    p.pos++;
    p.depth--;
    return arr;
  }

  while (true) {
    auto* elem = ParseValue(p);
    if (!elem) { JsonValue::Destroy(arr); p.depth--; return nullptr; }
    if (!arr->ArrayAppend(elem)) {
      JsonValue::Destroy(elem);
      JsonValue::Destroy(arr);
      p.depth--;
      p.SetError("allocation failure");
      return nullptr;
    }

    p.SkipWhitespace();
    if (p.HasMore() && p.Peek() == ',') {
      p.pos++;
      p.SkipWhitespace();
      continue;
    }
    break;
  }

  p.depth--;
  if (!p.Match(']')) {
    JsonValue::Destroy(arr);
    p.SetError("expected ']' or ','");
    return nullptr;
  }
  return arr;
}

static JsonValue* ParseObject(Parser& p) {
  if (p.depth >= kMaxJsonDepth) {
    p.SetError("maximum nesting depth exceeded");
    return nullptr;
  }
  p.depth++;
  p.pos++; // skip '{'
  p.SkipWhitespace();

  auto* obj = JsonValue::CreateObject();
  if (!obj) { p.depth--; p.SetError("allocation failure"); return nullptr; }

  if (p.HasMore() && p.Peek() == '}') {
    p.pos++;
    p.depth--;
    return obj;
  }

  SmallBuf key_scratch;
  while (true) {
    p.SkipWhitespace();
    std::string_view key_sv;
    if (!ParseStringView(p, key_scratch, key_sv)) {
      JsonValue::Destroy(obj);
      p.depth--;
      return nullptr;
    }

    p.SkipWhitespace();
    if (!p.Match(':')) {
      JsonValue::Destroy(obj);
      p.depth--;
      p.SetError("expected ':'");
      return nullptr;
    }

    auto* val = ParseValue(p);
    if (!val) {
      JsonValue::Destroy(obj);
      p.depth--;
      return nullptr;
    }

    if (!obj->ObjectSet(key_sv.data(), static_cast<uint32_t>(key_sv.size()), val)) {
      JsonValue::Destroy(val);
      JsonValue::Destroy(obj);
      p.depth--;
      p.SetError("allocation failure");
      return nullptr;
    }

    p.SkipWhitespace();
    if (p.HasMore() && p.Peek() == ',') {
      p.pos++;
      continue;
    }
    break;
  }

  p.depth--;
  if (!p.Match('}')) {
    JsonValue::Destroy(obj);
    p.SetError("expected '}' or ','");
    return nullptr;
  }
  return obj;
}

static JsonValue* ParseValue(Parser& p) {
  p.SkipWhitespace();
  if (!p.HasMore()) { p.SetError("unexpected end of input"); return nullptr; }

  char c = p.Peek();
  switch (c) {
    case 'n':
      if (p.MatchLiteral("null", 4)) return JsonValue::CreateNull();
      p.SetError("invalid token");
      return nullptr;
    case 't':
      if (p.MatchLiteral("true", 4)) return JsonValue::CreateBool(true);
      p.SetError("invalid token");
      return nullptr;
    case 'f':
      if (p.MatchLiteral("false", 5)) return JsonValue::CreateBool(false);
      p.SetError("invalid token");
      return nullptr;
    case '"':
      return ParseString(p);
    case '[':
      return ParseArray(p);
    case '{':
      return ParseObject(p);
    default:
      if (c == '-' || (c >= '0' && c <= '9'))
        return ParseNumber(p);
      p.SetError("unexpected character");
      return nullptr;
  }
}

} // namespace

ParseResult JsonParse(std::string_view input) {
  Parser p;
  p.pos = input.data();
  p.end = input.data() + input.size();
  p.start = input.data();
  p.error = nullptr;

  auto* val = ParseValue(p);
  if (!val) {
    return {nullptr, p.error ? p.error : "parse error", p.Offset()};
  }

  p.SkipWhitespace();
  if (p.HasMore()) {
    JsonValue::Destroy(val);
    return {nullptr, "trailing content after JSON value", p.Offset()};
  }

  return {val, nullptr, 0};
}
