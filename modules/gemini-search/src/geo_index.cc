#include "geo_index.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

static constexpr double kEarthRadiusMeters = 6372797.560856;
static constexpr double kDegToRad = M_PI / 180.0;

double HaversineDistance(double lon1, double lat1, double lon2, double lat2) {
  double dlat = (lat2 - lat1) * kDegToRad;
  double dlon = (lon2 - lon1) * kDegToRad;
  double a = std::sin(dlat / 2.0) * std::sin(dlat / 2.0) +
             std::cos(lat1 * kDegToRad) * std::cos(lat2 * kDegToRad) *
             std::sin(dlon / 2.0) * std::sin(dlon / 2.0);
  double c = 2.0 * std::asin(std::sqrt(a));
  return kEarthRadiusMeters * c;
}

bool ParseGeoCoord(const std::string& value, GeoCoord& out) {
  auto sep = value.find(',');
  if (sep == std::string::npos) return false;
  std::string lon_s = value.substr(0, sep);
  std::string lat_s = value.substr(sep + 1);
  char* ep1 = nullptr;
  char* ep2 = nullptr;
  double lon = std::strtod(lon_s.c_str(), &ep1);
  double lat = std::strtod(lat_s.c_str(), &ep2);
  if (ep1 == lon_s.c_str() || *ep1 != '\0') return false;
  if (ep2 == lat_s.c_str() || *ep2 != '\0') return false;
  if (std::isnan(lon) || std::isnan(lat)) return false;
  if (lon < -180.0 || lon > 180.0 || lat < -90.0 || lat > 90.0) return false;
  out.lon = lon;
  out.lat = lat;
  return true;
}

bool ParseGeoUnit(const std::string& s, GeoUnit& out) {
  if (s == "m") { out = GeoUnit::kM; return true; }
  if (s == "km") { out = GeoUnit::kKm; return true; }
  if (s == "mi") { out = GeoUnit::kMi; return true; }
  if (s == "ft") { out = GeoUnit::kFt; return true; }
  return false;
}

static constexpr double kCellDeg = 0.5;
static constexpr int32_t kNumLonCells = 720;  // 360 / 0.5
static constexpr int32_t kNumLatCells = 360;  // 180 / 0.5

uint64_t GeoIndex::CellKey(int32_t cx, int32_t cy) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) |
         static_cast<uint32_t>(cy);
}

int32_t GeoIndex::LonToCell(double lon) {
  int32_t c = static_cast<int32_t>(std::floor((lon + 180.0) / kCellDeg));
  if (c < 0) c = 0;
  if (c >= kNumLonCells) c = kNumLonCells - 1;
  return c;
}

int32_t GeoIndex::LatToCell(double lat) {
  int32_t c = static_cast<int32_t>(std::floor((lat + 90.0) / kCellDeg));
  if (c < 0) c = 0;
  if (c >= kNumLatCells) c = kNumLatCells - 1;
  return c;
}

void GeoIndex::GridRemove(const std::string& doc_id, double lon, double lat) {
  uint64_t key = CellKey(LonToCell(lon), LatToCell(lat));
  auto git = grid_.find(key);
  if (git == grid_.end()) return;
  auto& vec = git->second;
  vec.erase(std::remove(vec.begin(), vec.end(), doc_id), vec.end());
  if (vec.empty()) grid_.erase(git);
}

void GeoIndex::Add(const std::string& doc_id, double lon, double lat) {
  auto it = entries_.find(doc_id);
  if (it != entries_.end()) {
    GridRemove(doc_id, it->second.lon, it->second.lat);
    it->second = {lon, lat};
  } else {
    entries_.emplace(doc_id, GeoCoord{lon, lat});
  }
  grid_[CellKey(LonToCell(lon), LatToCell(lat))].push_back(doc_id);
}

bool GeoIndex::Remove(const std::string& doc_id) {
  auto it = entries_.find(doc_id);
  if (it == entries_.end()) return false;
  GridRemove(doc_id, it->second.lon, it->second.lat);
  entries_.erase(it);
  return true;
}

std::vector<GeoResult> GeoIndex::RadiusQuery(double lon, double lat,
                                             double radius_meters) const {
  std::vector<GeoResult> results;
  double dlat_deg = (radius_meters / kEarthRadiusMeters) * (180.0 / M_PI) + 0.01;
  bool use_grid = (std::abs(lat) + dlat_deg < 89.0 && dlat_deg <= 15.0);
  double dlon_deg = 0.0;
  if (use_grid) {
    double cos_lat = std::cos(lat * kDegToRad);
    if (cos_lat < 0.05) {
      use_grid = false;
    } else {
      dlon_deg = dlat_deg / cos_lat + 0.01;
      if (dlon_deg > 20.0) use_grid = false;
    }
  }

  if (use_grid) {
    int32_t cy_min = LatToCell(std::max(-90.0, lat - dlat_deg));
    int32_t cy_max = LatToCell(std::min(90.0, lat + dlat_deg));
    int32_t cx_start = static_cast<int32_t>(std::floor((lon - dlon_deg + 180.0) / kCellDeg));
    int32_t cx_end = static_cast<int32_t>(std::floor((lon + dlon_deg + 180.0) / kCellDeg));

    for (int32_t cy = cy_min; cy <= cy_max; cy++) {
      for (int32_t cx_raw = cx_start; cx_raw <= cx_end; cx_raw++) {
        int32_t cx = ((cx_raw % kNumLonCells) + kNumLonCells) % kNumLonCells;
        auto git = grid_.find(CellKey(cx, cy));
        if (git == grid_.end()) continue;
        for (const auto& doc_id : git->second) {
          const auto& coord = entries_.at(doc_id);
          double dist = HaversineDistance(lon, lat, coord.lon, coord.lat);
          if (dist <= radius_meters) {
            results.push_back({doc_id, dist});
          }
        }
      }
    }
  } else {
    for (const auto& [doc_id, coord] : entries_) {
      double dist = HaversineDistance(lon, lat, coord.lon, coord.lat);
      if (dist <= radius_meters) {
        results.push_back({doc_id, dist});
      }
    }
  }

  std::sort(results.begin(), results.end(),
            [](const GeoResult& a, const GeoResult& b) {
              if (a.distance != b.distance) return a.distance < b.distance;
              return a.doc_id < b.doc_id;
            });
  return results;
}

size_t GeoIndex::Size() const { return entries_.size(); }

void GeoIndex::Clear() {
  entries_.clear();
  grid_.clear();
}

GeoIndex& GeoFieldIndices::GetOrCreate(const std::string& field_name) {
  return field_indices_[field_name];
}

const GeoIndex* GeoFieldIndices::Get(const std::string& field_name) const {
  auto it = field_indices_.find(field_name);
  if (it == field_indices_.end()) return nullptr;
  return &it->second;
}

void GeoFieldIndices::Clear() { field_indices_.clear(); }
