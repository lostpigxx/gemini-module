#include "tag_index.h"

#include <algorithm>
#include <cctype>

std::string TagIndex::NormalizeTag(const std::string& tag, bool case_sensitive) {
  size_t s = tag.find_first_not_of(" \t");
  if (s == std::string::npos) return {};
  size_t e = tag.find_last_not_of(" \t");
  std::string out = tag.substr(s, e - s + 1);
  if (!case_sensitive) {
    for (auto& c : out) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  return out;
}

std::vector<std::string> TagIndex::SplitTags(const std::string& raw,
                                             char separator,
                                             bool case_sensitive) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= raw.size()) {
    size_t pos = raw.find(separator, start);
    if (pos == std::string::npos) pos = raw.size();
    std::string tok = NormalizeTag(raw.substr(start, pos - start), case_sensitive);
    if (!tok.empty()) out.push_back(std::move(tok));
    if (pos == raw.size()) break;
    start = pos + 1;
  }
  return out;
}

void TagIndex::Add(const std::string& tag_value, const std::string& doc_id) {
  postings_[tag_value].insert(doc_id);
}

void TagIndex::AddMulti(const std::string& raw, const std::string& doc_id,
                        char separator, bool case_sensitive) {
  for (const auto& t : SplitTags(raw, separator, case_sensitive)) {
    postings_[t].insert(doc_id);
  }
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

bool TagIndex::RemoveMulti(const std::string& raw, const std::string& doc_id,
                           char separator, bool case_sensitive) {
  bool any = false;
  for (const auto& t : SplitTags(raw, separator, case_sensitive)) {
    if (Remove(t, doc_id)) any = true;
  }
  return any;
}

std::vector<std::string> TagIndex::Lookup(const std::string& tag_value) const {
  auto it = postings_.find(tag_value);
  if (it == postings_.end()) return {};
  return {it->second.begin(), it->second.end()};
}

std::vector<std::string> TagIndex::LookupOr(
    const std::vector<std::string>& values, bool case_sensitive) const {
  std::set<std::string> merged;
  for (const auto& v : values) {
    std::string key = NormalizeTag(v, case_sensitive);
    auto it = postings_.find(key);
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
