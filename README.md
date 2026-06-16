# gemini-module

A collection of Redis modules, built from scratch with C++20.

## Modules

### gemini-bloom — Bloom Filter

Scalable Bloom Filter data structures with auto-expansion, supporting the full `BF.*` command set:
`BF.RESERVE`, `BF.ADD`, `BF.MADD`, `BF.INSERT`, `BF.EXISTS`, `BF.MEXISTS`, `BF.INFO`, `BF.CARD`, `BF.SCANDUMP`, `BF.LOADCHUNK`.

### gemini-json — JSON Data Type

JSON data type with JSONPath-based operations, compatible with the RedisJSON command set:
`JSON.SET`, `JSON.GET`, `JSON.DEL`, `JSON.TYPE`, `JSON.CLEAR`, `JSON.TOGGLE`,
`JSON.ARRAPPEND`, `JSON.ARRINDEX`, `JSON.ARRINSERT`, `JSON.ARRLEN`, `JSON.ARRPOP`, `JSON.ARRTRIM`,
`JSON.STRAPPEND`, `JSON.STRLEN`, `JSON.NUMINCRBY`, `JSON.NUMMULTBY`,
`JSON.OBJKEYS`, `JSON.OBJLEN`, `JSON.MGET`, `JSON.MSET`, `JSON.MERGE`,
`JSON.DEBUG`, `JSON.RESP`.

### gemini-search — Full-Text Search

Full-text search engine with support for text, tag, numeric, vector (HNSW) and geo field types:
`FT.CREATE`, `FT.DROPINDEX`, `FT.INFO`, `FT._LIST`, `FT.ADD`, `FT.DEL`, `FT.SEARCH`, `FT.AGGREGATE`.

## Project Structure

```
gemini-module/
├── CMakeLists.txt              # Umbrella project
├── include/                    # Shared headers (redismodule.h, test mocks)
├── modules/
│   ├── gemini-bloom/           # Bloom filter module
│   ├── gemini-json/            # JSON module
│   └── gemini-search/          # Full-text search module
└── doc/
```

## Build

Requires CMake 3.14+ and a C++20 compiler.

```bash
cmake -B build
cmake --build build -j$(nproc)
```

Output files:

| Module | Shared Library |
|--------|---------------|
| gemini-bloom | `build/redis_bloom.so` |
| gemini-json | `build/redis_json.so` |
| gemini-search | `build/redis_search.so` |

### AddressSanitizer

```bash
cmake -B build -DENABLE_ASAN=ON
cmake --build build -j$(nproc)
```

## Load Modules

```bash
# Load a single module
redis-server --loadmodule ./build/redis_bloom.so

# Load multiple modules
redis-server \
  --loadmodule ./build/redis_bloom.so \
  --loadmodule ./build/redis_json.so \
  --loadmodule ./build/redis_search.so
```

## Run Tests

### Unit Tests (Google Test)

```bash
# Build and run all tests for a specific module
cmake --build build --target bloom_test
cmake --build build --target json_test
cmake --build build --target search_test
```

If GTest is not in a standard path:

```bash
cmake -B build -DGTest_DIR=/opt/homebrew/lib/cmake/GTest
```

### Integration Tests (TCL)

Requires a `redis-server` binary in `$PATH`:

```bash
# gemini-bloom
tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl ./build/redis_bloom.so

# gemini-json
tclsh modules/gemini-json/tests/tcl/json_test.tcl ./build/redis_json.so

# gemini-search
tclsh modules/gemini-search/tests/tcl/search_test.tcl ./build/redis_search.so
```
