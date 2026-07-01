# Stage 02 — BUILD_EXISTING_TESTS


## Universal stage protocol

本 stage 必须遵守：

1. 重新读取 `modules/gemini-bloom/DESIGN.md`、`LOOP_CONTROL_BATCH.md`、所有 policies、`LOOP_STATE.md`、本 stage 文件。
2. 创建 `.codex/gemini-bloom-audit/v6/agents/stage02/rehydrate_log.md`。
3. 创建 planner 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage02/planner_output.md`。
4. 主 agent 审核 planner，然后执行。
5. 所有证据写入 `.codex/gemini-bloom-audit/v6/evidence/stage02/`。
6. 写 `.codex/gemini-bloom-audit/v6/agents/stage02/stage_result.md`。
7. 创建 reviewer 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage02/reviewer_output.md`。
8. reviewer 未 PASS 时必须补跑/修正/降级，然后重新 reviewer。
9. 更新 `LOOP_STATE.md`。
10. commit + push。
11. 标记 planner/reviewer closed。



## Goal

运行现有构建、GTest、TCL 集成测试，建立 baseline。此阶段只收集现状，不修生产代码。

## DESIGN.md constraints

DESIGN.md 声称项目有 BloomLayer、ScalingBloomFilter、RDB/wire、TCL 集成测试体系，并列出构建和运行方式。本 stage 必须验证这些声明是否真实可运行。

## Required evidence

- `evidence/stage02/build/`
- `evidence/stage02/gtest/`
- `evidence/stage02/tcl/`
- `evidence/stage02/design_test_claim_check.md`

## Main tasks

执行并记录：

```bash
cmake -B build
cmake --build build -j$(nproc)
cmake --build build -j$(nproc) --target bloom_test
tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl ./build/redis_bloom.so
```

如果 GTest 不存在、redis-server 不存在、tclsh 不存在，记录 BLOCKED evidence，但继续静态审计阶段。

必须分类测试失败：

- 真实实现 bug。
- DESIGN_INTENDED 差异。
- 测试 oracle 错。
- 环境问题。
- NOT_VERIFIED。

## Pass criteria

- 构建和测试输出完整落盘。
- 失败项有分类。
- reviewer 确认没有把失败静默忽略。

## Commit message

`audit(gemini-bloom): v6 stage 02 build existing tests`

