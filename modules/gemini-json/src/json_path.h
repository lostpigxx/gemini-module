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
  kRegex,
};

struct FilterValue {
  enum class Kind : uint8_t { kNull, kBool, kInteger, kNumber, kString };
  Kind kind = Kind::kNull;
  bool bool_val = false;
  int64_t int_val = 0;
  double num_val = 0.0;
  std::string str_val;
};

struct FilterStep {
  bool is_index = false;
  int32_t index = 0;
  std::string key;
};

struct FilterOperand {
  enum class Kind : uint8_t { kLiteral, kCurrentPath, kRootPath };
  Kind kind = Kind::kLiteral;
  FilterValue literal;
  std::vector<FilterStep> steps;
};

struct FilterNode {
  enum class Kind : uint8_t { kOr, kAnd, kNot, kExists, kCompare };
  Kind kind = Kind::kExists;
  int32_t left = -1;
  int32_t right = -1;
  FilterOp op = FilterOp::kExists;
  FilterOperand lhs;
  FilterOperand rhs;
  bool regex_icase = false;
};

struct FilterCondition {
  // Legacy summary fields (populated for simple single-condition filters)
  std::vector<std::string> field_path;
  FilterOp op = FilterOp::kExists;
  FilterValue value;

  // Full boolean/comparison/regex AST arena
  std::vector<FilterNode> nodes;
  int32_t root = -1;
};

struct UnionMember {
  enum class Kind : uint8_t { kIndex, kKey, kSlice };
  Kind kind = Kind::kIndex;
  bool is_index = false;  // kept for backward compatibility
  int32_t index = 0;
  std::string key;
  SliceParams slice;
};

struct UnionParams {
  std::vector<UnionMember> members;
};

struct PathSegment {
  PathSegType type = PathSegType::kRoot;
  std::string key;
  int32_t index = 0;
  SliceParams slice;
  FilterCondition filter;
  UnionParams union_params;
};

struct JsonPath {
  std::vector<PathSegment> segments;
  bool is_legacy = false;
  bool is_simple = false;          // true if only kRoot / kKey / kIndex
  bool has_multi_recursive = false;
};

struct PathMatch {
  JsonValue* value;
  JsonValue* parent;
  int32_t array_index = -1;
  std::string object_key;
  bool has_object_key = false;
};

struct PathParseResult {
  JsonPath path;
  const char* error = nullptr;
};

PathParseResult ParsePath(std::string_view input);

// Cached path parser (128-entry LRU, single-threaded).
// Returns pointer to cached JsonPath on success, or nullptr with *error set.
const JsonPath* ParsePathCached(std::string_view input, const char** error);

std::vector<PathMatch> EvaluatePath(const JsonPath& path, JsonValue* root);
