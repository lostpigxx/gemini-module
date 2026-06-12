#include <gtest/gtest.h>
#include "geo_index.h"

#include <cmath>
#include <string>
#include <vector>

// =============================================================
// HaversineDistance
// =============================================================

TEST(HaversineTest, SamePointIsZero) {
  EXPECT_DOUBLE_EQ(HaversineDistance(0, 0, 0, 0), 0.0);
  EXPECT_DOUBLE_EQ(HaversineDistance(13.4, 52.5, 13.4, 52.5), 0.0);
}

TEST(HaversineTest, KnownDistance) {
  // Berlin (13.405, 52.52) to Munich (11.582, 48.135) ≈ 504 km
  double d = HaversineDistance(13.405, 52.52, 11.582, 48.135);
  EXPECT_NEAR(d, 504000.0, 5000.0);
}

TEST(HaversineTest, AntipodalPoints) {
  // (0,0) to (180,0) ≈ half earth circumference ≈ 20015 km
  double d = HaversineDistance(0, 0, 180, 0);
  EXPECT_NEAR(d, 20015000.0, 100000.0);
}

// =============================================================
// ParseGeoCoord
// =============================================================

TEST(ParseGeoCoordTest, ValidCoord) {
  GeoCoord c;
  EXPECT_TRUE(ParseGeoCoord("13.405,52.52", c));
  EXPECT_DOUBLE_EQ(c.lon, 13.405);
  EXPECT_DOUBLE_EQ(c.lat, 52.52);
}

TEST(ParseGeoCoordTest, NegativeCoords) {
  GeoCoord c;
  EXPECT_TRUE(ParseGeoCoord("-122.41,37.77", c));
  EXPECT_DOUBLE_EQ(c.lon, -122.41);
  EXPECT_DOUBLE_EQ(c.lat, 37.77);
}

TEST(ParseGeoCoordTest, InvalidNoComma) {
  GeoCoord c;
  EXPECT_FALSE(ParseGeoCoord("13.405 52.52", c));
}

TEST(ParseGeoCoordTest, InvalidNonNumeric) {
  GeoCoord c;
  EXPECT_FALSE(ParseGeoCoord("abc,def", c));
}

TEST(ParseGeoCoordTest, InvalidOutOfRange) {
  GeoCoord c;
  EXPECT_FALSE(ParseGeoCoord("200.0,100.0", c));
  EXPECT_FALSE(ParseGeoCoord("0.0,-91.0", c));
  EXPECT_FALSE(ParseGeoCoord("-181.0,0.0", c));
}

TEST(ParseGeoCoordTest, BoundaryValues) {
  GeoCoord c;
  EXPECT_TRUE(ParseGeoCoord("-180.0,-90.0", c));
  EXPECT_TRUE(ParseGeoCoord("180.0,90.0", c));
}

// =============================================================
// ParseGeoUnit
// =============================================================

TEST(ParseGeoUnitTest, AllUnits) {
  GeoUnit u;
  EXPECT_TRUE(ParseGeoUnit("m", u));
  EXPECT_EQ(u, GeoUnit::kM);
  EXPECT_TRUE(ParseGeoUnit("km", u));
  EXPECT_EQ(u, GeoUnit::kKm);
  EXPECT_TRUE(ParseGeoUnit("mi", u));
  EXPECT_EQ(u, GeoUnit::kMi);
  EXPECT_TRUE(ParseGeoUnit("ft", u));
  EXPECT_EQ(u, GeoUnit::kFt);
}

TEST(ParseGeoUnitTest, Invalid) {
  GeoUnit u;
  EXPECT_FALSE(ParseGeoUnit("yard", u));
  EXPECT_FALSE(ParseGeoUnit("", u));
}

// =============================================================
// GeoUnitToMeters
// =============================================================

TEST(GeoUnitToMetersTest, ConversionFactors) {
  EXPECT_DOUBLE_EQ(GeoUnitToMeters(GeoUnit::kM), 1.0);
  EXPECT_DOUBLE_EQ(GeoUnitToMeters(GeoUnit::kKm), 1000.0);
  EXPECT_DOUBLE_EQ(GeoUnitToMeters(GeoUnit::kMi), 1609.344);
  EXPECT_DOUBLE_EQ(GeoUnitToMeters(GeoUnit::kFt), 0.3048);
}

// =============================================================
// GeoIndex
// =============================================================

TEST(GeoIndexTest, AddAndRadiusQuery) {
  GeoIndex idx;
  // San Francisco area
  idx.Add("sf", -122.4194, 37.7749);
  idx.Add("oakland", -122.2711, 37.8044);
  idx.Add("sanjose", -121.8863, 37.3382);
  idx.Add("nyc", -74.0060, 40.7128);

  // 30 km radius from SF center
  auto results = idx.RadiusQuery(-122.4194, 37.7749, 30000);
  ASSERT_EQ(results.size(), 2u);
  // Closest first
  EXPECT_EQ(results[0].doc_id, "sf");
  EXPECT_NEAR(results[0].distance, 0.0, 1.0);
  EXPECT_EQ(results[1].doc_id, "oakland");
}

TEST(GeoIndexTest, EmptyIndexReturnsEmpty) {
  GeoIndex idx;
  auto results = idx.RadiusQuery(0, 0, 1000);
  EXPECT_TRUE(results.empty());
}

TEST(GeoIndexTest, ZeroRadiusExactMatch) {
  GeoIndex idx;
  idx.Add("origin", 0, 0);
  auto results = idx.RadiusQuery(0, 0, 0);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].doc_id, "origin");
}

TEST(GeoIndexTest, LargeRadiusIncludesAll) {
  GeoIndex idx;
  idx.Add("a", 0, 0);
  idx.Add("b", 90, 45);
  idx.Add("c", -120, -30);
  // 25000 km should include everything
  auto results = idx.RadiusQuery(0, 0, 25000000);
  EXPECT_EQ(results.size(), 3u);
}

TEST(GeoIndexTest, Remove) {
  GeoIndex idx;
  idx.Add("a", 0, 0);
  idx.Add("b", 0, 0);
  EXPECT_TRUE(idx.Remove("a"));
  EXPECT_FALSE(idx.Remove("nonexistent"));
  auto results = idx.RadiusQuery(0, 0, 100);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].doc_id, "b");
}

TEST(GeoIndexTest, UpdateExistingDoc) {
  GeoIndex idx;
  idx.Add("doc1", 0, 0);
  idx.Add("doc1", 90, 45);
  EXPECT_EQ(idx.Size(), 1u);
  // doc1 should now be at (90,45), not (0,0)
  auto results = idx.RadiusQuery(0, 0, 100);
  EXPECT_TRUE(results.empty());
  results = idx.RadiusQuery(90, 45, 100);
  ASSERT_EQ(results.size(), 1u);
}

TEST(GeoIndexTest, SizeTracking) {
  GeoIndex idx;
  EXPECT_EQ(idx.Size(), 0u);
  idx.Add("a", 0, 0);
  EXPECT_EQ(idx.Size(), 1u);
  idx.Add("b", 1, 1);
  EXPECT_EQ(idx.Size(), 2u);
  idx.Remove("a");
  EXPECT_EQ(idx.Size(), 1u);
}

TEST(GeoIndexTest, Clear) {
  GeoIndex idx;
  idx.Add("a", 0, 0);
  idx.Add("b", 1, 1);
  idx.Clear();
  EXPECT_EQ(idx.Size(), 0u);
  EXPECT_TRUE(idx.RadiusQuery(0, 0, 100000).empty());
}

TEST(GeoIndexTest, ResultsSortedByDistance) {
  GeoIndex idx;
  idx.Add("far", 10, 10);
  idx.Add("near", 0.001, 0.001);
  idx.Add("mid", 5, 5);

  auto results = idx.RadiusQuery(0, 0, 2000000);
  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(results[0].doc_id, "near");
  EXPECT_EQ(results[1].doc_id, "mid");
  EXPECT_EQ(results[2].doc_id, "far");
  EXPECT_LT(results[0].distance, results[1].distance);
  EXPECT_LT(results[1].distance, results[2].distance);
}

// =============================================================
// GeoFieldIndices
// =============================================================

TEST(GeoFieldIndicesTest, GetOrCreate) {
  GeoFieldIndices indices;
  auto& idx = indices.GetOrCreate("location");
  idx.Add("doc1", 13.4, 52.5);
  auto results = idx.RadiusQuery(13.4, 52.5, 100);
  EXPECT_EQ(results.size(), 1u);
}

TEST(GeoFieldIndicesTest, GetNonexistentReturnsNull) {
  GeoFieldIndices indices;
  EXPECT_EQ(indices.Get("nope"), nullptr);
}

TEST(GeoFieldIndicesTest, GetExistingField) {
  GeoFieldIndices indices;
  indices.GetOrCreate("loc").Add("doc1", 0, 0);
  const auto* idx = indices.Get("loc");
  ASSERT_NE(idx, nullptr);
  EXPECT_EQ(idx->RadiusQuery(0, 0, 100).size(), 1u);
}

TEST(GeoFieldIndicesTest, Clear) {
  GeoFieldIndices indices;
  indices.GetOrCreate("a").Add("d1", 0, 0);
  indices.GetOrCreate("b").Add("d2", 1, 1);
  indices.Clear();
  EXPECT_EQ(indices.Get("a"), nullptr);
  EXPECT_EQ(indices.Get("b"), nullptr);
}
