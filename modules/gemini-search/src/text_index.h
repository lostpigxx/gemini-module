#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

struct TextSearchResult {
  std::string doc_id;
  double score;
};

class TextIndex {
  struct Posting {
    std::string doc_id;
    int tf;
    std::vector<int> positions;
  };

  std::unordered_map<std::string, std::vector<Posting>> postings_;
  std::unordered_map<std::string, std::set<std::string>> stem_map_;
  std::unordered_map<std::string, std::set<std::string>> phonetic_map_;
  std::unordered_map<std::string, int> doc_lengths_;
  double avg_doc_len_ = 0;

  void UpdateAvgDocLen();

 public:
  void Add(const std::string& doc_id, const std::string& text,
           bool phonetic = false, const std::string& language = "english",
           bool nofreqs = false, bool nooffsets = false);
  void Remove(const std::string& doc_id);
  std::vector<TextSearchResult> Search(const std::vector<std::string>& terms) const;
  std::vector<std::string> Lookup(const std::string& term) const;
  std::vector<std::string> PrefixLookup(const std::string& prefix,
                                         size_t max_expansions = 200) const;
  std::vector<std::string> SuffixLookup(const std::string& suffix,
                                         size_t max_expansions = 200) const;
  std::vector<std::string> InfixLookup(const std::string& infix,
                                        size_t max_expansions = 200) const;
  std::vector<std::string> FuzzyLookup(const std::string& term, int max_dist,
                                        size_t max_expansions = 200) const;
  std::vector<TextSearchResult> PrefixSearch(const std::string& prefix,
                                              size_t max_expansions = 200) const;
  std::vector<TextSearchResult> SuffixSearch(const std::string& suffix,
                                              size_t max_expansions = 200) const;
  std::vector<TextSearchResult> InfixSearch(const std::string& infix,
                                             size_t max_expansions = 200) const;
  std::vector<TextSearchResult> FuzzySearch(const std::string& term, int max_dist,
                                             size_t max_expansions = 200) const;
  std::vector<std::string> PhraseLookup(const std::vector<std::string>& terms,
                                         int slop, bool inorder) const;
  std::vector<TextSearchResult> PhraseSearch(const std::vector<std::string>& terms,
                                              int slop, bool inorder) const;
  std::vector<std::string> StemLookup(const std::string& term,
                                       const std::string& language = "english",
                                       size_t min_stem_len = 3) const;
  std::vector<TextSearchResult> StemSearch(const std::string& term,
                                            const std::string& language = "english",
                                            size_t min_stem_len = 3) const;
  std::vector<std::string> PhoneticLookup(const std::string& term) const;
  std::vector<TextSearchResult> PhoneticSearch(const std::string& term) const;
  size_t NumTerms() const;
  size_t NumDocs() const;
  void Clear();

  static std::vector<std::string> Tokenize(const std::string& text);
  static std::vector<std::string> Tokenize(const std::string& text,
                                            const std::vector<std::string>& stopwords);
  static std::vector<std::string> TokenizeRaw(const std::string& text);
  static int LevenshteinDistance(const std::string& a, const std::string& b);
};

class TextFieldIndices {
  std::unordered_map<std::string, TextIndex> field_indices_;

 public:
  TextIndex& GetOrCreate(const std::string& field_name);
  const TextIndex* Get(const std::string& field_name) const;
  void Clear();
};
