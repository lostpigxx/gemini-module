# Stage 00 — DESIGN_CONTRACT


## Universal stage protocol

本 stage 必须遵守：

1. 重新读取 `modules/gemini-bloom/DESIGN.md`、`LOOP_CONTROL_BATCH.md`、所有 policies、`LOOP_STATE.md`、本 stage 文件。
2. 创建 `.codex/gemini-bloom-audit/v6/agents/stage00/rehydrate_log.md`。
3. 创建 planner 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage00/planner_output.md`。
4. 主 agent 审核 planner，然后执行。
5. 所有证据写入 `.codex/gemini-bloom-audit/v6/evidence/stage00/`。
6. 写 `.codex/gemini-bloom-audit/v6/agents/stage00/stage_result.md`。
7. 创建 reviewer 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage00/reviewer_output.md`。
8. reviewer 未 PASS 时必须补跑/修正/降级，然后重新 reviewer。
9. 更新 `LOOP_STATE.md`。
10. commit + push。
11. 标记 planner/reviewer closed。



## Goal

抽取 `modules/gemini-bloom/DESIGN.md` 的设计契约，建立本轮审计的基线。此阶段是后续所有 stage 的标准来源。

## Required outputs

- `.codex/gemini-bloom-audit/v6/agents/stage00/design_contract.md`
- `.codex/gemini-bloom-audit/v6/agents/stage00/design_claims_matrix.md`
- `.codex/gemini-bloom-audit/v6/agents/stage00/stage_result.md`

## Main tasks

1. 读取完整 DESIGN.md。
2. 提取以下设计契约：
   - 产品定位：不是 RedisBloom drop-in 替代品。
   - 支持/不支持的兼容层级。
   - RedisBloom v2.4.20 版本边界。
   - RDB/DUMP/RESTORE/MIGRATE/fullsync/RDB-preamble AOF 承诺。
   - SCANDUMP/LOADCHUNK 私有协议边界。
   - RESP3 不支持边界。
   - command-AOF rewrite 非跨实现兼容边界。
   - 命令列表、flags、参数范围、资源限制。
   - RDB 字段序列、encver、hash seed、校验规则。
   - 现有测试设计声明。
   - 已知限制。
3. 建立 `DESIGN_INTENDED` 差异表，避免后续误判。
4. 建立 `DESIGN_CLAIM_REQUIRES_VERIFICATION` 表，列出 DESIGN.md 中每一个需要被运行验证的声明。
5. 标记 DESIGN.md 自身需要审计的点：例如声明的 corpus 是否存在、CI gate 是否存在、测试数量是否真实。

## Pass criteria

- design_contract.md 覆盖 DESIGN.md 的所有主要约束。
- design_claims_matrix.md 每一项都有后续 stage 映射。
- reviewer 确认没有把 DESIGN 明确限制误写成 bug。

## Commit message

`audit(gemini-bloom): v6 stage 00 design contract`

