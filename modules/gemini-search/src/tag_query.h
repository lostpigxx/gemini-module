#pragma once

#include <string>
#include <vector>

struct TagQuery {
  enum class Type { kMatchAll, kTagMatch, kNumericRange, kKnn };

  Type type = Type::kMatchAll;
  std::string field_name;
  std::vector<std::string> tag_values;
  double range_min = 0;
  double range_max = 0;
  bool min_exclusive = false;
  bool max_exclusive = false;
  size_t knn_k = 0;
  std::string knn_field;
  std::string knn_param_name;
};

bool ParseTagQuery(const std::string& input, TagQuery& out,
                   std::string& error_msg);
