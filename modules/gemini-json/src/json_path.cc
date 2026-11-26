#include "json_path.h"

#include <regex.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <list>
#include <unordered_map>
#include <unordered_set>

constexpr size_t kMaxRegexInputLen = 65536;

struct CompiledRegex {
  regex_t re;
  bool valid = false;

  CompiledRegex() = default;
  CompiledRegex(const CompiledRegex&) = delete;
  CompiledRegex& operator=(const CompiledRegex&) = delete;
  ~CompiledRegex() {
    if (valid) regfree(&re);
  }

  bool Match(std::string_view sv) const {
    if (!valid || sv.size() > kMaxRegexInputLen) return false;
    char stack_buf[256];
    if (sv.size() < sizeof(stack_buf)) {
      std::memcpy(stack_buf, sv.data(), sv.size());
      stack_buf[sv.size()] = '\0';
      return regexec(&re, stack_buf, 0, nullptr, 0) == 0;
    }
    std::string tmp(sv);
    return regexec(&re, tmp.c_str(), 0, nullptr, 0) == 0;
  }
};

namespace {

constexpr size_t kMaxRegexPatternLen = 512;
constexpr int kMaxFilterParenDepth = 32;

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
    while (HasMore() && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r')) pos++;
  }

  bool ParseNumberLiteral(FilterValue& out) {
    if (!HasMore()) return false;
    const char* start = pos;
    const char* p = pos;
    bool neg = false;
    if (p < end && *p == '-') { neg = true; p++; }
    if (p >= end || *p < '0' || *p > '9') return false;
    const char* digits_start = p;
    while (p < end && *p >= '0' && *p <= '9') p++;
    const char* digits_end = p;

    bool is_float = false;
    if (p < end && *p == '.') {
      is_float = true;
      p++;
      if (p >= end || *p < '0' || *p > '9') return false;
      while (p < end && *p >= '0' && *p <= '9') p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
      is_float = true;
      p++;
      if (p < end && (*p == '+' || *p == '-')) p++;
      if (p >= end || *p < '0' || *p > '9') return false;
      while (p < end && *p >= '0' && *p <= '9') p++;
    }

    if (!is_float) {
      size_t dc = static_cast<size_t>(digits_end - digits_start);
      if (dc <= 18) {
        uint64_t acc = 0;
        for (const char* d = digits_start; d < digits_end; d++) {
          acc = acc * 10 + static_cast<uint64_t>(*d - '0');
        }
        out.kind = FilterValue::Kind::kInteger;
        out.int_val = neg ? -static_cast<int64_t>(acc) : static_cast<int64_t>(acc);
        pos = p;
        return true;
      }
      if (dc == 19) {
        uint64_t acc = 0;
        for (const char* d = digits_start; d < digits_end; d++) {
          acc = acc * 10 + static_cast<uint64_t>(*d - '0');
        }
        if (!neg && acc <= static_cast<uint64_t>(INT64_MAX)) {
          out.kind = FilterValue::Kind::kInteger;
          out.int_val = static_cast<int64_t>(acc);
          pos = p;
          return true;
        }
        if (neg && acc <= static_cast<uint64_t>(INT64_MAX) + 1ULL) {
          out.kind = FilterValue::Kind::kInteger;
          out.int_val = (acc == static_cast<uint64_t>(INT64_MAX) + 1ULL)
                            ? INT64_MIN
                            : -static_cast<int64_t>(acc);
          pos = p;
          return true;
        }
      }
    }

    size_t len = static_cast<size_t>(p - start);
    char stack_buf[64];
    std::string heap_buf;
    const char* cstr = stack_buf;
    if (len < sizeof(stack_buf)) {
      std::memcpy(stack_buf, start, len);
      stack_buf[len] = '\0';
    } else {
      heap_buf.assign(start, len);
      cstr = heap_buf.c_str();
    }

    char* endptr = nullptr;
    errno = 0;
    double val = std::strtod(cstr, &endptr);
    if (errno == ERANGE || endptr != cstr + len || !std::isfinite(val)) {
      return false;
    }
    out.kind = FilterValue::Kind::kNumber;
    out.num_val = val;
    pos = p;
    return true;
  }
};

static int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int ParseHex4(PathParser& p) {
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

static void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

static bool ParseQuotedKey(PathParser& p, std::string& out) {
  if (!p.HasMore()) return false;
  char quote = p.Peek();
  if (quote != '\'' && quote != '"') return false;
  p.Advance();
  out.clear();
  while (p.HasMore()) {
    char c = p.Advance();
    if (c == quote) return true;
    if (c == '\\') {
      if (!p.HasMore()) return false;
      char esc = p.Advance();
      switch (esc) {
        case '"':  out.push_back('"'); break;
        case '\'': out.push_back('\''); break;
        case '\\': out.push_back('\\'); break;
        case '/':  out.push_back('/'); break;
        case 'b':  out.push_back('\b'); break;
        case 'f':  out.push_back('\f'); break;
        case 'n':  out.push_back('\n'); break;
        case 'r':  out.push_back('\r'); break;
        case 't':  out.push_back('\t'); break;
        case 'u': {
          int cp = ParseHex4(p);
          if (cp < 0) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (p.end - p.pos < 2 || p.pos[0] != '\\' || p.pos[1] != 'u') return false;
            p.pos += 2;
            int low = ParseHex4(p);
            if (low < 0xDC00 || low > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false;
          }
          AppendUtf8(out, static_cast<uint32_t>(cp));
          break;
        }
        default:
          out.push_back(esc);
          break;
      }
    } else {
      out.push_back(c);
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

static PathSegment MakeSeg(PathSegType type) {
  PathSegment seg;
  seg.type = type;
  return seg;
}

// --- Filter Expression Recursive-Descent Parser ---

static bool ParseFilterPathOperand(PathParser& p, FilterOperand& out) {
  char lead = p.Advance();  // '@' or '$'
  out.kind = (lead == '@') ? FilterOperand::Kind::kCurrentPath
                           : FilterOperand::Kind::kRootPath;
  out.steps.clear();

  while (p.HasMore()) {
    if (p.Peek() == '.') {
      p.Advance();
      if (p.HasMore() && p.Peek() == '*') {
        p.Advance();
        FilterStep st;
        st.is_wildcard = true;
        out.steps.push_back(std::move(st));
        continue;
      }
      std::string key;
      if (p.HasMore() && (p.Peek() == '\'' || p.Peek() == '"')) {
        if (!ParseQuotedKey(p, key)) { p.error = "invalid quoted key in filter"; return false; }
      } else if (!ParseDotKey(p, key)) {
        p.error = "expected field name after '.' in filter";
        return false;
      }
      FilterStep st;
      st.is_index = false;
      st.key = std::move(key);
      out.steps.push_back(std::move(st));
    } else if (p.Peek() == '[') {
      p.Advance();
      p.SkipSpaces();
      if (!p.HasMore()) { p.error = "unterminated '[' in filter"; return false; }
      if (p.Peek() == '*') {
        p.Advance();
        p.SkipSpaces();
        if (!p.HasMore() || p.Peek() != ']') { p.error = "expected ']' in filter"; return false; }
        p.Advance();
        FilterStep st;
        st.is_wildcard = true;
        out.steps.push_back(std::move(st));
      } else if (p.Peek() == '\'' || p.Peek() == '"') {
        std::string key;
        if (!ParseQuotedKey(p, key)) { p.error = "invalid quoted key in filter"; return false; }
        p.SkipSpaces();
        if (!p.HasMore() || p.Peek() != ']') { p.error = "expected ']' in filter"; return false; }
        p.Advance();
        FilterStep st;
        st.is_index = false;
        st.key = std::move(key);
        out.steps.push_back(std::move(st));
      } else {
        int32_t idx = 0;
        if (!p.ParseInt(idx)) { p.error = "expected index in filter bracket"; return false; }
        p.SkipSpaces();
        if (!p.HasMore() || p.Peek() != ']') { p.error = "expected ']' in filter"; return false; }
        p.Advance();
        FilterStep st;
        st.is_index = true;
        st.index = idx;
        out.steps.push_back(std::move(st));
      }
    } else {
      break;
    }
  }
  return true;
}

static bool CompileRegexFromString(std::string pat, bool anchor_full,
                                   std::shared_ptr<const CompiledRegex>& out_re,
                                   const char*& err) {
  bool icase = false;
  if (pat.size() >= 4 && pat.compare(0, 4, "(?i)") == 0) {
    icase = true;
    pat.erase(0, 4);
  }
  if (pat.size() > kMaxRegexPatternLen) {
    err = "regex pattern too long";
    return false;
  }
  std::string final_pat = anchor_full ? ("^(" + pat + ")$") : pat;
  auto compiled = std::make_shared<CompiledRegex>();
  int flags = REG_EXTENDED | REG_NOSUB | (icase ? REG_ICASE : 0);
  if (regcomp(&compiled->re, final_pat.c_str(), flags) != 0) {
    err = "invalid regex pattern";
    return false;
  }
  compiled->valid = true;
  out_re = std::move(compiled);
  return true;
}

static bool ParseFilterOperand(PathParser& p, FilterOperand& out);

static bool ParseFilterFuncCall(PathParser& p, std::string_view fname, FilterOperand& out) {
  FilterFuncKind fk = FilterFuncKind::kNone;
  size_t expected_args = 0;
  if (fname == "length") { fk = FilterFuncKind::kLength; expected_args = 1; }
  else if (fname == "count") { fk = FilterFuncKind::kCount; expected_args = 1; }
  else if (fname == "match") { fk = FilterFuncKind::kMatch; expected_args = 2; }
  else if (fname == "search") { fk = FilterFuncKind::kSearch; expected_args = 2; }
  else {
    p.error = "unknown filter function";
    return false;
  }

  p.Advance();  // skip '('
  out.kind = FilterOperand::Kind::kFuncCall;
  out.func = fk;
  out.func_args.clear();

  for (size_t i = 0; i < expected_args; i++) {
    if (i > 0) {
      p.SkipSpaces();
      if (!p.HasMore() || p.Peek() != ',') {
        p.error = "expected ',' between function arguments";
        return false;
      }
      p.Advance();
    }
    FilterOperand arg;
    if (!ParseFilterOperand(p, arg)) return false;
    out.func_args.push_back(std::move(arg));
  }

  p.SkipSpaces();
  if (!p.HasMore() || p.Peek() != ')') {
    p.error = "expected ')' after function arguments";
    return false;
  }
  p.Advance();

  if (fk == FilterFuncKind::kCount) {
    if (out.func_args[0].kind != FilterOperand::Kind::kCurrentPath &&
        out.func_args[0].kind != FilterOperand::Kind::kRootPath) {
      p.error = "count() requires a path argument";
      return false;
    }
  } else if (fk == FilterFuncKind::kMatch || fk == FilterFuncKind::kSearch) {
    const auto& pat_arg = out.func_args[1];
    if (pat_arg.kind != FilterOperand::Kind::kLiteral ||
        pat_arg.literal.kind != FilterValue::Kind::kString) {
      p.error = "match()/search() requires a string literal pattern";
      return false;
    }
    if (!CompileRegexFromString(pat_arg.literal.str_val,
                                fk == FilterFuncKind::kMatch,
                                out.compiled_regex, p.error)) {
      return false;
    }
  }
  return true;
}

static bool ParseFilterOperand(PathParser& p, FilterOperand& out) {
  p.SkipSpaces();
  if (!p.HasMore()) { p.error = "expected operand in filter"; return false; }

  char c = p.Peek();
  if (c == '@' || c == '$') {
    return ParseFilterPathOperand(p, out);
  }
  out.kind = FilterOperand::Kind::kLiteral;
  if (c == '\'' || c == '"') {
    std::string str;
    if (!ParseQuotedKey(p, str)) { p.error = "invalid string in filter"; return false; }
    out.literal.kind = FilterValue::Kind::kString;
    out.literal.str_val = std::move(str);
    return true;
  }
  if (c == 't' && p.end - p.pos >= 4 && std::memcmp(p.pos, "true", 4) == 0 &&
      (p.end - p.pos == 4 || !IsIdentChar(p.pos[4]))) {
    p.pos += 4;
    out.literal.kind = FilterValue::Kind::kBool;
    out.literal.bool_val = true;
    return true;
  }
  if (c == 'f' && p.end - p.pos >= 5 && std::memcmp(p.pos, "false", 5) == 0 &&
      (p.end - p.pos == 5 || !IsIdentChar(p.pos[5]))) {
    p.pos += 5;
    out.literal.kind = FilterValue::Kind::kBool;
    out.literal.bool_val = false;
    return true;
  }
  if (c == 'n' && p.end - p.pos >= 4 && std::memcmp(p.pos, "null", 4) == 0 &&
      (p.end - p.pos == 4 || !IsIdentChar(p.pos[4]))) {
    p.pos += 4;
    out.literal.kind = FilterValue::Kind::kNull;
    return true;
  }
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
    const char* fstart = p.pos;
    while (p.HasMore() && ((p.Peek() >= 'a' && p.Peek() <= 'z') ||
                           (p.Peek() >= 'A' && p.Peek() <= 'Z') ||
                           p.Peek() == '_')) {
      p.Advance();
    }
    std::string_view fname(fstart, static_cast<size_t>(p.pos - fstart));
    p.SkipSpaces();
    if (p.HasMore() && p.Peek() == '(') {
      return ParseFilterFuncCall(p, fname, out);
    }
    p.error = "unexpected identifier in filter";
    return false;
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    if (!p.ParseNumberLiteral(out.literal)) {
      p.error = "invalid number in filter";
      return false;
    }
    return true;
  }
  p.error = "unexpected token in filter";
  return false;
}

static bool ParseRegexPattern(PathParser& p, std::string& pattern, bool& icase,
                              std::shared_ptr<const CompiledRegex>& out_re) {
  p.SkipSpaces();
  icase = false;
  pattern.clear();
  out_re.reset();
  if (!p.HasMore()) { p.error = "expected regex pattern after '=~'"; return false; }

  if (p.Peek() == '/') {
    p.Advance();
    bool closed = false;
    while (p.HasMore()) {
      char c = p.Advance();
      if (c == '/') {
        closed = true;
        while (p.HasMore() && (p.Peek() == 'i' || p.Peek() == 'm' || p.Peek() == 's')) {
          if (p.Advance() == 'i') icase = true;
        }
        break;
      }
      if (c == '\\' && p.HasMore()) {
        char next = p.Advance();
        if (next != '/') pattern.push_back('\\');
        pattern.push_back(next);
      } else {
        pattern.push_back(c);
      }
    }
    if (!closed) {
      p.error = "unterminated regex literal";
      return false;
    }
  } else if (p.Peek() == '\'' || p.Peek() == '"') {
    if (!ParseQuotedKey(p, pattern)) {
      p.error = "invalid regex string";
      return false;
    }
    if (pattern.size() >= 4 && pattern.compare(0, 4, "(?i)") == 0) {
      icase = true;
      pattern.erase(0, 4);
    }
  } else {
    p.error = "expected string or /regex/ after '=~'";
    return false;
  }

  if (pattern.size() > kMaxRegexPatternLen) {
    p.error = "regex pattern too long";
    return false;
  }
  // Compile regex once upfront (exception-free POSIX NFA)
  auto compiled = std::make_shared<CompiledRegex>();
  int flags = REG_EXTENDED | REG_NOSUB | (icase ? REG_ICASE : 0);
  if (regcomp(&compiled->re, pattern.c_str(), flags) != 0) {
    p.error = "invalid regex pattern";
    return false;
  }
  compiled->valid = true;
  out_re = std::move(compiled);
  return true;
}

static int32_t ParseFilterOr(PathParser& p, std::vector<FilterNode>& nodes, int depth);

static int32_t ParseFilterPrimary(PathParser& p, std::vector<FilterNode>& nodes, int depth) {
  if (depth > kMaxFilterParenDepth) {
    p.error = "filter expression nested too deeply";
    return -1;
  }
  p.SkipSpaces();
  if (!p.HasMore()) { p.error = "unexpected end in filter"; return -1; }

  if (p.Peek() == '!') {
    p.Advance();
    int32_t child = ParseFilterPrimary(p, nodes, depth + 1);
    if (child < 0) return -1;
    FilterNode n;
    n.kind = FilterNode::Kind::kNot;
    n.left = child;
    nodes.push_back(std::move(n));
    return static_cast<int32_t>(nodes.size() - 1);
  }

  if (p.Peek() == '(') {
    p.Advance();
    int32_t inner = ParseFilterOr(p, nodes, depth + 1);
    if (inner < 0) return -1;
    p.SkipSpaces();
    if (!p.HasMore() || p.Peek() != ')') {
      p.error = "expected ')' in filter";
      return -1;
    }
    p.Advance();
    return inner;
  }

  FilterOperand lhs;
  if (!ParseFilterOperand(p, lhs)) return -1;

  p.SkipSpaces();

  // Check for comparison operator
  FilterOp op = FilterOp::kExists;
  bool has_op = false;
  if (p.HasMore()) {
    char c1 = p.Peek();
    char c2 = (p.end - p.pos >= 2) ? p.pos[1] : '\0';
    if (c1 == '=' && c2 == '=') { op = FilterOp::kEq; p.pos += 2; has_op = true; }
    else if (c1 == '!' && c2 == '=') { op = FilterOp::kNe; p.pos += 2; has_op = true; }
    else if (c1 == '>' && c2 == '=') { op = FilterOp::kGe; p.pos += 2; has_op = true; }
    else if (c1 == '<' && c2 == '=') { op = FilterOp::kLe; p.pos += 2; has_op = true; }
    else if (c1 == '=' && c2 == '~') { op = FilterOp::kRegex; p.pos += 2; has_op = true; }
    else if (c1 == '>') { op = FilterOp::kGt; p.pos += 1; has_op = true; }
    else if (c1 == '<') { op = FilterOp::kLt; p.pos += 1; has_op = true; }
  }

  if (!has_op) {
    if (lhs.kind == FilterOperand::Kind::kFuncCall &&
        (lhs.func == FilterFuncKind::kMatch || lhs.func == FilterFuncKind::kSearch)) {
      FilterNode n;
      n.kind = FilterNode::Kind::kCompare;
      n.op = FilterOp::kEq;
      n.lhs = std::move(lhs);
      n.rhs.kind = FilterOperand::Kind::kLiteral;
      n.rhs.literal.kind = FilterValue::Kind::kBool;
      n.rhs.literal.bool_val = true;
      nodes.push_back(std::move(n));
      return static_cast<int32_t>(nodes.size() - 1);
    }
    if (lhs.kind == FilterOperand::Kind::kLiteral ||
        lhs.kind == FilterOperand::Kind::kFuncCall) {
      p.error = "expected comparison operator after expression in filter";
      return -1;
    }
    FilterNode n;
    n.kind = FilterNode::Kind::kExists;
    n.op = FilterOp::kExists;
    n.lhs = std::move(lhs);
    nodes.push_back(std::move(n));
    return static_cast<int32_t>(nodes.size() - 1);
  }

  if (op == FilterOp::kRegex) {
    std::string pat;
    bool icase = false;
    std::shared_ptr<const CompiledRegex> compiled;
    if (!ParseRegexPattern(p, pat, icase, compiled)) return -1;
    FilterNode n;
    n.kind = FilterNode::Kind::kCompare;
    n.op = FilterOp::kRegex;
    n.lhs = std::move(lhs);
    n.rhs.kind = FilterOperand::Kind::kLiteral;
    n.rhs.literal.kind = FilterValue::Kind::kString;
    n.rhs.literal.str_val = std::move(pat);
    n.regex_icase = icase;
    n.compiled_regex = std::move(compiled);
    nodes.push_back(std::move(n));
    return static_cast<int32_t>(nodes.size() - 1);
  }

  FilterOperand rhs;
  if (!ParseFilterOperand(p, rhs)) return -1;

  FilterNode n;
  n.kind = FilterNode::Kind::kCompare;
  n.op = op;
  n.lhs = std::move(lhs);
  n.rhs = std::move(rhs);
  nodes.push_back(std::move(n));
  return static_cast<int32_t>(nodes.size() - 1);
}

static int32_t ParseFilterAnd(PathParser& p, std::vector<FilterNode>& nodes, int depth) {
  int32_t lhs = ParseFilterPrimary(p, nodes, depth);
  if (lhs < 0) return -1;

  while (true) {
    p.SkipSpaces();
    if (p.end - p.pos >= 2 && p.pos[0] == '&' && p.pos[1] == '&') {
      p.pos += 2;
      int32_t rhs = ParseFilterPrimary(p, nodes, depth);
      if (rhs < 0) return -1;
      FilterNode n;
      n.kind = FilterNode::Kind::kAnd;
      n.left = lhs;
      n.right = rhs;
      nodes.push_back(std::move(n));
      lhs = static_cast<int32_t>(nodes.size() - 1);
    } else {
      break;
    }
  }
  return lhs;
}

static int32_t ParseFilterOr(PathParser& p, std::vector<FilterNode>& nodes, int depth) {
  int32_t lhs = ParseFilterAnd(p, nodes, depth);
  if (lhs < 0) return -1;

  while (true) {
    p.SkipSpaces();
    if (p.end - p.pos >= 2 && p.pos[0] == '|' && p.pos[1] == '|') {
      p.pos += 2;
      int32_t rhs = ParseFilterAnd(p, nodes, depth);
      if (rhs < 0) return -1;
      FilterNode n;
      n.kind = FilterNode::Kind::kOr;
      n.left = lhs;
      n.right = rhs;
      nodes.push_back(std::move(n));
      lhs = static_cast<int32_t>(nodes.size() - 1);
    } else {
      break;
    }
  }
  return lhs;
}

static bool ParseFilterExpression(PathParser& p, FilterCondition& cond) {
  p.SkipSpaces();

  cond.nodes.clear();
  int32_t root = ParseFilterOr(p, cond.nodes, 0);
  if (root < 0) return false;

  p.SkipSpaces();
  cond.root = root;

  // Populate legacy summary fields for single-node filters (backward compat)
  const FilterNode& rn = cond.nodes[static_cast<size_t>(root)];
  if ((rn.kind == FilterNode::Kind::kExists || rn.kind == FilterNode::Kind::kCompare) &&
      rn.lhs.kind == FilterOperand::Kind::kCurrentPath) {
    cond.field_path.clear();
    for (const auto& st : rn.lhs.steps) {
      if (!st.is_index) cond.field_path.push_back(st.key);
    }
    cond.op = rn.op;
    if (rn.kind == FilterNode::Kind::kCompare &&
        rn.rhs.kind == FilterOperand::Kind::kLiteral) {
      cond.value = rn.rhs.literal;
    }
  }

  return true;
}

static bool ParseSliceSpec(PathParser& p, SliceParams& slice) {
  p.SkipSpaces();
  if (p.HasMore() && p.Peek() != ':') {
    if (!p.ParseInt(slice.start)) { p.error = "invalid slice start"; return false; }
    slice.start_set = true;
    p.SkipSpaces();
  }
  if (!p.HasMore() || p.Peek() != ':') { p.error = "expected ':' in slice"; return false; }
  p.Advance();
  p.SkipSpaces();

  if (p.HasMore() && p.Peek() != ':' && p.Peek() != ']' && p.Peek() != ',') {
    if (!p.ParseInt(slice.stop)) { p.error = "invalid slice stop"; return false; }
    slice.stop_set = true;
    p.SkipSpaces();
  }
  if (p.HasMore() && p.Peek() == ':') {
    p.Advance();
    p.SkipSpaces();
    if (p.HasMore() && p.Peek() != ']' && p.Peek() != ',') {
      if (!p.ParseInt(slice.step)) { p.error = "invalid slice step"; return false; }
      p.SkipSpaces();
    }
  }
  return true;
}

static bool ParseUnionContent(PathParser& p, UnionParams& params) {
  params.members.clear();

  while (true) {
    p.SkipSpaces();
    if (!p.HasMore()) { p.error = "unterminated union"; return false; }

    if (p.Peek() == '\'' || p.Peek() == '"') {
      std::string key;
      if (!ParseQuotedKey(p, key)) { p.error = "invalid key in union"; return false; }
      UnionMember m;
      m.kind = UnionMember::Kind::kKey;
      m.is_index = false;
      m.key = std::move(key);
      params.members.push_back(std::move(m));
    } else {
      // Check if this member contains ':' before ',' or ']' (slice in union)
      bool is_slice = false;
      for (const char* s = p.pos; s < p.end && *s != ',' && *s != ']'; s++) {
        if (*s == ':') { is_slice = true; break; }
      }
      if (is_slice) {
        SliceParams sp;
        if (!ParseSliceSpec(p, sp)) return false;
        UnionMember m;
        m.kind = UnionMember::Kind::kSlice;
        m.is_index = false;
        m.slice = sp;
        params.members.push_back(std::move(m));
      } else {
        int32_t idx = 0;
        if (!p.ParseInt(idx)) { p.error = "invalid index in union"; return false; }
        UnionMember m;
        m.kind = UnionMember::Kind::kIndex;
        m.is_index = true;
        m.index = idx;
        params.members.push_back(std::move(m));
      }
    }

    p.SkipSpaces();
    if (!p.HasMore()) { p.error = "unterminated union"; return false; }
    if (p.Peek() == ']') break;
    if (p.Peek() != ',') { p.error = "expected ',' or ']' in union"; return false; }
    p.Advance();
  }

  return true;
}

static bool ParseBracketContent(PathParser& p, std::vector<PathSegment>& segments) {
  p.Advance(); // skip '['
  p.SkipSpaces();

  if (!p.HasMore()) { p.error = "unexpected end after '['"; return false; }

  if (p.Peek() == '*') {
    p.Advance();
    p.SkipSpaces();
    if (!p.HasMore() || p.Peek() != ']') {
      p.error = "expected ']' after '*'"; return false;
    }
    p.Advance();
    segments.push_back(MakeSeg(PathSegType::kWildcard));
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
    PathSegment seg = MakeSeg(PathSegType::kFilter);
    seg.filter = std::move(cond);
    segments.push_back(std::move(seg));
    return true;
  }

  // Quote-aware scan for ':' and ',' at bracket depth 1
  bool has_colon = false, has_comma = false;
  {
    const char* scan = p.pos;
    int bracket_depth = 1;
    char in_quote = 0;
    while (scan < p.end && bracket_depth > 0) {
      char c = *scan;
      if (in_quote) {
        if (c == '\\' && scan + 1 < p.end) {
          scan += 2;
          continue;
        }
        if (c == in_quote) in_quote = 0;
      } else {
        if (c == '\'' || c == '"') in_quote = c;
        else if (c == '[') bracket_depth++;
        else if (c == ']') bracket_depth--;
        else if (c == ':' && bracket_depth == 1) has_colon = true;
        else if (c == ',' && bracket_depth == 1) has_comma = true;
      }
      scan++;
    }
  }

  if (has_comma) {
    UnionParams params;
    if (!ParseUnionContent(p, params)) return false;
    if (!p.HasMore() || p.Peek() != ']') {
      p.error = "expected ']' after union"; return false;
    }
    p.Advance();
    PathSegment seg = MakeSeg(PathSegType::kUnion);
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
    p.SkipSpaces();
    if (!p.HasMore() || p.Peek() != ']') {
      p.error = "expected ']' after quoted key"; return false;
    }
    p.Advance();
    PathSegment seg = MakeSeg(PathSegType::kKey);
    seg.key = std::move(key);
    segments.push_back(std::move(seg));
    return true;
  }

  if (has_colon) {
    SliceParams slice;
    if (!ParseSliceSpec(p, slice)) return false;
    if (!p.HasMore() || p.Peek() != ']') {
      p.error = "expected ']' after slice"; return false;
    }
    p.Advance();
    PathSegment seg = MakeSeg(PathSegType::kSlice);
    seg.slice = slice;
    segments.push_back(std::move(seg));
    return true;
  }

  // Simple index
  int32_t idx = 0;
  if (!p.ParseInt(idx)) {
    p.error = "expected integer index in brackets";
    return false;
  }
  p.SkipSpaces();
  if (!p.HasMore() || p.Peek() != ']') {
    p.error = "expected ']' after index";
    return false;
  }
  p.Advance();
  PathSegment seg = MakeSeg(PathSegType::kIndex);
  seg.index = idx;
  segments.push_back(std::move(seg));
  return true;
}

// --- Evaluation Helpers ---

static inline int64_t ResolveIndex64(int32_t idx, uint32_t len) {
  int64_t r = idx;
  if (r < 0) r += static_cast<int64_t>(len);
  return r;
}

static void ResolveSlice(const SliceParams& sp, uint32_t len,
                         int64_t& start, int64_t& stop, int64_t step) {
  int64_t n = static_cast<int64_t>(len);

  if (step > 0) {
    start = sp.start_set ? static_cast<int64_t>(sp.start) : 0;
    stop = sp.stop_set ? static_cast<int64_t>(sp.stop) : n;
  } else {
    start = sp.start_set ? static_cast<int64_t>(sp.start) : n - 1;
    stop = sp.stop_set ? static_cast<int64_t>(sp.stop) : -n - 1;
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

struct EvalValue {
  enum class Kind : uint8_t { kMissing, kNull, kBool, kInteger, kNumber, kString, kContainer };
  Kind kind = Kind::kMissing;
  bool bool_val = false;
  int64_t int_val = 0;
  double num_val = 0.0;
  std::string_view str_val;
  const JsonValue* json_ptr = nullptr;
};

static size_t CountUtf8CodePoints(std::string_view sv) {
  size_t count = 0;
  for (unsigned char c : sv) {
    if ((c & 0xC0) != 0x80) count++;
  }
  return count;
}

static size_t CountPathMatches(const std::vector<FilterStep>& steps, size_t idx,
                               JsonValue* cur) {
  if (!cur) return 0;
  if (idx >= steps.size()) return 1;
  const auto& step = steps[idx];
  if (step.is_wildcard) {
    size_t total = 0;
    if (cur->IsArray()) {
      for (uint32_t i = 0; i < cur->ArrayLen(); i++) {
        total += CountPathMatches(steps, idx + 1, cur->ArrayGet(i));
      }
    } else if (cur->IsObject()) {
      auto* entries = cur->ObjectEntries();
      for (uint32_t i = 0; i < cur->ObjectLen(); i++) {
        total += CountPathMatches(steps, idx + 1, entries[i].value);
      }
    }
    return total;
  }
  if (step.is_index) {
    if (!cur->IsArray()) return 0;
    int64_t r = ResolveIndex64(step.index, cur->ArrayLen());
    if (r < 0 || r >= static_cast<int64_t>(cur->ArrayLen())) return 0;
    return CountPathMatches(steps, idx + 1, cur->ArrayGet(static_cast<uint32_t>(r)));
  }
  if (!cur->IsObject()) return 0;
  return CountPathMatches(steps, idx + 1, cur->ObjectGet(step.key));
}

static EvalValue ResolveOperand(const FilterOperand& op, JsonValue* elem, JsonValue* root) {
  EvalValue out;
  if (op.kind == FilterOperand::Kind::kLiteral) {
    switch (op.literal.kind) {
      case FilterValue::Kind::kNull:    out.kind = EvalValue::Kind::kNull; break;
      case FilterValue::Kind::kBool:    out.kind = EvalValue::Kind::kBool; out.bool_val = op.literal.bool_val; break;
      case FilterValue::Kind::kInteger: out.kind = EvalValue::Kind::kInteger; out.int_val = op.literal.int_val; break;
      case FilterValue::Kind::kNumber:  out.kind = EvalValue::Kind::kNumber; out.num_val = op.literal.num_val; break;
      case FilterValue::Kind::kString:  out.kind = EvalValue::Kind::kString; out.str_val = op.literal.str_val; break;
    }
    return out;
  }

  if (op.kind == FilterOperand::Kind::kFuncCall) {
    switch (op.func) {
      case FilterFuncKind::kLength: {
        EvalValue arg = ResolveOperand(op.func_args[0], elem, root);
        if (arg.kind == EvalValue::Kind::kString) {
          out.kind = EvalValue::Kind::kInteger;
          out.int_val = static_cast<int64_t>(CountUtf8CodePoints(arg.str_val));
        } else if (arg.kind == EvalValue::Kind::kContainer && arg.json_ptr) {
          if (arg.json_ptr->IsArray()) {
            out.kind = EvalValue::Kind::kInteger;
            out.int_val = static_cast<int64_t>(arg.json_ptr->ArrayLen());
          } else if (arg.json_ptr->IsObject()) {
            out.kind = EvalValue::Kind::kInteger;
            out.int_val = static_cast<int64_t>(arg.json_ptr->ObjectLen());
          }
        }
        return out;
      }
      case FilterFuncKind::kCount: {
        const auto& path_arg = op.func_args[0];
        JsonValue* base = (path_arg.kind == FilterOperand::Kind::kCurrentPath) ? elem : root;
        out.kind = EvalValue::Kind::kInteger;
        out.int_val = static_cast<int64_t>(CountPathMatches(path_arg.steps, 0, base));
        return out;
      }
      case FilterFuncKind::kMatch:
      case FilterFuncKind::kSearch: {
        EvalValue arg = ResolveOperand(op.func_args[0], elem, root);
        out.kind = EvalValue::Kind::kBool;
        out.bool_val = (arg.kind == EvalValue::Kind::kString && op.compiled_regex)
                           ? op.compiled_regex->Match(arg.str_val)
                           : false;
        return out;
      }
      case FilterFuncKind::kNone:
        return out;
    }
    return out;
  }

  JsonValue* cur = (op.kind == FilterOperand::Kind::kCurrentPath) ? elem : root;
  for (const auto& step : op.steps) {
    if (!cur) return out;
    if (step.is_index) {
      if (!cur->IsArray()) return out;
      int64_t r = ResolveIndex64(step.index, cur->ArrayLen());
      if (r < 0 || r >= static_cast<int64_t>(cur->ArrayLen())) return out;
      cur = cur->ArrayGet(static_cast<uint32_t>(r));
    } else {
      if (!cur->IsObject()) return out;
      cur = cur->ObjectGet(step.key);
    }
  }
  if (!cur) return out;

  out.json_ptr = cur;
  switch (cur->Type()) {
    case JsonType::kNull:    out.kind = EvalValue::Kind::kNull; break;
    case JsonType::kBool:    out.kind = EvalValue::Kind::kBool; out.bool_val = cur->GetBool(); break;
    case JsonType::kInteger: out.kind = EvalValue::Kind::kInteger; out.int_val = cur->GetInteger(); break;
    case JsonType::kNumber:  out.kind = EvalValue::Kind::kNumber; out.num_val = cur->GetNumber(); break;
    case JsonType::kString:  out.kind = EvalValue::Kind::kString; out.str_val = cur->GetString(); break;
    default:                 out.kind = EvalValue::Kind::kContainer; break;
  }
  return out;
}

static int CompareIntDouble(int64_t i, double d) {
  if (std::isnan(d)) return 0;
  constexpr double kMinInt64D = -9223372036854775808.0;
  constexpr double kMaxInt64ExclusiveD = 9223372036854775808.0;
  if (d < kMinInt64D) return 1;
  if (d >= kMaxInt64ExclusiveD) return -1;
  double fl = std::floor(d);
  int64_t di = static_cast<int64_t>(fl);
  if (i < di) return -1;
  if (i > di) return 1;
  if (d > fl) return -1;
  return 0;
}

static bool CompareEvalValues(const EvalValue& lhs, FilterOp op,
                              const EvalValue& rhs,
                              const std::shared_ptr<const CompiledRegex>& compiled_re) {
  if (lhs.kind == EvalValue::Kind::kMissing || rhs.kind == EvalValue::Kind::kMissing) {
    return false;
  }

  if (op == FilterOp::kRegex) {
    if (lhs.kind != EvalValue::Kind::kString || !compiled_re) {
      return false;
    }
    return compiled_re->Match(lhs.str_val);
  }

  bool lhs_num = (lhs.kind == EvalValue::Kind::kInteger || lhs.kind == EvalValue::Kind::kNumber);
  bool rhs_num = (rhs.kind == EvalValue::Kind::kInteger || rhs.kind == EvalValue::Kind::kNumber);

  if (lhs_num && rhs_num) {
    int cmp = 0;
    if (lhs.kind == EvalValue::Kind::kInteger && rhs.kind == EvalValue::Kind::kInteger) {
      cmp = (lhs.int_val < rhs.int_val) ? -1 : (lhs.int_val > rhs.int_val ? 1 : 0);
    } else if (lhs.kind == EvalValue::Kind::kInteger) {
      cmp = CompareIntDouble(lhs.int_val, rhs.num_val);
    } else if (rhs.kind == EvalValue::Kind::kInteger) {
      cmp = -CompareIntDouble(rhs.int_val, lhs.num_val);
    } else {
      if (std::isnan(lhs.num_val) || std::isnan(rhs.num_val)) return op == FilterOp::kNe;
      cmp = (lhs.num_val < rhs.num_val) ? -1 : (lhs.num_val > rhs.num_val ? 1 : 0);
    }
    switch (op) {
      case FilterOp::kEq: return cmp == 0;
      case FilterOp::kNe: return cmp != 0;
      case FilterOp::kGt: return cmp > 0;
      case FilterOp::kGe: return cmp >= 0;
      case FilterOp::kLt: return cmp < 0;
      case FilterOp::kLe: return cmp <= 0;
      default: return false;
    }
  }

  if (lhs.kind == EvalValue::Kind::kString && rhs.kind == EvalValue::Kind::kString) {
    int cmp = lhs.str_val.compare(rhs.str_val);
    switch (op) {
      case FilterOp::kEq: return cmp == 0;
      case FilterOp::kNe: return cmp != 0;
      case FilterOp::kGt: return cmp > 0;
      case FilterOp::kGe: return cmp >= 0;
      case FilterOp::kLt: return cmp < 0;
      case FilterOp::kLe: return cmp <= 0;
      default: return false;
    }
  }

  if (op == FilterOp::kEq || op == FilterOp::kNe) {
    bool eq = false;
    if (lhs.kind == rhs.kind) {
      switch (lhs.kind) {
        case EvalValue::Kind::kNull: eq = true; break;
        case EvalValue::Kind::kBool: eq = (lhs.bool_val == rhs.bool_val); break;
        case EvalValue::Kind::kContainer:
          eq = (lhs.json_ptr && rhs.json_ptr && lhs.json_ptr->DeepEqual(rhs.json_ptr));
          break;
        default: break;
      }
    }
    return op == FilterOp::kEq ? eq : !eq;
  }

  return false;
}

static bool EvalFilterNode(const FilterCondition& cond, int32_t node_idx,
                           JsonValue* elem, JsonValue* root) {
  if (node_idx < 0 || static_cast<size_t>(node_idx) >= cond.nodes.size()) return false;
  const FilterNode& n = cond.nodes[static_cast<size_t>(node_idx)];
  switch (n.kind) {
    case FilterNode::Kind::kOr:
      return EvalFilterNode(cond, n.left, elem, root) ||
             EvalFilterNode(cond, n.right, elem, root);
    case FilterNode::Kind::kAnd:
      return EvalFilterNode(cond, n.left, elem, root) &&
             EvalFilterNode(cond, n.right, elem, root);
    case FilterNode::Kind::kNot:
      return !EvalFilterNode(cond, n.left, elem, root);
    case FilterNode::Kind::kExists: {
      EvalValue v = ResolveOperand(n.lhs, elem, root);
      return v.kind != EvalValue::Kind::kMissing;
    }
    case FilterNode::Kind::kCompare: {
      EvalValue lv = ResolveOperand(n.lhs, elem, root);
      EvalValue rv = ResolveOperand(n.rhs, elem, root);
      return CompareEvalValues(lv, n.op, rv, n.compiled_regex);
    }
  }
  return false;
}

using MatchVec = std::vector<PathMatch>;

struct EvalState {
  const JsonPath& path;
  JsonValue* root;
  MatchVec& results;
  // Used only when path.has_multi_recursive is true to prevent duplicate visits
  std::unordered_set<uint64_t> visited_rec;
};

static void Evaluate(EvalState& st, size_t seg_idx,
                     JsonValue* current, JsonValue* parent,
                     int32_t arr_idx, std::string_view obj_key,
                     bool has_obj_key, int depth);

static void EvalRecursive(EvalState& st, size_t seg_idx,
                          JsonValue* current, JsonValue* parent,
                          int32_t arr_idx, std::string_view obj_key,
                          bool has_obj_key, int depth) {
  if (depth > kMaxJsonDepth) return;
  if (st.path.has_multi_recursive) {
    uint64_t key = (reinterpret_cast<uintptr_t>(current) * 0x9e3779b97f4a7c15ULL) ^
                   (static_cast<uint64_t>(seg_idx) * 0xbf58476d1ce4e5b9ULL);
    if (!st.visited_rec.insert(key).second) return;
  }

  Evaluate(st, seg_idx, current, parent, arr_idx, obj_key, has_obj_key, depth);

  if (current->IsArray()) {
    uint32_t n = current->ArrayLen();
    for (uint32_t i = 0; i < n; i++) {
      EvalRecursive(st, seg_idx, current->ArrayGet(i), current,
                    static_cast<int32_t>(i), {}, false, depth + 1);
    }
  } else if (current->IsObject()) {
    uint32_t n = current->ObjectLen();
    auto* entries = current->ObjectEntries();
    for (uint32_t i = 0; i < n; i++) {
      EvalRecursive(st, seg_idx, entries[i].value, current, -1,
                    entries[i].KeyView(), true,
                    depth + 1);
    }
  }
}

static void EvalSlice(EvalState& st, size_t next_seg, const SliceParams& sp,
                      JsonValue* current, int depth) {
  if (!current->IsArray()) return;
  if (sp.step == 0) return;  // RFC 9535: step == 0 yields empty result

  int64_t start = 0, stop = 0;
  int64_t step = sp.step;
  uint32_t len = current->ArrayLen();
  ResolveSlice(sp, len, start, stop, step);

  if (step > 0) {
    for (int64_t i = start; i < stop; i += step) {
      if (i >= 0 && i < static_cast<int64_t>(len)) {
        Evaluate(st, next_seg, current->ArrayGet(static_cast<uint32_t>(i)),
                 current, static_cast<int32_t>(i), {}, false, depth + 1);
      }
    }
  } else {
    for (int64_t i = start; i > stop; i += step) {
      if (i >= 0 && i < static_cast<int64_t>(len)) {
        Evaluate(st, next_seg, current->ArrayGet(static_cast<uint32_t>(i)),
                 current, static_cast<int32_t>(i), {}, false, depth + 1);
      }
    }
  }
}

static void Evaluate(EvalState& st, size_t seg_idx,
                     JsonValue* current, JsonValue* parent,
                     int32_t arr_idx, std::string_view obj_key,
                     bool has_obj_key, int depth) {
  if (depth > kMaxJsonDepth) return;
  if (seg_idx >= st.path.segments.size()) {
    st.results.push_back({current, parent, arr_idx,
                          has_obj_key ? std::string(obj_key) : std::string(),
                          has_obj_key, depth});
    return;
  }

  const auto& seg = st.path.segments[seg_idx];

  switch (seg.type) {
    case PathSegType::kRoot:
      Evaluate(st, seg_idx + 1, current, nullptr, -1, {}, false, depth);
      break;

    case PathSegType::kKey:
      if (current->IsObject()) {
        auto* child = current->ObjectGet(seg.key);
        if (child) {
          Evaluate(st, seg_idx + 1, child, current, -1, seg.key, true, depth + 1);
        }
      }
      break;

    case PathSegType::kIndex:
      if (current->IsArray()) {
        int64_t resolved = ResolveIndex64(seg.index, current->ArrayLen());
        if (resolved >= 0 && resolved < static_cast<int64_t>(current->ArrayLen())) {
          Evaluate(st, seg_idx + 1, current->ArrayGet(static_cast<uint32_t>(resolved)),
                   current, static_cast<int32_t>(resolved), {}, false, depth + 1);
        }
      }
      break;

    case PathSegType::kSlice:
      EvalSlice(st, seg_idx + 1, seg.slice, current, depth);
      break;

    case PathSegType::kWildcard:
      if (current->IsArray()) {
        uint32_t n = current->ArrayLen();
        for (uint32_t i = 0; i < n; i++) {
          Evaluate(st, seg_idx + 1, current->ArrayGet(i),
                   current, static_cast<int32_t>(i), {}, false, depth + 1);
        }
      } else if (current->IsObject()) {
        uint32_t n = current->ObjectLen();
        auto* entries = current->ObjectEntries();
        for (uint32_t i = 0; i < n; i++) {
          Evaluate(st, seg_idx + 1, entries[i].value, current, -1,
                   entries[i].KeyView(), true, depth + 1);
        }
      }
      break;

    case PathSegType::kRecursive:
      EvalRecursive(st, seg_idx + 1, current, parent, arr_idx, obj_key, has_obj_key, depth);
      break;

    case PathSegType::kFilter: {
      const auto& cond = seg.filter;
      if (current->IsArray()) {
        uint32_t n = current->ArrayLen();
        for (uint32_t i = 0; i < n; i++) {
          JsonValue* elem = current->ArrayGet(i);
          if (EvalFilterNode(cond, cond.root, elem, st.root)) {
            Evaluate(st, seg_idx + 1, elem, current, static_cast<int32_t>(i), {}, false, depth + 1);
          }
        }
      } else if (current->IsObject()) {
        uint32_t n = current->ObjectLen();
        auto* entries = current->ObjectEntries();
        for (uint32_t i = 0; i < n; i++) {
          if (EvalFilterNode(cond, cond.root, entries[i].value, st.root)) {
            Evaluate(st, seg_idx + 1, entries[i].value, current, -1,
                     entries[i].KeyView(), true, depth + 1);
          }
        }
      }
      break;
    }

    case PathSegType::kUnion:
      for (const auto& member : seg.union_params.members) {
        if (member.kind == UnionMember::Kind::kIndex) {
          if (current->IsArray()) {
            int64_t resolved = ResolveIndex64(member.index, current->ArrayLen());
            if (resolved >= 0 && resolved < static_cast<int64_t>(current->ArrayLen())) {
              Evaluate(st, seg_idx + 1,
                       current->ArrayGet(static_cast<uint32_t>(resolved)),
                       current, static_cast<int32_t>(resolved), {}, false, depth + 1);
            }
          }
        } else if (member.kind == UnionMember::Kind::kKey) {
          if (current->IsObject()) {
            auto* child = current->ObjectGet(member.key);
            if (child) {
              Evaluate(st, seg_idx + 1, child, current, -1, member.key, true, depth + 1);
            }
          }
        } else if (member.kind == UnionMember::Kind::kSlice) {
          EvalSlice(st, seg_idx + 1, member.slice, current, depth);
        }
      }
      break;
  }
}

static void FinalizePathFlags(JsonPath& path) {
  bool simple = true;
  int rec_count = 0;
  for (const auto& seg : path.segments) {
    if (seg.type != PathSegType::kRoot &&
        seg.type != PathSegType::kKey &&
        seg.type != PathSegType::kIndex) {
      simple = false;
    }
    if (seg.type == PathSegType::kRecursive) rec_count++;
  }
  path.is_simple = simple;
  path.has_multi_recursive = (rec_count >= 2);
}

// 128-entry LRU cache for parsed JsonPaths (Redis main thread is single-threaded)
struct PathLruCache {
  static constexpr size_t kMaxEntries = 128;

  struct Entry {
    std::string raw;
    JsonPath path;
  };

  std::list<Entry> lru;
  std::unordered_map<std::string_view, std::list<Entry>::iterator> map;

  const JsonPath* GetOrParse(std::string_view input, const char** error) {
    auto it = map.find(input);
    if (it != map.end()) {
      lru.splice(lru.begin(), lru, it->second);
      if (error) *error = nullptr;
      return &it->second->path;
    }

    auto pr = ParsePath(input);
    if (pr.error) {
      if (error) *error = pr.error;
      return nullptr;
    }

    if (lru.size() >= kMaxEntries) {
      map.erase(lru.back().raw);
      lru.pop_back();
    }

    lru.push_front({std::string(input), std::move(pr.path)});
    auto front = lru.begin();
    map.emplace(std::string_view(front->raw), front);
    if (error) *error = nullptr;
    return &front->path;
  }
};

} // namespace

PathParseResult ParsePath(std::string_view input) {
  PathParseResult result;

  if (input.empty() || input == "." || input == "$") {
    result.path.segments.push_back(MakeSeg(PathSegType::kRoot));
    result.path.is_legacy = (input != "$");
    FinalizePathFlags(result.path);
    return result;
  }

  PathParser p;
  p.pos = input.data();
  p.end = input.data() + input.size();

  if (p.Peek() == '$') {
    result.path.is_legacy = false;
    result.path.segments.push_back(MakeSeg(PathSegType::kRoot));
    p.Advance();
  } else {
    result.path.is_legacy = true;
    result.path.segments.push_back(MakeSeg(PathSegType::kRoot));
    if (p.Peek() == '.') {
      // Legacy path starts with dot
    } else if (p.Peek() != '[') {
      std::string key;
      if (ParseDotKey(p, key)) {
        PathSegment seg = MakeSeg(PathSegType::kKey);
        seg.key = std::move(key);
        result.path.segments.push_back(std::move(seg));
      }
    }
  }

  while (p.HasMore()) {
    char c = p.Peek();

    if (c == '.') {
      p.Advance();
      if (p.HasMore() && p.Peek() == '.') {
        p.Advance();
        result.path.segments.push_back(MakeSeg(PathSegType::kRecursive));
        if (p.HasMore() && p.Peek() == '[') {
          continue;
        }
        if (p.HasMore() && p.Peek() == '*') {
          p.Advance();
          result.path.segments.push_back(MakeSeg(PathSegType::kWildcard));
          continue;
        }
        std::string key;
        if (ParseDotKey(p, key)) {
          PathSegment seg = MakeSeg(PathSegType::kKey);
          seg.key = std::move(key);
          result.path.segments.push_back(std::move(seg));
        } else {
          result.error = "expected key after '..'";
          return result;
        }
      } else if (p.HasMore() && p.Peek() == '*') {
        p.Advance();
        result.path.segments.push_back(MakeSeg(PathSegType::kWildcard));
      } else if (p.HasMore() && p.Peek() == '[') {
        if (!ParseBracketContent(p, result.path.segments)) {
          result.error = p.error;
          return result;
        }
      } else {
        std::string key;
        if (ParseDotKey(p, key)) {
          PathSegment seg = MakeSeg(PathSegType::kKey);
          seg.key = std::move(key);
          result.path.segments.push_back(std::move(seg));
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

  FinalizePathFlags(result.path);
  return result;
}

const JsonPath* ParsePathCached(std::string_view input, const char** error) {
  static PathLruCache cache;
  return cache.GetOrParse(input, error);
}

std::vector<PathMatch> EvaluatePath(const JsonPath& path, JsonValue* root) {
  MatchVec results;
  if (!root || path.segments.empty()) return results;

  // Iterative fast path for deterministic paths (only kRoot / kKey / kIndex)
  if (path.is_simple) {
    JsonValue* cur = root;
    JsonValue* parent = nullptr;
    int32_t arr_idx = -1;
    std::string_view obj_key;
    bool has_obj_key = false;

    int depth = 0;
    for (const auto& seg : path.segments) {
      if (seg.type == PathSegType::kRoot) {
        parent = nullptr;
        arr_idx = -1;
        has_obj_key = false;
        depth = 0;
      } else if (seg.type == PathSegType::kKey) {
        if (!cur->IsObject()) return results;
        JsonValue* child = cur->ObjectGet(seg.key);
        if (!child) return results;
        parent = cur;
        cur = child;
        arr_idx = -1;
        obj_key = seg.key;
        has_obj_key = true;
        depth++;
      } else if (seg.type == PathSegType::kIndex) {
        if (!cur->IsArray()) return results;
        int64_t r = ResolveIndex64(seg.index, cur->ArrayLen());
        if (r < 0 || r >= static_cast<int64_t>(cur->ArrayLen())) return results;
        parent = cur;
        cur = cur->ArrayGet(static_cast<uint32_t>(r));
        arr_idx = static_cast<int32_t>(r);
        has_obj_key = false;
        depth++;
      }
    }
    results.push_back({cur, parent, arr_idx,
                       has_obj_key ? std::string(obj_key) : std::string(),
                       has_obj_key, depth});
    return results;
  }

  EvalState st{path, root, results, {}};
  Evaluate(st, 0, root, nullptr, -1, {}, false, 0);
  return results;
}
