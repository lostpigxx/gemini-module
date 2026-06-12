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

void GeoIndex::Add(const std::string& doc_id, double lon, double lat) {
  entries_[doc_id] = {lon, lat};
}

bool GeoIndex::Remove(const std::string& doc_id) {
  return entries_.erase(doc_id) > 0;
}

std::vector<GeoResult> GeoIndex::RadiusQuery(double lon, double lat,
                                             double radius_meters) const {
  std::vector<GeoResult> results;
  for (auto& [doc_id, coord] : entries_) {
    double dist = HaversineDistance(lon, lat, coord.lon, coord.lat);
    if (dist <= radius_meters) {
      results.push_back({doc_id, dist});
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

void GeoIndex::Clear() { entries_.clear(); }

GeoIndex& GeoFieldIndices::GetOrCreate(const std::string& field_name) {
  return field_indices_[field_name];
}

const GeoIndex* GeoFieldIndices::Get(const std::string& field_name) const {
  auto it = field_indices_.find(field_name);
  if (it == field_indices_.end()) return nullptr;
  return &it->second;
}

void GeoFieldIndices::Clear() { field_indices_.clear(); }
