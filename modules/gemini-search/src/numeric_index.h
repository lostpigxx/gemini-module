#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class NumericIndex {
  std::vector<std::string> id_to_str_;
  std::vector<uint32_t> ref_count_;
  std::vector<uint32_t> free_ids_;
  std::unordered_map<std::string, uint32_t> str_to_id_;
  std::map<double, std::vector<uint32_t>> entries_;

  uint32_t InternDoc(const std::string& doc_id);
  void ReleaseDocRef(uint32_t did);

 public:
  void Add(double value, const std::string& doc_id);
  bool Remove(double value, const std::string& doc_id);
  std::vector<std::string> RangeQuery(double min, bool min_exclusive,
                                       double max, bool max_exclusive) const;
  size_t Size() const;
  void Clear();
};

class NumericFieldIndices {
  std::unordered_map<std::string, NumericIndex> field_indices_;

 public:
  NumericIndex& GetOrCreate(const std::string& field_name);
  const NumericIndex* Get(const std::string& field_name) const;
  void Clear();
};
