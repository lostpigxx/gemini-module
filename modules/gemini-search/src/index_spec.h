#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

enum class FieldType { kTag, kNumeric, kVector, kText, kGeo };

inline const char* FieldTypeName(FieldType t) {
  switch (t) {
    case FieldType::kTag: return "TAG";
    case FieldType::kNumeric: return "NUMERIC";
    case FieldType::kVector: return "VECTOR";
    case FieldType::kText: return "TEXT";
    case FieldType::kGeo: return "GEO";
  }
  return "UNKNOWN";
}

enum class VectorAlgorithm { kFlat, kHnsw };

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
    case VectorAlgorithm::kHnsw: return "HNSW";
  }
  return "UNKNOWN";
}

enum class IndexSourceType { kHash, kJson };

struct VectorFieldParams {
  VectorAlgorithm algorithm = VectorAlgorithm::kFlat;
  size_t dim = 0;
  DistanceMetric metric = DistanceMetric::kL2;
  size_t m = 16;
  size_t ef_construction = 200;
};

struct FieldSpec {
  std::string name;
  FieldType type;
  VectorFieldParams vector_params;
  bool nostem = false;
  std::string json_path;
  bool sortable = false;
  bool noindex = false;
  double weight = 1.0;
};

struct IndexSpec {
  std::string name;
  std::vector<FieldSpec> fields;
  std::vector<std::string> prefixes;
  std::string language = "english";
  IndexSourceType source_type = IndexSourceType::kHash;
  std::vector<std::string> custom_stopwords;
  bool has_custom_stopwords = false;
  bool nofreqs = false;
  bool nooffsets = false;
  bool nohl = false;
  int temporary_ttl = 0;

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
