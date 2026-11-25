#include "tag_index.h"

#include <algorithm>
#include <cctype>

uint32_t TagIndex::InternDoc(const std::string& doc_id) {
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

void TagIndex::ReleaseDocRef(uint32_t did) {
  if (ref_count_[did] > 0) ref_count_[did]--;
  if (ref_count_[did] == 0) {
    str_to_id_.erase(id_to_str_[did]);
    id_to_str_[did].clear();
    free_ids_.push_back(did);
  }
}

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
  uint32_t did = InternDoc(doc_id);
  auto& vec = postings_[tag_value];
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

void TagIndex::AddMulti(const std::string& raw, const std::string& doc_id,
                        char separator, bool case_sensitive) {
  for (const auto& t : SplitTags(raw, separator, case_sensitive)) {
    Add(t, doc_id);
  }
}

bool TagIndex::Remove(const std::string& tag_value, const std::string& doc_id) {
  auto dit = str_to_id_.find(doc_id);
  if (dit == str_to_id_.end()) return false;
  uint32_t did = dit->second;
  auto it = postings_.find(tag_value);
  if (it == postings_.end()) return false;
  auto& vec = it->second;
  auto pos = std::lower_bound(vec.begin(), vec.end(), did);
  if (pos == vec.end() || *pos != did) return false;
  vec.erase(pos);
  if (vec.empty()) postings_.erase(it);
  ReleaseDocRef(did);
  return true;
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
  std::vector<std::string> out;
  out.reserve(it->second.size());
  for (uint32_t did : it->second) out.push_back(id_to_str_[did]);
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> TagIndex::LookupOr(
    const std::vector<std::string>& values, bool case_sensitive) const {
  std::vector<uint32_t> merged;
  for (const auto& v : values) {
    std::string key = NormalizeTag(v, case_sensitive);
    auto it = postings_.find(key);
    if (it != postings_.end()) {
      merged.insert(merged.end(), it->second.begin(), it->second.end());
    }
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

std::vector<std::string> TagIndex::AllTags() const {
  std::vector<std::string> tags;
  tags.reserve(postings_.size());
  for (auto& [tag, docs] : postings_) tags.push_back(tag);
  std::sort(tags.begin(), tags.end());
  return tags;
}

size_t TagIndex::NumTags() const { return postings_.size(); }

void TagIndex::Clear() {
  id_to_str_.clear();
  ref_count_.clear();
  free_ids_.clear();
  str_to_id_.clear();
  postings_.clear();
}

TagIndex& TagFieldIndices::GetOrCreate(const std::string& field_name) {
  return field_indices_[field_name];
}

const TagIndex* TagFieldIndices::Get(const std::string& field_name) const {
  auto it = field_indices_.find(field_name);
  if (it == field_indices_.end()) return nullptr;
  return &it->second;
}

void TagFieldIndices::Clear() { field_indices_.clear(); }
