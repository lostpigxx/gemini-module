# Stage 03 — STATIC_DEEP_AUDIT


## Universal stage protocol

本 stage 必须遵守：

1. 重新读取 `modules/gemini-bloom/DESIGN.md`、`LOOP_CONTROL_BATCH.md`、所有 policies、`LOOP_STATE.md`、本 stage 文件。
2. 创建 `.codex/gemini-bloom-audit/v6/agents/stage03/rehydrate_log.md`。
3. 创建 planner 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage03/planner_output.md`。
4. 主 agent 审核 planner，然后执行。
5. 所有证据写入 `.codex/gemini-bloom-audit/v6/evidence/stage03/`。
6. 写 `.codex/gemini-bloom-audit/v6/agents/stage03/stage_result.md`。
7. 创建 reviewer 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage03/reviewer_output.md`。
8. reviewer 未 PASS 时必须补跑/修正/降级，然后重新 reviewer。
9. 更新 `LOOP_STATE.md`。
10. commit + push。
11. 标记 planner/reviewer closed。



## Goal

对 `modules/gemini-bloom` 进行源码级深度审计，覆盖实现正确性、内存安全、Redis Module API 使用、RDB/wire、命令语义和 DESIGN.md 对齐。

## Required source files

必须审计：

```text
modules/gemini-bloom/DESIGN.md
modules/gemini-bloom/CMakeLists.txt
modules/gemini-bloom/src/redis_bloom_module.cc
modules/gemini-bloom/src/bloom_commands.cc
modules/gemini-bloom/src/bloom_commands.h
modules/gemini-bloom/src/bloom_rdb.cc
modules/gemini-bloom/src/bloom_rdb.h
modules/gemini-bloom/src/bloom_filter.cc
modules/gemini-bloom/src/bloom_filter.h
modules/gemini-bloom/src/sb_chain.cc
modules/gemini-bloom/src/sb_chain.h
modules/gemini-bloom/src/bloom_config.cc
modules/gemini-bloom/src/bloom_config.h
modules/gemini-bloom/src/murmur2.cc
modules/gemini-bloom/src/murmur2.h
modules/gemini-bloom/src/rm_alloc.h
modules/gemini-bloom/tests/**
```

## Main audit checklist

- Bloom parameter formulas: `bitsPerEntry`、`hashCount`、`totalBits`、64-bit alignment。
- hash seed 和 double hashing 是否与 DESIGN.md 一致。
- `BloomLayer` RAII、move、析构、bit addressing、overflow guard。
- `ScalingBloomFilter` layer array、placement new、move relocation、capacity/count 语义。
- `Put()` 的 duplicate、false positive、cardinality 语义。
- command parser：arity、重复 option、unknown option、NOCREATE、NONSCALING、EXPANSION 0/1/32768。
- Redis Module API：OpenKey、ModuleTypeSetValue、AutoMemory、ReplicateVerbatim、ReplyWithArray/Postponed、CreateCommand flags/key specs。
- RDB：datatype name、encver 2/4、字段顺序、hashCount/log2Bits narrowing、flags mask、Loading flag 不持久化。
- SCANDUMP/LOADCHUNK 私有协议与 DESIGN.md 是否一致。
- AOF rewrite：RDB-preamble yes/no 风险、allocation failure 语义。
- Module config：loadmodule args、默认值、范围校验。
- 内存统计：BF.INFO Size/MemUsage 与 DESIGN.md 口径。
- 测试是否真的覆盖 DESIGN.md 声明。

## Required outputs

- `agents/stage03/static_audit_by_file.md`
- `agents/stage03/invariant_map.md`
- `agents/stage03/potential_findings.md`
- `agents/stage03/design_vs_code_matrix.md`

## Pass criteria

- 每个必审文件都有审计记录。
- 每个潜在问题有证据或标记待运行验证。
- reviewer 确认没有只凭猜测定性。

## Commit message

`audit(gemini-bloom): v6 stage 03 static deep audit`

