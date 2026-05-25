#include "index_spec.h"

#include <algorithm>
#include <utility>

bool IndexRegistry::Create(std::string name, std::vector<FieldSpec> fields) {
  std::string key = name;
  auto [it, inserted] =
      indices_.try_emplace(std::move(key), IndexSpec{std::move(name), std::move(fields)});
  return inserted;
}

bool IndexRegistry::Drop(const std::string& name) {
  return indices_.erase(name) > 0;
}

const IndexSpec* IndexRegistry::Get(const std::string& name) const {
  auto it = indices_.find(name);
  if (it == indices_.end()) return nullptr;
  return &it->second;
}

std::vector<std::string> IndexRegistry::List() const {
  std::vector<std::string> names;
  names.reserve(indices_.size());
  for (auto& [k, v] : indices_) {
    names.push_back(k);
  }
  std::sort(names.begin(), names.end());
  return names;
}

size_t IndexRegistry::Size() const { return indices_.size(); }

void IndexRegistry::Clear() { indices_.clear(); }
