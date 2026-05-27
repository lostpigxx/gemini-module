# gemini-bloom 目录组织问题

审查基准：`lostpigxx/gemini-module` 的 `main` 分支，重点文件为 `modules/gemini-bloom/**`、根 `CMakeLists.txt`、`README.md`、`include/mock_redismodule_io.h`。对照对象为 `RedisBloom/RedisBloom` 当前公开实现与 redis.io 命令文档。

严重等级：`P0` = 数据损坏/未定义行为/协议级不兼容；`P1` = 生产可用性或兼容性重大问题；`P2` = 功能、性能、维护性明显缺陷；`P3` = 较小但应修复的问题。


## 1. P1：README 与根 CMake 的项目范围不一致

**位置**：`README.md`、根 `CMakeLists.txt`。

README 写的是“Currently includes: gemini-bloom”，项目结构也只列出 `modules/gemini-bloom`。但根 `CMakeLists.txt` 同时 `add_subdirectory(modules/gemini-json)` 和 `add_subdirectory(modules/gemini-search)`。

**后果**：

- 用户按 README 构建，以为只构建 Bloom，实际构建三个模块。
- json/search 任一模块构建失败会阻断 bloom 用户。
- 项目范围和文档可信度下降。

**修复方向**：

- README 更新为真实模块列表；或
- 根 CMake 增加选项：`-DBUILD_GEMINI_BLOOM=ON`、`-DBUILD_GEMINI_JSON=OFF` 等。

## 2. P1：测试 mock 放在生产 `include/` 目录

**位置**：`include/mock_redismodule_io.h`。

`mock_redismodule_io.h` 是测试专用 mock，但被放在顶层公共 include 目录，与 `redismodule.h` 同级。

**后果**：

- 生产 include 面污染。
- 外部使用者可能误 include 测试 mock。
- 模块边界不清晰。

**修复方向**：移动到 `modules/gemini-bloom/tests/support/mock_redismodule_io.h` 或 `tests/include/`，CMake 只给测试 target 添加该 include path。

## 3. P1：`src/` 平铺导致 core、Redis adapter、serialization 混杂

**位置**：`modules/gemini-bloom/src/`。

当前文件：

- `bloom_filter.*`：单层核心结构。
- `sb_chain.*`：可扩展 filter 核心结构。
- `bloom_commands.*`：Redis 命令层。
- `bloom_rdb.*`：RDB、SCANDUMP wire、AOF、module callbacks。
- `bloom_config.*`：配置解析。
- `murmur2.*`：hash。
- `rm_alloc.h`：Redis allocator 封装。

这些属于不同层，却在一个平铺目录内。

**后果**：

- wire format 和 Redis module callback 混在一起，难以单独测试。
- core 逻辑被 Redis API 依赖间接污染。
- 新开发者很难判断哪些文件可复用、哪些只属于 Redis adapter。

**建议结构**：

```text
modules/gemini-bloom/
  src/
    core/
      bloom_layer.h/.cc
      scaling_bloom.h/.cc
      murmur2.h/.cc
    format/
      redisbloom_rdb.h/.cc
      redisbloom_scandump.h/.cc
      validation.h/.cc
    redis/
      commands.h/.cc
      module.cc
      config.h/.cc
      allocator.h
  tests/
    unit/
    integration/
    fixtures/
    support/
```

## 4. P2：缺少 RedisBloom 兼容 fixture 目录

当前 RDB/SCANDUMP 测试主要是自编码再自解码。应增加：

```text
modules/gemini-bloom/tests/fixtures/redis-bloom/
  rdb-encver4-small.bin
  rdb-encver4-multilayer.bin
  scandump-small/
  scandump-large-chunked/
  aof-loadchunk-large.aof
  expected-info-resp2.txt
  expected-info-resp3.txt
```

**原因**：兼容性不能靠同一套错误 encoder/decoder round-trip 证明。

## 5. P2：没有 benchmark/perf 目录

性能问题包括层扫描顺序、SCANDUMP chunk、RDB load copy、small filter memory。当前没有基准测试目录。

**建议**：新增：

```text
modules/gemini-bloom/bench/
  insert_bench.cc
  exists_bench.cc
  scan_dump_bench.tcl
  rdb_load_bench.tcl
```

至少覆盖：单层、多层、expansion=1、批量 MADD/MEXISTS、大 bit array。

## 6. P2：集成测试与构建系统没有统一入口

`CMakeLists.txt` 只定义 GTest 的 `bloom_test` target。TCL 集成测试需要手动执行，并且 README 也只是给命令示例。

**后果**：

- CI 很容易只跑单元测试，不跑 Redis 集成测试。
- 本地开发者不知道完整测试矩阵。

**修复方向**：

- 在 CMake 增加 `bloom_integration_test` target。
- 使用 `enable_testing()`/`add_test()` 注册所有 GTest 和 TCL 测试。
- README 明确“完整测试”命令。

## 7. P2：根构建输出目录扁平，模块产物没有隔离

**位置**：各模块 CMake 均设置 `LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}`。

所有模块 `.so` 都输出到 build 根目录。

**问题**：

- 多模块产物混杂。
- 如果后续出现同名 helper 或 test artifact，冲突风险上升。

**建议**：输出到 `${CMAKE_BINARY_DIR}/modules/gemini-bloom/redis_bloom.so`，同时可在 build 根目录创建 convenience symlink/copy。

## 8. P2：共享 Redis module 工具没有统一归属

`rm_alloc.h` 在 bloom 模块内，但 json/search 模块可能也需要 Redis allocator、API include workaround、mock IO 等通用组件。

**建议**：建立：

```text
include/gemini/redis_module_api.h
include/gemini/redis_alloc.h
include/gemini/test/mock_redismodule_io.h   # 或 tests/support
```

模块内只保留 bloom 相关逻辑。

## 9. P2：`doc/` 目录存在但没有 bloom 文档

README 项目结构列出 `doc/`，但本次未发现 `doc/BLOOM.md`。对于一个兼容 RedisBloom 的模块，缺少文档会直接导致行为误解。

**建议**：新增：

- `doc/gemini-bloom/commands.md`
- `doc/gemini-bloom/compatibility.md`
- `doc/gemini-bloom/wire-format.md`
- `doc/gemini-bloom/testing.md`

## 10. P3：测试文件命名和覆盖说明不一致

`sb_chain_test.cc` 注释说 Serialize/DeserializeHeader 由 TCL 集成测试覆盖，但 `bloom_rdb_test.cc` 实际已有 wire header round-trip 测试。

**问题**：测试职责说明过期，会误导维护者。

**建议**：删除过期注释，或在测试 README 中集中维护覆盖矩阵。

## 11. P3：头文件依赖顺序脆弱

`include/mock_redismodule_io.h` 使用 `RedisModuleIO` 类型，但自身不 include `redismodule.h`；依赖调用方先 include。当前 `bloom_rdb_test.cc` 刚好先 include 了。

**建议**：测试 support 头文件应自包含，或者在头部显式要求并静态断言。
