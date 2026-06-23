#include <gtest/gtest.h>
#include "phonetic.h"

TEST(DoubleMetaphoneTest, BasicEncodings) {
  auto [p1, p2] = DoubleMetaphone("Smith");
  EXPECT_FALSE(p1.empty());
  EXPECT_FALSE(p2.empty());
}

TEST(DoubleMetaphoneTest, JohnAndJon) {
  auto [j1_p, j1_s] = DoubleMetaphone("John");
  auto [j2_p, j2_s] = DoubleMetaphone("Jon");
  EXPECT_EQ(j1_p, j2_p);
}

TEST(DoubleMetaphoneTest, ShaunAndShawn) {
  auto [s1_p, s1_s] = DoubleMetaphone("Shaun");
  auto [s2_p, s2_s] = DoubleMetaphone("Shawn");
  EXPECT_EQ(s1_p, s2_p);
}

TEST(DoubleMetaphoneTest, StephenAndSteven) {
  auto [s1_p, s1_s] = DoubleMetaphone("Stephen");
  auto [s2_p, s2_s] = DoubleMetaphone("Steven");
  bool match = (s1_p == s2_p || s1_p == s2_s || s1_s == s2_p || s1_s == s2_s);
  EXPECT_TRUE(match);
}

TEST(DoubleMetaphoneTest, EmptyInput) {
  auto [p, s] = DoubleMetaphone("");
  EXPECT_TRUE(p.empty());
  EXPECT_TRUE(s.empty());
}

TEST(DoubleMetaphoneTest, SingleChar) {
  auto [p, s] = DoubleMetaphone("A");
  EXPECT_EQ(p, "A");
}

TEST(DoubleMetaphoneTest, CaseInsensitive) {
  auto [p1, s1] = DoubleMetaphone("SMITH");
  auto [p2, s2] = DoubleMetaphone("smith");
  EXPECT_EQ(p1, p2);
  EXPECT_EQ(s1, s2);
}

TEST(DoubleMetaphoneTest, MaxLength4) {
  auto [p, s] = DoubleMetaphone("Abrahamson");
  EXPECT_LE(p.size(), 4u);
  EXPECT_LE(s.size(), 4u);
}

TEST(DoubleMetaphoneTest, SilentInitials) {
  auto [p1, s1] = DoubleMetaphone("Knight");
  auto [p2, s2] = DoubleMetaphone("Night");
  EXPECT_EQ(p1, p2);
}

TEST(DoubleMetaphoneTest, PhoneWithPh) {
  auto [p, s] = DoubleMetaphone("Phone");
  EXPECT_EQ(p[0], 'F');
}
