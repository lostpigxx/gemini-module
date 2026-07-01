# Stage 00 Planner Output

## 1. Stage 目标

Stage 00 的目标是把 `modules/gemini-bloom/DESIGN.md` 转换成本轮 v6 审计的可执行设计契约，作为后续所有 stage 的判断标准。主 agent 应只做文档抽取、静态核对和证据整理，不运行测试，不修改 `modules/gemini-bloom/src/` 等生产代码。

本 stage 应产出：

- `.codex/gemini-bloom-audit/v6/agents/stage00/design_contract.md`：按主题提取 DESIGN.md 的设计承诺、设计内差异、资源/安全边界和已知限制。
- `.codex/gemini-bloom-audit/v6/agents/stage00/design_claims_matrix.md`：列出 DESIGN.md 中需要后续验证的声明，并映射到 Stage 01-12。
- `.codex/gemini-bloom-audit/v6/agents/stage00/stage_result.md`：记录 planner 采纳情况、执行动作、证据路径、findings、分类结论和 agent closed 状态。

Stage 00 是不可 BLOCKED 后继续的基线阶段。如果无法完整读取 DESIGN.md、控制文件、policies、LOOP_STATE 或 stage 文件，应阻塞并记录影响。

## 2. DESIGN.md 相关约束

必须优先抽取并固定以下 DESIGN.md 约束：

- 产品定位：gemini-bloom 是独立 C++20 Redis Module，不是 RedisBloom 源码移植，也不是 RedisBloom drop-in 替代品。
- 兼容承诺：RDB 序列化、DUMP/RESTORE、MIGRATE、psync/fullsync replication、RDB-preamble AOF 是兼容层级；兼容基线限定为 Redis 6.2.17 + RedisBloom v2.4.20 已验证范围。
- 设计内差异：SCANDUMP/LOADCHUNK 使用 gemini 私有 layer-index cursor 协议，不与 RedisBloom 互通；command-AOF rewrite 不跨实现兼容；RESP3、BF.DEBUG 不支持。
- 命令契约：BF.RESERVE、BF.ADD、BF.MADD、BF.INSERT、BF.EXISTS、BF.MEXISTS、BF.INFO、BF.CARD、BF.SCANDUMP、BF.LOADCHUNK 的 flags、返回语义、错误边界和部分失败语义。
- 参数和资源限制：capacity 1..2^30，error_rate 为有限 (0,1)，expansion 0..32768，max layers 1024，单层 data size <= 2GB，总 data size <= 4GB，bitsPerEntry <= 1000。
- RDB/wire 契约：data type name `MBbloom--`，encver 2/4，字段序列、hash seed、flags 持久化掩码、narrowing cast 前置检查、ValidateLayerFields 校验规则。
- 安全契约：RDB 文件和 LOADCHUNK payload 视为非信任输入；所有 write 命令 deny-oom；LOADCHUNK loading 状态保护已完成 key 不被覆写。
- 测试声明：GTest、TCL、compat corpus、RedisBloom v2.4.20 双向矩阵、AOF/RDB/replication 等覆盖声明都应进入 claims matrix，不能在 Stage 00 直接视为已验证。
- 已知限制：SCANDUMP/LOADCHUNK 不互通、非 RDB preamble command-AOF 不互通、live command replication 下 BF.CARD 差异、BF.INFO Size 统计口径差异、不支持删除、EXPANSION 1 查询性能风险、AOF rewrite OOM 跳过 key 风险、与 RedisBloom/Redis 8 Bloom 同实例互斥。

## 3. 必审对象

Stage 00 必须审阅这些输入文件并在 rehydrate/stage_result 中引用：

- `modules/gemini-bloom/DESIGN.md`
- `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`
- `.codex/gemini-bloom-audit/v6/policies/00_design_first_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/01_rehydration_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/02_subagent_protocol.md`
- `.codex/gemini-bloom-audit/v6/policies/03_evidence_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/04_quality_gates.md`
- `.codex/gemini-bloom-audit/v6/policies/05_commit_push_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/06_final_report_policy.md`
- `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`
- `.codex/gemini-bloom-audit/v6/stages/STAGE_00_DESIGN_CONTRACT.md`

Stage 00 输出应重点审阅：

- `design_contract.md` 是否覆盖 DESIGN.md 的主要章节和限制。
- `design_claims_matrix.md` 是否把每项设计承诺映射到后续 stage。
- `stage_result.md` 是否明确 DESIGN_INTENDED、DESIGN_CLAIM_REQUIRES_VERIFICATION、NOT_VERIFIED、BLOCKED 的边界。

## 4. 运行/静态检查计划

本 planner 子任务不运行测试。主 agent 在 Stage 00 执行时也不应运行 GTest、TCL、Redis、Docker、fuzz、sanitizer 或 compat oracle。

允许的静态/文件级动作：

- 读取并摘要 DESIGN.md、控制文件、policies、LOOP_STATE、Stage 00 文件。
- 创建或更新 `.codex/gemini-bloom-audit/v6/agents/stage00/rehydrate_log.md`。
- 编写 `design_contract.md`，按“设计承诺 / 设计内差异 / 设计未覆盖风险 / 已知限制 / 后续验证需求”组织。
- 编写 `design_claims_matrix.md`，至少包含 claim id、DESIGN.md 来源章节、声明内容、分类、后续 stage、所需证据、当前状态。
- 编写 `.codex/gemini-bloom-audit/v6/evidence/stage00/` 下的证据索引和只读命令日志，记录读文件和静态核对动作。
- 编写 `stage_result.md` 并更新 `LOOP_STATE.md`，但不得修改生产代码。

可选的轻量静态核对仅限文件存在性和文本路径核对，例如确认 DESIGN.md 提到的 `tests/compat/redisbloom-2.4.20/`、测试文件路径、文档路径是否存在。若执行这些核对，也必须落盘证据；若未执行，标记为后续 stage 待验证或 NOT_VERIFIED，不得写成 PASS。

## 5. 证据清单

Stage 00 应至少准备以下证据：

- `.codex/gemini-bloom-audit/v6/evidence/stage00/commands.txt`：所有只读文件读取、静态核对和写审计文件相关命令。
- `.codex/gemini-bloom-audit/v6/evidence/stage00/stdout.log`：命令标准输出或摘要。
- `.codex/gemini-bloom-audit/v6/evidence/stage00/stderr.log`：命令标准错误。
- `.codex/gemini-bloom-audit/v6/evidence/stage00/exit_codes.txt`：每个命令的退出码。
- `.codex/gemini-bloom-audit/v6/evidence/stage00/env_snapshot.txt`：本阶段相关环境信息，例如 cwd、当前分支、git status、关键文件路径存在性。
- `.codex/gemini-bloom-audit/v6/evidence/stage00/evidence_index.md`：证据索引，说明每个证据支撑哪些 Stage 00 结论。
- `.codex/gemini-bloom-audit/v6/agents/stage00/rehydrate_log.md`：记录已读取文件、Stage 00 相关 DESIGN 约束、LOOP_STATE 当前状态和越界限制。

结论引用必须指向具体文件路径。DESIGN.md 中声称“已验证通过”的内容，在 Stage 00 只能作为 design claim 记录，不能作为运行证据。

## 6. 易误判点

- 不得把“不支持 RESP3”“不支持 BF.DEBUG”“SCANDUMP/LOADCHUNK 不与 RedisBloom 互通”“command-AOF rewrite 不跨实现兼容”判成 bug；这些应归为 `DESIGN_INTENDED`，除非实现或文档没有正确表达边界。
- 不得写“完全兼容 RedisBloom”。兼容范围必须限定在 DESIGN.md 承诺的 RDB/native transport 路径和 RedisBloom v2.4.20 基线。
- 不得把 DESIGN.md 中的测试覆盖声明当作本轮已验证结果。Stage 00 只能建立待验证矩阵。
- 不得因为 DESIGN.md 未要求某风险而跳过后续审计。UB、内存安全、恶意输入、资源耗尽、cluster/ACL/COMMAND metadata、性能和报告证据完整性仍需进入 claims 或 risk matrix。
- 不得运行测试或修改生产代码来“补强”Stage 00。发现缺口时应记录为待验证、NOT_VERIFIED 或 BLOCKED。
- DESIGN.md 自身也可能有问题，例如路径不存在、CI gate 未落实、兼容声明缺少可复现证据、已知限制未覆盖到最终用户文档。此类应作为文档/证据风险记录，而不是直接改设计结论。

## 7. PASS / FAIL / BLOCKED 判据

PASS 条件：

- 已完整读取 planner_prompt 要求的所有文件。
- `planner_output.md`、`rehydrate_log.md`、`design_contract.md`、`design_claims_matrix.md`、`stage_result.md` 均存在且结构完整。
- `design_contract.md` 覆盖 DESIGN.md 的产品定位、兼容边界、命令契约、资源限制、RDB/wire 契约、安全策略、测试声明和已知限制。
- `design_claims_matrix.md` 将每个需要验证的 DESIGN claim 映射到后续 stage 和证据类型。
- 明确列出 `DESIGN_INTENDED` 差异，避免后续误判。
- Stage 00 证据路径完整，结论没有声称未执行的测试结果。

FAIL 条件：

- 漏掉 DESIGN.md 的主要约束，特别是兼容承诺或已知不兼容边界。
- 把 DESIGN.md 明确声明的非目标误判为 bug，或把 RedisBloom 差异泛化成不准确结论。
- 把未运行的测试、未核对的 corpus 或未验证的兼容矩阵写成 PASS。
- 修改生产代码、删除/弱化测试，或运行本 stage 禁止的测试。

BLOCKED 条件：

- 必读文件缺失、无法读取或内容截断到无法建立设计契约。
- 无法写入 `.codex/gemini-bloom-audit/v6/agents/stage00/` 或 `.codex/gemini-bloom-audit/v6/evidence/stage00/`。
- Stage 00 关键输出无法生成，且不能通过只读静态方式补齐。

Stage 00 在 LOOP_CONTROL_BATCH 中标记为 BLOCKED 后不可继续；若 BLOCKED，必须记录 blocker、影响范围和证据路径。

## 8. 对最终报告的影响

Stage 00 的输出决定最终中文报告的边界和措辞：

- `design_contract.md` 应成为 `doc/code_review/gemini-bloom/v6/01_DESIGN约束与结论对齐.md` 的主要输入。
- `design_claims_matrix.md` 应驱动 Stage 02-10 的证据收集，并成为最终报告 evidence index、coverage matrix 和 NOT_VERIFIED 列表的基础。
- `DESIGN_INTENDED` 表应防止最终报告把已知限制写成 bug，同时要求清晰披露客户可见影响。
- 任何 DESIGN.md 自身不一致、路径缺失、CI 未覆盖或证据不可复现的问题，应进入最终报告的问题清单或测试覆盖缺口。
- 如果 Stage 00 未能建立完整设计契约，后续所有 stage 的结论可信度应降级，最终报告不得给出 High confidence。
