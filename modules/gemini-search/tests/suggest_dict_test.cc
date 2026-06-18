#include <gtest/gtest.h>
#include "suggest_dict.h"

// =============================================================
// SuggestDict
// =============================================================

TEST(SuggestDictTest, AddAndGet) {
  SuggestDict sd;
  sd.Add("hello world", 5.0);
  sd.Add("hello there", 3.0);
  sd.Add("help me", 1.0);
  sd.Add("goodbye", 2.0);

  auto results = sd.Get("hel", false, 10);
  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(results[0].str, "hello world");
  EXPECT_EQ(results[1].str, "hello there");
  EXPECT_EQ(results[2].str, "help me");
}

TEST(SuggestDictTest, MaxResults) {
  SuggestDict sd;
  sd.Add("aa", 1.0);
  sd.Add("ab", 2.0);
  sd.Add("ac", 3.0);
  sd.Add("ad", 4.0);

  auto results = sd.Get("a", false, 2);
  ASSERT_EQ(results.size(), 2u);
  EXPECT_DOUBLE_EQ(results[0].score, 4.0);
  EXPECT_DOUBLE_EQ(results[1].score, 3.0);
}

TEST(SuggestDictTest, IncrScore) {
  SuggestDict sd;
  sd.Add("hello", 5.0);
  sd.Add("hello", 3.0, true);
  auto results = sd.Get("hello", false, 10);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_DOUBLE_EQ(results[0].score, 8.0);
}

TEST(SuggestDictTest, ReplaceScore) {
  SuggestDict sd;
  sd.Add("hello", 5.0);
  sd.Add("hello", 3.0, false);
  auto results = sd.Get("hello", false, 10);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_DOUBLE_EQ(results[0].score, 3.0);
}

TEST(SuggestDictTest, Payload) {
  SuggestDict sd;
  sd.Add("hello", 1.0, false, "extra_data");
  auto results = sd.Get("hello", false, 10);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].payload, "extra_data");
}

TEST(SuggestDictTest, Del) {
  SuggestDict sd;
  sd.Add("hello", 1.0);
  sd.Add("help", 2.0);
  EXPECT_TRUE(sd.Del("hello"));
  EXPECT_FALSE(sd.Del("nonexistent"));
  EXPECT_EQ(sd.Len(), 1u);
  auto results = sd.Get("hel", false, 10);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].str, "help");
}

TEST(SuggestDictTest, Len) {
  SuggestDict sd;
  EXPECT_EQ(sd.Len(), 0u);
  sd.Add("a", 1.0);
  sd.Add("b", 2.0);
  EXPECT_EQ(sd.Len(), 2u);
  sd.Del("a");
  EXPECT_EQ(sd.Len(), 1u);
}

TEST(SuggestDictTest, CaseInsensitive) {
  SuggestDict sd;
  sd.Add("Hello", 1.0);
  auto results = sd.Get("hel", false, 10);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].str, "hello");
}

TEST(SuggestDictTest, FuzzyMatch) {
  SuggestDict sd;
  sd.Add("hello", 5.0);
  sd.Add("hallo", 3.0);
  sd.Add("world", 1.0);

  auto results = sd.Get("helo", true, 10);
  EXPECT_GE(results.size(), 1u);
  bool found_hello = false;
  for (auto& r : results) {
    if (r.str == "hello") found_hello = true;
  }
  EXPECT_TRUE(found_hello);
}

TEST(SuggestDictTest, EmptyPrefix) {
  SuggestDict sd;
  sd.Add("abc", 1.0);
  auto results = sd.Get("", false, 10);
  EXPECT_EQ(results.size(), 1u);
}

TEST(SuggestDictTest, NoMatch) {
  SuggestDict sd;
  sd.Add("hello", 1.0);
  auto results = sd.Get("xyz", false, 10);
  EXPECT_TRUE(results.empty());
}

// =============================================================
// TermDict
// =============================================================

TEST(TermDictTest, AddAndDump) {
  TermDict td;
  td.Add({"hello", "world", "test"});
  auto terms = td.Dump();
  ASSERT_EQ(terms.size(), 3u);
  EXPECT_EQ(terms[0], "hello");
  EXPECT_EQ(terms[1], "test");
  EXPECT_EQ(terms[2], "world");
}

TEST(TermDictTest, AddDuplicates) {
  TermDict td;
  size_t added = td.Add({"hello", "hello", "world"});
  EXPECT_EQ(added, 2u);
  EXPECT_EQ(td.Size(), 2u);
}

TEST(TermDictTest, Del) {
  TermDict td;
  td.Add({"hello", "world"});
  size_t removed = td.Del({"hello", "nonexistent"});
  EXPECT_EQ(removed, 1u);
  EXPECT_EQ(td.Size(), 1u);
  EXPECT_FALSE(td.Contains("hello"));
  EXPECT_TRUE(td.Contains("world"));
}

TEST(TermDictTest, Contains) {
  TermDict td;
  td.Add({"hello"});
  EXPECT_TRUE(td.Contains("hello"));
  EXPECT_FALSE(td.Contains("world"));
}

// =============================================================
// SynonymMap
// =============================================================

TEST(SynonymMapTest, UpdateAndExpand) {
  SynonymMap sm;
  sm.Update("1", {"happy", "glad", "joyful"});

  auto expanded = sm.Expand("happy");
  ASSERT_EQ(expanded.size(), 2u);
  EXPECT_EQ(expanded[0], "glad");
  EXPECT_EQ(expanded[1], "joyful");
}

TEST(SynonymMapTest, ExpandNonMember) {
  SynonymMap sm;
  sm.Update("1", {"happy", "glad"});
  auto expanded = sm.Expand("sad");
  EXPECT_TRUE(expanded.empty());
}

TEST(SynonymMapTest, MultipleGroups) {
  SynonymMap sm;
  sm.Update("1", {"happy", "glad"});
  sm.Update("2", {"happy", "cheerful"});

  auto expanded = sm.Expand("happy");
  ASSERT_EQ(expanded.size(), 2u);
  EXPECT_EQ(expanded[0], "cheerful");
  EXPECT_EQ(expanded[1], "glad");
}

TEST(SynonymMapTest, Dump) {
  SynonymMap sm;
  sm.Update("g1", {"a", "b"});
  sm.Update("g2", {"x", "y"});

  auto dump = sm.Dump();
  ASSERT_EQ(dump.size(), 2u);
  EXPECT_EQ(dump[0].first, "g1");
  EXPECT_EQ(dump[0].second.size(), 2u);
  EXPECT_EQ(dump[1].first, "g2");
}

TEST(SynonymMapTest, UpdateAddsToExisting) {
  SynonymMap sm;
  sm.Update("1", {"happy", "glad"});
  sm.Update("1", {"cheerful"});

  auto expanded = sm.Expand("happy");
  ASSERT_EQ(expanded.size(), 2u);
}
