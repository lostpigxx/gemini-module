# Stage 02 Planner Output

## 1. Stage 目标

Stage 02 的目标是运行现有构建、GTest 和 TCL 集成测试，建立 gemini-bloom v6 审计的 build/test baseline。此阶段只收集现状并分类结果，不修复生产代码，不删除或弱化测试。

本 stage 必须验证 DESIGN.md 中关于测试体系和构建运行方式的声明是否在当前仓库、当前环境下可复现：

- CMake configure/build 是否可完成。
- `redis_bloom.so` 模块产物是否可构建。
- GTest target `bloom_test` 是否存在、可编译、可运行，并覆盖 BloomLayer、ScalingBloomFilter、RDB/wire 相关单元测试。
- TCL 集成测试 `modules/gemini-bloom/tests/tcl/bloom_test.tcl` 是否能用构建出的 `./build/redis_bloom.so` 在真实 `redis-server` 上运行。
- 所有失败都必须分类为真实实现 bug、DESIGN_INTENDED 差异、测试 oracle 错、环境问题、BLOCKED 或 NOT_VERIFIED，不能静默忽略。

Stage 02 在 LOOP_CONTROL_BATCH 中允许 BLOCKED 后继续，但 BLOCKED 会降低最终报告可信度，尤其是现有 GTest/TCL 测试被列为关键可信度 gate。

## 2. DESIGN.md 相关约束

与 Stage 02 直接相关的 DESIGN.md 约束如下：

- gemini-bloom 是 C++20 Redis Module，源码结构包含 `src/` 下模块、命令、RDB/wire、BloomLayer、ScalingBloomFilter、MurmurHash2 和内存抽象实现。
- Core 层通过 `REDIS_BLOOM_TESTING` 宏与 Redis Module API 解耦，GTest 应可独立编译运行，不需要启动 Redis。
- RDB mock 测试应通过 `include/mock_redismodule_io.h` 覆盖序列化 round-trip、恶意 metadata 拒绝、encver 兼容、loading flag 剥离和 narrowing cast 防御。
- TCL 集成测试应在真实 Redis server 上启动隔离实例，加载 `redis_bloom.so`，覆盖命令语义、错误路径、持久化、loading 状态保护、模块配置、COMMAND metadata 和 ACL。
- DESIGN.md 声称测试体系包含：
  - `bloom_filter_test`：28 个 BloomLayer 单元测试和 hash golden vectors。
  - `sb_chain_test`：21 个 ScalingBloomFilter 单元测试。
  - `bloom_rdb_test`：65 个 RDB/wire 序列化测试。
  - `bloom_test.tcl`：150 个 TCL 集成测试。
- DESIGN.md 给出的构建和运行命令是本 stage 主路径：
  - `cmake -B build`
  - `cmake --build build -j$(nproc)`
  - `cmake --build build -j$(nproc) --target bloom_test`
  - `tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl ./build/redis_bloom.so`
- DESIGN_INTENDED 边界不能误判为 Stage 02 bug：RESP3 不支持、BF.DEBUG 不支持、SCANDUMP/LOADCHUNK 不与 RedisBloom 互通、command-AOF rewrite 非 RDB-preamble 模式不跨实现兼容、BF.INFO Size 统计口径差异、严格 parser 行为差异。
- DESIGN.md 的 RedisBloom v2.4.20 兼容矩阵和 `tests/compat/redisbloom-2.4.20/` fixture 声明不属于 Stage 02 的执行范围；现有 open finding `GBV6-00-001` 仍需保留，除非本 stage 发现直接 superseding evidence。

## 3. 必审对象

Stage 02 主 agent 必须审阅和记录以下对象：

- 必读控制文件：
  - `modules/gemini-bloom/DESIGN.md`
  - `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`
  - `.codex/gemini-bloom-audit/v6/policies/*.md`
  - `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`
  - `.codex/gemini-bloom-audit/v6/stages/STAGE_02_BUILD_EXISTING_TESTS.md`
  - Stage 01 `stage_result.md` 和 `reviewer_output.md`
- 构建配置和测试入口：
  - `modules/gemini-bloom/CMakeLists.txt` 或仓库实际 CMake 配置文件。
  - `modules/gemini-bloom/tests/bloom_filter_test.cc`
  - `modules/gemini-bloom/tests/sb_chain_test.cc`
  - `modules/gemini-bloom/tests/bloom_rdb_test.cc`
  - `modules/gemini-bloom/tests/tcl/bloom_test.tcl`
  - 生成产物 `build/redis_bloom.so`。
- 依赖和环境：
  - CMake、C++ compiler、GTest availability。
  - `tclsh`、`redis-server`、`redis-cli` availability and versions。
  - Host Redis version。Stage 01 记录 host Redis 为 6.2.16，因此本 stage 若使用 host Redis，不能写成 Redis 6.2.17 baseline reproduction。
- Stage 00/01 inherited risks：
  - `GBV6-00-001`：DESIGN.md claims RedisBloom v2.4.20 compat fixtures exist, but path is absent。
  - `GBV6-00-002`：`sb_chain.h` SCANDUMP/LOADCHUNK comment contradicts DESIGN.md private protocol boundary。

本 stage 不应审计为 PASS 的对象：

- RedisBloom oracle 对比、DUMP/RESTORE/MIGRATE/fullsync/AOF 双向兼容矩阵属于后续 Stage 05/06。
- Fuzz、fault injection、sanitizer、replica/cluster/performance 属于后续 Stage 07-10。
- 不修改 `modules/gemini-bloom/src/**`、现有测试或 CMake 配置来制造通过。

## 4. 运行/静态检查计划

Planner 子 agent 本身不运行构建或测试。以下是 Stage 02 主 agent execution 阶段应执行的计划。

1. Rehydrate：
   - 重新读取 DESIGN.md、LOOP_CONTROL_BATCH、所有 policies、LOOP_STATE、Stage 02 文件、Stage 01 result 和 reviewer output。
   - 写 `.codex/gemini-bloom-audit/v6/agents/stage02/rehydrate_log.md`，记录读取文件、Stage 02 DESIGN 约束、Stage 01 PASS 状态、本 stage 禁止越界项。

2. 准备 evidence 目录：
   - 创建 `.codex/gemini-bloom-audit/v6/evidence/stage02/build/`
   - 创建 `.codex/gemini-bloom-audit/v6/evidence/stage02/gtest/`
   - 创建 `.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/`
   - 创建本 stage 通用 `commands.txt`、`stdout.log`、`stderr.log`、`exit_codes.txt`、`env_snapshot.txt`、`evidence_index.md`。

3. 环境快照：
   - 记录 `pwd`、`git rev-parse HEAD`、`git status --short`、`cmake --version`、compiler versions、`redis-server --version`、`redis-cli --version`、`tclsh` patchlevel。
   - 对缺失工具保留原始 stderr 和 exit code。缺失 `cmake`/compiler 会阻塞 build；缺失 `redis-server` 或 `tclsh` 会阻塞 TCL，但不应阻止静态分类和已能运行的 GTest/build 证据。

4. DESIGN test claim 静态核对：
   - 检查 DESIGN.md 提到的测试文件是否存在。
   - 检查 CMake 中是否定义 `bloom_test` target 或等价 GTest target。
   - 检查 TCL 文件是否包含 expected-gap/skip/EXPECTED GAP 逻辑，避免将设计内差异误读为失败。
   - 输出 `.codex/gemini-bloom-audit/v6/evidence/stage02/design_test_claim_check.md`。

5. CMake configure/build：
   - 运行并落盘：
     - `cmake -B build`
     - `cmake --build build -j$(nproc)`
   - macOS 若无 `nproc`，先记录该环境差异，并使用等价 job count 命令或显式 `-j` 继续；不能因为命令替换差异而把项目误判为实现失败。
   - stdout/stderr/exit code 分别写入 `evidence/stage02/build/` 下的文件，并在通用 evidence index 中引用。

6. GTest build/run：
   - 运行并落盘：
     - `cmake --build build -j$(nproc) --target bloom_test`
   - 确认该 target 是否仅编译、是否同时执行测试；如果只编译，必须再用 CMake/CTest 或生成的 GTest binary 运行实际测试，并记录命令。若无法确定或无法找到 binary，分类为 BLOCKED/NOT_VERIFIED，不得写 GTest PASS。
   - 记录 GTest test count、failed tests、skipped tests、crash/timeout、exit code。
   - 若 GTest target 不存在，分类为 BLOCKED 或 DESIGN claim mismatch，并记录 CMake target 查询证据。

7. TCL 集成测试：
   - 前置确认 `./build/redis_bloom.so` 存在。
   - 运行并落盘：
     - `tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl ./build/redis_bloom.so`
   - 记录 redis-server 启动失败、端口占用、module load 失败、test failure、expected gap、timeout、cleanup 情况。
   - 若 TCL 测试启动残留 Redis 进程或临时目录，记录 cleanup 证据；不得用删除/改测试方式规避失败。

8. 失败分类：
   - 对每个 build/test failure，读取最小相关日志并分类：
     - 真实实现 bug：代码行为或模块加载违反 DESIGN.md 或基本安全/正确性。
     - DESIGN_INTENDED：失败实际对应 DESIGN 明确的非目标或 expected gap。
     - 测试 oracle 错：测试期望与 DESIGN.md 明确边界冲突。
     - 环境问题：工具缺失、Redis version mismatch、端口/权限/路径问题。
     - NOT_VERIFIED：未覆盖或命令未实际运行。
     - BLOCKED：必要依赖不可用导致无法验证。

9. Stage result handoff：
   - 写 `main_execution.md` 和 `stage_result.md`。
   - 更新 LOOP_STATE。
   - 创建 reviewer output，由 reviewer 检查是否有缺失证据、未支持结论、静默忽略失败或误判 DESIGN_INTENDED。

## 5. 证据清单

Stage 02 evidence 目录应为：

- `.codex/gemini-bloom-audit/v6/evidence/stage02/`

Required evidence：

- `.codex/gemini-bloom-audit/v6/evidence/stage02/build/`
  - CMake configure stdout/stderr/exit code。
  - Full build stdout/stderr/exit code。
  - 构建产物路径和 `redis_bloom.so` 存在性证据。
- `.codex/gemini-bloom-audit/v6/evidence/stage02/gtest/`
  - `bloom_test` target build stdout/stderr/exit code。
  - 实际 GTest execution stdout/stderr/exit code 或无法执行的 blocker 证据。
  - failed/skipped test summary。
- `.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/`
  - TCL command stdout/stderr/exit code。
  - Redis server/module load/version evidence。
  - test summary、expected gap summary、cleanup evidence。
- `.codex/gemini-bloom-audit/v6/evidence/stage02/design_test_claim_check.md`
  - DESIGN.md 测试声明与仓库文件/CMake target/TCL 入口的静态核对。

Evidence policy 必需文件：

- `.codex/gemini-bloom-audit/v6/evidence/stage02/commands.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage02/stdout.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage02/stderr.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage02/exit_codes.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage02/env_snapshot.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage02/evidence_index.md`

如发生阻塞，还应有明确 blocker 证据，例如：

- `cmake: command not found`
- compiler not found or unsupported C++20
- GTest target missing
- `redis-server not found`
- `tclsh not found`
- module load failure
- permission/port/tempdir failure

所有 Stage 02 结论必须引用具体 evidence 路径，不能只写“build passed”或“tests failed”。

## 6. 易误判点

- `cmake --build ... --target bloom_test` 可能只构建测试 binary，不一定运行测试；必须确认实际 GTest execution 证据。
- `bloom_test` target 名称可能同时包含多个 GTest suites；不能只看到 target build success 就宣称 28+21+65 个用例 PASS。
- macOS 环境可能没有 `nproc`。这是命令 portability/environment issue，不是 gemini-bloom 实现 bug；应记录替代 job count。
- Host Redis 版本若为 6.2.16，只能证明本环境结果，不能复现 DESIGN.md 声称的 Redis 6.2.17 baseline。
- TCL 中标为 `EXPECTED GAP` 的 RESP3 和 RedisBloom byte-offset SCANDUMP 差异不应计为 FAIL；但 expected gap 机制本身必须被记录。
- RedisBloom 兼容 fixtures 缺失不是 Stage 02 build/test failure；它是 Stage 00 open finding，后续 Stage 05/06 继续处理。
- SCANDUMP/LOADCHUNK 与 RedisBloom 不互通、BF.DEBUG 不支持、RESP3 不支持、command-AOF non-preamble 不互通是 DESIGN_INTENDED 边界；现有测试如果将这些写成 expected gap，不能误判为产品 bug。
- TCL 测试失败可能来自环境：redis-server 缺失、端口冲突、module load path 错、Redis 版本行为差异、权限或临时目录问题。必须先分类，不能直接归因于实现。
- Build warning 不等于 FAIL，但影响 C++20 portability、UB 或 sanitizer 的 warning 应记录为风险，供 Stage 03/08 深审。
- 不允许通过修改 CMake、测试源码、TCL oracle 或 production code 来使 Stage 02 通过；若现有测试本身错误，应记录为测试 oracle 问题。
- 测试全部通过也不等于没有问题；只能说明现有 baseline 在当前环境 PASS，后续 stages 仍需覆盖 runtime semantics、compat、persistence、fuzz、sanitizer、ops、perf。

## 7. PASS / FAIL / BLOCKED 判据

PASS：

- Stage 02 required evidence 目录和文件全部存在。
- CMake configure/build 命令实际运行，stdout/stderr/exit code 完整落盘。
- GTest target build 和实际 GTest execution 均有证据；若 target build 自动执行测试，也有日志证明。
- TCL 集成测试实际运行并有 Redis/module/version/test summary 证据。
- 所有失败、skip、expected gap 均被显式分类。
- `design_test_claim_check.md` 核对 DESIGN.md 测试声明与仓库实际对象。
- `stage_result.md` 引用具体 evidence 路径，没有把未运行项写成 PASS。
- Reviewer verdict 为 PASS，并确认没有静默忽略失败。

FAIL：

- 构建或测试运行发现违反 DESIGN.md 明确承诺的行为，并有复现/日志证据。
- 测试失败被分类为真实实现 bug，且不是 DESIGN_INTENDED、环境问题或测试 oracle 错。
- DESIGN.md 声称的现有测试入口/target 与仓库实际严重不符，且不是单纯环境缺失。
- Stage result 声称 PASS 但日志显示失败、crash、timeout、未运行或缺失证据。
- 主 agent 修改 production code、删除/弱化测试或改 oracle 来制造通过。

BLOCKED：

- 无法读取必要 stage/control/DESIGN 文件，或无法写入 Stage 02 evidence/result 文件。
- CMake/compiler/GTest infrastructure 缺失导致 build/GTest 无法验证，并有原始错误证据。
- `redis-server`、`tclsh`、module load path、权限或端口问题导致 TCL 无法验证，并有原始错误证据。
- CMake configure 失败导致后续 build/GTest/TCL 均无法形成有效证据；仍需记录后续项为 BLOCKED/NOT_VERIFIED，而非省略。
- Stage 02 允许 BLOCKED 后继续，但最终报告可信度必须下降，且 LOOP_STATE/global blockers 需要记录影响。

NOT_VERIFIED：

- 未实际运行的 GTest suite、TCL 场景或 DESIGN.md 声称测试数量必须标为 NOT_VERIFIED。
- RedisBloom v2.4.20 oracle、RDB/DUMP/RESTORE/MIGRATE/fullsync/AOF 双向矩阵、fuzz、sanitizer、cluster、performance 均不属于 Stage 02 验证范围，继续保持 NOT_VERIFIED/VERIFY_LATER。

DESIGN_INTENDED：

- RESP3 不支持、BF.DEBUG 不支持、RedisBloom SCANDUMP/LOADCHUNK 不互通、非 RDB-preamble command-AOF 不跨实现兼容、BF.INFO Size 统计口径差异、严格 parser 差异等，如测试输出将其标为 expected gap，应归为 DESIGN_INTENDED 而非 bug。

## 8. 对最终报告的影响

Stage 02 是最终中文报告中“现有测试基线”和“可信度评级”的关键输入：

- 若 build/GTest/TCL 全部 PASS，最终报告只能写“当前环境下现有构建和测试 baseline PASS”，不能写“没有问题”或“完全兼容 RedisBloom”。
- 若 GTest 或 TCL BLOCKED，最终报告可信度不能为 High，且必须说明缺失的是现有测试基线证据。
- 若测试失败被分类为真实实现 bug，应进入最终问题清单，包含 severity、影响、复现命令、expected/actual、证据路径和建议修复方向。
- 若失败属于测试 oracle 错，应进入测试覆盖/测试质量问题清单，不能当作生产实现 PASS 的替代证据。
- 若失败属于 DESIGN_INTENDED，最终报告应在 DESIGN 约束章节解释边界，避免读者误解为 RedisBloom drop-in 兼容缺陷。
- 若 Stage 02 使用 Redis 6.2.16 或其它非 DESIGN 历史基线版本，最终报告必须写清楚实际环境，不能声称复现 Redis 6.2.17 + RedisBloom v2.4.20 历史矩阵。
- Stage 02 不能关闭 Stage 00 的 RedisBloom fixture 缺失 finding，除非提供同等或更强的可复现 compat evidence；通常该 finding 留给 Stage 05/06。
