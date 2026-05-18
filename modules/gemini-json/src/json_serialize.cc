#include "json_serialize.h"

#include <charconv>
#include <cinttypes>
#include <cmath>
#include <cstdio>

namespace {

struct Writer {
  std::string& out;
  const SerializeOptions& opts;
  int depth = 0;

  bool Pretty() const { return opts.indent != nullptr; }

  void WriteIndent() {
    if (!Pretty()) return;
    if (opts.newline) out += opts.newline;
    for (int i = 0; i < depth; i++) out += opts.indent;
  }

  void WriteNewline() {
    if (Pretty() && opts.newline) out += opts.newline;
  }

  void WriteColon() {
    out += ':';
    if (Pretty() && opts.space) out += opts.space;
  }

  void WriteString(const char* data, uint32_t len) {
    out += '"';
    for (uint32_t i = 0; i < len; i++) {
      auto c = static_cast<unsigned char>(data[i]);
      switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
          } else {
            out += static_cast<char>(c);
          }
          break;
      }
    }
    out += '"';
  }

  void WriteValue(const JsonValue* v) {
    if (!v) { out += "null"; return; }

    switch (v->Type()) {
      case JsonType::kNull:
        out += "null";
        break;

      case JsonType::kBool:
        out += v->GetBool() ? "true" : "false";
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
          out += "null";
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
        if (len == 0) { out += "[]"; break; }
        out += '[';
        depth++;
        for (uint32_t i = 0; i < len; i++) {
          if (i > 0) out += ',';
          WriteIndent();
          WriteValue(v->ArrayGet(i));
        }
        depth--;
        WriteIndent();
        out += ']';
        break;
      }

      case JsonType::kObject: {
        uint32_t len = v->ObjectLen();
        if (len == 0) { out += "{}"; break; }
        out += '{';
        depth++;
        auto* entries = v->ObjectEntries();
        for (uint32_t i = 0; i < len; i++) {
          if (i > 0) out += ',';
          WriteIndent();
          WriteString(entries[i].key, entries[i].key_len);
          WriteColon();
          WriteValue(entries[i].value);
        }
        depth--;
        WriteIndent();
        out += '}';
        break;
      }
    }
  }
};

} // namespace

std::string JsonSerialize(const JsonValue* value, const SerializeOptions& opts) {
  std::string result;
  Writer w{result, opts, 0};
  w.WriteValue(value);
  return result;
}
