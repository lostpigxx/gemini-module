#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// =============================================================
// Suggestion dictionary (trie-based autocomplete)
// =============================================================

struct SuggestEntry {
  std::string str;
  double score;
  std::string payload;
};

class SuggestDict {
  struct TrieNode {
    std::unordered_map<char, TrieNode*> children;
    bool is_terminal = false;
    double score = 0.0;
    std::string payload;
    ~TrieNode() { for (auto& [c, n] : children) delete n; }
  };

  TrieNode root_;
  size_t size_ = 0;

  void Collect(const TrieNode* node, const std::string& prefix,
               std::vector<SuggestEntry>& out) const;
  void CollectFuzzy(const TrieNode* node, const std::string& prefix,
                    const std::string& target, int max_dist,
                    std::vector<SuggestEntry>& out) const;

 public:
  ~SuggestDict() = default;
  bool Add(const std::string& str, double score,
           bool incr = false, const std::string& payload = "");
  bool Del(const std::string& str);
  std::vector<SuggestEntry> Get(const std::string& prefix, bool fuzzy = false,
                                size_t max_results = 5) const;
  size_t Len() const;
};

// =============================================================
// Global dictionary (term set)
// =============================================================

class TermDict {
  std::unordered_set<std::string> terms_;

 public:
  size_t Add(const std::vector<std::string>& terms);
  size_t Del(const std::vector<std::string>& terms);
  std::vector<std::string> Dump() const;
  bool Contains(const std::string& term) const;
  size_t Size() const;
};

// =============================================================
// Synonym groups (per-index)
// =============================================================

class SynonymMap {
  std::unordered_map<std::string, std::unordered_set<std::string>> groups_;

 public:
  void Update(const std::string& group_id, const std::vector<std::string>& terms);
  std::vector<std::string> Expand(const std::string& term) const;
  std::vector<std::pair<std::string, std::vector<std::string>>> Dump() const;
};
