# Stage 05 — REDISBLOOM_COMPAT


## Universal stage protocol

本 stage 必须遵守：

1. 重新读取 `modules/gemini-bloom/DESIGN.md`、`LOOP_CONTROL_BATCH.md`、所有 policies、`LOOP_STATE.md`、本 stage 文件。
2. 创建 `.codex/gemini-bloom-audit/v6/agents/stage05/rehydrate_log.md`。
3. 创建 planner 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage05/planner_output.md`。
4. 主 agent 审核 planner，然后执行。
5. 所有证据写入 `.codex/gemini-bloom-audit/v6/evidence/stage05/`。
6. 写 `.codex/gemini-bloom-audit/v6/agents/stage05/stage_result.md`。
7. 创建 reviewer 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage05/reviewer_output.md`。
8. reviewer 未 PASS 时必须补跑/修正/降级，然后重新 reviewer。
9. 更新 `LOOP_STATE.md`。
10. commit + push。
11. 标记 planner/reviewer closed。



## Goal

以 RedisBloom v2.4.20 为优先 oracle，验证 DESIGN.md 声明的兼容性边界：RDB/DUMP/RESTORE/MIGRATE/fullsync/RDB-preamble AOF 应兼容；RESP3、SCANDUMP/LOADCHUNK、command-AOF rewrite 不要求跨实现兼容。

## Oracle priority

优先使用：

- Redis 6.2.17
- RedisBloom v2.4.20

如果无法获得完全一致版本，允许记录替代版本，但必须降级结论，不能外推。

## Main tasks

建立两个 Redis 实例：

1. gemini-bloom instance：加载 `./build/redis_bloom.so`
2. RedisBloom oracle instance：加载官方 RedisBloom v2.4.20 或 redis-stack 中 RedisBloom

对比：

- command surface。
- RESP2 raw reply。
- RESP3 raw reply，按 DESIGN.md 处理。
- BF.INFO 差异。
- BF.INSERT EXPANSION 0 差异。
- unknown option / duplicate option 差异。
- SCANDUMP/LOADCHUNK 跨实现差异，预期 DESIGN_INTENDED。
- RDB/DUMP/RESTORE/MIGRATE/fullsync/RDB-preamble AOF 兼容项。

## Corpus

至少覆盖：

- empty
- single-layer
- multi-layer
- fixed/non-scaling
- expansion 1/2/4
- binary items
- long item
- large item，如环境允许
- high false positive scenario

## Required evidence

- `evidence/stage05/oracle_env.txt`
- `evidence/stage05/diff_raw_resp.log`
- `evidence/stage05/diff_normalized.md`
- `evidence/stage05/compatibility_matrix.md`
- `evidence/stage05/blocked_oracle.md` 如果 oracle 不可用

## Pass criteria

- RedisBloom oracle 可用时，有双实例差分结果。
- RedisBloom oracle 不可用时，明确 BLOCKED 并降级最终可信度。
- DESIGN_INTENDED 差异不被误判为 bug。
- DESIGN 声明兼容的路径若失败，必须记录 P1/P0 finding。

## Commit message

`audit(gemini-bloom): v6 stage 05 redisbloom compat`

