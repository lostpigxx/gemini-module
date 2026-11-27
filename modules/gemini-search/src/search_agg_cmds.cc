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
    auto it = GetCursors().find(req.cursor_id);
    if (it == GetCursors().end() ||
        (!req.index_name.empty() && it->second.index_name != req.index_name)) {
      resp.ok = false;
      resp.error_msg = "ERR cursor not found";
      return resp;
    }
    GetCursors().erase(it);
    resp.ok = true;
    return resp;
  }

  auto it = GetCursors().find(req.cursor_id);
  if (it == GetCursors().end() ||
      (!req.index_name.empty() && it->second.index_name != req.index_name)) {
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
  TouchIndex(entry);
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
    deadline_ms = SaturatingAddMs(NowMs(), req.remaining_timeout_ms);
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

  // Raw rows mode: optional pushed-down local SORTBY & top-K truncation (or explicit LIMIT ... 0)
  if (req.raw_zero_limit) {
    rows.clear();
  } else if (req.raw_has_sortby && !req.raw_sortby_keys.empty()) {
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

namespace {

struct ParsedAggOptions {
  std::vector<PipelineStage> stages;
  std::unordered_map<std::string, std::string> params;
  int dialect = 2;
  long long timeout_ms = 0;
  bool with_cursor = false;
  size_t cursor_count = 1000;
  int64_t cursor_max_idle_ms = 300000;
};

// Single structured pass parser for FT.AGGREGATE options & pipeline stages.
// Consumes option values properly so PARAMS/APPLY/FILTER/LOAD/SORTBY values are never
// misparsed as top-level keywords (TIMEOUT, DIALECT, WITHCURSOR, COUNT, MAXIDLE).
bool ParseAggPipelineAndOptions(RedisModuleString** argv, int start_arg, int argc,
                                ParsedAggOptions& out, std::string& err_out) {
  PipelineStage* pending_groupby = nullptr;
  int arg_i = start_arg;
  while (arg_i < argc) {
    auto kw = ArgView(argv[arg_i]);

    if (MatchArg(kw, "LOAD")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) { err_out = "ERR LOAD requires count or *"; return false; }
      if (ArgView(argv[arg_i]) == "*") {
        arg_i++;
        PipelineStage st;
        st.type = PipelineStageType::kLoad;
        out.stages.push_back(std::move(st));
        continue;
      }
      long long ld_count = 0;
      if (!ParseAggLong(argv[arg_i++], ld_count) || ld_count < 0) {
        err_out = "ERR LOAD count must be non-negative";
        return false;
      }
      PipelineStage st;
      st.type = PipelineStageType::kLoad;
      for (long long li = 0; li < ld_count; li++) {
        if (arg_i >= argc) { err_out = "ERR not enough LOAD fields"; return false; }
        auto fv = ArgView(argv[arg_i]);
        arg_i++;
        if (fv.empty() || fv[0] != '@') {
          err_out = "ERR LOAD field must start with @";
          return false;
        }
        st.load_fields.emplace_back(fv.substr(1));
      }
      out.stages.push_back(std::move(st));
    } else if (MatchArg(kw, "APPLY")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) { err_out = "ERR APPLY requires expression"; return false; }
      std::string expr(ArgView(argv[arg_i]));
      arg_i++;
      if (arg_i >= argc || !MatchArg(ArgView(argv[arg_i]), "AS")) {
        err_out = "ERR APPLY requires AS alias";
        return false;
      }
      arg_i++;
      if (arg_i >= argc) { err_out = "ERR APPLY AS requires alias name"; return false; }
      std::string alias(ArgView(argv[arg_i]));
      arg_i++;
      PipelineStage st;
      st.type = PipelineStageType::kApply;
      st.apply_expr = std::move(expr);
      st.apply_alias = std::move(alias);
      out.stages.push_back(std::move(st));
    } else if (MatchArg(kw, "FILTER")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) { err_out = "ERR FILTER requires expression"; return false; }
      PipelineStage st;
      st.type = PipelineStageType::kFilter;
      st.filter_expr = std::string(ArgView(argv[arg_i]));
      arg_i++;
      out.stages.push_back(std::move(st));
    } else if (MatchArg(kw, "GROUPBY")) {
      arg_i++;
      if (arg_i >= argc) { err_out = "ERR GROUPBY requires count"; return false; }
      long long gb_count = 0;
      if (!ParseAggLong(argv[arg_i++], gb_count) || gb_count < 0) {
        err_out = "ERR GROUPBY count must be non-negative";
        return false;
      }
      PipelineStage st;
      st.type = PipelineStageType::kGroupby;
      for (long long g = 0; g < gb_count; g++) {
        if (arg_i >= argc) { err_out = "ERR not enough GROUPBY fields"; return false; }
        auto fv = ArgView(argv[arg_i]);
        arg_i++;
        if (fv.empty() || fv[0] != '@') {
          err_out = "ERR GROUPBY field must start with @";
          return false;
        }
        st.groupby_fields.emplace_back(fv.substr(1));
      }
      out.stages.push_back(std::move(st));
      pending_groupby = &out.stages.back();
    } else if (MatchArg(kw, "REDUCE")) {
      arg_i++;
      if (arg_i >= argc) { err_out = "ERR REDUCE requires function name"; return false; }
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
        err_out = "ERR unknown REDUCE function";
        return false;
      }

      if (arg_i >= argc) { err_out = "ERR REDUCE requires nargs"; return false; }
      long long nargs = 0;
      if (!ParseAggLong(argv[arg_i++], nargs) || nargs < 0) {
        err_out = "ERR REDUCE nargs must be non-negative";
        return false;
      }

      long long consumed = 0;
      if (needs_field && nargs >= 1) {
        if (arg_i >= argc) { err_out = "ERR REDUCE missing field argument"; return false; }
        auto rf = ArgView(argv[arg_i]);
        arg_i++; consumed++;
        if (rf.empty() || rf[0] != '@') {
          err_out = "ERR REDUCE field must start with @";
          return false;
        }
        rop.field = std::string(rf.substr(1));
      }

      if (rop.func == ReduceFunc::kQuantile && consumed < nargs) {
        if (arg_i >= argc) { err_out = "ERR QUANTILE requires percentile"; return false; }
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
        if (arg_i >= argc) { err_out = "ERR RANDOM_SAMPLE requires count"; return false; }
        long long rsc = 0;
        ParseAggLong(argv[arg_i++], rsc);
        rop.random_sample_count = static_cast<long>(rsc);
        consumed++;
      }

      for (long long skip = consumed; skip < nargs; skip++) {
        if (arg_i < argc) arg_i++;
      }

      if (arg_i >= argc || !MatchArg(ArgView(argv[arg_i]), "AS")) {
        err_out = "ERR REDUCE requires AS alias";
        return false;
      }
      arg_i++;
      if (arg_i >= argc) { err_out = "ERR REDUCE AS requires alias name"; return false; }
      rop.alias = std::string(ArgView(argv[arg_i]));
      arg_i++;

      if (pending_groupby) {
        pending_groupby->reducers.push_back(std::move(rop));
      } else {
        PipelineStage st;
        st.type = PipelineStageType::kGroupby;
        st.reducers.push_back(std::move(rop));
        out.stages.push_back(std::move(st));
        pending_groupby = &out.stages.back();
      }
    } else if (MatchArg(kw, "SORTBY")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) { err_out = "ERR SORTBY requires count"; return false; }
      long long sb_count = 0;
      if (!ParseAggLong(argv[arg_i++], sb_count) || sb_count < 1 || arg_i + sb_count > argc) {
        err_out = "ERR SORTBY count invalid";
        return false;
      }
      PipelineStage st;
      st.type = PipelineStageType::kSortby;
      int sb_end = arg_i + static_cast<int>(sb_count);
      while (arg_i < sb_end) {
        auto sf = ArgView(argv[arg_i++]);
        if (sf.empty() || sf[0] != '@') {
          err_out = "ERR SORTBY field must start with @";
          return false;
        }
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
        if (!ParseAggLong(argv[arg_i + 1], m) || m < 0) {
          err_out = "ERR SORTBY MAX must be a non-negative integer";
          return false;
        }
        st.sortby_max = m;
        arg_i += 2;
      }
      out.stages.push_back(std::move(st));
    } else if (MatchArg(kw, "PARAMS")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) { err_out = "ERR PARAMS requires count"; return false; }
      long long pcount = 0;
      if (!ParseAggLong(argv[arg_i++], pcount) || pcount < 0 || pcount % 2 != 0 ||
          arg_i + pcount > argc) {
        err_out = "ERR invalid PARAMS specification";
        return false;
      }
      for (long long p = 0; p < pcount; p += 2) {
        std::string pname(ArgView(argv[arg_i + p]));
        std::string pval = ArgStr(argv[arg_i + p + 1]);
        out.params[std::move(pname)] = std::move(pval);
      }
      arg_i += static_cast<int>(pcount);
    } else if (MatchArg(kw, "LIMIT")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i + 1 >= argc) { err_out = "ERR LIMIT requires offset and count"; return false; }
      long long off = 0, cnt = 0;
      if (!ParseAggLong(argv[arg_i], off) || !ParseAggLong(argv[arg_i + 1], cnt) ||
          off < 0 || cnt < 0) {
        err_out = "ERR LIMIT offset and count must be non-negative";
        return false;
      }
      PipelineStage st;
      st.type = PipelineStageType::kLimit;
      st.limit_offset = off;
      st.limit_count = cnt;
      out.stages.push_back(std::move(st));
      arg_i += 2;
    } else if (MatchArg(kw, "WITHCURSOR")) {
      pending_groupby = nullptr;
      out.with_cursor = true;
      arg_i++;
    } else if (MatchArg(kw, "COUNT")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) { err_out = "ERR COUNT requires a value"; return false; }
      long long cv = 0;
      if (ParseAggLong(argv[arg_i], cv) && cv > 0) out.cursor_count = static_cast<size_t>(cv);
      arg_i++;
    } else if (MatchArg(kw, "MAXIDLE")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) { err_out = "ERR MAXIDLE requires a value"; return false; }
      long long mv = 0;
      if (!ParseAggLong(argv[arg_i], mv) || mv < 0) {
        err_out = "ERR MAXIDLE must be a non-negative integer";
        return false;
      }
      out.cursor_max_idle_ms = static_cast<int64_t>(mv);
      arg_i++;
    } else if (MatchArg(kw, "DIALECT")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) { err_out = "ERR option requires a value"; return false; }
      long long d = 0;
      if (ParseAggLong(argv[arg_i], d) && d >= 1) out.dialect = static_cast<int>(d);
      arg_i++;
    } else if (MatchArg(kw, "TIMEOUT")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) { err_out = "ERR option requires a value"; return false; }
      long long tms = 0;
      if (ParseAggLong(argv[arg_i], tms) && tms > 0) out.timeout_ms = tms;
      arg_i++;
    } else if (MatchArg(kw, "SCORER")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc) { err_out = "ERR option requires a value"; return false; }
      arg_i++;
    } else if (MatchArg(kw, "VERBATIM") || MatchArg(kw, "ADDSCORES")) {
      pending_groupby = nullptr;
      arg_i++;
    } else {
      err_out = "ERR unknown AGGREGATE option";
      return false;
    }
  }
  return true;
}

struct AsyncAggContext {
  RedisModuleBlockedClient* bc = nullptr;
  std::mutex mu;
  bool cancelled = false;
  std::string error;
  std::vector<AggRow> rows;
};

BlockedClientRegistry<AsyncAggContext> g_active_async_aggs;

void AsyncAggDisconnectCb(RedisModuleCtx* /*ctx*/, RedisModuleBlockedClient* bc) {
  g_active_async_aggs.OnDisconnect(bc);
}

int AsyncAggReplyCb(RedisModuleCtx* c, RedisModuleString** /*av*/, int /*ac*/) {
  auto* holder = static_cast<std::shared_ptr<AsyncAggContext>*>(
      RedisModule_GetBlockedClientPrivateData(c));
  if (!holder || !(*holder)) return RedisModule_ReplyWithError(c, "ERR async aggregate internal error");
  auto& r = **holder;
  std::lock_guard<std::mutex> lk(r.mu);
  if (!r.error.empty()) {
    if (r.error == "ERR query timeout" && IsOnTimeoutReturn()) {
      RedisModule_ReplyWithArray(c, 0);
      return REDISMODULE_OK;
    }
    return RedisModule_ReplyWithError(c, r.error.c_str());
  }
  RedisModule_ReplyWithArray(c, static_cast<long>(r.rows.size()));
  for (const auto& row : r.rows) {
    std::vector<std::string> keys;
    keys.reserve(row.size());
    for (const auto& [k, v] : row) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    RedisModule_ReplyWithArray(c, static_cast<long>(keys.size()) * 2);
    for (const auto& k : keys) {
      RedisModule_ReplyWithStringBuffer(c, k.data(), k.size());
      const std::string& val = row.at(k);
      RedisModule_ReplyWithStringBuffer(c, val.data(), val.size());
    }
  }
  return REDISMODULE_OK;
}

int AsyncAggTimeoutCb(RedisModuleCtx* c, RedisModuleString** /*av*/, int /*ac*/) {
  if (IsOnTimeoutReturn()) {
    RedisModule_ReplyWithArray(c, 0);
    return REDISMODULE_OK;
  }
  return RedisModule_ReplyWithError(c, "ERR query timeout");
}

void AsyncAggFreeCb(RedisModuleCtx* /*c*/, void* priv) {
  delete static_cast<std::shared_ptr<AsyncAggContext>*>(priv);
}

}  // namespace

bool ParseAggArgsForProfile(
    RedisModuleString** argv, int start_arg, int argc,
    std::vector<gemini_search::PipelineStage>& stages_out,
    std::unordered_map<std::string, std::string>& params_out,
    int& dialect_inout, long long& timeout_ms_out, std::string& error_out) {
  ParsedAggOptions opts;
  opts.dialect = dialect_inout;
  if (!ParseAggPipelineAndOptions(argv, start_arg, argc, opts, error_out)) {
    return false;
  }
  stages_out = std::move(opts.stages);
  params_out = std::move(opts.params);
  dialect_inout = opts.dialect;
  timeout_ms_out = opts.timeout_ms;
  return true;
}

int FtAggregateCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                              int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);
  RedisModule_AutoMemory(ctx);

  std::string idx_str = ResolveAlias(std::string(ArgView(argv[1])));
  auto entry_ptr = GetIndexEntryShared(idx_str);
  if (!entry_ptr) {
    return RedisModule_ReplyWithError(ctx, "ERR index not found");
  }
  auto& entry = *entry_ptr;
  TouchIndex(entry);

  // Parse ALL options and pipeline stages in one structured pass (never misparses values as keywords)
  ParsedAggOptions parsed_opts;
  parsed_opts.dialect = GetDefaultDialect();
  std::string opt_err;
  if (!ParseAggPipelineAndOptions(argv, 3, argc, parsed_opts, opt_err)) {
    return RedisModule_ReplyWithError(ctx, opt_err.c_str());
  }

  QueryOptions qopts;
  std::unordered_set<std::string> custom_dict_copy;
  {
    std::shared_lock<std::shared_mutex> idx_lk(entry.Mutex());
    qopts.language = entry.spec.language;
    qopts.dialect = parsed_opts.dialect;
    if (!entry.spec.custom_dict.empty()) {
      std::shared_lock<std::shared_mutex> dlk(GetTermDictsMutex());
      auto dit = GetTermDicts().find(entry.spec.custom_dict);
      if (dit != GetTermDicts().end()) {
        custom_dict_copy = dit->second.Terms();
        qopts.custom_dict = &custom_dict_copy;
      }
    }
  }
  long long timeout_ms = parsed_opts.timeout_ms;
  if (timeout_ms > 0) {
    qopts.deadline_ms = SaturatingAddMs(NowMs(), static_cast<int64_t>(timeout_ms));
  }
  auto& params = parsed_opts.params;
  auto& stages = parsed_opts.stages;
  bool with_cursor = parsed_opts.with_cursor;
  size_t cursor_count = parsed_opts.cursor_count;
  int64_t cursor_max_idle_ms = parsed_opts.cursor_max_idle_ms;

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
    std::shared_lock<std::shared_mutex> idx_lk(entry.Mutex());
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
    mreq.raw_zero_limit = split.raw_zero_limit;
    mreq.raw_has_sortby = split.raw_has_sortby;
    mreq.raw_sortby_keys = std::move(split.raw_sortby_keys);

    return gemini_search::ExecuteClusterAggregate(ctx, std::move(aplan));
  }

  // Phase 53: Non-blocking async offload for non-cursor FT.AGGREGATE (race-free BlockedClientRegistry)
  size_t async_min = GetAsyncMinDocs();
  size_t doc_count = 0;
  {
    std::shared_lock<std::shared_mutex> idx_lk(entry.Mutex());
    doc_count = entry.doc_store.Size();
  }
  if (!with_cursor && RedisModule_BlockClient && RedisModule_UnblockClient &&
      async_min > 0 && doc_count >= async_min) {
    auto actx = std::make_shared<AsyncAggContext>();
    // Pass timeout=0 because worker enforces qopts.deadline_ms and unblocks via BlockedClientRegistry
    RedisModuleBlockedClient* bc =
        RedisModule_BlockClient(ctx, AsyncAggReplyCb, AsyncAggTimeoutCb, AsyncAggFreeCb, 0);
    if (!bc) return RedisModule_ReplyWithError(ctx, "ERR failed to block client for async aggregate");
    actx->bc = bc;
    g_active_async_aggs.Register(bc, actx);
    if (RedisModule_SetDisconnectCallback) {
      RedisModule_SetDisconnectCallback(bc, AsyncAggDisconnectCb);
    }

    GetSearchExecutor().Submit(
        [actx, idx_str, parsed = std::move(parsed), qvec = std::move(qvec),
         knn_score_field = std::move(knn_score_field),
         stages = std::move(stages), params = std::move(params),
         qopts = std::move(qopts), custom_dict_copy = std::move(custom_dict_copy)]() mutable {
          if (!custom_dict_copy.empty()) qopts.custom_dict = &custom_dict_copy;
          std::string err;
          std::vector<AggRow> out_rows;
          {
            auto eptr = GetIndexEntryShared(idx_str);
            if (!eptr) {
              err = "ERR index not found";
            } else {
              const auto& e = *eptr;
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
                      err = ferr;
                    } else {
                      knn_hits = vi->KnnQueryFiltered(qvec.data(), parsed.knn_k, cands,
                                                      parsed.knn_ef_runtime, parsed.knn_oversample);
                    }
                  } else {
                    knn_hits = vi->KnnQueryEf(qvec.data(), parsed.knn_k,
                                              parsed.knn_ef_runtime, parsed.knn_oversample);
                  }
                  if (err.empty()) {
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
                if (!eval_error.empty()) err = eval_error;
              }
              if (err.empty()) {
                out_rows.reserve(result_ids.size());
                for (const auto& doc_id : result_ids) {
                  const auto* doc = e.doc_store.Get(doc_id);
                  if (!doc) continue;
                  AggRow r = doc->fields;
                  if (parsed.has_knn) {
                    auto sit = knn_scores.find(doc_id);
                    if (sit != knn_scores.end()) r[knn_score_field] = sit->second;
                  }
                  out_rows.push_back(std::move(r));
                }
                ExecuteAggPipeline(out_rows, stages, qopts.deadline_ms, err);
              }
            }
          }
          RedisModuleBlockedClient* to_unblock = nullptr;
          {
            std::lock_guard<std::mutex> lk(actx->mu);
            if (!actx->cancelled) {
              actx->error = std::move(err);
              actx->rows = std::move(out_rows);
              to_unblock = actx->bc;
              actx->bc = nullptr;
            }
          }
          g_active_async_aggs.Unblock(actx, to_unblock);
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
  ParsedAggOptions opts;
  if (!ParseAggPipelineAndOptions(argv, start_arg, argc, opts, error_msg)) {
    return false;
  }

  out_rows.clear();
  out_rows.reserve(result_ids.size());
  for (const auto& doc_id : result_ids) {
    if (const auto* doc = entry.doc_store.Get(doc_id)) {
      out_rows.push_back(doc->fields);
    }
  }

  return ExecuteAggPipeline(out_rows, opts.stages, deadline_ms, error_msg);
}
