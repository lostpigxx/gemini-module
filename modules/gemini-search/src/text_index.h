#pragma once

#include <atomic>
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

// Optional cluster-wide BM25 statistics for 2-Phase DFS exact scoring (Phase 69).
struct GlobalBm25Stats {
  uint64_t total_docs = 0;
  double avg_doc_len = 0.0;
  std::unordered_map<std::string, uint64_t> term_df;
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

  // Inverted index with postings sorted by uint32_t doc_id.
  // TermStats packs (max_tf:31, min_dl:31, dirty:1) into a single atomic uint64_t so
  // concurrent SearchTopK() readers under shared_lock can lazily refresh dirty stats
  // without mutating term_stats_ structure or racing on non-atomic fields.
  struct TermStats {
    static constexpr uint64_t kMask31 = (1ULL << 31) - 1ULL;
    static constexpr uint64_t kDirtyBit = 1ULL << 62;

    mutable std::atomic<uint64_t> packed{0};

    TermStats() = default;
    TermStats(const TermStats& o)
        : packed(o.packed.load(std::memory_order_relaxed)) {}
    TermStats& operator=(const TermStats& o) {
      packed.store(o.packed.load(std::memory_order_relaxed),
                   std::memory_order_relaxed);
      return *this;
    }

    static uint64_t Pack(int max_tf, int min_dl, bool dirty) {
      uint64_t mtf = (max_tf > 0) ? (static_cast<uint64_t>(max_tf) & kMask31) : 0ULL;
      uint64_t mdl = (min_dl > 0) ? (static_cast<uint64_t>(min_dl) & kMask31) : 0ULL;
      return mtf | (mdl << 31) | (dirty ? kDirtyBit : 0ULL);
    }
    void Load(int& max_tf, int& min_dl, bool& dirty) const {
      uint64_t v = packed.load(std::memory_order_relaxed);
      max_tf = static_cast<int>(v & kMask31);
      min_dl = static_cast<int>((v >> 31) & kMask31);
      dirty = (v & kDirtyBit) != 0;
    }
    void Store(int max_tf, int min_dl, bool dirty) const {
      packed.store(Pack(max_tf, min_dl, dirty), std::memory_order_relaxed);
    }
  };
  std::unordered_map<std::string, std::vector<Posting>> postings_;
  std::unordered_map<std::string, TermStats> term_stats_;
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
  std::vector<TextSearchResult> Search(
      const std::vector<std::string>& terms,
      const GlobalBm25Stats* global_stats = nullptr) const;
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
  std::vector<TextSearchResult> PrefixSearch(
      const std::string& prefix, size_t max_expansions = 200,
      const GlobalBm25Stats* global_stats = nullptr) const;
  std::vector<TextSearchResult> SuffixSearch(
      const std::string& suffix, size_t max_expansions = 200,
      const GlobalBm25Stats* global_stats = nullptr) const;
  std::vector<TextSearchResult> InfixSearch(
      const std::string& infix, size_t max_expansions = 200,
      const GlobalBm25Stats* global_stats = nullptr) const;
  std::vector<TextSearchResult> FuzzySearch(
      const std::string& term, int max_dist, size_t max_expansions = 200,
      const GlobalBm25Stats* global_stats = nullptr) const;
  std::vector<std::string> PhraseLookup(const std::vector<std::string>& terms,
                                         int slop, bool inorder) const;
  std::vector<TextSearchResult> PhraseSearch(
      const std::vector<std::string>& terms, int slop, bool inorder,
      const GlobalBm25Stats* global_stats = nullptr) const;
  std::vector<std::string> StemLookup(const std::string& term,
                                       const std::string& language = "english",
                                       size_t min_stem_len = 3) const;
  std::vector<TextSearchResult> StemSearch(
      const std::string& term, const std::string& language = "english",
      size_t min_stem_len = 3,
      const GlobalBm25Stats* global_stats = nullptr) const;
  std::vector<std::string> PhoneticLookup(const std::string& term) const;
  std::vector<TextSearchResult> PhoneticSearch(
      const std::string& term,
      const GlobalBm25Stats* global_stats = nullptr) const;

  // Phase 69: Helpers for 2-Phase DFS term statistics collection
  uint64_t TotalDocLen() const { return total_doc_len_; }
  uint64_t TermDocFreq(const std::string& term) const;
  std::vector<std::string> ExpandPrefixTerms(const std::string& prefix,
                                             size_t max_expansions = 200) const;
  std::vector<std::string> ExpandSuffixTerms(const std::string& suffix,
                                             size_t max_expansions = 200) const;
  std::vector<std::string> ExpandInfixTerms(const std::string& infix,
                                            size_t max_expansions = 200) const;
  std::vector<std::string> ExpandFuzzyTerms(const std::string& term, int max_dist,
                                            size_t max_expansions = 200) const;
  std::vector<std::string> ExpandStemTerms(const std::string& term,
                                           const std::string& language = "english",
                                           size_t min_stem_len = 3) const;
  std::vector<std::string> ExpandPhoneticTerms(const std::string& term) const;

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
