#include "json_serialize.h"

#include <charconv>
#include <cmath>
#include <cstring>

namespace {

// 0 = copy directly, non-zero = needs escaping:
// 1 = control char (\u00xx), otherwise the escape letter ('"', '\\', 'b', 'f', 'n', 'r', 't')
static constexpr uint8_t kEscapeTable[256] = {
  1,1,1,1,1,1,1,1, 'b','t','n',1,'f','r',1,1,
  1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
  0,0,'"',0,0,0,0,0, 0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0, 0,0,0,0,'\\',0,0,0,
  // rest are 0
};

static constexpr char kHexChars[16] = {
  '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'
};

struct Writer {
  std::string& out;
  const SerializeOptions& opts;
  int depth = 0;
  size_t indent_len = 0;
  size_t newline_len = 0;
  size_t space_len = 0;

  Writer(std::string& o, const SerializeOptions& op)
      : out(o),
        opts(op),
        indent_len(op.indent ? std::strlen(op.indent) : 0),
        newline_len(op.newline ? std::strlen(op.newline) : 0),
        space_len(op.space ? std::strlen(op.space) : 0) {}

  inline void WriteIndent() {
    if (newline_len > 0) out.append(opts.newline, newline_len);
    if (indent_len > 0) {
      for (int i = 0; i < depth; i++) out.append(opts.indent, indent_len);
    }
  }

  inline void WriteColon() {
    out.push_back(':');
    if (space_len > 0) out.append(opts.space, space_len);
  }

  void WriteString(const char* data, uint32_t len) {
    out.push_back('"');
    uint32_t chunk_start = 0;
    for (uint32_t i = 0; i < len; i++) {
      uint8_t esc = kEscapeTable[static_cast<uint8_t>(data[i])];
      if (esc == 0) continue;

      if (i > chunk_start) {
        out.append(data + chunk_start, i - chunk_start);
      }
      if (esc == 1) {
        auto c = static_cast<uint8_t>(data[i]);
        char buf[6] = {'\\', 'u', '0', '0', kHexChars[c >> 4], kHexChars[c & 0x0F]};
        out.append(buf, 6);
      } else {
        char buf[2] = {'\\', static_cast<char>(esc)};
        out.append(buf, 2);
      }
      chunk_start = i + 1;
    }
    if (len > chunk_start) {
      out.append(data + chunk_start, len - chunk_start);
    }
    out.push_back('"');
  }

  void WriteValue(const JsonValue* v) {
    if (!v || depth > kMaxJsonDepth) { out.append("null", 4); return; }

    switch (v->Type()) {
      case JsonType::kNull:
        out.append("null", 4);
        break;

      case JsonType::kBool:
        if (v->GetBool()) out.append("true", 4);
        else out.append("false", 5);
        break;

      case JsonType::kInteger: {
        char buf[32];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v->GetInteger());
        out.append(buf, static_cast<size_t>(ptr - buf));
        break;
      }

      case JsonType::kNumber: {
        double d = v->GetNumber();
        if (std::isinf(d) || std::isnan(d)) {
          out.append("null", 4);
          break;
        }
        char buf[64];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), d);
        out.append(buf, static_cast<size_t>(ptr - buf));
        break;
      }

      case JsonType::kString: {
        auto sv = v->GetString();
        WriteString(sv.data(), static_cast<uint32_t>(sv.size()));
        break;
      }

      case JsonType::kArray: {
        uint32_t len = v->ArrayLen();
        if (len == 0) { out.append("[]", 2); break; }
        out.push_back('[');
        depth++;
        for (uint32_t i = 0; i < len; i++) {
          if (i > 0) out.push_back(',');
          WriteIndent();
          WriteValue(v->ArrayGet(i));
        }
        depth--;
        WriteIndent();
        out.push_back(']');
        break;
      }

      case JsonType::kObject: {
        uint32_t len = v->ObjectLen();
        if (len == 0) { out.append("{}", 2); break; }
        out.push_back('{');
        depth++;
        auto* entries = v->ObjectEntries();
        for (uint32_t i = 0; i < len; i++) {
          if (i > 0) out.push_back(',');
          WriteIndent();
          WriteString(entries[i].key, entries[i].key_len);
          WriteColon();
          WriteValue(entries[i].value);
        }
        depth--;
        WriteIndent();
        out.push_back('}');
        break;
      }
    }
  }

  void WriteArraySlice(const JsonValue* const* items, size_t count) {
    if (count == 0) { out.append("[]", 2); return; }
    out.push_back('[');
    depth++;
    for (size_t i = 0; i < count; i++) {
      if (i > 0) out.push_back(',');
      WriteIndent();
      WriteValue(items[i]);
    }
    depth--;
    WriteIndent();
    out.push_back(']');
  }
};

static size_t EstimateReserve(const JsonValue* v) {
  if (!v) return 8;
  switch (v->Type()) {
    case JsonType::kString:
      return v->GetString().size() + 4;
    case JsonType::kArray:
      return static_cast<size_t>(v->ArrayLen()) * 16 + 16;
    case JsonType::kObject:
      return static_cast<size_t>(v->ObjectLen()) * 32 + 16;
    default:
      return 32;
  }
}

} // namespace

std::string JsonSerialize(const JsonValue* value, const SerializeOptions& opts) {
  std::string result;
  result.reserve(EstimateReserve(value));
  Writer w{result, opts};
  w.WriteValue(value);
  return result;
}

std::string JsonSerializeArray(const JsonValue* const* items, size_t count,
                               const SerializeOptions& opts) {
  std::string result;
  size_t est = 2;
  for (size_t i = 0; i < count && i < 16; i++) est += EstimateReserve(items[i]);
  if (count > 16) est = (est / 16) * count;
  result.reserve(est);
  Writer w{result, opts};
  w.WriteArraySlice(items, count);
  return result;
}

std::string JsonSerializeObjectOfArrays(const NamedMatchList* lists, size_t count,
                                        const SerializeOptions& opts) {
  std::string result;
  result.reserve(count * 64 + 16);
  Writer w{result, opts};
  if (count == 0) {
    result.append("{}", 2);
    return result;
  }
  result.push_back('{');
  w.depth++;
  for (size_t i = 0; i < count; i++) {
    if (i > 0) result.push_back(',');
    w.WriteIndent();
    w.WriteString(lists[i].name.data(), static_cast<uint32_t>(lists[i].name.size()));
    w.WriteColon();
    w.WriteArraySlice(lists[i].items, lists[i].count);
  }
  w.depth--;
  w.WriteIndent();
  result.push_back('}');
  return result;
}
