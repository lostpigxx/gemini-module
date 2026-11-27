#pragma once

#include "document_store.h"
#include "geo_index.h"
#include "geoshape_index.h"
#include "index_spec.h"
#include "numeric_index.h"
#include "suggest_dict.h"
#include "tag_index.h"
#include "text_index.h"
#include "vector_index.h"

#include <cstdint>
#include <string>
#include <vector>

struct TextTermModifier {
  bool is_prefix = false;
  bool is_suffix = false;
  bool is_infix = false;
  int fuzzy_dist = 0;
  bool is_optional = false;
};

struct QueryNode {
  enum class Type {
    kMatchAll,
    kTagMatch,
    kNumericRange,
    kTextMatch,
    kGeoFilter,
    kGeoShapeQuery,
    kVectorRange,
    kIsMissing,
    kIsEmpty,
    kAnd,
    kOr,
    kNot,
    kOptional,
  };

  Type type = Type::kMatchAll;

  std::string field_name;
  std::vector<std::string> tag_values;
  std::vector<std::string> text_terms;
  std::vector<TextTermModifier> text_term_mods;
  bool is_phrase = false;

  double range_min = 0;
  double range_max = 0;
  bool min_exclusive = false;
  bool max_exclusive = false;

  double geo_lon = 0;
  double geo_lat = 0;
  double geo_radius = 0;
  GeoUnit geo_unit = GeoUnit::kKm;

  GeoShapeOp geoshape_op = GeoShapeOp::kWithin;
  std::string geoshape_param_name;

  double vec_radius = 0;
  std::string vec_param_name;
  std::string vec_score_alias;

  std::vector<QueryNode> children;
};

struct ParsedQuery {
  QueryNode root;
  bool has_knn = false;
  size_t knn_k = 0;
  std::string knn_field;
  std::string knn_param_name;
  size_t knn_ef_runtime = 0;
  size_t knn_oversample = 0;
  std::string knn_score_alias;
};

enum class ScorerType { kBm25, kTfIdf, kTfIdfDocNorm, kDismax, kDocScore };

struct QueryOptions {
  bool nostopwords = false;
  std::vector<std::string> infields;
  int slop = 0;
  bool inorder = false;
  bool stem = true;
  std::string language = "english";
  int dialect = 2;
  int64_t deadline_ms = 0;
  ScorerType scorer = ScorerType::kBm25;
  size_t max_expansions = 200;
  size_t min_stem_len = 3;
  const std::unordered_set<std::string>* custom_dict = nullptr;
  // Phase 69: Optional per-field global BM25 statistics for 2-Phase DFS scoring
  const std::unordered_map<std::string, GlobalBm25Stats>* global_bm25_stats = nullptr;
};

struct LocalFieldBm25Stats {
  std::string field_name;
  uint64_t live_docs = 0;
  uint64_t total_doc_len = 0;
  std::unordered_map<std::string, uint64_t> term_df;
};

// Phase 69: Collects local (live_docs, total_doc_len, term_df) per referenced text field
// for 2-Phase DFS global BM25 scoring.
std::vector<LocalFieldBm25Stats> CollectQueryTermStats(
    const QueryNode& node, const IndexSpec& spec, const TextFieldIndices& text_indices,
    const QueryOptions& qopts = {}, const SynonymMap* synonyms = nullptr);

bool ParseQuery(const std::string& input, ParsedQuery& out,
                std::string& error_msg,
                const QueryOptions& qopts = {});

std::vector<std::string> SetIntersect(const std::vector<std::string>& a,
                                       const std::vector<std::string>& b);
std::vector<std::string> SetUnion(const std::vector<std::string>& a,
                                   const std::vector<std::string>& b);
std::vector<std::string> SetDifference(const std::vector<std::string>& a,
                                        const std::vector<std::string>& b);

std::vector<std::string> EvaluateQuery(
    const QueryNode& node, const IndexSpec& spec,
    const DocumentStore& doc_store, const TagFieldIndices& tag_indices,
    const NumericFieldIndices& numeric_indices,
    const TextFieldIndices& text_indices,
    const GeoFieldIndices& geo_indices,
    std::string& error_msg,
    const QueryOptions& qopts = {},
    const GeoShapeFieldIndices* geoshape_indices = nullptr,
    const std::unordered_map<std::string, std::string>* params = nullptr,
    const VectorFieldIndices* vector_indices = nullptr,
    const SynonymMap* synonyms = nullptr);

struct ScoredResult {
  std::string doc_id;
  double score;
};

std::vector<ScoredResult> EvaluateQueryScored(
    const QueryNode& node, const IndexSpec& spec,
    const DocumentStore& doc_store, const TagFieldIndices& tag_indices,
    const NumericFieldIndices& numeric_indices,
    const TextFieldIndices& text_indices,
    const GeoFieldIndices& geo_indices, std::string& error_msg,
    const QueryOptions& qopts = {},
    const GeoShapeFieldIndices* geoshape_indices = nullptr,
    const std::unordered_map<std::string, std::string>* params = nullptr,
    const VectorFieldIndices* vector_indices = nullptr,
    const SynonymMap* synonyms = nullptr);
