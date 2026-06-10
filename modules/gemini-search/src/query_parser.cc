#include "query_parser.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <set>
#include <unordered_set>

// =============================================================
// Set operations on sorted vectors
// =============================================================

std::vector<std::string> SetIntersect(const std::vector<std::string>& a,
                                       const std::vector<std::string>& b) {
  std::vector<std::string> out;
  std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                        std::back_inserter(out));
  return out;
}

std::vector<std::string> SetUnion(const std::vector<std::string>& a,
                                   const std::vector<std::string>& b) {
  std::vector<std::string> out;
  std::set_union(a.begin(), a.end(), b.begin(), b.end(),
                 std::back_inserter(out));
  return out;
}

std::vector<std::string> SetDifference(const std::vector<std::string>& a,
                                        const std::vector<std::string>& b) {
  std::vector<std::string> out;
  std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                      std::back_inserter(out));
  return out;
}

// =============================================================
// Query evaluation
// =============================================================

std::vector<std::string> EvaluateQuery(
    const QueryNode& node, const IndexSpec& spec,
    const DocumentStore& doc_store, const TagFieldIndices& tag_indices,
    const NumericFieldIndices& numeric_indices,
    const TextFieldIndices& text_indices, std::string& error_msg,
    const std::vector<std::string>& infields) {
  switch (node.type) {
    case QueryNode::Type::kMatchAll:
      return doc_store.AllIds();

    case QueryNode::Type::kTagMatch: {
      const auto* fspec = spec.FindField(node.field_name);
      if (!fspec) {
        error_msg = "ERR query field not in schema";
        return {};
      }
      if (fspec->type != FieldType::kTag) {
        error_msg = "ERR field is not a TAG field";
        return {};
      }
      const auto* idx = tag_indices.Get(node.field_name);
      if (!idx) return {};
      if (node.tag_values.size() == 1) {
        return idx->Lookup(node.tag_values[0]);
      }
      return idx->LookupOr(node.tag_values);
    }

    case QueryNode::Type::kNumericRange: {
      const auto* fspec = spec.FindField(node.field_name);
      if (!fspec) {
        error_msg = "ERR query field not in schema";
        return {};
      }
      if (fspec->type != FieldType::kNumeric) {
        error_msg = "ERR field is not a NUMERIC field";
        return {};
      }
      const auto* idx = numeric_indices.Get(node.field_name);
      if (!idx) return {};
      return idx->RangeQuery(node.range_min, node.min_exclusive,
                              node.range_max, node.max_exclusive);
    }

    case QueryNode::Type::kTextMatch: {
      auto LookupWithMod = [](const TextIndex* idx, const std::string& term,
                              const TextTermModifier& mod) -> std::vector<std::string> {
        if (mod.is_prefix) return idx->PrefixLookup(term);
        if (mod.fuzzy_dist > 0) return idx->FuzzyLookup(term, mod.fuzzy_dist);
        return idx->Lookup(term);
      };

      TextTermModifier default_mod;
      if (!node.field_name.empty()) {
        const auto* fspec = spec.FindField(node.field_name);
        if (!fspec) {
          error_msg = "ERR query field not in schema";
          return {};
        }
        if (fspec->type != FieldType::kText) {
          error_msg = "ERR field is not a TEXT field";
          return {};
        }
        const auto* idx = text_indices.Get(node.field_name);
        if (!idx) return {};
        std::set<std::string> merged;
        for (size_t i = 0; i < node.text_terms.size(); i++) {
          const auto& mod = (i < node.text_term_mods.size()) ? node.text_term_mods[i] : default_mod;
          auto ids = LookupWithMod(idx, node.text_terms[i], mod);
          merged.insert(ids.begin(), ids.end());
        }
        return {merged.begin(), merged.end()};
      }
      std::set<std::string> merged;
      std::unordered_set<std::string> infield_set(infields.begin(), infields.end());
      for (auto& f : spec.fields) {
        if (f.type != FieldType::kText) continue;
        if (!infield_set.empty() && infield_set.find(f.name) == infield_set.end()) continue;
        const auto* idx = text_indices.Get(f.name);
        if (!idx) continue;
        for (size_t i = 0; i < node.text_terms.size(); i++) {
          const auto& mod = (i < node.text_term_mods.size()) ? node.text_term_mods[i] : default_mod;
          auto ids = LookupWithMod(idx, node.text_terms[i], mod);
          merged.insert(ids.begin(), ids.end());
        }
      }
      return {merged.begin(), merged.end()};
    }

    case QueryNode::Type::kOptional:
      return doc_store.AllIds();

    case QueryNode::Type::kAnd: {
      if (node.children.size() != 2) {
        error_msg = "ERR internal: AND requires 2 children";
        return {};
      }
      auto left = EvaluateQuery(node.children[0], spec, doc_store,
                                 tag_indices, numeric_indices, text_indices,
                                 error_msg, infields);
      if (!error_msg.empty()) return {};
      auto right = EvaluateQuery(node.children[1], spec, doc_store,
                                  tag_indices, numeric_indices, text_indices,
                                  error_msg, infields);
      if (!error_msg.empty()) return {};
      return SetIntersect(left, right);
    }

    case QueryNode::Type::kOr: {
      if (node.children.size() != 2) {
        error_msg = "ERR internal: OR requires 2 children";
        return {};
      }
      auto left = EvaluateQuery(node.children[0], spec, doc_store,
                                 tag_indices, numeric_indices, text_indices,
                                 error_msg, infields);
      if (!error_msg.empty()) return {};
      auto right = EvaluateQuery(node.children[1], spec, doc_store,
                                  tag_indices, numeric_indices, text_indices,
                                  error_msg, infields);
      if (!error_msg.empty()) return {};
      return SetUnion(left, right);
    }

    case QueryNode::Type::kNot: {
      if (node.children.size() != 1) {
        error_msg = "ERR internal: NOT requires 1 child";
        return {};
      }
      auto all = doc_store.AllIds();
      auto child = EvaluateQuery(node.children[0], spec, doc_store,
                                  tag_indices, numeric_indices, text_indices,
                                  error_msg, infields);
      if (!error_msg.empty()) return {};
      return SetDifference(all, child);
    }
  }
  error_msg = "ERR internal: unknown query node type";
  return {};
}

// =============================================================
// Score-aware query evaluation (for WITHSCORES)
// =============================================================

static std::vector<ScoredResult> ScoredIntersect(
    const std::vector<ScoredResult>& a, const std::vector<ScoredResult>& b) {
  std::unordered_map<std::string, double> b_map;
  for (auto& r : b) b_map[r.doc_id] = r.score;
  std::vector<ScoredResult> out;
  for (auto& r : a) {
    auto it = b_map.find(r.doc_id);
    if (it != b_map.end()) {
      out.push_back({r.doc_id, r.score + it->second});
    }
  }
  std::sort(out.begin(), out.end(),
            [](const ScoredResult& x, const ScoredResult& y) {
              return x.doc_id < y.doc_id;
            });
  return out;
}

static std::vector<ScoredResult> ScoredUnion(
    const std::vector<ScoredResult>& a, const std::vector<ScoredResult>& b) {
  std::unordered_map<std::string, double> merged;
  for (auto& r : a) merged[r.doc_id] += r.score;
  for (auto& r : b) merged[r.doc_id] += r.score;
  std::vector<ScoredResult> out;
  out.reserve(merged.size());
  for (auto& [id, sc] : merged) out.push_back({id, sc});
  std::sort(out.begin(), out.end(),
            [](const ScoredResult& x, const ScoredResult& y) {
              return x.doc_id < y.doc_id;
            });
  return out;
}

static std::vector<ScoredResult> ScoredDifference(
    const std::vector<ScoredResult>& a, const std::vector<ScoredResult>& b) {
  std::unordered_set<std::string> b_set;
  for (auto& r : b) b_set.insert(r.doc_id);
  std::vector<ScoredResult> out;
  for (auto& r : a) {
    if (b_set.find(r.doc_id) == b_set.end()) out.push_back(r);
  }
  return out;
}

std::vector<ScoredResult> EvaluateQueryScored(
    const QueryNode& node, const IndexSpec& spec,
    const DocumentStore& doc_store, const TagFieldIndices& tag_indices,
    const NumericFieldIndices& numeric_indices,
    const TextFieldIndices& text_indices, std::string& error_msg,
    const std::vector<std::string>& infields) {
  switch (node.type) {
    case QueryNode::Type::kMatchAll: {
      auto ids = doc_store.AllIds();
      std::vector<ScoredResult> out;
      out.reserve(ids.size());
      for (auto& id : ids) out.push_back({id, 1.0});
      return out;
    }

    case QueryNode::Type::kTagMatch: {
      auto ids = EvaluateQuery(node, spec, doc_store, tag_indices,
                               numeric_indices, text_indices, error_msg, infields);
      if (!error_msg.empty()) return {};
      std::vector<ScoredResult> out;
      out.reserve(ids.size());
      for (auto& id : ids) out.push_back({id, 1.0});
      return out;
    }

    case QueryNode::Type::kNumericRange: {
      auto ids = EvaluateQuery(node, spec, doc_store, tag_indices,
                               numeric_indices, text_indices, error_msg, infields);
      if (!error_msg.empty()) return {};
      std::vector<ScoredResult> out;
      out.reserve(ids.size());
      for (auto& id : ids) out.push_back({id, 1.0});
      return out;
    }

    case QueryNode::Type::kTextMatch: {
      auto SearchWithMod = [](const TextIndex* idx, const std::string& term,
                              const TextTermModifier& mod) -> std::vector<TextSearchResult> {
        if (mod.is_prefix) return idx->PrefixSearch(term);
        if (mod.fuzzy_dist > 0) return idx->FuzzySearch(term, mod.fuzzy_dist);
        return idx->Search({term});
      };

      TextTermModifier default_mod;
      if (!node.field_name.empty()) {
        const auto* fspec = spec.FindField(node.field_name);
        if (!fspec) {
          error_msg = "ERR query field not in schema";
          return {};
        }
        if (fspec->type != FieldType::kText) {
          error_msg = "ERR field is not a TEXT field";
          return {};
        }
        const auto* idx = text_indices.Get(node.field_name);
        if (!idx) return {};
        bool has_mods = !node.text_term_mods.empty();
        bool any_mod = false;
        if (has_mods) {
          for (auto& m : node.text_term_mods) {
            if (m.is_prefix || m.fuzzy_dist > 0) { any_mod = true; break; }
          }
        }
        if (!any_mod) {
          auto results = idx->Search(node.text_terms);
          std::vector<ScoredResult> out;
          out.reserve(results.size());
          for (auto& r : results) out.push_back({r.doc_id, r.score});
          std::sort(out.begin(), out.end(),
                    [](const ScoredResult& x, const ScoredResult& y) {
                      return x.doc_id < y.doc_id;
                    });
          return out;
        }
        std::unordered_map<std::string, double> score_map;
        for (size_t i = 0; i < node.text_terms.size(); i++) {
          const auto& mod = (i < node.text_term_mods.size()) ? node.text_term_mods[i] : default_mod;
          auto results = SearchWithMod(idx, node.text_terms[i], mod);
          for (auto& r : results) score_map[r.doc_id] += r.score;
        }
        std::vector<ScoredResult> out;
        out.reserve(score_map.size());
        for (auto& [id, sc] : score_map) out.push_back({id, sc});
        std::sort(out.begin(), out.end(),
                  [](const ScoredResult& x, const ScoredResult& y) {
                    return x.doc_id < y.doc_id;
                  });
        return out;
      }
      std::unordered_map<std::string, double> merged;
      std::unordered_set<std::string> infield_set(infields.begin(), infields.end());
      bool has_mods = !node.text_term_mods.empty();
      bool any_mod = false;
      if (has_mods) {
        for (auto& m : node.text_term_mods) {
          if (m.is_prefix || m.fuzzy_dist > 0) { any_mod = true; break; }
        }
      }
      for (auto& f : spec.fields) {
        if (f.type != FieldType::kText) continue;
        if (!infield_set.empty() && infield_set.find(f.name) == infield_set.end()) continue;
        const auto* idx = text_indices.Get(f.name);
        if (!idx) continue;
        if (!any_mod) {
          auto results = idx->Search(node.text_terms);
          for (auto& r : results) merged[r.doc_id] += r.score;
        } else {
          for (size_t i = 0; i < node.text_terms.size(); i++) {
            const auto& mod = (i < node.text_term_mods.size()) ? node.text_term_mods[i] : default_mod;
            auto results = SearchWithMod(idx, node.text_terms[i], mod);
            for (auto& r : results) merged[r.doc_id] += r.score;
          }
        }
      }
      std::vector<ScoredResult> out;
      out.reserve(merged.size());
      for (auto& [id, sc] : merged) out.push_back({id, sc});
      std::sort(out.begin(), out.end(),
                [](const ScoredResult& x, const ScoredResult& y) {
                  return x.doc_id < y.doc_id;
                });
      return out;
    }

    case QueryNode::Type::kOptional: {
      if (node.children.size() != 1) {
        error_msg = "ERR internal: OPTIONAL requires 1 child";
        return {};
      }
      auto child = EvaluateQueryScored(node.children[0], spec, doc_store,
                                        tag_indices, numeric_indices,
                                        text_indices, error_msg, infields);
      if (!error_msg.empty()) return {};
      auto all_ids = doc_store.AllIds();
      std::unordered_map<std::string, double> score_map;
      for (auto& r : child) score_map[r.doc_id] = r.score;
      std::vector<ScoredResult> out;
      out.reserve(all_ids.size());
      for (auto& id : all_ids) {
        auto it = score_map.find(id);
        out.push_back({id, it != score_map.end() ? it->second : 0.0});
      }
      std::sort(out.begin(), out.end(),
                [](const ScoredResult& x, const ScoredResult& y) {
                  return x.doc_id < y.doc_id;
                });
      return out;
    }

    case QueryNode::Type::kAnd: {
      if (node.children.size() != 2) {
        error_msg = "ERR internal: AND requires 2 children";
        return {};
      }
      auto left = EvaluateQueryScored(node.children[0], spec, doc_store,
                                       tag_indices, numeric_indices,
                                       text_indices, error_msg, infields);
      if (!error_msg.empty()) return {};
      auto right = EvaluateQueryScored(node.children[1], spec, doc_store,
                                        tag_indices, numeric_indices,
                                        text_indices, error_msg, infields);
      if (!error_msg.empty()) return {};
      return ScoredIntersect(left, right);
    }

    case QueryNode::Type::kOr: {
      if (node.children.size() != 2) {
        error_msg = "ERR internal: OR requires 2 children";
        return {};
      }
      auto left = EvaluateQueryScored(node.children[0], spec, doc_store,
                                       tag_indices, numeric_indices,
                                       text_indices, error_msg, infields);
      if (!error_msg.empty()) return {};
      auto right = EvaluateQueryScored(node.children[1], spec, doc_store,
                                        tag_indices, numeric_indices,
                                        text_indices, error_msg, infields);
      if (!error_msg.empty()) return {};
      return ScoredUnion(left, right);
    }

    case QueryNode::Type::kNot: {
      if (node.children.size() != 1) {
        error_msg = "ERR internal: NOT requires 1 child";
        return {};
      }
      auto all_ids = doc_store.AllIds();
      std::vector<ScoredResult> all;
      all.reserve(all_ids.size());
      for (auto& id : all_ids) all.push_back({id, 1.0});
      auto child = EvaluateQueryScored(node.children[0], spec, doc_store,
                                        tag_indices, numeric_indices,
                                        text_indices, error_msg, infields);
      if (!error_msg.empty()) return {};
      return ScoredDifference(all, child);
    }
  }
  error_msg = "ERR internal: unknown query node type";
  return {};
}

// =============================================================
// Recursive descent parser
// =============================================================

static bool ParseNumericBound(const std::string& token, double& value,
                              bool& exclusive) {
  std::string s = token;
  exclusive = false;
  if (!s.empty() && s[0] == '(') {
    exclusive = true;
    s = s.substr(1);
  }
  if (s == "inf" || s == "+inf") {
    value = std::numeric_limits<double>::infinity();
    return true;
  }
  if (s == "-inf") {
    value = -std::numeric_limits<double>::infinity();
    return true;
  }
  char* endptr = nullptr;
  value = std::strtod(s.c_str(), &endptr);
  return endptr != s.c_str() && *endptr == '\0' && !std::isnan(value);
}

struct Parser {
  const std::string& input;
  size_t pos = 0;
  std::string& error_msg;
  bool nostopwords = false;

  Parser(const std::string& s, std::string& err, bool nostop = false)
      : input(s), error_msg(err), nostopwords(nostop) {}

  char Peek() const {
    if (pos >= input.size()) return '\0';
    return input[pos];
  }

  void SkipSpaces() {
    while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\t')) {
      pos++;
    }
  }

  // Find matching closing bracket/brace, respecting nesting
  size_t FindClosing(char open, char close, size_t start) const {
    int depth = 1;
    for (size_t i = start; i < input.size(); i++) {
      if (input[i] == open) depth++;
      if (input[i] == close) {
        depth--;
        if (depth == 0) return i;
      }
    }
    return std::string::npos;
  }

  // Detect and strip prefix/fuzzy/optional modifiers from a raw token.
  // Returns the cleaned base term and fills the modifier.
  static std::string StripModifiers(const std::string& raw, TextTermModifier& mod) {
    std::string s = raw;
    mod = {};

    // Optional: leading ~
    if (!s.empty() && s[0] == '~') {
      mod.is_optional = true;
      s = s.substr(1);
    }

    // Fuzzy: wrapped in % pairs (1-3 deep)
    int pct = 0;
    while (s.size() >= 2 && s[0] == '%' && s.back() == '%') {
      pct++;
      s = s.substr(1, s.size() - 2);
      if (pct >= 3) break;
    }
    if (pct > 0) mod.fuzzy_dist = pct;

    // Prefix: trailing *
    if (!s.empty() && s.back() == '*') {
      mod.is_prefix = true;
      s.pop_back();
    }

    // Lowercase the base
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  }

  // Parse a text body (possibly multi-word) into terms with modifiers.
  // Used for @field:(term1 %term2% hel*) and bare terms.
  void ParseTextTermsWithModifiers(const std::string& body,
                                    std::vector<std::string>& terms,
                                    std::vector<TextTermModifier>& mods) {
    terms.clear();
    mods.clear();
    size_t i = 0;
    while (i < body.size()) {
      while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) i++;
      if (i >= body.size()) break;
      size_t start = i;
      while (i < body.size() && body[i] != ' ' && body[i] != '\t') i++;
      std::string raw = body.substr(start, i - start);
      TextTermModifier mod;
      std::string base = StripModifiers(raw, mod);
      if (base.empty()) continue;
      if (!mod.is_prefix && !mod.fuzzy_dist && !mod.is_optional) {
        // Plain term: apply stop word filter unless nostopwords
        auto filtered = nostopwords ? TextIndex::TokenizeRaw(base)
                                    : TextIndex::Tokenize(base);
        for (auto& t : filtered) {
          terms.push_back(std::move(t));
          mods.push_back(mod);
        }
      } else {
        // Modified term: don't filter (prefix/fuzzy/optional always kept)
        terms.push_back(std::move(base));
        mods.push_back(mod);
      }
    }
  }

  // Parse a @field:{...} or @field:[...] leaf, starting at '@'
  bool ParseFieldExpr(QueryNode& out) {
    if (Peek() != '@') {
      error_msg = "ERR syntax error: expected @";
      return false;
    }
    pos++;

    size_t field_start = pos;
    while (pos < input.size() && input[pos] != ':') {
      pos++;
    }
    if (pos >= input.size() || input[pos] != ':') {
      error_msg = "ERR syntax error: expected : after field name";
      return false;
    }
    std::string field_name = input.substr(field_start, pos - field_start);
    if (field_name.empty()) {
      error_msg = "ERR syntax error: empty field name";
      return false;
    }
    pos++;  // skip ':'

    if (pos >= input.size()) {
      error_msg = "ERR syntax error: unexpected end after :";
      return false;
    }

    if (input[pos] == '{') {
      // TAG match
      size_t close = FindClosing('{', '}', pos + 1);
      if (close == std::string::npos) {
        error_msg = "ERR syntax error: expected closing }";
        return false;
      }
      std::string body = input.substr(pos + 1, close - pos - 1);
      pos = close + 1;

      if (body.empty()) {
        error_msg = "ERR syntax error: empty tag value";
        return false;
      }
      if (body.back() == '|') {
        error_msg = "ERR syntax error: trailing | in tag value list";
        return false;
      }

      std::vector<std::string> values;
      size_t vp = 0;
      while (vp < body.size()) {
        size_t pipe = body.find('|', vp);
        if (pipe == std::string::npos) pipe = body.size();
        std::string val = body.substr(vp, pipe - vp);
        if (val.empty()) {
          error_msg = "ERR syntax error: empty tag value in OR list";
          return false;
        }
        values.push_back(std::move(val));
        vp = pipe + 1;
      }

      out.type = QueryNode::Type::kTagMatch;
      out.field_name = std::move(field_name);
      out.tag_values = std::move(values);
      return true;
    }

    if (input[pos] == '[') {
      // Numeric range
      size_t close = FindClosing('[', ']', pos + 1);
      if (close == std::string::npos) {
        error_msg = "ERR syntax error: expected closing ]";
        return false;
      }
      std::string body = input.substr(pos + 1, close - pos - 1);
      pos = close + 1;

      if (body.empty()) {
        error_msg = "ERR syntax error: empty numeric range";
        return false;
      }

      size_t space = body.find(' ');
      if (space == std::string::npos) {
        error_msg = "ERR syntax error: numeric range requires two bounds";
        return false;
      }
      std::string min_tok = body.substr(0, space);
      std::string max_tok = body.substr(space + 1);
      if (min_tok.empty() || max_tok.empty()) {
        error_msg = "ERR syntax error: empty numeric bound";
        return false;
      }

      double min_val, max_val;
      bool min_excl, max_excl;
      if (!ParseNumericBound(min_tok, min_val, min_excl)) {
        error_msg = "ERR syntax error: invalid numeric min value";
        return false;
      }
      if (!ParseNumericBound(max_tok, max_val, max_excl)) {
        error_msg = "ERR syntax error: invalid numeric max value";
        return false;
      }

      out.type = QueryNode::Type::kNumericRange;
      out.field_name = std::move(field_name);
      out.range_min = min_val;
      out.range_max = max_val;
      out.min_exclusive = min_excl;
      out.max_exclusive = max_excl;
      return true;
    }

    // TEXT match: @field:term or @field:(term1 term2)
    if (input[pos] == '(') {
      size_t close = FindClosing('(', ')', pos + 1);
      if (close == std::string::npos) {
        error_msg = "ERR syntax error: expected closing )";
        return false;
      }
      std::string body = input.substr(pos + 1, close - pos - 1);
      pos = close + 1;
      std::vector<std::string> terms;
      std::vector<TextTermModifier> mods;
      ParseTextTermsWithModifiers(body, terms, mods);
      if (terms.empty()) {
        error_msg = "ERR syntax error: empty text query";
        return false;
      }
      out.type = QueryNode::Type::kTextMatch;
      out.field_name = std::move(field_name);
      out.text_terms = std::move(terms);
      out.text_term_mods = std::move(mods);
      return true;
    }

    {
      size_t term_start = pos;
      while (pos < input.size() && input[pos] != ' ' && input[pos] != '\t' &&
             input[pos] != ')' && input[pos] != '|' && input[pos] != '=' &&
             input[pos] != '\0') {
        pos++;
      }
      if (pos == term_start) {
        error_msg = "ERR syntax error: expected {, [, or text term after field:";
        return false;
      }
      std::string raw = input.substr(term_start, pos - term_start);
      std::vector<std::string> terms;
      std::vector<TextTermModifier> mods;
      ParseTextTermsWithModifiers(raw, terms, mods);
      out.type = QueryNode::Type::kTextMatch;
      out.field_name = std::move(field_name);
      out.text_terms = std::move(terms);
      out.text_term_mods = std::move(mods);
      return true;
    }
  }

  // Primary: '(' expr ')', '*', '@field:...'
  bool ParsePrimary(QueryNode& out) {
    SkipSpaces();
    if (Peek() == '(') {
      pos++;
      if (!ParseOr(out)) return false;
      SkipSpaces();
      if (Peek() != ')') {
        error_msg = "ERR syntax error: expected closing )";
        return false;
      }
      pos++;
      return true;
    }
    if (Peek() == '*') {
      pos++;
      out.type = QueryNode::Type::kMatchAll;
      return true;
    }
    if (Peek() == '@') {
      return ParseFieldExpr(out);
    }
    // Bare term (including prefix hel*, fuzzy %hello%): full-text search
    if (std::isalnum(static_cast<unsigned char>(Peek())) || Peek() == '%') {
      size_t term_start = pos;
      while (pos < input.size() && input[pos] != ' ' && input[pos] != '\t' &&
             input[pos] != ')' && input[pos] != '|' && input[pos] != '=' &&
             input[pos] != '\0') {
        pos++;
      }
      std::string raw = input.substr(term_start, pos - term_start);
      std::vector<std::string> terms;
      std::vector<TextTermModifier> mods;
      ParseTextTermsWithModifiers(raw, terms, mods);
      if (terms.empty()) {
        out.type = QueryNode::Type::kTextMatch;
        out.field_name = "";
        out.text_terms = {};
        out.text_term_mods = {};
        return true;
      }
      out.type = QueryNode::Type::kTextMatch;
      out.field_name = "";
      out.text_terms = std::move(terms);
      out.text_term_mods = std::move(mods);
      return true;
    }
    error_msg = "ERR syntax error: expected @, *, (, or term";
    return false;
  }

  // Unary: '-' unary | '~' unary | primary
  bool ParseUnary(QueryNode& out) {
    SkipSpaces();
    if (Peek() == '-') {
      pos++;
      QueryNode child;
      if (!ParseUnary(child)) return false;
      out.type = QueryNode::Type::kNot;
      out.children.push_back(std::move(child));
      return true;
    }
    if (Peek() == '~') {
      pos++;
      QueryNode child;
      if (!ParsePrimary(child)) return false;
      out.type = QueryNode::Type::kOptional;
      out.children.push_back(std::move(child));
      return true;
    }
    return ParsePrimary(out);
  }

  // And: unary (space unary)*
  // Implicit AND: two expressions separated by space (no explicit operator)
  bool ParseAnd(QueryNode& out) {
    if (!ParseUnary(out)) return false;

    while (true) {
      SkipSpaces();
      char c = Peek();
      // Stop at OR operator, closing paren, end of input, or KNN arrow
      if (c == '|' || c == ')' || c == '\0') break;
      // Check for '=>' (KNN suffix) — stop AND parsing
      if (c == '=' && pos + 1 < input.size() && input[pos + 1] == '>') break;

      QueryNode right;
      if (!ParseUnary(right)) return false;

      QueryNode combined;
      combined.type = QueryNode::Type::kAnd;
      combined.children.push_back(std::move(out));
      combined.children.push_back(std::move(right));
      out = std::move(combined);
    }
    return true;
  }

  // Or: and ('|' and)*
  bool ParseOr(QueryNode& out) {
    if (!ParseAnd(out)) return false;

    while (true) {
      SkipSpaces();
      if (Peek() != '|') break;
      pos++;  // skip '|'

      QueryNode right;
      if (!ParseAnd(right)) return false;

      QueryNode combined;
      combined.type = QueryNode::Type::kOr;
      combined.children.push_back(std::move(out));
      combined.children.push_back(std::move(right));
      out = std::move(combined);
    }
    return true;
  }
};

// Parse KNN suffix: [KNN k @field $param]
static bool ParseKnnSuffix(const std::string& knn_str, ParsedQuery& out,
                           std::string& error_msg) {
  std::string s = knn_str;
  // Trim
  size_t ks = s.find_first_not_of(" \t");
  if (ks != std::string::npos) s = s.substr(ks);
  size_t ke = s.find_last_not_of(" \t");
  if (ke != std::string::npos) s = s.substr(0, ke + 1);

  if (s.empty() || s.front() != '[' || s.back() != ']') {
    error_msg = "ERR syntax error: KNN clause must be [KNN k @field $param]";
    return false;
  }
  std::string body = s.substr(1, s.size() - 2);

  std::vector<std::string> tokens;
  size_t tp = 0;
  while (tp < body.size()) {
    size_t sp = body.find(' ', tp);
    if (sp == std::string::npos) sp = body.size();
    if (sp > tp) tokens.push_back(body.substr(tp, sp - tp));
    tp = sp + 1;
  }

  if (tokens.size() != 4 || (tokens[0] != "KNN" && tokens[0] != "knn")) {
    error_msg = "ERR syntax error: expected [KNN k @field $param]";
    return false;
  }

  char* endptr = nullptr;
  long k_val = std::strtol(tokens[1].c_str(), &endptr, 10);
  if (*endptr != '\0' || k_val <= 0) {
    error_msg = "ERR syntax error: KNN k must be a positive integer";
    return false;
  }

  if (tokens[2].empty() || tokens[2][0] != '@') {
    error_msg = "ERR syntax error: KNN field must start with @";
    return false;
  }
  std::string knn_field = tokens[2].substr(1);
  if (knn_field.empty()) {
    error_msg = "ERR syntax error: empty KNN field name";
    return false;
  }

  if (tokens[3].empty() || tokens[3][0] != '$') {
    error_msg = "ERR syntax error: KNN param must start with $";
    return false;
  }
  std::string knn_param = tokens[3].substr(1);
  if (knn_param.empty()) {
    error_msg = "ERR syntax error: empty KNN param name";
    return false;
  }

  out.has_knn = true;
  out.knn_k = static_cast<size_t>(k_val);
  out.knn_field = std::move(knn_field);
  out.knn_param_name = std::move(knn_param);
  return true;
}

bool ParseQuery(const std::string& input, ParsedQuery& out,
                std::string& error_msg, const QueryOptions& qopts) {
  // Trim
  size_t start = input.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    error_msg = "ERR empty query";
    return false;
  }
  size_t end = input.find_last_not_of(" \t\r\n");
  std::string trimmed = input.substr(start, end - start + 1);

  // Check for => KNN suffix
  size_t arrow = trimmed.find("=>");
  if (arrow != std::string::npos) {
    std::string filter_part = trimmed.substr(0, arrow);
    std::string knn_part = trimmed.substr(arrow + 2);

    if (!ParseKnnSuffix(knn_part, out, error_msg)) return false;

    // Parse filter part as query tree
    size_t fs = filter_part.find_first_not_of(" \t");
    if (fs == std::string::npos) {
      error_msg = "ERR syntax error: empty pre-filter before =>";
      return false;
    }
    size_t fe = filter_part.find_last_not_of(" \t");
    filter_part = filter_part.substr(fs, fe - fs + 1);

    Parser parser(filter_part, error_msg, qopts.nostopwords);
    if (!parser.ParseOr(out.root)) return false;
    parser.SkipSpaces();
    if (parser.pos != filter_part.size()) {
      error_msg = "ERR syntax error: unexpected input after query";
      return false;
    }
    return true;
  }

  // No KNN — parse as regular query tree
  Parser parser(trimmed, error_msg, qopts.nostopwords);
  if (!parser.ParseOr(out.root)) return false;
  parser.SkipSpaces();
  if (parser.pos != trimmed.size()) {
    error_msg = "ERR syntax error: unexpected input after query";
    return false;
  }
  return true;
}
