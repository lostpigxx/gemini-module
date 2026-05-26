#include "tag_query.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>

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

bool ParseTagQuery(const std::string& input, TagQuery& out,
                   std::string& error_msg) {
  size_t start = input.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    error_msg = "ERR empty query";
    return false;
  }
  size_t end = input.find_last_not_of(" \t\r\n");
  std::string q = input.substr(start, end - start + 1);

  if (q == "*") {
    out.type = TagQuery::Type::kMatchAll;
    return true;
  }

  if (q.empty() || q[0] != '@') {
    error_msg = "ERR syntax error: query must start with @ or be *";
    return false;
  }

  // Detect TAG vs NUMERIC by ":{ vs ":["
  size_t colon_brace = q.find(":{");
  size_t colon_bracket = q.find(":[");

  if (colon_bracket != std::string::npos &&
      (colon_brace == std::string::npos || colon_bracket < colon_brace)) {
    // Numeric range: @field:[min max]
    if (q.back() != ']') {
      error_msg = "ERR syntax error: expected closing ]";
      return false;
    }

    std::string field_name = q.substr(1, colon_bracket - 1);
    if (field_name.empty()) {
      error_msg = "ERR syntax error: empty field name";
      return false;
    }

    size_t body_start = colon_bracket + 2;
    size_t body_end = q.size() - 1;
    if (body_start >= body_end) {
      error_msg = "ERR syntax error: empty numeric range";
      return false;
    }
    std::string body = q.substr(body_start, body_end - body_start);

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

    out.type = TagQuery::Type::kNumericRange;
    out.field_name = std::move(field_name);
    out.range_min = min_val;
    out.range_max = max_val;
    out.min_exclusive = min_excl;
    out.max_exclusive = max_excl;
    return true;
  }

  if (colon_brace == std::string::npos) {
    error_msg = "ERR syntax error: expected @field:{value} or @field:[min max]";
    return false;
  }

  // TAG match: @field:{value|...}
  if (q.back() != '}') {
    error_msg = "ERR syntax error: expected closing }";
    return false;
  }

  std::string field_name = q.substr(1, colon_brace - 1);
  if (field_name.empty()) {
    error_msg = "ERR syntax error: empty field name";
    return false;
  }

  size_t values_start = colon_brace + 2;
  size_t values_end = q.size() - 1;
  if (values_start >= values_end) {
    error_msg = "ERR syntax error: empty tag value";
    return false;
  }
  std::string values_str = q.substr(values_start, values_end - values_start);

  if (values_str.back() == '|') {
    error_msg = "ERR syntax error: trailing | in tag value list";
    return false;
  }

  std::vector<std::string> values;
  size_t pos = 0;
  while (pos < values_str.size()) {
    size_t pipe = values_str.find('|', pos);
    if (pipe == std::string::npos) pipe = values_str.size();
    std::string val = values_str.substr(pos, pipe - pos);
    if (val.empty()) {
      error_msg = "ERR syntax error: empty tag value in OR list";
      return false;
    }
    values.push_back(std::move(val));
    pos = pipe + 1;
  }

  out.type = TagQuery::Type::kTagMatch;
  out.field_name = std::move(field_name);
  out.tag_values = std::move(values);
  return true;
}
