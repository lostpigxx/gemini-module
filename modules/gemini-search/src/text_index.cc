#include "text_index.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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

void TextIndex::Add(const std::string& doc_id, const std::string& text) {
  Remove(doc_id);
  auto tokens = TokenizeRaw(text);
  doc_lengths_[doc_id] = static_cast<int>(tokens.size());
  UpdateAvgDocLen();

  std::unordered_map<std::string, int> tf_map;
  for (auto& t : tokens) tf_map[t]++;

  for (auto& [term, tf] : tf_map) {
    postings_[term].push_back({doc_id, tf});
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

size_t TextIndex::NumTerms() const { return postings_.size(); }

size_t TextIndex::NumDocs() const { return doc_lengths_.size(); }

void TextIndex::Clear() {
  postings_.clear();
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
