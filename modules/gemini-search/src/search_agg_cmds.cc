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
      RedisModule_ReplyWithCString(ctx, k.c_str());
      RedisModule_ReplyWithCString(ctx, row.at(k).c_str());
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

  int64_t now_ms = NowMs();
  PurgeExpiredCursors(now_ms);

  auto sub = ArgView(argv[1]);

  if (MatchArg(sub, "READ")) {
    if (argc < 4) return RedisModule_WrongArity(ctx);
    char* ep = nullptr;
    uint64_t cid = static_cast<uint64_t>(
        std::strtoull(std::string(ArgView(argv[3])).c_str(), &ep, 10));
    if (*ep != '\0') return RedisModule_ReplyWithError(ctx, "ERR invalid cursor id");

    auto it = GetCursors().find(cid);
    if (it == GetCursors().end()) {
      return RedisModule_ReplyWithError(ctx, "ERR cursor not found");
    }

    it->second.last_access_ms = now_ms;
    if (argc >= 6 && MatchArg(ArgView(argv[4]), "COUNT")) {
      char* ep2 = nullptr;
      long cnt = std::strtol(std::string(ArgView(argv[5])).c_str(), &ep2, 10);
      if (*ep2 == '\0' && cnt > 0)
        it->second.batch_size = static_cast<size_t>(cnt);
    }

    ReplyCursorBatch(ctx, cid, it->second);
    return REDISMODULE_OK;
  }

  if (MatchArg(sub, "DEL")) {
    if (argc < 4) return RedisModule_WrongArity(ctx);
    char* ep = nullptr;
    uint64_t cid = static_cast<uint64_t>(
        std::strtoull(std::string(ArgView(argv[3])).c_str(), &ep, 10));
    if (*ep != '\0') return RedisModule_ReplyWithError(ctx, "ERR invalid cursor id");

    if (GetCursors().erase(cid) == 0) {
      return RedisModule_ReplyWithError(ctx, "ERR cursor not found");
    }
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
  }

  return RedisModule_ReplyWithError(ctx, "ERR unknown CURSOR subcommand, expected READ or DEL");
}


// =============================================================
// FT.AGGREGATE pipeline
// =============================================================

enum class ReduceFunc {
  kCount, kSum, kAvg, kMin, kMax, kCountDistinct,
  kStddev, kQuantile, kTolist, kFirstValue, kRandomSample, kCountDistinctish
};

struct ReduceOp {
  ReduceFunc func;
  std::string field;
  std::string alias;
  double quantile_pct = 0.0;
  std::string first_value_by;
  bool first_value_desc = false;
  long random_sample_count = 0;
};

enum class PipelineStageType {
  kLoad, kApply, kFilter, kGroupby, kSortby, kLimit
};

struct PipelineStage {
  PipelineStageType type;
  // LOAD
  std::vector<std::string> load_fields;
  // APPLY
  std::string apply_expr;
  std::string apply_alias;
  // FILTER
  std::string filter_expr;
  // GROUPBY + REDUCE
  std::vector<std::string> groupby_fields;
  std::vector<ReduceOp> reducers;
  // SORTBY
  std::string sortby_field;
  bool sortby_desc = false;
  // LIMIT
  long long limit_offset = 0;
  long long limit_count = 0;
};

using AggRow = std::unordered_map<std::string, std::string>;

static void ApplyLimit(std::vector<AggRow>& rows, long long offset, long long count) {
  long long total = static_cast<long long>(rows.size());
  if (offset >= total) { rows.clear(); return; }
  auto begin = rows.begin() + offset;
  auto end = (count >= 0 && offset + count < total)
                 ? rows.begin() + offset + count : rows.end();
  rows = std::vector<AggRow>(begin, end);
}

static void ApplySortby(std::vector<AggRow>& rows,
                        const std::string& field, bool desc) {
  std::sort(rows.begin(), rows.end(),
            [&](const AggRow& a, const AggRow& b) {
              auto ait = a.find(field);
              auto bit = b.find(field);
              std::string va = (ait != a.end()) ? ait->second : "";
              std::string vb = (bit != b.end()) ? bit->second : "";
              double na = 0, nb = 0;
              bool num = TryParseDouble(va, na) && TryParseDouble(vb, nb);
              if (num) return desc ? na > nb : na < nb;
              return desc ? va > vb : va < vb;
            });
}

static void ApplyGroupby(std::vector<AggRow>& rows,
                          const std::vector<std::string>& gb_fields,
                          const std::vector<ReduceOp>& reducers) {
  struct GroupAccum {
    long long count = 0;
    std::unordered_map<std::string, double> sum;
    std::unordered_map<std::string, double> sum_sq;
    std::unordered_map<std::string, long long> field_count;
    std::unordered_map<std::string, double> min_val;
    std::unordered_map<std::string, double> max_val;
    std::unordered_map<std::string, std::unordered_set<std::string>> distinct;
    std::unordered_map<std::string, std::vector<double>> values;
    std::unordered_map<std::string, std::vector<std::string>> str_values;
    std::unordered_map<std::string, std::string> first_val;
    std::unordered_map<std::string, double> first_sort_val;
    std::unordered_map<std::string, bool> first_set;
  };

  auto MakeKey = [&](const AggRow& row) -> std::string {
    std::string key;
    for (size_t i = 0; i < gb_fields.size(); i++) {
      if (i > 0) key += "\x01";
      auto fit = row.find(gb_fields[i]);
      if (fit != row.end()) key += fit->second;
    }
    return key;
  };

  std::unordered_map<std::string, GroupAccum> groups;
  std::unordered_map<std::string, AggRow> group_key_values;

  for (auto& row : rows) {
    std::string gk = MakeKey(row);
    auto& accum = groups[gk];
    accum.count++;

    if (group_key_values.find(gk) == group_key_values.end()) {
      AggRow kv;
      for (auto& gf : gb_fields) {
        auto fit = row.find(gf);
        kv[gf] = (fit != row.end()) ? fit->second : "";
      }
      group_key_values[gk] = std::move(kv);
    }

    for (auto& r : reducers) {
      if (r.func == ReduceFunc::kCount) continue;

      auto fit = row.find(r.field);
      if (fit == row.end()) continue;
      const auto& fval = fit->second;

      if (r.func == ReduceFunc::kCountDistinct || r.func == ReduceFunc::kCountDistinctish) {
        accum.distinct[r.alias].insert(fval);
        continue;
      }
      if (r.func == ReduceFunc::kTolist) {
        accum.str_values[r.alias].push_back(fval);
        continue;
      }
      if (r.func == ReduceFunc::kFirstValue) {
        if (!r.first_value_by.empty()) {
          auto sort_it = row.find(r.first_value_by);
          double sv = 0;
          if (sort_it != row.end()) TryParseDouble(sort_it->second, sv);
          if (!accum.first_set[r.alias] ||
              (r.first_value_desc ? sv > accum.first_sort_val[r.alias]
                                  : sv < accum.first_sort_val[r.alias])) {
            accum.first_val[r.alias] = fval;
            accum.first_sort_val[r.alias] = sv;
            accum.first_set[r.alias] = true;
          }
        } else if (!accum.first_set[r.alias]) {
          accum.first_val[r.alias] = fval;
          accum.first_set[r.alias] = true;
        }
        continue;
      }
      if (r.func == ReduceFunc::kRandomSample) {
        accum.str_values[r.alias].push_back(fval);
        continue;
      }

      double val = 0;
      if (!TryParseDouble(fval, val)) continue;

      accum.sum[r.alias] += val;
      accum.sum_sq[r.alias] += val * val;
      accum.field_count[r.alias]++;

      if (r.func == ReduceFunc::kQuantile) {
        accum.values[r.alias].push_back(val);
      }

      auto min_it = accum.min_val.find(r.alias);
      if (min_it == accum.min_val.end() || val < min_it->second)
        accum.min_val[r.alias] = val;
      auto max_it = accum.max_val.find(r.alias);
      if (max_it == accum.max_val.end() || val > max_it->second)
        accum.max_val[r.alias] = val;
    }
  }

  std::vector<AggRow> result;
  result.reserve(groups.size());
  for (auto& [gk, accum] : groups) {
    AggRow row = group_key_values[gk];
    for (auto& r : reducers) {
      switch (r.func) {
        case ReduceFunc::kCount:
          row[r.alias] = std::to_string(accum.count);
          break;
        case ReduceFunc::kSum: {
          auto sit = accum.sum.find(r.alias);
          row[r.alias] = (sit != accum.sum.end()) ? std::to_string(sit->second) : "0";
          break;
        }
        case ReduceFunc::kAvg: {
          auto sit = accum.sum.find(r.alias);
          auto cit = accum.field_count.find(r.alias);
          if (sit != accum.sum.end() && cit != accum.field_count.end() && cit->second > 0)
            row[r.alias] = std::to_string(sit->second / static_cast<double>(cit->second));
          else
            row[r.alias] = "0";
          break;
        }
        case ReduceFunc::kMin: {
          auto mit = accum.min_val.find(r.alias);
          row[r.alias] = (mit != accum.min_val.end()) ? std::to_string(mit->second) : "0";
          break;
        }
        case ReduceFunc::kMax: {
          auto mit = accum.max_val.find(r.alias);
          row[r.alias] = (mit != accum.max_val.end()) ? std::to_string(mit->second) : "0";
          break;
        }
        case ReduceFunc::kCountDistinct:
        case ReduceFunc::kCountDistinctish: {
          auto dit = accum.distinct.find(r.alias);
          long long dc = (dit != accum.distinct.end()) ? static_cast<long long>(dit->second.size()) : 0;
          row[r.alias] = std::to_string(dc);
          break;
        }
        case ReduceFunc::kStddev: {
          auto sit = accum.sum.find(r.alias);
          auto sqit = accum.sum_sq.find(r.alias);
          auto cit = accum.field_count.find(r.alias);
          if (sit != accum.sum.end() && sqit != accum.sum_sq.end() &&
              cit != accum.field_count.end() && cit->second > 1) {
            double n = static_cast<double>(cit->second);
            double mean = sit->second / n;
            double variance = (sqit->second / n) - (mean * mean);
            if (variance < 0) variance = 0;
            row[r.alias] = std::to_string(std::sqrt(variance));
          } else {
            row[r.alias] = "0";
          }
          break;
        }
        case ReduceFunc::kQuantile: {
          auto vit = accum.values.find(r.alias);
          if (vit != accum.values.end() && !vit->second.empty()) {
            auto& vals = vit->second;
            std::sort(vals.begin(), vals.end());
            size_t idx = static_cast<size_t>(r.quantile_pct * static_cast<double>(vals.size() - 1));
            if (idx >= vals.size()) idx = vals.size() - 1;
            row[r.alias] = std::to_string(vals[idx]);
          } else {
            row[r.alias] = "0";
          }
          break;
        }
        case ReduceFunc::kTolist: {
          auto lit = accum.str_values.find(r.alias);
          if (lit != accum.str_values.end()) {
            std::string joined;
            for (size_t i = 0; i < lit->second.size(); i++) {
              if (i > 0) joined += ",";
              joined += lit->second[i];
            }
            row[r.alias] = std::move(joined);
          } else {
            row[r.alias] = "";
          }
          break;
        }
        case ReduceFunc::kFirstValue: {
          auto fit = accum.first_val.find(r.alias);
          row[r.alias] = (fit != accum.first_val.end()) ? fit->second : "";
          break;
        }
        case ReduceFunc::kRandomSample: {
          auto lit = accum.str_values.find(r.alias);
          if (lit != accum.str_values.end() && !lit->second.empty()) {
            long count = r.random_sample_count;
            auto& vals = lit->second;
            // Deterministic sampling: take evenly spaced elements
            std::string sampled;
            for (long si = 0; si < count && si < static_cast<long>(vals.size()); si++) {
              size_t idx = static_cast<size_t>(
                  si * static_cast<long>(vals.size()) / count);
              if (si > 0) sampled += ",";
              sampled += vals[idx];
            }
            row[r.alias] = std::move(sampled);
          } else {
            row[r.alias] = "";
          }
          break;
        }
      }
    }
    result.push_back(std::move(row));
  }
  rows = std::move(result);
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
  auto& entry = it->second;
  TouchIndex(entry);

  // Pre-scan for DIALECT and TIMEOUT options
  QueryOptions qopts;
  qopts.language = entry.spec.language;
  qopts.dialect = GetDefaultDialect();
  for (int ci = 3; ci < argc; ci++) {
    auto kw = ArgView(argv[ci]);
    if (MatchArg(kw, "DIALECT") && ci + 1 < argc) {
      qopts.dialect = static_cast<int>(std::strtol(std::string(ArgView(argv[ci + 1])).c_str(), nullptr, 10));
      ci++;
    } else if (MatchArg(kw, "TIMEOUT") && ci + 1 < argc) {
      long long tms = std::strtoll(std::string(ArgView(argv[ci + 1])).c_str(), nullptr, 10);
      if (tms > 0) qopts.deadline_ms = NowMs() + tms;
      ci++;
    }
  }

  ParsedQuery parsed;
  std::string parse_error;
  if (!ParseQuery(std::string(ArgView(argv[2])), parsed, parse_error, qopts)) {
    return RedisModule_ReplyWithError(ctx, parse_error.c_str());
  }

  std::string eval_error;
  auto result_ids =
      EvaluateQuery(parsed.root, entry.spec, entry.doc_store,
                    entry.tag_indices, entry.numeric_indices,
                    entry.text_indices, entry.geo_indices, eval_error,
                    qopts, &entry.geoshape_indices, nullptr,
                    &entry.vector_indices, &entry.synonyms);
  if (!eval_error.empty()) {
    return RedisModule_ReplyWithError(ctx, eval_error.c_str());
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
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR LOAD requires count");
      char* endptr = nullptr;
      long ld_count = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || ld_count < 0)
        return RedisModule_ReplyWithError(ctx, "ERR LOAD count must be non-negative");
      PipelineStage st;
      st.type = PipelineStageType::kLoad;
      for (long li = 0; li < ld_count; li++) {
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
      char* endptr = nullptr;
      long gb_count = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || gb_count < 0)
        return RedisModule_ReplyWithError(ctx, "ERR GROUPBY count must be non-negative");
      PipelineStage st;
      st.type = PipelineStageType::kGroupby;
      for (long g = 0; g < gb_count; g++) {
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
      char* endptr = nullptr;
      long nargs = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || nargs < 0)
        return RedisModule_ReplyWithError(ctx, "ERR REDUCE nargs must be non-negative");

      long consumed = 0;
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
        char* ep = nullptr;
        rop.quantile_pct = std::strtod(std::string(ArgView(argv[arg_i])).c_str(), &ep);
        arg_i++; consumed++;
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
        char* ep = nullptr;
        rop.random_sample_count = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &ep, 10);
        arg_i++; consumed++;
      }

      // Skip any remaining args
      for (long skip = consumed; skip < nargs; skip++) {
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
      char* endptr = nullptr;
      long sb_count = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), &endptr, 10);
      arg_i++;
      if (*endptr != '\0' || sb_count < 2)
        return RedisModule_ReplyWithError(ctx, "ERR SORTBY count must be >= 2");
      if (arg_i >= argc) return RedisModule_ReplyWithError(ctx, "ERR SORTBY requires field");
      auto sf = ArgView(argv[arg_i]);
      arg_i++;
      if (sf.empty() || sf[0] != '@')
        return RedisModule_ReplyWithError(ctx, "ERR SORTBY field must start with @");
      PipelineStage st;
      st.type = PipelineStageType::kSortby;
      st.sortby_field = std::string(sf.substr(1));
      if (arg_i < argc) {
        auto dir = ArgView(argv[arg_i]);
        if (MatchArg(dir, "ASC")) { st.sortby_desc = false; arg_i++; }
        else if (MatchArg(dir, "DESC")) { st.sortby_desc = true; arg_i++; }
      }
      for (long skip = 2; skip < sb_count; skip++) {
        if (arg_i < argc) arg_i++;
      }
      stages.push_back(std::move(st));
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
    } else if (MatchArg(kw, "DIALECT") || MatchArg(kw, "TIMEOUT")) {
      pending_groupby = nullptr;
      arg_i++;
      if (arg_i >= argc)
        return RedisModule_ReplyWithError(ctx, "ERR option requires a value");
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
      char* ep = nullptr;
      long cv = std::strtol(std::string(ArgView(argv[ci + 1])).c_str(), &ep, 10);
      if (*ep == '\0' && cv > 0) cursor_count = static_cast<size_t>(cv);
    } else if (MatchArg(ArgView(argv[ci]), "MAXIDLE") && ci + 1 < argc) {
      char* ep = nullptr;
      long long mv = std::strtoll(std::string(ArgView(argv[ci + 1])).c_str(), &ep, 10);
      if (*ep != '\0' || mv < 0) {
        return RedisModule_ReplyWithError(ctx, "ERR MAXIDLE must be a non-negative integer");
      }
      cursor_max_idle_ms = static_cast<int64_t>(mv);
    }
  }

  // Build initial rows from query results
  std::vector<AggRow> rows;
  rows.reserve(result_ids.size());
  for (auto& doc_id : result_ids) {
    const auto* doc = entry.doc_store.Get(doc_id);
    if (!doc) continue;
    rows.push_back(doc->fields);
  }

  // Execute pipeline stages
  for (auto& stage : stages) {
    if (qopts.deadline_ms > 0 && NowMs() > qopts.deadline_ms) {
      return RedisModule_ReplyWithError(ctx, "ERR query timeout");
    }
    switch (stage.type) {
      case PipelineStageType::kLoad:
        // LOAD adds fields from document store (for rows that still reference docs)
        // In our model, rows already contain all doc fields from initial load
        // LOAD is a no-op for additional fields already in the row
        break;

      case PipelineStageType::kApply: {
        ExprNode expr;
        std::string expr_err;
        if (!ParseExpr(stage.apply_expr, expr, expr_err)) {
          return RedisModule_ReplyWithError(ctx, expr_err.c_str());
        }
        for (auto& row : rows) {
          auto val = EvalExpr(expr, row);
          row[stage.apply_alias] = val.AsString();
        }
        break;
      }

      case PipelineStageType::kFilter: {
        ExprNode expr;
        std::string expr_err;
        if (!ParseExpr(stage.filter_expr, expr, expr_err)) {
          return RedisModule_ReplyWithError(ctx, expr_err.c_str());
        }
        rows.erase(
            std::remove_if(rows.begin(), rows.end(),
                           [&](const AggRow& row) {
                             return !EvalExpr(expr, row).Truthy();
                           }),
            rows.end());
        break;
      }

      case PipelineStageType::kGroupby:
        if (stage.groupby_fields.empty() && stage.reducers.empty()) break;
        ApplyGroupby(rows, stage.groupby_fields, stage.reducers);
        break;

      case PipelineStageType::kSortby:
        ApplySortby(rows, stage.sortby_field, stage.sortby_desc);
        break;

      case PipelineStageType::kLimit:
        ApplyLimit(rows, stage.limit_offset, stage.limit_count);
        break;
    }
  }

  // Reply
  if (with_cursor) {
    int64_t now_ms = NowMs();
    PurgeExpiredCursors(now_ms);
    size_t idx_cursors = 0;
    for (const auto& [id, c] : GetCursors()) {
      if (c.index_name == idx_str) idx_cursors++;
    }
    if (idx_cursors >= kMaxCursorsPerIndex) {
      return RedisModule_ReplyWithError(ctx, "ERR too many active cursors");
    }
    uint64_t cid = NextCursorId();
    AggCursor cursor;
    cursor.index_name = idx_str;
    cursor.rows = std::move(rows);
    cursor.pos = 0;
    cursor.batch_size = cursor_count;
    cursor.last_access_ms = now_ms;
    cursor.max_idle_ms = cursor_max_idle_ms;
    GetCursors()[cid] = std::move(cursor);
    ReplyCursorBatch(ctx, cid, GetCursors()[cid]);
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
      long n = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), nullptr, 10);
      arg_i++;
      if (arg_i + n > argc) { error_msg = "ERR LOAD field count mismatch"; return false; }
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
      long n = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), nullptr, 10);
      arg_i++;
      if (arg_i + n > argc) { error_msg = "ERR GROUPBY field count mismatch"; return false; }
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
      long nargs = std::strtol(std::string(ArgView(argv[arg_i++])).c_str(), nullptr, 10);
      if (arg_i + nargs > argc) { error_msg = "ERR REDUCE nargs mismatch"; return false; }
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
      long n = std::strtol(std::string(ArgView(argv[arg_i])).c_str(), nullptr, 10);
      arg_i++;
      if (arg_i + n > argc || n < 1) { error_msg = "ERR SORTBY args mismatch"; return false; }
      PipelineStage st;
      st.type = PipelineStageType::kSortby;
      std::string sf(ArgView(argv[arg_i]));
      if (!sf.empty() && sf[0] == '@') sf = sf.substr(1);
      st.sortby_field = std::move(sf);
      if (n >= 2 && MatchArg(ArgView(argv[arg_i + 1]), "DESC")) st.sortby_desc = true;
      stages.push_back(std::move(st));
      arg_i += static_cast<int>(n);
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

  for (auto& stage : stages) {
    if (deadline_ms > 0 && NowMs() > deadline_ms) {
      error_msg = "ERR query timeout";
      return false;
    }
    switch (stage.type) {
      case PipelineStageType::kLoad:
        break;
      case PipelineStageType::kApply: {
        ExprNode expr;
        if (!ParseExpr(stage.apply_expr, expr, error_msg)) return false;
        for (auto& row : out_rows) row[stage.apply_alias] = EvalExpr(expr, row).AsString();
        break;
      }
      case PipelineStageType::kFilter: {
        ExprNode expr;
        if (!ParseExpr(stage.filter_expr, expr, error_msg)) return false;
        out_rows.erase(
            std::remove_if(out_rows.begin(), out_rows.end(),
                           [&](const AggRow& row) { return !EvalExpr(expr, row).Truthy(); }),
            out_rows.end());
        break;
      }
      case PipelineStageType::kGroupby:
        if (!stage.groupby_fields.empty() || !stage.reducers.empty()) {
          ApplyGroupby(out_rows, stage.groupby_fields, stage.reducers);
        }
        break;
      case PipelineStageType::kSortby:
        ApplySortby(out_rows, stage.sortby_field, stage.sortby_desc);
        break;
      case PipelineStageType::kLimit:
        ApplyLimit(out_rows, stage.limit_offset, stage.limit_count);
        break;
    }
  }
  return true;
}
