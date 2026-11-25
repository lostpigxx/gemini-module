#pragma once

#include "query_parser.h"
#include "vector_index.h"

#include <string>
#include <vector>

enum class FusionMethod { kRrf, kLinear };

struct HybridConfig {
  FusionMethod method = FusionMethod::kRrf;
  double rrf_k = 60.0;
  double alpha = 0.5;
  double text_weight = 1.0;
  double vector_weight = 1.0;
};

struct HybridHit {
  std::string doc_id;
  double score = 0.0;
  double text_score = 0.0;
  double vector_distance = 0.0;
  int text_rank = 0;
  int vector_rank = 0;
};

std::vector<HybridHit> FuseHybridResults(
    const std::vector<ScoredResult>& text_hits,
    const std::vector<KnnResult>& vec_hits,
    const HybridConfig& cfg);
