#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

class SortableColumn {
  std::unordered_map<std::string, double> num_values_;
  std::unordered_map<std::string, std::string> str_values_;

 public:
  void SetNumeric(const std::string& doc_id, double value) {
    num_values_[doc_id] = value;
  }
  void SetString(const std::string& doc_id, const std::string& value) {
    str_values_[doc_id] = value;
  }
  void Remove(const std::string& doc_id) {
    num_values_.erase(doc_id);
    str_values_.erase(doc_id);
  }
  double GetNumeric(const std::string& doc_id, double def = 0.0) const {
    auto it = num_values_.find(doc_id);
    return (it != num_values_.end()) ? it->second : def;
  }
  const std::string& GetString(const std::string& doc_id) const {
    static const std::string kEmpty;
    auto it = str_values_.find(doc_id);
    return (it != str_values_.end()) ? it->second : kEmpty;
  }
  size_t Size() const { return num_values_.size() + str_values_.size(); }
  void Clear() {
    num_values_.clear();
    str_values_.clear();
  }
};

class SortFieldIndices {
  std::unordered_map<std::string, SortableColumn> columns_;

 public:
  SortableColumn& GetOrCreate(const std::string& field_name) {
    return columns_[field_name];
  }
  const SortableColumn* Get(const std::string& field_name) const {
    auto it = columns_.find(field_name);
    return (it != columns_.end()) ? &it->second : nullptr;
  }
  void Clear() { columns_.clear(); }
};
