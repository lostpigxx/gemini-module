# gemini-search Development Plan

A phased plan for building a Redis search module. Each phase produces a
loadable `.so` that can be tested with `redis-cli`.

**Clean-room policy**: All implementations are written from scratch based
on publicly documented command interfaces only. No RediSearch source code
is referenced. Internal data structures and algorithms are independently
designed.

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

## Phase 4 — VECTOR FLAT Index (Brute-Force KNN)

**Goal**: Store fixed-dimension float vectors and support exact K-nearest
neighbour queries via brute-force scan.

**Schema extension**:
```
FT.CREATE idx SCHEMA ... embedding VECTOR FLAT DIM 128 DISTANCE_METRIC L2
```

New field type `VECTOR` with required parameters:
- Algorithm: `FLAT` (brute-force, this phase only)
- `DIM <n>` — vector dimensionality (fixed per field)
- `DISTANCE_METRIC <L2|COSINE|IP>` — distance function

**Query syntax** (hybrid search — append vector query to a filter):
```
FT.SEARCH idx "*=>[KNN 5 @embedding $blob]" PARAMS 2 blob <binary>
```

- `*` or any filter expression as pre-filter
- `=>[KNN k @field $param]` as the vector query suffix
- `PARAMS <n> <name> <value> ...` passes the binary query vector
- Results include a `__vec_score` pseudo-field with the distance

**Data structures**:
- `VectorFieldSpec` — extends FieldSpec with dim, metric, algorithm
- `FlatVectorIndex` — stores `doc_id → float[]` pairs, scans all on query
- Distance functions: L2 (Euclidean), cosine similarity, inner product

**Unit tests**:
- Store and retrieve vectors of various dimensions
- KNN with L2 / cosine / IP returns correct top-k ordering
- Dimension mismatch on insert → error
- KNN k > total docs → returns all docs
- Empty index → empty result
- Pre-filter + KNN combination
- ASAN: repeated insert/delete cycles, large vectors, high dimensionality

---

## Phase 5 — Boolean Combination Queries

**Goal**: AND / OR / NOT across multiple conditions.

**Core modules**:
- **Query parser** — tokenize + parse query string into tree
- **QueryNode** — AND / OR / NOT / TagMatch / NumericRange / VectorKNN variants
- **Set ops** — intersect / union on sorted doc-id lists

**Syntax**:
- `@a:{x} @b:[1 10]` — AND (space-separated)
- `@a:{x} | @b:{y}` — OR
- `-@a:{x}` — NOT
- `(@a:{x} | @b:{y}) @c:[0 100]` — parenthesised grouping
- `@a:{x} =>[KNN 5 @vec $blob]` — filter + vector KNN

**Unit tests**:
- Parser: various strings → QueryNode tree
- intersect / union algorithm
- End-to-end combined queries including vector conditions

---

## Phase 6 — RETURN / SORTBY / LIMIT

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

## Phase 7 — RDB Persistence

**Goal**: Survive Redis restart via RDB save/load.

**Implementation**:
- `RdbSave` — serialize IndexRegistry + all index data (TAG, NUMERIC,
  VECTOR indices and document store)
- `RdbLoad` — deserialize and restore
- Follow gemini-bloom / gemini-json RDB patterns

**Unit tests**:
- Mock RDB IO round-trip
- Mixed TAG + NUMERIC + VECTOR index data

---

## Phase 8 — Auto-Indexing (ON HASH)

**Goal**: Automatically index Redis Hash keys matching a prefix, without
requiring explicit `FT.ADD`. This is the single most impactful usability
improvement — it makes gemini-search work like RediSearch out of the box.

**Schema extension**:
```
FT.CREATE idx ON HASH PREFIX 1 product: SCHEMA name TAG price NUMERIC
```

**Implementation**:
- Subscribe to keyspace notifications for HSET/HDEL/DEL on matching prefixes
  via `RedisModule_SubscribeToKeyspaceEvents`
- On HSET: read Hash fields, update index (equivalent to FT.ADD)
- On DEL: remove from index (equivalent to FT.DEL)
- On module load: scan existing keys matching prefix and index them
- `FT.ADD` remains available for non-Hash use cases

**Unit tests**:
- HSET triggers indexing, FT.SEARCH finds the doc
- HDEL/DEL removes from index
- Multiple prefixes
- Prefix mismatch does not trigger indexing
- Module load scans existing keys

---

## Phase 9 — Full-Text Search

**Goal**: Tokenized text search with relevance scoring. This is the largest
single feature — comparable in scope to P0–P3 combined.

**Schema extension**:
```
FT.CREATE idx SCHEMA ... title TEXT description TEXT
```

**Core modules**:
- **Tokenizer** — whitespace + punctuation splitting, Unicode-aware,
  lowercase normalization
- **Stopwords** — configurable stop word list (default: English common words)
- **Inverted index** — term → posting list with term frequency per doc
- **BM25 scoring** — standard BM25 relevance ranking (clean-room, based on
  the public BM25 formula from Robertson & Zaragoza)

**Query syntax**:
- `@title:{hello world}` is TAG exact match; TEXT uses different syntax:
- `hello world` — full-text search across all TEXT fields
- `@title:hello` — full-text search in specific field
- Combine with existing: `@title:hello @price:[0 100]`

**Sub-phases** (can be split further):
1. Tokenizer + inverted index + basic term matching
2. BM25 scoring + result ranking
3. Stopwords + stemming (optional, language-dependent)
4. Phrase queries (`"hello world"` as exact phrase)
5. Prefix/wildcard queries (`hel*`)

---

## Phase 10 — KNN Pre-Filter + AOF Rewrite

**Goal**: Fix two gaps in the current implementation.

### KNN Pre-Filter

Currently KNN only supports `*` as pre-filter. This phase adds support for
arbitrary boolean pre-filters before KNN:
```
FT.SEARCH idx "@category:{shoes} =>[KNN 5 @embedding $blob]" PARAMS 2 blob <binary>
```

**Implementation**:
- Evaluate the pre-filter query tree to get a candidate doc_id set
- Pass candidate set to FlatVectorIndex (or future HNSW) — only compute
  distances for docs in the candidate set
- Add `KnnQueryFiltered(query, k, candidate_ids)` to FlatVectorIndex

### AOF Rewrite

Currently `AofRewriteSearch` is a no-op. Implement it so indices survive
AOF-only persistence mode.

**Implementation**:
- For each document in the index, emit `FT.ADD` commands in the AOF
- Emit `FT.CREATE` first, then all `FT.ADD` commands

**Unit tests**:
- TCL: filtered KNN returns correct subset
- TCL: AOF rewrite + restart preserves data (appendonly yes)

---

## Phase 11 — VECTOR HNSW Index (Approximate Nearest Neighbour)

**Goal**: Approximate K-nearest neighbour search using a Hierarchical
Navigable Small World (HNSW) graph for practical performance on large
datasets.

**Schema extension**:
```
FT.CREATE idx SCHEMA ... embedding VECTOR HNSW DIM 128 DISTANCE_METRIC L2 M 16 EF_CONSTRUCTION 200
```

New algorithm `HNSW` with additional parameters:
- `M <n>` — max number of bi-directional links per node per layer
  (default 16)
- `EF_CONSTRUCTION <n>` — size of the dynamic candidate list during
  index construction (default 200)

**Query-time parameter**:
```
FT.SEARCH idx "*=>[KNN 10 @embedding $blob EF_RUNTIME 100]"
```
- `EF_RUNTIME <n>` — candidate list size during search (trade-off between
  speed and recall, default 10)

**Core algorithm** (clean-room, based on the public HNSW paper
[arXiv:1603.09320]):
- Multi-layer graph: each node appears on layer 0, promoted to higher
  layers with exponentially decreasing probability
- Insert: greedy search from top layer down, connect to M nearest
  neighbours on each layer the node occupies
- Search: greedy search from top layer, expand candidate list on layer 0
  with beam width = EF_RUNTIME
- Delete: mark as tombstone, optionally repair neighbour links

**Data structures**:
- `HnswGraph` — layered adjacency list, per-node vector storage
- `HnswNode` — vector data, neighbour lists per layer, max layer
- Reuse distance functions from Phase 4

**Unit tests**:
- Recall measurement: KNN on random data, compare HNSW results against
  brute-force ground truth, expect recall > 0.95 at default parameters
- Insert / delete / re-insert cycles
- Varying M and EF_CONSTRUCTION values
- Single-element and empty-index edge cases
- ASAN: concurrent-style insert stress, large graph build/teardown
- TCL: end-to-end with redis-cli, compare HNSW vs FLAT results

---

## Phase 12 — FT.AGGREGATE

**Goal**: Aggregation pipeline for analytics queries.

**Syntax**:
```
FT.AGGREGATE idx "@status:{active}"
    GROUPBY 1 @category
    REDUCE COUNT 0 AS count
    REDUCE AVG 1 @price AS avg_price
    SORTBY 2 @count DESC
    LIMIT 0 10
```

**Pipeline stages**:
- `GROUPBY <n> @field ...` — group results by field values
- `REDUCE <func> <nargs> @field ... AS alias` — aggregate functions:
  COUNT, SUM, AVG, MIN, MAX, COUNT_DISTINCT
- `SORTBY` / `LIMIT` — reuse from Phase 6
- `APPLY <expr> AS alias` — computed fields (optional)

---

## Milestone Summary

| Phase | Core Capability | Status |
|-------|----------------|--------|
| P0 | Empty module skeleton | Done |
| P1 | Schema management | Done |
| P2 | TAG exact query | Done |
| P3 | NUMERIC range query | Done |
| P4 | VECTOR FLAT (brute-force KNN) | Done |
| P5 | Boolean combinations | Done |
| P6 | RETURN / SORT / LIMIT | Done |
| P7 | RDB persistence | Done |
| P8 | Auto-indexing (ON HASH) | Planned |
| P9 | Full-text search (TEXT + BM25) | Planned |
| P10 | KNN pre-filter + AOF rewrite | Planned |
| P11 | VECTOR HNSW (approximate KNN) | Planned |
| P12 | FT.AGGREGATE | Planned |

P0–P7 = complete, functional search engine.
P8 = usability — write Hash, auto-index.
P9 = RediSearch's core selling point — full-text search.
P10–P11 = production-grade vector search.
P12 = analytics queries.
