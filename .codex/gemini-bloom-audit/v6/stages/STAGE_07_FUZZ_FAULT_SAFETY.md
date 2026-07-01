# Stage 07 — FUZZ_FAULT_SAFETY


## Universal stage protocol

本 stage 必须遵守：

1. 重新读取 `modules/gemini-bloom/DESIGN.md`、`LOOP_CONTROL_BATCH.md`、所有 policies、`LOOP_STATE.md`、本 stage 文件。
2. 创建 `.codex/gemini-bloom-audit/v6/agents/stage07/rehydrate_log.md`。
3. 创建 planner 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage07/planner_output.md`。
4. 主 agent 审核 planner，然后执行。
5. 所有证据写入 `.codex/gemini-bloom-audit/v6/evidence/stage07/`。
6. 写 `.codex/gemini-bloom-audit/v6/agents/stage07/stage_result.md`。
7. 创建 reviewer 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage07/reviewer_output.md`。
8. reviewer 未 PASS 时必须补跑/修正/降级，然后重新 reviewer。
9. 更新 `LOOP_STATE.md`。
10. commit + push。
11. 标记 planner/reviewer closed。



## Goal

审计非信任输入、恶意 payload、异常流程和故障注入安全性，重点是 RDB/wire/LOADCHUNK。

## Main fuzz areas

- LOADCHUNK header too short/too long/random bytes。
- numLayers=0、>1024。
- unknown flags、RawBits、Loading flag 注入。
- expansion=0、UINT_MAX、溢出。
- totalItems 与 itemCount sum 不一致。
- itemCount > capacity。
- dataSize 与 totalBits 不一致。
- log2Bits >=64 或 totalBits != 2^log2Bits。
- hashCount=0 或与 bitsPerEntry 不一致。
- fpRate NaN/Inf/0/1/负数。
- bitsPerEntry NaN/Inf/0/>1000。
- bit payload 长度不匹配。
- cursor 乱序、重复、跳过、cursor>1 对不存在/已完成 key。
- half-loaded filter 上执行 ADD/MADD/INSERT/EXISTS/MEXISTS/INFO/CARD/SCANDUMP。
- malformed RDB stream。
- kill/restart during BGSAVE/BGREWRITEAOF，如果环境允许。

## Safety expectations

- 不 crash。
- 不 UAF/越界。
- 不破坏已有 key。
- 不制造 false negative。
- 错误返回稳定。
- loading 状态符合 DESIGN.md。

## Required evidence

- `evidence/stage07/fuzz_seeds.txt`
- `evidence/stage07/fuzz_results.log`
- `evidence/stage07/crash_or_failure_repro/` 如有失败
- `evidence/stage07/safety_matrix.md`

## Pass criteria

- 至少覆盖每类恶意输入。
- 每个失败有最小复现。
- 无法运行 fuzz 时，至少静态构造 corpus 并标记 BLOCKED runtime。
- reviewer 确认没有把 fuzz crash 忽略。

## Commit message

`audit(gemini-bloom): v6 stage 07 fuzz fault safety`

