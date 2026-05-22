#pragma once

#include "json_value.h"
#include <string>

struct SerializeOptions {
  const char* indent = nullptr;
  const char* newline = nullptr;
  const char* space = nullptr;
};

std::string JsonSerialize(const JsonValue* value, const SerializeOptions& opts = {});
