# gemini-search Development Plan v2 — Phases 13–24

Continuation of the original development plan. Phases 0–12 are complete and
cover: module skeleton, schema management, TAG/NUMERIC/TEXT/VECTOR field
types, boolean queries, RETURN/SORTBY/LIMIT, RDB/AOF persistence, ON HASH
auto-indexing, BM25 full-text search, KNN pre-filter, HNSW, and
FT.AGGREGATE.

This plan covers the remaining feature gaps compared to RediSearch, ordered
by user impact and dependency chain.

**Clean-room policy** continues to apply: all implementations are based on
publicly documented command interfaces only. No RediSearch source code is
referenced.

---

## Phase 13 — Search Options Enhancement

**Goal**: Add commonly used FT.SEARCH options that control result format and
query behaviour.

**New options**:
| Option | Description |
|--------|-------------|
| `NOCONTENT` | Return only doc IDs, no field values |
| `WITHSCORES` | Include BM25/vector relevance score per result |
| `VERBATIM` | Disable stemming (future-proof, no-op until Phase 16) |
| `NOSTOPWORDS` | Disable stop word filtering |
| `INFIELDS count field ...` | Restrict TEXT search to specific fields |
| `INKEYS count key ...` | Restrict search to specific document keys |
| `TIMEOUT ms` | Per-query timeout |

**Implementation notes**:
- WITHSCORES requires threading the BM25 score (already computed in Phase 9)
  and vector distance score through to the result formatter
- NOCONTENT is a trivial projection change — skip field emission
- INFIELDS narrows the TEXT posting list union to named fields only
- INKEYS pre-filters the candidate set before query evaluation

**Unit tests**:
- NOCONTENT returns IDs only
- WITHSCORES returns numeric score per doc
- INFIELDS limits which TEXT fields are searched
- INKEYS limits which document keys participate
- NOSTOPWORDS: stop words are indexed and searchable
- TIMEOUT: long query terminates with error

---

## Phase 14 — Prefix Search + Fuzzy Match + Optional Terms

**Goal**: Expand query syntax with the most commonly used full-text search
operators.

**Query syntax additions**:
| Syntax | Description |
|--------|-------------|
| `hel*` | Prefix — match all terms starting with `hel` (min 2 chars) |
| `%hello%` | Fuzzy — Levenshtein distance 1 |
| `%%hello%%` | Fuzzy — Levenshtein distance 2 |
| `%%%hello%%%` | Fuzzy — Levenshtein distance 3 (max) |
| `foo ~bar` | Optional — `bar` boosts score but is not required |

**Data structures**:
- **Term dictionary trie** — built from the TEXT inverted index term set,
  used for prefix expansion and fuzzy candidate generation
- Levenshtein automaton or brute-force dictionary scan for fuzzy matching
  (dictionary is typically small enough for brute-force)

**Implementation notes**:
- Prefix: walk trie to prefix node, collect all descendants, union their
  posting lists
- Fuzzy: iterate dictionary, compute edit distance, union posting lists of
  matches within threshold
- Optional: evaluate as OR but with reduced weight — doc matching optional
  term gets a score boost, doc without it still appears

**Unit tests**:
- Prefix matches correct terms, min-length enforced
- Fuzzy LD=1/2/3 returns expected matches
- Optional term boosts but does not filter
- Combined: `hel* ~world %fuzz%`
- Empty prefix / no fuzzy matches → graceful handling

---

## Phase 15 — Exact Phrase + Proximity Search

**Goal**: Support quoted phrase queries and proximity matching via positional
information in the posting list.

**Query syntax additions**:
| Syntax | Description |
|--------|-------------|
| `"hello world"` | Exact phrase — terms must be adjacent and in order |
| `SLOP n` | Allow up to n intervening terms between query terms |
| `INORDER` | Terms must appear in query order (with SLOP) |

**Data structure changes**:
- Extend TEXT posting list from `{doc_id, term_freq}` to
  `{doc_id, term_freq, positions: vector<uint32>}`
- Positions are token offsets within the field (0-based)

**Implementation notes**:
- Phrase query: intersect posting lists, then verify position adjacency
  within each doc
- SLOP: relax adjacency check to allow gaps ≤ SLOP
- INORDER: position of term[i+1] must be > position of term[i]
- Storage overhead: ~4 bytes per term occurrence for position data

**Unit tests**:
- Exact phrase matches only adjacent terms
- Phrase across field boundary does not match
- SLOP 0 = exact phrase, SLOP 1 allows one gap
- INORDER rejects reverse-order matches
- Multi-word phrase with repeated terms
- Phrase + boolean combination: `"hello world" @tag:{active}`

---

## Phase 16 — Stemming + Multi-Language Support

**Goal**: Language-aware stemming for query expansion and index-time term
normalization.

**New options**:
| Location | Option | Description |
|----------|--------|-------------|
| FT.CREATE | `LANGUAGE lang` | Default language for the index |
| FT.CREATE | `LANGUAGE_FIELD field` | Per-document language field |
| FT.SEARCH | `LANGUAGE lang` | Override language for this query |
| Field attr | `NOSTEM` | Disable stemming for this field |

**Implementation**:
- Clean-room Snowball-style stemmer for English (most common case)
- At index time: store both original and stemmed form in the inverted index
- At query time: stem query terms, expand search to stemmed forms
- VERBATIM (Phase 13) disables stemming for the query
- Support at minimum: English. Additional languages can be added
  incrementally

**Supported languages** (initial):
- English (required)
- Extensible architecture for adding more languages later

**Unit tests**:
- "running" matches "run", "runs", "running"
- VERBATIM disables stemming
- NOSTEM field is not stemmed
- LANGUAGE override at query time
- Stemmer does not corrupt non-English text

---

## Phase 17 — GEO Field Type + GEOFILTER

**Goal**: Geospatial point indexing with radius queries.

**Schema extension**:
```
FT.CREATE idx SCHEMA ... location GEO
```

**Data structures**:
- `GeoIndex` — store `{doc_id, lon, lat}` tuples
- Spatial indexing via geohash bucketing or R-tree for efficient radius
  queries
- Haversine formula for distance calculation on the sphere

**Query syntax**:
- `@location:[lon lat radius unit]` — radius query in query string
- `GEOFILTER field lon lat radius m|km|mi|ft` — FT.SEARCH option
- Distance units: `m` (meters), `km`, `mi` (miles), `ft` (feet)

**Implementation notes**:
- GEO field value format: `"lon,lat"` string (e.g. `"-104.99,39.74"`)
- Combinable with boolean queries: `@category:{restaurant} @location:[...] `
- Results can include `__geo_distance` pseudo-field (optional)

**Unit tests**:
- Store points, radius query returns correct docs
- Distance units m/km/mi/ft
- Boundary: point exactly on radius edge
- Empty result: no points within radius
- Combined with TAG/NUMERIC filters
- Invalid coordinates → error
- RDB/AOF round-trip for GEO data

---

## Phase 18 — Aggregation Pipeline Enhancement

**Goal**: Full-featured aggregation pipeline with expressions, inline
filtering, field loading, and additional reducer functions.

**New pipeline stages**:
| Stage | Description |
|-------|-------------|
| `LOAD n @field ...` | Load additional fields into the pipeline |
| `APPLY "expr" AS alias` | Compute expression, store as new field |
| `FILTER "expr"` | Filter rows mid-pipeline by expression |

**New reducer functions**:
| Reducer | Description |
|---------|-------------|
| `STDDEV` | Standard deviation of numeric field |
| `QUANTILE field pct` | Value at given percentile (0–1) |
| `TOLIST` | Collect all values into an array |
| `FIRST_VALUE field [BY field ASC\|DESC]` | First value in group |
| `RANDOM_SAMPLE field count` | Random sample from group |
| `COUNT_DISTINCTISH` | Approximate distinct count (HyperLogLog) |

**Expression engine** (for APPLY / FILTER):
- Arithmetic: `+`, `-`, `*`, `/`, `%`, `^`
- Math functions: `sqrt()`, `log()`, `log2()`, `abs()`, `ceil()`,
  `floor()`, `round()`
- String functions: `lower()`, `upper()`, `strlen()`, `substr()`,
  `format()`
- Conditional: `if(cond, then, else)`
- Comparison: `==`, `!=`, `<`, `>`, `<=`, `>=`
- Logical: `&&`, `||`, `!`
- Field references: `@field_name`

**Multi-stage pipeline**:
```
FT.AGGREGATE idx "*"
    LOAD 2 @name @price
    APPLY "@price * 1.1" AS price_with_tax
    FILTER "@price_with_tax > 100"
    GROUPBY 1 @category
    REDUCE AVG 1 @price_with_tax AS avg_taxed
    SORTBY 2 @avg_taxed DESC
    LIMIT 0 10
```

- Stages can be chained: GROUPBY → APPLY → GROUPBY → ...
- Each stage transforms the row set for the next stage

**Unit tests**:
- APPLY arithmetic and function expressions
- FILTER removes rows mid-pipeline
- LOAD brings in non-indexed fields
- Multi-stage: GROUPBY → APPLY → GROUPBY
- Each new reducer function with edge cases
- COUNT_DISTINCTISH approximate accuracy (within ~5%)
- QUANTILE at 0, 0.5, 1.0 boundaries
- Expression parse errors → clear error message

---

## Phase 19 — FT.ALTER + HIGHLIGHT + SUMMARIZE

**Goal**: Dynamic schema evolution and result presentation features.

### FT.ALTER

```
FT.ALTER idx SCHEMA ADD description TEXT new_tag TAG
```

- Add new fields to an existing index schema
- Cannot remove or modify existing fields
- New fields are empty for existing docs until re-indexed
- ON HASH indexes: existing docs re-scanned for new fields

### HIGHLIGHT

```
FT.SEARCH idx "hello world"
    HIGHLIGHT FIELDS 2 title body TAGS <b> </b>
```

- Wrap matched terms in open/close tags within result fields
- Default tags: `<b>` / `</b>`
- Requires positional info from Phase 15

### SUMMARIZE

```
FT.SEARCH idx "hello world"
    SUMMARIZE FIELDS 1 body FRAGS 3 LEN 20 SEPARATOR "..."
```

- Extract text fragments containing matched terms
- FRAGS: number of fragments (default 3)
- LEN: fragment length in words (default 20)
- SEPARATOR: between fragments (default "...")

**Unit tests**:
- ALTER adds field, new docs use it, old docs unaffected
- ALTER duplicate field name → error
- HIGHLIGHT wraps matched terms with custom tags
- SUMMARIZE returns fragments around matches
- HIGHLIGHT + SUMMARIZE combined
- ALTER + ON HASH: re-scan picks up new field from existing hashes

---

## Phase 20 — ON JSON Support

**Goal**: Index JSON documents stored via the RedisJSON module, using
JSONPath expressions in the schema.

**Schema extension**:
```
FT.CREATE idx ON JSON PREFIX 1 product: SCHEMA
    $.name AS name TEXT
    $.price AS price NUMERIC
    $.tags[*] AS tags TAG
    $.location AS location GEO
```

**Implementation notes**:
- Detect RedisJSON module at load time via `RedisModule_GetSharedAPI` or
  command probing
- On keyspace event: call `JSON.GET` to read fields via JSONPath
- JSONPath → field value mapping at index time
- If RedisJSON is not loaded, `ON JSON` returns an error
- Array results from JSONPath: index each element (e.g. multi-value TAG)

**Supported JSONPath patterns**:
- `$.field` — root-level field
- `$.nested.field` — nested field
- `$.array[*]` — all array elements
- `$.array[0]` — specific index

**Unit tests**:
- JSON doc indexed via keyspace event
- JSONPath extracts nested fields correctly
- Array fields produce multiple index entries
- JSON.SET update triggers re-index
- JSON.DEL removes from index
- ON JSON without RedisJSON loaded → error
- RDB/AOF round-trip for JSON-indexed data

---

## Phase 21 — Index Management Commands

**Goal**: Utility commands for index management, debugging, and aliasing.

**New commands**:
| Command | Description |
|---------|-------------|
| `FT.ALIASADD alias idx` | Create alias pointing to index |
| `FT.ALIASDEL alias` | Delete an alias |
| `FT.ALIASUPDATE alias idx` | Update alias to point to different index |
| `FT.TAGVALS idx field` | Return all distinct values of a TAG field |
| `FT.EXPLAIN idx query` | Return query execution plan as string |
| `FT.EXPLAINCLI idx query` | Execution plan in CLI-friendly indented format |

**Implementation notes**:
- Aliases: global `alias → index_name` map, resolve alias before any
  command execution
- TAGVALS: iterate the TAG inverted index keys
- EXPLAIN: parse query into tree, serialize tree to human-readable string
  showing node types, field names, and operators

**Unit tests**:
- Alias CRUD cycle: add, search via alias, update, delete
- Alias to nonexistent index → error
- TAGVALS returns sorted distinct values
- TAGVALS on non-TAG field → error
- EXPLAIN output matches expected tree structure
- Alias survives RDB save/load

---

## Phase 22 — SORTABLE + Advanced Index Creation Options

**Goal**: Performance-oriented field attributes and index-level options.

**New field attributes**:
| Attribute | Description |
|-----------|-------------|
| `SORTABLE` | Pre-build sort index for low-latency SORTBY |
| `NOINDEX` | Store field value but do not index (for RETURN only) |
| `NOSTEM` | Disable stemming for this TEXT field (Phase 16 dep) |
| `WEIGHT w` | Scoring weight multiplier for TEXT field |

**New index creation options**:
| Option | Description |
|--------|-------------|
| `STOPWORDS n word ...` | Custom stop word list (0 = none) |
| `TEMPORARY seconds` | Auto-expire index after idle period |
| `MAXTEXTFIELDS` | Allow >32 TEXT fields |
| `NOFREQS` | Don't store term frequencies (saves memory) |
| `NOOFFSETS` | Don't store term offsets (disables phrase/highlight) |
| `NOHL` | Disable highlight support |
| `FILTER expr` | Index-time filter expression |

**Implementation notes**:
- SORTABLE: maintain a parallel sorted column per SORTABLE field; SORTBY
  on a SORTABLE field uses this instead of re-sorting results
- TEMPORARY: use Redis timer to check idle time, drop index after expiry
- NOFREQS/NOOFFSETS: posting list variants with reduced storage
- WEIGHT: multiply BM25 field score by weight factor

**Unit tests**:
- SORTABLE field: SORTBY performance / correctness
- NOINDEX field: not searchable but returned in results
- Custom STOPWORDS override default list
- TEMPORARY index auto-expires
- NOFREQS disables term frequency storage
- WEIGHT affects scoring order

---

## Phase 23 — Cursor Pagination

**Goal**: Server-side cursor for iterating large FT.AGGREGATE result sets.

**Commands**:
| Command | Description |
|---------|-------------|
| `FT.AGGREGATE ... WITHCURSOR [COUNT n] [MAXIDLE ms]` | Start cursor |
| `FT.CURSOR READ idx cursor_id [COUNT n]` | Read next batch |
| `FT.CURSOR DEL idx cursor_id` | Delete cursor early |

**Implementation notes**:
- Cursor holds a snapshot of the aggregation pipeline result
- COUNT controls batch size (default: 1000)
- MAXIDLE: auto-delete cursor after idle timeout (default: 300000 ms)
- Cursor ID: 64-bit integer, globally unique
- Memory management: cap total active cursors per index

**Unit tests**:
- Full iteration via repeated CURSOR READ
- COUNT controls batch size
- MAXIDLE triggers auto-cleanup
- CURSOR DEL works mid-iteration
- Cursor on empty result
- Multiple concurrent cursors on same index

---

## Phase 24 — Autocomplete + Spellcheck + Synonyms + Dictionaries

**Goal**: Search quality features — suggestions, spelling correction,
synonym expansion, and custom dictionaries.

### Autocomplete

| Command | Description |
|---------|-------------|
| `FT.SUGADD dict string score [INCR] [PAYLOAD payload]` | Add suggestion |
| `FT.SUGGET dict prefix [FUZZY] [WITHSCORES] [WITHPAYLOADS] [MAX n]` | Get suggestions |
| `FT.SUGDEL dict string` | Delete suggestion |
| `FT.SUGLEN dict` | Count entries |

- Trie-based suggestion dictionary (separate from index)
- FUZZY: Levenshtein distance 1 matching on prefix
- INCR: increment score instead of replacing

### Spellcheck

| Command | Description |
|---------|-------------|
| `FT.SPELLCHECK idx query [DISTANCE d] [TERMS INCLUDE/EXCLUDE dict]` | Spelling correction |

- For each query term, find closest matches in the index dictionary
- Use custom dictionaries to include/exclude terms

### Synonyms

| Command | Description |
|---------|-------------|
| `FT.SYNUPDATE idx group_id term ...` | Add terms to synonym group |
| `FT.SYNDUMP idx` | Dump all synonym groups |

- At query time: expand terms to include synonyms from the same group
- Synonym groups are per-index

### Dictionaries

| Command | Description |
|---------|-------------|
| `FT.DICTADD dict term ...` | Add terms to dictionary |
| `FT.DICTDEL dict term ...` | Delete terms |
| `FT.DICTDUMP dict` | List all terms |

- Global dictionaries used by SPELLCHECK and SUGGET

**Unit tests**:
- SUGADD/SUGGET prefix completion
- SUGGET FUZZY handles typos
- SUGDEL removes entries
- SPELLCHECK suggests corrections
- Synonym expansion in queries
- DICTADD/DICTDEL/DICTDUMP CRUD
- RDB persistence for suggestion dictionaries and synonym groups

---

## Phase 25 — GEOSHAPE Field Type

**Goal**: Advanced geospatial indexing with polygon shapes.

**Schema extension**:
```
FT.CREATE idx SCHEMA ... area GEOSHAPE [FLAT|SPHERICAL]
```

**Supported WKT primitives**:
- `POINT(lon lat)` — single point
- `POLYGON((lon1 lat1, lon2 lat2, ..., lon1 lat1))` — closed polygon
  (with optional holes)

**Query operations**:
| Operator | Description |
|----------|-------------|
| `@area:[WITHIN $poly]` | Docs whose shape is entirely inside query shape |
| `@area:[CONTAINS $point]` | Docs whose shape contains the query point/shape |
| `@area:[INTERSECTS $poly]` | Docs whose shape overlaps the query shape |
| `@area:[DISJOINT $poly]` | Docs whose shape does not overlap query shape |

**Coordinate systems**:
- `SPHERICAL` (default) — geographic lon/lat coordinates
- `FLAT` — Cartesian X/Y coordinates

**Implementation notes**:
- Spatial index: R-tree for bounding-box pre-filter + exact geometry check
- Point-in-polygon: ray casting algorithm
- Polygon intersection: Sutherland-Hodgman or separating axis test
- Clean-room computational geometry, no external libraries required

**Unit tests**:
- POINT within POLYGON
- POLYGON contains POINT
- POLYGON intersects POLYGON (partial overlap)
- DISJOINT: non-overlapping shapes
- Polygon with hole
- FLAT vs SPHERICAL coordinate systems
- Edge cases: degenerate polygon, self-intersecting polygon → error
- Combined with boolean queries

---

## Phase 26 — Phonetic Matching + FT.CONFIG + FT.PROFILE + DIALECT

**Goal**: Remaining quality and operational features.

### Phonetic Matching

**Field attribute**:
```
FT.CREATE idx SCHEMA name TEXT PHONETIC "dm:en"
```

- Double Metaphone algorithm for English phonetic matching
- At index time: store phonetic codes alongside original terms
- At query time: match by phonetic code in addition to exact/stemmed match
- Supported: `dm:en` (Double Metaphone English), extensible

### FT.CONFIG

| Command | Description |
|---------|-------------|
| `FT.CONFIG GET param` | Read module configuration |
| `FT.CONFIG SET param value` | Set module configuration |

Parameters: `DEFAULT_DIALECT`, `TIMEOUT`, `MAXEXPANSIONS`, `MINSTEMLEN`,
`GC_POLICY`, etc.

### FT.PROFILE

| Command | Description |
|---------|-------------|
| `FT.PROFILE idx SEARCH\|AGGREGATE query ...` | Profile a query |

- Returns query results plus timing breakdown per stage
- Shows: parsing time, index scan time, scoring time, sort time,
  total time, docs scanned vs returned

### DIALECT

- `DIALECT n` option in FT.SEARCH / FT.AGGREGATE
- Controls query parsing behaviour for backward compatibility
- Dialect 1: legacy (implicit NOT applies to all following terms)
- Dialect 2: standard (NOT applies only to next term, PARAMS support)
- Dialect 3+: future extensions

**Unit tests**:
- Phonetic: "John" matches "Jon", "Sean" matches "Shawn"
- CONFIG GET/SET round-trip
- PROFILE returns timing info
- DIALECT 1 vs 2 negation behaviour differs

---

## Milestone Summary

| Phase | Core Capability | Dependencies | Estimated Size |
|-------|----------------|-------------|----------------|
| P0–P12 | *(complete)* | — | — |
| P13 | Search options (NOCONTENT, WITHSCORES, ...) | — | S |
| P14 | Prefix + fuzzy + optional terms | P9 (TEXT index) | M |
| P15 | Exact phrase + SLOP + INORDER | P9, positional index | L |
| P16 | Stemming + multi-language | P9 | M |
| P17 | GEO field type + GEOFILTER | P1 (schema) | M |
| P18 | Aggregation enhancement (APPLY/FILTER/LOAD) | P12 | L |
| P19 | FT.ALTER + HIGHLIGHT + SUMMARIZE | P15 (positions) | M |
| P20 | ON JSON support | P8 (ON HASH) | M |
| P21 | Index management (aliases, TAGVALS, EXPLAIN) | P1 | S |
| P22 | SORTABLE + index creation options | P1 | M |
| P23 | Cursor pagination | P12 | S |
| P24 | Autocomplete + spellcheck + synonyms + dicts | — | L |
| P25 | GEOSHAPE field type | P17 (GEO) | L |
| P26 | Phonetic + CONFIG + PROFILE + DIALECT | P9 | M |

Size: S = 1–2 days, M = 3–5 days, L = 1–2 weeks.

### Ordering rationale

1. **P13–P16** (search quality): These are the most visible gaps for full-text
   search users. Prefix, fuzzy, phrase, and stemming are table-stakes
   features. P13 is quick wins that unblock many use cases.

2. **P17** (GEO): A fundamental field type used in many real-world apps
   (store locators, ride-sharing, delivery). Independent of text features.

3. **P18** (aggregation): APPLY and FILTER unlock the "analytics engine"
   use case. Building on the existing P12 pipeline is straightforward.

4. **P19** (ALTER + HIGHLIGHT): Schema evolution is critical for production
   use. HIGHLIGHT/SUMMARIZE are paired here because they depend on
   positional data from P15.

5. **P20** (JSON): Modern Redis usage increasingly uses JSON. Depends on
   RedisJSON module being available.

6. **P21–P23** (management + options + cursors): Important for operational
   maturity but not core search functionality.

7. **P24** (autocomplete + spellcheck): Specialized features, useful but
   not essential for core search/query.

8. **P25** (GEOSHAPE): Advanced geospatial — fewer users need polygons
   than radius queries.

9. **P26** (phonetic + config + profile): Nice-to-have features for
   advanced tuning.
