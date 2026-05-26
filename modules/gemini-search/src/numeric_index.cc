#include "numeric_index.h"

#include <algorithm>

void NumericIndex::Add(double value, const std::string& doc_id) {
  entries_[value].insert(doc_id);
}

bool NumericIndex::Remove(double value, const std::string& doc_id) {
  auto it = entries_.find(value);
  if (it == entries_.end()) return false;
  bool erased = it->second.erase(doc_id) > 0;
  if (it->second.empty()) {
    entries_.erase(it);
  }
  return erased;
}

std::vector<std::string> NumericIndex::RangeQuery(double min,
                                                   bool min_exclusive,
                                                   double max,
                                                   bool max_exclusive) const {
  auto lo = min_exclusive ? entries_.upper_bound(min)
                          : entries_.lower_bound(min);

  std::set<std::string> merged;
  for (auto it = lo; it != entries_.end(); ++it) {
    if (max_exclusive ? it->first >= max : it->first > max) break;
    merged.insert(it->second.begin(), it->second.end());
  }
  return {merged.begin(), merged.end()};
}

size_t NumericIndex::Size() const { return entries_.size(); }

void NumericIndex::Clear() { entries_.clear(); }

NumericIndex& NumericFieldIndices::GetOrCreate(const std::string& field_name) {
  return field_indices_[field_name];
}

const NumericIndex* NumericFieldIndices::Get(
    const std::string& field_name) const {
  auto it = field_indices_.find(field_name);
  if (it == field_indices_.end()) return nullptr;
  return &it->second;
}

void NumericFieldIndices::Clear() { field_indices_.clear(); }
