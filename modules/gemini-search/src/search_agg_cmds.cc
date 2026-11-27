#include "cluster_agg_reduce.h"
#include "cluster_coordinator.h"
#include "cluster_topology.h"
#include "search_internal.h"
#include "expr_engine.h"
#include "query_parser.h"

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

using gemini_search::AggRow;
using gemini_search::PipelineStage;
using gemini_search::PipelineStageType;
using gemini_search::ReduceFunc;
using gemini_search::ReduceOp;

namespace {

bool ParseAggLong(RedisModuleString* arg, long long& out) {
  std::string s(ArgView(arg));
  if (s.empty()) return false;
  char* ep = nullptr;
  out = std::strtoll(s.c_str(), &ep, 10);
  return ep != s.c_str() && *ep == '\0';
}

bool ParseAggU64(RedisModuleString* arg, uint64_t& out) {
  std::string s(ArgView(arg));
  if (s.empty() || s[0] == '-') return false;
  char* ep = nullptr;
  out = static_cast<uint64_t>(std::strtoull(s.c_str(), &ep, 10));
  return ep != s.c_str() && *ep == '\0';
}

bool ParseAggDouble(RedisModuleString* arg, double& out) {
  std::string s(ArgView(arg));
  if (s.empty()) return false;
  char* ep = nullptr;
  out = std::strtod(s.c_str(), &ep);
  return ep != s.c_str() && *ep == '\0' && std::isfinite(out);
}

constexpr const char* kInternalDocIdKey = "\x01__doc_id";
constexpr const char* kInternalKnnDistKey = "\x01__knn_dist";

}  // namespace

gemini_search::RpcCursorResponse ExecuteLocalCursorOp(
    const gemini_search::RpcCursorRequest& req) {
  gemini_search::RpcCursorResponse resp;
  int64_t now_ms = NowMs();

  std::lock_guard<std::mutex> lk(GetCursorsMutex());
  PurgeExpiredCursorsLocked(now_ms);

  if (req.op == gemini_search::kRpcCursorOpDel) {
    if (GetCursors().erase(req.cursor_id) == 0) {
      resp.ok = false;
      resp.error_msg = "ERR cursor not found";
      return resp;
    }
    resp.ok = true;
    return resp;
  }

  auto it = GetCursors().find(req.cursor_id);
  if (it == GetCursors().end()) {
    resp.ok = false;
    resp.error_msg = "ERR cursor not found";
    return resp;
  }

  AggCursor& cursor = it->second;
  cursor.last_access_ms = now_ms;
  if (req.has_count && req.count > 0) {
    cursor.batch_size = static_cast<size_t>(req.count);
  }

  size_t end = std::min(cursor.pos + cursor.batch_size, cursor.rows.size());
  resp.rows.reserve(end - cursor.pos);
  for (size_t ri = cursor.pos; ri < end; ri++) {
    const auto& row = cursor.rows[ri];
    std::vector<std::string> keys;
    keys.reserve(row.size());
    for (const auto& [k, v] : row) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    std::vector<std::pair<std::string, std::string>> out_row;
    out_row.reserve(keys.size());
    for (const auto& k : keys) {
      out_row.emplace_back(k, row.at(k));
    }
    resp.rows.push_back(std::move(out_row));
  }

  cursor.pos = end;
  if (cursor.pos >= cursor.rows.size()) {
    GetCursors().erase(it);
    resp.next_cursor_id = 0;
  } else {
    resp.next_cursor_id = req.cursor_id;
  }
  resp.ok = true;
  return resp;
}

void ReplyCursorBatch(RedisModuleCtx* ctx, uint64_t cursor_id,
                             AggCursor& cursor) {
  size_t end = std::min(cursor.pos + cursor.batch_size, cursor.rows.size());
  size_t count = end - cursor.pos;

  // Reply: [results_array, cursor_id]
  RedisModule_ReplyWithArray(ctx, 2);

  RedisModule_ReplyWithArray(ctx, static_cast<long>(count));
  for (size_t ri = cursor.pos; ri < end; ri++) {
    auto& row = cursor.rows[ri];
    std::vector<std::string> keys;
    for (auto& [k, v] : row) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    RedisModule_ReplyWithArray(ctx, static_cast<long>(keys.size()) * 2);
    for (auto& k : keys) {
      RedisModule_ReplyWithStringBuffer(ctx, k.data(), k.size());
      const std::string& val = row.at(k);
      RedisModule_ReplyWithStringBuffer(ctx, val.data(), val.size());
    }
  }

  cursor.pos = end;
  if (cursor.pos >= cursor.rows.size()) {
    GetCursors().erase(cursor_id);
    RedisModule_ReplyWithLongLong(ctx, 0);
  } else {
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(cursor_id));
  }
}

// FT.CURSOR READ <index> <cursor_id> [COUNT <n>]
// FT.CURSOR DEL <index> <cursor_id>
int FtCursorCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
  if (argc < 4) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  auto sub = ArgView(argv[1]);
  bool is_read = MatchArg(sub, "READ");
  bool is_del = MatchArg(sub, "DEL");
  if (!is_read && !is_del) {
    return RedisModule_ReplyWithError(ctx, "ERR unknown CURSOR subcommand, expected READ or DEL");
  }

  uint64_t cid = 0;
  if (!ParseAggU64(argv[3], cid)) {
    return RedisModule_ReplyWithError(ctx, "ERR invalid cursor id");
  }

  gemini_search::RpcCursorRequest req;
  req.op = is_read ? gemini_search::kRpcCursorOpRead : gemini_search::kRpcCursorOpDel;
  req.index_name = ResolveAlias(std::string(ArgView(argv[2])));
  req.cursor_id = cid;
  if (is_read && argc >= 6 && MatchArg(ArgView(argv[4]), "COUNT")) {
    long long cnt = 0;
    if (ParseAggLong(argv[5], cnt) && cnt > 0) {
      req.has_count = true;
      req.count = static_cast<uint64_t>(cnt);
    }
  }

  // First try local cursor map (handles standalone and same-node cluster cursors)
  auto local_resp = ExecuteLocalCursorOp(req);
  if (local_resp.ok) {
    if (is_del) {
      return RedisModule_ReplyWithSimpleString(ctx, "OK");
    }
    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithArray(ctx, static_cast<long>(local_resp.rows.size()));
    for (const auto& row : local_resp.rows) {
      RedisModule_ReplyWithArray(ctx, static_cast<long>(row.size()) * 2);
      for (const auto& [k, v] : row) {
        RedisModule_ReplyWithStringBuffer(ctx, k.data(), k.size());
        RedisModule_ReplyWithStringBuffer(ctx, v.data(), v.size());
      }
    }
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(local_resp.next_cursor_id));
    return REDISMODULE_OK;
  }

  // Phase 70: If not found locally in cluster mode, forward to Coordinator owner encoded in cursor_id
  if (gemini_search::ClusterManager::Instance().IsClusterScatterEnabled() &&
      gemini_search::ExtractCursorNodeTag(cid) != 0) {
    return gemini_search::ExecuteClusterCursor(ctx, std::move(req));
  }

  return RedisModule_ReplyWithError(ctx, local_resp.error_msg.c_str());
}

// =============================================================
// FT.AGGREGATE pipeline (delegates to cluster_agg_reduce.cc)
// =============================================================

static bool ExecuteAggPipeline(std::vector<AggRow>& rows,
                               const std::vector<PipelineStage>& stages,
                               int64_t deadline_ms, std::string& error_out) {
  return gemini_search::ExecuteAggPipelineStages(rows, stages, deadline_ms, error_out);
}

// Phase 70: Lock-only shard Map execution for distributed FT.AGGREGATE (zero Redis GIL!)
gemini_search::RpcAggMapResponse ExecuteLocalAggMap(
    const gemini_search::RpcAggMapRequest& req) {
  gemini_search::RpcAggMapResponse resp;
  resp.mode = req.mode;

  auto entry_ptr = GetIndexEntryShared(req.index_name);
  if (!entry_ptr) {
    resp.ok = false;
    resp.error_msg = "ERR index not found";
    return resp;
  }

  const auto& entry = *entry_ptr;
  std::shared_lock<std::shared_mutex> idx_lk(entry.Mutex());

  std::unordered_map<std::string, std::string> params;
  params.reserve(req.params.size());
  for (const auto& [k, v] : req.params) params[k] = v;

  QueryOptions qopts;
  qopts.language = entry.spec.language;
  qopts.dialect = req.dialect;
  qopts.max_expansions = GetMaxExpansions();
  qopts.min_stem_len = GetMinStemLen();

  std::unordered_set<std::string> custom_dict_copy;
  if (!entry.spec.custom_dict.empty()) {
    std::shared_lock<std::shared_mutex> dlk(GetTermDictsMutex());
    auto dit = GetTermDicts().find(entry.spec.custom_dict);
    if (dit != GetTermDicts().end()) {
      custom_dict_copy = dit->second.Terms();
      qopts.custom_dict = &custom_dict_copy;
    }
  }
  int64_t deadline_ms = 0;
  if (req.remaining_timeout_ms > 0) {
    deadline_ms = NowMs() + req.remaining_timeout_ms;
    qopts.deadline_ms = deadline_ms;
  }

  ParsedQuery parsed;
  std::string parse_err;
  if (!ParseQuery(req.query_str, parsed, parse_err, qopts)) {
    resp.ok = false;
    resp.error_msg = std::move(parse_err);
    return resp;
  }

  std::vector<std::string> result_ids;
  std::unordered_map<std::string, double> knn_dists;
  std::string knn_score_field =
      parsed.knn_score_alias.empty() ? "__vec_score" : parsed.knn_score_alias;

  if (parsed.has_knn) {
    const auto* fspec = entry.spec.FindField(parsed.knn_field);
    if (!fspec || fspec->type != FieldType::kVector) {
      resp.ok = false;
      resp.error_msg = "ERR KNN field is not a VECTOR field";
      return resp;
    }
    auto pit = params.find(parsed.knn_param_name);
    if (pit == params.end()) {
      resp.ok = false;
      resp.error_msg = "ERR KNN param not found in PARAMS";
      return resp;
    }
    std::vector<float> qvec;
    if (!DecodeVectorBlob(pit->second, fspec->vector_params.dim, fspec->vector_params.data_type,
                          qvec)) {
      resp.ok = false;
      resp.error_msg = "ERR query vector dimension mismatch";
      return resp;
    }
    if (const auto* vidx = entry.vector_indices.Get(parsed.knn_field)) {
      std::vector<KnnResult> knn_hits;
      if (parsed.root.type != QueryNode::Type::kMatchAll) {
        std::string ferr;
        auto cands = EvaluateQuery(parsed.root, entry.spec, entry.doc_store, entry.tag_indices,
                                   entry.numeric_indices, entry.text_indices, entry.geo_indices,
                                   ferr, qopts, &entry.geoshape_indices, &params,
                                   &entry.vector_indices, &entry.synonyms);
        if (!ferr.empty()) {
          resp.ok = false;
          resp.error_msg = std::move(ferr);
          return resp;
        }
        knn_hits = vidx->KnnQueryFiltered(qvec.data(), parsed.knn_k, cands, parsed.knn_ef_runtime,
                                          parsed.knn_oversample);
      } else {
        knn_hits = vidx->KnnQueryEf(qvec.data(), parsed.knn_k, parsed.knn_ef_runtime,
                                    parsed.knn_oversample);
      }
      result_ids.reserve(knn_hits.size());
      for (const auto& h : knn_hits) {
        result_ids.push_back(h.doc_id);
        knn_dists[h.doc_id] = static_cast<double>(h.score);
      }
    }
  } else {
    std::string eval_err;
    result_ids = EvaluateQuery(parsed.root, entry.spec, entry.doc_store, entry.tag_indices,
                               entry.numeric_indices, entry.text_indices, entry.geo_indices,
                               eval_err, qopts, &entry.geoshape_indices, &params,
                               &entry.vector_indices, &entry.synonyms);
    if (!eval_err.empty()) {
      resp.ok = false;
      resp.error_msg = std::move(eval_err);
      return resp;
    }
  }

  std::vector<AggRow> rows;
  rows.reserve(result_ids.size());
  for (const auto& doc_id : result_ids) {
    const auto* doc = entry.doc_store.Get(doc_id);
    if (!doc) continue;
    AggRow r = doc->fields;
    if (parsed.has_knn) {
      auto sit = knn_dists.find(doc_id);
      if (sit != knn_dists.end()) {
        r[knn_score_field] = std::to_string(static_cast<float>(sit->second));
        if (req.mode == 0) r[kInternalKnnDistKey] = std::to_string(sit->second);
      }
    }
    if (req.mode == 0) r[kInternalDocIdKey] = doc_id;
    rows.push_back(std::move(r));
  }

  std::string pipe_err;
  if (!req.map_stages.empty() &&
      !gemini_search::ExecuteAggPipelineStages(rows, req.map_stages, deadline_ms, pipe_err)) {
    resp.ok = false;
    resp.error_msg = std::move(pipe_err);
    return resp;
  }

  if (req.mode == 1) {
    // Two-phase Map-Reduce: partial GROUPBY reduction on shard with max_groups quota
    size_t max_g = static_cast<size_t>(req.max_groups);
    if (!gemini_search::BuildPartialGroups(rows, req.groupby_stage.groupby_fields,
                                           req.groupby_stage.reducers, max_g, resp.partial_groups,
                                           pipe_err)) {
      resp.ok = false;
      resp.error_msg = std::move(pipe_err);
      return resp;
    }
    resp.ok = true;
    return resp;
  }

  // Raw rows mode: optional pushed-down local SORTBY & top-K truncation
  if (req.raw_has_sortby && !req.raw_sortby_keys.empty()) {
    long long max_r = (req.raw_shard_limit > 0) ? static_cast<long long>(req.raw_shard_limit) : -1;
    gemini_search::ApplyAggSortby(rows, req.raw_sortby_keys, max_r);
  } else if (req.raw_shard_limit > 0 && rows.size() > static_cast<size_t>(req.raw_shard_limit)) {
    rows.resize(static_cast<size_t>(req.raw_shard_limit));
  }

  resp.raw_rows.reserve(rows.size());
  for (auto& r : rows) {
    gemini_search::RpcAggRawRow out_r;
    auto dit = r.find(kInternalDocIdKey);
    if (dit != r.end()) {
      out_r.doc_id = std::move(dit->second);
      r.erase(dit);
    }
    auto kit = r.find(kInternalKnnDistKey);
    if (kit != r.end()) {
      double dv = 0.0;
      if (gemini_search::TryParseAggDouble(kit->second, dv)) {
        out_r.has_knn_dist = true;
        out_r.knn_dist = dv;
      }
      r.erase(kit);
    }
    out_r.fields.reserve(r.size());
    for (auto& [k, v] : r) out_r.fields.emplace_back(k, std::move(v));
    resp.raw_rows.push_back(std::move(out_r));
  }
  resp.ok = true;
  return resp;
}

int FtAggregateCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                              int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  auto it = GetIndices().find(idx_str);
  if (it == GetIndices().end()) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  auto& entry = *it->second;
  TouchIndex(entry);

  // Pre-scan for DIALECT, TIMEOUT, and PARAMS options
  QueryOptions qopts;
  qopts.language = entry.spec.language;
  qopts.dialect = GetDefaultDialect();
  if (!entry.spec.custom_dict.empty()) {
    auto dit = GetTermDicts().find(entry.spec.custom_dict);
    if (dit != GetTermDicts().end()) qopts.custom_dict = &dit->second.Terms();
  }
  long long timeout_ms = 0;
  std::unordered_map<std::string, std::string> params;
  for (int ci = 3; ci < argc; ci++) {
    auto kw = ArgView(argv[ci]);
    if (MatchArg(kw, "DIALECT") && ci + 1 < argc) {
      qopts.dialect = static_cast<int>(std::strtol(std::string(ArgView(argv[ci + 1])).c_str(), nullptr, 10));
      ci++;
    } else if (MatchArg(kw, "TIMEOUT") && ci + 1 < argc) {
      long long tms = std::strtoll(std::string(ArgView(argv[ci + 1])).c_str(), nullptr, 10);
      if (tms > 0) {
        timeout_ms = tms;
        qopts.deadline_ms = NowMs() + tms;
      }
      ci++;
    } else if (MatchArg(kw, "PARAMS") && ci + 1 < argc) {
      long long pcount = 0;
      if (!ParseAggLong(argv[ci + 1], pcount) || pcount < 0 || pcount % 2 != 0 ||
          ci + 2 + pcount > argc) {
        return RedisModule_ReplyWithError(ctx, "ERR invalid PARAMS specification");
      }
      for (long long p = 0; p < pcount; p += 2) {
        std::string pname(ArgView(argv[ci + 2 + p]));
        std::string pval = ArgStr(argv[ci + 3 + p]);
        params[std::move(pname)] = std::move(pval);
      }
      ci += 1 + static_cast<int>(pcount);
    }
  }

  ParsedQuery parsed;
  std::string parse_error;
  if (!ParseQuery(std::string(ArgView(argv[2])), parsed, parse_error, qopts)) {
    return RedisModule_ReplyWithError(ctx, parse_error.c_str());
  }

  std::vector<float> qvec;
  std::string knn_score_field = parsed.knn_score_alias.empty()
                                    ? "__vec_score"
                                    : parsed.knn_score_alias;
  if (parsed.has_knn) {
    const auto* fspec = entry.spec.FindField(parsed.knn_field);
    if (!fspec || fspec->type != FieldType::kVector) {
      return RedisModule_ReplyWithError(ctx, "ERR KNN field is not a VECTOR field");
    }
    auto pit = params.find(parsed.knn_param_name);
    if (pit == params.end()) {
      return RedisModule_ReplyWithError(ctx, "ERR KNN param not found in PARAMS");
    }
    if (!DecodeVectorBlob(pit->second, fspec->vector_params.dim,
                          fspec->vector_params.data_type, qvec)) {
      return RedisModule_ReplyWithError(ctx, "ERR query vector dimension mismatch");
    }
  }

  // Parse pipeline stages
  std::vector<PipelineStage> stages;
  PipelineStage* pending_groupby = nullptr;

  int arg_i = 3;
  while (arg_i < argc) {
    auto kw = ArgView(argv[arg_i]);

    if (MatchArg(kw, "LOAD")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR LOAD requires count or *");
      if (ArgView(argv[arg_i]) == "*") {
        arg_i++;
        PipelineStage st;
        st.type = PipelineStageType::kLoad;
        stages.push_back(std::move(st));
        continue;
      }
      long long ld_count = 0;
      if (!ParseAggLong(argv[arg_i++], ld_count) || ld_count < 0)
        return RedisModule_ReplyWithError(ctx, "ERR LOAD count must be non-negative");
      PipelineStage st;
      st.type = PipelineStageType::kLoad;
      for (long long li = 0; li < ld_count; li++) {
        if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR not enough LOAD fields");
        auto fv = ArgView(argv[arg_i]);
        arg_i++;
        if (fv.empty() || fv[0] != '@')
          return RedisModule_ReplyWithError(ctx, "ERR LOAD field must start with @");
        st.load_fields.emplace_back(fv.substr(1));
      }
      stages.push_back(std::move(st));
    } else if (MatchArg(kw, "APPLY")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR APPLY requires expression");
      std::string expr(ArgView(argv[arg_i]));
      arg_i++;
      if (arg_i >= argc || !MatchArg(ArgView(argv[arg_i]), "AS"))
        return RedisModule_ReplyWithError(ctx, "ERR APPLY requires AS alias");
      arg_i++;
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR APPLY AS requires alias name");
      std::string alias(ArgView(argv[arg_i]));
      arg_i++;
      PipelineStage st;
      st.type = PipelineStageType::kApply;
      st.apply_expr = std::move(expr);
      st.apply_alias = std::move(alias);
      stages.push_back(std::move(st));
    } else if (MatchArg(kw, "FILTER")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR FILTER requires expression");
      PipelineStage st;
      st.type = PipelineStageType::kFilter;
      st.filter_expr = std::string(ArgView(argv[arg_i]));
      arg_i++;
      stages.push_back(std::move(st));
    } else if (MatchArg(kw, "GROUPBY")) {
      arg_i++;
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR GROUPBY requires count");
      long long gb_count = 0;
      if (!ParseAggLong(argv[arg_i++], gb_count) || gb_count < 0)
        return RedisModule_ReplyWithError(ctx, "ERR GROUPBY count must be non-negative");
      PipelineStage st;
      st.type = PipelineStageType::kGroupby;
      for (long long g = 0; g < gb_count; g++) {
        if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR not enough GROUPBY fields");
        auto fv = ArgView(argv[arg_i]);
        arg_i++;
        if (fv.empty() || fv[0] != '@')
          return RedisModule_ReplyWithError(ctx, "ERR GROUPBY field must start with @");
        st.groupby_fields.emplace_back(fv.substr(1));
      }
      stages.push_back(std::move(st));
      pending_groupby = &stages.back();
    } else if (MatchArg(kw, "REDUCE")) {
      arg_i++;
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR REDUCE requires function name");
      auto func_str = ArgView(argv[arg_i]);
      arg_i++;

      ReduceOp rop;
      bool needs_field = true;
      if (MatchArg(func_str, "COUNT")) {
        rop.func = ReduceFunc::kCount; needs_field = false;
      } else if (MatchArg(func_str, "SUM")) {
        rop.func = ReduceFunc::kSum;
      } else if (MatchArg(func_str, "AVG")) {
        rop.func = ReduceFunc::kAvg;
      } else if (MatchArg(func_str, "MIN")) {
        rop.func = ReduceFunc::kMin;
      } else if (MatchArg(func_str, "MAX")) {
        rop.func = ReduceFunc::kMax;
      } else if (MatchArg(func_str, "COUNT_DISTINCT")) {
        rop.func = ReduceFunc::kCountDistinct;
      } else if (MatchArg(func_str, "STDDEV")) {
        rop.func = ReduceFunc::kStddev;
      } else if (MatchArg(func_str, "QUANTILE")) {
        rop.func = ReduceFunc::kQuantile;
      } else if (MatchArg(func_str, "TOLIST")) {
        rop.func = ReduceFunc::kTolist;
      } else if (MatchArg(func_str, "FIRST_VALUE")) {
        rop.func = ReduceFunc::kFirstValue;
      } else if (MatchArg(func_str, "RANDOM_SAMPLE")) {
        rop.func = ReduceFunc::kRandomSample;
      } else if (MatchArg(func_str, "COUNT_DISTINCTISH")) {
        rop.func = ReduceFunc::kCountDistinctish; needs_field = true;
      } else {
        return RedisModule_ReplyWithError(ctx, "ERR unknown REDUCE function");
      }

      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR REDUCE requires nargs");
      long long nargs = 0;
      if (!ParseAggLong(argv[arg_i++], nargs) || nargs < 0)
        return RedisModule_ReplyWithError(ctx, "ERR REDUCE nargs must be non-negative");

      long long consumed = 0;
      if (needs_field && nargs >= 1) {
        if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR REDUCE missing field argument");
        auto rf = ArgView(argv[arg_i]);
        arg_i++; consumed++;
        if (rf.empty() || rf[0] != '@')
          return RedisModule_ReplyWithError(ctx, "ERR REDUCE field must start with @");
        rop.field = std::string(rf.substr(1));
      }

      // Parse extra args for specific reducers
      if (rop.func == ReduceFunc::kQuantile && consumed < nargs) {
        if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR QUANTILE requires percentile");
        ParseAggDouble(argv[arg_i++], rop.quantile_pct);
        consumed++;
      }
      if (rop.func == ReduceFunc::kFirstValue && consumed < nargs) {
        if (arg_i < argc && MatchArg(ArgView(argv[arg_i]), "BY")) {
          arg_i++; consumed++;
          if (arg_i < argc && consumed < nargs) {
            auto by_f = ArgView(argv[arg_i]);
            if (!by_f.empty() && by_f[0] == '@')
              rop.first_value_by = std::string(by_f.substr(1));
            arg_i++; consumed++;
          }
          if (arg_i < argc && consumed < nargs) {
            auto dir = ArgView(argv[arg_i]);
            if (MatchArg(dir, "DESC")) { rop.first_value_desc = true; arg_i++; consumed++; }
            else if (MatchArg(dir, "ASC")) { arg_i++; consumed++; }
          }
        }
      }
      if (rop.func == ReduceFunc::kRandomSample && consumed < nargs) {
        if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR RANDOM_SAMPLE requires count");
        long long rsc = 0;
        ParseAggLong(argv[arg_i++], rsc);
        rop.random_sample_count = static_cast<long>(rsc);
        consumed++;
      }

      // Skip any remaining args
      for (long long skip = consumed; skip < nargs; skip++) {
        if (arg_i < argc) arg_i++;
      }

      if (arg_i >= argc || !MatchArg(ArgView(argv[arg_i]), "AS"))
        return RedisModule_ReplyWithError(ctx, "ERR REDUCE requires AS alias");
      arg_i++;
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR REDUCE AS requires alias name");
      rop.alias = std::string(ArgView(argv[arg_i]));
      arg_i++;

      if (pending_groupby) {
        pending_groupby->reducers.push_back(std::move(rop));
      } else {
        // Implicit empty GROUPBY
        PipelineStage st;
        st.type = PipelineStageType::kGroupby;
        st.reducers.push_back(std::move(rop));
        stages.push_back(std::move(st));
        pending_groupby = &stages.back();
      }
    } else if (MatchArg(kw, "SORTBY")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR SORTBY requires count");
      long long sb_count = 0;
      if (!ParseAggLong(argv[arg_i++], sb_count) || sb_count < 1 || arg_i + sb_count > argc)
        return RedisModule_ReplyWithError(ctx, "ERR SORTBY count invalid");
      PipelineStage st;
      st.type = PipelineStageType::kSortby;
      int sb_end = arg_i + static_cast<int>(sb_count);
      while (arg_i < sb_end) {
        auto sf = ArgView(argv[arg_i++]);
        if (sf.empty() || sf[0] != '@')
          return RedisModule_ReplyWithError(ctx, "ERR SORTBY field must start with @");
        bool desc = false;
        if (arg_i < sb_end) {
          auto dir = ArgView(argv[arg_i]);
          if (MatchArg(dir, "ASC")) { desc = false; arg_i++; }
          else if (MatchArg(dir, "DESC")) { desc = true; arg_i++; }
        }
        st.sortby_keys.emplace_back(std::string(sf.substr(1)), desc);
      }
      if (arg_i + 1 < argc && MatchArg(ArgView(argv[arg_i]), "MAX")) {
        long long m = 0;
        if (!ParseAggLong(argv[arg_i + 1], m) || m < 0)
          return RedisModule_ReplyWithError(ctx, "ERR SORTBY MAX must be a non-negative integer");
        st.sortby_max = m;
        arg_i += 2;
      }
      stages.push_back(std::move(st));
    } else if (MatchArg(kw, "PARAMS")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR PARAMS requires count");
      long long pcount = 0;
      if (!ParseAggLong(argv[arg_i++], pcount) || pcount < 0 || arg_i + pcount > argc)
        return RedisModule_ReplyWithError(ctx, "ERR invalid PARAMS count");
      arg_i += static_cast<int>(pcount);
    } else if (MatchArg(kw, "LIMIT")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i + 1 >= argc)
        return RedisModule_ReplyWithError(ctx, "ERR LIMIT requires offset and count");
      char* ep1 = nullptr;
      char* ep2 = nullptr;
      std::string off_s(ArgView(argv[arg_i]));
      std::string cnt_s(ArgView(argv[arg_i + 1]));
      long long off = std::strtoll(off_s.c_str(), &ep1, 10);
      long long cnt = std::strtoll(cnt_s.c_str(), &ep2, 10);
      if (*ep1 != '\0' || *ep2 != '\0' || off < 0 || cnt < 0)
        return RedisModule_ReplyWithError(ctx, "ERR LIMIT offset and count must be non-negative");
      PipelineStage st;
      st.type = PipelineStageType::kLimit;
      st.limit_offset = off;
      st.limit_count = cnt;
      stages.push_back(std::move(st));
      arg_i += 2;
    } else if (MatchArg(kw, "WITHCURSOR")) {
      pending_groupby = nullptr;
      arg_i++;
    } else if (MatchArg(kw, "COUNT")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc)
        return RedisModule_ReplyWithError(ctx, "ERR COUNT requires a value");
      arg_i++;
    } else if (MatchArg(kw, "MAXIDLE")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc)
        return RedisModule_ReplyWithError(ctx, "ERR MAXIDLE requires a value");
      arg_i++;
    } else if (MatchArg(kw, "DIALECT") || MatchArg(kw, "TIMEOUT") ||
               MatchArg(kw, "SCORER")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc)
        return RedisModule_ReplyWithError(ctx, "ERR option requires a value");
      arg_i++;
    } else if (MatchArg(kw, "VERBATIM") || MatchArg(kw, "ADDSCORES")) {
      pending_groupby = nullptr;
      arg_i++;
    } else {
      return RedisModule_ReplyWithError(ctx, "ERR unknown AGGREGATE option");
    }
  }

  // Check for WITHCURSOR / COUNT / MAXIDLE
  bool with_cursor = false;
  size_t cursor_count = 1000;
  int64_t cursor_max_idle_ms = 300000;
  for (int ci = 3; ci < argc; ci++) {
    if (MatchArg(ArgView(argv[ci]), "WITHCURSOR")) {
      with_cursor = true;
    } else if (MatchArg(ArgView(argv[ci]), "COUNT") && ci + 1 < argc) {
      long long cv = 0;
      if (ParseAggLong(argv[ci + 1], cv) && cv > 0) cursor_count = static_cast<size_t>(cv);
    } else if (MatchArg(ArgView(argv[ci]), "MAXIDLE") && ci + 1 < argc) {
      long long mv = 0;
      if (!ParseAggLong(argv[ci + 1], mv) || mv < 0) {
        return RedisModule_ReplyWithError(ctx, "ERR MAXIDLE must be a non-negative integer");
      }
      cursor_max_idle_ms = static_cast<int64_t>(mv);
    }
  }

  // Phase 70: Cluster Two-Phase Map-Reduce FT.AGGREGATE & Coordinator-hosted cursors
  if (gemini_search::ClusterManager::Instance().IsClusterScatterEnabled()) {
    int64_t cluster_timeout_ms =
        (timeout_ms > 0) ? static_cast<int64_t>(timeout_ms)
                         : static_cast<int64_t>(GetClusterQueryTimeoutMs());

    auto split = gemini_search::SplitAggPipeline(stages, parsed.has_knn);

    gemini_search::ClusterAggPlan aplan;
    aplan.is_knn = parsed.has_knn;
    aplan.knn_k = parsed.knn_k;
    aplan.with_cursor = with_cursor;
    aplan.cursor_count = cursor_count;
    aplan.cursor_max_idle_ms = cursor_max_idle_ms;
    aplan.timeout_ms = cluster_timeout_ms;
    aplan.reduce_stages = std::move(split.reduce_stages);

    auto& mreq = aplan.map_req;
    mreq.index_name = idx_str;
    mreq.query_str = std::string(ArgView(argv[2]));
    mreq.params.reserve(params.size());
    for (const auto& [k, v] : params) mreq.params.emplace_back(k, v);
    mreq.dialect = qopts.dialect;
    mreq.remaining_timeout_ms = cluster_timeout_ms;
    mreq.mode = (split.mode == gemini_search::ClusterAggPlanSplit::Mode::kMapReduceGroupby) ? 1 : 0;
    mreq.map_stages = std::move(split.map_stages);
    mreq.groupby_stage = std::move(split.groupby_stage);
    mreq.max_groups = static_cast<uint64_t>(GetClusterAggMaxGroups());
    mreq.raw_shard_limit = static_cast<uint64_t>(split.raw_shard_limit);
    mreq.raw_has_sortby = split.raw_has_sortby;
    mreq.raw_sortby_keys = std::move(split.raw_sortby_keys);

    return gemini_search::ExecuteClusterAggregate(ctx, std::move(aplan));
  }

  // Phase 53: Non-blocking async offload for non-cursor FT.AGGREGATE
  size_t async_min = GetAsyncMinDocs();
  if (!with_cursor && RedisModule_BlockClient && RedisModule_UnblockClient &&
      async_min > 0 && entry.doc_store.Size() >= async_min) {
    struct AsyncAggReply {
      std::string error;
      std::vector<AggRow> rows;
    };

    auto ReplyCb = [](RedisModuleCtx* c, RedisModuleString** /*av*/, int /*ac*/) -> int {
      auto* r = static_cast<AsyncAggReply*>(RedisModule_GetBlockedClientPrivateData(c));
      if (!r) return RedisModule_ReplyWithError(c, "ERR async aggregate internal error");
      if (!r->error.empty()) return RedisModule_ReplyWithError(c, r->error.c_str());
      RedisModule_ReplyWithArray(c, static_cast<long>(r->rows.size()));
      for (const auto& row : r->rows) {
        std::vector<std::string> keys;
        keys.reserve(row.size());
        for (const auto& [k, v] : row) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        RedisModule_ReplyWithArray(c, static_cast<long>(keys.size()) * 2);
        for (const auto& k : keys) {
          RedisModule_ReplyWithCString(c, k.c_str());
          RedisModule_ReplyWithCString(c, row.at(k).c_str());
        }
      }
      return REDISMODULE_OK;
    };
    auto TimeoutCb = [](RedisModuleCtx* c, RedisModuleString** /*av*/, int /*ac*/) -> int {
      if (IsOnTimeoutReturn()) {
        RedisModule_ReplyWithArray(c, 0);
        return REDISMODULE_OK;
      }
      return RedisModule_ReplyWithError(c, "ERR query timeout");
    };
    auto FreeCb = [](RedisModuleCtx* /*c*/, void* priv) {
      delete static_cast<AsyncAggReply*>(priv);
    };

    RedisModuleBlockedClient* bc =
        RedisModule_BlockClient(ctx, ReplyCb, TimeoutCb, FreeCb, timeout_ms);

    GetSearchExecutor().Submit(
        [bc, idx_str, parsed = std::move(parsed), qvec = std::move(qvec),
         knn_score_field = std::move(knn_score_field),
         stages = std::move(stages), params = std::move(params),
         qopts = std::move(qopts)]() {
          auto* rep = new AsyncAggReply();
          {
            auto entry_ptr = GetIndexEntryShared(idx_str);
            if (!entry_ptr) {
              rep->error = "ERR index not found";
            } else {
              const auto& e = *entry_ptr;
              std::shared_lock<std::shared_mutex> idx_lk(e.Mutex());
              std::vector<std::string> result_ids;
              std::unordered_map<std::string, std::string> knn_scores;
              if (parsed.has_knn) {
                if (const auto* vi = e.vector_indices.Get(parsed.knn_field)) {
                  std::vector<KnnResult> knn_hits;
                  if (parsed.root.type != QueryNode::Type::kMatchAll) {
                    std::string ferr;
                    auto cands = EvaluateQuery(
                        parsed.root, e.spec, e.doc_store, e.tag_indices,
                        e.numeric_indices, e.text_indices, e.geo_indices, ferr,
                        qopts, &e.geoshape_indices, &params, &e.vector_indices,
                        &e.synonyms);
                    if (!ferr.empty()) {
                      rep->error = ferr;
                    } else {
                      knn_hits = vi->KnnQueryFiltered(qvec.data(), parsed.knn_k, cands,
                                                      parsed.knn_ef_runtime, parsed.knn_oversample);
                    }
                  } else {
                    knn_hits = vi->KnnQueryEf(qvec.data(), parsed.knn_k,
                                              parsed.knn_ef_runtime, parsed.knn_oversample);
                  }
                  if (rep->error.empty()) {
                    result_ids.reserve(knn_hits.size());
                    for (const auto& h : knn_hits) {
                      result_ids.push_back(h.doc_id);
                      knn_scores[h.doc_id] = std::to_string(h.score);
                    }
                  }
                }
              } else {
                std::string eval_error;
                result_ids = EvaluateQuery(
                    parsed.root, e.spec, e.doc_store, e.tag_indices,
                    e.numeric_indices, e.text_indices, e.geo_indices, eval_error,
                    qopts, &e.geoshape_indices, &params, &e.vector_indices,
                    &e.synonyms);
                if (!eval_error.empty()) rep->error = eval_error;
              }
              if (rep->error.empty()) {
                rep->rows.reserve(result_ids.size());
                for (const auto& doc_id : result_ids) {
                  const auto* doc = e.doc_store.Get(doc_id);
                  if (!doc) continue;
                  AggRow r = doc->fields;
                  if (parsed.has_knn) {
                    auto sit = knn_scores.find(doc_id);
                    if (sit != knn_scores.end()) r[knn_score_field] = sit->second;
                  }
                  rep->rows.push_back(std::move(r));
                }
                ExecuteAggPipeline(rep->rows, stages, qopts.deadline_ms, rep->error);
              }
            }
          }
          RedisModule_UnblockClient(bc, rep);
        });
    return REDISMODULE_OK;
  }

  // Synchronous query evaluation
  std::vector<std::string> result_ids;
  std::unordered_map<std::string, std::string> knn_scores;
  if (parsed.has_knn) {
    if (const auto* vidx = entry.vector_indices.Get(parsed.knn_field)) {
      std::vector<KnnResult> knn_hits;
      if (parsed.root.type != QueryNode::Type::kMatchAll) {
        std::string ferr;
        auto cands = EvaluateQuery(parsed.root, entry.spec, entry.doc_store,
                                   entry.tag_indices, entry.numeric_indices,
                                   entry.text_indices, entry.geo_indices, ferr,
                                   qopts, &entry.geoshape_indices, &params,
                                   &entry.vector_indices, &entry.synonyms);
        if (!ferr.empty()) return RedisModule_ReplyWithError(ctx, ferr.c_str());
        knn_hits = vidx->KnnQueryFiltered(qvec.data(), parsed.knn_k, cands,
                                          parsed.knn_ef_runtime, parsed.knn_oversample);
      } else {
        knn_hits = vidx->KnnQueryEf(qvec.data(), parsed.knn_k,
                                    parsed.knn_ef_runtime, parsed.knn_oversample);
      }
      result_ids.reserve(knn_hits.size());
      for (const auto& h : knn_hits) {
        result_ids.push_back(h.doc_id);
        knn_scores[h.doc_id] = std::to_string(h.score);
      }
    }
  } else {
    std::string eval_error;
    result_ids = EvaluateQuery(parsed.root, entry.spec, entry.doc_store,
                               entry.tag_indices, entry.numeric_indices,
                               entry.text_indices, entry.geo_indices, eval_error,
                               qopts, &entry.geoshape_indices, &params,
                               &entry.vector_indices, &entry.synonyms);
    if (!eval_error.empty()) {
      return RedisModule_ReplyWithError(ctx, eval_error.c_str());
    }
  }

  // Build initial rows from query results
  std::vector<AggRow> rows;
  rows.reserve(result_ids.size());
  for (auto& doc_id : result_ids) {
    const auto* doc = entry.doc_store.Get(doc_id);
    if (!doc) continue;
    AggRow r = doc->fields;
    if (parsed.has_knn) {
      auto sit = knn_scores.find(doc_id);
      if (sit != knn_scores.end()) r[knn_score_field] = sit->second;
    }
    rows.push_back(std::move(r));
  }

  // Execute pipeline stages
  std::string pipe_err;
  if (!ExecuteAggPipeline(rows, stages, qopts.deadline_ms, pipe_err)) {
    return RedisModule_ReplyWithError(ctx, pipe_err.c_str());
  }

  // Reply
  if (with_cursor) {
    int64_t now_ms = NowMs();
    uint64_t cid = 0;
    {
      std::lock_guard<std::mutex> clk(GetCursorsMutex());
      PurgeExpiredCursorsLocked(now_ms);
      size_t idx_cursors = 0;
      for (const auto& [id, c] : GetCursors()) {
        if (c.index_name == idx_str) idx_cursors++;
      }
      if (idx_cursors >= kMaxCursorsPerIndex) {
        return RedisModule_ReplyWithError(ctx, "ERR too many active cursors");
      }
      cid = NextCursorId();
      AggCursor cursor;
      cursor.index_name = idx_str;
      cursor.rows = std::move(rows);
      cursor.pos = 0;
      cursor.batch_size = cursor_count;
      cursor.last_access_ms = now_ms;
      cursor.max_idle_ms = cursor_max_idle_ms;
      GetCursors()[cid] = std::move(cursor);
    }
    gemini_search::RpcCursorRequest rreq;
    rreq.op = gemini_search::kRpcCursorOpRead;
    rreq.index_name = idx_str;
    rreq.cursor_id = cid;
    auto bresp = ExecuteLocalCursorOp(rreq);
    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithArray(ctx, static_cast<long>(bresp.rows.size()));
    for (const auto& row : bresp.rows) {
      RedisModule_ReplyWithArray(ctx, static_cast<long>(row.size()) * 2);
      for (const auto& [k, v] : row) {
        RedisModule_ReplyWithStringBuffer(ctx, k.data(), k.size());
        RedisModule_ReplyWithStringBuffer(ctx, v.data(), v.size());
      }
    }
    RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(bresp.next_cursor_id));
  } else {
    RedisModule_ReplyWithArray(ctx, static_cast<long>(rows.size()));
    for (auto& row : rows) {
      std::vector<std::string> keys;
      for (auto& [k, v] : row) keys.push_back(k);
      std::sort(keys.begin(), keys.end());
      RedisModule_ReplyWithArray(ctx, static_cast<long>(keys.size()) * 2);
      for (auto& k : keys) {
        RedisModule_ReplyWithCString(ctx, k.c_str());
        RedisModule_ReplyWithCString(ctx, row.at(k).c_str());
      }
    }
  }

  return REDISMODULE_OK;
}

bool RunAggregateForProfile(
    IndexEntry& entry, const std::vector<std::string>& result_ids,
    RedisModuleString** argv, int start_arg, int argc,
    std::vector<std::unordered_map<std::string, std::string>>& out_rows,
    std::string& error_msg, int64_t deadline_ms) {
  std::vector<PipelineStage> stages;
  PipelineStage* pending_groupby = nullptr;

  int arg_i = start_arg;
  while (arg_i < argc) {
    auto kw = ArgView(argv[arg_i]);
    if (MatchArg(kw, "LOAD")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) { error_msg = "ERR LOAD requires nargs"; return false; }
      char* ep = nullptr;
      long n = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &ep, 10);
      arg_i++;
      if (*ep != '\0' || n < 0 || arg_i + n > argc) {
        error_msg = "ERR LOAD field count mismatch";
        return false;
      }
      arg_i += static_cast<int>(n);
    } else if (MatchArg(kw, "APPLY")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i + 2 >= argc || !MatchArg(ArgView(argv[arg_i + 1]), "AS")) {
        error_msg = "ERR APPLY requires expr AS alias";
        return false;
      }
      PipelineStage st;
      st.type = PipelineStageType::kApply;
      st.apply_expr = std::string(ArgView(argv[arg_i]));
      st.apply_alias = std::string(ArgView(argv[arg_i + 2]));
      stages.push_back(std::move(st));
      arg_i += 3;
    } else if (MatchArg(kw, "FILTER")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) { error_msg = "ERR FILTER requires expr"; return false; }
      PipelineStage st;
      st.type = PipelineStageType::kFilter;
      st.filter_expr = std::string(ArgView(argv[arg_i]));
      stages.push_back(std::move(st));
      arg_i++;
    } else if (MatchArg(kw, "GROUPBY")) {
      arg_i++;
      if (arg_i >= argc) { error_msg = "ERR GROUPBY requires nargs"; return false; }
      char* ep = nullptr;
      long n = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &ep, 10);
      arg_i++;
      if (*ep != '\0' || n < 0 || arg_i + n > argc) {
        error_msg = "ERR GROUPBY field count mismatch";
        return false;
      }
      PipelineStage st;
      st.type = PipelineStageType::kGroupby;
      for (long fi = 0; fi < n; fi++) {
        std::string f(ArgView(argv[arg_i + fi]));
        if (!f.empty() && f[0] == '@') f = f.substr(1);
        st.groupby_fields.push_back(std::move(f));
      }
      arg_i += static_cast<int>(n);
      stages.push_back(std::move(st));
      pending_groupby = &stages.back();
    } else if (MatchArg(kw, "REDUCE") && pending_groupby) {
      arg_i++;
      if (arg_i + 1 >= argc) { error_msg = "ERR REDUCE requires func and nargs"; return false; }
      auto fstr = ArgView(argv[arg_i++]);
      char* ep = nullptr;
      long nargs = std::strtol(std::string(ArgView(argv[arg_i++])).c_str(), &ep, 10);
      if (*ep != '\0' || nargs < 0 || arg_i + nargs > argc) {
        error_msg = "ERR REDUCE nargs mismatch";
        return false;
      }
      ReduceOp rop;
      if (MatchArg(fstr, "COUNT")) rop.func = ReduceFunc::kCount;
      else if (MatchArg(fstr, "SUM")) rop.func = ReduceFunc::kSum;
      else if (MatchArg(fstr, "AVG")) rop.func = ReduceFunc::kAvg;
      else if (MatchArg(fstr, "MIN")) rop.func = ReduceFunc::kMin;
      else if (MatchArg(fstr, "MAX")) rop.func = ReduceFunc::kMax;
      else if (MatchArg(fstr, "COUNT_DISTINCT")) rop.func = ReduceFunc::kCountDistinct;
      else if (MatchArg(fstr, "COUNT_DISTINCTISH")) rop.func = ReduceFunc::kCountDistinctish;
      else if (MatchArg(fstr, "STDDEV")) rop.func = ReduceFunc::kStddev;
      else if (MatchArg(fstr, "TOLIST")) rop.func = ReduceFunc::kTolist;
      else { error_msg = "ERR unknown REDUCE function"; return false; }
      if (nargs >= 1) {
        std::string f(ArgView(argv[arg_i]));
        if (!f.empty() && f[0] == '@') f = f.substr(1);
        rop.field = std::move(f);
      }
      arg_i += static_cast<int>(nargs);
      if (arg_i + 1 < argc && MatchArg(ArgView(argv[arg_i]), "AS")) {
        rop.alias = std::string(ArgView(argv[arg_i + 1]));
        arg_i += 2;
      } else {
        rop.alias = std::string(fstr);
      }
      pending_groupby->reducers.push_back(std::move(rop));
    } else if (MatchArg(kw, "SORTBY")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) { error_msg = "ERR SORTBY requires nargs"; return false; }
      char* ep = nullptr;
      long n = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &ep, 10);
      arg_i++;
      if (*ep != '\0' || n < 1 || arg_i + n > argc) {
        error_msg = "ERR SORTBY args mismatch";
        return false;
      }
      PipelineStage st;
      st.type = PipelineStageType::kSortby;
      int sb_end = arg_i + static_cast<int>(n);
      while (arg_i < sb_end) {
        std::string sf(ArgView(argv[arg_i++]));
        if (!sf.empty() && sf[0] == '@') sf = sf.substr(1);
        bool desc = false;
        if (arg_i < sb_end) {
          auto dir = ArgView(argv[arg_i]);
          if (MatchArg(dir, "DESC")) { desc = true; arg_i++; }
          else if (MatchArg(dir, "ASC")) { desc = false; arg_i++; }
        }
        st.sortby_keys.emplace_back(std::move(sf), desc);
      }
      stages.push_back(std::move(st));
    } else if (MatchArg(kw, "LIMIT")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i + 1 >= argc) { error_msg = "ERR LIMIT requires offset count"; return false; }
      PipelineStage st;
      st.type = PipelineStageType::kLimit;
      st.limit_offset = std::strtoll(std::string(ArgView(argv[arg_i])).c_str(), nullptr, 10);
      st.limit_count = std::strtoll(std::string(ArgView(argv[arg_i + 1])).c_str(), nullptr, 10);
      stages.push_back(std::move(st));
      arg_i += 2;
    } else {
      arg_i++;
    }
  }

  out_rows.clear();
  out_rows.reserve(result_ids.size());
  for (const auto& doc_id : result_ids) {
    if (const auto* doc = entry.doc_store.Get(doc_id)) {
      out_rows.push_back(doc->fields);
    }
  }

  return ExecuteAggPipeline(out_rows, stages, deadline_ms, error_msg);
}
