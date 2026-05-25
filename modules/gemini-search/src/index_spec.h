#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

enum class FieldType { kTag, kNumeric };

inline const char* FieldTypeName(FieldType t) {
  switch (t) {
    case FieldType::kTag: return "TAG";
    case FieldType::kNumeric: return "NUMERIC";
  }
  return "UNKNOWN";
}

struct FieldSpec {
  std::string name;
  FieldType type;
};

struct IndexSpec {
  std::string name;
  std::vector<FieldSpec> fields;

  const FieldSpec* FindField(const std::string& field_name) const {
    for (auto& f : fields) {
      if (f.name == field_name) return &f;
    }
    return nullptr;
  }
};

class IndexRegistry {
  std::unordered_map<std::string, IndexSpec> indices_;

 public:
  bool Create(std::string name, std::vector<FieldSpec> fields);
  bool Drop(const std::string& name);
  const IndexSpec* Get(const std::string& name) const;
  std::vector<std::string> List() const;
  size_t Size() const;
  void Clear();
};
