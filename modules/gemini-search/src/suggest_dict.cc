#include "suggest_dict.h"

#include <algorithm>
#include <cctype>

// =============================================================
// SuggestDict
// =============================================================

void SuggestDict::Collect(const TrieNode* node, const std::string& prefix,
                          std::vector<SuggestEntry>& out) const {
  if (node->is_terminal) {
    out.push_back({prefix, node->score, node->payload});
  }
  for (auto& [c, child] : node->children) {
    Collect(child, prefix + c, out);
  }
}

static int LevenshteinDist(const std::string& a, const std::string& b) {
  size_t m = a.size(), n = b.size();
  std::vector<int> prev(n + 1), curr(n + 1);
  for (size_t j = 0; j <= n; j++) prev[j] = static_cast<int>(j);
  for (size_t i = 1; i <= m; i++) {
    curr[0] = static_cast<int>(i);
    for (size_t j = 1; j <= n; j++) {
      int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
    }
    std::swap(prev, curr);
  }
  return prev[n];
}

void SuggestDict::CollectFuzzy(const TrieNode* node, const std::string& prefix,
                               const std::string& target, int max_dist,
                               std::vector<SuggestEntry>& out) const {
  if (node->is_terminal) {
    if (LevenshteinDist(prefix, target) <= max_dist) {
      out.push_back({prefix, node->score, node->payload});
    }
  }
  if (prefix.size() > target.size() + static_cast<size_t>(max_dist)) return;
  for (auto& [c, child] : node->children) {
    CollectFuzzy(child, prefix + c, target, max_dist, out);
  }
}

bool SuggestDict::Add(const std::string& str, double score,
                      bool incr, const std::string& payload) {
  std::string lower;
  lower.reserve(str.size());
  for (auto c : str)
    lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  TrieNode* node = &root_;
  for (char c : lower) {
    auto& child = node->children[c];
    if (!child) child = new TrieNode();
    node = child;
  }
  bool is_new = !node->is_terminal;
  node->is_terminal = true;
  if (incr) {
    node->score += score;
  } else {
    node->score = score;
  }
  if (!payload.empty()) node->payload = payload;
  if (is_new) size_++;
  return is_new;
}

bool SuggestDict::Del(const std::string& str) {
  std::string lower;
  lower.reserve(str.size());
  for (auto c : str)
    lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  TrieNode* node = &root_;
  for (char c : lower) {
    auto it = node->children.find(c);
    if (it == node->children.end()) return false;
    node = it->second;
  }
  if (!node->is_terminal) return false;
  node->is_terminal = false;
  node->score = 0;
  node->payload.clear();
  size_--;
  return true;
}

std::vector<SuggestEntry> SuggestDict::Get(const std::string& prefix,
                                           bool fuzzy,
                                           size_t max_results) const {
  std::string lower;
  lower.reserve(prefix.size());
  for (auto c : prefix)
    lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  std::vector<SuggestEntry> results;

  if (fuzzy) {
    CollectFuzzy(&root_, "", lower, 1, results);
  } else {
    const TrieNode* node = &root_;
    for (char c : lower) {
      auto it = node->children.find(c);
      if (it == node->children.end()) return {};
      node = it->second;
    }
    Collect(node, lower, results);
  }

  std::sort(results.begin(), results.end(),
            [](const SuggestEntry& a, const SuggestEntry& b) {
              if (a.score != b.score) return a.score > b.score;
              return a.str < b.str;
            });

  if (results.size() > max_results) results.resize(max_results);
  return results;
}

size_t SuggestDict::Len() const { return size_; }

// =============================================================
// TermDict
// =============================================================

size_t TermDict::Add(const std::vector<std::string>& terms) {
  size_t added = 0;
  for (auto& t : terms) {
    if (terms_.insert(t).second) added++;
  }
  return added;
}

size_t TermDict::Del(const std::vector<std::string>& terms) {
  size_t removed = 0;
  for (auto& t : terms) {
    if (terms_.erase(t) > 0) removed++;
  }
  return removed;
}

std::vector<std::string> TermDict::Dump() const {
  std::vector<std::string> result(terms_.begin(), terms_.end());
  std::sort(result.begin(), result.end());
  return result;
}

bool TermDict::Contains(const std::string& term) const {
  return terms_.count(term) > 0;
}

size_t TermDict::Size() const { return terms_.size(); }

// =============================================================
// SynonymMap
// =============================================================

void SynonymMap::Update(const std::string& group_id,
                        const std::vector<std::string>& terms) {
  auto& group = groups_[group_id];
  for (auto& t : terms) group.insert(t);
}

std::vector<std::string> SynonymMap::Expand(const std::string& term) const {
  std::vector<std::string> result;
  for (auto& [gid, terms] : groups_) {
    if (terms.count(term)) {
      for (auto& t : terms) {
        if (t != term) result.push_back(t);
      }
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<std::pair<std::string, std::vector<std::string>>>
SynonymMap::Dump() const {
  std::vector<std::pair<std::string, std::vector<std::string>>> result;
  for (auto& [gid, terms] : groups_) {
    std::vector<std::string> sorted_terms(terms.begin(), terms.end());
    std::sort(sorted_terms.begin(), sorted_terms.end());
    result.emplace_back(gid, std::move(sorted_terms));
  }
  std::sort(result.begin(), result.end());
  return result;
}
