# LOOP CONTROL BATCH — gemini-bloom v6 审计

## 0. Loop 不变量

本文件是本轮 Codex 审计的主控文件。每个 stage 开始时必须重新读取本文件，避免上下文 compact、遗忘或子 agent 上下文丢失导致流程漂移。

硬性不变量：

1. `modules/gemini-bloom/DESIGN.md` 是最高优先级的设计标准。
2. DESIGN.md 中明确声明“不支持/不兼容/有意差异”的行为，不得直接判定为 bug；必须判为“设计内差异”，再验证实现是否确实符合该设计边界。
3. DESIGN.md 中没有写的工程风险仍必须审计；不得以“设计没要求”为由跳过内存安全、UB、恶意输入、持久化损坏、复制、集群、性能和测试质量。
4. 所有结论必须有证据路径。没有运行过的内容只能标记为 `NOT_VERIFIED` 或 `BLOCKED`。
5. 每个 stage 必须执行 planner 子 agent → 主 agent 执行 → reviewer 子 agent 审计 → 修正/补跑 → commit → push → 关闭子 agent → 进入下一 stage。
6. 不允许用修改/删除/弱化已有测试的方式制造通过。
7. 默认不修改 `modules/gemini-bloom/src/` 的生产代码。本轮目标是审计，不是修复。若发现 bug，记录 finding 和最小复现，不直接修复生产代码。
8. 所有 agent 输出、证据、日志、状态必须落盘到 `.codex/gemini-bloom-audit/v6/`。
9. 最终人类可读报告必须是中文，落盘到 `doc/code_review/gemini-bloom/v6/`。

## 1. Stage 生命周期

每个 stage 按固定生命周期运行：

```text
[Rehydrate]
    │  重新读取 DESIGN.md + control batch + policies + LOOP_STATE + 当前 stage 文件
    ▼
[Planner Subagent]
    │  只分析，不执行；输出 planner_output.md
    ▼
[Main Agent Review]
    │  主 agent 审核 planner，必要时补充计划
    ▼
[Execution]
    │  静态审计 / 运行测试 / 差分 / fuzz / 证据落盘
    ▼
[Stage Result]
    │  写 stage_result.md、更新 findings/matrix/state
    ▼
[Reviewer Subagent]
    │  审计本 stage 结论和证据；输出 reviewer_output.md
    ▼
[Gate]
    ├── PASS     → commit + push + close agents + next stage
    ├── FAIL     → 主 agent 修正/补跑，然后重新 reviewer
    └── BLOCKED  → 记录阻塞、commit + push；是否继续由 stage 文件决定
```

## 2. Stage 列表

| Stage | 文件 | 目标 | 是否可在 BLOCKED 后继续 |
|---|---|---|---|
| 00 | `stages/STAGE_00_DESIGN_CONTRACT.md` | 抽取 DESIGN 约束，建立设计契约和审计基线 | 否 |
| 01 | `stages/STAGE_01_ENV_REPO_BASELINE.md` | 环境、仓库、依赖、分支、commit 基线 | 否 |
| 02 | `stages/STAGE_02_BUILD_EXISTING_TESTS.md` | 构建、GTest、TCL 现有测试基线 | 是 |
| 03 | `stages/STAGE_03_STATIC_DEEP_AUDIT.md` | 源码深度静态审计 | 是 |
| 04 | `stages/STAGE_04_RUNTIME_COMMAND_SEMANTICS.md` | 真实 Redis runtime 命令语义矩阵 | 是 |
| 05 | `stages/STAGE_05_REDISBLOOM_COMPAT.md` | RedisBloom v2.4.20 兼容性和差分 oracle | 是，但必须降级可信度 |
| 06 | `stages/STAGE_06_PERSISTENCE_TRANSPORT.md` | RDB/DUMP/RESTORE/MIGRATE/fullsync/AOF/SCANDUMP/LOADCHUNK | 是 |
| 07 | `stages/STAGE_07_FUZZ_FAULT_SAFETY.md` | 恶意输入、fuzz、故障注入、安全边界 | 是 |
| 08 | `stages/STAGE_08_SANITIZER_MEMORY.md` | ASAN/UBSAN/内存安全/UB | 是 |
| 09 | `stages/STAGE_09_REPLICA_CLUSTER_OPS.md` | replica、cluster、ACL、COMMAND metadata、操作性 | 是 |
| 10 | `stages/STAGE_10_PERF_RESOURCE.md` | 性能、资源、内存统计、极限参数 | 是 |
| 11 | `stages/STAGE_11_FINAL_REPORT_SYNTHESIS.md` | 归纳所有阶段，生成中文最终报告 | 否 |
| 12 | `stages/STAGE_12_FINAL_REPORT_AUDIT.md` | 审计最终报告本身，补证据/修正结论 | 否 |

## 3. 状态文件

主状态文件：

- `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`

每个 stage 必须追加或更新：

- stage 状态：`PENDING / RUNNING / REVIEW_FAILED / PASS / FAIL / BLOCKED / NOT_VERIFIED`
- planner output 路径
- execution evidence 路径
- reviewer output 路径
- commit SHA
- push 状态
- 子 agent close 状态
- 下一 stage

## 4. Severity 规则

| Severity | 定义 |
|---|---|
| P0 | crash、UAF、越界、UB 可触发 Redis 进程退出；持久化损坏；数据丢失；RDB/AOF 无法恢复且不在 DESIGN 限制内 |
| P1 | 违反 DESIGN 明确承诺的兼容性；RDB/DUMP/RESTORE/MIGRATE/fullsync 关键路径不兼容；RESP/命令行为破坏主要客户端；安全边界缺失 |
| P2 | 边界条件错误、错误类型不稳定、统计错误、资源限制不足、测试覆盖缺口影响判断 |
| P3 | 文档不一致、非关键兼容差异、测试可维护性、可观测性、性能风险 |
| INFO | 设计内差异、已知限制、非 bug 但需在报告中说明 |

## 5. 结论状态

所有审计项必须归类为：

- `PASS`：已运行或已静态验证，证据充分。
- `FAIL`：发现问题，有复现和证据。
- `BLOCKED`：由于环境、权限、依赖缺失无法验证，有阻塞证据。
- `NOT_VERIFIED`：本轮没有覆盖，必须在最终报告降级可信度。
- `DESIGN_INTENDED`：与 RedisBloom 或一般预期不同，但符合 DESIGN.md 明确约束。

## 6. 最终完成条件

Stage 12 结束时必须满足：

1. `doc/code_review/gemini-bloom/v6/` 下存在完整中文报告文件。
2. `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md` 记录所有 stage 的状态、commit、push。
3. 所有 `FAIL` 都有最小复现和 evidence。
4. 所有 `BLOCKED` 都有阻塞原因和可信度影响。
5. 报告自审结果为 PASS，或明确记录未通过且停止。
6. 最后一个 commit 已 push 到远端审计分支。
