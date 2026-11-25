#include "hybrid_fusion.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

std::vector<HybridHit> FuseHybridResults(
    const std::vector<ScoredResult>& text_hits,
    const std::vector<KnnResult>& vec_hits,
    const HybridConfig& cfg) {
  // Sort text hits descending by score (tie-break by doc_id asc)
  std::vector<ScoredResult> sorted_text = text_hits;
  std::sort(sorted_text.begin(), sorted_text.end(),
            [](const ScoredResult& a, const ScoredResult& b) {
              if (a.score != b.score) return a.score > b.score;
              return a.doc_id < b.doc_id;
            });

  // Sort vector hits ascending by distance (tie-break by doc_id asc)
  std::vector<KnnResult> sorted_vec = vec_hits;
  std::sort(sorted_vec.begin(), sorted_vec.end(),
            [](const KnnResult& a, const KnnResult& b) {
              if (a.score != b.score) return a.score < b.score;
              return a.doc_id < b.doc_id;
            });

  std::unordered_map<std::string, HybridHit> merged;
  merged.reserve(sorted_text.size() + sorted_vec.size());

  for (size_t i = 0; i < sorted_text.size(); i++) {
    auto& h = merged[sorted_text[i].doc_id];
    h.doc_id = sorted_text[i].doc_id;
    h.text_score = sorted_text[i].score;
    h.text_rank = static_cast<int>(i + 1);
  }
  for (size_t i = 0; i < sorted_vec.size(); i++) {
    auto& h = merged[sorted_vec[i].doc_id];
    h.doc_id = sorted_vec[i].doc_id;
    h.vector_distance = static_cast<double>(sorted_vec[i].score);
    h.vector_rank = static_cast<int>(i + 1);
  }

  if (cfg.method == FusionMethod::kRrf) {
    double k = (std::isfinite(cfg.rrf_k) && cfg.rrf_k > 0.0) ? cfg.rrf_k : 60.0;
    double tw = (std::isfinite(cfg.text_weight) && cfg.text_weight >= 0.0)
                    ? cfg.text_weight
                    : 1.0;
    double vw = (std::isfinite(cfg.vector_weight) && cfg.vector_weight >= 0.0)
                    ? cfg.vector_weight
                    : 1.0;
    for (auto& [id, h] : merged) {
      double s = 0.0;
      if (h.text_rank > 0) s += tw / (k + static_cast<double>(h.text_rank));
      if (h.vector_rank > 0) s += vw / (k + static_cast<double>(h.vector_rank));
      h.score = s;
    }
  } else {
    // LINEAR: min-max normalize text scores (higher = better) and vector distances (lower = better)
    double alpha = std::isfinite(cfg.alpha)
                       ? std::clamp(cfg.alpha, 0.0, 1.0)
                       : 0.5;
    double t_min = std::numeric_limits<double>::infinity();
    double t_max = -std::numeric_limits<double>::infinity();
    for (const auto& t : sorted_text) {
      if (t.score < t_min) t_min = t.score;
      if (t.score > t_max) t_max = t.score;
    }
    double v_min = std::numeric_limits<double>::infinity();
    double v_max = -std::numeric_limits<double>::infinity();
    for (const auto& v : sorted_vec) {
      double d = static_cast<double>(v.score);
      if (d < v_min) v_min = d;
      if (d > v_max) v_max = d;
    }

    for (auto& [id, h] : merged) {
      double t_norm = 0.0;
      if (h.text_rank > 0) {
        if (t_max > t_min) {
          t_norm = (h.text_score - t_min) / (t_max - t_min);
        } else {
          t_norm = 1.0;
        }
      }
      double v_norm = 0.0;
      if (h.vector_rank > 0) {
        if (v_max > v_min) {
          v_norm = (v_max - h.vector_distance) / (v_max - v_min);
        } else {
          v_norm = 1.0;
        }
      }
      h.score = alpha * t_norm + (1.0 - alpha) * v_norm;
    }
  }

  std::vector<HybridHit> out;
  out.reserve(merged.size());
  for (auto& [id, h] : merged) {
    out.push_back(std::move(h));
  }
  std::sort(out.begin(), out.end(), [](const HybridHit& a, const HybridHit& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.doc_id < b.doc_id;
  });
  return out;
}
