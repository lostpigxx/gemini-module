# 05 — 目录组织问题

本文件关注工程结构、模块边界、测试组织和文档组织，不讨论具体算法正确性。

## ORG-01：`src/` 同时放核心算法、Redis 命令、持久化、配置、allocator，边界不清

**级别：P2**

### 现状

```text
src/
  bloom_filter.{h,cc}
  sb_chain.{h,cc}
  bloom_commands.{h,cc}
  bloom_rdb.{h,cc}
  bloom_config.{h,cc}
  murmur2.{h,cc}
  rm_alloc.h
  redis_bloom_module.cc
```

### 问题

`bloom_rdb.cc` 既做 RDB，又做 wire header，又做 AOF rewrite；`sb_chain.h` 暴露 wire structs；核心层和 Redis API 层互相渗透。

### 建议组织

```text
modules/gemini-bloom/
  include/gemini_bloom/
    bloom_filter.h
    scaling_bloom.h
    hash.h
  src/core/
    bloom_filter.cc
    scaling_bloom.cc
    murmur2.cc
  src/redis/
    module.cc
    commands.cc
    config.cc
    allocator.cc
  src/serialization/
    rdb.cc
    scandump.cc
    aof.cc
    redisbloom_wire.cc
  tests/
    unit/
    integration/
    compat/
    fuzz/
    bench/
```

---

## ORG-02：wire format struct 放在 `sb_chain.h`，导致核心数据结构依赖序列化协议

**级别：P2**

`WireLayerMeta`、`WireFilterHeader` 定义在 `src/sb_chain.h`。scalable bloom filter 核心类型不应知道 SCANDUMP/LOADCHUNK wire layout。否则任何格式变更都污染核心 header，增加 rebuild 和耦合。

### 建议

把 wire struct 移到：

```text
src/serialization/redisbloom_wire.h
```

核心层只暴露受控 snapshot/build API。

---

## ORG-03：`bloom_rdb.cc` 职责过多

**级别：P2**

### 当前职责

- `RdbWriter` / `RdbReader`
- `BloomLayer::WriteTo/ReadFrom`
- `ScalingBloomFilter::WriteTo/ReadFrom`
- `SerializeHeader` / `DeserializeHeader`
- `RdbLoadBloom` / `RdbSaveBloom`
- `AofRewriteBloom`
- `BloomMemUsage`

### 问题

格式、I/O、Redis callback、核心对象构造混在一个文件。生命周期问题和校验差异很容易从这里扩散。

### 建议拆分

```text
rdb_format.cc          // RDB native format
redisbloom_wire.cc     // SCANDUMP/LOADCHUNK official wire
aof_rewrite.cc         // AOF command emission
redis_type.cc          // RedisModuleType callbacks
```

---

## ORG-04：TCL 集成测试文件过大，覆盖多个主题但缺少结构化分组

**级别：P2/P3**

`tests/tcl/bloom_test.tcl` 接近千行，包含 mini Redis client、server lifecycle、test framework、所有命令测试、persistence tests、cleanup。

### 问题

单文件难以维护，失败定位成本高。新增 compat/fuzz/RESP3 测试会继续膨胀。

### 建议

```text
tests/tcl/
  support/
    redis_client.tcl
    test_harness.tcl
    server.tcl
  commands/
    reserve.test.tcl
    add_exists.test.tcl
    insert.test.tcl
    info.test.tcl
  persistence/
    rdb.test.tcl
    aof.test.tcl
  compat/
    redisbloom_scandump.test.tcl
    resp3.test.tcl
```

---

## ORG-05：缺少 `compat/` 金样本目录

**级别：P1/P2**

既然源码注释声称“intended to match RedisBloom”，就必须有官方金样本：

```text
tests/compat/redisbloom/
  scandump/
  rdb/
  aof/
  info_resp2/
  info_resp3/
  hash_vectors/
```

当前没有这样的目录，兼容性只能靠人工推断。

### 建议

把 RedisBloom official 输出固化为二进制 fixtures，并写双向测试：

```text
RedisBloom dump -> gemini load -> no false negatives
gemini dump -> RedisBloom load -> no false negatives
metadata exact match
```

---

## ORG-06：root `CMakeLists.txt` 总是构建所有模块，不方便只审计/测试 bloom

**级别：P3**

root CMake 直接：

```cmake
add_subdirectory(modules/gemini-bloom)
add_subdirectory(modules/gemini-json)
add_subdirectory(modules/gemini-search)
```

### 影响

- bloom 开发者必须同时处理 json/search 的构建依赖和错误。
- CI 难以做模块级 matrix。
- 不能通过标准入口最小化构建 bloom。

### 建议

```cmake
option(BUILD_GEMINI_BLOOM "Build gemini-bloom" ON)
option(BUILD_GEMINI_JSON "Build gemini-json" ON)
option(BUILD_GEMINI_SEARCH "Build gemini-search" ON)
```

---

## ORG-07：GTest 目标只有 GTest_FOUND 时静默创建，缺少显式反馈

**级别：P3**

`modules/gemini-bloom/CMakeLists.txt`：

```cmake
find_package(GTest QUIET)
if(GTest_FOUND)
  add_executable(...)
  add_custom_target(bloom_test ...)
endif()
```

如果 GTest 未找到，`bloom_test` target 不存在，但用户只会在 build target 时看到 target 不存在，不知道原因。

### 建议

增加 option：

```cmake
option(BUILD_TESTING "Build tests" ON)
if(BUILD_TESTING AND NOT GTest_FOUND)
  message(FATAL_ERROR "GTest required for BUILD_TESTING=ON")
endif()
```

或至少 `message(WARNING ...)`。

---

## ORG-08：mock RedisModule IO 是共享 include，但 bloom 测试强依赖其细节

**级别：P3**

`bloom_rdb_test.cc` 直接 include `mock_redismodule_io.h`。如果该 mock 被 json/search 共享，Bloom RDB 的错误注入需求会和其他模块耦合。

### 建议

把通用 mock 放 `tests/support/redis_module_mock/`，每个模块的 fixture 放本模块测试目录，避免跨模块隐式依赖。

---

## ORG-09：缺少模块级 README/API 文档

**级别：P2/P3**

根 README 只列命令名。gemini-bloom 没有自己的文档说明：

- 默认 capacity/error/expansion
- error rate cap/范围
- non-scaling full 行为
- SCANDUMP/LOADCHUNK 是否兼容 RedisBloom
- RDB/AOF 兼容状态
- RESP2/RESP3 支持状态
- 与 Redis 8 内置 Bloom 的关系

### 建议

新增：

```text
modules/gemini-bloom/README.md
modules/gemini-bloom/COMPATIBILITY.md
modules/gemini-bloom/FORMAT.md
modules/gemini-bloom/TESTING.md
```
