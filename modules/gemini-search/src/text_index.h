#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct TextSearchResult {
  std::string doc_id;
  double score;
};

class TextIndex {
  struct Posting {
    uint32_t doc_id;
    int tf;
    std::vector<int> positions;
  };

  // Dense uint32_t doc ID table with slot reuse
  std::vector<std::string> id_to_str_;
  std::vector<int> doc_lengths_;
  std::vector<uint8_t> alive_;
  std::vector<std::vector<std::string>> doc_terms_;
  std::vector<uint32_t> free_ids_;
  std::unordered_map<std::string, uint32_t> str_to_id_;
  size_t live_docs_ = 0;
  uint64_t total_doc_len_ = 0;
  double avg_doc_len_ = 0.0;

  // Inverted index with postings sorted by uint32_t doc_id
  struct TermStats {
    int max_tf = 0;
    int min_dl = 0;
    bool dirty = false;
  };
  std::unordered_map<std::string, std::vector<Posting>> postings_;
  mutable std::unordered_map<std::string, TermStats> term_stats_;
  std::set<std::string> sorted_terms_;
  std::set<std::string> reversed_terms_;
  std::unordered_map<std::string, std::set<std::string>> stem_map_;
  std::unordered_map<std::string, std::set<std::string>> phonetic_map_;

  void UpdateAvgDocLen();

 public:
  void Add(const std::string& doc_id, const std::string& text,
           bool phonetic = false, const std::string& language = "english",
           bool nofreqs = false, bool nooffsets = false,
           const std::unordered_set<std::string>* custom_dict = nullptr);
  void Remove(const std::string& doc_id);
  std::vector<TextSearchResult> Search(const std::vector<std::string>& terms) const;
  std::vector<TextSearchResult> SearchTopK(const std::vector<std::string>& terms,
                                           size_t k) const;
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
  static std::vector<std::string> TokenizeWithDict(
      const std::string& text, const std::unordered_set<std::string>& dict,
      bool filter_stopwords = false);
  static int LevenshteinDistance(const std::string& a, const std::string& b);
};

class TextFieldIndices {
  std::unordered_map<std::string, TextIndex> field_indices_;

 public:
  TextIndex& GetOrCreate(const std::string& field_name);
  const TextIndex* Get(const std::string& field_name) const;
  void Clear();
};
