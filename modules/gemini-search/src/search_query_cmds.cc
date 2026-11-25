#include "search_internal.h"
#include "hybrid_fusion.h"
#include "query_parser.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>


struct SearchOptions {
  std::vector<std::string> return_fields;
  std::unordered_map<std::string, std::string> return_aliases;
  bool has_return = false;
  bool withsortkeys = false;
  bool withpayloads = false;
  bool explainscore = false;
  ScorerType scorer = ScorerType::kBm25;
  std::string scorer_name = "BM25";
  std::string sortby_field;
  bool sortby_desc = false;
  bool has_sortby = false;
  long long limit_offset = 0;
  long long limit_count = -1;
  bool has_limit = false;
  bool nocontent = false;
  bool withscores = false;
  bool verbatim = false;
  bool nostopwords = false;
  std::vector<std::string> infields;
  bool has_infields = false;
  std::vector<std::string> inkeys;
  bool has_inkeys = false;
  long long timeout_ms = 0;
  bool has_timeout = false;
  int slop = 0;
  bool has_slop = false;
  bool inorder = false;
  std::string language;
  bool has_language = false;
  std::string geofilter_field;
  double geofilter_lon = 0;
  double geofilter_lat = 0;
  double geofilter_radius = 0;
  GeoUnit geofilter_unit = GeoUnit::kKm;
  bool has_geofilter = false;
  bool has_highlight = false;
  std::vector<std::string> highlight_fields;
  std::string highlight_open = "<b>";
  std::string highlight_close = "</b>";
  bool has_summarize = false;
  std::vector<std::string> summarize_fields;
  int summarize_frags = 3;
  int summarize_len = 20;
  std::string summarize_separator = "...";
  int dialect = 0;
  bool has_dialect = false;
  struct NumericFilterOpt {
    std::string field;
    double min = 0;
    double max = 0;
    bool min_exclusive = false;
    bool max_exclusive = false;
  };
  std::vector<NumericFilterOpt> numeric_filters;
};

static void CollectQueryTerms(const QueryNode& node,
                              std::unordered_set<std::string>& terms) {
  if (node.type == QueryNode::Type::kTextMatch) {
    for (auto& t : node.text_terms) terms.insert(t);
  }
  for (auto& child : node.children) CollectQueryTerms(child, terms);
}

static std::string ApplyHighlight(const std::string& text,
                                  const std::unordered_set<std::string>& terms,
                                  const std::string& open_tag,
                                  const std::string& close_tag) {
  std::string result;
  for (size_t i = 0; i < text.size(); ) {
    if (std::isalnum(static_cast<unsigned char>(text[i]))) {
      size_t start = i;
      while (i < text.size() && std::isalnum(static_cast<unsigned char>(text[i]))) i++;
      std::string word = text.substr(start, i - start);
      std::string lower = word;
      for (auto& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (terms.count(lower)) {
        result += open_tag + word + close_tag;
      } else {
        result += word;
      }
    } else {
      result += text[i];
      i++;
    }
  }
  return result;
}

static std::string ApplySummarize(const std::string& text,
                                  const std::unordered_set<std::string>& terms,
                                  int max_frags, int frag_len,
                                  const std::string& separator) {
  // Tokenize into words preserving their positions
  std::vector<std::string> words;
  std::vector<bool> is_match;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && !std::isalnum(static_cast<unsigned char>(text[i]))) i++;
    if (i >= text.size()) break;
    size_t start = i;
    while (i < text.size() && std::isalnum(static_cast<unsigned char>(text[i]))) i++;
    std::string word = text.substr(start, i - start);
    std::string lower = word;
    for (auto& c : lower)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    words.push_back(word);
    is_match.push_back(terms.count(lower) > 0);
  }

  if (words.empty()) return text;

  // Find match positions, extract fragments around them
  std::vector<std::string> fragments;
  std::set<int> used_positions;

  for (int wi = 0; wi < static_cast<int>(words.size()) && static_cast<int>(fragments.size()) < max_frags; wi++) {
    if (!is_match[wi] || used_positions.count(wi)) continue;

    int half = frag_len / 2;
    int frag_start = std::max(0, wi - half);
    int frag_end = std::min(static_cast<int>(words.size()), frag_start + frag_len);
    if (frag_end - frag_start < frag_len && frag_end == static_cast<int>(words.size()))
      frag_start = std::max(0, frag_end - frag_len);

    std::string frag;
    for (int fi = frag_start; fi < frag_end; fi++) {
      if (fi > frag_start) frag += " ";
      frag += words[fi];
      used_positions.insert(fi);
    }
    fragments.push_back(std::move(frag));
  }

  if (fragments.empty()) {
    // No matches found — return first frag_len words
    std::string frag;
    int limit = std::min(frag_len, static_cast<int>(words.size()));
    for (int fi = 0; fi < limit; fi++) {
      if (fi > 0) frag += " ";
      frag += words[fi];
    }
    return frag + separator;
  }

  std::string result;
  for (size_t fi = 0; fi < fragments.size(); fi++) {
    if (fi > 0) result += separator;
    result += fragments[fi];
  }
  return result;
}

static void ReplyWithDocFields(RedisModuleCtx* ctx, const Document* doc,
                               const SearchOptions& opts,
                               const std::string* score_val,
                               const std::unordered_set<std::string>& query_terms,
                               const char* score_name = "__vec_score") {
  if (!doc) {
    RedisModule_ReplyWithEmptyArray(ctx);
    return;
  }

  std::vector<std::string> keys;
  if (opts.has_return) {
    keys = opts.return_fields;
  } else {
    keys.reserve(doc->fields.size());
    for (auto& [k, v] : doc->fields) {
      keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());
  }

  long count = static_cast<long>(keys.size()) * 2;
  if (score_val) count += 2;
  if (score_val && opts.explainscore) count += 2;

  RedisModule_ReplyWithArray(ctx, count);
  if (score_val) {
    RedisModule_ReplyWithCString(ctx, score_name);
    RedisModule_ReplyWithCString(ctx, score_val->c_str());
    if (opts.explainscore) {
      std::string expl = opts.scorer_name + "(score=" + *score_val + ")";
      RedisModule_ReplyWithCString(ctx, "__score_explanation");
      RedisModule_ReplyWithCString(ctx, expl.c_str());
    }
  }

  std::unordered_set<std::string> hl_set(opts.highlight_fields.begin(),
                                          opts.highlight_fields.end());
  std::unordered_set<std::string> sm_set(opts.summarize_fields.begin(),
                                          opts.summarize_fields.end());

  for (auto& k : keys) {
    auto ait = opts.return_aliases.find(k);
    const std::string& out_key = (ait != opts.return_aliases.end()) ? ait->second : k;
    RedisModule_ReplyWithCString(ctx, out_key.c_str());
    auto fit = doc->fields.find(k);
    if (fit != doc->fields.end()) {
      std::string val = fit->second;
      bool do_hl = opts.has_highlight && (hl_set.empty() || hl_set.count(k));
      bool do_sm = opts.has_summarize && (sm_set.empty() || sm_set.count(k));
      if (do_sm && !query_terms.empty()) {
        val = ApplySummarize(val, query_terms, opts.summarize_frags,
                             opts.summarize_len, opts.summarize_separator);
      }
      if (do_hl && !query_terms.empty()) {
        val = ApplyHighlight(val, query_terms, opts.highlight_open, opts.highlight_close);
      }
      RedisModule_ReplyWithCString(ctx, val.c_str());
    } else {
      RedisModule_ReplyWithCString(ctx, "");
    }
  }
}

// FT.SEARCH <index> <query> [PARAMS ...] [RETURN ...] [SORTBY ...] [LIMIT ...]
int FtSearchCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  auto it = GetIndices().find(idx_str);
  if (it == GetIndices().end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  auto& entry = it->second;

  std::string query_str(ArgView(argv[2]));

  // Parse optional blocks: PARAMS, RETURN, SORTBY, LIMIT, etc.
  std::unordered_map<std::string, std::string> params;
  SearchOptions opts;
  int arg_i = 3;

  while (arg_i < argc) {
    auto kw = ArgView(argv[arg_i]);

    if (MatchArg(kw, "PARAMS")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR PARAMS requires count");
      }
      char* endptr = nullptr;
      long param_count =
          std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || param_count < 0 || param_count % 2 != 0) {
        return RedisModule_ReplyWithError(
            ctx, "ERR PARAMS count must be a non-negative even integer");
      }
      long pairs = param_count / 2;
      for (long p = 0; p < pairs; p++) {
        if (arg_i + 1 >= argc) {
          return RedisModule_ReplyWithError(ctx,
                                            "ERR not enough PARAMS values");
        }
        std::string pname(ArgView(argv[arg_i]));
        std::string pval = ArgStr(argv[arg_i + 1]);
        arg_i += 2;
        params[std::move(pname)] = std::move(pval);
      }
    } else if (MatchArg(kw, "RETURN")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR RETURN requires count");
      }
      char* endptr = nullptr;
      long ret_count =
          std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || ret_count < 0) {
        return RedisModule_ReplyWithError(
            ctx, "ERR RETURN count must be a non-negative integer");
      }
      opts.has_return = true;
      for (long r = 0; r < ret_count; ) {
        if (arg_i >= argc) {
          return RedisModule_ReplyWithError(ctx,
                                            "ERR not enough RETURN fields");
        }
        std::string fname(ArgView(argv[arg_i]));
        arg_i++;
        r++;
        if (r + 2 <= ret_count && arg_i + 1 < argc &&
            MatchArg(ArgView(argv[arg_i]), "AS")) {
          std::string alias(ArgView(argv[arg_i + 1]));
          opts.return_aliases[fname] = std::move(alias);
          arg_i += 2;
          r += 2;
        }
        opts.return_fields.push_back(std::move(fname));
      }
    } else if (MatchArg(kw, "SORTBY")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR SORTBY requires field");
      }
      opts.sortby_field = std::string(ArgView(argv[arg_i]));
      opts.has_sortby = true;
      arg_i++;
      if (arg_i < argc) {
        auto dir = ArgView(argv[arg_i]);
        if (MatchArg(dir, "ASC")) {
          opts.sortby_desc = false;
          arg_i++;
        } else if (MatchArg(dir, "DESC")) {
          opts.sortby_desc = true;
          arg_i++;
        }
      }
    } else if (MatchArg(kw, "LIMIT")) {
      arg_i++;
      if (arg_i + 1 >= argc) {
        return RedisModule_ReplyWithError(
            ctx, "ERR LIMIT requires offset and count");
      }
      char* ep1 = nullptr;
      char* ep2 = nullptr;
      std::string off_s(ArgView(argv[arg_i]));
      std::string cnt_s(ArgView(argv[arg_i + 1]));
      long long off = std::strtoll(off_s.c_str(), &ep1, 10);
      long long cnt = std::strtoll(cnt_s.c_str(), &ep2, 10);
      if (*ep1 != '\0' || *ep2 != '\0' || off < 0 || cnt < 0) {
        return RedisModule_ReplyWithError(
            ctx, "ERR LIMIT offset and count must be non-negative integers");
      }
      opts.limit_offset = off;
      opts.limit_count = cnt;
      opts.has_limit = true;
      arg_i += 2;
    } else if (MatchArg(kw, "NOCONTENT")) {
      opts.nocontent = true;
      arg_i++;
    } else if (MatchArg(kw, "WITHSCORES")) {
      opts.withscores = true;
      arg_i++;
    } else if (MatchArg(kw, "VERBATIM")) {
      opts.verbatim = true;
      arg_i++;
    } else if (MatchArg(kw, "NOSTOPWORDS")) {
      opts.nostopwords = true;
      arg_i++;
    } else if (MatchArg(kw, "INFIELDS")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR INFIELDS requires count");
      }
      char* endptr = nullptr;
      long inf_count =
          std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || inf_count < 0) {
        return RedisModule_ReplyWithError(
            ctx, "ERR INFIELDS count must be a non-negative integer");
      }
      opts.has_infields = true;
      for (long f = 0; f < inf_count; f++) {
        if (arg_i >= argc) {
          return RedisModule_ReplyWithError(ctx, "ERR not enough INFIELDS fields");
        }
        opts.infields.emplace_back(ArgView(argv[arg_i]));
        arg_i++;
      }
    } else if (MatchArg(kw, "INKEYS")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR INKEYS requires count");
      }
      char* endptr = nullptr;
      long ink_count =
          std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || ink_count < 0) {
        return RedisModule_ReplyWithError(
            ctx, "ERR INKEYS count must be a non-negative integer");
      }
      opts.has_inkeys = true;
      for (long k = 0; k < ink_count; k++) {
        if (arg_i >= argc) {
          return RedisModule_ReplyWithError(ctx, "ERR not enough INKEYS keys");
        }
        opts.inkeys.emplace_back(ArgView(argv[arg_i]));
        arg_i++;
      }
    } else if (MatchArg(kw, "SLOP")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR SLOP requires value");
      }
      char* endptr = nullptr;
      long slop_val =
          std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      if (*endptr != '\0' || slop_val < 0) {
        return RedisModule_ReplyWithError(
            ctx, "ERR SLOP must be a non-negative integer");
      }
      opts.slop = static_cast<int>(slop_val);
      opts.has_slop = true;
      arg_i++;
    } else if (MatchArg(kw, "INORDER")) {
      opts.inorder = true;
      arg_i++;
    } else if (MatchArg(kw, "LANGUAGE")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR LANGUAGE requires value");
      }
      opts.language = std::string(ArgView(argv[arg_i]));
      opts.has_language = true;
      arg_i++;
    } else if (MatchArg(kw, "TIMEOUT")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR TIMEOUT requires value");
      }
      char* endptr = nullptr;
      long long tms =
          std::strtoll(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      if (*endptr != '\0' || tms < 0) {
        return RedisModule_ReplyWithError(
            ctx, "ERR TIMEOUT must be a non-negative integer");
      }
      opts.timeout_ms = tms;
      opts.has_timeout = true;
      arg_i++;
    } else if (MatchArg(kw, "DIALECT")) {
      arg_i++;
      if (arg_i >= argc)
        return RedisModule_ReplyWithError(ctx, "ERR DIALECT requires value");
      char* ep = nullptr;
      opts.dialect = static_cast<int>(
          std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &ep, 10));
      opts.has_dialect = true;
      arg_i++;
    } else if (MatchArg(kw, "HIGHLIGHT")) {
      opts.has_highlight = true;
      arg_i++;
      if (arg_i < argc && MatchArg(ArgView(argv[arg_i]), "FIELDS")) {
        arg_i++;
        if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR HIGHLIGHT FIELDS requires count");
        char* endptr = nullptr;
        long hf_count = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
        arg_i++;
        if (*endptr != '\0' || hf_count < 0)
          return RedisModule_ReplyWithError(ctx, "ERR HIGHLIGHT FIELDS count must be non-negative");
        for (long hf = 0; hf < hf_count; hf++) {
          if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR not enough HIGHLIGHT FIELDS");
          opts.highlight_fields.emplace_back(ArgView(argv[arg_i]));
          arg_i++;
        }
      }
      if (arg_i < argc && MatchArg(ArgView(argv[arg_i]), "TAGS")) {
        arg_i++;
        if (arg_i + 1 >= argc)
          return RedisModule_ReplyWithError(ctx, "ERR HIGHLIGHT TAGS requires open and close tags");
        opts.highlight_open = std::string(ArgView(argv[arg_i]));
        arg_i++;
        opts.highlight_close = std::string(ArgView(argv[arg_i]));
        arg_i++;
      }
    } else if (MatchArg(kw, "SUMMARIZE")) {
      opts.has_summarize = true;
      arg_i++;
      if (arg_i < argc && MatchArg(ArgView(argv[arg_i]), "FIELDS")) {
        arg_i++;
        if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR SUMMARIZE FIELDS requires count");
        char* endptr = nullptr;
        long sf_count = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
        arg_i++;
        if (*endptr != '\0' || sf_count < 0)
          return RedisModule_ReplyWithError(ctx, "ERR SUMMARIZE FIELDS count must be non-negative");
        for (long sf = 0; sf < sf_count; sf++) {
          if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR not enough SUMMARIZE FIELDS");
          opts.summarize_fields.emplace_back(ArgView(argv[arg_i]));
          arg_i++;
        }
      }
      while (arg_i < argc) {
        auto sub = ArgView(argv[arg_i]);
        if (MatchArg(sub, "FRAGS")) {
          arg_i++;
          if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR SUMMARIZE FRAGS requires value");
          char* ep = nullptr;
          opts.summarize_frags = static_cast<int>(
              std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &ep, 10));
          arg_i++;
        } else if (MatchArg(sub, "LEN")) {
          arg_i++;
          if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR SUMMARIZE LEN requires value");
          char* ep = nullptr;
          opts.summarize_len = static_cast<int>(
              std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &ep, 10));
          arg_i++;
        } else if (MatchArg(sub, "SEPARATOR")) {
          arg_i++;
          if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR SUMMARIZE SEPARATOR requires value");
          opts.summarize_separator = std::string(ArgView(argv[arg_i]));
          arg_i++;
        } else {
          break;
        }
      }
    } else if (MatchArg(kw, "GEOFILTER")) {
      arg_i++;
      if (arg_i + 4 >= argc) {
        return RedisModule_ReplyWithError(
            ctx, "ERR GEOFILTER requires field lon lat radius unit");
      }
      opts.geofilter_field = std::string(ArgView(argv[arg_i]));
      arg_i++;
      char* ep1 = nullptr;
      char* ep2 = nullptr;
      char* ep3 = nullptr;
      opts.geofilter_lon = std::strtod(std::string(ArgView(argv[arg_i])).c_str(), &ep1);
      arg_i++;
      opts.geofilter_lat = std::strtod(std::string(ArgView(argv[arg_i])).c_str(), &ep2);
      arg_i++;
      opts.geofilter_radius = std::strtod(std::string(ArgView(argv[arg_i])).c_str(), &ep3);
      arg_i++;
      if (*ep1 != '\0' || *ep2 != '\0' || *ep3 != '\0' ||
          !std::isfinite(opts.geofilter_lon) || !std::isfinite(opts.geofilter_lat) ||
          !std::isfinite(opts.geofilter_radius) ||
          opts.geofilter_lon < -180.0 || opts.geofilter_lon > 180.0 ||
          opts.geofilter_lat < -90.0 || opts.geofilter_lat > 90.0 ||
          opts.geofilter_radius < 0) {
        return RedisModule_ReplyWithError(
            ctx, "ERR GEOFILTER invalid numeric values");
      }
      std::string unit_s(ArgView(argv[arg_i]));
      if (!ParseGeoUnit(unit_s, opts.geofilter_unit)) {
        return RedisModule_ReplyWithError(
            ctx, "ERR GEOFILTER unknown unit, expected m|km|mi|ft");
      }
      opts.has_geofilter = true;
      arg_i++;
    } else if (MatchArg(kw, "FILTER")) {
      arg_i++;
      if (arg_i + 2 >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR FILTER requires field min max");
      }
      SearchOptions::NumericFilterOpt nf;
      nf.field = std::string(ArgView(argv[arg_i++]));
      auto ParseBound = [](std::string_view sv, double& val, bool& excl) -> bool {
        std::string s(sv);
        if (!s.empty() && s[0] == '(') {
          excl = true;
          s = s.substr(1);
        }
        if (s == "-inf") { val = -std::numeric_limits<double>::infinity(); return true; }
        if (s == "+inf" || s == "inf") { val = std::numeric_limits<double>::infinity(); return true; }
        char* ep = nullptr;
        val = std::strtod(s.c_str(), &ep);
        return ep != s.c_str() && *ep == '\0' && !std::isnan(val);
      };
      if (!ParseBound(ArgView(argv[arg_i++]), nf.min, nf.min_exclusive) ||
          !ParseBound(ArgView(argv[arg_i++]), nf.max, nf.max_exclusive)) {
        return RedisModule_ReplyWithError(ctx, "ERR FILTER invalid numeric bounds");
      }
      opts.numeric_filters.push_back(std::move(nf));
    } else if (MatchArg(kw, "WITHSORTKEYS")) {
      opts.withsortkeys = true;
      arg_i++;
    } else if (MatchArg(kw, "WITHPAYLOADS")) {
      opts.withpayloads = true;
      arg_i++;
    } else if (MatchArg(kw, "EXPLAINSCORE")) {
      opts.explainscore = true;
      arg_i++;
    } else if (MatchArg(kw, "SCORER")) {
      arg_i++;
      if (arg_i >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR SCORER requires a name");
      }
      auto sname = ArgView(argv[arg_i]);
      if (MatchArg(sname, "BM25") || MatchArg(sname, "BM25STD")) {
        opts.scorer = ScorerType::kBm25;
        opts.scorer_name = "BM25";
      } else if (MatchArg(sname, "TFIDF")) {
        opts.scorer = ScorerType::kTfIdf;
        opts.scorer_name = "TFIDF";
      } else if (MatchArg(sname, "TFIDF.DOCNORM")) {
        opts.scorer = ScorerType::kTfIdfDocNorm;
        opts.scorer_name = "TFIDF.DOCNORM";
      } else if (MatchArg(sname, "DISMAX")) {
        opts.scorer = ScorerType::kDismax;
        opts.scorer_name = "DISMAX";
      } else if (MatchArg(sname, "DOCSCORE")) {
        opts.scorer = ScorerType::kDocScore;
        opts.scorer_name = "DOCSCORE";
      } else {
        return RedisModule_ReplyWithError(ctx, "ERR unknown SCORER");
      }
      arg_i++;
    } else {
      return RedisModule_ReplyWithError(ctx,
                                        "ERR unknown search option");
    }
  }

  // Build query options
  TouchIndex(entry);
  QueryOptions qopts;
  qopts.nostopwords = opts.nostopwords || opts.verbatim;
  qopts.infields = opts.infields;
  qopts.slop = opts.slop;
  qopts.inorder = opts.inorder;
  qopts.stem = !opts.verbatim;
  qopts.language = opts.has_language ? opts.language : entry.spec.language;
  qopts.dialect = opts.has_dialect ? opts.dialect : GetDefaultDialect();
  qopts.scorer = opts.scorer;
  qopts.max_expansions = GetMaxExpansions();
  qopts.min_stem_len = GetMinStemLen();
  if (opts.has_timeout && opts.timeout_ms > 0) {
    qopts.deadline_ms = NowMs() + opts.timeout_ms;
  }
  if (entry.spec.nohl || entry.spec.nooffsets) {
    opts.has_highlight = false;
  }

  ParsedQuery parsed;
  std::string parse_error;
  if (!ParseQuery(query_str, parsed, parse_error, qopts)) {
    return RedisModule_ReplyWithError(ctx, parse_error.c_str());
  }

  // Collect query terms for HIGHLIGHT/SUMMARIZE
  std::unordered_set<std::string> query_terms;
  if (opts.has_highlight || opts.has_summarize) {
    CollectQueryTerms(parsed.root, query_terms);
  }

  // Record start time for TIMEOUT
  struct timespec start_time;
  if (opts.has_timeout) {
    clock_gettime(CLOCK_MONOTONIC, &start_time);
  }

  // KNN path
  if (parsed.has_knn) {
    const auto* fspec = entry.spec.FindField(parsed.knn_field);
    if (!fspec) {
      return RedisModule_ReplyWithError(ctx, "ERR KNN field not in schema");
    }
    if (fspec->type != FieldType::kVector) {
      return RedisModule_ReplyWithError(
          ctx, "ERR KNN field is not a VECTOR field");
    }

    auto pit = params.find(parsed.knn_param_name);
    if (pit == params.end()) {
      return RedisModule_ReplyWithError(
          ctx, "ERR KNN param not found in PARAMS");
    }
    auto& blob = pit->second;
    std::vector<float> qvec;
    if (!DecodeVectorBlob(blob, fspec->vector_params.dim,
                          fspec->vector_params.data_type, qvec)) {
      return RedisModule_ReplyWithError(
          ctx, "ERR query vector dimension mismatch");
    }

    const auto* vidx = entry.vector_indices.Get(parsed.knn_field);
    std::vector<KnnResult> knn_results;
    if (vidx) {
      if (parsed.root.type != QueryNode::Type::kMatchAll) {
        std::string filter_error;
        auto candidates = EvaluateQuery(parsed.root, entry.spec, entry.doc_store,
                                        entry.tag_indices, entry.numeric_indices,
                                        entry.text_indices, entry.geo_indices,
                                        filter_error, qopts,
                                        &entry.geoshape_indices, &params,
                                        &entry.vector_indices);
        if (!filter_error.empty()) {
          return RedisModule_ReplyWithError(ctx, filter_error.c_str());
        }
        knn_results = vidx->KnnQueryFiltered(qvec.data(), parsed.knn_k, candidates);
      } else {
        knn_results = vidx->KnnQueryEf(qvec.data(), parsed.knn_k,
                                       parsed.knn_ef_runtime);
      }
    }

    // TIMEOUT check
    if (opts.has_timeout) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      long long elapsed_ms = (now.tv_sec - start_time.tv_sec) * 1000LL +
                             (now.tv_nsec - start_time.tv_nsec) / 1000000LL;
      if (elapsed_ms > opts.timeout_ms) {
        return RedisModule_ReplyWithError(ctx, "ERR query timed out");
      }
    }

    // INKEYS filter
    if (opts.has_inkeys) {
      std::unordered_set<std::string> allowed(opts.inkeys.begin(), opts.inkeys.end());
      knn_results.erase(
          std::remove_if(knn_results.begin(), knn_results.end(),
                         [&](const KnnResult& r) {
                           return allowed.find(r.doc_id) == allowed.end();
                         }),
          knn_results.end());
    }

    // Apply LIMIT to KNN results
    long long knn_total_before_limit = static_cast<long long>(knn_results.size());
    if (opts.has_limit && opts.limit_offset == 0 && opts.limit_count == 0) {
      RedisModule_ReplyWithArray(ctx, 1);
      RedisModule_ReplyWithLongLong(ctx, knn_total_before_limit);
      return REDISMODULE_OK;
    }
    if (opts.has_limit) {
      if (opts.limit_offset >= knn_total_before_limit) {
        knn_results.clear();
      } else {
        auto begin = knn_results.begin() + opts.limit_offset;
        auto end = (opts.limit_count >= 0 &&
                    opts.limit_offset + opts.limit_count < knn_total_before_limit)
                       ? knn_results.begin() + opts.limit_offset + opts.limit_count
                       : knn_results.end();
        knn_results = std::vector<KnnResult>(begin, end);
      }
    }

    long total = static_cast<long>(knn_results.size());
    long multiplier = 1 + (opts.withpayloads ? 1 : 0) + (opts.nocontent ? 0 : 1);
    RedisModule_ReplyWithArray(ctx, 1 + total * multiplier);
    RedisModule_ReplyWithLongLong(ctx, total);

    const char* score_field = parsed.knn_score_alias.empty()
                                  ? "__vec_score"
                                  : parsed.knn_score_alias.c_str();
    for (auto& kr : knn_results) {
      RedisModule_ReplyWithCString(ctx, kr.doc_id.c_str());
      const auto* doc = entry.doc_store.Get(kr.doc_id);
      if (opts.withpayloads) {
        if (doc && doc->has_payload) {
          RedisModule_ReplyWithStringBuffer(ctx, doc->payload.data(), doc->payload.size());
        } else {
          RedisModule_ReplyWithNull(ctx);
        }
      }
      if (!opts.nocontent) {
        auto score_str = std::to_string(kr.score);
        ReplyWithDocFields(ctx, doc, opts,
                           &score_str, query_terms, score_field);
      }
    }

    return REDISMODULE_OK;
  }

  // Non-KNN: evaluate query tree
  std::string eval_error;

  // Use scored evaluation when WITHSCORES is requested
  std::vector<ScoredResult> scored_results;
  std::vector<std::string> result_ids;

  if (opts.withscores) {
    scored_results = EvaluateQueryScored(
        parsed.root, entry.spec, entry.doc_store,
        entry.tag_indices, entry.numeric_indices,
        entry.text_indices, entry.geo_indices, eval_error, qopts,
        &entry.geoshape_indices, &params, &entry.vector_indices, &entry.synonyms);
    if (!eval_error.empty()) {
      return RedisModule_ReplyWithError(ctx, eval_error.c_str());
    }
  } else {
    result_ids = EvaluateQuery(
        parsed.root, entry.spec, entry.doc_store,
        entry.tag_indices, entry.numeric_indices,
        entry.text_indices, entry.geo_indices, eval_error, qopts,
        &entry.geoshape_indices, &params, &entry.vector_indices, &entry.synonyms);
    if (!eval_error.empty()) {
      return RedisModule_ReplyWithError(ctx, eval_error.c_str());
    }
  }

  // TIMEOUT check (coarse, post-evaluation)
  if (opts.has_timeout) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long elapsed_ms = (now.tv_sec - start_time.tv_sec) * 1000LL +
                           (now.tv_nsec - start_time.tv_nsec) / 1000000LL;
    if (elapsed_ms > opts.timeout_ms) {
      return RedisModule_ReplyWithError(ctx, "ERR query timed out");
    }
  }

  // INKEYS filter
  if (opts.has_inkeys) {
    std::unordered_set<std::string> allowed(opts.inkeys.begin(), opts.inkeys.end());
    if (opts.withscores) {
      scored_results.erase(
          std::remove_if(scored_results.begin(), scored_results.end(),
                         [&](const ScoredResult& r) {
                           return allowed.find(r.doc_id) == allowed.end();
                         }),
          scored_results.end());
    } else {
      result_ids.erase(
          std::remove_if(result_ids.begin(), result_ids.end(),
                         [&](const std::string& id) {
                           return allowed.find(id) == allowed.end();
                         }),
          result_ids.end());
    }
  }

  // FILTER numeric options
  for (const auto& nf : opts.numeric_filters) {
    const auto* nf_spec = entry.spec.FindField(nf.field);
    if (!nf_spec || nf_spec->type != FieldType::kNumeric) {
      return RedisModule_ReplyWithError(ctx, "ERR FILTER field is not a NUMERIC field");
    }
    const auto* nidx = entry.numeric_indices.Get(nf.field);
    if (!nidx) {
      scored_results.clear();
      result_ids.clear();
      break;
    }
    auto hits = nidx->RangeQuery(nf.min, nf.min_exclusive, nf.max, nf.max_exclusive);
    std::unordered_set<std::string> hit_set(hits.begin(), hits.end());
    if (opts.withscores) {
      scored_results.erase(
          std::remove_if(scored_results.begin(), scored_results.end(),
                         [&](const ScoredResult& r) { return hit_set.find(r.doc_id) == hit_set.end(); }),
          scored_results.end());
    } else {
      result_ids.erase(
          std::remove_if(result_ids.begin(), result_ids.end(),
                         [&](const std::string& id) { return hit_set.find(id) == hit_set.end(); }),
          result_ids.end());
    }
  }

  // GEOFILTER (legacy option)
  if (opts.has_geofilter) {
    const auto* gf_fspec = entry.spec.FindField(opts.geofilter_field);
    if (!gf_fspec || gf_fspec->type != FieldType::kGeo) {
      return RedisModule_ReplyWithError(ctx, "ERR GEOFILTER field is not a GEO field");
    }
    const auto* gidx = entry.geo_indices.Get(opts.geofilter_field);
    if (gidx) {
      double radius_m = opts.geofilter_radius * GeoUnitToMeters(opts.geofilter_unit);
      auto geo_hits = gidx->RadiusQuery(opts.geofilter_lon, opts.geofilter_lat, radius_m);
      std::unordered_set<std::string> geo_set;
      for (auto& gr : geo_hits) geo_set.insert(gr.doc_id);
      if (opts.withscores) {
        scored_results.erase(
            std::remove_if(scored_results.begin(), scored_results.end(),
                           [&](const ScoredResult& r) {
                             return geo_set.find(r.doc_id) == geo_set.end();
                           }),
            scored_results.end());
      } else {
        result_ids.erase(
            std::remove_if(result_ids.begin(), result_ids.end(),
                           [&](const std::string& id) {
                             return geo_set.find(id) == geo_set.end();
                           }),
            result_ids.end());
      }
    } else {
      scored_results.clear();
      result_ids.clear();
    }
  }

  if (opts.withscores) {
    // Fast-path LIMIT 0 0: total count only without sorting or field loading
    if (opts.has_limit && opts.limit_offset == 0 && opts.limit_count == 0) {
      RedisModule_ReplyWithArray(ctx, 1);
      RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(scored_results.size()));
      return REDISMODULE_OK;
    }
    // SORTBY overrides score-based ordering; otherwise sort by score desc
    if (opts.has_sortby) {
      const auto* sort_fspec = entry.spec.FindField(opts.sortby_field);
      if (!sort_fspec) {
        return RedisModule_ReplyWithError(ctx, "ERR SORTBY field not in schema");
      }
      bool numeric_sort = (sort_fspec->type == FieldType::kNumeric);
      bool desc = opts.sortby_desc;
      std::sort(scored_results.begin(), scored_results.end(),
                [&](const ScoredResult& a, const ScoredResult& b) {
                  const auto* da = entry.doc_store.Get(a.doc_id);
                  const auto* db = entry.doc_store.Get(b.doc_id);
                  std::string va, vb;
                  if (da) {
                    auto fa = da->fields.find(opts.sortby_field);
                    if (fa != da->fields.end()) va = fa->second;
                  }
                  if (db) {
                    auto fb = db->fields.find(opts.sortby_field);
                    if (fb != db->fields.end()) vb = fb->second;
                  }
                  if (numeric_sort) {
                    double na = 0, nb = 0;
                    TryParseDouble(va, na);
                    TryParseDouble(vb, nb);
                    return desc ? na > nb : na < nb;
                  }
                  return desc ? va > vb : va < vb;
                });
    } else {
      std::sort(scored_results.begin(), scored_results.end(),
                [](const ScoredResult& a, const ScoredResult& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.doc_id < b.doc_id;
                });
    }

    // LIMIT
    if (opts.has_limit) {
      long long total_before = static_cast<long long>(scored_results.size());
      if (opts.limit_offset >= total_before) {
        scored_results.clear();
      } else {
        auto begin = scored_results.begin() + opts.limit_offset;
        auto end = (opts.limit_count >= 0 &&
                    opts.limit_offset + opts.limit_count < total_before)
                       ? scored_results.begin() + opts.limit_offset + opts.limit_count
                       : scored_results.end();
        scored_results = std::vector<ScoredResult>(begin, end);
      }
    }

    long total = static_cast<long>(scored_results.size());
    long multiplier = 1 + (opts.withpayloads ? 1 : 0) + (opts.nocontent ? 0 : 1);
    RedisModule_ReplyWithArray(ctx, 1 + total * multiplier);
    RedisModule_ReplyWithLongLong(ctx, total);

    for (auto& sr : scored_results) {
      RedisModule_ReplyWithCString(ctx, sr.doc_id.c_str());
      const auto* doc = entry.doc_store.Get(sr.doc_id);
      if (opts.withpayloads) {
        if (doc && doc->has_payload) {
          RedisModule_ReplyWithStringBuffer(ctx, doc->payload.data(), doc->payload.size());
        } else {
          RedisModule_ReplyWithNull(ctx);
        }
      }
      if (!opts.nocontent) {
        auto score_str = std::to_string(sr.score);
        ReplyWithDocFields(ctx, doc, opts,
                           &score_str, query_terms, "__search_score");
      }
    }
  } else {
    // Fast-path LIMIT 0 0: total count only without sorting or field loading
    if (opts.has_limit && opts.limit_offset == 0 && opts.limit_count == 0) {
      RedisModule_ReplyWithArray(ctx, 1);
      RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(result_ids.size()));
      return REDISMODULE_OK;
    }
    // SORTBY
    if (opts.has_sortby) {
      const auto* sort_fspec = entry.spec.FindField(opts.sortby_field);
      if (!sort_fspec) {
        return RedisModule_ReplyWithError(ctx, "ERR SORTBY field not in schema");
      }
      bool numeric_sort = (sort_fspec->type == FieldType::kNumeric);
      bool desc = opts.sortby_desc;
      const auto* sort_col = sort_fspec->sortable
                                 ? entry.sort_indices.Get(opts.sortby_field)
                                 : nullptr;
      if (sort_col && numeric_sort) {
        std::vector<std::pair<double, size_t>> keyed;
        keyed.reserve(result_ids.size());
        for (size_t i = 0; i < result_ids.size(); i++) {
          keyed.emplace_back(sort_col->GetNumeric(result_ids[i]), i);
        }
        std::sort(keyed.begin(), keyed.end(),
                  [desc](const auto& a, const auto& b) {
                    return desc ? (a.first > b.first) : (a.first < b.first);
                  });
        std::vector<std::string> sorted;
        sorted.reserve(result_ids.size());
        for (const auto& [k, idx] : keyed) sorted.push_back(std::move(result_ids[idx]));
        result_ids = std::move(sorted);
      } else if (sort_col && !numeric_sort) {
        std::vector<std::pair<std::string_view, size_t>> keyed;
        keyed.reserve(result_ids.size());
        for (size_t i = 0; i < result_ids.size(); i++) {
          keyed.emplace_back(sort_col->GetString(result_ids[i]), i);
        }
        std::sort(keyed.begin(), keyed.end(),
                  [desc](const auto& a, const auto& b) {
                    return desc ? (a.first > b.first) : (a.first < b.first);
                  });
        std::vector<std::string> sorted;
        sorted.reserve(result_ids.size());
        for (const auto& [k, idx] : keyed) sorted.push_back(std::move(result_ids[idx]));
        result_ids = std::move(sorted);
      } else {
        std::sort(result_ids.begin(), result_ids.end(),
                  [&](const std::string& a, const std::string& b) {
                    const auto* da = entry.doc_store.Get(a);
                    const auto* db = entry.doc_store.Get(b);
                    std::string va, vb;
                    if (da) {
                      auto fa = da->fields.find(opts.sortby_field);
                      if (fa != da->fields.end()) va = fa->second;
                    }
                    if (db) {
                      auto fb = db->fields.find(opts.sortby_field);
                      if (fb != db->fields.end()) vb = fb->second;
                    }
                    if (numeric_sort) {
                      double na = 0, nb = 0;
                      TryParseDouble(va, na);
                      TryParseDouble(vb, nb);
                      return desc ? na > nb : na < nb;
                    }
                    return desc ? va > vb : va < vb;
                  });
      }
    }

    // LIMIT
    if (opts.has_limit) {
      long long total_before = static_cast<long long>(result_ids.size());
      if (opts.limit_offset >= total_before) {
        result_ids.clear();
      } else {
        auto begin = result_ids.begin() + opts.limit_offset;
        auto end = (opts.limit_count >= 0 &&
                    opts.limit_offset + opts.limit_count < total_before)
                       ? result_ids.begin() + opts.limit_offset + opts.limit_count
                       : result_ids.end();
        result_ids = std::vector<std::string>(begin, end);
      }
    }

    // Optional $YIELD_DISTANCE_AS from VECTOR_RANGE node
    std::string yield_alias;
    std::unordered_map<std::string, std::string> yield_dists;
    auto FindVecRange = [&](auto& self, const QueryNode& n) -> void {
      if (n.type == QueryNode::Type::kVectorRange && !n.vec_score_alias.empty() &&
          yield_alias.empty()) {
        yield_alias = n.vec_score_alias;
        const auto* fspec = entry.spec.FindField(n.field_name);
        const auto* vidx = entry.vector_indices.Get(n.field_name);
        auto pit = params.find(n.vec_param_name);
        if (fspec && vidx && pit != params.end()) {
          std::vector<float> qvec;
          if (DecodeVectorBlob(pit->second, fspec->vector_params.dim,
                               fspec->vector_params.data_type, qvec)) {
            for (const auto& h : vidx->RangeQuery(qvec.data(), static_cast<float>(n.vec_radius))) {
              yield_dists[h.doc_id] = std::to_string(h.score);
            }
          }
        }
      }
      for (const auto& ch : n.children) self(self, ch);
    };
    FindVecRange(FindVecRange, parsed.root);

    long total = static_cast<long>(result_ids.size());
    bool emit_sortkey = opts.withsortkeys && opts.has_sortby;
    long multiplier = 1 + (emit_sortkey ? 1 : 0) + (opts.withpayloads ? 1 : 0) +
                      (opts.nocontent ? 0 : 1);
    RedisModule_ReplyWithArray(ctx, 1 + total * multiplier);
    RedisModule_ReplyWithLongLong(ctx, total);

    for (auto& rid : result_ids) {
      RedisModule_ReplyWithCString(ctx, rid.c_str());
      const auto* doc = entry.doc_store.Get(rid);
      if (emit_sortkey) {
        std::string sk;
        if (doc) {
          auto sit = doc->fields.find(opts.sortby_field);
          if (sit != doc->fields.end()) sk = sit->second;
        }
        RedisModule_ReplyWithCString(ctx, sk.c_str());
      }
      if (opts.withpayloads) {
        if (doc && doc->has_payload) {
          RedisModule_ReplyWithStringBuffer(ctx, doc->payload.data(), doc->payload.size());
        } else {
          RedisModule_ReplyWithNull(ctx);
        }
      }
      if (!opts.nocontent) {
        const std::string* dptr = nullptr;
        auto dit = yield_dists.find(rid);
        if (!yield_alias.empty() && dit != yield_dists.end()) dptr = &dit->second;
        ReplyWithDocFields(ctx, doc, opts, dptr, query_terms,
                           yield_alias.empty() ? "__vec_score" : yield_alias.c_str());
      }
    }
  }

  return REDISMODULE_OK;
}

// Query tree serializer for EXPLAIN
static std::string ExplainNode(const QueryNode& node, int indent) {
  std::string pad(static_cast<size_t>(indent * 2), ' ');
  std::string result;

  switch (node.type) {
    case QueryNode::Type::kMatchAll:
      result = pad + "*\n";
      break;
    case QueryNode::Type::kTagMatch:
      result = pad + "TAG @" + node.field_name + ":{";
      for (size_t i = 0; i < node.tag_values.size(); i++) {
        if (i > 0) result += " | ";
        result += node.tag_values[i];
      }
      result += "}\n";
      break;
    case QueryNode::Type::kNumericRange:
      result = pad + "NUMERIC @" + node.field_name + ":[" +
               (node.min_exclusive ? "(" : "") + std::to_string(node.range_min) +
               " " +
               (node.max_exclusive ? "(" : "") + std::to_string(node.range_max) +
               "]\n";
      break;
    case QueryNode::Type::kTextMatch:
      result = pad + "TEXT";
      if (!node.field_name.empty()) result += " @" + node.field_name;
      if (node.is_phrase) result += " PHRASE";
      result += ":";
      for (size_t i = 0; i < node.text_terms.size(); i++) {
        result += " " + node.text_terms[i];
      }
      result += "\n";
      break;
    case QueryNode::Type::kGeoFilter:
      result = pad + "GEO @" + node.field_name + ":[" +
               std::to_string(node.geo_lon) + " " +
               std::to_string(node.geo_lat) + " " +
               std::to_string(node.geo_radius) + "]\n";
      break;
    case QueryNode::Type::kGeoShapeQuery: {
      const char* op_name = "WITHIN";
      if (node.geoshape_op == GeoShapeOp::kContains) op_name = "CONTAINS";
      else if (node.geoshape_op == GeoShapeOp::kIntersects) op_name = "INTERSECTS";
      else if (node.geoshape_op == GeoShapeOp::kDisjoint) op_name = "DISJOINT";
      result = pad + "GEOSHAPE @" + node.field_name + ":[" +
               op_name + " $" + node.geoshape_param_name + "]\n";
      break;
    }
    case QueryNode::Type::kVectorRange:
      result = pad + "VECTOR_RANGE @" + node.field_name + ":[" +
               std::to_string(node.vec_radius) + " $" + node.vec_param_name + "]\n";
      break;
    case QueryNode::Type::kIsMissing:
      result = pad + "ISMISSING @" + node.field_name + "\n";
      break;
    case QueryNode::Type::kIsEmpty:
      result = pad + "ISEMPTY @" + node.field_name + "\n";
      break;
    case QueryNode::Type::kAnd:
      result = pad + "AND\n";
      for (auto& c : node.children) result += ExplainNode(c, indent + 1);
      break;
    case QueryNode::Type::kOr:
      result = pad + "OR\n";
      for (auto& c : node.children) result += ExplainNode(c, indent + 1);
      break;
    case QueryNode::Type::kNot:
      result = pad + "NOT\n";
      for (auto& c : node.children) result += ExplainNode(c, indent + 1);
      break;
    case QueryNode::Type::kOptional:
      result = pad + "OPTIONAL\n";
      for (auto& c : node.children) result += ExplainNode(c, indent + 1);
      break;
  }
  return result;
}

// FT.EXPLAIN <index> <query>
int FtExplainCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                            int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  if (GetIndices().find(idx_str) == GetIndices().end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }

  ParsedQuery parsed;
  std::string parse_error;
  if (!ParseQuery(std::string(ArgView(argv[2])), parsed, parse_error)) {
    return RedisModule_ReplyWithError(ctx, parse_error.c_str());
  }

  std::string plan = ExplainNode(parsed.root, 0);
  if (parsed.has_knn) {
    plan += "KNN k=" + std::to_string(parsed.knn_k) +
            " @" + parsed.knn_field +
            " $" + parsed.knn_param_name + "\n";
  }

  // Remove trailing newline
  while (!plan.empty() && plan.back() == '\n') plan.pop_back();

  return RedisModule_ReplyWithCString(ctx, plan.c_str());
}

// FT.EXPLAINCLI <index> <query>
int FtExplainCliCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                               int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  if (GetIndices().find(idx_str) == GetIndices().end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }

  ParsedQuery parsed;
  std::string parse_error;
  if (!ParseQuery(std::string(ArgView(argv[2])), parsed, parse_error)) {
    return RedisModule_ReplyWithError(ctx, parse_error.c_str());
  }

  std::string plan = ExplainNode(parsed.root, 0);
  if (parsed.has_knn) {
    plan += "KNN k=" + std::to_string(parsed.knn_k) +
            " @" + parsed.knn_field +
            " $" + parsed.knn_param_name + "\n";
  }

  // Split into lines and reply as array
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < plan.size()) {
    size_t nl = plan.find('\n', pos);
    if (nl == std::string::npos) nl = plan.size();
    if (nl > pos) lines.push_back(plan.substr(pos, nl - pos));
    pos = nl + 1;
  }

  RedisModule_ReplyWithArray(ctx, static_cast<long>(lines.size()));
  for (auto& line : lines) {
    RedisModule_ReplyWithCString(ctx, line.c_str());
  }
  return REDISMODULE_OK;
}


// FT.PROFILE <index> SEARCH|AGGREGATE [LIMITED] [QUERY] <query> [options...]
int FtProfileCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                            int argc) {
  if (argc < 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  auto it = GetIndices().find(idx_str);
  if (it == GetIndices().end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  auto& entry = it->second;
  TouchIndex(entry);

  auto mode = ArgView(argv[2]);
  bool is_search = MatchArg(mode, "SEARCH");
  bool is_aggregate = MatchArg(mode, "AGGREGATE");
  if (!is_search && !is_aggregate) {
    return RedisModule_ReplyWithError(ctx, "ERR FT.PROFILE requires SEARCH or AGGREGATE");
  }

  int qidx = 3;
  if (qidx < argc && MatchArg(ArgView(argv[qidx]), "LIMITED")) qidx++;
  if (qidx < argc && MatchArg(ArgView(argv[qidx]), "QUERY")) qidx++;
  if (qidx >= argc) return RedisModule_WrongArity(ctx);
  std::string query_str(ArgView(argv[qidx]));
  int opt_start = qidx + 1;

  // Parse optional SEARCH modifiers (DIALECT, LIMIT, SORTBY, VERBATIM, TIMEOUT)
  QueryOptions qopts;
  qopts.language = entry.spec.language;
  qopts.dialect = GetDefaultDialect();
  long long limit_offset = 0;
  long long limit_count = -1;
  std::string sortby_field;
  bool sortby_desc = false;
  if (is_search) {
    for (int oi = opt_start; oi < argc; ) {
      auto kw = ArgView(argv[oi]);
      if (MatchArg(kw, "DIALECT") && oi + 1 < argc) {
        qopts.dialect = static_cast<int>(std::strtol(std::string(ArgView(argv[oi + 1])).c_str(), nullptr, 10));
        oi += 2;
      } else if (MatchArg(kw, "VERBATIM")) {
        qopts.stem = false;
        oi++;
      } else if (MatchArg(kw, "LIMIT") && oi + 2 < argc) {
        limit_offset = std::strtoll(std::string(ArgView(argv[oi + 1])).c_str(), nullptr, 10);
        limit_count = std::strtoll(std::string(ArgView(argv[oi + 2])).c_str(), nullptr, 10);
        oi += 3;
      } else if (MatchArg(kw, "SORTBY") && oi + 1 < argc) {
        sortby_field = std::string(ArgView(argv[oi + 1]));
        oi += 2;
        if (oi < argc && MatchArg(ArgView(argv[oi]), "DESC")) {
          sortby_desc = true;
          oi++;
        } else if (oi < argc && MatchArg(ArgView(argv[oi]), "ASC")) {
          oi++;
        }
      } else if (MatchArg(kw, "TIMEOUT") && oi + 1 < argc) {
        long long tms = std::strtoll(std::string(ArgView(argv[oi + 1])).c_str(), nullptr, 10);
        if (tms > 0) qopts.deadline_ms = NowMs() + tms;
        oi += 2;
      } else {
        oi++;
      }
    }
  }

  struct timespec t_start, t_parse, t_eval, t_pipe, t_end;
  clock_gettime(CLOCK_MONOTONIC, &t_start);

  ParsedQuery parsed;
  std::string parse_error;
  if (!ParseQuery(query_str, parsed, parse_error, qopts)) {
    return RedisModule_ReplyWithError(ctx, parse_error.c_str());
  }
  clock_gettime(CLOCK_MONOTONIC, &t_parse);

  std::string eval_error;
  auto result_ids = EvaluateQuery(
      parsed.root, entry.spec, entry.doc_store,
      entry.tag_indices, entry.numeric_indices,
      entry.text_indices, entry.geo_indices, eval_error,
      qopts, &entry.geoshape_indices);
  if (!eval_error.empty()) {
    return RedisModule_ReplyWithError(ctx, eval_error.c_str());
  }
  clock_gettime(CLOCK_MONOTONIC, &t_eval);

  auto ElapsedUs = [](const struct timespec& a, const struct timespec& b) -> double {
    return static_cast<double>((b.tv_sec - a.tv_sec) * 1000000LL +
                               (b.tv_nsec - a.tv_nsec) / 1000LL);
  };

  if (is_aggregate) {
    std::vector<std::unordered_map<std::string, std::string>> rows;
    std::string agg_error;
    if (!RunAggregateForProfile(entry, result_ids, argv, opt_start, argc,
                                rows, agg_error, qopts.deadline_ms)) {
      return RedisModule_ReplyWithError(ctx, agg_error.c_str());
    }
    clock_gettime(CLOCK_MONOTONIC, &t_pipe);
    clock_gettime(CLOCK_MONOTONIC, &t_end);

    double parse_us = ElapsedUs(t_start, t_parse);
    double eval_us = ElapsedUs(t_parse, t_eval);
    double pipe_us = ElapsedUs(t_eval, t_pipe);
    double total_us = ElapsedUs(t_start, t_end);

    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithArray(ctx, static_cast<long>(rows.size()));
    for (const auto& row : rows) {
      std::vector<std::string> keys;
      keys.reserve(row.size());
      for (const auto& [k, v] : row) keys.push_back(k);
      std::sort(keys.begin(), keys.end());
      RedisModule_ReplyWithArray(ctx, static_cast<long>(keys.size()) * 2);
      for (const auto& k : keys) {
        RedisModule_ReplyWithCString(ctx, k.c_str());
        RedisModule_ReplyWithCString(ctx, row.at(k).c_str());
      }
    }

    RedisModule_ReplyWithArray(ctx, 12);
    RedisModule_ReplyWithSimpleString(ctx, "parse_time_us");
    RedisModule_ReplyWithCString(ctx, std::to_string(parse_us).c_str());
    RedisModule_ReplyWithSimpleString(ctx, "eval_time_us");
    RedisModule_ReplyWithCString(ctx, std::to_string(eval_us).c_str());
    RedisModule_ReplyWithSimpleString(ctx, "pipeline_time_us");
    RedisModule_ReplyWithCString(ctx, std::to_string(pipe_us).c_str());
    RedisModule_ReplyWithSimpleString(ctx, "total_time_us");
    RedisModule_ReplyWithCString(ctx, std::to_string(total_us).c_str());
    RedisModule_ReplyWithSimpleString(ctx, "total_docs");
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(entry.doc_store.Size()));
    RedisModule_ReplyWithSimpleString(ctx, "result_count");
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(rows.size()));
    return REDISMODULE_OK;
  }

  // Optional SORTBY / LIMIT for SEARCH profile
  if (!sortby_field.empty()) {
    std::sort(result_ids.begin(), result_ids.end(),
              [&](const std::string& a, const std::string& b) {
                const auto* da = entry.doc_store.Get(a);
                const auto* db = entry.doc_store.Get(b);
                std::string va = (da && da->fields.count(sortby_field)) ? da->fields.at(sortby_field) : "";
                std::string vb = (db && db->fields.count(sortby_field)) ? db->fields.at(sortby_field) : "";
                double na = 0, nb = 0;
                if (TryParseDouble(va, na) && TryParseDouble(vb, nb)) {
                  return sortby_desc ? (na > nb) : (na < nb);
                }
                return sortby_desc ? (va > vb) : (va < vb);
              });
  }
  if (limit_count >= 0) {
    if (limit_offset >= static_cast<long long>(result_ids.size())) {
      result_ids.clear();
    } else {
      size_t start = static_cast<size_t>(std::max<long long>(0, limit_offset));
      size_t end = std::min(result_ids.size(), start + static_cast<size_t>(limit_count));
      result_ids.assign(result_ids.begin() + static_cast<ptrdiff_t>(start),
                        result_ids.begin() + static_cast<ptrdiff_t>(end));
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &t_end);

  double parse_us = ElapsedUs(t_start, t_parse);
  double eval_us = ElapsedUs(t_parse, t_eval);
  double total_us = ElapsedUs(t_start, t_end);

  // Reply: [results, profile_info]
  RedisModule_ReplyWithArray(ctx, 2);

  // Results
  RedisModule_ReplyWithArray(ctx, static_cast<long>(result_ids.size()));
  for (auto& rid : result_ids) {
    RedisModule_ReplyWithCString(ctx, rid.c_str());
  }

  // Profile info
  RedisModule_ReplyWithArray(ctx, 10);
  RedisModule_ReplyWithSimpleString(ctx, "parse_time_us");
  RedisModule_ReplyWithCString(ctx, std::to_string(parse_us).c_str());
  RedisModule_ReplyWithSimpleString(ctx, "eval_time_us");
  RedisModule_ReplyWithCString(ctx, std::to_string(eval_us).c_str());
  RedisModule_ReplyWithSimpleString(ctx, "total_time_us");
  RedisModule_ReplyWithCString(ctx, std::to_string(total_us).c_str());
  RedisModule_ReplyWithSimpleString(ctx, "total_docs");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(entry.doc_store.Size()));
  RedisModule_ReplyWithSimpleString(ctx, "result_count");
  RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(result_ids.size()));

  return REDISMODULE_OK;
}

// FT.HYBRID <index>
//   SEARCH <text_query> [SCORER <scorer>] [TEXT_LIMIT <n>]
//   VSIM @<vec_field> $<param> [KNN <k>] [EF_RUNTIME <ef>] [FILTER <vec_filter_query>]
//   [COMBINE RRF [CONSTANT <k>] [TEXT_WEIGHT <w>] [VECTOR_WEIGHT <w>]
//          | LINEAR [ALPHA <alpha>]]
//   [PARAMS <count> <name> <val> ...]
//   [RETURN <count> <field> [AS <alias>] ...]
//   [LIMIT <offset> <num>]
//   [WITHSCORES] [EXPLAINSCORE] [NOCONTENT] [WITHPAYLOADS]
//   [TIMEOUT <ms>] [DIALECT <d>]
int FtHybridCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc < 5) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  auto it = GetIndices().find(idx_str);
  if (it == GetIndices().end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  auto& entry = it->second;

  bool has_search = false;
  std::string text_query;
  long long text_limit = -1;

  bool has_vsim = false;
  std::string vec_field;
  std::string vec_param;
  size_t knn_k = 10;
  size_t ef_runtime = 0;
  std::string vec_filter_query;

  HybridConfig hcfg;
  SearchOptions opts;
  std::unordered_map<std::string, std::string> params;

  int i = 2;
  while (i < argc) {
    auto kw = ArgView(argv[i]);
    if (MatchArg(kw, "SEARCH")) {
      if (i + 1 >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR SEARCH requires a query");
      }
      has_search = true;
      text_query = std::string(ArgView(argv[i + 1]));
      i += 2;
      while (i < argc) {
        auto sub = ArgView(argv[i]);
        if (MatchArg(sub, "SCORER")) {
          if (i + 1 >= argc) {
            return RedisModule_ReplyWithError(ctx, "ERR SCORER requires a name");
          }
          auto sname = ArgView(argv[i + 1]);
          if (MatchArg(sname, "BM25") || MatchArg(sname, "BM25STD")) {
            opts.scorer = ScorerType::kBm25;
            opts.scorer_name = "BM25";
          } else if (MatchArg(sname, "TFIDF")) {
            opts.scorer = ScorerType::kTfIdf;
            opts.scorer_name = "TFIDF";
          } else if (MatchArg(sname, "TFIDF.DOCNORM")) {
            opts.scorer = ScorerType::kTfIdfDocNorm;
            opts.scorer_name = "TFIDF.DOCNORM";
          } else if (MatchArg(sname, "DISMAX")) {
            opts.scorer = ScorerType::kDismax;
            opts.scorer_name = "DISMAX";
          } else if (MatchArg(sname, "DOCSCORE")) {
            opts.scorer = ScorerType::kDocScore;
            opts.scorer_name = "DOCSCORE";
          } else {
            return RedisModule_ReplyWithError(ctx, "ERR unknown SCORER");
          }
          i += 2;
        } else if (MatchArg(sub, "TEXT_LIMIT")) {
          if (i + 1 >= argc) {
            return RedisModule_ReplyWithError(ctx, "ERR TEXT_LIMIT requires a value");
          }
          char* ep = nullptr;
          long long tl = std::strtoll(std::string(ArgView(argv[i + 1])).c_str(), &ep, 10);
          if (*ep != '\0' || tl <= 0) {
            return RedisModule_ReplyWithError(ctx, "ERR TEXT_LIMIT must be a positive integer");
          }
          text_limit = tl;
          i += 2;
        } else {
          break;
        }
      }
    } else if (MatchArg(kw, "VSIM")) {
      if (i + 2 >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR VSIM requires @field $param");
      }
      std::string f(ArgView(argv[i + 1]));
      std::string p(ArgView(argv[i + 2]));
      if (!f.empty() && f[0] == '@') f = f.substr(1);
      if (f.empty() || p.size() < 2 || p[0] != '$') {
        return RedisModule_ReplyWithError(ctx, "ERR VSIM requires @field and $param");
      }
      has_vsim = true;
      vec_field = std::move(f);
      vec_param = p.substr(1);
      i += 3;
      while (i < argc) {
        auto sub = ArgView(argv[i]);
        if (MatchArg(sub, "KNN")) {
          if (i + 1 >= argc) return RedisModule_ReplyWithError(ctx, "ERR KNN requires k");
          char* ep = nullptr;
          long k = std::strtol(std::string(ArgView(argv[i + 1])).c_str(), &ep, 10);
          if (*ep != '\0' || k <= 0) {
            return RedisModule_ReplyWithError(ctx, "ERR KNN k must be a positive integer");
          }
          knn_k = static_cast<size_t>(k);
          i += 2;
        } else if (MatchArg(sub, "EF_RUNTIME")) {
          if (i + 1 >= argc) {
            return RedisModule_ReplyWithError(ctx, "ERR EF_RUNTIME requires a value");
          }
          char* ep = nullptr;
          long ef = std::strtol(std::string(ArgView(argv[i + 1])).c_str(), &ep, 10);
          if (*ep != '\0' || ef <= 0) {
            return RedisModule_ReplyWithError(ctx, "ERR EF_RUNTIME must be a positive integer");
          }
          ef_runtime = static_cast<size_t>(ef);
          i += 2;
        } else if (MatchArg(sub, "FILTER")) {
          if (i + 1 >= argc) {
            return RedisModule_ReplyWithError(ctx, "ERR VSIM FILTER requires a query");
          }
          vec_filter_query = std::string(ArgView(argv[i + 1]));
          i += 2;
        } else {
          break;
        }
      }
    } else if (MatchArg(kw, "COMBINE")) {
      if (i + 1 >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR COMBINE requires RRF or LINEAR");
      }
      auto method_s = ArgView(argv[i + 1]);
      i += 2;
      if (MatchArg(method_s, "RRF")) {
        hcfg.method = FusionMethod::kRrf;
        while (i < argc) {
          auto sub = ArgView(argv[i]);
          if (MatchArg(sub, "CONSTANT")) {
            if (i + 1 >= argc) {
              return RedisModule_ReplyWithError(ctx, "ERR CONSTANT requires a value");
            }
            char* ep = nullptr;
            double v = std::strtod(std::string(ArgView(argv[i + 1])).c_str(), &ep);
            if (*ep != '\0' || !std::isfinite(v) || v <= 0.0) {
              return RedisModule_ReplyWithError(ctx, "ERR CONSTANT must be a positive number");
            }
            hcfg.rrf_k = v;
            i += 2;
          } else if (MatchArg(sub, "TEXT_WEIGHT")) {
            if (i + 1 >= argc) {
              return RedisModule_ReplyWithError(ctx, "ERR TEXT_WEIGHT requires a value");
            }
            char* ep = nullptr;
            double v = std::strtod(std::string(ArgView(argv[i + 1])).c_str(), &ep);
            if (*ep != '\0' || !std::isfinite(v) || v < 0.0) {
              return RedisModule_ReplyWithError(ctx, "ERR TEXT_WEIGHT must be non-negative");
            }
            hcfg.text_weight = v;
            i += 2;
          } else if (MatchArg(sub, "VECTOR_WEIGHT")) {
            if (i + 1 >= argc) {
              return RedisModule_ReplyWithError(ctx, "ERR VECTOR_WEIGHT requires a value");
            }
            char* ep = nullptr;
            double v = std::strtod(std::string(ArgView(argv[i + 1])).c_str(), &ep);
            if (*ep != '\0' || !std::isfinite(v) || v < 0.0) {
              return RedisModule_ReplyWithError(ctx, "ERR VECTOR_WEIGHT must be non-negative");
            }
            hcfg.vector_weight = v;
            i += 2;
          } else {
            break;
          }
        }
      } else if (MatchArg(method_s, "LINEAR")) {
        hcfg.method = FusionMethod::kLinear;
        if (i < argc && MatchArg(ArgView(argv[i]), "ALPHA")) {
          if (i + 1 >= argc) {
            return RedisModule_ReplyWithError(ctx, "ERR ALPHA requires a value");
          }
          char* ep = nullptr;
          double a = std::strtod(std::string(ArgView(argv[i + 1])).c_str(), &ep);
          if (*ep != '\0' || !std::isfinite(a) || a < 0.0 || a > 1.0) {
            return RedisModule_ReplyWithError(ctx, "ERR ALPHA must be in [0, 1]");
          }
          hcfg.alpha = a;
          i += 2;
        }
      } else {
        return RedisModule_ReplyWithError(ctx, "ERR COMBINE expects RRF or LINEAR");
      }
    } else if (MatchArg(kw, "PARAMS")) {
      i++;
      if (i >= argc) return RedisModule_ReplyWithError(ctx, "ERR PARAMS requires count");
      char* ep = nullptr;
      long pcount = std::strtol(std::string(ArgView(argv[i++])).c_str(), &ep, 10);
      if (*ep != '\0' || pcount < 0 || pcount % 2 != 0) {
        return RedisModule_ReplyWithError(ctx, "ERR PARAMS count must be a non-negative even integer");
      }
      for (long p = 0; p < pcount / 2; p++) {
        if (i + 1 >= argc) {
          return RedisModule_ReplyWithError(ctx, "ERR not enough PARAMS values");
        }
        std::string pname(ArgView(argv[i]));
        std::string pval = ArgStr(argv[i + 1]);
        params[std::move(pname)] = std::move(pval);
        i += 2;
      }
    } else if (MatchArg(kw, "RETURN")) {
      i++;
      if (i >= argc) return RedisModule_ReplyWithError(ctx, "ERR RETURN requires count");
      char* ep = nullptr;
      long rcount = std::strtol(std::string(ArgView(argv[i++])).c_str(), &ep, 10);
      if (*ep != '\0' || rcount < 0) {
        return RedisModule_ReplyWithError(ctx, "ERR RETURN count must be a non-negative integer");
      }
      opts.has_return = true;
      for (long r = 0; r < rcount; ) {
        if (i >= argc) return RedisModule_ReplyWithError(ctx, "ERR not enough RETURN fields");
        std::string fname(ArgView(argv[i++]));
        r++;
        if (r + 2 <= rcount && i + 1 < argc && MatchArg(ArgView(argv[i]), "AS")) {
          opts.return_aliases[fname] = std::string(ArgView(argv[i + 1]));
          i += 2;
          r += 2;
        }
        opts.return_fields.push_back(std::move(fname));
      }
    } else if (MatchArg(kw, "LIMIT")) {
      if (i + 2 >= argc) {
        return RedisModule_ReplyWithError(ctx, "ERR LIMIT requires offset and count");
      }
      char* ep1 = nullptr;
      char* ep2 = nullptr;
      long long off = std::strtoll(std::string(ArgView(argv[i + 1])).c_str(), &ep1, 10);
      long long cnt = std::strtoll(std::string(ArgView(argv[i + 2])).c_str(), &ep2, 10);
      if (*ep1 != '\0' || *ep2 != '\0' || off < 0 || cnt < 0) {
        return RedisModule_ReplyWithError(ctx, "ERR LIMIT offset and count must be non-negative integers");
      }
      opts.limit_offset = off;
      opts.limit_count = cnt;
      opts.has_limit = true;
      i += 3;
    } else if (MatchArg(kw, "WITHSCORES")) {
      opts.withscores = true;
      i++;
    } else if (MatchArg(kw, "EXPLAINSCORE")) {
      opts.explainscore = true;
      i++;
    } else if (MatchArg(kw, "NOCONTENT")) {
      opts.nocontent = true;
      i++;
    } else if (MatchArg(kw, "WITHPAYLOADS")) {
      opts.withpayloads = true;
      i++;
    } else if (MatchArg(kw, "TIMEOUT")) {
      if (i + 1 >= argc) return RedisModule_ReplyWithError(ctx, "ERR TIMEOUT requires value");
      char* ep = nullptr;
      long long tms = std::strtoll(std::string(ArgView(argv[i + 1])).c_str(), &ep, 10);
      if (*ep != '\0' || tms < 0) {
        return RedisModule_ReplyWithError(ctx, "ERR TIMEOUT must be a non-negative integer");
      }
      opts.timeout_ms = tms;
      opts.has_timeout = true;
      i += 2;
    } else if (MatchArg(kw, "DIALECT")) {
      if (i + 1 >= argc) return RedisModule_ReplyWithError(ctx, "ERR DIALECT requires value");
      char* ep = nullptr;
      opts.dialect = static_cast<int>(std::strtol(std::string(ArgView(argv[i + 1])).c_str(), &ep, 10));
      opts.has_dialect = true;
      i += 2;
    } else {
      return RedisModule_ReplyWithError(ctx, "ERR unknown FT.HYBRID option");
    }
  }

  if (!has_search || !has_vsim) {
    return RedisModule_ReplyWithError(ctx, "ERR FT.HYBRID requires both SEARCH and VSIM clauses");
  }

  TouchIndex(entry);
  QueryOptions qopts;
  qopts.language = entry.spec.language;
  qopts.dialect = opts.has_dialect ? opts.dialect : GetDefaultDialect();
  qopts.scorer = opts.scorer;
  qopts.max_expansions = GetMaxExpansions();
  qopts.min_stem_len = GetMinStemLen();
  if (opts.has_timeout && opts.timeout_ms > 0) {
    qopts.deadline_ms = NowMs() + opts.timeout_ms;
  }

  // 1. Validate and execute Vector branch
  const auto* fspec = entry.spec.FindField(vec_field);
  if (!fspec) return RedisModule_ReplyWithError(ctx, "ERR VSIM field not in schema");
  if (fspec->type != FieldType::kVector) {
    return RedisModule_ReplyWithError(ctx, "ERR VSIM field is not a VECTOR field");
  }
  auto pit = params.find(vec_param);
  if (pit == params.end()) {
    return RedisModule_ReplyWithError(ctx, "ERR VSIM param not found in PARAMS");
  }
  std::vector<float> qvec;
  if (!DecodeVectorBlob(pit->second, fspec->vector_params.dim,
                        fspec->vector_params.data_type, qvec)) {
    return RedisModule_ReplyWithError(ctx, "ERR query vector dimension mismatch");
  }

  std::vector<KnnResult> vec_hits;
  if (const auto* vidx = entry.vector_indices.Get(vec_field)) {
    if (!vec_filter_query.empty()) {
      ParsedQuery fpq;
      std::string ferr;
      if (!ParseQuery(vec_filter_query, fpq, ferr, qopts)) {
        return RedisModule_ReplyWithError(ctx, ferr.c_str());
      }
      auto candidates = EvaluateQuery(
          fpq.root, entry.spec, entry.doc_store, entry.tag_indices,
          entry.numeric_indices, entry.text_indices, entry.geo_indices,
          ferr, qopts, &entry.geoshape_indices, &params, &entry.vector_indices,
          &entry.synonyms);
      if (!ferr.empty()) return RedisModule_ReplyWithError(ctx, ferr.c_str());
      vec_hits = vidx->KnnQueryFiltered(qvec.data(), knn_k, candidates);
    } else {
      vec_hits = vidx->KnnQueryEf(qvec.data(), knn_k, ef_runtime);
    }
  }

  // 2. Execute Text branch
  ParsedQuery tpq;
  std::string terr;
  if (!ParseQuery(text_query, tpq, terr, qopts)) {
    return RedisModule_ReplyWithError(ctx, terr.c_str());
  }
  auto text_hits = EvaluateQueryScored(
      tpq.root, entry.spec, entry.doc_store, entry.tag_indices,
      entry.numeric_indices, entry.text_indices, entry.geo_indices,
      terr, qopts, &entry.geoshape_indices, &params, &entry.vector_indices,
      &entry.synonyms);
  if (!terr.empty()) return RedisModule_ReplyWithError(ctx, terr.c_str());

  if (text_limit > 0 && static_cast<long long>(text_hits.size()) > text_limit) {
    std::sort(text_hits.begin(), text_hits.end(),
              [](const ScoredResult& a, const ScoredResult& b) {
                if (a.score != b.score) return a.score > b.score;
                return a.doc_id < b.doc_id;
              });
    text_hits.resize(static_cast<size_t>(text_limit));
  }

  // 3. Fuse results
  auto fused = FuseHybridResults(text_hits, vec_hits, hcfg);

  // LIMIT 0 0 fast path
  if (opts.has_limit && opts.limit_offset == 0 && opts.limit_count == 0) {
    RedisModule_ReplyWithArray(ctx, 1);
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(fused.size()));
    return REDISMODULE_OK;
  }

  if (opts.has_limit) {
    long long total_before = static_cast<long long>(fused.size());
    if (opts.limit_offset >= total_before) {
      fused.clear();
    } else {
      auto begin = fused.begin() + opts.limit_offset;
      auto end = (opts.limit_count >= 0 &&
                  opts.limit_offset + opts.limit_count < total_before)
                     ? fused.begin() + opts.limit_offset + opts.limit_count
                     : fused.end();
      fused = std::vector<HybridHit>(begin, end);
    }
  }

  // 4. Reply
  std::unordered_set<std::string> empty_terms;
  long total = static_cast<long>(fused.size());
  long multiplier = 1 + (opts.withpayloads ? 1 : 0) + (opts.nocontent ? 0 : 1);
  RedisModule_ReplyWithArray(ctx, 1 + total * multiplier);
  RedisModule_ReplyWithLongLong(ctx, total);

  for (const auto& h : fused) {
    RedisModule_ReplyWithCString(ctx, h.doc_id.c_str());
    const auto* doc = entry.doc_store.Get(h.doc_id);
    if (opts.withpayloads) {
      if (doc && doc->has_payload) {
        RedisModule_ReplyWithStringBuffer(ctx, doc->payload.data(), doc->payload.size());
      } else {
        RedisModule_ReplyWithNull(ctx);
      }
    }
    if (!opts.nocontent) {
      if (opts.withscores) {
        std::string score_str = std::to_string(h.score);
        if (opts.explainscore) {
          SearchOptions tmp_opts = opts;
          tmp_opts.explainscore = false;
          // Emit custom hybrid score + explanation + fields
          std::vector<std::string> keys;
          if (opts.has_return) {
            keys = opts.return_fields;
          } else if (doc) {
            keys.reserve(doc->fields.size());
            for (const auto& [k, v] : doc->fields) keys.push_back(k);
            std::sort(keys.begin(), keys.end());
          }
          std::string expl =
              (hcfg.method == FusionMethod::kRrf)
                  ? ("RRF(k=" + std::to_string(hcfg.rrf_k) +
                     ",text_rank=" + std::to_string(h.text_rank) +
                     ",vec_rank=" + std::to_string(h.vector_rank) + ")")
                  : ("LINEAR(alpha=" + std::to_string(hcfg.alpha) +
                     ",text_score=" + std::to_string(h.text_score) +
                     ",vec_dist=" + std::to_string(h.vector_distance) + ")");
          RedisModule_ReplyWithArray(ctx, static_cast<long>(keys.size()) * 2 + 4);
          RedisModule_ReplyWithCString(ctx, "__hybrid_score");
          RedisModule_ReplyWithCString(ctx, score_str.c_str());
          RedisModule_ReplyWithCString(ctx, "__score_explanation");
          RedisModule_ReplyWithCString(ctx, expl.c_str());
          for (const auto& k : keys) {
            auto ait = opts.return_aliases.find(k);
            const std::string& out_key =
                (ait != opts.return_aliases.end()) ? ait->second : k;
            RedisModule_ReplyWithCString(ctx, out_key.c_str());
            if (doc) {
              auto fit = doc->fields.find(k);
              RedisModule_ReplyWithCString(
                  ctx, (fit != doc->fields.end()) ? fit->second.c_str() : "");
            } else {
              RedisModule_ReplyWithCString(ctx, "");
            }
          }
        } else {
          ReplyWithDocFields(ctx, doc, opts, &score_str, empty_terms, "__hybrid_score");
        }
      } else {
        ReplyWithDocFields(ctx, doc, opts, nullptr, empty_terms);
      }
    }
  }
  return REDISMODULE_OK;
}

