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

  void SkipSpaces() {
    while (HasMore() && (*pos == ' ' || *pos == '\t')) pos++;
  }

  bool ParseDouble(double& out) {
    if (!HasMore()) return false;
    const char* start = pos;
    char* endptr;
    out = strtod(pos, &endptr);
    if (endptr == start || endptr > end) { pos = start; return false; }
    pos = endptr;
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

static bool ParseFilterExpression(PathParser& p, FilterCondition& cond) {
  if (!p.HasMore() || p.Peek() != '(') { p.error = "expected '(' after '?'"; return false; }
  p.Advance();
  p.SkipSpaces();

  if (!p.HasMore() || p.Peek() != '@') { p.error = "expected '@' in filter"; return false; }
  p.Advance();

  cond.field_path.clear();
  while (p.HasMore() && p.Peek() == '.') {
    p.Advance();
    std::string key;
    if (p.HasMore() && (p.Peek() == '\'' || p.Peek() == '"')) {
      if (!ParseQuotedKey(p, key)) { p.error = "invalid quoted key in filter"; return false; }
    } else if (!ParseDotKey(p, key)) {
      p.error = "expected field name after '.' in filter";
      return false;
    }
    cond.field_path.push_back(std::move(key));
  }

  if (cond.field_path.empty()) { p.error = "filter requires at least one field"; return false; }

  p.SkipSpaces();

  if (p.HasMore() && p.Peek() == ')') {
    p.Advance();
    cond.op = FilterOp::kExists;
    return true;
  }

  if (!p.HasMore()) { p.error = "unexpected end in filter"; return false; }
  char c1 = p.Advance();
  if (!p.HasMore()) { p.error = "unexpected end in filter operator"; return false; }
  char c2 = p.Peek();

  if (c1 == '=' && c2 == '=') { cond.op = FilterOp::kEq; p.Advance(); }
  else if (c1 == '!' && c2 == '=') { cond.op = FilterOp::kNe; p.Advance(); }
  else if (c1 == '>' && c2 == '=') { cond.op = FilterOp::kGe; p.Advance(); }
  else if (c1 == '<' && c2 == '=') { cond.op = FilterOp::kLe; p.Advance(); }
  else if (c1 == '>') { cond.op = FilterOp::kGt; }
  else if (c1 == '<') { cond.op = FilterOp::kLt; }
  else { p.error = "unknown filter operator"; return false; }

  p.SkipSpaces();

  if (!p.HasMore()) { p.error = "expected value in filter"; return false; }

  if (p.Peek() == '\'' || p.Peek() == '"') {
    std::string str;
    if (!ParseQuotedKey(p, str)) { p.error = "invalid string in filter value"; return false; }
    cond.value.kind = FilterValue::Kind::kString;
    cond.value.str_val = std::move(str);
  } else if (p.Peek() == 't' || p.Peek() == 'f') {
    const char* start = p.pos;
    if (p.end - p.pos >= 4 && std::memcmp(p.pos, "true", 4) == 0) {
      cond.value.kind = FilterValue::Kind::kBool;
      cond.value.bool_val = true;
      p.pos += 4;
    } else if (p.end - p.pos >= 5 && std::memcmp(p.pos, "false", 5) == 0) {
      cond.value.kind = FilterValue::Kind::kBool;
      cond.value.bool_val = false;
      p.pos += 5;
    } else {
      p.pos = start;
      p.error = "invalid boolean in filter value";
      return false;
    }
  } else if (p.Peek() == 'n') {
    if (p.end - p.pos >= 4 && std::memcmp(p.pos, "null", 4) == 0) {
      cond.value.kind = FilterValue::Kind::kNull;
      p.pos += 4;
    } else {
      p.error = "invalid null in filter value";
      return false;
    }
  } else {
    const char* num_start = p.pos;
    double dval;
    if (!p.ParseDouble(dval)) { p.error = "invalid number in filter value"; return false; }
    bool is_int = true;
    for (const char* s = num_start; s < p.pos; s++) {
      if (*s == '.' || *s == 'e' || *s == 'E') { is_int = false; break; }
    }
    if (is_int) {
      cond.value.kind = FilterValue::Kind::kInteger;
      cond.value.int_val = static_cast<int64_t>(dval);
    } else {
      cond.value.kind = FilterValue::Kind::kNumber;
      cond.value.num_val = dval;
    }
  }

  p.SkipSpaces();

  if (!p.HasMore() || p.Peek() != ')') { p.error = "expected ')' to close filter"; return false; }
  p.Advance();
  return true;
}

static bool ParseUnionContent(PathParser& p, UnionParams& params,
                              std::vector<PathSegment>& /*segments*/) {
  params.members.clear();

  bool is_key_union = (p.HasMore() && (p.Peek() == '\'' || p.Peek() == '"'));

  while (true) {
    if (is_key_union) {
      std::string key;
      if (!ParseQuotedKey(p, key)) { p.error = "invalid key in union"; return false; }
      params.members.push_back({false, 0, std::move(key)});
    } else {
      int32_t idx;
      if (!p.ParseInt(idx)) { p.error = "invalid index in union"; return false; }
      params.members.push_back({true, idx, {}});
    }

    if (!p.HasMore()) { p.error = "unterminated union"; return false; }
    if (p.Peek() == ']') break;
    if (p.Peek() != ',') { p.error = "expected ',' or ']' in union"; return false; }
    p.Advance();
  }

  return true;
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

  if (p.Peek() == '?') {
    p.Advance();
    FilterCondition cond;
    if (!ParseFilterExpression(p, cond)) return false;
    if (!p.HasMore() || p.Peek() != ']') {
      p.error = "expected ']' after filter"; return false;
    }
    p.Advance();
    PathSegment seg;
    seg.type = PathSegType::kFilter;
    seg.filter = std::move(cond);
    segments.push_back(std::move(seg));
    return true;
  }

  // Scan for ':' and ',' at bracket depth 1 to disambiguate slice/union/index/key
  bool has_colon = false, has_comma = false;
  {
    const char* scan = p.pos;
    int bracket_depth = 1;
    while (scan < p.end && bracket_depth > 0) {
      if (*scan == '[') bracket_depth++;
      else if (*scan == ']') bracket_depth--;
      else if (*scan == ':' && bracket_depth == 1) has_colon = true;
      else if (*scan == ',' && bracket_depth == 1) has_comma = true;
      scan++;
    }
  }

  if (has_comma) {
    UnionParams params;
    if (!ParseUnionContent(p, params, segments)) return false;
    if (!p.HasMore() || p.Peek() != ']') {
      p.error = "expected ']' after union"; return false;
    }
    p.Advance();
    PathSegment seg;
    seg.type = PathSegType::kUnion;
    seg.union_params = std::move(params);
    segments.push_back(std::move(seg));
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
    PathSegment seg;
    seg.type = PathSegType::kKey;
    seg.key = std::move(key);
    segments.push_back(std::move(seg));
    return true;
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

static JsonValue* EvalFilterField(JsonValue* element,
                                  const std::vector<std::string>& field_path) {
  JsonValue* cur = element;
  for (const auto& key : field_path) {
    if (!cur || !cur->IsObject()) return nullptr;
    cur = cur->ObjectGet(std::string_view(key));
  }
  return cur;
}

static bool CompareValues(const JsonValue* lhs, FilterOp op, const FilterValue& rhs) {
  if (op == FilterOp::kExists) return true;

  if (op == FilterOp::kEq || op == FilterOp::kNe) {
    bool eq = false;
    switch (rhs.kind) {
      case FilterValue::Kind::kString:
        eq = lhs->IsString() && lhs->GetString() == rhs.str_val;
        break;
      case FilterValue::Kind::kInteger:
        eq = lhs->IsNumber() && lhs->GetNumber() == static_cast<double>(rhs.int_val);
        break;
      case FilterValue::Kind::kNumber:
        eq = lhs->IsNumber() && lhs->GetNumber() == rhs.num_val;
        break;
      case FilterValue::Kind::kBool:
        eq = lhs->IsBool() && lhs->GetBool() == rhs.bool_val;
        break;
      case FilterValue::Kind::kNull:
        eq = lhs->IsNull();
        break;
    }
    return op == FilterOp::kEq ? eq : !eq;
  }

  // Ordering comparisons: >, >=, <, <=
  if (lhs->IsNumber() && (rhs.kind == FilterValue::Kind::kInteger ||
                           rhs.kind == FilterValue::Kind::kNumber)) {
    double lv = lhs->GetNumber();
    double rv = (rhs.kind == FilterValue::Kind::kInteger)
                  ? static_cast<double>(rhs.int_val) : rhs.num_val;
    switch (op) {
      case FilterOp::kGt: return lv > rv;
      case FilterOp::kGe: return lv >= rv;
      case FilterOp::kLt: return lv < rv;
      case FilterOp::kLe: return lv <= rv;
      default: break;
    }
  }
  if (lhs->IsString() && rhs.kind == FilterValue::Kind::kString) {
    int cmp = lhs->GetString().compare(rhs.str_val);
    switch (op) {
      case FilterOp::kGt: return cmp > 0;
      case FilterOp::kGe: return cmp >= 0;
      case FilterOp::kLt: return cmp < 0;
      case FilterOp::kLe: return cmp <= 0;
      default: break;
    }
  }
  return false;
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

    case PathSegType::kFilter: {
      const auto& cond = seg.filter;
      auto TestElement = [&](JsonValue* elem, JsonValue* par, int32_t ai, const std::string& ok) {
        auto* field_val = EvalFilterField(elem, cond.field_path);
        bool match = false;
        if (cond.op == FilterOp::kExists) {
          match = (field_val != nullptr);
        } else if (field_val) {
          match = CompareValues(field_val, cond.op, cond.value);
        }
        if (match) {
          Evaluate(path, seg_idx + 1, elem, par, ai, ok, results);
        }
      };

      if (current->IsArray()) {
        for (uint32_t i = 0; i < current->ArrayLen(); i++) {
          TestElement(current->ArrayGet(i), current, static_cast<int32_t>(i), {});
        }
      } else if (current->IsObject()) {
        auto* entries = current->ObjectEntries();
        for (uint32_t i = 0; i < current->ObjectLen(); i++) {
          TestElement(entries[i].value, current, -1,
                      std::string(entries[i].key, entries[i].key_len));
        }
      }
      break;
    }

    case PathSegType::kUnion:
      for (const auto& member : seg.union_params.members) {
        if (member.is_index) {
          if (current->IsArray()) {
            int32_t resolved = ResolveIndex(member.index, current->ArrayLen());
            if (resolved >= 0 && static_cast<uint32_t>(resolved) < current->ArrayLen()) {
              Evaluate(path, seg_idx + 1,
                       current->ArrayGet(static_cast<uint32_t>(resolved)),
                       current, resolved, {}, results);
            }
          }
        } else {
          if (current->IsObject()) {
            auto* child = current->ObjectGet(std::string_view(member.key));
            if (child) {
              Evaluate(path, seg_idx + 1, child, current, -1, member.key, results);
            }
          }
        }
      }
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
