#include "tag_query.h"

#include <cstddef>

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

  size_t colon_brace = q.find(":{");
  if (colon_brace == std::string::npos) {
    error_msg = "ERR syntax error: expected @field:{value}";
    return false;
  }

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
