# gemini-search Development Plan

A phased plan for building a Redis search module. Each phase produces a
loadable `.so` that can be tested with `redis-cli`.

## Phase 0 — Module Skeleton

**Goal**: Empty module that loads into Redis and responds to a debug command.

**Deliverables**:
- `modules/gemini-search/` directory + CMakeLists.txt
- `redis_search_module.cc` entry point, module name `GeminiSearch`
- `FT._DEBUG` command returns `"GeminiSearch OK"`

**Test**:
```
redis-server --loadmodule ./redis_search.so
redis-cli FT._DEBUG   → "GeminiSearch OK"
```

---

## Phase 1 — Index Schema Management

**Goal**: Create / drop / inspect index definitions (metadata only, no data
indexing yet).

**Commands**:
| Command | Description |
|---------|-------------|
| `FT.CREATE idx SCHEMA name TAG price NUMERIC` | Create index with schema |
| `FT.DROPINDEX idx` | Drop index |
| `FT.INFO idx` | Show schema and stats |

**Data structures**:
- `IndexSpec` — index name, field list (name + type enum TAG/NUMERIC)
- `IndexRegistry` — global name → IndexSpec map

**Unit tests**:
- Create / find / delete IndexSpec
- Duplicate name error
- Drop nonexistent index error

---

## Phase 2 — TAG Inverted Index + Exact Query

**Goal**: Index documents and search by TAG exact match.

**Commands**:
| Command | Description |
|---------|-------------|
| `FT.ADD idx doc1 FIELDS status active region us` | Add document |
| `FT.SEARCH idx "@status:{active}"` | TAG exact query |
| `FT.DEL idx doc1` | Remove document from index |

**Data structures**:
- `InvertedIndex` — `tag_value → sorted vector<doc_id>`
- `TagIndex` — one InvertedIndex per TAG field
- `DocumentStore` — `doc_id → {field: value}` for result retrieval

**Query syntax**:
- `@field:{value}` — single value
- `@field:{val1|val2}` — multi-value OR within a TAG

**Unit tests**:
- Single/multi TAG field write + query
- Missing tag returns empty
- Delete removes from results
- Multi-value OR syntax

---

## Phase 3 — NUMERIC Range Index

**Goal**: Range queries on numeric fields.

**Data structures**:
- `NumericIndex` — ordered map or sorted array, supports lower_bound /
  upper_bound range scan

**Query syntax**:
- `@price:[100 500]` — closed interval
- `@price:[(100 (500]` — open boundaries (optional)
- `@price:[-inf 500]` / `@price:[100 +inf]` — unbounded

**Unit tests**:
- Basic range query
- Boundary values
- Empty result
- `-inf` / `+inf`

---

## Phase 4 — Boolean Combination Queries

**Goal**: AND / OR / NOT across multiple conditions.

**Core modules**:
- **Query parser** — tokenize + parse query string into tree
- **QueryNode** — AND / OR / NOT / TagMatch / NumericRange variants
- **Set ops** — intersect / union on sorted doc-id lists

**Syntax**:
- `@a:{x} @b:[1 10]` — AND (space-separated)
- `@a:{x} | @b:{y}` — OR
- `-@a:{x}` — NOT
- `(@a:{x} | @b:{y}) @c:[0 100]` — parenthesised grouping

**Unit tests**:
- Parser: various strings → QueryNode tree
- intersect / union algorithm
- End-to-end combined queries

---

## Phase 5 — RETURN / SORTBY / LIMIT

**Goal**: Result formatting options.

**Syntax**:
```
FT.SEARCH idx "@status:{active}"
    RETURN 2 name price
    SORTBY price ASC
    LIMIT 0 10
```

**Unit tests**:
- RETURN projects only named fields
- SORTBY ASC / DESC
- LIMIT offset + count
- All three combined

---

## Phase 6 — RDB Persistence

**Goal**: Survive Redis restart via RDB save/load.

**Implementation**:
- `RdbSave` — serialize IndexRegistry + all index data
- `RdbLoad` — deserialize and restore
- Follow gemini-bloom / gemini-json RDB patterns

**Unit tests**:
- Mock RDB IO round-trip
- Mixed TAG + NUMERIC index data

---

## Phase 7 — Future Extensions (pick by interest)

| Direction | Notes |
|-----------|-------|
| GEO index | Lat/lon field + radius query |
| Full-text search | Tokenizer + inverted index + BM25 scoring |
| FT.AGGREGATE | GROUPBY + REDUCE pipeline |
| Auto-indexing | Keyspace notification on HSET → auto-index |
| Suggestion / autocomplete | `FT.SUGADD` / `FT.SUGGET` via trie |

---

## Milestone Summary

| Phase | Core Capability | Approx. Size |
|-------|----------------|--------------|
| P0 | Empty module skeleton | ~3 files |
| P1 | Schema management | ~4 files |
| P2 | TAG exact query | ~5 files — first real search |
| P3 | NUMERIC range query | ~2 new files |
| P4 | Boolean combinations | ~3 new files (parser is the bulk) |
| P5 | RETURN / SORT / LIMIT | Mostly edits, little new code |
| P6 | RDB persistence | ~2 new files |

P0–P3 = minimum viable search engine.
P4 = practical query power.
P5–P6 = polish.
