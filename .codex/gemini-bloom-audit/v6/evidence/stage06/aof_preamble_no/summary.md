# AOF RDB Preamble No Summary

Verdict: PASS for gemini self, DESIGN_INTENDED_INCOMPATIBLE for cross implementation. gemini self command-AOF replay passed all 9 corpora. Cross implementation uses private BF.LOADCHUNK payloads and is not a DESIGN compatibility contract; non-empty corpora fail cleanly rather than silently claiming compatibility. Evidence: `../transport_matrix.json`, `../transport_matrix/cross_impl_transport_results.json`, `../transport_matrix/gemini_self_transport_results.json`.
