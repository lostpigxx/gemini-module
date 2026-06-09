#include <gtest/gtest.h>
#include "text_index.h"

#include <algorithm>
#include <string>
#include <vector>

TEST(TextIndexTokenize, BasicTokenization) {
  auto tokens = TextIndex::Tokenize("Hello World");
  ASSERT_EQ(tokens.size(), 2u);
  EXPECT_EQ(tokens[0], "hello");
  EXPECT_EQ(tokens[1], "world");
}

TEST(TextIndexTokenize, PunctuationSplit) {
  auto tokens = TextIndex::Tokenize("hello, world! foo-bar");
  ASSERT_EQ(tokens.size(), 4u);
  EXPECT_EQ(tokens[0], "hello");
  EXPECT_EQ(tokens[1], "world");
  EXPECT_EQ(tokens[2], "foo");
  EXPECT_EQ(tokens[3], "bar");
}

TEST(TextIndexTokenize, StopWordsRemoved) {
  auto tokens = TextIndex::Tokenize("the quick brown fox is a test");
  // "the", "is", "a" are stop words
  ASSERT_EQ(tokens.size(), 4u);
  EXPECT_EQ(tokens[0], "quick");
  EXPECT_EQ(tokens[1], "brown");
  EXPECT_EQ(tokens[2], "fox");
  EXPECT_EQ(tokens[3], "test");
}

TEST(TextIndexTokenize, EmptyInput) {
  auto tokens = TextIndex::Tokenize("");
  EXPECT_TRUE(tokens.empty());
}

TEST(TextIndexTokenize, OnlyStopWords) {
  auto tokens = TextIndex::Tokenize("the a an is");
  EXPECT_TRUE(tokens.empty());
}

TEST(TextIndexTokenize, Numbers) {
  auto tokens = TextIndex::Tokenize("item123 456abc");
  ASSERT_EQ(tokens.size(), 2u);
  EXPECT_EQ(tokens[0], "item123");
  EXPECT_EQ(tokens[1], "456abc");
}

TEST(TextIndexTokenize, CaseNormalization) {
  auto tokens = TextIndex::Tokenize("UPPER lower MiXeD");
  ASSERT_EQ(tokens.size(), 3u);
  EXPECT_EQ(tokens[0], "upper");
  EXPECT_EQ(tokens[1], "lower");
  EXPECT_EQ(tokens[2], "mixed");
}

class TextIndexTest : public ::testing::Test {
 protected:
  TextIndex idx;

  void SetUp() override {
    idx.Add("d1", "the quick brown fox jumps over the lazy dog");
    idx.Add("d2", "a quick red car drives fast");
    idx.Add("d3", "the brown bear sleeps in the forest");
    idx.Add("d4", "quick quick quick fox fox");
  }
};

TEST_F(TextIndexTest, LookupSingleTerm) {
  auto ids = idx.Lookup("quick");
  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids[0], "d1");
  EXPECT_EQ(ids[1], "d2");
  EXPECT_EQ(ids[2], "d4");
}

TEST_F(TextIndexTest, LookupMissing) {
  auto ids = idx.Lookup("nonexistent");
  EXPECT_TRUE(ids.empty());
}

TEST_F(TextIndexTest, LookupStopWordIndexed) {
  // Stop words are indexed (for NOSTOPWORDS support) but filtered at query time
  auto ids = idx.Lookup("the");
  ASSERT_EQ(ids.size(), 2u);
  EXPECT_EQ(ids[0], "d1");
  EXPECT_EQ(ids[1], "d3");
}

TEST_F(TextIndexTest, SearchSingleTerm) {
  auto results = idx.Search({"fox"});
  ASSERT_EQ(results.size(), 2u);
  // d4 has higher TF for fox (2 occurrences in shorter doc)
  bool found_d1 = false, found_d4 = false;
  for (auto& r : results) {
    if (r.doc_id == "d1") found_d1 = true;
    if (r.doc_id == "d4") found_d4 = true;
    EXPECT_GT(r.score, 0.0);
  }
  EXPECT_TRUE(found_d1);
  EXPECT_TRUE(found_d4);
}

TEST_F(TextIndexTest, SearchMultipleTerms) {
  auto results = idx.Search({"quick", "fox"});
  ASSERT_GE(results.size(), 1u);
  // d1 and d4 contain both terms, should score higher
  EXPECT_TRUE(results[0].doc_id == "d4" || results[0].doc_id == "d1");
}

TEST_F(TextIndexTest, SearchBM25Ordering) {
  // "quick" appears 3 times in d4 (5 tokens total), once in d1 (7 tokens), once in d2 (5 tokens)
  auto results = idx.Search({"quick"});
  ASSERT_GE(results.size(), 3u);
  // d4 should rank highest due to highest TF
  EXPECT_EQ(results[0].doc_id, "d4");
}

TEST_F(TextIndexTest, SearchNoMatch) {
  auto results = idx.Search({"nonexistent"});
  EXPECT_TRUE(results.empty());
}

TEST_F(TextIndexTest, Remove) {
  idx.Remove("d1");
  auto ids = idx.Lookup("fox");
  ASSERT_EQ(ids.size(), 1u);
  EXPECT_EQ(ids[0], "d4");
}

TEST_F(TextIndexTest, RemoveNonexistent) {
  idx.Remove("d999");
  EXPECT_EQ(idx.NumDocs(), 4u);
}

TEST_F(TextIndexTest, AddReplace) {
  idx.Add("d1", "completely new content");
  auto ids = idx.Lookup("fox");
  ASSERT_EQ(ids.size(), 1u);
  EXPECT_EQ(ids[0], "d4");
  auto ids2 = idx.Lookup("completely");
  ASSERT_EQ(ids2.size(), 1u);
  EXPECT_EQ(ids2[0], "d1");
}

TEST_F(TextIndexTest, NumTermsAndDocs) {
  EXPECT_EQ(idx.NumDocs(), 4u);
  EXPECT_GT(idx.NumTerms(), 0u);
}

TEST_F(TextIndexTest, Clear) {
  idx.Clear();
  EXPECT_EQ(idx.NumDocs(), 0u);
  EXPECT_EQ(idx.NumTerms(), 0u);
  auto ids = idx.Lookup("quick");
  EXPECT_TRUE(ids.empty());
}

TEST(TextFieldIndicesTest, GetOrCreate) {
  TextFieldIndices indices;
  auto& idx1 = indices.GetOrCreate("title");
  idx1.Add("d1", "hello world");
  auto* idx2 = indices.Get("title");
  ASSERT_NE(idx2, nullptr);
  auto ids = idx2->Lookup("hello");
  ASSERT_EQ(ids.size(), 1u);
}

TEST(TextFieldIndicesTest, GetMissing) {
  TextFieldIndices indices;
  EXPECT_EQ(indices.Get("nonexistent"), nullptr);
}
