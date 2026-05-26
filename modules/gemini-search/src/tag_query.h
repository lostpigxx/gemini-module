#pragma once

#include <string>
#include <vector>

struct TagQuery {
  enum class Type { kMatchAll, kTagMatch };

  Type type = Type::kMatchAll;
  std::string field_name;
  std::vector<std::string> tag_values;
};

bool ParseTagQuery(const std::string& input, TagQuery& out,
                   std::string& error_msg);
