#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class TagIndex {
  std::unordered_map<std::string, std::set<std::string>> postings_;

 public:
  static std::string NormalizeTag(const std::string& tag, bool case_sensitive);
  static std::vector<std::string> SplitTags(const std::string& raw,
                                            char separator = ',',
                                            bool case_sensitive = false);

  void Add(const std::string& tag_value, const std::string& doc_id);
  void AddMulti(const std::string& raw, const std::string& doc_id,
                char separator = ',', bool case_sensitive = false);
  bool Remove(const std::string& tag_value, const std::string& doc_id);
  bool RemoveMulti(const std::string& raw, const std::string& doc_id,
                   char separator = ',', bool case_sensitive = false);
  std::vector<std::string> Lookup(const std::string& tag_value) const;
  std::vector<std::string> LookupOr(
      const std::vector<std::string>& values,
      bool case_sensitive = true) const;
  std::vector<std::string> AllTags() const;
  size_t NumTags() const;
  void Clear();
};

class TagFieldIndices {
  std::unordered_map<std::string, TagIndex> field_indices_;

 public:
  TagIndex& GetOrCreate(const std::string& field_name);
  const TagIndex* Get(const std::string& field_name) const;
  void Clear();
};
