#pragma once

#include "json_value.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class PathSegType : uint8_t {
  kRoot,
  kKey,
  kIndex,
  kSlice,
  kWildcard,
  kRecursive,
  kFilter,
  kUnion,
};

struct SliceParams {
  int32_t start = 0;
  int32_t stop = 0;
  int32_t step = 1;
  bool start_set = false;
  bool stop_set = false;
};

enum class FilterOp : uint8_t {
  kExists,
  kEq,
  kNe,
  kGt,
  kGe,
  kLt,
  kLe,
};

struct FilterValue {
  enum class Kind : uint8_t { kNull, kBool, kInteger, kNumber, kString };
  Kind kind = Kind::kNull;
  bool bool_val = false;
  int64_t int_val = 0;
  double num_val = 0.0;
  std::string str_val;
};

struct FilterCondition {
  std::vector<std::string> field_path;
  FilterOp op = FilterOp::kExists;
  FilterValue value;
};

struct UnionMember {
  bool is_index;
  int32_t index = 0;
  std::string key;
};

struct UnionParams {
  std::vector<UnionMember> members;
};

struct PathSegment {
  PathSegType type;
  std::string key;
  int32_t index = 0;
  SliceParams slice;
  FilterCondition filter;
  UnionParams union_params;
};

struct JsonPath {
  std::vector<PathSegment> segments;
  bool is_legacy = false;
};

struct PathMatch {
  JsonValue* value;
  JsonValue* parent;
  int32_t array_index = -1;
  std::string object_key;
};

struct PathParseResult {
  JsonPath path;
  const char* error = nullptr;
};

PathParseResult ParsePath(std::string_view input);

std::vector<PathMatch> EvaluatePath(const JsonPath& path, JsonValue* root);
