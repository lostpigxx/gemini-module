#include "cluster_coordinator.h"
#include "cluster_topology.h"
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

bool ApplyLocalSuggestCommand(const std::string& cmd_name,
                              const std::vector<std::string>& args,
                              bool& out_mutated,
                              gemini_search::RpcSuggestResponse& resp) {
  out_mutated = false;
  resp = gemini_search::RpcSuggestResponse{};

  if (MatchArg(cmd_name, "FT.SUGADD")) {
    if (args.size() < 3) {
      resp.error_msg = "ERR wrong number of arguments for 'FT.SUGADD' command";
      return false;
    }
    char* ep = nullptr;
    double score = std::strtod(args[2].c_str(), &ep);
    if (args[2].empty() || *ep != '\0' || !std::isfinite(score)) {
      resp.error_msg = "ERR invalid score";
      return false;
    }
    bool incr = false;
    std::string payload;
    for (size_t i = 3; i < args.size(); i++) {
      if (MatchArg(args[i], "INCR")) {
        incr = true;
      } else if (MatchArg(args[i], "PAYLOAD") && i + 1 < args.size()) {
        i++;
        payload = args[i];
      }
    }
    std::unique_lock<std::shared_mutex> lk(GetSuggestDictsMutex());
    auto& dict = GetSuggestDicts()[args[0]];
    dict.Add(args[1], score, incr, payload);
    out_mutated = true;
    resp.ok = true;
    resp.int_reply = static_cast<int64_t>(dict.Len());
    return true;
  }

  if (MatchArg(cmd_name, "FT.SUGDEL")) {
    if (args.size() != 2) {
      resp.error_msg = "ERR wrong number of arguments for 'FT.SUGDEL' command";
      return false;
    }
    std::unique_lock<std::shared_mutex> lk(GetSuggestDictsMutex());
    auto it = GetSuggestDicts().find(args[0]);
    bool removed = (it != GetSuggestDicts().end()) ? it->second.Del(args[1]) : false;
    out_mutated = removed;
    resp.ok = true;
    resp.int_reply = removed ? 1 : 0;
    return true;
  }

  if (MatchArg(cmd_name, "FT.SUGLEN")) {
    if (args.size() != 1) {
      resp.error_msg = "ERR wrong number of arguments for 'FT.SUGLEN' command";
      return false;
    }
    std::shared_lock<std::shared_mutex> lk(GetSuggestDictsMutex());
    auto it = GetSuggestDicts().find(args[0]);
    resp.ok = true;
    resp.int_reply = static_cast<int64_t>((it != GetSuggestDicts().end()) ? it->second.Len() : 0);
    return true;
  }

  if (MatchArg(cmd_name, "FT.SUGGET")) {
    if (args.size() < 2) {
      resp.error_msg = "ERR wrong number of arguments for 'FT.SUGGET' command";
      return false;
    }
    bool fuzzy = false;
    size_t max_results = 5;
    for (size_t i = 2; i < args.size(); i++) {
      if (MatchArg(args[i], "FUZZY")) {
        fuzzy = true;
      } else if (MatchArg(args[i], "MAX") && i + 1 < args.size()) {
        i++;
        char* ep = nullptr;
        long v = std::strtol(args[i].c_str(), &ep, 10);
        if (*ep == '\0' && v > 0) max_results = static_cast<size_t>(v);
      }
    }
    std::shared_lock<std::shared_mutex> lk(GetSuggestDictsMutex());
    auto it = GetSuggestDicts().find(args[0]);
    resp.ok = true;
    if (it != GetSuggestDicts().end()) {
      auto results = it->second.Get(args[1], fuzzy, max_results);
      resp.entries.reserve(results.size());
      for (auto& e : results) {
        resp.entries.push_back({std::move(e.str), e.score, std::move(e.payload)});
      }
    }
    return true;
  }

  resp.error_msg = "ERR unsupported suggest command: " + cmd_name;
  return false;
}

// FT.SUGADD <dict> <string> <score> [INCR] [PAYLOAD <payload>]
int FtSugAddCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
  if (argc < 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string dict_name(ArgView(argv[1]));
  std::string score_str(ArgView(argv[3]));
  char* ep = nullptr;
  double score_check = std::strtod(score_str.c_str(), &ep);
  if (score_str.empty() || *ep != '\0' || !std::isfinite(score_check)) {
    return RedisModule_ReplyWithError(ctx, "ERR invalid score");
  }

  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc - 1));
  for (int i = 1; i < argc; i++) args.emplace_back(ArgView(argv[i]));

  int ctx_flags = RedisModule_GetContextFlags ? RedisModule_GetContextFlags(ctx) : 0;
  bool is_replicated = (ctx_flags & REDISMODULE_CTX_FLAGS_REPLICATED) != 0;
  if (!is_replicated && gemini_search::ClusterManager::Instance().IsClusterScatterEnabled()) {
    gemini_search::ClusterSuggestPlan splan;
    splan.cmd_name = "FT.SUGADD";
    splan.args = std::move(args);
    splan.dict_name = dict_name;
    splan.is_write = true;
    return gemini_search::ExecuteClusterSuggest(ctx, std::move(splan));
  }

  bool mutated = false;
  gemini_search::RpcSuggestResponse resp;
  if (!ApplyLocalSuggestCommand("FT.SUGADD", args, mutated, resp)) {
    return RedisModule_ReplyWithError(ctx, resp.error_msg.c_str());
  }
  if (mutated) RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(resp.int_reply));
}

// FT.SUGGET <dict> <prefix> [FUZZY] [WITHSCORES] [WITHPAYLOADS] [MAX <n>]
int FtSugGetCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string dict_name(ArgView(argv[1]));
  bool withscores = false;
  bool withpayloads = false;
  for (int i = 3; i < argc; i++) {
    if (MatchArg(ArgView(argv[i]), "WITHSCORES")) withscores = true;
    else if (MatchArg(ArgView(argv[i]), "WITHPAYLOADS")) withpayloads = true;
  }

  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc - 1));
  for (int i = 1; i < argc; i++) args.emplace_back(ArgView(argv[i]));

  if (gemini_search::ClusterManager::Instance().IsClusterScatterEnabled()) {
    gemini_search::ClusterSuggestPlan splan;
    splan.cmd_name = "FT.SUGGET";
    splan.args = std::move(args);
    splan.dict_name = dict_name;
    splan.is_write = false;
    splan.sugget_withscores = withscores;
    splan.sugget_withpayloads = withpayloads;
    return gemini_search::ExecuteClusterSuggest(ctx, std::move(splan));
  }

  bool mutated = false;
  gemini_search::RpcSuggestResponse resp;
  if (!ApplyLocalSuggestCommand("FT.SUGGET", args, mutated, resp)) {
    return RedisModule_ReplyWithError(ctx, resp.error_msg.c_str());
  }

  long multiplier = 1 + (withscores ? 1 : 0) + (withpayloads ? 1 : 0);
  RedisModule_ReplyWithArray(ctx, static_cast<long>(resp.entries.size()) * multiplier);
  for (const auto& entry : resp.entries) {
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
  std::vector<std::string> args = {dict_name, ArgStr(argv[2])};

  int ctx_flags = RedisModule_GetContextFlags ? RedisModule_GetContextFlags(ctx) : 0;
  bool is_replicated = (ctx_flags & REDISMODULE_CTX_FLAGS_REPLICATED) != 0;
  if (!is_replicated && gemini_search::ClusterManager::Instance().IsClusterScatterEnabled()) {
    gemini_search::ClusterSuggestPlan splan;
    splan.cmd_name = "FT.SUGDEL";
    splan.args = std::move(args);
    splan.dict_name = dict_name;
    splan.is_write = true;
    return gemini_search::ExecuteClusterSuggest(ctx, std::move(splan));
  }

  bool mutated = false;
  gemini_search::RpcSuggestResponse resp;
  if (!ApplyLocalSuggestCommand("FT.SUGDEL", args, mutated, resp)) {
    return RedisModule_ReplyWithError(ctx, resp.error_msg.c_str());
  }
  if (mutated) RedisModule_ReplicateVerbatim(ctx);
  return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(resp.int_reply));
}

// FT.SUGLEN <dict>
int FtSugLenCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string dict_name(ArgView(argv[1]));
  std::vector<std::string> args = {dict_name};

  if (gemini_search::ClusterManager::Instance().IsClusterScatterEnabled()) {
    gemini_search::ClusterSuggestPlan splan;
    splan.cmd_name = "FT.SUGLEN";
    splan.args = std::move(args);
    splan.dict_name = dict_name;
    splan.is_write = false;
    return gemini_search::ExecuteClusterSuggest(ctx, std::move(splan));
  }

  bool mutated = false;
  gemini_search::RpcSuggestResponse resp;
  if (!ApplyLocalSuggestCommand("FT.SUGLEN", args, mutated, resp)) {
    return RedisModule_ReplyWithError(ctx, resp.error_msg.c_str());
  }
  return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(resp.int_reply));
}

bool ApplyLocalDictAdd(const std::vector<std::string>& args, int64_t& out_added,
                       std::string& err_out) {
  if (args.size() < 2) {
    err_out = "ERR wrong number of arguments for 'FT.DICTADD' command";
    return false;
  }
  const std::string& dict_name = args[0];
  std::vector<std::string> terms(args.begin() + 1, args.end());
  std::unique_lock<std::shared_mutex> lk(GetTermDictsMutex());
  out_added = static_cast<int64_t>(GetTermDicts()[dict_name].Add(terms));
  return true;
}

bool ApplyLocalDictDel(const std::vector<std::string>& args, int64_t& out_removed,
                       std::string& err_out) {
  if (args.size() < 2) {
    err_out = "ERR wrong number of arguments for 'FT.DICTDEL' command";
    return false;
  }
  const std::string& dict_name = args[0];
  std::unique_lock<std::shared_mutex> lk(GetTermDictsMutex());
  auto it = GetTermDicts().find(dict_name);
  if (it == GetTermDicts().end()) {
    out_removed = 0;
    return true;
  }
  std::vector<std::string> terms(args.begin() + 1, args.end());
  out_removed = static_cast<int64_t>(it->second.Del(terms));
  return true;
}

bool ApplyLocalSynUpdate(const std::vector<std::string>& args, std::string& err_out) {
  if (args.size() < 3) {
    err_out = "ERR wrong number of arguments for 'FT.SYNUPDATE' command";
    return false;
  }
  std::string idx_str = ResolveAlias(args[0]);
  std::shared_lock<std::shared_mutex> lk(GetIndicesMutex());
  auto it = GetIndices().find(idx_str);
  if (it == GetIndices().end()) {
    err_out = "ERR index not found";
    return false;
  }
  std::unique_lock<std::shared_mutex> idx_lk(it->second->Mutex());
  std::vector<std::string> terms(args.begin() + 2, args.end());
  it->second->synonyms.Update(args[1], terms);
  return true;
}

// FT.DICTADD <dict> <term> [term ...]
int FtDictAddCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                            int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc - 1));
  for (int i = 1; i < argc; i++) args.emplace_back(ArgView(argv[i]));

  int ctx_flags = RedisModule_GetContextFlags ? RedisModule_GetContextFlags(ctx) : 0;
  bool is_replicated = (ctx_flags & REDISMODULE_CTX_FLAGS_REPLICATED) != 0;
  bool is_replica_or_replay = (ctx_flags & (REDISMODULE_CTX_FLAGS_REPLICATED |
                                            REDISMODULE_CTX_FLAGS_SLAVE |
                                            REDISMODULE_CTX_FLAGS_LOADING)) != 0;
  if (!is_replicated && gemini_search::ClusterManager::Instance().IsClusterScatterEnabled()) {
    return gemini_search::ExecuteClusterDdl(
        ctx, gemini_search::RpcOpCode::kRpcDdlDict, "FT.DICTADD", args, /*reply_integer=*/true);
  }

  int64_t added = 0;
  std::string err;
  if (!ApplyLocalDictAdd(args, added, err)) {
    return RedisModule_ReplyWithError(ctx, err.c_str());
  }
  if (added > 0) {
    RedisModule_ReplicateVerbatim(ctx);
    if (gemini_search::ClusterManager::Instance().IsClusterMode() && !is_replica_or_replay) {
      SetPersistedSchemaEpoch(gemini_search::ClusterManager::Instance().AdvanceSchemaEpoch());
    }
  }
  return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(added));
}

// FT.DICTDEL <dict> <term> [term ...]
int FtDictDelCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                            int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc - 1));
  for (int i = 1; i < argc; i++) args.emplace_back(ArgView(argv[i]));

  int ctx_flags = RedisModule_GetContextFlags ? RedisModule_GetContextFlags(ctx) : 0;
  bool is_replicated = (ctx_flags & REDISMODULE_CTX_FLAGS_REPLICATED) != 0;
  bool is_replica_or_replay = (ctx_flags & (REDISMODULE_CTX_FLAGS_REPLICATED |
                                            REDISMODULE_CTX_FLAGS_SLAVE |
                                            REDISMODULE_CTX_FLAGS_LOADING)) != 0;
  if (!is_replicated && gemini_search::ClusterManager::Instance().IsClusterScatterEnabled()) {
    return gemini_search::ExecuteClusterDdl(
        ctx, gemini_search::RpcOpCode::kRpcDdlDict, "FT.DICTDEL", args, /*reply_integer=*/true);
  }

  int64_t removed = 0;
  std::string err;
  if (!ApplyLocalDictDel(args, removed, err)) {
    return RedisModule_ReplyWithError(ctx, err.c_str());
  }
  if (removed > 0) {
    RedisModule_ReplicateVerbatim(ctx);
    if (gemini_search::ClusterManager::Instance().IsClusterMode() && !is_replica_or_replay) {
      SetPersistedSchemaEpoch(gemini_search::ClusterManager::Instance().AdvanceSchemaEpoch());
    }
  }
  return RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(removed));
}

// FT.DICTDUMP <dict>
int FtDictDumpCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                             int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string dict_name(ArgView(argv[1]));
  std::vector<std::string> terms;
  {
    std::shared_lock<std::shared_mutex> dlk(GetTermDictsMutex());
    auto it = GetTermDicts().find(dict_name);
    if (it == GetTermDicts().end()) {
      RedisModule_ReplyWithArray(ctx, 0);
      return REDISMODULE_OK;
    }
    terms = it->second.Dump();
  }
  RedisModule_ReplyWithArray(ctx, static_cast<long>(terms.size()));
  for (auto& t : terms) RedisModule_ReplyWithCString(ctx, t.c_str());
  return REDISMODULE_OK;
}

// FT.SYNUPDATE <index> <group_id> <term> [term ...]
int FtSynUpdateCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                               int argc) {
  if (argc < 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc - 1));
  for (int i = 1; i < argc; i++) args.emplace_back(ArgView(argv[i]));

  std::string idx_str = ResolveAlias(args[0]);
  {
    std::shared_lock<std::shared_mutex> lk(GetIndicesMutex());
    if (GetIndices().find(idx_str) == GetIndices().end()) {
      return RedisModule_ReplyWithError(ctx, "ERR index not found");
    }
  }

  int ctx_flags = RedisModule_GetContextFlags ? RedisModule_GetContextFlags(ctx) : 0;
  bool is_replicated = (ctx_flags & REDISMODULE_CTX_FLAGS_REPLICATED) != 0;
  bool is_replica_or_replay = (ctx_flags & (REDISMODULE_CTX_FLAGS_REPLICATED |
                                            REDISMODULE_CTX_FLAGS_SLAVE |
                                            REDISMODULE_CTX_FLAGS_LOADING)) != 0;
  if (!is_replicated && gemini_search::ClusterManager::Instance().IsClusterScatterEnabled()) {
    return gemini_search::ExecuteClusterDdl(
        ctx, gemini_search::RpcOpCode::kRpcDdlSyn, "FT.SYNUPDATE", args, /*reply_integer=*/false);
  }

  std::string err;
  if (!ApplyLocalSynUpdate(args, err)) {
    return RedisModule_ReplyWithError(ctx, err.c_str());
  }
  RedisModule_ReplicateVerbatim(ctx);
  if (gemini_search::ClusterManager::Instance().IsClusterMode() && !is_replica_or_replay) {
    SetPersistedSchemaEpoch(gemini_search::ClusterManager::Instance().AdvanceSchemaEpoch());
  }
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

// FT.SYNDUMP <index>
int FtSynDumpCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                            int argc) {
  if (argc != 2) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto entry_ptr = GetIndexEntryShared(std::string(ArgView(argv[1])));
  if (!entry_ptr) return RedisModule_ReplyWithError(ctx, "ERR index not found");

  std::vector<std::pair<std::string, std::vector<std::string>>> groups;
  {
    std::shared_lock<std::shared_mutex> idx_lk(entry_ptr->Mutex());
    groups = entry_ptr->synonyms.Dump();
  }
  long total = 0;
  for (auto& [gid, terms] : groups) total += 1 + static_cast<long>(terms.size());
  RedisModule_ReplyWithArray(ctx, total);
  for (auto& [gid, terms] : groups) {
    RedisModule_ReplyWithCString(ctx, gid.c_str());
    for (auto& t : terms) RedisModule_ReplyWithCString(ctx, t.c_str());
  }
  return REDISMODULE_OK;
}

gemini_search::RpcSpellCheckResponse ExecuteLocalSpellCheck(
    const gemini_search::RpcSpellCheckRequest& req) {
  gemini_search::RpcSpellCheckResponse resp;
  auto entry_ptr = GetIndexEntryShared(req.index_name);
  if (!entry_ptr) {
    resp.ok = false;
    resp.error_msg = "ERR index not found";
    return resp;
  }

  const auto& entry = *entry_ptr;
  std::shared_lock<std::shared_mutex> idx_lk(entry.Mutex());

  auto terms = TextIndex::Tokenize(req.query_str);
  std::unordered_set<std::string> known_local;
  for (const auto& f : entry.spec.fields) {
    if (f.type != FieldType::kText) continue;
    const auto* tidx = entry.text_indices.Get(f.name);
    if (!tidx) continue;
    for (const auto& term : terms) {
      if (!tidx->Lookup(term).empty()) known_local.insert(term);
    }
  }

  std::unordered_set<std::string> excluded;
  if (!req.exclude_dicts.empty()) {
    std::shared_lock<std::shared_mutex> dlk(GetTermDictsMutex());
    for (const auto& dname : req.exclude_dicts) {
      auto dit = GetTermDicts().find(dname);
      if (dit != GetTermDicts().end()) {
        for (auto& dt : dit->second.Dump()) {
          excluded.insert(std::move(dt));
        }
      }
    }
  }

  resp.ok = true;
  resp.known_terms.assign(known_local.begin(), known_local.end());
  std::sort(resp.known_terms.begin(), resp.known_terms.end());

  std::unordered_set<std::string> processed_terms;
  for (const auto& term : terms) {
    if (known_local.count(term) || excluded.count(term)) continue;
    if (!processed_terms.insert(term).second) continue;

    std::unordered_map<std::string, double> suggestion_scores;
    for (const auto& f : entry.spec.fields) {
      if (f.type != FieldType::kText) continue;
      const auto* tidx = entry.text_indices.Get(f.name);
      if (!tidx) continue;
      auto stem_results = tidx->StemSearch(term);
      for (const auto& r : stem_results) suggestion_scores[r.doc_id] += r.score;
      auto fuzzy_results = tidx->FuzzySearch(term, req.max_dist);
      for (const auto& r : fuzzy_results) suggestion_scores[r.doc_id] += r.score;
    }

    gemini_search::RpcSpellCheckTermSugs ts;
    ts.term = term;
    ts.suggestions.reserve(suggestion_scores.size());
    for (auto& [sug, sc] : suggestion_scores) {
      if (!excluded.count(sug)) ts.suggestions.emplace_back(sug, sc);
    }
    resp.term_suggestions.push_back(std::move(ts));
  }
  return resp;
}

// FT.SPELLCHECK <index> <query> [DISTANCE <d>] [TERMS INCLUDE|EXCLUDE <dict>]
int FtSpellCheckCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                               int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  auto entry_ptr = GetIndexEntryShared(idx_str);
  if (!entry_ptr) return RedisModule_ReplyWithError(ctx, "ERR index not found");
  auto& entry = *entry_ptr;

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

  if (gemini_search::ClusterManager::Instance().IsClusterScatterEnabled()) {
    gemini_search::RpcSpellCheckRequest req;
    req.index_name = idx_str;
    req.query_str = std::move(query_str);
    req.max_dist = max_dist;
    req.include_dicts = std::move(include_dicts);
    req.exclude_dicts = std::move(exclude_dicts);
    return gemini_search::ExecuteClusterSpellCheck(ctx, std::move(req));
  }

  // Tokenize query
  auto terms = TextIndex::Tokenize(query_str);

  std::shared_lock<std::shared_mutex> idx_lk(entry.Mutex());

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

  // Snapshot include & exclude dictionary terms under shared_lock
  std::vector<std::string> include_terms;
  std::unordered_set<std::string> excluded;
  {
    std::shared_lock<std::shared_mutex> dlk(GetTermDictsMutex());
    for (const auto& dname : include_dicts) {
      auto dit = GetTermDicts().find(dname);
      if (dit != GetTermDicts().end()) {
        for (const auto& term : terms) {
          if (dit->second.Contains(term)) index_terms.insert(term);
        }
        for (auto& dt : dit->second.Dump()) {
          include_terms.push_back(std::move(dt));
        }
      }
    }
    for (const auto& dname : exclude_dicts) {
      auto dit = GetTermDicts().find(dname);
      if (dit != GetTermDicts().end()) {
        for (auto& dt : dit->second.Dump()) {
          excluded.insert(std::move(dt));
        }
      }
    }
  }

  // For each misspelled term, find suggestions from index & include dicts, excluding exclude_dicts
  struct SpellSuggestion {
    std::string term;
    std::vector<std::pair<double, std::string>> suggestions;
  };
  std::vector<SpellSuggestion> results;

  for (auto& term : terms) {
    if (index_terms.count(term) || excluded.count(term)) continue;

    SpellSuggestion ss;
    ss.term = term;

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

    for (const auto& dt : include_terms) {
      if (excluded.count(dt)) continue;
      int dist = TextIndex::LevenshteinDistance(term, dt);
      if (dist <= max_dist && dist > 0) {
        suggestion_scores[dt] += 1.0 / static_cast<double>(dist);
      }
    }

    for (auto& [sug, score] : suggestion_scores) {
      if (!excluded.count(sug))
        ss.suggestions.emplace_back(score, sug);
    }
    std::sort(ss.suggestions.begin(), ss.suggestions.end(),
              [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first > b.first;
                return a.second < b.second;
              });

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
    std::shared_lock<std::shared_mutex> lk(GetConfigMutex());
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
    std::string val(ArgView(argv[3]));
    {
      std::shared_lock<std::shared_mutex> lk(GetConfigMutex());
      if (GetConfig().find(param) == GetConfig().end()) {
        return RedisModule_ReplyWithError(ctx, "ERR unknown config parameter");
      }
    }
    if (param == "ON_TIMEOUT") {
      if (MatchArg(val, "FAIL")) val = "FAIL";
      else if (MatchArg(val, "RETURN")) val = "RETURN";
      else return RedisModule_ReplyWithError(ctx, "ERR ON_TIMEOUT must be FAIL or RETURN");
    } else if (param == "MAXSEARCHRESULTS" || param == "ASYNC_MIN_DOCS" ||
               param == "BG_INDEX_MIN_KEYS" || param == "BG_INDEX_TICK_MS") {
      char* ep = nullptr;
      long long v = std::strtoll(val.c_str(), &ep, 10);
      if (*ep != '\0' || v < 0) {
        return RedisModule_ReplyWithError(ctx, "ERR parameter must be a non-negative integer");
      }
    } else if (param == "BG_INDEX_BATCH_KEYS" || param == "CLUSTER_QUERY_TIMEOUT_MS" ||
               param == "CLUSTER_MAX_SCATTER_LIMIT" || param == "CLUSTER_AGG_MAX_GROUPS") {
      char* ep = nullptr;
      long long v = std::strtoll(val.c_str(), &ep, 10);
      if (*ep != '\0' || v <= 0) {
        return RedisModule_ReplyWithError(ctx, "ERR parameter must be a positive integer");
      }
    } else if (param == "CLUSTER_RPC_PORT_OFFSET") {
      char* ep = nullptr;
      long long v = std::strtoll(val.c_str(), &ep, 10);
      if (*ep != '\0' || v < 0 || v > 50000) {
        return RedisModule_ReplyWithError(ctx, "ERR CLUSTER_RPC_PORT_OFFSET must be in [0, 50000]");
      }
    } else if (param == "CLUSTER_ENABLED" || param == "CLUSTER_BM25_DFS") {
      if (val == "1" || MatchArg(val, "TRUE") || MatchArg(val, "YES")) {
        val = "1";
      } else if (val == "0" || MatchArg(val, "FALSE") || MatchArg(val, "NO")) {
        val = "0";
      } else {
        return RedisModule_ReplyWithError(ctx, "ERR parameter must be 0 or 1");
      }
      if (param == "CLUSTER_ENABLED") {
        gemini_search::ClusterManager::Instance().SetClusterEnabled(val == "1");
      }
    }
    {
      std::unique_lock<std::shared_mutex> lk(GetConfigMutex());
      GetConfig()[param] = std::move(val);
    }
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
  }

  return RedisModule_ReplyWithError(ctx, "ERR FT.CONFIG requires GET or SET");
}


// FT._DEBUG [CLUSTER [PING [timeout_ms]]]
int FtDebugCommand(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
  if (argc >= 2 && MatchArg(ArgView(argv[1]), "CLUSTER")) {
    return gemini_search::HandleDebugClusterCommand(ctx, argv, argc);
  }
  return RedisModule_ReplyWithSimpleString(ctx, "GeminiSearch OK");
}

