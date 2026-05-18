#include "json_parse.h"
#include "rm_alloc.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace {

struct Parser {
  const char* pos;
  const char* end;
  const char* start;
  const char* error;

  void SkipWhitespace() {
    while (pos < end && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r'))
      pos++;
  }

  bool HasMore() const { return pos < end; }

  char Peek() const { return *pos; }

  char Advance() { return *pos++; }

  bool Match(char c) {
    if (pos < end && *pos == c) { pos++; return true; }
    return false;
  }

  bool MatchLiteral(const char* lit, size_t len) {
    if (static_cast<size_t>(end - pos) >= len && std::memcmp(pos, lit, len) == 0) {
      pos += len;
      return true;
    }
    return false;
  }

  void SetError(const char* msg) { error = msg; }
  size_t Offset() const { return static_cast<size_t>(pos - start); }
};

static int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int ParseHex4(Parser& p) {
  if (p.end - p.pos < 4) return -1;
  int val = 0;
  for (int i = 0; i < 4; i++) {
    int d = HexDigit(p.pos[i]);
    if (d < 0) return -1;
    val = (val << 4) | d;
  }
  p.pos += 4;
  return val;
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

struct GrowBuf {
  char* data = nullptr;
  size_t len = 0;
  size_t cap = 0;

  ~GrowBuf() { RMFree(data); }

  bool Push(char c) {
    if (len == cap && !Grow()) return false;
    data[len++] = c;
    return true;
  }

  bool PushN(const char* s, size_t n) {
    while (len + n > cap) { if (!Grow()) return false; }
    std::memcpy(data + len, s, n);
    len += n;
    return true;
  }

  bool Grow() {
    size_t nc = cap == 0 ? 64 : cap * 2;
    auto* nd = static_cast<char*>(RMRealloc(data, nc));
    if (!nd) return false;
    data = nd;
    cap = nc;
    return true;
  }

  char* Release() {
    char* r = data;
    data = nullptr;
    len = cap = 0;
    return r;
  }
};

static JsonValue* ParseValue(Parser& p);

static JsonValue* ParseString(Parser& p, bool as_value = true) {
  if (!p.Match('"')) { p.SetError("expected '\"'"); return nullptr; }

  GrowBuf buf;

  while (p.HasMore()) {
    char c = p.Advance();
    if (c == '"') {
      if (as_value) {
        auto* v = JsonValue::CreateString(buf.data, static_cast<uint32_t>(buf.len));
        return v;
      }
      buf.Push('\0');
      return reinterpret_cast<JsonValue*>(buf.Release());
    }
    if (c == '\\') {
      if (!p.HasMore()) { p.SetError("unexpected end in string escape"); return nullptr; }
      char esc = p.Advance();
      switch (esc) {
        case '"':  buf.Push('"'); break;
        case '\\': buf.Push('\\'); break;
        case '/':  buf.Push('/'); break;
        case 'b':  buf.Push('\b'); break;
        case 'f':  buf.Push('\f'); break;
        case 'n':  buf.Push('\n'); break;
        case 'r':  buf.Push('\r'); break;
        case 't':  buf.Push('\t'); break;
        case 'u': {
          int cp = ParseHex4(p);
          if (cp < 0) { p.SetError("invalid \\uXXXX escape"); return nullptr; }
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (p.end - p.pos < 2 || p.pos[0] != '\\' || p.pos[1] != 'u') {
              p.SetError("missing low surrogate"); return nullptr;
            }
            p.pos += 2;
            int low = ParseHex4(p);
            if (low < 0xDC00 || low > 0xDFFF) {
              p.SetError("invalid low surrogate"); return nullptr;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            p.SetError("unexpected low surrogate"); return nullptr;
          }
          char utf8[4];
          int n = EncodeUtf8(static_cast<uint32_t>(cp), utf8);
          if (n == 0) { p.SetError("invalid codepoint"); return nullptr; }
          buf.PushN(utf8, static_cast<size_t>(n));
          break;
        }
        default:
          p.SetError("invalid escape character");
          return nullptr;
      }
    } else {
      buf.Push(c);
    }
  }
  p.SetError("unterminated string");
  return nullptr;
}

struct StringKeyResult {
  char* data;
  size_t len;
};

static bool ParseStringKey(Parser& p, StringKeyResult& out) {
  auto* raw = ParseString(p, false);
  if (!raw) return false;
  out.data = reinterpret_cast<char*>(raw);
  out.len = std::strlen(out.data);
  return true;
}

static JsonValue* ParseNumber(Parser& p) {
  const char* num_start = p.pos;
  bool is_float = false;

  if (p.HasMore() && *p.pos == '-') p.pos++;

  if (!p.HasMore() || *p.pos < '0' || *p.pos > '9') {
    p.SetError("invalid number");
    p.pos = num_start;
    return nullptr;
  }

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

  size_t num_len = static_cast<size_t>(p.pos - num_start);

  if (!is_float) {
    char buf[32];
    if (num_len < sizeof(buf)) {
      std::memcpy(buf, num_start, num_len);
      buf[num_len] = '\0';
      errno = 0;
      char* endptr;
      long long val = std::strtoll(buf, &endptr, 10);
      if (errno == 0 && endptr == buf + num_len) {
        return JsonValue::CreateInteger(static_cast<int64_t>(val));
      }
    }
  }

  char buf[64];
  char* heap_buf = nullptr;
  char* parse_buf = buf;
  if (num_len >= sizeof(buf)) {
    heap_buf = static_cast<char*>(RMAlloc(num_len + 1));
    if (!heap_buf) { p.SetError("allocation failure"); return nullptr; }
    parse_buf = heap_buf;
  }
  std::memcpy(parse_buf, num_start, num_len);
  parse_buf[num_len] = '\0';

  char* endptr;
  errno = 0;
  double val = std::strtod(parse_buf, &endptr);
  RMFree(heap_buf);

  if (errno == ERANGE || endptr != parse_buf + num_len) {
    p.SetError("invalid number");
    return nullptr;
  }
  return JsonValue::CreateNumber(val);
}

static JsonValue* ParseArray(Parser& p) {
  p.pos++; // skip '['
  p.SkipWhitespace();

  auto* arr = JsonValue::CreateArray();
  if (!arr) { p.SetError("allocation failure"); return nullptr; }

  if (p.HasMore() && p.Peek() == ']') {
    p.pos++;
    return arr;
  }

  while (true) {
    auto* elem = ParseValue(p);
    if (!elem) { JsonValue::Destroy(arr); return nullptr; }
    if (!arr->ArrayAppend(elem)) {
      JsonValue::Destroy(elem);
      JsonValue::Destroy(arr);
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

  if (!p.Match(']')) {
    JsonValue::Destroy(arr);
    p.SetError("expected ']' or ','");
    return nullptr;
  }
  return arr;
}

static JsonValue* ParseObject(Parser& p) {
  p.pos++; // skip '{'
  p.SkipWhitespace();

  auto* obj = JsonValue::CreateObject();
  if (!obj) { p.SetError("allocation failure"); return nullptr; }

  if (p.HasMore() && p.Peek() == '}') {
    p.pos++;
    return obj;
  }

  while (true) {
    p.SkipWhitespace();
    StringKeyResult key;
    if (!ParseStringKey(p, key)) {
      JsonValue::Destroy(obj);
      return nullptr;
    }

    p.SkipWhitespace();
    if (!p.Match(':')) {
      RMFree(key.data);
      JsonValue::Destroy(obj);
      p.SetError("expected ':'");
      return nullptr;
    }

    auto* val = ParseValue(p);
    if (!val) {
      RMFree(key.data);
      JsonValue::Destroy(obj);
      return nullptr;
    }

    if (!obj->ObjectSet(key.data, static_cast<uint32_t>(key.len), val)) {
      JsonValue::Destroy(val);
      RMFree(key.data);
      JsonValue::Destroy(obj);
      p.SetError("allocation failure");
      return nullptr;
    }
    RMFree(key.data);

    p.SkipWhitespace();
    if (p.HasMore() && p.Peek() == ',') {
      p.pos++;
      continue;
    }
    break;
  }

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
