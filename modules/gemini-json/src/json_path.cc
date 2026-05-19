#include "json_path.h"

#include <climits>
#include <cstdlib>
#include <cstring>

namespace {

struct PathParser {
  const char* pos;
  const char* end;
  const char* error = nullptr;

  bool HasMore() const { return pos < end; }
  char Peek() const { return *pos; }
  char Advance() { return *pos++; }

  bool ParseInt(int32_t& out) {
    if (!HasMore()) return false;
    const char* start = pos;
    bool neg = false;
    if (*pos == '-') { neg = true; pos++; }
    if (!HasMore() || *pos < '0' || *pos > '9') { pos = start; return false; }
    int64_t val = 0;
    while (HasMore() && *pos >= '0' && *pos <= '9') {
      val = val * 10 + (*pos - '0');
      if (val > static_cast<int64_t>(INT32_MAX) + 1) {
        pos = start;
        return false;
      }
      pos++;
    }
    if (neg) val = -val;
    if (val < INT32_MIN || val > INT32_MAX) { pos = start; return false; }
    out = static_cast<int32_t>(val);
    return true;
  }
};

static bool ParseQuotedKey(PathParser& p, std::string& out) {
  if (!p.HasMore()) return false;
  char quote = p.Peek();
  if (quote != '\'' && quote != '"') return false;
  p.Advance();
  out.clear();
  while (p.HasMore()) {
    char c = p.Advance();
    if (c == quote) return true;
    if (c == '\\' && p.HasMore()) {
      out += p.Advance();
    } else {
      out += c;
    }
  }
  return false;
}

static bool IsIdentChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-' ||
         (static_cast<unsigned char>(c) >= 0x80);
}

static bool ParseDotKey(PathParser& p, std::string& out) {
  out.clear();
  while (p.HasMore() && IsIdentChar(p.Peek())) {
    out += p.Advance();
  }
  return !out.empty();
}

static bool ParseBracketContent(PathParser& p, std::vector<PathSegment>& segments) {
  p.Advance(); // skip '['

  if (!p.HasMore()) { p.error = "unexpected end after '['"; return false; }

  if (p.Peek() == '*') {
    p.Advance();
    if (!p.HasMore() || p.Peek() != ']') {
      p.error = "expected ']' after '*'"; return false;
    }
    p.Advance();
    segments.push_back({PathSegType::kWildcard, {}, 0, {}});
    return true;
  }

  if (p.Peek() == '\'' || p.Peek() == '"') {
    std::string key;
    if (!ParseQuotedKey(p, key)) {
      p.error = "invalid quoted key in bracket";
      return false;
    }
    if (!p.HasMore() || p.Peek() != ']') {
      p.error = "expected ']' after quoted key"; return false;
    }
    p.Advance();
    segments.push_back({PathSegType::kKey, std::move(key), 0, {}});
    return true;
  }

  // Check for slice or index
  bool has_colon = false;
  {
    const char* scan = p.pos;
    int bracket_depth = 1;
    while (scan < p.end && bracket_depth > 0) {
      if (*scan == '[') bracket_depth++;
      else if (*scan == ']') bracket_depth--;
      else if (*scan == ':' && bracket_depth == 1) has_colon = true;
      scan++;
    }
  }

  if (has_colon) {
    SliceParams slice;

    if (p.HasMore() && p.Peek() != ':') {
      if (!p.ParseInt(slice.start)) {
        p.error = "invalid slice start"; return false;
      }
      slice.start_set = true;
    }

    if (!p.HasMore() || p.Peek() != ':') {
      p.error = "expected ':' in slice"; return false;
    }
    p.Advance();

    if (p.HasMore() && p.Peek() != ':' && p.Peek() != ']') {
      if (!p.ParseInt(slice.stop)) {
        p.error = "invalid slice stop"; return false;
      }
      slice.stop_set = true;
    }

    if (p.HasMore() && p.Peek() == ':') {
      p.Advance();
      if (p.HasMore() && p.Peek() != ']') {
        if (!p.ParseInt(slice.step)) {
          p.error = "invalid slice step"; return false;
        }
      }
    }

    if (!p.HasMore() || p.Peek() != ']') {
      p.error = "expected ']' after slice"; return false;
    }
    p.Advance();
    segments.push_back({PathSegType::kSlice, {}, 0, slice});
    return true;
  }

  // Simple index
  int32_t idx;
  if (!p.ParseInt(idx)) {
    p.error = "expected integer index in brackets";
    return false;
  }
  if (!p.HasMore() || p.Peek() != ']') {
    p.error = "expected ']' after index";
    return false;
  }
  p.Advance();
  segments.push_back({PathSegType::kIndex, {}, idx, {}});
  return true;
}

// Resolve negative index relative to array length
static int32_t ResolveIndex(int32_t idx, uint32_t len) {
  if (idx < 0) idx += static_cast<int32_t>(len);
  return idx;
}

static void ResolveSlice(const SliceParams& sp, uint32_t len,
                         int32_t& start, int32_t& stop, int32_t step) {
  int32_t n = static_cast<int32_t>(len);

  if (step > 0) {
    start = sp.start_set ? sp.start : 0;
    stop = sp.stop_set ? sp.stop : n;
  } else {
    start = sp.start_set ? sp.start : n - 1;
    stop = sp.stop_set ? sp.stop : -n - 1;
  }

  if (start < 0) start += n;
  if (stop < 0) stop += n;

  if (step > 0) {
    if (start < 0) start = 0;
    if (stop > n) stop = n;
  } else {
    if (start >= n) start = n - 1;
    if (stop < -1) stop = -1;
  }
}

using MatchVec = std::vector<PathMatch>;

static void Evaluate(const JsonPath& path, size_t seg_idx,
                     JsonValue* current, JsonValue* parent,
                     int32_t arr_idx, const std::string& obj_key,
                     MatchVec& results);

static void EvalRecursive(const JsonPath& path, size_t seg_idx,
                          JsonValue* current, JsonValue* parent,
                          int32_t arr_idx, const std::string& obj_key,
                          MatchVec& results) {
  Evaluate(path, seg_idx, current, parent, arr_idx, obj_key, results);

  if (current->IsArray()) {
    for (uint32_t i = 0; i < current->ArrayLen(); i++) {
      EvalRecursive(path, seg_idx, current->ArrayGet(i), current,
                    static_cast<int32_t>(i), {}, results);
    }
  } else if (current->IsObject()) {
    auto* entries = current->ObjectEntries();
    for (uint32_t i = 0; i < current->ObjectLen(); i++) {
      EvalRecursive(path, seg_idx, entries[i].value, current, -1,
                    std::string(entries[i].key, entries[i].key_len), results);
    }
  }
}

static void Evaluate(const JsonPath& path, size_t seg_idx,
                     JsonValue* current, JsonValue* parent,
                     int32_t arr_idx, const std::string& obj_key,
                     MatchVec& results) {
  if (seg_idx >= path.segments.size()) {
    results.push_back({current, parent, arr_idx, obj_key});
    return;
  }

  const auto& seg = path.segments[seg_idx];

  switch (seg.type) {
    case PathSegType::kRoot:
      Evaluate(path, seg_idx + 1, current, nullptr, -1, {}, results);
      break;

    case PathSegType::kKey:
      if (current->IsObject()) {
        auto sv = std::string_view(seg.key);
        auto* child = current->ObjectGet(sv);
        if (child) {
          Evaluate(path, seg_idx + 1, child, current, -1, seg.key, results);
        }
      }
      break;

    case PathSegType::kIndex:
      if (current->IsArray()) {
        int32_t resolved = ResolveIndex(seg.index, current->ArrayLen());
        if (resolved >= 0 && static_cast<uint32_t>(resolved) < current->ArrayLen()) {
          Evaluate(path, seg_idx + 1, current->ArrayGet(static_cast<uint32_t>(resolved)),
                   current, resolved, {}, results);
        }
      }
      break;

    case PathSegType::kSlice:
      if (current->IsArray()) {
        int32_t start, stop;
        int32_t step = seg.slice.step != 0 ? seg.slice.step : 1;
        ResolveSlice(seg.slice, current->ArrayLen(), start, stop, step);

        if (step > 0) {
          for (int32_t i = start; i < stop; i += step) {
            if (i >= 0 && static_cast<uint32_t>(i) < current->ArrayLen()) {
              Evaluate(path, seg_idx + 1, current->ArrayGet(static_cast<uint32_t>(i)),
                       current, i, {}, results);
            }
          }
        } else {
          for (int32_t i = start; i > stop; i += step) {
            if (i >= 0 && static_cast<uint32_t>(i) < current->ArrayLen()) {
              Evaluate(path, seg_idx + 1, current->ArrayGet(static_cast<uint32_t>(i)),
                       current, i, {}, results);
            }
          }
        }
      }
      break;

    case PathSegType::kWildcard:
      if (current->IsArray()) {
        for (uint32_t i = 0; i < current->ArrayLen(); i++) {
          Evaluate(path, seg_idx + 1, current->ArrayGet(i),
                   current, static_cast<int32_t>(i), {}, results);
        }
      } else if (current->IsObject()) {
        auto* entries = current->ObjectEntries();
        for (uint32_t i = 0; i < current->ObjectLen(); i++) {
          Evaluate(path, seg_idx + 1, entries[i].value, current, -1,
                   std::string(entries[i].key, entries[i].key_len), results);
        }
      }
      break;

    case PathSegType::kRecursive:
      EvalRecursive(path, seg_idx + 1, current, parent, arr_idx, obj_key, results);
      break;
  }
}

} // namespace

PathParseResult ParsePath(std::string_view input) {
  PathParseResult result;

  if (input.empty() || input == "." || input == "$") {
    result.path.segments.push_back({PathSegType::kRoot, {}, 0, {}});
    result.path.is_legacy = (input != "$");
    return result;
  }

  PathParser p;
  p.pos = input.data();
  p.end = input.data() + input.size();

  if (p.Peek() == '$') {
    result.path.is_legacy = false;
    result.path.segments.push_back({PathSegType::kRoot, {}, 0, {}});
    p.Advance();
  } else {
    result.path.is_legacy = true;
    result.path.segments.push_back({PathSegType::kRoot, {}, 0, {}});
    if (p.Peek() == '.') {
      // Legacy path starts with dot
    } else {
      // Bare key as first segment
      std::string key;
      if (ParseDotKey(p, key)) {
        result.path.segments.push_back({PathSegType::kKey, std::move(key), 0, {}});
      }
    }
  }

  while (p.HasMore()) {
    char c = p.Peek();

    if (c == '.') {
      p.Advance();
      if (p.HasMore() && p.Peek() == '.') {
        p.Advance();
        result.path.segments.push_back({PathSegType::kRecursive, {}, 0, {}});
        if (p.HasMore() && p.Peek() == '[') {
          continue;
        }
        if (p.HasMore() && p.Peek() == '*') {
          p.Advance();
          result.path.segments.push_back({PathSegType::kWildcard, {}, 0, {}});
          continue;
        }
        std::string key;
        if (ParseDotKey(p, key)) {
          result.path.segments.push_back({PathSegType::kKey, std::move(key), 0, {}});
        } else {
          result.error = "expected key after '..'";
          return result;
        }
      } else if (p.HasMore() && p.Peek() == '*') {
        p.Advance();
        result.path.segments.push_back({PathSegType::kWildcard, {}, 0, {}});
      } else if (p.HasMore() && p.Peek() == '[') {
        // .["key"] notation
        if (!ParseBracketContent(p, result.path.segments)) {
          result.error = p.error;
          return result;
        }
      } else {
        std::string key;
        if (ParseDotKey(p, key)) {
          result.path.segments.push_back({PathSegType::kKey, std::move(key), 0, {}});
        } else {
          result.error = "expected key after '.'";
          return result;
        }
      }
    } else if (c == '[') {
      if (!ParseBracketContent(p, result.path.segments)) {
        result.error = p.error;
        return result;
      }
    } else {
      result.error = "unexpected character in path";
      return result;
    }
  }

  return result;
}

std::vector<PathMatch> EvaluatePath(const JsonPath& path, JsonValue* root) {
  MatchVec results;
  if (!root || path.segments.empty()) return results;
  Evaluate(path, 0, root, nullptr, -1, {}, results);
  return results;
}
