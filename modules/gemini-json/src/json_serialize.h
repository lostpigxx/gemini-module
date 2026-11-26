#pragma once

#include "json_value.h"
#include <cstddef>
#include <string>
#include <string_view>

struct SerializeOptions {
  const char* indent = nullptr;
  const char* newline = nullptr;
  const char* space = nullptr;
  bool noescape = false;
};

struct NamedMatchList {
  std::string_view name;
  const JsonValue* const* items = nullptr;
  size_t count = 0;
};

std::string JsonSerialize(const JsonValue* value, const SerializeOptions& opts = {});

// Zero-copy streaming serialization of a JSON array of existing JsonValue*
// pointers (avoids cloning matched subtrees in JSON.GET / JSON.MGET).
std::string JsonSerializeArray(const JsonValue* const* items, size_t count,
                               const SerializeOptions& opts = {});

// Zero-copy streaming serialization of { path: [matches...], ... } for multi-path JSON.GET.
std::string JsonSerializeObjectOfArrays(const NamedMatchList* lists, size_t count,
                                        const SerializeOptions& opts = {});
