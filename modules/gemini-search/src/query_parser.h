#pragma once

#include "document_store.h"
#include "index_spec.h"
#include "numeric_index.h"
#include "tag_index.h"
#include "text_index.h"

#include <string>
#include <vector>

struct QueryNode {
  enum class Type {
    kMatchAll,
    kTagMatch,
    kNumericRange,
    kTextMatch,
    kAnd,
    kOr,
    kNot,
  };

  Type type = Type::kMatchAll;

  std::string field_name;
  std::vector<std::string> tag_values;
  std::vector<std::string> text_terms;

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

bool ParseQuery(const std::string& input, ParsedQuery& out,
                std::string& error_msg);

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
                                        std::string& error_msg);
