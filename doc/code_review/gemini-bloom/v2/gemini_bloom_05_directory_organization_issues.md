> 分析基准：GitHub 仓库 `lostpigxx/gemini-module` 的 `main` 分支当前可读取源码；重点目录为 `modules/gemini-bloom`。  
> 方法：静态源码审查 + 与当前 RedisBloom 官方源码/命令文档做行为对照。未在本地编译运行；容器内无法直接 `git clone`，因此结论基于 GitHub API 拉取到的文件内容。  
> 严重性：P0=可能崩溃/数据损坏/安全风险；P1=兼容性或核心语义错误；P2=明显设计/性能/可维护性缺陷；P3=低风险但应修正。

# 05. 目录组织问题

## 总览

本文件关注工程结构、模块边界、测试组织、文档位置、构建入口和长期维护成本。

| ID | 严重性 | 位置 | 目录/工程组织问题 |
|---|---:|---|---|
| ORG-01 | P1 | 根 README vs `CMakeLists.txt` | README 声称当前只有 gemini-bloom，但顶层 CMake 已加入 json/search |
| ORG-02 | P2 | `include/mock_redismodule_io.h` | 测试 mock 放在公共 include 目录，污染生产 include 空间 |
| ORG-03 | P1 | `modules/gemini-bloom/CMakeLists.txt` | 单元测试 `EXCLUDE_FROM_ALL` 且仅在 GTest_FOUND 时静默启用 |
| ORG-04 | P1 | `tests/tcl/bloom_test.tcl` | 集成测试未纳入 CMake/CTest，容易被 CI 漏掉 |
| ORG-05 | P2 | 顶层 `CMakeLists.txt` | 无 build option 控制单个 module，默认强制 add 所有模块 |
| ORG-06 | P2 | build output | bloom 输出名 `redis_bloom.so` 放 build 根目录，命名与 Gemini 模块身份不一致 |
| ORG-07 | P2 | `src/` | 数据结构、命令、RDB、SCANDUMP wire、配置混在同级目录 |
| ORG-08 | P2 | 多个 header | `redismodule.h` 宏 workaround 在多个 header 重复 |
| ORG-09 | P2 | docs | 缺少 `modules/gemini-bloom/README.md` 或兼容性文档 |
| ORG-10 | P2 | tests | 没有 golden corpus/fuzz/benchmark/coverage 目录 |
| ORG-11 | P3 | shared code | `rm_alloc.h` 这类通用 wrapper 放在 bloom 私有 src，不利于多模块共享 |
| ORG-12 | P3 | TCL test | 测试脚本兼具 client/server/test runner，职责过重 |

## 详细问题

### ORG-01：README 与工程实际不一致

根 README 的项目结构说明仍写成：

```text
modules/
└── gemini-bloom/
```

并声明当前包含 `gemini-bloom`。但顶层 `CMakeLists.txt` 已经：

```cmake
add_subdirectory(modules/gemini-bloom)
add_subdirectory(modules/gemini-json)
add_subdirectory(modules/gemini-search)
```

这会误导：

- 用户以为只构建 bloom。
- 贡献者不清楚 json/search 是否正式模块。
- CI/发布脚本难以判断哪些模块是稳定模块。

建议：

- README 更新为当前模块矩阵。
- 每个模块标注 status：experimental / alpha / stable。
- 顶层构建说明写明默认构建哪些模块。

### ORG-02：测试 mock 不应放公共 include

`include/mock_redismodule_io.h` 是纯测试辅助：

```cpp
MockRdbStream
InstallMockRedisModuleIO()
Mock_SaveUnsigned()
...
```

但它位于根 `include/`，与 `redismodule.h` 同级。复核结论：这是合理的工程组织建议，但原报告列为 P1 过重；当前仓库的测试约定已经明确 RDB 相关测试复用这个 mock，短期不是会破坏构建/运行的核心问题。长期问题是：

- 生产模块也能误 include。
- 公共 include 语义变混乱。
- 多模块会看到 bloom 专用测试设施。

建议移动到：

```text
tests/include/mock_redismodule_io.h
modules/gemini-bloom/tests/support/mock_redismodule_io.h
```

并只给测试 target 加 include path。

### ORG-03：GTest 不存在时测试静默消失

`modules/gemini-bloom/CMakeLists.txt`：

```cmake
find_package(GTest QUIET)
if(GTest_FOUND)
  add_executable(... EXCLUDE_FROM_ALL)
  ...
endif()
```

问题：

- 没有 GTest 时不会失败，只是不生成测试。
- `EXCLUDE_FROM_ALL` 意味着常规 build 不会构建测试。
- 没有 `add_test()`，CTest 无法发现。

建议：

```cmake
option(BUILD_TESTING "Build tests" ON)
include(CTest)
if(BUILD_TESTING)
  find_package(GTest REQUIRED)
  add_test(NAME bloom_filter_test COMMAND bloom_filter_test)
endif()
```

### ORG-04：TCL 集成测试未纳入标准测试入口

`tests/tcl/bloom_test.tcl` 是重要的 Redis 实例级测试，覆盖命令、RDB、AOF。但 CMake 只定义了 `bloom_test` custom target 跑三个 GTest binary，没有包含 TCL 测试。

结果：

- 开发者跑 `cmake --build build --target bloom_test` 不会跑 Redis 集成测试。
- CI 如果只跑 CTest，也不会跑 TCL。
- 命令层 bug 容易漏掉。

建议：

```cmake
add_test(NAME bloom_tcl_integration
  COMMAND tclsh ${CMAKE_CURRENT_SOURCE_DIR}/tests/tcl/bloom_test.tcl $<TARGET_FILE:redis_bloom>)
```

并给它加 `LABELS integration redis`.

### ORG-05：顶层缺少模块选择开关

当前顶层无条件：

```cmake
add_subdirectory(modules/gemini-bloom)
add_subdirectory(modules/gemini-json)
add_subdirectory(modules/gemini-search)
```

这会导致：

- 只想分析/构建 bloom 时，也受 json/search 构建质量影响。
- 某个实验模块坏了会拖垮整个仓库。
- 发布单独模块不方便。

建议：

```cmake
option(BUILD_GEMINI_BLOOM "Build Gemini Bloom module" ON)
option(BUILD_GEMINI_JSON "Build Gemini JSON module" ON)
option(BUILD_GEMINI_SEARCH "Build Gemini Search module" ON)
```

### ORG-06：输出名与模块身份混乱

CMake 产物名是：

```cmake
add_library(redis_bloom SHARED ...)
set_target_properties(redis_bloom PROPERTIES PREFIX "" SUFFIX ".so")
```

最终是 `redis_bloom.so`。但模块初始化名是 `GeminiBloom`。

问题：

- 从文件名看像 RedisBloom 官方模块。
- 与产品名 GeminiBloom 不一致。
- 如果用户同时有 RedisBloom 官方 `.so`，容易误加载。

建议：

- drop-in replacement 模式：文档明确“产物命名为 redis_bloom.so 是为了兼容”。
- 独立模块模式：产物改为 `gemini_bloom.so`，命令也命名空间化。

### ORG-07：`src/` 职责混在一起

当前 bloom 的 `src/` 同时包含：

- data structure：`bloom_filter.*`, `sb_chain.*`
- command layer：`bloom_commands.*`
- RDB callback：`bloom_rdb.*`
- SCANDUMP wire format：也在 `bloom_rdb.cc`
- config：`bloom_config.*`
- hashing：`murmur2.*`
- allocator wrapper：`rm_alloc.h`
- module entry：`redis_bloom_module.cc`

建议按职责拆：

```text
src/core/
  bloom_filter.*
  sb_chain.*
  murmur2.*
src/redis/
  redis_bloom_module.cc
  bloom_commands.*
  bloom_config.*
  rm_alloc.*
src/persistence/
  bloom_rdb.*
  bloom_wire.*
tests/
```

这样 RDB 与 SCANDUMP wire 的边界会更清晰。

### ORG-08：`REDISMODULE_API` 宏 workaround 重复

多个 header 都有类似：

```cpp
#ifndef REDISMODULE_API
#define REDISMODULE_API extern
#define _BF_RDB_API_DEFINED
#endif
extern "C" { #include "redismodule.h" }
#ifdef _BF_RDB_API_DEFINED
#undef REDISMODULE_API
#endif
```

重复位置包括：

- `bloom_rdb.h`
- `bloom_config.h`
- `bloom_commands.h`
- `rm_alloc.h`

建议创建一个统一 header：

```cpp
// redis_module_include.h
#pragma once
#ifndef REDISMODULE_API
#define REDISMODULE_API extern
#define GEMINI_DEFINED_REDISMODULE_API
#endif
extern "C" {
#include "redismodule.h"
}
#ifdef GEMINI_DEFINED_REDISMODULE_API
#undef REDISMODULE_API
#undef GEMINI_DEFINED_REDISMODULE_API
#endif
```

所有地方只 include 这一层。

### ORG-09：缺少模块级文档

`modules/gemini-bloom` 没有独立 README。用户需要知道：

- 命令支持列表。
- 与 RedisBloom 兼容到什么程度。
- 加载参数。
- RDB/AOF 兼容性。
- SCANDUMP/LOADCHUNK 限制。
- RESP3 支持状态。
- 编译与测试方式。

建议新增：

```text
modules/gemini-bloom/README.md
modules/gemini-bloom/docs/compatibility.md
modules/gemini-bloom/docs/wire-format.md
```

### ORG-10：缺少 golden/fuzz/benchmark/coverage 目录

当前只有：

```text
tests/*.cc
tests/tcl/bloom_test.tcl
```

缺少：

```text
tests/golden/redis-bloom/
tests/fuzz/
bench/
coverage/
```

对一个声称 RedisBloom 兼容的模块，golden corpus 尤其关键：

- 官方 RedisBloom 生成的 RDB。
- 官方 RedisBloom 生成的 SCANDUMP chunks。
- 给定参数和 item 的 bit-array golden。
- RESP2/RESP3 回复 golden。

### ORG-11：通用 allocator wrapper 不应是 bloom 私有

`rm_alloc.h` 不是 bloom 特有概念。仓库已经有多个模块，建议把 Redis allocator adapter 放到：

```text
include/gemini/redis_alloc.h
common/redis_alloc.h
```

再由各模块复用，避免 json/search 各自复制。

### ORG-12：TCL 脚本职责过重

`bloom_test.tcl` 同时做：

- Redis client RESP parser
- Redis server lifecycle
- test framework
- test cases
- persistence/AOF orchestration

这让维护困难，也不利于复用。

建议拆分：

```text
tests/tcl/lib/redis_client.tcl
tests/tcl/lib/test_framework.tcl
tests/tcl/lib/server.tcl
tests/tcl/bloom_commands.test.tcl
tests/tcl/bloom_persistence.test.tcl
```
