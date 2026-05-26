#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class TagIndex {
  std::unordered_map<std::string, std::set<std::string>> postings_;

 public:
  void Add(const std::string& tag_value, const std::string& doc_id);
  bool Remove(const std::string& tag_value, const std::string& doc_id);
  std::vector<std::string> Lookup(const std::string& tag_value) const;
  std::vector<std::string> LookupOr(
      const std::vector<std::string>& values) const;
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
