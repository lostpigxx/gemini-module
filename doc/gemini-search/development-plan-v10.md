# gemini-search Development Plan v10 — Persistence Closure, HNSW Fast Reload & ON JSON Arrays (Phases 59–61)

Target platform: **Redis 7** (clean-room C++20).
Status: **Planned (Handoff from v9 / Phase 58)**

| Phase | Scope | Status |
|---|---|---|
| **P59** | **RDB `encver 13` & AOF Rewrite Closure**: Persist `IndexSpec::custom_dict` (`DICT <name>`) in RDB `encver 13` (backward-compatible with `1..12`), expose `custom_dict` in `FT.INFO`, emit `DICT` and `QUANTIZATION BQ` in `AofRewriteSearch`, clean up `-Wmissing-field-initializers` warnings + RDB/AOF/PSYNC tests | Planned |
| **P60** | **HNSW Graph Direct RDB Serialization / Fast Load**: Serialize `HnswIndex` layered neighbor lists & metadata in RDB (or parallel load rebuild) to avoid $O(N \log N)$ graph reconstruction on restart + RDB round-trip recall & fuzz tests | Planned |
| **P61** | **`ON JSON` Multi-Value Array JSONPath Indexing**: Expand array wildcards (`$.tags[*]`, `$.items[*].title`) during `IndexJsonKey` into `TAG` / `TEXT` / `NUMERIC` indices + end-to-end `gemini-json` + `gemini-search` Tcl tests | Planned |

## Additional Backlog (v11+)

- **WAND Query-Path Wiring**: Trigger `TextIndex::SearchTopK` automatically on top-level BM25 `FT.SEARCH ... LIMIT offset count` queries (when no custom `SORTBY` overrides score ordering).
- **BQ Oversample + Rerank**: Optional two-stage retrieval (BQ Hamming coarse scan + FP16/SQ8 exact rerank) for high-recall 768d/1536d search.
- **GitHub Actions CI**: Add `.github/workflows/ci.yml` to run Docker Redis 7.2.11 3-module GTest, ASan/UBSan, Tcl, and soak regression on every push/PR.
