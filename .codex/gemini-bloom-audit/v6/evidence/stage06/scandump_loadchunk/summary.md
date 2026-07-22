# SCANDUMP/LOADCHUNK Summary

Verdict: PASS for gemini self, DESIGN_INTENDED_INCOMPATIBLE for cross implementation. gemini self round-trip passed all 9 corpora. Safety checks passed for mid-load read/write rejection, wrongtype/existing-key protection, bad chunk handling, and completed-key overwrite rejection. Cross RedisBloom/gemini SCANDUMP payloads are private and incompatible by DESIGN. Evidence: `../transport_matrix.json`, `../transport_matrix/gemini_self_transport_results.json`, `../transport_matrix/cross_impl_transport_results.json`.
