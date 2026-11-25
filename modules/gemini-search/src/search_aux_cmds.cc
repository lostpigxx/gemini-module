#include "search_internal.h"
#include "suggest_dict.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// FT.SUGADD <dict> <string> <score> [INCR] [PAYLOAD <payload>]
int FtSugAddCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
  if (argc < 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string dict_name(ArgView(argv[1]));
  std::string str(ArgView(argv[2]));
  char* ep = nullptr;
  double score = std::strtod(std::string(ArgView(argv[3])).c_str(), &ep);
  if (*ep != '\0') return RedisModule_ReplyWithError(ctx, "ERR invalid score");

  bool incr = false;
  std::string payload;
  for (int i = 4; i < argc; i++) {
    if (MatchArg(ArgView(argv[i]), "INCR")) {
      incr = true;
    } else if (MatchArg(ArgView(argv[i]), "PAYLOAD") && i + 1 < argc) {
      i++;
      payload = std::string(ArgView(argv[i]));
    }
  }

  GetSuggestDicts()[dict_name].Add(str, score, incr, payload);
  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithLongLong(
      ctx, static_cast<long long>(GetSuggestDicts()[dict_name].Len()));
}

// FT.SUGGET <dict> <prefix> [FUZZY] [WITHSCORES] [WITHPAYLOADS] [MAX <n>]
int FtSugGetCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string dict_name(ArgView(argv[1]));
  std::string prefix(ArgView(argv[2]));

  bool fuzzy = false;
  bool withscores = false;
  bool withpayloads = false;
  size_t max_results = 5;

  for (int i = 3; i < argc; i++) {
    if (MatchArg(ArgView(argv[i]), "FUZZY")) fuzzy = true;
    else if (MatchArg(ArgView(argv[i]), "WITHSCORES")) withscores = true;
    else if (MatchArg(ArgView(argv[i]), "WITHPAYLOADS")) withpayloads = true;
    else if (MatchArg(ArgView(argv[i]), "MAX") && i + 1 < argc) {
      i++;
      char* ep = nullptr;
      long v = std::strtol(std::string(ArgView(argv[i])).c_str(), &ep, 10);
      if (*ep == '\0' && v > 0) max_results = static_cast<size_t>(v);
    }
  }

  auto it = GetSuggestDicts().find(dict_name);
  if (it == GetSuggestDicts().end()) {
    RedisModule_ReplyWithArray(ctx, 0);
    return REDISMODULE_OK;
  }

  auto results = it->second.Get(prefix, fuzzy, max_results);
  long multiplier = 1;
  if (withscores) multiplier++;
  if (withpayloads) multiplier++;
  RedisModule_ReplyWithArray(ctx, static_cast<long>(results.size()) * multiplier);
  for (auto& entry : results) {
    RedisModule_ReplyWithCString(ctx, entry.str.c_str());
    if (withscores) RedisModule_ReplyWithCString(ctx, std::to_string(entry.score).c_str());
    if (withpayloads) RedisModule_ReplyWithCString(ctx, entry.payload.c_str());
  }
  return REDISMODULE_OK;
}

// FT.SUGDEL <dict> <string>
int FtSugDelCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
  if (argc != 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string dict_name(ArgView(argv[1]));
  std::string str(ArgView(argv[2]));

  auto it = GetSuggestDicts().find(dict_name);
  if (it == GetSuggestDicts().end()) {
    return RedisModule_ReplyWithLongLong(ctx, 0);
  }
  bool removed = it->second.Del(str);
  if (removed) RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithLongLong(ctx, removed ? 1 : 0);
}

// FT.SUGLEN <dict>
int FtSugLenCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string dict_name(ArgView(argv[1]));
  auto it = GetSuggestDicts().find(dict_name);
  size_t len = (it != GetSuggestDicts().end()) ? it->second.Len() : 0;
  return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(len));
}

// FT.DICTADD <dict> <term> [term ...]
int FtDictAddCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                            int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string dict_name(ArgView(argv[1]));
  std::vector<std::string> terms;
  for (int i = 2; i < argc; i++) terms.emplace_back(ArgView(argv[i]));
  size_t added = GetTermDicts()[dict_name].Add(terms);
  if (added > 0) RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(added));
}

// FT.DICTDEL <dict> <term> [term ...]
int FtDictDelCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                            int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string dict_name(ArgView(argv[1]));
  auto it = GetTermDicts().find(dict_name);
  if (it == GetTermDicts().end()) return RedisModule_ReplyWithLongLong(ctx, 0);
  std::vector<std::string> terms;
  for (int i = 2; i < argc; i++) terms.emplace_back(ArgView(argv[i]));
  size_t removed = it->second.Del(terms);
  if (removed > 0) RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(removed));
}

// FT.DICTDUMP <dict>
int FtDictDumpCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                             int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string dict_name(ArgView(argv[1]));
  auto it = GetTermDicts().find(dict_name);
  if (it == GetTermDicts().end()) {
    RedisModule_ReplyWithArray(ctx, 0);
    return REDISMODULE_OK;
  }
  auto terms = it->second.Dump();
  RedisModule_ReplyWithArray(ctx, static_cast<long>(terms.size()));
  for (auto& t : terms) RedisModule_ReplyWithCString(ctx, t.c_str());
  return REDISMODULE_OK;
}

// FT.SYNUPDATE <index> <group_id> <term> [term ...]
int FtSynUpdateCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                               int argc) {
  if (argc < 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  auto it = GetIndices().find(idx_str);
  if (it == GetIndices().end()) return RedisModule_ReplyWithError(ctx, "ERR index not found");

  std::string group_id(ArgView(argv[2]));
  std::vector<std::string> terms;
  for (int i = 3; i < argc; i++) terms.emplace_back(ArgView(argv[i]));

  it->second.synonyms.Update(group_id, terms);
  RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.SYNDUMP <index>
int FtSynDumpCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                            int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  auto it = GetIndices().find(idx_str);
  if (it == GetIndices().end()) return RedisModule_ReplyWithError(ctx, "ERR index not found");

  auto groups = it->second.synonyms.Dump();
  long total = 0;
  for (auto& [gid, terms] : groups) total += 1 + static_cast<long>(terms.size());
  RedisModule_ReplyWithArray(ctx, total);
  for (auto& [gid, terms] : groups) {
    RedisModule_ReplyWithCString(ctx, gid.c_str());
    for (auto& t : terms) RedisModule_ReplyWithCString(ctx, t.c_str());
  }
  return REDISMODULE_OK;
}

// FT.SPELLCHECK <index> <query> [DISTANCE <d>] [TERMS INCLUDE|EXCLUDE <dict>]
int FtSpellCheckCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                               int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  auto it = GetIndices().find(idx_str);
  if (it == GetIndices().end()) return RedisModule_ReplyWithError(ctx, "ERR index not found");
  auto& entry = it->second;

  std::string query_str(ArgView(argv[2]));
  int max_dist = 1;
  std::vector<std::string> include_dicts;
  std::vector<std::string> exclude_dicts;

  for (int i = 3; i < argc; i++) {
    if (MatchArg(ArgView(argv[i]), "DISTANCE") && i + 1 < argc) {
      i++;
      char* ep = nullptr;
      max_dist = static_cast<int>(std::strtol(std::string(ArgView(argv[i])).c_str(), &ep, 10));
    } else if (MatchArg(ArgView(argv[i]), "TERMS") && i + 2 < argc) {
      i++;
      bool is_include = MatchArg(ArgView(argv[i]), "INCLUDE");
      i++;
      std::string dict_name(ArgView(argv[i]));
      if (is_include) include_dicts.push_back(dict_name);
      else exclude_dicts.push_back(dict_name);
    }
  }

  // Tokenize query
  auto terms = TextIndex::Tokenize(query_str);

  // Collect known terms from index
  std::unordered_set<std::string> index_terms;
  for (auto& f : entry.spec.fields) {
    if (f.type != FieldType::kText) continue;
    const auto* tidx = entry.text_indices.Get(f.name);
    if (!tidx) continue;
    for (auto& term : terms) {
      auto docs = tidx->Lookup(term);
      if (!docs.empty()) index_terms.insert(term);
    }
  }

  // Collect terms from include dicts
  for (auto& dname : include_dicts) {
    auto dit = GetTermDicts().find(dname);
    if (dit != GetTermDicts().end()) {
      for (auto& term : terms) {
        if (dit->second.Contains(term)) index_terms.insert(term);
      }
    }
  }

  // Collect terms to exclude
  std::unordered_set<std::string> excluded;
  for (auto& dname : exclude_dicts) {
    auto dit = GetTermDicts().find(dname);
    if (dit != GetTermDicts().end()) {
      for (auto& term : terms) {
        if (dit->second.Contains(term)) excluded.insert(term);
      }
    }
  }

  // For each misspelled term, find suggestions from index
  struct SpellSuggestion {
    std::string term;
    std::vector<std::pair<double, std::string>> suggestions;
  };
  std::vector<SpellSuggestion> results;

  for (auto& term : terms) {
    if (index_terms.count(term) || excluded.count(term)) continue;

    SpellSuggestion ss;
    ss.term = term;

    for (auto& f : entry.spec.fields) {
      if (f.type != FieldType::kText) continue;
      const auto* tidx = entry.text_indices.Get(f.name);
      if (!tidx) continue;
      auto fuzzy_docs = tidx->FuzzyLookup(term, max_dist);
      (void)fuzzy_docs;
    }

    // Use fuzzy lookup on the text index to find candidate terms
    std::unordered_set<std::string> seen;
    for (auto& f : entry.spec.fields) {
      if (f.type != FieldType::kText) continue;
      const auto* tidx = entry.text_indices.Get(f.name);
      if (!tidx) continue;
      auto candidates = tidx->FuzzyLookup(term, max_dist);
      for (auto& doc_id : candidates) {
        (void)doc_id;
      }
    }

    // Simpler approach: collect all suggestions via StemLookup and FuzzyLookup
    std::unordered_map<std::string, double> suggestion_scores;
    for (auto& f : entry.spec.fields) {
      if (f.type != FieldType::kText) continue;
      const auto* tidx = entry.text_indices.Get(f.name);
      if (!tidx) continue;
      auto stem_results = tidx->StemSearch(term);
      for (auto& r : stem_results) suggestion_scores[r.doc_id] += r.score;
      auto fuzzy_results = tidx->FuzzySearch(term, max_dist);
      for (auto& r : fuzzy_results) suggestion_scores[r.doc_id] += r.score;
    }

    // Include suggestions from include dicts
    for (auto& dname : include_dicts) {
      auto dit = GetTermDicts().find(dname);
      if (dit == GetTermDicts().end()) continue;
      for (auto& dt : dit->second.Dump()) {
        int dist = TextIndex::LevenshteinDistance(term, dt);
        if (dist <= max_dist && dist > 0) {
          suggestion_scores[dt] += 1.0 / static_cast<double>(dist);
        }
      }
    }

    for (auto& [sug, score] : suggestion_scores) {
      if (!excluded.count(sug))
        ss.suggestions.emplace_back(score, sug);
    }
    std::sort(ss.suggestions.begin(), ss.suggestions.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    results.push_back(std::move(ss));
  }

  // Reply: array of [TERM, term, suggestions_array]
  RedisModule_ReplyWithArray(ctx, static_cast<long>(results.size()));
  for (auto& ss : results) {
    long entry_len = 2 + static_cast<long>(ss.suggestions.size());
    RedisModule_ReplyWithArray(ctx, entry_len);
    RedisModule_ReplyWithSimpleString(ctx, "TERM");
    RedisModule_ReplyWithCString(ctx, ss.term.c_str());
    for (auto& [score, sug] : ss.suggestions) {
      RedisModule_ReplyWithCString(ctx, sug.c_str());
    }
  }
  return REDISMODULE_OK;
}

// FT.CONFIG GET|SET <param> [value]
int FtConfigCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto sub = ArgView(argv[1]);
  std::string param(ArgView(argv[2]));
  for (auto& c : param) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

  if (MatchArg(sub, "GET")) {
    if (param == "*") {
      RedisModule_ReplyWithArray(ctx, static_cast<long>(GetConfig().size()) * 2);
      for (auto& [k, v] : GetConfig()) {
        RedisModule_ReplyWithCString(ctx, k.c_str());
        RedisModule_ReplyWithCString(ctx, v.c_str());
      }
      return REDISMODULE_OK;
    }
    auto it = GetConfig().find(param);
    if (it == GetConfig().end()) {
      return RedisModule_ReplyWithError(ctx, "ERR unknown config parameter");
    }
    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithCString(ctx, it->first.c_str());
    RedisModule_ReplyWithCString(ctx, it->second.c_str());
    return REDISMODULE_OK;
  }

  if (MatchArg(sub, "SET")) {
    if (argc < 4) return RedisModule_WrongArity(ctx);
    auto it = GetConfig().find(param);
    if (it == GetConfig().end()) {
      return RedisModule_ReplyWithError(ctx, "ERR unknown config parameter");
    }
    std::string val(ArgView(argv[3]));
    if (param == "ON_TIMEOUT") {
      if (MatchArg(val, "FAIL")) val = "FAIL";
      else if (MatchArg(val, "RETURN")) val = "RETURN";
      else return RedisModule_ReplyWithError(ctx, "ERR ON_TIMEOUT must be FAIL or RETURN");
    } else if (param == "MAXSEARCHRESULTS" || param == "ASYNC_MIN_DOCS") {
      char* ep = nullptr;
      long long v = std::strtoll(val.c_str(), &ep, 10);
      if (*ep != '\0' || v < 0) {
        return RedisModule_ReplyWithError(ctx, "ERR parameter must be a non-negative integer");
      }
    }
    it->second = std::move(val);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
  }

  return RedisModule_ReplyWithError(ctx, "ERR FT.CONFIG requires GET or SET");
}


// FT._DEBUG
int FtDebugCommand(RedisModuleCtx* ctx, RedisModuleString** /*argv*/,
                          int /*argc*/) {
  return RedisModule_ReplyWithSimpleString(ctx, "GeminiSearch OK");
}

