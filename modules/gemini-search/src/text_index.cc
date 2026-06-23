#include "text_index.h"
#include "phonetic.h"
#include "stemmer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <unordered_set>

static const std::unordered_set<std::string> kStopWords = {
    "a",    "an",   "and",  "are",  "as",   "at",   "be",   "but",
    "by",   "for",  "if",   "in",   "into", "is",   "it",   "no",
    "not",  "of",   "on",   "or",   "such", "that", "the",  "their",
    "then", "there","these","they", "this", "to",   "was",  "will",
    "with",
};

static std::vector<std::string> TokenizeImpl(const std::string& text,
                                              bool filter_stopwords) {
  std::vector<std::string> tokens;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && !std::isalnum(static_cast<unsigned char>(text[i]))) i++;
    if (i >= text.size()) break;
    size_t start = i;
    while (i < text.size() && std::isalnum(static_cast<unsigned char>(text[i]))) i++;
    std::string token;
    token.reserve(i - start);
    for (size_t j = start; j < i; j++) {
      token += static_cast<char>(std::tolower(static_cast<unsigned char>(text[j])));
    }
    if (!token.empty() &&
        (!filter_stopwords || kStopWords.find(token) == kStopWords.end())) {
      tokens.push_back(std::move(token));
    }
  }
  return tokens;
}

std::vector<std::string> TextIndex::Tokenize(const std::string& text) {
  return TokenizeImpl(text, true);
}

std::vector<std::string> TextIndex::Tokenize(const std::string& text,
                                              const std::vector<std::string>& stopwords) {
  std::unordered_set<std::string> sw_set(stopwords.begin(), stopwords.end());
  std::vector<std::string> tokens;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && !std::isalnum(static_cast<unsigned char>(text[i]))) i++;
    if (i >= text.size()) break;
    size_t start = i;
    while (i < text.size() && std::isalnum(static_cast<unsigned char>(text[i]))) i++;
    std::string token;
    token.reserve(i - start);
    for (size_t j = start; j < i; j++)
      token += static_cast<char>(std::tolower(static_cast<unsigned char>(text[j])));
    if (!token.empty() && sw_set.find(token) == sw_set.end())
      tokens.push_back(std::move(token));
  }
  return tokens;
}

std::vector<std::string> TextIndex::TokenizeRaw(const std::string& text) {
  return TokenizeImpl(text, false);
}

void TextIndex::UpdateAvgDocLen() {
  if (doc_lengths_.empty()) {
    avg_doc_len_ = 0;
    return;
  }
  double total = 0;
  for (auto& [id, len] : doc_lengths_) total += len;
  avg_doc_len_ = total / static_cast<double>(doc_lengths_.size());
}

void TextIndex::Add(const std::string& doc_id, const std::string& text,
                    bool phonetic) {
  Remove(doc_id);
  auto tokens = TokenizeRaw(text);
  doc_lengths_[doc_id] = static_cast<int>(tokens.size());
  UpdateAvgDocLen();

  struct TermInfo {
    int tf = 0;
    std::vector<int> positions;
  };
  std::unordered_map<std::string, TermInfo> tf_map;
  for (int i = 0; i < static_cast<int>(tokens.size()); i++) {
    auto& info = tf_map[tokens[i]];
    info.tf++;
    info.positions.push_back(i);
  }

  for (auto& [term, info] : tf_map) {
    postings_[term].push_back({doc_id, info.tf, std::move(info.positions)});
    auto stemmed = StemEnglish(term);
    if (stemmed != term) {
      stem_map_[stemmed].insert(term);
    }
    if (phonetic) {
      auto [p1, p2] = DoubleMetaphone(term);
      if (!p1.empty()) phonetic_map_[p1].insert(term);
      if (!p2.empty() && p2 != p1) phonetic_map_[p2].insert(term);
    }
  }
}

void TextIndex::Remove(const std::string& doc_id) {
  if (doc_lengths_.erase(doc_id) == 0) return;
  UpdateAvgDocLen();

  std::vector<std::string> empty_terms;
  for (auto& [term, posts] : postings_) {
    posts.erase(std::remove_if(posts.begin(), posts.end(),
                               [&](const Posting& p) { return p.doc_id == doc_id; }),
                posts.end());
    if (posts.empty()) empty_terms.push_back(term);
  }
  for (auto& t : empty_terms) postings_.erase(t);
}

std::vector<TextSearchResult> TextIndex::Search(
    const std::vector<std::string>& terms) const {
  if (doc_lengths_.empty()) return {};

  double N = static_cast<double>(doc_lengths_.size());
  double k1 = 1.2;
  double b = 0.75;

  std::unordered_map<std::string, double> scores;

  for (auto& term : terms) {
    auto it = postings_.find(term);
    if (it == postings_.end()) continue;
    auto& posts = it->second;
    double df = static_cast<double>(posts.size());
    double idf = std::log((N - df + 0.5) / (df + 0.5) + 1.0);

    for (auto& p : posts) {
      double tf = static_cast<double>(p.tf);
      auto dl_it = doc_lengths_.find(p.doc_id);
      double dl = (dl_it != doc_lengths_.end()) ? static_cast<double>(dl_it->second) : 0;
      double tf_norm = (tf * (k1 + 1.0)) / (tf + k1 * (1.0 - b + b * dl / avg_doc_len_));
      scores[p.doc_id] += idf * tf_norm;
    }
  }

  std::vector<TextSearchResult> results;
  results.reserve(scores.size());
  for (auto& [doc_id, score] : scores) {
    results.push_back({doc_id, score});
  }
  std::sort(results.begin(), results.end(),
            [](const TextSearchResult& a, const TextSearchResult& b) {
              if (a.score != b.score) return a.score > b.score;
              return a.doc_id < b.doc_id;
            });
  return results;
}

std::vector<std::string> TextIndex::Lookup(const std::string& term) const {
  auto it = postings_.find(term);
  if (it == postings_.end()) return {};
  std::vector<std::string> ids;
  for (auto& p : it->second) ids.push_back(p.doc_id);
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<std::string> TextIndex::PrefixLookup(const std::string& prefix) const {
  if (prefix.size() < 2) return {};
  std::set<std::string> result;
  for (auto& [term, posts] : postings_) {
    if (term.size() >= prefix.size() &&
        term.compare(0, prefix.size(), prefix) == 0) {
      for (auto& p : posts) result.insert(p.doc_id);
    }
  }
  return {result.begin(), result.end()};
}

std::vector<std::string> TextIndex::FuzzyLookup(const std::string& term, int max_dist) const {
  if (max_dist < 1 || max_dist > 3) return {};
  std::set<std::string> result;
  for (auto& [posting_term, posts] : postings_) {
    if (LevenshteinDistance(term, posting_term) <= max_dist) {
      for (auto& p : posts) result.insert(p.doc_id);
    }
  }
  return {result.begin(), result.end()};
}

std::vector<TextSearchResult> TextIndex::PrefixSearch(const std::string& prefix) const {
  if (prefix.size() < 2 || doc_lengths_.empty()) return {};
  std::vector<std::string> matching_terms;
  for (auto& [term, posts] : postings_) {
    if (term.size() >= prefix.size() &&
        term.compare(0, prefix.size(), prefix) == 0) {
      matching_terms.push_back(term);
    }
  }
  return Search(matching_terms);
}

std::vector<TextSearchResult> TextIndex::FuzzySearch(const std::string& term, int max_dist) const {
  if (max_dist < 1 || max_dist > 3 || doc_lengths_.empty()) return {};
  std::vector<std::string> matching_terms;
  for (auto& [posting_term, posts] : postings_) {
    if (LevenshteinDistance(term, posting_term) <= max_dist) {
      matching_terms.push_back(posting_term);
    }
  }
  return Search(matching_terms);
}

static bool CheckPhrasePositions(
    const std::vector<std::vector<int>>& pos_lists,
    int slop, bool inorder) {
  if (pos_lists.empty()) return false;
  if (pos_lists.size() == 1) return !pos_lists[0].empty();

  // For each starting position of the first term, try to build a chain
  for (int start_pos : pos_lists[0]) {
    int prev = start_pos;
    bool found = true;
    for (size_t t = 1; t < pos_lists.size(); t++) {
      bool term_found = false;
      for (int p : pos_lists[t]) {
        if (inorder || slop == 0) {
          int gap = p - prev - 1;
          if (gap < 0) continue;
          if (gap <= slop) {
            prev = p;
            term_found = true;
            break;
          }
        } else {
          int gap = std::abs(p - prev) - 1;
          if (gap <= slop && p != prev) {
            prev = p;
            term_found = true;
            break;
          }
        }
      }
      if (!term_found) { found = false; break; }
    }
    if (found) return true;
  }
  return false;
}

std::vector<std::string> TextIndex::PhraseLookup(
    const std::vector<std::string>& terms, int slop, bool inorder) const {
  if (terms.empty()) return {};
  if (terms.size() == 1) return Lookup(terms[0]);

  // Get posting lists for all terms
  std::vector<const std::vector<Posting>*> all_posts;
  for (auto& term : terms) {
    auto it = postings_.find(term);
    if (it == postings_.end()) return {};
    all_posts.push_back(&it->second);
  }

  // Build doc_id → position lists mapping for the first term
  std::unordered_map<std::string, std::vector<int>> first_positions;
  for (auto& p : *all_posts[0]) {
    first_positions[p.doc_id] = p.positions;
  }

  // Intersect: find docs containing ALL terms, collect positions
  std::vector<std::string> result;
  for (auto& [doc_id, first_pos] : first_positions) {
    std::vector<std::vector<int>> pos_lists;
    pos_lists.push_back(first_pos);
    bool has_all = true;
    for (size_t t = 1; t < all_posts.size(); t++) {
      bool found = false;
      for (auto& p : *all_posts[t]) {
        if (p.doc_id == doc_id) {
          pos_lists.push_back(p.positions);
          found = true;
          break;
        }
      }
      if (!found) { has_all = false; break; }
    }
    if (!has_all) continue;
    if (CheckPhrasePositions(pos_lists, slop, inorder)) {
      result.push_back(doc_id);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<TextSearchResult> TextIndex::PhraseSearch(
    const std::vector<std::string>& terms, int slop, bool inorder) const {
  if (terms.empty() || doc_lengths_.empty()) return {};
  if (terms.size() == 1) return Search(terms);

  auto phrase_docs = PhraseLookup(terms, slop, inorder);
  if (phrase_docs.empty()) return {};

  std::unordered_set<std::string> phrase_set(phrase_docs.begin(), phrase_docs.end());
  auto all_results = Search(terms);

  std::vector<TextSearchResult> filtered;
  for (auto& r : all_results) {
    if (phrase_set.count(r.doc_id)) {
      filtered.push_back(r);
    }
  }
  return filtered;
}

std::vector<std::string> TextIndex::StemLookup(const std::string& term) const {
  auto stemmed = StemEnglish(term);
  std::set<std::string> result;
  auto ids = Lookup(term);
  result.insert(ids.begin(), ids.end());
  auto it = stem_map_.find(stemmed);
  if (it != stem_map_.end()) {
    for (auto& orig : it->second) {
      auto orig_ids = Lookup(orig);
      result.insert(orig_ids.begin(), orig_ids.end());
    }
  }
  if (stemmed != term) {
    auto stem_ids = Lookup(stemmed);
    result.insert(stem_ids.begin(), stem_ids.end());
  }
  return {result.begin(), result.end()};
}

std::vector<TextSearchResult> TextIndex::StemSearch(const std::string& term) const {
  auto stemmed = StemEnglish(term);
  std::vector<std::string> search_terms = {term};
  auto it = stem_map_.find(stemmed);
  if (it != stem_map_.end()) {
    for (auto& orig : it->second) search_terms.push_back(orig);
  }
  if (stemmed != term) search_terms.push_back(stemmed);
  return Search(search_terms);
}

std::vector<std::string> TextIndex::PhoneticLookup(const std::string& term) const {
  std::set<std::string> result;
  auto ids = Lookup(term);
  result.insert(ids.begin(), ids.end());
  auto [p1, p2] = DoubleMetaphone(term);
  for (auto& code : {p1, p2}) {
    if (code.empty()) continue;
    auto it = phonetic_map_.find(code);
    if (it == phonetic_map_.end()) continue;
    for (auto& orig : it->second) {
      auto orig_ids = Lookup(orig);
      result.insert(orig_ids.begin(), orig_ids.end());
    }
  }
  return {result.begin(), result.end()};
}

std::vector<TextSearchResult> TextIndex::PhoneticSearch(const std::string& term) const {
  auto [p1, p2] = DoubleMetaphone(term);
  std::vector<std::string> search_terms = {term};
  for (auto& code : {p1, p2}) {
    if (code.empty()) continue;
    auto it = phonetic_map_.find(code);
    if (it == phonetic_map_.end()) continue;
    for (auto& orig : it->second) search_terms.push_back(orig);
  }
  return Search(search_terms);
}

int TextIndex::LevenshteinDistance(const std::string& a, const std::string& b) {
  size_t m = a.size(), n = b.size();
  if (m == 0) return static_cast<int>(n);
  if (n == 0) return static_cast<int>(m);
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

size_t TextIndex::NumTerms() const { return postings_.size(); }

size_t TextIndex::NumDocs() const { return doc_lengths_.size(); }

void TextIndex::Clear() {
  postings_.clear();
  stem_map_.clear();
  doc_lengths_.clear();
  avg_doc_len_ = 0;
}

TextIndex& TextFieldIndices::GetOrCreate(const std::string& field_name) {
  return field_indices_[field_name];
}

const TextIndex* TextFieldIndices::Get(const std::string& field_name) const {
  auto it = field_indices_.find(field_name);
  if (it == field_indices_.end()) return nullptr;
  return &it->second;
}

void TextFieldIndices::Clear() { field_indices_.clear(); }
