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
};

struct SliceParams {
  int32_t start = 0;
  int32_t stop = 0;
  int32_t step = 1;
  bool start_set = false;
  bool stop_set = false;
};

struct PathSegment {
  PathSegType type;
  std::string key;
  int32_t index = 0;
  SliceParams slice;
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
