#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct GeoCoord {
  double lon = 0.0;
  double lat = 0.0;
};

enum class GeoUnit { kM, kKm, kMi, kFt };

inline double GeoUnitToMeters(GeoUnit unit) {
  switch (unit) {
    case GeoUnit::kM: return 1.0;
    case GeoUnit::kKm: return 1000.0;
    case GeoUnit::kMi: return 1609.344;
    case GeoUnit::kFt: return 0.3048;
  }
  return 1.0;
}

double HaversineDistance(double lon1, double lat1, double lon2, double lat2);

bool ParseGeoCoord(const std::string& value, GeoCoord& out);

bool ParseGeoUnit(const std::string& s, GeoUnit& out);

struct GeoResult {
  std::string doc_id;
  double distance;
};

class GeoIndex {
  std::unordered_map<std::string, GeoCoord> entries_;
  std::unordered_map<uint64_t, std::vector<std::string>> grid_;

  static uint64_t CellKey(int32_t cx, int32_t cy);
  static int32_t LonToCell(double lon);
  static int32_t LatToCell(double lat);
  void GridRemove(const std::string& doc_id, double lon, double lat);

 public:
  void Add(const std::string& doc_id, double lon, double lat);
  bool Remove(const std::string& doc_id);
  std::vector<GeoResult> RadiusQuery(double lon, double lat,
                                     double radius_meters) const;
  size_t Size() const;
  void Clear();
};

class GeoFieldIndices {
  std::unordered_map<std::string, GeoIndex> field_indices_;

 public:
  GeoIndex& GetOrCreate(const std::string& field_name);
  const GeoIndex* Get(const std::string& field_name) const;
  void Clear();
};
