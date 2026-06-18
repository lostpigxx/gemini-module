#include <gtest/gtest.h>
#include "geoshape_index.h"

#include <string>
#include <vector>

// =============================================================
// WKT Parsing
// =============================================================

TEST(ParseWktTest, Point) {
  GeoShape shape;
  EXPECT_TRUE(ParseWkt("POINT(10 20)", shape));
  EXPECT_EQ(shape.kind, GeoShape::Kind::kPoint);
  ASSERT_EQ(shape.points.size(), 1u);
  EXPECT_DOUBLE_EQ(shape.points[0].x, 10.0);
  EXPECT_DOUBLE_EQ(shape.points[0].y, 20.0);
}

TEST(ParseWktTest, PointCaseInsensitive) {
  GeoShape shape;
  EXPECT_TRUE(ParseWkt("point(5 15)", shape));
  EXPECT_EQ(shape.kind, GeoShape::Kind::kPoint);
}

TEST(ParseWktTest, Polygon) {
  GeoShape shape;
  EXPECT_TRUE(ParseWkt("POLYGON((0 0, 10 0, 10 10, 0 10, 0 0))", shape));
  EXPECT_EQ(shape.kind, GeoShape::Kind::kPolygon);
  EXPECT_EQ(shape.points.size(), 4u);
}

TEST(ParseWktTest, PolygonWithSpaces) {
  GeoShape shape;
  EXPECT_TRUE(ParseWkt("POLYGON(( 0 0 , 5 0 , 5 5 , 0 5 , 0 0 ))", shape));
  EXPECT_EQ(shape.kind, GeoShape::Kind::kPolygon);
  EXPECT_EQ(shape.points.size(), 4u);
}

TEST(ParseWktTest, InvalidEmpty) {
  GeoShape shape;
  EXPECT_FALSE(ParseWkt("", shape));
}

TEST(ParseWktTest, InvalidNoParens) {
  GeoShape shape;
  EXPECT_FALSE(ParseWkt("POINT 10 20", shape));
}

TEST(ParseWktTest, InvalidUnknownType) {
  GeoShape shape;
  EXPECT_FALSE(ParseWkt("LINE(0 0, 1 1)", shape));
}

// =============================================================
// GeoShape operations
// =============================================================

TEST(GeoShapeTest, PointInPolygon) {
  GeoShape poly;
  poly.kind = GeoShape::Kind::kPolygon;
  poly.points = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};

  EXPECT_TRUE(poly.ContainsPoint(5, 5));
  EXPECT_TRUE(poly.ContainsPoint(1, 1));
  EXPECT_FALSE(poly.ContainsPoint(15, 5));
  EXPECT_FALSE(poly.ContainsPoint(-1, 5));
}

TEST(GeoShapeTest, PointDoesNotContainAnything) {
  GeoShape pt;
  pt.kind = GeoShape::Kind::kPoint;
  pt.points = {{5, 5}};

  GeoShape other;
  other.kind = GeoShape::Kind::kPoint;
  other.points = {{5, 5}};

  EXPECT_FALSE(pt.ContainsShape(other));
}

TEST(GeoShapeTest, PolygonContainsPolygon) {
  GeoShape outer;
  outer.kind = GeoShape::Kind::kPolygon;
  outer.points = {{0, 0}, {20, 0}, {20, 20}, {0, 20}};

  GeoShape inner;
  inner.kind = GeoShape::Kind::kPolygon;
  inner.points = {{5, 5}, {15, 5}, {15, 15}, {5, 15}};

  EXPECT_TRUE(outer.ContainsShape(inner));
  EXPECT_FALSE(inner.ContainsShape(outer));
}

TEST(GeoShapeTest, PolygonsIntersect) {
  GeoShape a;
  a.kind = GeoShape::Kind::kPolygon;
  a.points = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};

  GeoShape b;
  b.kind = GeoShape::Kind::kPolygon;
  b.points = {{5, 5}, {15, 5}, {15, 15}, {5, 15}};

  EXPECT_TRUE(a.IntersectsShape(b));
  EXPECT_TRUE(b.IntersectsShape(a));
}

TEST(GeoShapeTest, DisjointPolygons) {
  GeoShape a;
  a.kind = GeoShape::Kind::kPolygon;
  a.points = {{0, 0}, {5, 0}, {5, 5}, {0, 5}};

  GeoShape b;
  b.kind = GeoShape::Kind::kPolygon;
  b.points = {{10, 10}, {15, 10}, {15, 15}, {10, 15}};

  EXPECT_FALSE(a.IntersectsShape(b));
  EXPECT_FALSE(b.IntersectsShape(a));
}

TEST(GeoShapeTest, PointIntersectsPolygon) {
  GeoShape pt;
  pt.kind = GeoShape::Kind::kPoint;
  pt.points = {{5, 5}};

  GeoShape poly;
  poly.kind = GeoShape::Kind::kPolygon;
  poly.points = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};

  EXPECT_TRUE(pt.IntersectsShape(poly));
  EXPECT_TRUE(poly.IntersectsShape(pt));
}

TEST(GeoShapeTest, BBox) {
  GeoShape poly;
  poly.kind = GeoShape::Kind::kPolygon;
  poly.points = {{1, 2}, {5, 8}, {3, 4}};
  auto bb = poly.Bounds();
  EXPECT_DOUBLE_EQ(bb.min_x, 1.0);
  EXPECT_DOUBLE_EQ(bb.min_y, 2.0);
  EXPECT_DOUBLE_EQ(bb.max_x, 5.0);
  EXPECT_DOUBLE_EQ(bb.max_y, 8.0);
}

// =============================================================
// GeoShapeIndex
// =============================================================

TEST(GeoShapeIndexTest, WithinQuery) {
  GeoShapeIndex idx;
  GeoShape inner;
  inner.kind = GeoShape::Kind::kPolygon;
  inner.points = {{2, 2}, {4, 2}, {4, 4}, {2, 4}};
  idx.Add("inner", inner);

  GeoShape outer;
  outer.kind = GeoShape::Kind::kPolygon;
  outer.points = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
  idx.Add("outer", outer);

  GeoShape query;
  query.kind = GeoShape::Kind::kPolygon;
  query.points = {{0, 0}, {5, 0}, {5, 5}, {0, 5}};

  auto results = idx.Query(GeoShapeOp::kWithin, query);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0], "inner");
}

TEST(GeoShapeIndexTest, ContainsQuery) {
  GeoShapeIndex idx;
  GeoShape big;
  big.kind = GeoShape::Kind::kPolygon;
  big.points = {{0, 0}, {20, 0}, {20, 20}, {0, 20}};
  idx.Add("big", big);

  GeoShape small_q;
  small_q.kind = GeoShape::Kind::kPolygon;
  small_q.points = {{5, 5}, {15, 5}, {15, 15}, {5, 15}};

  auto results = idx.Query(GeoShapeOp::kContains, small_q);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0], "big");
}

TEST(GeoShapeIndexTest, IntersectsQuery) {
  GeoShapeIndex idx;
  GeoShape a;
  a.kind = GeoShape::Kind::kPolygon;
  a.points = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
  idx.Add("a", a);

  GeoShape far;
  far.kind = GeoShape::Kind::kPolygon;
  far.points = {{100, 100}, {110, 100}, {110, 110}, {100, 110}};
  idx.Add("far", far);

  GeoShape query;
  query.kind = GeoShape::Kind::kPolygon;
  query.points = {{5, 5}, {15, 5}, {15, 15}, {5, 15}};

  auto results = idx.Query(GeoShapeOp::kIntersects, query);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0], "a");
}

TEST(GeoShapeIndexTest, DisjointQuery) {
  GeoShapeIndex idx;
  GeoShape a;
  a.kind = GeoShape::Kind::kPolygon;
  a.points = {{0, 0}, {5, 0}, {5, 5}, {0, 5}};
  idx.Add("a", a);

  GeoShape b;
  b.kind = GeoShape::Kind::kPolygon;
  b.points = {{20, 20}, {25, 20}, {25, 25}, {20, 25}};
  idx.Add("b", b);

  GeoShape query;
  query.kind = GeoShape::Kind::kPolygon;
  query.points = {{10, 10}, {15, 10}, {15, 15}, {10, 15}};

  auto results = idx.Query(GeoShapeOp::kDisjoint, query);
  ASSERT_EQ(results.size(), 2u);
}

TEST(GeoShapeIndexTest, RemoveAndSize) {
  GeoShapeIndex idx;
  GeoShape pt;
  pt.kind = GeoShape::Kind::kPoint;
  pt.points = {{1, 1}};
  idx.Add("p1", pt);
  idx.Add("p2", pt);
  EXPECT_EQ(idx.Size(), 2u);
  EXPECT_TRUE(idx.Remove("p1"));
  EXPECT_EQ(idx.Size(), 1u);
  EXPECT_FALSE(idx.Remove("nonexistent"));
}

TEST(GeoShapeIndexTest, Clear) {
  GeoShapeIndex idx;
  GeoShape pt;
  pt.kind = GeoShape::Kind::kPoint;
  pt.points = {{0, 0}};
  idx.Add("a", pt);
  idx.Clear();
  EXPECT_EQ(idx.Size(), 0u);
}

// =============================================================
// GeoShapeFieldIndices
// =============================================================

TEST(GeoShapeFieldIndicesTest, GetOrCreate) {
  GeoShapeFieldIndices indices;
  GeoShape pt;
  pt.kind = GeoShape::Kind::kPoint;
  pt.points = {{1, 2}};
  indices.GetOrCreate("area").Add("doc1", pt);
  EXPECT_EQ(indices.GetOrCreate("area").Size(), 1u);
}

TEST(GeoShapeFieldIndicesTest, GetNonexistent) {
  GeoShapeFieldIndices indices;
  EXPECT_EQ(indices.Get("nope"), nullptr);
}
