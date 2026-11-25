#include "query_parser.h"
#include "stemmer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <limits>
#include <set>
#include <unordered_set>

static int64_t QueryMonotonicMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000LL +
         static_cast<int64_t>(ts.tv_nsec) / 1000000LL;
}

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
    const TextFieldIndices& text_indices,
    const GeoFieldIndices& geo_indices, std::string& error_msg,
    const QueryOptions& qopts,
    const GeoShapeFieldIndices* geoshape_indices,
    const std::unordered_map<std::string, std::string>* params,
    const VectorFieldIndices* vector_indices,
    const SynonymMap* synonyms) {
  if (qopts.deadline_ms > 0 && QueryMonotonicMs() > qopts.deadline_ms) {
    error_msg = "ERR query timeout";
    return {};
  }
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
      return idx->LookupOr(node.tag_values, fspec->case_sensitive);
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
      auto LookupWithMod = [&qopts, synonyms](const TextIndex* idx, const std::string& term,
                                     const TextTermModifier& mod,
                                     bool nostem, bool phonetic) -> std::vector<std::string> {
        if (mod.is_infix) return idx->InfixLookup(term, qopts.max_expansions);
        if (mod.is_suffix) return idx->SuffixLookup(term, qopts.max_expansions);
        if (mod.is_prefix) return idx->PrefixLookup(term, qopts.max_expansions);
        if (mod.fuzzy_dist > 0) return idx->FuzzyLookup(term, mod.fuzzy_dist, qopts.max_expansions);
        auto LookupOne = [&](const std::string& t) {
          return (qopts.stem && !nostem && IsSupportedStemLanguage(qopts.language))
                     ? idx->StemLookup(t, qopts.language, qopts.min_stem_len)
                     : idx->Lookup(t);
        };
        std::vector<std::string> base = LookupOne(term);
        if (synonyms && qopts.stem) {
          for (const auto& syn : synonyms->Expand(term)) {
            auto sids = LookupOne(syn);
            if (!sids.empty()) base = SetUnion(base, sids);
          }
        }
        if (phonetic && qopts.stem) {
          auto ph = idx->PhoneticLookup(term);
          if (!ph.empty()) base = SetUnion(base, ph);
        }
        return base;
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
        if (node.is_phrase) {
          return idx->PhraseLookup(node.text_terms, qopts.slop, qopts.inorder);
        }
        std::set<std::string> merged;
        for (size_t i = 0; i < node.text_terms.size(); i++) {
          const auto& mod = (i < node.text_term_mods.size()) ? node.text_term_mods[i] : default_mod;
          auto ids = LookupWithMod(idx, node.text_terms[i], mod, fspec->nostem,
                                   !fspec->phonetic.empty());
          merged.insert(ids.begin(), ids.end());
        }
        return {merged.begin(), merged.end()};
      }
      std::set<std::string> merged;
      std::unordered_set<std::string> infield_set(qopts.infields.begin(), qopts.infields.end());
      for (auto& f : spec.fields) {
        if (f.type != FieldType::kText) continue;
        if (!infield_set.empty() && infield_set.find(f.name) == infield_set.end()) continue;
        const auto* idx = text_indices.Get(f.name);
        if (!idx) continue;
        if (node.is_phrase) {
          auto ids = idx->PhraseLookup(node.text_terms, qopts.slop, qopts.inorder);
          merged.insert(ids.begin(), ids.end());
        } else {
          for (size_t i = 0; i < node.text_terms.size(); i++) {
            const auto& mod = (i < node.text_term_mods.size()) ? node.text_term_mods[i] : default_mod;
            auto ids = LookupWithMod(idx, node.text_terms[i], mod, f.nostem,
                                     !f.phonetic.empty());
            merged.insert(ids.begin(), ids.end());
          }
        }
      }
      return {merged.begin(), merged.end()};
    }

    case QueryNode::Type::kGeoFilter: {
      const auto* fspec = spec.FindField(node.field_name);
      if (!fspec) {
        error_msg = "ERR query field not in schema";
        return {};
      }
      if (fspec->type != FieldType::kGeo) {
        error_msg = "ERR field is not a GEO field";
        return {};
      }
      const auto* idx = geo_indices.Get(node.field_name);
      if (!idx) return {};
      double radius_m = node.geo_radius * GeoUnitToMeters(node.geo_unit);
      auto geo_results = idx->RadiusQuery(node.geo_lon, node.geo_lat, radius_m);
      std::vector<std::string> ids;
      ids.reserve(geo_results.size());
      for (auto& gr : geo_results) ids.push_back(gr.doc_id);
      std::sort(ids.begin(), ids.end());
      return ids;
    }

    case QueryNode::Type::kGeoShapeQuery: {
      const auto* fspec = spec.FindField(node.field_name);
      if (!fspec) { error_msg = "ERR query field not in schema"; return {}; }
      if (fspec->type != FieldType::kGeoShape) { error_msg = "ERR field is not a GEOSHAPE field"; return {}; }
      if (!geoshape_indices) return {};
      const auto* idx = geoshape_indices->Get(node.field_name);
      if (!idx) return {};
      if (!params || node.geoshape_param_name.empty()) { error_msg = "ERR GEOSHAPE query requires $param"; return {}; }
      auto pit = params->find(node.geoshape_param_name);
      if (pit == params->end()) { error_msg = "ERR GEOSHAPE param not found"; return {}; }
      GeoShape query_shape;
      if (!ParseWkt(pit->second, query_shape)) { error_msg = "ERR invalid WKT in GEOSHAPE param"; return {}; }
      return idx->Query(node.geoshape_op, query_shape);
    }

    case QueryNode::Type::kVectorRange: {
      const auto* fspec = spec.FindField(node.field_name);
      if (!fspec) { error_msg = "ERR query field not in schema"; return {}; }
      if (fspec->type != FieldType::kVector) { error_msg = "ERR field is not a VECTOR field"; return {}; }
      if (!vector_indices) return {};
      const auto* vidx = vector_indices->Get(node.field_name);
      if (!vidx) return {};
      if (!params || node.vec_param_name.empty()) { error_msg = "ERR VECTOR_RANGE requires $param"; return {}; }
      auto pit = params->find(node.vec_param_name);
      if (pit == params->end()) { error_msg = "ERR VECTOR_RANGE param not found"; return {}; }
      std::vector<float> qvec;
      if (!DecodeVectorBlob(pit->second, fspec->vector_params.dim,
                            fspec->vector_params.data_type, qvec)) {
        error_msg = "ERR query vector dimension mismatch";
        return {};
      }
      auto hits = vidx->RangeQuery(qvec.data(), static_cast<float>(node.vec_radius));
      std::vector<std::string> ids;
      ids.reserve(hits.size());
      for (const auto& h : hits) ids.push_back(h.doc_id);
      std::sort(ids.begin(), ids.end());
      return ids;
    }

    case QueryNode::Type::kIsMissing: {
      const auto* fspec = spec.FindField(node.field_name);
      if (!fspec) { error_msg = "ERR query field not in schema"; return {}; }
      if (!fspec->index_missing) {
        error_msg = "ERR field is not indexed with INDEXMISSING";
        return {};
      }
      std::vector<std::string> ids;
      for (const auto& doc_id : doc_store.AllIds()) {
        const auto* doc = doc_store.Get(doc_id);
        if (doc && doc->fields.find(node.field_name) == doc->fields.end()) {
          ids.push_back(doc_id);
        }
      }
      return ids;
    }

    case QueryNode::Type::kIsEmpty: {
      const auto* fspec = spec.FindField(node.field_name);
      if (!fspec) { error_msg = "ERR query field not in schema"; return {}; }
      if (!fspec->index_empty) {
        error_msg = "ERR field is not indexed with INDEXEMPTY";
        return {};
      }
      std::vector<std::string> ids;
      for (const auto& doc_id : doc_store.AllIds()) {
        const auto* doc = doc_store.Get(doc_id);
        if (doc) {
          auto fit = doc->fields.find(node.field_name);
          if (fit != doc->fields.end() && fit->second.empty()) {
            ids.push_back(doc_id);
          }
        }
      }
      return ids;
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
                                 geo_indices, error_msg, qopts, geoshape_indices, params,
                                 vector_indices, synonyms);
      if (!error_msg.empty()) return {};
      auto right = EvaluateQuery(node.children[1], spec, doc_store,
                                  tag_indices, numeric_indices, text_indices,
                                  geo_indices, error_msg, qopts, geoshape_indices, params,
                                 vector_indices, synonyms);
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
                                 geo_indices, error_msg, qopts, geoshape_indices, params,
                                 vector_indices, synonyms);
      if (!error_msg.empty()) return {};
      auto right = EvaluateQuery(node.children[1], spec, doc_store,
                                  tag_indices, numeric_indices, text_indices,
                                  geo_indices, error_msg, qopts, geoshape_indices, params,
                                 vector_indices, synonyms);
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
                                  geo_indices, error_msg, qopts, geoshape_indices, params,
                                 vector_indices, synonyms);
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
    const TextFieldIndices& text_indices,
    const GeoFieldIndices& geo_indices, std::string& error_msg,
    const QueryOptions& qopts,
    const GeoShapeFieldIndices* geoshape_indices,
    const std::unordered_map<std::string, std::string>* params,
    const VectorFieldIndices* vector_indices,
    const SynonymMap* synonyms) {
  if (qopts.deadline_ms > 0 && QueryMonotonicMs() > qopts.deadline_ms) {
    error_msg = "ERR query timeout";
    return {};
  }
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
                               numeric_indices, text_indices, geo_indices,
                               error_msg, qopts);
      if (!error_msg.empty()) return {};
      std::vector<ScoredResult> out;
      out.reserve(ids.size());
      for (auto& id : ids) out.push_back({id, 1.0});
      return out;
    }

    case QueryNode::Type::kNumericRange: {
      auto ids = EvaluateQuery(node, spec, doc_store, tag_indices,
                               numeric_indices, text_indices, geo_indices,
                               error_msg, qopts);
      if (!error_msg.empty()) return {};
      std::vector<ScoredResult> out;
      out.reserve(ids.size());
      for (auto& id : ids) out.push_back({id, 1.0});
      return out;
    }

    case QueryNode::Type::kTextMatch: {
      auto SearchWithMod = [&qopts, synonyms](const TextIndex* idx, const std::string& term,
                                     const TextTermModifier& mod,
                                     bool nostem, bool phonetic) -> std::vector<TextSearchResult> {
        if (mod.is_infix) return idx->InfixSearch(term, qopts.max_expansions);
        if (mod.is_suffix) return idx->SuffixSearch(term, qopts.max_expansions);
        if (mod.is_prefix) return idx->PrefixSearch(term, qopts.max_expansions);
        if (mod.fuzzy_dist > 0) return idx->FuzzySearch(term, mod.fuzzy_dist, qopts.max_expansions);
        auto SearchOne = [&](const std::string& t) {
          return (qopts.stem && !nostem && IsSupportedStemLanguage(qopts.language))
                     ? idx->StemSearch(t, qopts.language, qopts.min_stem_len)
                     : idx->Search({t});
        };
        std::vector<TextSearchResult> base = SearchOne(term);
        if (synonyms && qopts.stem) {
          auto syns = synonyms->Expand(term);
          if (!syns.empty()) {
            std::unordered_map<std::string, double> sc;
            for (const auto& r : base) sc[r.doc_id] = std::max(sc[r.doc_id], r.score);
            for (const auto& syn : syns) {
              for (const auto& r : SearchOne(syn)) sc[r.doc_id] = std::max(sc[r.doc_id], r.score);
            }
            base.clear();
            base.reserve(sc.size());
            for (const auto& [id, s] : sc) base.push_back({id, s});
          }
        }
        if (phonetic && qopts.stem) {
          auto ph = idx->PhoneticSearch(term);
          if (!ph.empty()) {
            std::unordered_map<std::string, double> sc;
            for (const auto& r : base) sc[r.doc_id] = std::max(sc[r.doc_id], r.score);
            for (const auto& r : ph) sc[r.doc_id] = std::max(sc[r.doc_id], r.score);
            base.clear();
            base.reserve(sc.size());
            for (const auto& [id, s] : sc) base.push_back({id, s});
          }
        }
        return base;
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
        if (node.is_phrase) {
          auto results = idx->PhraseSearch(node.text_terms, qopts.slop, qopts.inorder);
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
        double field_w = fspec->weight;
        for (size_t i = 0; i < node.text_terms.size(); i++) {
          const auto& mod = (i < node.text_term_mods.size()) ? node.text_term_mods[i] : default_mod;
          auto results = SearchWithMod(idx, node.text_terms[i], mod, fspec->nostem,
                                       !fspec->phonetic.empty());
          for (auto& r : results) {
            double contrib = (qopts.scorer == ScorerType::kDocScore) ? 1.0 : (r.score * field_w);
            if (qopts.scorer == ScorerType::kDismax) {
              score_map[r.doc_id] = std::max(score_map[r.doc_id], contrib);
            } else {
              score_map[r.doc_id] += contrib;
            }
          }
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
      std::unordered_set<std::string> infield_set(qopts.infields.begin(), qopts.infields.end());
      for (auto& f : spec.fields) {
        if (f.type != FieldType::kText) continue;
        if (!infield_set.empty() && infield_set.find(f.name) == infield_set.end()) continue;
        const auto* idx = text_indices.Get(f.name);
        if (!idx) continue;
        if (node.is_phrase) {
          auto results = idx->PhraseSearch(node.text_terms, qopts.slop, qopts.inorder);
          for (auto& r : results) {
            double contrib = (qopts.scorer == ScorerType::kDocScore) ? 1.0 : (r.score * f.weight);
            if (qopts.scorer == ScorerType::kDismax) {
              merged[r.doc_id] = std::max(merged[r.doc_id], contrib);
            } else {
              merged[r.doc_id] += contrib;
            }
          }
        } else {
          for (size_t i = 0; i < node.text_terms.size(); i++) {
            const auto& mod = (i < node.text_term_mods.size()) ? node.text_term_mods[i] : default_mod;
            auto results = SearchWithMod(idx, node.text_terms[i], mod, f.nostem,
                                         !f.phonetic.empty());
            for (auto& r : results) {
              double contrib = (qopts.scorer == ScorerType::kDocScore) ? 1.0 : (r.score * f.weight);
              if (qopts.scorer == ScorerType::kDismax) {
                merged[r.doc_id] = std::max(merged[r.doc_id], contrib);
              } else {
                merged[r.doc_id] += contrib;
              }
            }
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

    case QueryNode::Type::kGeoFilter: {
      auto ids = EvaluateQuery(node, spec, doc_store, tag_indices,
                               numeric_indices, text_indices, geo_indices,
                               error_msg, qopts, geoshape_indices, params, vector_indices,
                               synonyms);
      if (!error_msg.empty()) return {};
      std::vector<ScoredResult> out;
      out.reserve(ids.size());
      for (auto& id : ids) out.push_back({id, 1.0});
      return out;
    }

    case QueryNode::Type::kGeoShapeQuery:
    case QueryNode::Type::kVectorRange:
    case QueryNode::Type::kIsMissing:
    case QueryNode::Type::kIsEmpty: {
      auto ids = EvaluateQuery(node, spec, doc_store, tag_indices,
                               numeric_indices, text_indices, geo_indices,
                               error_msg, qopts, geoshape_indices, params,
                               vector_indices);
      if (!error_msg.empty()) return {};
      std::vector<ScoredResult> out;
      out.reserve(ids.size());
      for (auto& id : ids) out.push_back({id, 1.0});
      return out;
    }

    case QueryNode::Type::kOptional: {
      if (node.children.size() != 1) {
        error_msg = "ERR internal: OPTIONAL requires 1 child";
        return {};
      }
      auto child = EvaluateQueryScored(node.children[0], spec, doc_store,
                                        tag_indices, numeric_indices,
                                        text_indices, geo_indices,
                                        error_msg, qopts, geoshape_indices, params, vector_indices,
                               synonyms);
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
                                       text_indices, geo_indices,
                                       error_msg, qopts, geoshape_indices, params, vector_indices,
                               synonyms);
      if (!error_msg.empty()) return {};
      auto right = EvaluateQueryScored(node.children[1], spec, doc_store,
                                        tag_indices, numeric_indices,
                                        text_indices, geo_indices,
                                        error_msg, qopts, geoshape_indices, params, vector_indices,
                               synonyms);
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
                                       text_indices, geo_indices,
                                       error_msg, qopts, geoshape_indices, params, vector_indices,
                               synonyms);
      if (!error_msg.empty()) return {};
      auto right = EvaluateQueryScored(node.children[1], spec, doc_store,
                                        tag_indices, numeric_indices,
                                        text_indices, geo_indices,
                                        error_msg, qopts, geoshape_indices, params, vector_indices,
                               synonyms);
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
                                        text_indices, geo_indices,
                                        error_msg, qopts, geoshape_indices, params, vector_indices,
                               synonyms);
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
  static constexpr int kMaxDepth = 64;
  const std::string& input;
  size_t pos = 0;
  std::string& error_msg;
  bool nostopwords = false;
  int dialect = 2;
  int depth = 0;
  const std::unordered_set<std::string>* custom_dict = nullptr;

  Parser(const std::string& s, std::string& err, bool nostop = false,
         int dia = 2,
         const std::unordered_set<std::string>* cdict = nullptr)
      : input(s),
        error_msg(err),
        nostopwords(nostop),
        dialect(dia),
        custom_dict(cdict) {}

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

    // Wildcards: *infix*, *suffix, prefix*
    bool lead_star = (!s.empty() && s.front() == '*');
    bool trail_star = (s.size() >= 2 && s.back() == '*');
    if (lead_star && trail_star) {
      mod.is_infix = true;
      s = s.substr(1, s.size() - 2);
    } else if (lead_star) {
      mod.is_suffix = true;
      s = s.substr(1);
    } else if (!s.empty() && s.back() == '*') {
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
      if (!mod.is_prefix && !mod.is_suffix && !mod.is_infix &&
          !mod.fuzzy_dist && !mod.is_optional) {
        // Plain term: apply stop word filter unless nostopwords
        auto filtered = (custom_dict && !custom_dict->empty())
                            ? TextIndex::TokenizeWithDict(base, *custom_dict,
                                                          !nostopwords)
                            : (nostopwords ? TextIndex::TokenizeRaw(base)
                                           : TextIndex::Tokenize(base));
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
      // Numeric range or GEO filter
      size_t close = FindClosing('[', ']', pos + 1);
      if (close == std::string::npos) {
        error_msg = "ERR syntax error: expected closing ]";
        return false;
      }
      std::string body = input.substr(pos + 1, close - pos - 1);
      pos = close + 1;

      if (body.empty()) {
        error_msg = "ERR syntax error: empty range expression";
        return false;
      }

      // Tokenize by spaces to distinguish numeric (2 tokens) from geo (4 tokens)
      std::vector<std::string> tokens;
      {
        size_t ti = 0;
        while (ti < body.size()) {
          while (ti < body.size() && body[ti] == ' ') ti++;
          if (ti >= body.size()) break;
          size_t ts = ti;
          while (ti < body.size() && body[ti] != ' ') ti++;
          tokens.push_back(body.substr(ts, ti - ts));
        }
      }

      if (tokens.size() == 4) {
        // GEO filter: @field:[lon lat radius unit]
        char* ep1 = nullptr;
        char* ep2 = nullptr;
        char* ep3 = nullptr;
        double lon = std::strtod(tokens[0].c_str(), &ep1);
        double lat = std::strtod(tokens[1].c_str(), &ep2);
        double radius = std::strtod(tokens[2].c_str(), &ep3);
        GeoUnit unit;
        if (*ep1 != '\0' || *ep2 != '\0' || *ep3 != '\0' ||
            std::isnan(lon) || std::isnan(lat) || std::isnan(radius) ||
            radius < 0 || !ParseGeoUnit(tokens[3], unit)) {
          error_msg = "ERR syntax error: expected [lon lat radius m|km|mi|ft]";
          return false;
        }
        out.type = QueryNode::Type::kGeoFilter;
        out.field_name = std::move(field_name);
        out.geo_lon = lon;
        out.geo_lat = lat;
        out.geo_radius = radius;
        out.geo_unit = unit;
        return true;
      }

      if (tokens.size() == 3) {
        std::string op_upper = tokens[0];
        for (auto& c : op_upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (op_upper == "VECTOR_RANGE" && !tokens[2].empty() && tokens[2][0] == '$') {
          char* ep = nullptr;
          double radius = std::strtod(tokens[1].c_str(), &ep);
          if (*ep != '\0' || std::isnan(radius) || radius < 0) {
            error_msg = "ERR syntax error: VECTOR_RANGE radius must be a non-negative number";
            return false;
          }
          out.type = QueryNode::Type::kVectorRange;
          out.field_name = std::move(field_name);
          out.vec_radius = radius;
          out.vec_param_name = tokens[2].substr(1);
          // Optional =>{$YIELD_DISTANCE_AS: alias}
          size_t save_pos = pos;
          SkipSpaces();
          if (pos + 2 < input.size() && input[pos] == '=' && input[pos + 1] == '>') {
            size_t p2 = pos + 2;
            while (p2 < input.size() && (input[p2] == ' ' || input[p2] == '\t')) p2++;
            if (p2 < input.size() && input[p2] == '{') {
              size_t c2 = FindClosing('{', '}', p2 + 1);
              if (c2 != std::string::npos) {
                std::string attrs = input.substr(p2 + 1, c2 - p2 - 1);
                size_t colon = attrs.find(':');
                if (colon != std::string::npos) {
                  std::string k = attrs.substr(0, colon);
                  std::string v = attrs.substr(colon + 1);
                  auto Trim = [](std::string& s) {
                    size_t a = s.find_first_not_of(" \t");
                    if (a == std::string::npos) { s.clear(); return; }
                    size_t b = s.find_last_not_of(" \t");
                    s = s.substr(a, b - a + 1);
                  };
                  Trim(k);
                  Trim(v);
                  std::string ku = k;
                  for (auto& ch : ku) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                  if (ku == "$YIELD_DISTANCE_AS" && !v.empty()) {
                    out.vec_score_alias = v;
                  }
                }
                pos = c2 + 1;
              } else {
                pos = save_pos;
              }
            } else {
              pos = save_pos;
            }
          } else {
            pos = save_pos;
          }
          return true;
        }
      }

      if (tokens.size() == 2) {
        // Check for GEOSHAPE query: @field:[WITHIN|CONTAINS|INTERSECTS|DISJOINT $param]
        std::string op_upper = tokens[0];
        for (auto& c : op_upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if ((op_upper == "WITHIN" || op_upper == "CONTAINS" ||
             op_upper == "INTERSECTS" || op_upper == "DISJOINT") &&
            !tokens[1].empty() && tokens[1][0] == '$') {
          GeoShapeOp gs_op;
          if (op_upper == "WITHIN") gs_op = GeoShapeOp::kWithin;
          else if (op_upper == "CONTAINS") gs_op = GeoShapeOp::kContains;
          else if (op_upper == "INTERSECTS") gs_op = GeoShapeOp::kIntersects;
          else gs_op = GeoShapeOp::kDisjoint;
          out.type = QueryNode::Type::kGeoShapeQuery;
          out.field_name = std::move(field_name);
          out.geoshape_op = gs_op;
          out.geoshape_param_name = tokens[1].substr(1);
          return true;
        }
      }

      if (tokens.size() != 2) {
        error_msg = "ERR syntax error: expected numeric range [min max] or geo filter [lon lat radius unit]";
        return false;
      }

      // Numeric range (2 tokens)
      double min_val, max_val;
      bool min_excl, max_excl;
      if (!ParseNumericBound(tokens[0], min_val, min_excl)) {
        error_msg = "ERR syntax error: invalid numeric min value";
        return false;
      }
      if (!ParseNumericBound(tokens[1], max_val, max_excl)) {
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

    // TEXT phrase match: @field:"phrase"
    if (input[pos] == '"') {
      pos++;
      size_t start = pos;
      while (pos < input.size() && input[pos] != '"') pos++;
      if (pos >= input.size()) {
        error_msg = "ERR syntax error: unclosed quote";
        return false;
      }
      std::string body = input.substr(start, pos - start);
      pos++;
      auto terms = (custom_dict && !custom_dict->empty())
                       ? TextIndex::TokenizeWithDict(body, *custom_dict, false)
                       : TextIndex::TokenizeRaw(body);
      if (terms.empty()) {
        error_msg = "ERR syntax error: empty phrase";
        return false;
      }
      out.type = QueryNode::Type::kTextMatch;
      out.field_name = std::move(field_name);
      out.text_terms = std::move(terms);
      out.is_phrase = true;
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
    if (depth >= kMaxDepth) {
      error_msg = "ERR query exceeds maximum nesting depth";
      return false;
    }
    SkipSpaces();
    if (Peek() == '(') {
      pos++;
      depth++;
      bool ok = ParseOr(out);
      depth--;
      if (!ok) return false;
      SkipSpaces();
      if (Peek() != ')') {
        error_msg = "ERR syntax error: expected closing )";
        return false;
      }
      pos++;
      return true;
    }
    if (Peek() == '*' && (pos + 1 >= input.size() || input[pos + 1] == ' ' ||
                          input[pos + 1] == '\t' || input[pos + 1] == ')' ||
                          input[pos + 1] == '|' || input[pos + 1] == '=')) {
      pos++;
      out.type = QueryNode::Type::kMatchAll;
      return true;
    }
    if (Peek() == '@') {
      return ParseFieldExpr(out);
    }
    // Quoted phrase: "hello world"
    if (Peek() == '"') {
      pos++;
      size_t start = pos;
      while (pos < input.size() && input[pos] != '"') pos++;
      if (pos >= input.size()) {
        error_msg = "ERR syntax error: unclosed quote";
        return false;
      }
      std::string body = input.substr(start, pos - start);
      pos++;
      auto terms = (custom_dict && !custom_dict->empty())
                       ? TextIndex::TokenizeWithDict(body, *custom_dict, false)
                       : TextIndex::TokenizeRaw(body);
      if (terms.empty()) {
        error_msg = "ERR syntax error: empty phrase";
        return false;
      }
      out.type = QueryNode::Type::kTextMatch;
      out.field_name = "";
      out.text_terms = std::move(terms);
      out.is_phrase = true;
      return true;
    }
    // Predicate functions: ismissing(@field), isempty(@field)
    if (std::isalpha(static_cast<unsigned char>(Peek()))) {
      size_t fn_start = pos;
      size_t p = pos;
      while (p < input.size() && std::isalpha(static_cast<unsigned char>(input[p]))) p++;
      if (p < input.size() && input[p] == '(') {
        std::string fn = input.substr(fn_start, p - fn_start);
        for (auto& c : fn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (fn == "ismissing" || fn == "isempty") {
          size_t close = FindClosing('(', ')', p + 1);
          if (close == std::string::npos) {
            error_msg = "ERR syntax error: expected closing )";
            return false;
          }
          std::string arg = input.substr(p + 1, close - p - 1);
          size_t a = arg.find_first_not_of(" \t");
          size_t b = arg.find_last_not_of(" \t");
          if (a == std::string::npos || arg[a] != '@' || b <= a) {
            error_msg = "ERR syntax error: " + fn + " requires @field";
            return false;
          }
          std::string fname = arg.substr(a + 1, b - a);
          pos = close + 1;
          out.type = (fn == "ismissing") ? QueryNode::Type::kIsMissing
                                         : QueryNode::Type::kIsEmpty;
          out.field_name = std::move(fname);
          return true;
        }
      }
    }
    // Bare term (including prefix hel*, suffix *ing, infix *earc*, fuzzy %hello%, UTF-8 CJK)
    if (std::isalnum(static_cast<unsigned char>(Peek())) || Peek() == '%' ||
        Peek() == '*' || static_cast<unsigned char>(Peek()) >= 0x80) {
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
      if (terms.size() <= 1) {
        out.type = QueryNode::Type::kTextMatch;
        out.field_name = "";
        out.text_terms = std::move(terms);
        out.text_term_mods = std::move(mods);
        return true;
      }
      // Multiple tokens produced from a single bare token (e.g. CJK bigrams): AND them
      QueryNode acc;
      acc.type = QueryNode::Type::kTextMatch;
      acc.field_name = "";
      acc.text_terms = {std::move(terms[0])};
      acc.text_term_mods = {mods[0]};
      for (size_t ti = 1; ti < terms.size(); ti++) {
        QueryNode rhs;
        rhs.type = QueryNode::Type::kTextMatch;
        rhs.field_name = "";
        rhs.text_terms = {std::move(terms[ti])};
        rhs.text_term_mods = {mods[ti]};
        QueryNode combined;
        combined.type = QueryNode::Type::kAnd;
        combined.children.push_back(std::move(acc));
        combined.children.push_back(std::move(rhs));
        acc = std::move(combined);
      }
      out = std::move(acc);
      return true;
    }
    error_msg = "ERR syntax error: expected @, *, (, or term";
    return false;
  }

  // Unary: '-' unary | '~' unary | primary
  bool ParseUnary(QueryNode& out) {
    if (depth >= kMaxDepth) {
      error_msg = "ERR query exceeds maximum nesting depth";
      return false;
    }
    SkipSpaces();
    if (Peek() == '-') {
      pos++;
      QueryNode child;
      depth++;
      // Dialect 1: negation applies to the entire following AND clause (-a b => -(a AND b)).
      // Dialect 2+: negation binds tightly to the next unary atom (-a b => (-a) AND b).
      bool ok = (dialect == 1) ? ParseAnd(child) : ParseUnary(child);
      depth--;
      if (!ok) return false;
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

  if (tokens.size() < 4 || (tokens[0] != "KNN" && tokens[0] != "knn")) {
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

  size_t ef_runtime = 0;
  std::string score_alias;
  for (size_t ti = 4; ti < tokens.size(); ) {
    std::string kw = tokens[ti];
    for (auto& ch : kw) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (kw == "EF_RUNTIME" && ti + 1 < tokens.size()) {
      char* ep = nullptr;
      long ef = std::strtol(tokens[ti + 1].c_str(), &ep, 10);
      if (*ep != '\0' || ef <= 0) {
        error_msg = "ERR syntax error: EF_RUNTIME must be a positive integer";
        return false;
      }
      ef_runtime = static_cast<size_t>(ef);
      ti += 2;
    } else if (kw == "AS" && ti + 1 < tokens.size()) {
      score_alias = tokens[ti + 1];
      ti += 2;
    } else {
      error_msg = "ERR syntax error: unexpected token in KNN clause";
      return false;
    }
  }

  out.has_knn = true;
  out.knn_k = static_cast<size_t>(k_val);
  out.knn_field = std::move(knn_field);
  out.knn_param_name = std::move(knn_param);
  out.knn_ef_runtime = ef_runtime;
  out.knn_score_alias = std::move(score_alias);
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

  // Check for =>[KNN ...] suffix (distinguish from =>{$YIELD_DISTANCE_AS:...})
  size_t arrow = std::string::npos;
  for (size_t search_pos = 0; ; ) {
    size_t found = trimmed.find("=>", search_pos);
    if (found == std::string::npos) break;
    size_t after = trimmed.find_first_not_of(" \t", found + 2);
    if (after != std::string::npos && trimmed[after] == '[') {
      arrow = found;
      break;
    }
    search_pos = found + 2;
  }
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

    Parser parser(filter_part, error_msg, qopts.nostopwords, qopts.dialect,
                  qopts.custom_dict);
    if (!parser.ParseOr(out.root)) return false;
    parser.SkipSpaces();
    if (parser.pos != filter_part.size()) {
      error_msg = "ERR syntax error: unexpected input after query";
      return false;
    }
    return true;
  }

  // No KNN — parse as regular query tree
  Parser parser(trimmed, error_msg, qopts.nostopwords, qopts.dialect,
                qopts.custom_dict);
  if (!parser.ParseOr(out.root)) return false;
  parser.SkipSpaces();
  if (parser.pos != trimmed.size()) {
    error_msg = "ERR syntax error: unexpected input after query";
    return false;
  }
  return true;
}
