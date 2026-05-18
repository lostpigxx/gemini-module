#pragma once

#include "json_value.h"
#include <string_view>

struct ParseResult {
  JsonValue* value = nullptr;
  const char* error = nullptr;
  size_t error_offset = 0;
};

ParseResult JsonParse(std::string_view input);
