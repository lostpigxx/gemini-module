#include "geoshape_index.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

// =============================================================
// BBox
// =============================================================

bool BBox::Intersects(const BBox& o) const {
  return !(max_x < o.min_x || min_x > o.max_x ||
           max_y < o.min_y || min_y > o.max_y);
}

bool BBox::Contains(const BBox& o) const {
  return min_x <= o.min_x && max_x >= o.max_x &&
         min_y <= o.min_y && max_y >= o.max_y;
}

// =============================================================
// GeoShape
// =============================================================

BBox GeoShape::Bounds() const {
  if (points.empty()) return {};
  BBox b;
  b.min_x = b.max_x = points[0].x;
  b.min_y = b.max_y = points[0].y;
  for (size_t i = 1; i < points.size(); i++) {
    b.min_x = std::min(b.min_x, points[i].x);
    b.max_x = std::max(b.max_x, points[i].x);
    b.min_y = std::min(b.min_y, points[i].y);
    b.max_y = std::max(b.max_y, points[i].y);
  }
  return b;
}

bool GeoShape::ContainsPoint(double x, double y) const {
  if (kind == Kind::kPoint) {
    return points.size() == 1 && points[0].x == x && points[0].y == y;
  }
  // Ray casting algorithm
  bool inside = false;
  size_t n = points.size();
  for (size_t i = 0, j = n - 1; i < n; j = i++) {
    double xi = points[i].x, yi = points[i].y;
    double xj = points[j].x, yj = points[j].y;
    if (((yi > y) != (yj > y)) &&
        (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) {
      inside = !inside;
    }
  }
  return inside;
}

bool GeoShape::ContainsShape(const GeoShape& other) const {
  if (kind == Kind::kPoint) return false;
  for (auto& p : other.points) {
    if (!ContainsPoint(p.x, p.y)) return false;
  }
  return true;
}

static bool SegmentsIntersect(double ax1, double ay1, double ax2, double ay2,
                              double bx1, double by1, double bx2, double by2) {
  auto Cross = [](double ox, double oy, double ax, double ay, double bx, double by) {
    return (ax - ox) * (by - oy) - (ay - oy) * (bx - ox);
  };
  double d1 = Cross(bx1, by1, bx2, by2, ax1, ay1);
  double d2 = Cross(bx1, by1, bx2, by2, ax2, ay2);
  double d3 = Cross(ax1, ay1, ax2, ay2, bx1, by1);
  double d4 = Cross(ax1, ay1, ax2, ay2, bx2, by2);
  if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
      ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
    return true;
  return false;
}

bool GeoShape::IntersectsShape(const GeoShape& other) const {
  // Point vs anything
  if (kind == Kind::kPoint && !points.empty()) {
    if (other.kind == Kind::kPoint) {
      return !other.points.empty() &&
             points[0].x == other.points[0].x &&
             points[0].y == other.points[0].y;
    }
    return other.ContainsPoint(points[0].x, points[0].y);
  }
  if (other.kind == Kind::kPoint && !other.points.empty()) {
    return ContainsPoint(other.points[0].x, other.points[0].y);
  }

  // Check if any vertex of one polygon is inside the other
  for (auto& p : other.points) {
    if (ContainsPoint(p.x, p.y)) return true;
  }
  for (auto& p : points) {
    if (other.ContainsPoint(p.x, p.y)) return true;
  }

  // Check edge intersections
  size_t na = points.size(), nb = other.points.size();
  for (size_t i = 0; i < na; i++) {
    size_t ni = (i + 1) % na;
    for (size_t j = 0; j < nb; j++) {
      size_t nj = (j + 1) % nb;
      if (SegmentsIntersect(points[i].x, points[i].y,
                            points[ni].x, points[ni].y,
                            other.points[j].x, other.points[j].y,
                            other.points[nj].x, other.points[nj].y))
        return true;
    }
  }
  return false;
}

// =============================================================
// WKT Parser
// =============================================================

static void SkipWs(const char*& p) {
  while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
}

static bool ParseDouble(const char*& p, double& out) {
  SkipWs(p);
  char* ep = nullptr;
  out = std::strtod(p, &ep);
  if (ep == p) return false;
  p = ep;
  return true;
}

static bool MatchWord(const char*& p, const char* word) {
  SkipWs(p);
  size_t len = std::strlen(word);
  if (strncasecmp(p, word, len) != 0) return false;
  p += len;
  return true;
}

bool ParseWkt(const std::string& wkt, GeoShape& out) {
  const char* p = wkt.c_str();
  SkipWs(p);

  if (MatchWord(p, "POINT")) {
    SkipWs(p);
    if (*p != '(') return false;
    p++;
    double x, y;
    if (!ParseDouble(p, x) || !ParseDouble(p, y)) return false;
    SkipWs(p);
    if (*p != ')') return false;
    out.kind = GeoShape::Kind::kPoint;
    out.points = {{x, y}};
    return true;
  }

  if (MatchWord(p, "POLYGON")) {
    SkipWs(p);
    if (*p != '(') return false;
    p++;
    SkipWs(p);
    if (*p != '(') return false;
    p++;

    out.kind = GeoShape::Kind::kPolygon;
    out.points.clear();

    while (true) {
      double x, y;
      if (!ParseDouble(p, x) || !ParseDouble(p, y)) return false;
      out.points.push_back({x, y});
      SkipWs(p);
      if (*p == ',') { p++; continue; }
      if (*p == ')') { p++; break; }
      return false;
    }

    SkipWs(p);
    if (*p == ')') p++;

    if (out.points.size() < 3) return false;

    // Remove closing point if same as first
    if (out.points.size() >= 4 &&
        out.points.front().x == out.points.back().x &&
        out.points.front().y == out.points.back().y) {
      out.points.pop_back();
    }
    return out.points.size() >= 3;
  }

  return false;
}

// =============================================================
// GeoShapeIndex
// =============================================================

void GeoShapeIndex::Add(const std::string& doc_id, const GeoShape& shape) {
  entries_[doc_id] = {shape, shape.Bounds()};
}

bool GeoShapeIndex::Remove(const std::string& doc_id) {
  return entries_.erase(doc_id) > 0;
}

std::vector<std::string> GeoShapeIndex::Query(GeoShapeOp op,
                                              const GeoShape& query_shape) const {
  BBox qbox = query_shape.Bounds();
  std::vector<std::string> results;

  for (auto& [doc_id, entry] : entries_) {
    bool match = false;
    switch (op) {
      case GeoShapeOp::kWithin:
        if (qbox.Contains(entry.bbox))
          match = query_shape.ContainsShape(entry.shape);
        break;
      case GeoShapeOp::kContains:
        if (entry.bbox.Contains(qbox))
          match = entry.shape.ContainsShape(query_shape);
        break;
      case GeoShapeOp::kIntersects:
        if (entry.bbox.Intersects(qbox))
          match = entry.shape.IntersectsShape(query_shape);
        break;
      case GeoShapeOp::kDisjoint:
        if (!entry.bbox.Intersects(qbox))
          match = true;
        else
          match = !entry.shape.IntersectsShape(query_shape);
        break;
    }
    if (match) results.push_back(doc_id);
  }

  std::sort(results.begin(), results.end());
  return results;
}

size_t GeoShapeIndex::Size() const { return entries_.size(); }
void GeoShapeIndex::Clear() { entries_.clear(); }

GeoShapeIndex& GeoShapeFieldIndices::GetOrCreate(const std::string& field_name) {
  return field_indices_[field_name];
}

const GeoShapeIndex* GeoShapeFieldIndices::Get(const std::string& field_name) const {
  auto it = field_indices_.find(field_name);
  if (it == field_indices_.end()) return nullptr;
  return &it->second;
}

void GeoShapeFieldIndices::Clear() { field_indices_.clear(); }
