# gemini-module

A collection of Redis modules. Currently includes:

- **gemini-bloom** — Bloom Filter data structures (`BF.*` commands)

## Project Structure

```
gemini-module/
├── CMakeLists.txt              # Umbrella project
├── include/                    # Shared headers (redismodule.h)
├── modules/
│   └── gemini-bloom/           # Bloom filter module
│       ├── CMakeLists.txt
│       ├── src/
│       └── tests/
└── doc/
```

## Build

Requires CMake 3.14+ and a C++20 compiler.

```bash
cmake -B build
cmake --build build -j$(nproc)
```

The output is `build/redis_bloom.so`.

### AddressSanitizer

```bash
cmake -B build -DENABLE_ASAN=ON
cmake --build build -j$(nproc)
```

## Load Module

```bash
redis-server --loadmodule ./build/redis_bloom.so
```

## Run Tests

### Unit Tests (Google Test)

```bash
cmake -B build -DGTest_DIR=/opt/homebrew/lib/cmake/GTest
cmake --build build --target bloom_test
```

If GTest is installed in a standard system path, the `-DGTest_DIR` flag can be omitted.

### Integration Tests (TCL)

Requires a `redis-server` binary in `$PATH`:

```bash
tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl
```

Or specify the module path explicitly:

```bash
tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl ./build/redis_bloom.so
```
