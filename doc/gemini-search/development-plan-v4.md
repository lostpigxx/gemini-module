# gemini-search Development Plan v4 — Advanced Search Features (Phases 32–35)

Follow-up to v3 production hardening (Phases 27–31), targeting **Redis 7** with clean-room C++20 implementations.

## Milestone Summary

| Phase | Core Capabilities | Status |
|---|---|---|
| **P32** | `VECTOR_RANGE` + `$YIELD_DISTANCE_AS`, `VECTOR FLOAT64` (RDB `encver 8` + AOF), KNN `EF_RUNTIME` & `AS <alias>` | Done |
| **P33** | `TAG SEPARATOR` & `CASESENSITIVE`, `SKIPINITIALSCAN`, index-time `FILTER <expr>` (RDB `encver 9` + AOF), `FT.DROPINDEX DD`, `FT.SEARCH FILTER` | Done |
| **P34** | `SCORER` (`BM25`/`TFIDF`/`TFIDF.DOCNORM`/`DISMAX`/`DOCSCORE`), field `WEIGHT` multiplier, `EXPLAINSCORE`, `RETURN AS`, `WITHSORTKEYS` | Done |
| **P35** | Multi-language stemming (`english`/`french`/`german`/`spanish`), suffix (`*suffix`) & infix (`*infix*`) wildcards, `WITHSUFFIXTRIE`, updated `DESIGN.md` | Done |
| **P36** | Close remaining v2 loose ends: query-time synonym expansion, `LANGUAGE_FIELD`, enforce `NOHL`/`NOOFFSETS`/`NOFREQS`, `GEOSHAPE [FLAT\|SPHERICAL]`, wire `MAXEXPANSIONS`/`MINSTEMLEN` (RDB `encver 10`) | Done |
