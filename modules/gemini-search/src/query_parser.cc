#include "query_parser.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <limits>

// =============================================================
// Set operations on sorted vectors
// =============================================================

std::vector<std::string> SetIntersect(const std::vector<std::string>& a,
                                       const std::vector<std::string>& b) {
  std::vector<std::string> out;
  std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                        std::back_inserter(out));
  return out;
}

std::vector<std::string> SetUnion(const std::vector<std::string>& a,
                                   const std::vector<std::string>& b) {
  std::vector<std::string> out;
  std::set_union(a.begin(), a.end(), b.begin(), b.end(),
                 std::back_inserter(out));
  return out;
}

std::vector<std::string> SetDifference(const std::vector<std::string>& a,
                                        const std::vector<std::string>& b) {
  std::vector<std::string> out;
  std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                      std::back_inserter(out));
  return out;
}

// =============================================================
// Query evaluation
// =============================================================

std::vector<std::string> EvaluateQuery(
    const QueryNode& node, const IndexSpec& spec,
    const DocumentStore& doc_store, const TagFieldIndices& tag_indices,
    const NumericFieldIndices& numeric_indices, std::string& error_msg) {
  switch (node.type) {
    case QueryNode::Type::kMatchAll:
      return doc_store.AllIds();

    case QueryNode::Type::kTagMatch: {
      const auto* fspec = spec.FindField(node.field_name);
      if (!fspec) {
        error_msg = "ERR query field not in schema";
        return {};
      }
      if (fspec->type != FieldType::kTag) {
        error_msg = "ERR field is not a TAG field";
        return {};
      }
      const auto* idx = tag_indices.Get(node.field_name);
      if (!idx) return {};
      if (node.tag_values.size() == 1) {
        return idx->Lookup(node.tag_values[0]);
      }
      return idx->LookupOr(node.tag_values);
    }

    case QueryNode::Type::kNumericRange: {
      const auto* fspec = spec.FindField(node.field_name);
      if (!fspec) {
        error_msg = "ERR query field not in schema";
        return {};
      }
      if (fspec->type != FieldType::kNumeric) {
        error_msg = "ERR field is not a NUMERIC field";
        return {};
      }
      const auto* idx = numeric_indices.Get(node.field_name);
      if (!idx) return {};
      return idx->RangeQuery(node.range_min, node.min_exclusive,
                              node.range_max, node.max_exclusive);
    }

    case QueryNode::Type::kAnd: {
      if (node.children.size() != 2) {
        error_msg = "ERR internal: AND requires 2 children";
        return {};
      }
      auto left = EvaluateQuery(node.children[0], spec, doc_store,
                                 tag_indices, numeric_indices, error_msg);
      if (!error_msg.empty()) return {};
      auto right = EvaluateQuery(node.children[1], spec, doc_store,
                                  tag_indices, numeric_indices, error_msg);
      if (!error_msg.empty()) return {};
      return SetIntersect(left, right);
    }

    case QueryNode::Type::kOr: {
      if (node.children.size() != 2) {
        error_msg = "ERR internal: OR requires 2 children";
        return {};
      }
      auto left = EvaluateQuery(node.children[0], spec, doc_store,
                                 tag_indices, numeric_indices, error_msg);
      if (!error_msg.empty()) return {};
      auto right = EvaluateQuery(node.children[1], spec, doc_store,
                                  tag_indices, numeric_indices, error_msg);
      if (!error_msg.empty()) return {};
      return SetUnion(left, right);
    }

    case QueryNode::Type::kNot: {
      if (node.children.size() != 1) {
        error_msg = "ERR internal: NOT requires 1 child";
        return {};
      }
      auto all = doc_store.AllIds();
      auto child = EvaluateQuery(node.children[0], spec, doc_store,
                                  tag_indices, numeric_indices, error_msg);
      if (!error_msg.empty()) return {};
      return SetDifference(all, child);
    }
  }
  error_msg = "ERR internal: unknown query node type";
  return {};
}

// =============================================================
// Recursive descent parser
// =============================================================

static bool ParseNumericBound(const std::string& token, double& value,
                              bool& exclusive) {
  std::string s = token;
  exclusive = false;
  if (!s.empty() && s[0] == '(') {
    exclusive = true;
    s = s.substr(1);
  }
  if (s == "inf" || s == "+inf") {
    value = std::numeric_limits<double>::infinity();
    return true;
  }
  if (s == "-inf") {
    value = -std::numeric_limits<double>::infinity();
    return true;
  }
  char* endptr = nullptr;
  value = std::strtod(s.c_str(), &endptr);
  return endptr != s.c_str() && *endptr == '\0' && !std::isnan(value);
}

struct Parser {
  const std::string& input;
  size_t pos = 0;
  std::string& error_msg;

  Parser(const std::string& s, std::string& err) : input(s), error_msg(err) {}

  char Peek() const {
    if (pos >= input.size()) return '\0';
    return input[pos];
  }

  void SkipSpaces() {
    while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\t')) {
      pos++;
    }
  }

  // Find matching closing bracket/brace, respecting nesting
  size_t FindClosing(char open, char close, size_t start) const {
    int depth = 1;
    for (size_t i = start; i < input.size(); i++) {
      if (input[i] == open) depth++;
      if (input[i] == close) {
        depth--;
        if (depth == 0) return i;
      }
    }
    return std::string::npos;
  }

  // Parse a @field:{...} or @field:[...] leaf, starting at '@'
  bool ParseFieldExpr(QueryNode& out) {
    if (Peek() != '@') {
      error_msg = "ERR syntax error: expected @";
      return false;
    }
    pos++;

    size_t field_start = pos;
    while (pos < input.size() && input[pos] != ':') {
      pos++;
    }
    if (pos >= input.size() || input[pos] != ':') {
      error_msg = "ERR syntax error: expected : after field name";
      return false;
    }
    std::string field_name = input.substr(field_start, pos - field_start);
    if (field_name.empty()) {
      error_msg = "ERR syntax error: empty field name";
      return false;
    }
    pos++;  // skip ':'

    if (pos >= input.size()) {
      error_msg = "ERR syntax error: unexpected end after :";
      return false;
    }

    if (input[pos] == '{') {
      // TAG match
      size_t close = FindClosing('{', '}', pos + 1);
      if (close == std::string::npos) {
        error_msg = "ERR syntax error: expected closing }";
        return false;
      }
      std::string body = input.substr(pos + 1, close - pos - 1);
      pos = close + 1;

      if (body.empty()) {
        error_msg = "ERR syntax error: empty tag value";
        return false;
      }
      if (body.back() == '|') {
        error_msg = "ERR syntax error: trailing | in tag value list";
        return false;
      }

      std::vector<std::string> values;
      size_t vp = 0;
      while (vp < body.size()) {
        size_t pipe = body.find('|', vp);
        if (pipe == std::string::npos) pipe = body.size();
        std::string val = body.substr(vp, pipe - vp);
        if (val.empty()) {
          error_msg = "ERR syntax error: empty tag value in OR list";
          return false;
        }
        values.push_back(std::move(val));
        vp = pipe + 1;
      }

      out.type = QueryNode::Type::kTagMatch;
      out.field_name = std::move(field_name);
      out.tag_values = std::move(values);
      return true;
    }

    if (input[pos] == '[') {
      // Numeric range
      size_t close = FindClosing('[', ']', pos + 1);
      if (close == std::string::npos) {
        error_msg = "ERR syntax error: expected closing ]";
        return false;
      }
      std::string body = input.substr(pos + 1, close - pos - 1);
      pos = close + 1;

      if (body.empty()) {
        error_msg = "ERR syntax error: empty numeric range";
        return false;
      }

      size_t space = body.find(' ');
      if (space == std::string::npos) {
        error_msg = "ERR syntax error: numeric range requires two bounds";
        return false;
      }
      std::string min_tok = body.substr(0, space);
      std::string max_tok = body.substr(space + 1);
      if (min_tok.empty() || max_tok.empty()) {
        error_msg = "ERR syntax error: empty numeric bound";
        return false;
      }

      double min_val, max_val;
      bool min_excl, max_excl;
      if (!ParseNumericBound(min_tok, min_val, min_excl)) {
        error_msg = "ERR syntax error: invalid numeric min value";
        return false;
      }
      if (!ParseNumericBound(max_tok, max_val, max_excl)) {
        error_msg = "ERR syntax error: invalid numeric max value";
        return false;
      }

      out.type = QueryNode::Type::kNumericRange;
      out.field_name = std::move(field_name);
      out.range_min = min_val;
      out.range_max = max_val;
      out.min_exclusive = min_excl;
      out.max_exclusive = max_excl;
      return true;
    }

    error_msg = "ERR syntax error: expected { or [ after field:";
    return false;
  }

  // Primary: '(' expr ')', '*', '@field:...'
  bool ParsePrimary(QueryNode& out) {
    SkipSpaces();
    if (Peek() == '(') {
      pos++;
      if (!ParseOr(out)) return false;
      SkipSpaces();
      if (Peek() != ')') {
        error_msg = "ERR syntax error: expected closing )";
        return false;
      }
      pos++;
      return true;
    }
    if (Peek() == '*') {
      pos++;
      out.type = QueryNode::Type::kMatchAll;
      return true;
    }
    if (Peek() == '@') {
      return ParseFieldExpr(out);
    }
    error_msg = "ERR syntax error: expected @, *, or (";
    return false;
  }

  // Unary: '-' unary | primary
  bool ParseUnary(QueryNode& out) {
    SkipSpaces();
    if (Peek() == '-') {
      pos++;
      QueryNode child;
      if (!ParseUnary(child)) return false;
      out.type = QueryNode::Type::kNot;
      out.children.push_back(std::move(child));
      return true;
    }
    return ParsePrimary(out);
  }

  // And: unary (space unary)*
  // Implicit AND: two expressions separated by space (no explicit operator)
  bool ParseAnd(QueryNode& out) {
    if (!ParseUnary(out)) return false;

    while (true) {
      SkipSpaces();
      char c = Peek();
      // Stop at OR operator, closing paren, end of input, or KNN arrow
      if (c == '|' || c == ')' || c == '\0') break;
      // Check for '=>' (KNN suffix) — stop AND parsing
      if (c == '=' && pos + 1 < input.size() && input[pos + 1] == '>') break;

      QueryNode right;
      if (!ParseUnary(right)) return false;

      QueryNode combined;
      combined.type = QueryNode::Type::kAnd;
      combined.children.push_back(std::move(out));
      combined.children.push_back(std::move(right));
      out = std::move(combined);
    }
    return true;
  }

  // Or: and ('|' and)*
  bool ParseOr(QueryNode& out) {
    if (!ParseAnd(out)) return false;

    while (true) {
      SkipSpaces();
      if (Peek() != '|') break;
      pos++;  // skip '|'

      QueryNode right;
      if (!ParseAnd(right)) return false;

      QueryNode combined;
      combined.type = QueryNode::Type::kOr;
      combined.children.push_back(std::move(out));
      combined.children.push_back(std::move(right));
      out = std::move(combined);
    }
    return true;
  }
};

// Parse KNN suffix: [KNN k @field $param]
static bool ParseKnnSuffix(const std::string& knn_str, ParsedQuery& out,
                           std::string& error_msg) {
  std::string s = knn_str;
  // Trim
  size_t ks = s.find_first_not_of(" \t");
  if (ks != std::string::npos) s = s.substr(ks);
  size_t ke = s.find_last_not_of(" \t");
  if (ke != std::string::npos) s = s.substr(0, ke + 1);

  if (s.empty() || s.front() != '[' || s.back() != ']') {
    error_msg = "ERR syntax error: KNN clause must be [KNN k @field $param]";
    return false;
  }
  std::string body = s.substr(1, s.size() - 2);

  std::vector<std::string> tokens;
  size_t tp = 0;
  while (tp < body.size()) {
    size_t sp = body.find(' ', tp);
    if (sp == std::string::npos) sp = body.size();
    if (sp > tp) tokens.push_back(body.substr(tp, sp - tp));
    tp = sp + 1;
  }

  if (tokens.size() != 4 || (tokens[0] != "KNN" && tokens[0] != "knn")) {
    error_msg = "ERR syntax error: expected [KNN k @field $param]";
    return false;
  }

  char* endptr = nullptr;
  long k_val = std::strtol(tokens[1].c_str(), &endptr, 10);
  if (*endptr != '\0' || k_val <= 0) {
    error_msg = "ERR syntax error: KNN k must be a positive integer";
    return false;
  }

  if (tokens[2].empty() || tokens[2][0] != '@') {
    error_msg = "ERR syntax error: KNN field must start with @";
    return false;
  }
  std::string knn_field = tokens[2].substr(1);
  if (knn_field.empty()) {
    error_msg = "ERR syntax error: empty KNN field name";
    return false;
  }

  if (tokens[3].empty() || tokens[3][0] != '$') {
    error_msg = "ERR syntax error: KNN param must start with $";
    return false;
  }
  std::string knn_param = tokens[3].substr(1);
  if (knn_param.empty()) {
    error_msg = "ERR syntax error: empty KNN param name";
    return false;
  }

  out.has_knn = true;
  out.knn_k = static_cast<size_t>(k_val);
  out.knn_field = std::move(knn_field);
  out.knn_param_name = std::move(knn_param);
  return true;
}

bool ParseQuery(const std::string& input, ParsedQuery& out,
                std::string& error_msg) {
  // Trim
  size_t start = input.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    error_msg = "ERR empty query";
    return false;
  }
  size_t end = input.find_last_not_of(" \t\r\n");
  std::string trimmed = input.substr(start, end - start + 1);

  // Check for => KNN suffix
  size_t arrow = trimmed.find("=>");
  if (arrow != std::string::npos) {
    std::string filter_part = trimmed.substr(0, arrow);
    std::string knn_part = trimmed.substr(arrow + 2);

    if (!ParseKnnSuffix(knn_part, out, error_msg)) return false;

    // Parse filter part as query tree
    size_t fs = filter_part.find_first_not_of(" \t");
    if (fs == std::string::npos) {
      error_msg = "ERR syntax error: empty pre-filter before =>";
      return false;
    }
    size_t fe = filter_part.find_last_not_of(" \t");
    filter_part = filter_part.substr(fs, fe - fs + 1);

    Parser parser(filter_part, error_msg);
    if (!parser.ParseOr(out.root)) return false;
    parser.SkipSpaces();
    if (parser.pos != filter_part.size()) {
      error_msg = "ERR syntax error: unexpected input after query";
      return false;
    }
    return true;
  }

  // No KNN — parse as regular query tree
  Parser parser(trimmed, error_msg);
  if (!parser.ParseOr(out.root)) return false;
  parser.SkipSpaces();
  if (parser.pos != trimmed.size()) {
    error_msg = "ERR syntax error: unexpected input after query";
    return false;
  }
  return true;
}
