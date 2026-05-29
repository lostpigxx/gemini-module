# Codex Review: gemini-bloom 目录组织问题

## 1. 仓库已有 `doc/`，本次按要求新增 `docs/`，文档目录出现双轨

现状：

- 既有文档在 `doc/`。
- 本次审计按用户要求输出到 `docs/`。

风险：

- 后续维护者不知道新文档应该放 `doc/` 还是 `docs/`。
- README 的项目结构仍写 `doc/`。

建议：

- 统一为一个目录。
- 如果保留 `docs/`，迁移现有 `doc/` 内容或在 README 中说明目录分工。

## 2. Bloom 模块的源码层次过平

当前所有实现都在 `modules/gemini-bloom/src/`：

- command layer：`bloom_commands.cc`
- SBF layer：`sb_chain.cc`
- single-layer Bloom：`bloom_filter.cc`
- RDB、AOF、SCANDUMP wire：`bloom_rdb.cc`
- module config：`bloom_config.cc`
- hash：`murmur2.cc`

问题：

- Redis command 语义、算法、wire format、Redis Module API adapter 混在同一层目录。
- `bloom_rdb.cc` 同时承载 RDB callback、AOF rewrite、SCANDUMP header、`BloomLayer` serialization 方法，职责过宽。

建议拆分：

- `src/core/`：BloomLayer、ScalingBloomFilter、hash。
- `src/redis/`：command handlers、module load、config。
- `src/persistence/`：RDB/AOF callbacks。
- `src/wire/`：SCANDUMP/LOADCHUNK header/chunk codec。

## 3. SCANDUMP wire format 没有独立 schema 和 golden corpus

位置：

- `modules/gemini-bloom/src/sb_chain.h:74-97`
- `modules/gemini-bloom/src/bloom_rdb.cc:142-199`

wire format 现在只是 C++ packed struct。没有：

- 单独格式文档。
- 字段版本说明。
- RedisBloom golden bytes。
- endian/alignment 说明。

建议：

- 增加 `docs/gemini-bloom-wire-format.md`。
- 增加 `tests/fixtures/redisbloom/`，保存官方 RedisBloom 生成的 header/chunk/RDB/AOF。

## 4. 各模块重复 allocator wrapper

位置：

- `modules/gemini-bloom/src/rm_alloc.h`
- `modules/gemini-json/src/rm_alloc.h`

问题：

- 测试宏、RedisModule allocator wrapper 重复。
- 后续修复 allocator 行为需要多处同步。

建议：

- 抽到 `include/gemini/rm_alloc.h` 或 `modules/common/`。
- 测试模式用统一宏，例如 `GEMINI_MODULE_TESTING`。

## 5. CMake 测试目标在找不到 GTest 时静默消失

位置：`modules/gemini-bloom/CMakeLists.txt:25-81`

`find_package(GTest QUIET)` 找不到时，不创建任何单元测试目标。实际容器里 `bloom_test` 目标不存在。

问题：

- CI 或开发者可能以为测试通过，其实单元测试根本没编译。
- `/init` 后 README 中的 `cmake --build build --target bloom_test` 在当前容器不可用。

建议：

- 增加显式配置项：`-DENABLE_UNIT_TESTS=ON` 时找不到 GTest 直接 fail。
- 或 vendor/fetch GTest，但要符合离线构建策略。

## 6. Tcl 集成测试没有纳入 CMake/CTest

位置：`modules/gemini-bloom/tests/tcl/bloom_test.tcl`

当前 Tcl 测试需要手动运行，不属于 CMake target。

建议：

- 增加 `add_custom_target(bloom_tcl_test ...)`。
- 或用 CTest 注册，统一 `ctest --test-dir build`。
- 按 AGENTS 要求，文档中所有运行命令必须明确通过 Docker container 执行。

## 7. README 对模块清单已经过期

位置：`README.md`

README 开头写 “Currently includes gemini-bloom”，但 top-level CMake 已包含：

- `gemini-bloom`
- `gemini-json`
- `gemini-search`

建议：

- 更新 README 的项目结构和输出产物。
- 明确每个模块的支持命令范围。

## 8. 缺少兼容性测试目录

建议新增：

- `modules/gemini-bloom/tests/compat/`
- `modules/gemini-bloom/tests/fixtures/redisbloom-2.8/`
- `modules/gemini-bloom/tests/fixtures/redis-8/`

覆盖：

- official -> gemini RDB。
- gemini -> official RDB。
- official -> gemini SCANDUMP/LOADCHUNK。
- gemini -> official SCANDUMP/LOADCHUNK。
- AOF rewrite replay。
- RESP2/RESP3 reply trace。
