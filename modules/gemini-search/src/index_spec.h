#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

enum class FieldType { kTag, kNumeric, kVector, kText };

inline const char* FieldTypeName(FieldType t) {
  switch (t) {
    case FieldType::kTag: return "TAG";
    case FieldType::kNumeric: return "NUMERIC";
    case FieldType::kVector: return "VECTOR";
    case FieldType::kText: return "TEXT";
  }
  return "UNKNOWN";
}

enum class VectorAlgorithm { kFlat };

enum class DistanceMetric { kL2, kCosine, kIP };

inline const char* DistanceMetricName(DistanceMetric m) {
  switch (m) {
    case DistanceMetric::kL2: return "L2";
    case DistanceMetric::kCosine: return "COSINE";
    case DistanceMetric::kIP: return "IP";
  }
  return "UNKNOWN";
}

inline const char* VectorAlgorithmName(VectorAlgorithm a) {
  switch (a) {
    case VectorAlgorithm::kFlat: return "FLAT";
  }
  return "UNKNOWN";
}

struct VectorFieldParams {
  VectorAlgorithm algorithm = VectorAlgorithm::kFlat;
  size_t dim = 0;
  DistanceMetric metric = DistanceMetric::kL2;
};

struct FieldSpec {
  std::string name;
  FieldType type;
  VectorFieldParams vector_params;
};

struct IndexSpec {
  std::string name;
  std::vector<FieldSpec> fields;
  std::vector<std::string> prefixes;

  bool HasPrefixes() const { return !prefixes.empty(); }

  bool MatchesPrefix(const std::string& key) const {
    if (prefixes.empty()) return false;
    for (auto& p : prefixes) {
      if (key.size() >= p.size() && key.compare(0, p.size(), p) == 0) return true;
    }
    return false;
  }

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
