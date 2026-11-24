# gemini-search Development Plan v3 — Production Hardening (Redis 7)

Phases 0–26 implemented the core search engine and 31 `FT.*` commands (all unit and TCL tests pass).
This v3 plan focuses on closing the gap between feature-complete prototype and production-ready Redis 7 module.

**Target platform**: **Redis 7 only** (baseline: Redis 7.2.11). No Redis 5/6 compatibility is required.
We can rely directly on Redis 7 Module APIs (`RedisModule_EmitAOF` `"v"` vector format, `aux_save2`/`aux_load`,
`REDISMODULE_OPTIONS_HANDLE_IO_ERRORS`, `RedisModule_SubscribeToServerEvent`, `RedisModule_CreateTimer`,
`RedisModule_ReplyWithMap`, and full `RedisModuleTypeMethods` callbacks).

**Clean-room policy** continues to apply: only public documentation (redis.io) and academic references are used;
no RediSearch source code is referenced.

---

## Phase 27 — Persistence & Module Type Hardening

**Goal**: Make RDB and AOF persistence complete, safe against malformed input, and remove silent data-loss bugs.

### 27.1 Fix AOF Rewrite 10-Field Truncation
- Replace the `switch (nf)` (1..10) dispatch in `AofRewriteSearch` (`search_rdb.cc`) with
  `RedisModule_EmitAOF(aof, "FT.ADD", "v", argv, argc)` using `RedisModule_CreateString` so documents with
  arbitrary field counts are never silently dropped.
- Ensure all schema/index options (`NOSTEM`, `PHONETIC`, `GEOSHAPE` coordinate system, `TEMPORARY`, etc.) are
  faithfully emitted.

### 27.2 Complete RDB Serialization & Global State Persistence
- Bump `kSearchEncVer` (backward-compatible with older encver) to persist missing `FieldSpec` attributes:
  `nostem`, `phonetic`, and `geoshape` coordinate system.
- Persist non-key global state (`g_aliases`, `g_suggest_dicts`, `g_term_dicts`, per-index synonyms, `g_config`)
  using Redis 7 `aux_save2` / `aux_load` callbacks so restarts preserve the full module state.
- Enable `REDISMODULE_OPTIONS_HANDLE_IO_ERRORS` in `RedisModule_OnLoad` and add bounds/sanity validation on
  all RDB load paths (field counts, string lengths, enum ranges, vector dimensions, truncated streams).

### 27.3 Module Type Callbacks & Accurate Memory Accounting
- Implement missing `RedisModuleTypeMethods` callbacks on `GmSearch-`:
  `digest`, `copy2`, `free_effort2`, `defrag`.
- Replace `SearchMemUsage`'s `doc_store.Size() * 256` placeholder with real memory accounting across
  `DocumentStore`, `TagIndex`, `NumericIndex`, `TextIndex`, `VectorIndex`/`HnswIndex`, `GeoIndex`, and
  `GeoShapeIndex`.

### 27.4 Tests
- Expand `search_rdb_test.cc` (currently 5 tests) to cover: full schema/option round-trip, global aux state
  round-trip, malformed/truncated RDB rejection, and `digest`/`copy2`/`free_effort2` callbacks.
- Add TCL tests for >10 fields AOF rewrite + restart, and alias/suggest/dict/synonym persistence across restart.

---

## Phase 28 — Modular Refactoring of `search_commands.cc`

**Goal**: Break up the 3,296-line `search_commands.cc` monolith into focused, auditable translation units
without breaking the cross-platform linking rule (pure-core headers must never pull in `redismodule.h`).

**Proposed module layout**:
- `search_state.h/.cc` — `IndexEntry`, global registries (`g_indices`, `g_aliases`, `g_cursors`,
  `g_suggest_dicts`, `g_term_dicts`, `g_config`), helper lookup/indexing routines
- `search_index_cmds.cc` — `FT.CREATE`, `FT.ALTER`, `FT.DROPINDEX`, `FT.INFO`, `FT._LIST`, `FT.ALIAS*`, `FT.TAGVALS`, `FT.ADD`, `FT.DEL`
- `search_query_cmds.cc` — `FT.SEARCH`, `FT.EXPLAIN`, `FT.EXPLAINCLI`, `FT.PROFILE`, highlight/summarize
- `search_agg_cmds.cc` — `FT.AGGREGATE`, `FT.CURSOR`, pipeline stages & reducers
- `search_aux_cmds.cc` — `FT.SUG*`, `FT.DICT*`, `FT.SYN*`, `FT.SPELLCHECK`, `FT.CONFIG`, `FT._DEBUG`
- `search_keyspace.cc` — `ON HASH` / `ON JSON` keyspace & server event handlers

**Verification**: Zero behavior change; full `search_test` (GTest) + `search_test.tcl` + ASan build pass.

---

## Phase 29 — Complete Partial Options & Runtime Lifecycle

**Goal**: Make every accepted option in `FT.CREATE` / `FT.SEARCH` / `FT.AGGREGATE` / `FT.PROFILE` / `FT.CURSOR`
genuinely enforced.

- **`TEMPORARY <seconds>`**: Use `RedisModule_CreateTimer` to track index idle time and automatically drop
  expired temporary indices.
- **`FT.CURSOR MAXIDLE`**: Enforce idle expiration (lazy check on `READ` + periodic timer sweep) and cap max
  active cursors per index to prevent memory leaks.
- **`DIALECT 1 | 2`**: Wire `opts.dialect` (and `DEFAULT_DIALECT` from `FT.CONFIG`) into `ParseQuery` so
  Dialect 1 vs Dialect 2 negation scoping and `PARAMS` handling actually differ as documented.
- **`FT.PROFILE`**: Support full `FT.SEARCH` options (`SORTBY`, `LIMIT`, `PARAMS`, `NOCONTENT`, etc.) and
  real `FT.AGGREGATE` pipeline execution with per-stage timing breakdown.
- **Cooperative `TIMEOUT`**: Pass a deadline into `EvaluateQuery` / `HnswIndex` / aggregation loops so slow
  queries abort promptly instead of only checking after full evaluation.
- **`PHONETIC` query-time wiring**: Wire `TextIndex::PhoneticLookup` / `PhoneticSearch` into `EvaluateQuery`
  for `TEXT` fields configured with `PHONETIC` (currently indexed at write time but not queried in `FT.SEARCH`).

---

## Phase 30 — Keyspace & Server Event Completeness

**Goal**: Keep `ON HASH` and `ON JSON` indices consistent under all Redis 7 key lifecycle operations.

- Subscribe to Redis 7 server events via `RedisModule_SubscribeToServerEvent`:
  - `REDISMODULE_EVENT_FLUSHDB` — clear indexed docs (and/or reset indices according to policy) on `FLUSHDB`/`FLUSHALL`.
  - `REDISMODULE_EVENT_LOADING` — avoid redundant keyspace re-indexing during RDB/AOF load.
- Expand `OnKeyspaceEvent` coverage:
  - Handle `rename_from`, `rename_to`, `unlink`, `restore`, `copy_to`.
  - Handle remaining `gemini-json` mutation events (`json.merge`, `json.clear`, `json.arrpop`, `json.arrtrim`, `json.toggle`, `json.nummultby`).
- Add TCL integration tests loading both `redis_json.so` and `redis_search.so` together to verify `ON JSON`
  end-to-end across create/update/delete/rename/flush/restart.

---

## Phase 31 — Redis 7 Production Verification & Design Doc

**Goal**: Bring `gemini-search` to the same verification bar as `gemini-bloom` on Redis 7.

- **Soak test**: Add `ci/search_soak_test.py` (continuous mixed `FT.SEARCH` / `FT.AGGREGATE` / `HSET` / `DEL` /
  HNSW insert+delete workload) checking correctness and bounded memory growth.
- **ASan CI**: Run `search_test` under `-DENABLE_ASAN=ON` in Redis 7 CI.
- **RESP3 coverage**: Verify `HELLO 3` behavior across `FT.SEARCH`, `FT.AGGREGATE`, `FT.INFO`, `FT.PROFILE`.
- **Design documentation**: Write `modules/gemini-search/DESIGN.md` covering architecture, clean-room
  boundaries, RDB/aux format, resource limits, and known limitations.

---

## Milestone Summary

| Phase | Focus | Priority | Status |
|---|---|---|---|
| **P27** | RDB/AOF completeness, bounds checks, ModuleType callbacks, real `SearchMemUsage` | P0 (Data safety) | Done |
| **P28** | Split `search_commands.cc` monolith into focused modules | P1 (Maintainability) | Done |
| **P29** | Enforce `TEMPORARY`, `CURSOR MAXIDLE`, `DIALECT`, `FT.PROFILE`, cooperative `TIMEOUT`, `PHONETIC` | P1 (Correctness) | Done |
| **P30** | Keyspace (`RENAME`/`UNLINK`/`RESTORE`/JSON) + `FLUSHDB` server events + `ON JSON` integration | P1 (Consistency) | Done |
| **P31** | Redis 7 Soak test, ASan CI, RESP3 tests, `DESIGN.md` | P2 (Production bar) | Done |
