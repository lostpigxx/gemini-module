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
    const std::unordered_set<std::string>* custom_sw = nullptr,
    const std::unordered_set<std::string>* custom_dict = nullptr) {
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
      if (custom_dict && !custom_dict->empty()) {
        // Phase 54: Forward Maximum Matching (FMM) with bigram fallback
        size_t n = cjk_chars.size();
        size_t pos = 0;
        bool prev_was_bigram = false;
        while (pos < n) {
          bool matched = false;
          size_t max_len = std::min(n - pos, size_t{16});
          for (size_t len = max_len; len >= 2; len--) {
            std::string cand;
            for (size_t k = 0; k < len; k++) cand += cjk_chars[pos + k];
            if (custom_dict->find(cand) != custom_dict->end()) {
              if (!filter_stopwords || sw.find(cand) == sw.end()) {
                tokens.push_back(std::move(cand));
              }
              pos += len;
              matched = true;
              prev_was_bigram = false;
              break;
            }
          }
          if (!matched) {
            if (pos + 1 < n) {
              std::string bigram = cjk_chars[pos] + cjk_chars[pos + 1];
              if (!filter_stopwords || sw.find(bigram) == sw.end()) {
                tokens.push_back(std::move(bigram));
              }
              prev_was_bigram = true;
              pos += 1;
            } else {
              if (!prev_was_bigram) {
                if (!filter_stopwords || sw.find(cjk_chars[pos]) == sw.end()) {
                  tokens.push_back(cjk_chars[pos]);
                }
              }
              pos += 1;
            }
          }
        }
      } else if (cjk_chars.size() == 1) {
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

std::vector<std::string> TextIndex::TokenizeWithDict(
    const std::string& text, const std::unordered_set<std::string>& dict,
    bool filter_stopwords) {
  return TokenizeImpl(text, filter_stopwords, nullptr, &dict);
}

void TextIndex::UpdateAvgDocLen() {
  if (live_docs_ == 0) {
    avg_doc_len_ = 0.0;
    return;
  }
  avg_doc_len_ = static_cast<double>(total_doc_len_) / static_cast<double>(live_docs_);
}

void TextIndex::Add(const std::string& doc_id, const std::string& text,
                    bool phonetic, const std::string& language,
                    bool nofreqs, bool nooffsets,
                    const std::unordered_set<std::string>* custom_dict) {
  Remove(doc_id);
  auto tokens = (custom_dict && !custom_dict->empty())
                    ? TokenizeWithDict(text, *custom_dict, false)
                    : TokenizeRaw(text);

  uint32_t did = 0;
  if (!free_ids_.empty()) {
    did = free_ids_.back();
    free_ids_.pop_back();
    id_to_str_[did] = doc_id;
    doc_lengths_[did] = static_cast<int>(tokens.size());
    alive_[did] = 1;
    doc_terms_[did].clear();
  } else {
    did = static_cast<uint32_t>(id_to_str_.size());
    id_to_str_.push_back(doc_id);
    doc_lengths_.push_back(static_cast<int>(tokens.size()));
    alive_.push_back(1);
    doc_terms_.emplace_back();
  }
  str_to_id_[doc_id] = did;
  live_docs_++;
  total_doc_len_ += tokens.size();
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

  auto& fwd = doc_terms_[did];
  fwd.reserve(tf_map.size());
  for (auto& [term, info] : tf_map) {
    fwd.push_back(term);
    auto& vec = postings_[term];
    if (vec.empty()) {
      sorted_terms_.insert(term);
      reversed_terms_.emplace(term.rbegin(), term.rend());
    }
    Posting p{did, info.tf, std::move(info.positions)};
    if (vec.empty() || vec.back().doc_id < did) {
      vec.push_back(std::move(p));
    } else {
      auto pos = std::lower_bound(
          vec.begin(), vec.end(), did,
          [](const Posting& a, uint32_t id) { return a.doc_id < id; });
      vec.insert(pos, std::move(p));
    }
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
  auto it = str_to_id_.find(doc_id);
  if (it == str_to_id_.end()) return;
  uint32_t did = it->second;
  str_to_id_.erase(it);

  total_doc_len_ -= static_cast<uint64_t>(doc_lengths_[did]);
  live_docs_--;
  UpdateAvgDocLen();

  for (const auto& term : doc_terms_[did]) {
    auto pit = postings_.find(term);
    if (pit == postings_.end()) continue;
    auto& vec = pit->second;
    auto pos = std::lower_bound(
        vec.begin(), vec.end(), did,
        [](const Posting& a, uint32_t id) { return a.doc_id < id; });
    if (pos != vec.end() && pos->doc_id == did) {
      vec.erase(pos);
    }
    if (vec.empty()) {
      postings_.erase(pit);
      sorted_terms_.erase(term);
      reversed_terms_.erase(std::string(term.rbegin(), term.rend()));
    }
  }

  doc_terms_[did].clear();
  id_to_str_[did].clear();
  doc_lengths_[did] = 0;
  alive_[did] = 0;
  free_ids_.push_back(did);
}

std::vector<TextSearchResult> TextIndex::Search(
    const std::vector<std::string>& terms) const {
  if (live_docs_ == 0) return {};

  double N = static_cast<double>(live_docs_);
  double k1 = 1.2;
  double b = 0.75;

  std::unordered_map<uint32_t, double> scores;

  for (auto& term : terms) {
    auto it = postings_.find(term);
    if (it == postings_.end()) continue;
    auto& posts = it->second;
    double df = static_cast<double>(posts.size());
    double idf = std::log((N - df + 0.5) / (df + 0.5) + 1.0);

    for (auto& p : posts) {
      double tf = static_cast<double>(p.tf);
      double dl = static_cast<double>(doc_lengths_[p.doc_id]);
      double tf_norm = (tf * (k1 + 1.0)) / (tf + k1 * (1.0 - b + b * dl / avg_doc_len_));
      scores[p.doc_id] += idf * tf_norm;
    }
  }

  std::vector<TextSearchResult> results;
  results.reserve(scores.size());
  for (auto& [did, score] : scores) {
    results.push_back({id_to_str_[did], score});
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
  ids.reserve(it->second.size());
  for (auto& p : it->second) ids.push_back(id_to_str_[p.doc_id]);
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<std::string> TextIndex::PrefixLookup(const std::string& prefix,
                                                  size_t max_expansions) const {
  if (prefix.size() < 2) return {};
  std::unordered_set<uint32_t> seen;
  size_t matched = 0;
  for (auto it = sorted_terms_.lower_bound(prefix); it != sorted_terms_.end(); ++it) {
    if (it->size() < prefix.size() || it->compare(0, prefix.size(), prefix) != 0) break;
    auto pit = postings_.find(*it);
    if (pit != postings_.end()) {
      for (const auto& p : pit->second) seen.insert(p.doc_id);
    }
    if (++matched >= max_expansions) break;
  }
  std::vector<std::string> result;
  result.reserve(seen.size());
  for (uint32_t did : seen) result.push_back(id_to_str_[did]);
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::string> TextIndex::SuffixLookup(const std::string& suffix,
                                                  size_t max_expansions) const {
  if (suffix.size() < 2) return {};
  std::string rev_suf(suffix.rbegin(), suffix.rend());
  std::unordered_set<uint32_t> seen;
  size_t matched = 0;
  for (auto it = reversed_terms_.lower_bound(rev_suf); it != reversed_terms_.end(); ++it) {
    if (it->size() < rev_suf.size() || it->compare(0, rev_suf.size(), rev_suf) != 0) break;
    std::string orig(it->rbegin(), it->rend());
    auto pit = postings_.find(orig);
    if (pit != postings_.end()) {
      for (const auto& p : pit->second) seen.insert(p.doc_id);
    }
    if (++matched >= max_expansions) break;
  }
  std::vector<std::string> result;
  result.reserve(seen.size());
  for (uint32_t did : seen) result.push_back(id_to_str_[did]);
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::string> TextIndex::InfixLookup(const std::string& infix,
                                                 size_t max_expansions) const {
  if (infix.size() < 2) return {};
  std::unordered_set<uint32_t> seen;
  size_t matched = 0;
  for (const auto& [term, posts] : postings_) {
    if (term.find(infix) != std::string::npos) {
      for (const auto& p : posts) seen.insert(p.doc_id);
      if (++matched >= max_expansions) break;
    }
  }
  std::vector<std::string> result;
  result.reserve(seen.size());
  for (uint32_t did : seen) result.push_back(id_to_str_[did]);
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::string> TextIndex::FuzzyLookup(const std::string& term, int max_dist,
                                                 size_t max_expansions) const {
  if (max_dist < 1 || max_dist > 3) return {};
  std::unordered_set<uint32_t> seen;
  size_t matched = 0;
  for (auto& [posting_term, posts] : postings_) {
    if (LevenshteinDistance(term, posting_term) <= max_dist) {
      for (auto& p : posts) seen.insert(p.doc_id);
      if (++matched >= max_expansions) break;
    }
  }
  std::vector<std::string> result;
  result.reserve(seen.size());
  for (uint32_t did : seen) result.push_back(id_to_str_[did]);
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<TextSearchResult> TextIndex::PrefixSearch(const std::string& prefix,
                                                       size_t max_expansions) const {
  if (prefix.size() < 2 || live_docs_ == 0) return {};
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
  if (suffix.size() < 2 || live_docs_ == 0) return {};
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
  if (infix.size() < 2 || live_docs_ == 0) return {};
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
  if (max_dist < 1 || max_dist > 3 || live_docs_ == 0) return {};
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

  std::vector<const std::vector<Posting>*> all_posts;
  all_posts.reserve(terms.size());
  for (auto& term : terms) {
    auto it = postings_.find(term);
    if (it == postings_.end()) return {};
    all_posts.push_back(&it->second);
  }

  // Intersect uint32_t-sorted posting lists via binary search
  std::vector<std::string> result;
  for (const auto& first_p : *all_posts[0]) {
    uint32_t did = first_p.doc_id;
    std::vector<std::vector<int>> pos_lists;
    pos_lists.reserve(all_posts.size());
    pos_lists.push_back(first_p.positions);
    bool has_all = true;
    for (size_t t = 1; t < all_posts.size(); t++) {
      const auto& vec = *all_posts[t];
      auto pos = std::lower_bound(
          vec.begin(), vec.end(), did,
          [](const Posting& a, uint32_t id) { return a.doc_id < id; });
      if (pos == vec.end() || pos->doc_id != did) {
        has_all = false;
        break;
      }
      pos_lists.push_back(pos->positions);
    }
    if (!has_all) continue;
    if (CheckPhrasePositions(pos_lists, slop, inorder)) {
      result.push_back(id_to_str_[did]);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<TextSearchResult> TextIndex::PhraseSearch(
    const std::vector<std::string>& terms, int slop, bool inorder) const {
  if (terms.empty() || live_docs_ == 0) return {};
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

size_t TextIndex::NumDocs() const { return live_docs_; }

void TextIndex::Clear() {
  id_to_str_.clear();
  doc_lengths_.clear();
  alive_.clear();
  doc_terms_.clear();
  free_ids_.clear();
  str_to_id_.clear();
  live_docs_ = 0;
  total_doc_len_ = 0;
  avg_doc_len_ = 0.0;
  postings_.clear();
  sorted_terms_.clear();
  reversed_terms_.clear();
  stem_map_.clear();
  phonetic_map_.clear();
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
