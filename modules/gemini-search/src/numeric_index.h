#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class NumericIndex {
  std::map<double, std::set<std::string>> entries_;

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
