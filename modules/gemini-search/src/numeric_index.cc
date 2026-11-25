#include "numeric_index.h"

#include <algorithm>

uint32_t NumericIndex::InternDoc(const std::string& doc_id) {
  auto it = str_to_id_.find(doc_id);
  if (it != str_to_id_.end()) return it->second;
  uint32_t did = 0;
  if (!free_ids_.empty()) {
    did = free_ids_.back();
    free_ids_.pop_back();
    id_to_str_[did] = doc_id;
    ref_count_[did] = 0;
  } else {
    did = static_cast<uint32_t>(id_to_str_.size());
    id_to_str_.push_back(doc_id);
    ref_count_.push_back(0);
  }
  str_to_id_[doc_id] = did;
  return did;
}

void NumericIndex::ReleaseDocRef(uint32_t did) {
  if (ref_count_[did] > 0) ref_count_[did]--;
  if (ref_count_[did] == 0) {
    str_to_id_.erase(id_to_str_[did]);
    id_to_str_[did].clear();
    free_ids_.push_back(did);
  }
}

void NumericIndex::Add(double value, const std::string& doc_id) {
  uint32_t did = InternDoc(doc_id);
  auto& vec = entries_[value];
  if (vec.empty() || vec.back() < did) {
    vec.push_back(did);
    ref_count_[did]++;
    return;
  }
  auto pos = std::lower_bound(vec.begin(), vec.end(), did);
  if (pos == vec.end() || *pos != did) {
    vec.insert(pos, did);
    ref_count_[did]++;
  }
}

bool NumericIndex::Remove(double value, const std::string& doc_id) {
  auto dit = str_to_id_.find(doc_id);
  if (dit == str_to_id_.end()) return false;
  uint32_t did = dit->second;
  auto it = entries_.find(value);
  if (it == entries_.end()) return false;
  auto& vec = it->second;
  auto pos = std::lower_bound(vec.begin(), vec.end(), did);
  if (pos == vec.end() || *pos != did) return false;
  vec.erase(pos);
  if (vec.empty()) entries_.erase(it);
  ReleaseDocRef(did);
  return true;
}

std::vector<std::string> NumericIndex::RangeQuery(double min,
                                                   bool min_exclusive,
                                                   double max,
                                                   bool max_exclusive) const {
  auto lo = min_exclusive ? entries_.upper_bound(min)
                          : entries_.lower_bound(min);

  std::vector<uint32_t> merged;
  for (auto it = lo; it != entries_.end(); ++it) {
    if (max_exclusive ? it->first >= max : it->first > max) break;
    merged.insert(merged.end(), it->second.begin(), it->second.end());
  }
  if (merged.empty()) return {};
  std::sort(merged.begin(), merged.end());
  merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
  std::vector<std::string> out;
  out.reserve(merged.size());
  for (uint32_t did : merged) out.push_back(id_to_str_[did]);
  std::sort(out.begin(), out.end());
  return out;
}

size_t NumericIndex::Size() const { return entries_.size(); }

void NumericIndex::Clear() {
  id_to_str_.clear();
  ref_count_.clear();
  free_ids_.clear();
  str_to_id_.clear();
  entries_.clear();
}

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
