#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class TagIndex {
  std::vector<std::string> id_to_str_;
  std::vector<uint32_t> ref_count_;
  std::vector<uint32_t> free_ids_;
  std::unordered_map<std::string, uint32_t> str_to_id_;
  std::unordered_map<std::string, std::vector<uint32_t>> postings_;

  uint32_t InternDoc(const std::string& doc_id);
  void ReleaseDocRef(uint32_t did);

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
