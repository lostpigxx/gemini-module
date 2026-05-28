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

  // Check for KNN suffix: <filter>=>[KNN k @field $param]
  size_t arrow = q.find("=>");
  if (arrow != std::string::npos) {
    std::string filter_part = q.substr(0, arrow);
    std::string knn_part = q.substr(arrow + 2);

    // Trim knn_part
    size_t ks = knn_part.find_first_not_of(" \t");
    if (ks != std::string::npos)
      knn_part = knn_part.substr(ks);
    size_t ke = knn_part.find_last_not_of(" \t");
    if (ke != std::string::npos)
      knn_part = knn_part.substr(0, ke + 1);

    if (knn_part.empty() || knn_part.front() != '[' || knn_part.back() != ']') {
      error_msg = "ERR syntax error: KNN clause must be [KNN k @field $param]";
      return false;
    }
    std::string knn_body = knn_part.substr(1, knn_part.size() - 2);

    // Trim filter_part
    size_t fs = filter_part.find_first_not_of(" \t");
    size_t fe = filter_part.find_last_not_of(" \t");
    if (fs == std::string::npos) {
      error_msg = "ERR syntax error: empty pre-filter before =>";
      return false;
    }
    filter_part = filter_part.substr(fs, fe - fs + 1);
    if (filter_part != "*") {
      error_msg = "ERR syntax error: only * pre-filter supported with KNN";
      return false;
    }

    // Parse: KNN k @field $param
    // Split by spaces
    std::vector<std::string> tokens;
    size_t tp = 0;
    while (tp < knn_body.size()) {
      size_t sp = knn_body.find(' ', tp);
      if (sp == std::string::npos) sp = knn_body.size();
      if (sp > tp) tokens.push_back(knn_body.substr(tp, sp - tp));
      tp = sp + 1;
    }

    if (tokens.size() != 4 ||
        (tokens[0] != "KNN" && tokens[0] != "knn")) {
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

    out.type = TagQuery::Type::kKnn;
    out.knn_k = static_cast<size_t>(k_val);
    out.knn_field = std::move(knn_field);
    out.knn_param_name = std::move(knn_param);
    return true;
  }

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
