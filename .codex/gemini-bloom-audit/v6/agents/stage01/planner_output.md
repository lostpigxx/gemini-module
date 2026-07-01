# Stage 01 Planner Output

## 1. Stage 目标

Stage 01 的目标是建立 gemini-bloom v6 审计的环境、仓库、依赖、分支和 commit 基线，使后续 Stage 02-10 的构建、运行、兼容性、安全和性能证据可以复现。

本 stage 不判定 gemini-bloom 功能正确性，不运行构建产物测试，不验证 RedisBloom 兼容性承诺；只记录可复现性所需的外部条件和仓库状态。

必须产出的核心结果：

- 当前 commit、当前分支、dirty tree、remote 配置可追溯。
- 目标审计分支 `audit/gemini-bloom-v6` 存在，并完成 push。
- 操作系统、编译器、CMake、Redis、TCL、Python、Docker 等后续 stage 依赖工具版本已落盘。
- `modules/gemini-bloom` 文件树快照已落盘，便于后续证明审计对象范围。
- 依赖缺失或 push 失败必须作为 BLOCKED 证据记录，不能口头说明。

## 2. DESIGN.md 相关约束

与 Stage 01 直接相关的 DESIGN.md 约束主要是“验证边界”和“运行环境假设”，而不是 Bloom 算法行为：

- gemini-bloom 是 C++20 Redis Module，后续构建基线需要能反映 C++ 编译器与 CMake 可用性。
- 目标运行环境是 Redis 6.x / 7.x，且不与 RedisBloom 或 Redis 8 内置 Bloom 同实例共存；Stage 01 应记录本机 `redis-server` / `redis-cli` 版本，但不应据此判定兼容性 PASS。
- DESIGN.md 声称 Redis 6.2.17 + RedisBloom v2.4.20 曾用于兼容性矩阵。Stage 01 只能记录本机是否具备相关工具或依赖，不能把该声明升级为已复现。
- DESIGN.md 声称测试体系包括 GTest、TCL 集成测试、RDB/wire 测试和 Redis 进程隔离。Stage 01 应记录 GTest/TCL/Redis/Docker 等依赖状态，为 Stage 02 之后判断 BLOCKED 或 PASS 提供基础。
- DESIGN.md 明确 SCANDUMP/LOADCHUNK 不与 RedisBloom 互通、RESP3 不支持、BF.DEBUG 不支持、command-AOF rewrite 非 RDB-preamble 模式不跨实现兼容。Stage 01 只需保留这些边界，不应把工具缺失或未运行测试误判为这些设计内差异的实现问题。
- DESIGN.md 的兼容性、持久化、安全、资源限制和测试覆盖声明仍需后续 stage 证据；Stage 01 的环境基线不能替代功能验证。

## 3. 必审对象

Stage 01 主 agent 必须审计和记录以下对象：

- Git 基线：
  - `git rev-parse HEAD`
  - `git branch --show-current`
  - `git status --short`
  - `git remote -v`
  - 目标分支 `audit/gemini-bloom-v6` 的 checkout / push 状态
- 系统与工具基线：
  - `uname -a`
  - `/etc/os-release`，如当前系统不存在该文件，应记录失败输出或等价 blocker 说明
  - `cmake --version`
  - `c++ --version`
  - `g++ --version`
  - `clang++ --version`
  - `redis-server --version`
  - `redis-cli --version`
  - `tclsh` patchlevel
  - `python3 --version`
  - `docker --version`
- 仓库审计范围：
  - `modules/gemini-bloom` 下 maxdepth 4 的文件列表
  - `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md` 中 Stage 00 PASS 状态和 Stage 01 PENDING 状态
  - Stage 00 的 open findings：`GBV6-00-001` 和 `GBV6-00-002`
- Stage 01 自身流程文件：
  - `rehydrate_log.md`
  - `planner_output.md`
  - 后续主 agent 应写的 `main_execution.md`、`stage_result.md`、`reviewer_output.md`

不得审计为 Stage 01 PASS 的对象：

- 不运行 CMake build 或 GTest/TCL 测试。
- 不启动 redis-server。
- 不运行 RedisBloom oracle。
- 不运行 Docker 容器。
- 不修改 `modules/gemini-bloom/src/**` 或现有测试制造通过。

## 4. 运行/静态检查计划

Planner 子 agent 本身不执行检查；以下是主 agent 在 Stage 01 execution 阶段应执行并落盘的计划。

1. Rehydrate：
   - 重新读取 DESIGN.md、LOOP_CONTROL_BATCH、所有 policies、LOOP_STATE、Stage 01 文件、Stage 00 result 和 reviewer output。
   - 写 `agents/stage01/rehydrate_log.md`，列出读取文件、Stage 相关 DESIGN 约束、上一 stage 状态、本 stage 禁止越界项。

2. Git snapshot：
   - 记录 HEAD、branch、status、remote。
   - 确认当前分支是否为 `audit/gemini-bloom-v6`。
   - 如需切换或创建分支，按 commit/push policy 使用目标分支，不覆盖未解释改动。

3. Environment snapshot：
   - 记录 OS 信息和基础路径上下文。
   - 对不可用命令使用 `|| true` 风格继续收集其余信息，但必须保留 stderr/exit code。

4. Tool versions：
   - 逐项记录 CMake、C++ 编译器、Redis、TCL、Python、Docker 版本。
   - 对缺失工具分类为 dependency gap；是否 BLOCKED 取决于 Stage 01 pass criteria 和后续 stage 必需性。

5. Repo tree snapshot：
   - 记录 `modules/gemini-bloom -maxdepth 4 -type f | sort` 的结果。
   - 若 Stage 00 finding `GBV6-00-001` 涉及的 `tests/compat/redisbloom-2.4.20/` 仍不存在，应在 Stage 01 result 中保留该 finding 的影响，不要静默关闭。

6. Dependency status：
   - 汇总哪些工具存在、哪些缺失、哪些版本与 DESIGN 或后续 stage 期望存在差距。
   - 明确后续 Stage 02/05/06/08 可能受影响的项。

7. Branch push gate：
   - 执行 `git push -u origin audit/gemini-bloom-v6`。
   - push 失败则记录 `evidence/stage01/push_failure.log`，更新 stage 为 BLOCKED，不得进入 Stage 02。

8. Stage result and reviewer handoff：
   - 写 `main_execution.md` 和 `stage_result.md`。
   - 创建 reviewer output，由 reviewer 检查证据是否足够复现、是否有 overclaim、是否允许进入 Stage 02。

## 5. 证据清单

Stage 01 evidence 目录应为：

- `.codex/gemini-bloom-audit/v6/evidence/stage01/`

必须包含 stage 文件要求的证据：

- `git_snapshot.txt`
- `env_snapshot.txt`
- `tool_versions.txt`
- `repo_tree_gemini_bloom.txt`
- `dependency_status.txt`

同时按 evidence policy 至少包含：

- `commands.txt`：实际执行的所有命令，包含分支和 push 命令。
- `stdout.log`：标准输出汇总或逐命令输出索引。
- `stderr.log`：标准错误汇总，缺失工具、缺失文件、push 失败必须可见。
- `exit_codes.txt`：每个命令 exit code。
- `evidence_index.md`：证据索引，说明每个文件支持哪些结论。

如发生阻塞，还应包含：

- `push_failure.log`：push 失败原始输出。
- 对缺失工具的 stderr/exit code 记录，例如 `redis-server not found`、`docker: command not found`、`tclsh not found`。

Stage 01 结论引用证据时必须使用具体路径，不能写“已记录环境”但不指向文件。

## 6. 易误判点

- `redis-server --version` 存在不等于 Redis runtime 测试 PASS；只能说明工具可用。
- `docker --version` 存在不等于 RedisBloom v2.4.20 oracle 可用；RedisBloom 下载、加载和版本确认属于 Stage 05。
- `cmake` 或编译器存在不等于项目可构建；构建属于 Stage 02。
- `tclsh` 存在不等于 TCL 集成测试可运行；真实测试启动 Redis 和加载 module 属于 Stage 02。
- macOS 环境下 `/etc/os-release` 缺失可能是正常环境差异，应记录为环境事实，不应单独判 Stage 01 FAIL。
- dirty tree 如果只包含本 audit stage 正在生成的 `.codex/gemini-bloom-audit/v6/**` 文件，可以解释并继续；未知 production code dirty file 必须记录风险，不能覆盖或 revert。
- Stage 00 的 `GBV6-00-001` 是 DESIGN 声称 fixture path 缺失的 P3 finding；Stage 01 repo tree 快照可以补充证据，但不能在未发现实际 fixture 时关闭。
- Stage 00 的 `GBV6-00-002` 是 DESIGN 与源码注释的边界冲突；Stage 01 不应把它作为环境问题处理。
- 目标分支已存在不代表 push gate 通过；必须有本 stage 的 push 证据。
- push 权限失败是 BLOCKED_PUSH / BLOCKED，不应降级为 NOT_VERIFIED 后继续 Stage 02，因为 Stage 01 明确不允许 BLOCKED 后继续。

## 7. PASS / FAIL / BLOCKED 判据

PASS：

- Stage 01 required evidence 文件全部存在。
- Git HEAD、branch、status、remote 已落盘。
- OS/tool/dependency 版本或缺失状态已落盘。
- `modules/gemini-bloom` repo tree 快照已落盘。
- 当前审计分支为 `audit/gemini-bloom-v6`，且 push 成功。
- `stage_result.md` 引用具体 evidence 路径。
- reviewer verdict 为 PASS。
- LOOP_STATE 已更新，planner/reviewer closed 状态已记录。

FAIL：

- Stage 01 结论与证据矛盾，例如声称工具可用但版本文件显示 command not found。
- 漏掉 required evidence 且未标记 BLOCKED。
- 未执行或未记录 branch/push gate，却声称可进入 Stage 02。
- 将未运行的 build/test/compatibility 检查写成 PASS。
- 修改 production code 或删除/弱化测试来满足基线。

BLOCKED：

- 无法读取必要仓库文件或无法写入 Stage 01 evidence/result 文件。
- 目标分支无法创建或 checkout，且无法在当前仓库安全继续。
- `git push -u origin audit/gemini-bloom-v6` 失败；Stage 01 文件要求此时不得进入 Stage 02。
- 缺失关键环境工具本身通常不阻塞 Stage 01，只要已记录；但如果缺失导致 required evidence 无法形成，应 BLOCKED 并记录原始错误。

NOT_VERIFIED：

- Stage 01 不运行的功能项必须保持 NOT_VERIFIED 或 VERIFY_LATER：构建、GTest、TCL、Redis runtime、RedisBloom oracle、RDB/DUMP/RESTORE/MIGRATE/fullsync/AOF、fuzz、sanitizer、performance。

DESIGN_INTENDED：

- SCANDUMP/LOADCHUNK 不与 RedisBloom 互通、RESP3 不支持、BF.DEBUG 不支持、command-AOF non-preamble 不跨实现兼容等只作为后续报告边界保留，不作为 Stage 01 环境问题。

## 8. 对最终报告的影响

Stage 01 的最终报告影响是“可信度基础”，不是功能结论：

- 最终报告必须引用 Stage 01 的 git/env/tool evidence，说明本轮审计发生在哪个 commit、branch、OS 和工具版本上。
- 如果 Stage 01 发现 Redis、TCL、Docker、编译器或 CMake 缺失，最终报告必须把受影响的后续 stage 标为 BLOCKED 或降低可信度，除非后续 stage 使用替代环境补足证据。
- 如果 push gate 失败，Stage 01 必须 BLOCKED，最终报告或 LOOP_STATE 需要说明审计分支未完成远端同步，且不得宣称本轮 stage gate 完整通过。
- Stage 01 repo tree 快照会支持 Stage 00 finding `GBV6-00-001` 的后续判断：若 compat fixture path 仍缺失，最终报告必须保留该 P3 evidence gap 或由后续 stage 给出 superseding evidence。
- Stage 01 不能提高 RedisBloom 兼容性、持久化、安全或测试覆盖的结论可信度；这些只能由 Stage 02-10 的实际证据决定。
- 最终中文报告应避免把“工具存在”写成“功能已验证”，并应明确未运行项的 `NOT_VERIFIED` / `VERIFY_LATER` 状态。
