# gemini-search Development Plan v6 — Advanced Capabilities (Phases 42–46)

Target platform: **Redis 7** (clean-room C++20).
Every phase ships with comprehensive GTest unit tests (covering core logic, edge cases, and error paths) and Redis 7 Tcl integration tests.

## Roadmap

| Phase | Scope | Status |
|---|---|---|
| **P42** | **Hybrid Search (`FT.HYBRID`)**: BM25 full-text + Vector KNN dual-branch retrieval with RRF (`RRF_K`) and Linear (`ALPHA`) score fusion, independent pre-filters, `PARAMS`, `LIMIT`, `RETURN`, `WITHSCORES`, `EXPLAINSCORE`, `NOCONTENT` | Done |
| **P43** | **High-Perf HNSW + Low-Precision Vectors**: Dense `uint32_t` node IDs + contiguous float buffer + visited-epoch array in `HnswIndex` (benchmarked in `search_bench`); `TYPE FLOAT16` & `BFLOAT16` (`encver 12`); optional `SQ8` 8-bit scalar quantization | Done |
| **P44** | **`FT.AGGREGATE` Analytics Enhancements**: Multi-key `SORTBY` + `MAX <n>` Top-K; expanded `APPLY` functions (`startswith`, `endswith`, `contains`, `trim`, `replace`, `split`, `year`, `monthofyear`, `day`, `dayofweek`, `hour`, `timefmt`, `geodistance`, `exists`); `FT.AGGREGATE` with KNN + `PARAMS` + `LOAD *` | Pending |
| **P45** | **CJK / Unicode Tokenization**: UTF-8 CJK bigram + unigram fallback tokenizer (`LANGUAGE chinese` + auto-detect CJK codepoints) with full BM25, phrase search, `HIGHLIGHT`, and `SUMMARIZE` support | Pending |
| **P46** | **Observability & Runtime Governance**: Extended `FT.INFO` metrics (`num_terms`, `inverted_sz_mb`, `vector_index_sz_mb`, `cursor_stats`, `total_indexing_time`); `FT.CONFIG` `ON_TIMEOUT` (`FAIL` / `RETURN`) and `MAXSEARCHRESULTS` | Pending |
