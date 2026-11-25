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

static bool DecodeUtf8(const std::string& s, size_t pos, uint32_t& cp,
                       size_t& byte_len) {
  if (pos >= s.size()) return false;
  unsigned char c0 = static_cast<unsigned char>(s[pos]);
  if (c0 < 0x80) {
    cp = c0;
    byte_len = 1;
    return true;
  }
  if ((c0 & 0xE0) == 0xC0 && pos + 1 < s.size()) {
    unsigned char c1 = static_cast<unsigned char>(s[pos + 1]);
    if ((c1 & 0xC0) == 0x80) {
      cp = ((c0 & 0x1Fu) << 6) | (c1 & 0x3Fu);
      byte_len = 2;
      return true;
    }
  } else if ((c0 & 0xF0) == 0xE0 && pos + 2 < s.size()) {
    unsigned char c1 = static_cast<unsigned char>(s[pos + 1]);
    unsigned char c2 = static_cast<unsigned char>(s[pos + 2]);
    if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
      cp = ((c0 & 0x0Fu) << 12) | ((c1 & 0x3Fu) << 6) | (c2 & 0x3Fu);
      byte_len = 3;
      return true;
    }
  } else if ((c0 & 0xF8) == 0xF0 && pos + 3 < s.size()) {
    unsigned char c1 = static_cast<unsigned char>(s[pos + 1]);
    unsigned char c2 = static_cast<unsigned char>(s[pos + 2]);
    unsigned char c3 = static_cast<unsigned char>(s[pos + 3]);
    if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
      cp = ((c0 & 0x07u) << 18) | ((c1 & 0x3Fu) << 12) |
           ((c2 & 0x3Fu) << 6) | (c3 & 0x3Fu);
      byte_len = 4;
      return true;
    }
  }
  cp = c0;
  byte_len = 1;
  return true;
}

static bool IsCjkCodepoint(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) ||  // CJK Unified Ideographs
         (cp >= 0x3400 && cp <= 0x4DBF) ||  // CJK Extension A
         (cp >= 0xF900 && cp <= 0xFAFF) ||  // CJK Compatibility Ideographs
         (cp >= 0x3040 && cp <= 0x30FF) ||  // Hiragana & Katakana
         (cp >= 0xAC00 && cp <= 0xD7AF);    // Hangul Syllables
}

static std::vector<std::string> TokenizeImpl(
    const std::string& text, bool filter_stopwords,
    const std::unordered_set<std::string>* custom_sw = nullptr) {
  std::vector<std::string> tokens;
  const auto& sw = custom_sw ? *custom_sw : kStopWords;
  size_t i = 0;
  while (i < text.size()) {
    unsigned char ch = static_cast<unsigned char>(text[i]);
    if (ch < 0x80) {
      if (!std::isalnum(ch)) {
        i++;
        continue;
      }
      size_t start = i;
      while (i < text.size() &&
             static_cast<unsigned char>(text[i]) < 0x80 &&
             std::isalnum(static_cast<unsigned char>(text[i]))) {
        i++;
      }
      std::string token;
      token.reserve(i - start);
      for (size_t j = start; j < i; j++) {
        token += static_cast<char>(std::tolower(static_cast<unsigned char>(text[j])));
      }
      if (!token.empty() && (!filter_stopwords || sw.find(token) == sw.end())) {
        tokens.push_back(std::move(token));
      }
    } else {
      uint32_t cp = 0;
      size_t blen = 0;
      DecodeUtf8(text, i, cp, blen);
      if (!IsCjkCodepoint(cp)) {
        i += blen;
        continue;
      }
      // Collect contiguous CJK run
      std::vector<std::string> cjk_chars;
      while (i < text.size()) {
        uint32_t c2 = 0;
        size_t l2 = 0;
        DecodeUtf8(text, i, c2, l2);
        if (!IsCjkCodepoint(c2)) break;
        cjk_chars.push_back(text.substr(i, l2));
        i += l2;
      }
      if (cjk_chars.size() == 1) {
        if (!filter_stopwords || sw.find(cjk_chars[0]) == sw.end()) {
          tokens.push_back(std::move(cjk_chars[0]));
        }
      } else {
        for (size_t ci = 0; ci + 1 < cjk_chars.size(); ci++) {
          std::string bigram = cjk_chars[ci] + cjk_chars[ci + 1];
          if (!filter_stopwords || sw.find(bigram) == sw.end()) {
            tokens.push_back(std::move(bigram));
          }
        }
      }
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
  return TokenizeImpl(text, true, &sw_set);
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
                    bool phonetic, const std::string& language,
                    bool nofreqs, bool nooffsets) {
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
    info.tf = nofreqs ? 1 : (info.tf + 1);
    if (!nooffsets) info.positions.push_back(i);
  }

  for (auto& [term, info] : tf_map) {
    auto& vec = postings_[term];
    if (vec.empty()) {
      sorted_terms_.insert(term);
      reversed_terms_.emplace(term.rbegin(), term.rend());
    }
    vec.push_back({doc_id, info.tf, std::move(info.positions)});
    auto stemmed = StemWord(term, language);
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
  for (auto& t : empty_terms) {
    postings_.erase(t);
    sorted_terms_.erase(t);
    reversed_terms_.erase(std::string(t.rbegin(), t.rend()));
  }
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

std::vector<std::string> TextIndex::PrefixLookup(const std::string& prefix,
                                                  size_t max_expansions) const {
  if (prefix.size() < 2) return {};
  std::set<std::string> result;
  size_t matched = 0;
  for (auto it = sorted_terms_.lower_bound(prefix); it != sorted_terms_.end(); ++it) {
    if (it->size() < prefix.size() || it->compare(0, prefix.size(), prefix) != 0) break;
    auto pit = postings_.find(*it);
    if (pit != postings_.end()) {
      for (const auto& p : pit->second) result.insert(p.doc_id);
    }
    if (++matched >= max_expansions) break;
  }
  return {result.begin(), result.end()};
}

std::vector<std::string> TextIndex::SuffixLookup(const std::string& suffix,
                                                  size_t max_expansions) const {
  if (suffix.size() < 2) return {};
  std::string rev_suf(suffix.rbegin(), suffix.rend());
  std::set<std::string> result;
  size_t matched = 0;
  for (auto it = reversed_terms_.lower_bound(rev_suf); it != reversed_terms_.end(); ++it) {
    if (it->size() < rev_suf.size() || it->compare(0, rev_suf.size(), rev_suf) != 0) break;
    std::string orig(it->rbegin(), it->rend());
    auto pit = postings_.find(orig);
    if (pit != postings_.end()) {
      for (const auto& p : pit->second) result.insert(p.doc_id);
    }
    if (++matched >= max_expansions) break;
  }
  return {result.begin(), result.end()};
}

std::vector<std::string> TextIndex::InfixLookup(const std::string& infix,
                                                 size_t max_expansions) const {
  if (infix.size() < 2) return {};
  std::set<std::string> result;
  size_t matched = 0;
  for (const auto& [term, posts] : postings_) {
    if (term.find(infix) != std::string::npos) {
      for (const auto& p : posts) result.insert(p.doc_id);
      if (++matched >= max_expansions) break;
    }
  }
  return {result.begin(), result.end()};
}

std::vector<std::string> TextIndex::FuzzyLookup(const std::string& term, int max_dist,
                                                 size_t max_expansions) const {
  if (max_dist < 1 || max_dist > 3) return {};
  std::set<std::string> result;
  size_t matched = 0;
  for (auto& [posting_term, posts] : postings_) {
    if (LevenshteinDistance(term, posting_term) <= max_dist) {
      for (auto& p : posts) result.insert(p.doc_id);
      if (++matched >= max_expansions) break;
    }
  }
  return {result.begin(), result.end()};
}

std::vector<TextSearchResult> TextIndex::PrefixSearch(const std::string& prefix,
                                                       size_t max_expansions) const {
  if (prefix.size() < 2 || doc_lengths_.empty()) return {};
  std::vector<std::string> matching_terms;
  for (auto it = sorted_terms_.lower_bound(prefix); it != sorted_terms_.end(); ++it) {
    if (it->size() < prefix.size() || it->compare(0, prefix.size(), prefix) != 0) break;
    matching_terms.push_back(*it);
    if (matching_terms.size() >= max_expansions) break;
  }
  return Search(matching_terms);
}

std::vector<TextSearchResult> TextIndex::SuffixSearch(const std::string& suffix,
                                                       size_t max_expansions) const {
  if (suffix.size() < 2 || doc_lengths_.empty()) return {};
  std::string rev_suf(suffix.rbegin(), suffix.rend());
  std::vector<std::string> matching_terms;
  for (auto it = reversed_terms_.lower_bound(rev_suf); it != reversed_terms_.end(); ++it) {
    if (it->size() < rev_suf.size() || it->compare(0, rev_suf.size(), rev_suf) != 0) break;
    matching_terms.emplace_back(it->rbegin(), it->rend());
    if (matching_terms.size() >= max_expansions) break;
  }
  return Search(matching_terms);
}

std::vector<TextSearchResult> TextIndex::InfixSearch(const std::string& infix,
                                                      size_t max_expansions) const {
  if (infix.size() < 2 || doc_lengths_.empty()) return {};
  std::vector<std::string> matching_terms;
  for (const auto& [term, posts] : postings_) {
    if (term.find(infix) != std::string::npos) {
      matching_terms.push_back(term);
      if (matching_terms.size() >= max_expansions) break;
    }
  }
  return Search(matching_terms);
}

std::vector<TextSearchResult> TextIndex::FuzzySearch(const std::string& term, int max_dist,
                                                      size_t max_expansions) const {
  if (max_dist < 1 || max_dist > 3 || doc_lengths_.empty()) return {};
  std::vector<std::string> matching_terms;
  for (auto& [posting_term, posts] : postings_) {
    if (LevenshteinDistance(term, posting_term) <= max_dist) {
      matching_terms.push_back(posting_term);
      if (matching_terms.size() >= max_expansions) break;
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

std::vector<std::string> TextIndex::StemLookup(const std::string& term,
                                                const std::string& language,
                                                size_t min_stem_len) const {
  if (term.size() < min_stem_len) return Lookup(term);
  auto stemmed = StemWord(term, language);
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

std::vector<TextSearchResult> TextIndex::StemSearch(const std::string& term,
                                                     const std::string& language,
                                                     size_t min_stem_len) const {
  if (term.size() < min_stem_len) return Search({term});
  auto stemmed = StemWord(term, language);
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
  sorted_terms_.clear();
  reversed_terms_.clear();
  stem_map_.clear();
  phonetic_map_.clear();
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
