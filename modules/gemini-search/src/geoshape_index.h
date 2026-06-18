#pragma once

#include <string>
#include <unordered_map>
#include <vector>

struct GeoPoint {
  double x = 0;
  double y = 0;
};

struct BBox {
  double min_x = 0, min_y = 0, max_x = 0, max_y = 0;
  bool Intersects(const BBox& o) const;
  bool Contains(const BBox& o) const;
};

struct GeoShape {
  enum class Kind { kPoint, kPolygon };
  Kind kind = Kind::kPoint;
  std::vector<GeoPoint> points;

  BBox Bounds() const;
  bool ContainsPoint(double x, double y) const;
  bool ContainsShape(const GeoShape& other) const;
  bool IntersectsShape(const GeoShape& other) const;
};

enum class GeoShapeOp { kWithin, kContains, kIntersects, kDisjoint };

enum class GeoShapeCoord { kSpherical, kFlat };

bool ParseWkt(const std::string& wkt, GeoShape& out);

class GeoShapeIndex {
  struct Entry {
    GeoShape shape;
    BBox bbox;
  };
  std::unordered_map<std::string, Entry> entries_;

 public:
  void Add(const std::string& doc_id, const GeoShape& shape);
  bool Remove(const std::string& doc_id);
  std::vector<std::string> Query(GeoShapeOp op, const GeoShape& query_shape) const;
  size_t Size() const;
  void Clear();
};

class GeoShapeFieldIndices {
  std::unordered_map<std::string, GeoShapeIndex> field_indices_;

 public:
  GeoShapeIndex& GetOrCreate(const std::string& field_name);
  const GeoShapeIndex* Get(const std::string& field_name) const;
  void Clear();
};
