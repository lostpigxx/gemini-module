#include "json_parse.h"
#include "json_path.h"
#include "json_serialize.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

constexpr size_t kMaxInputSize = 1U << 20;

[[noreturn]] void FuzzInvariantFailed() {
  __builtin_trap();
}

void FuzzDocument(std::string_view input) {
  auto parsed = JsonParse(input);
  if (!parsed.value) return;

  auto* clone = parsed.value->Clone();
  if (!clone || !parsed.value->DeepEqual(clone)) FuzzInvariantFailed();
  JsonValue::Destroy(clone);

  const std::string serialized = JsonSerialize(parsed.value);
  auto reparsed = JsonParse(serialized);
  if (!reparsed.value || !parsed.value->DeepEqual(reparsed.value)) {
    JsonValue::Destroy(reparsed.value);
    JsonValue::Destroy(parsed.value);
    FuzzInvariantFailed();
  }

  JsonValue::Destroy(reparsed.value);
  JsonValue::Destroy(parsed.value);
}

void FuzzPath(std::string_view input) {
  (void)ParsePath(input);
}

void FuzzDocumentAndPath(std::string_view document, std::string_view path) {
  auto parsed = JsonParse(document);
  if (!parsed.value) return;

  auto parsed_path = ParsePath(path);
  if (!parsed_path.error) {
    (void)EvaluatePath(parsed_path.path, parsed.value);
  }
  JsonValue::Destroy(parsed.value);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > kMaxInputSize) return 0;
  const auto* chars = reinterpret_cast<const char*>(data);
  const std::string_view input(chars, size);

  FuzzDocument(input);
  FuzzPath(input);

  size_t split = input.find('\n');
  size_t path_start = split;
  if (split == std::string_view::npos && size > 1) {
    split = data[0] % (size - 1);
    path_start = split;
  } else if (split != std::string_view::npos) {
    path_start = split + 1;
  }
  if (split != std::string_view::npos) {
    FuzzDocumentAndPath(input.substr(0, split), input.substr(path_start));
  }
  return 0;
}
