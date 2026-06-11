#pragma once

#include "document_store.h"
#include "index_spec.h"
#include "numeric_index.h"
#include "tag_index.h"
#include "text_index.h"

#include <string>
#include <vector>

struct TextTermModifier {
  bool is_prefix = false;
  int fuzzy_dist = 0;
  bool is_optional = false;
};

struct QueryNode {
  enum class Type {
    kMatchAll,
    kTagMatch,
    kNumericRange,
    kTextMatch,
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

  std::vector<QueryNode> children;
};

struct ParsedQuery {
  QueryNode root;
  bool has_knn = false;
  size_t knn_k = 0;
  std::string knn_field;
  std::string knn_param_name;
};

struct QueryOptions {
  bool nostopwords = false;
  std::vector<std::string> infields;
  int slop = 0;
  bool inorder = false;
  bool stem = true;
  std::string language = "english";
};

bool ParseQuery(const std::string& input, ParsedQuery& out,
                std::string& error_msg,
                const QueryOptions& qopts = {});

std::vector<std::string> SetIntersect(const std::vector<std::string>& a,
                                       const std::vector<std::string>& b);
std::vector<std::string> SetUnion(const std::vector<std::string>& a,
                                   const std::vector<std::string>& b);
std::vector<std::string> SetDifference(const std::vector<std::string>& a,
                                        const std::vector<std::string>& b);

std::vector<std::string> EvaluateQuery(const QueryNode& node,
                                        const IndexSpec& spec,
                                        const DocumentStore& doc_store,
                                        const TagFieldIndices& tag_indices,
                                        const NumericFieldIndices& numeric_indices,
                                        const TextFieldIndices& text_indices,
                                        std::string& error_msg,
                                        const QueryOptions& qopts = {});

struct ScoredResult {
  std::string doc_id;
  double score;
};

std::vector<ScoredResult> EvaluateQueryScored(
    const QueryNode& node, const IndexSpec& spec,
    const DocumentStore& doc_store, const TagFieldIndices& tag_indices,
    const NumericFieldIndices& numeric_indices,
    const TextFieldIndices& text_indices, std::string& error_msg,
    const QueryOptions& qopts = {});
