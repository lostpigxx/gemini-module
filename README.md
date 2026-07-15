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

## Docker CI

Docker CI 提供三组环境验证 gemini-bloom 在不同 Redis 版本下的兼容性：

| 环境 | Redis | RedisBloom | Dockerfile |
|---|---|---|---|
| redis5 | 5.0.14 | 2.2.18 | `Dockerfile.redis5` |
| redis6 | 6.2.20 | 2.4.20 | `Dockerfile.redis6` |
| redis7 | 7.2.11 | 2.6.25 | `Dockerfile.redis7` |

每组环境运行以下测试：

| 测试项 | 说明 |
|---|---|
| GTest 单元测试 | bloom_filter_test、bloom_rdb_test、sb_chain_test |
| Tcl 集成测试 | 启动 redis-server 加载模块，验证命令行为 |
| Compat 对比测试 | 同时启动 gemini-bloom 和 RedisBloom，对比所有 BF.* 命令返回值 |
| Soak 长稳测试 | 持续运行 BF.* 命令验证零 false negative、内存无泄漏（默认 300s） |
| RDB 迁移测试 | 4 方向 RDB 迁入迁出（RB→GB、GB→RB、GB→GB、RB→RB），验证 BF.EXISTS 100% 一致（含 false positive），覆盖 16 种 filter 配置、约 30 万 items |

Redis 6/7 环境下 Compat 和 Soak 会跑两轮（RESP3 + RESP2），Redis 5 只跑 RESP2。

### 运行单组环境

```bash
# 构建并运行（以 redis7 为例）
docker build -f Dockerfile.redis7 -t gemini-module:redis7 .
docker run --rm gemini-module:redis7

# 缩短 soak 测试时间（默认 300s）
docker run --rm -e SOAK_DURATION_SEC=30 gemini-module:redis7
```

### 运行全部三组环境

```bash
bash ci/run_all_envs.sh
```

### 离线构建

所有外部依赖源码以 tarball 形式存放在 `deps/` 目录中，Dockerfile 从本地文件解压编译，支持纯离线构建（无外网内网环境）。
