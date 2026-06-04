#pragma once

#include <cstddef>
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
  };

  std::unordered_map<std::string, std::vector<Posting>> postings_;
  std::unordered_map<std::string, int> doc_lengths_;
  double avg_doc_len_ = 0;

  void UpdateAvgDocLen();

 public:
  void Add(const std::string& doc_id, const std::string& text);
  void Remove(const std::string& doc_id);
  std::vector<TextSearchResult> Search(const std::vector<std::string>& terms) const;
  std::vector<std::string> Lookup(const std::string& term) const;
  size_t NumTerms() const;
  size_t NumDocs() const;
  void Clear();

  static std::vector<std::string> Tokenize(const std::string& text);
};

class TextFieldIndices {
  std::unordered_map<std::string, TextIndex> field_indices_;

 public:
  TextIndex& GetOrCreate(const std::string& field_name);
  const TextIndex* Get(const std::string& field_name) const;
  void Clear();
};
