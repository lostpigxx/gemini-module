#include "tag_index.h"

#include <algorithm>

void TagIndex::Add(const std::string& tag_value, const std::string& doc_id) {
  postings_[tag_value].insert(doc_id);
}

bool TagIndex::Remove(const std::string& tag_value, const std::string& doc_id) {
  auto it = postings_.find(tag_value);
  if (it == postings_.end()) return false;
  bool erased = it->second.erase(doc_id) > 0;
  if (it->second.empty()) {
    postings_.erase(it);
  }
  return erased;
}

std::vector<std::string> TagIndex::Lookup(const std::string& tag_value) const {
  auto it = postings_.find(tag_value);
  if (it == postings_.end()) return {};
  return {it->second.begin(), it->second.end()};
}

std::vector<std::string> TagIndex::LookupOr(
    const std::vector<std::string>& values) const {
  std::set<std::string> merged;
  for (auto& v : values) {
    auto it = postings_.find(v);
    if (it != postings_.end()) {
      merged.insert(it->second.begin(), it->second.end());
    }
  }
  return {merged.begin(), merged.end()};
}

std::vector<std::string> TagIndex::AllTags() const {
  std::vector<std::string> tags;
  tags.reserve(postings_.size());
  for (auto& [tag, docs] : postings_) tags.push_back(tag);
  std::sort(tags.begin(), tags.end());
  return tags;
}

size_t TagIndex::NumTags() const { return postings_.size(); }

void TagIndex::Clear() { postings_.clear(); }

TagIndex& TagFieldIndices::GetOrCreate(const std::string& field_name) {
  return field_indices_[field_name];
}

const TagIndex* TagFieldIndices::Get(const std::string& field_name) const {
  auto it = field_indices_.find(field_name);
  if (it == field_indices_.end()) return nullptr;
  return &it->second;
}

void TagFieldIndices::Clear() { field_indices_.clear(); }
