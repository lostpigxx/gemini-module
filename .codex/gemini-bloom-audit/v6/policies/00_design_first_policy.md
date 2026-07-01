# Policy 00 — DESIGN.md 优先策略

## 1. 最高标准

`modules/gemini-bloom/DESIGN.md` 是本轮审计的最高优先级标准。每个 stage 开始时必须重新读取它，并提取与本 stage 有关的约束。

## 2. 如何使用 DESIGN.md

必须区分三类内容：

1. **设计承诺**：例如 RDB/DUMP/RESTORE/MIGRATE/fullsync/RDB-preamble AOF 兼容性。如果实现或测试不满足，通常是 P1 或更高。
2. **设计内差异**：例如 RESP3 不支持、RedisBloom SCANDUMP/LOADCHUNK 不互通、command-AOF rewrite 不跨实现兼容。若实现符合这些边界，应标记 `DESIGN_INTENDED`，不能误判为 bug。
3. **设计未覆盖风险**：例如 fuzz、UB、sanitizer、资源耗尽、cluster metadata、报告证据完整性。必须继续审计，不能跳过。

## 3. 报告措辞约束

最终报告中禁止出现以下不精确结论：

- “完全兼容 RedisBloom”
- “SCANDUMP/LOADCHUNK 与 RedisBloom 兼容”
- “支持 RESP3”
- “所有 Redis 版本均兼容”
- “所有测试通过所以没有问题”

除非证据明确支持，否则只能写更窄的结论，例如：

- “在本轮环境下，RDB round-trip PASS”
- “符合 DESIGN.md 中声明的私有 SCANDUMP/LOADCHUNK 协议”
- “RedisBloom v2.4.20 对照项 X PASS；RedisBloom 其它版本 NOT_VERIFIED”

## 4. DESIGN.md 自身也要被审计

如果 DESIGN.md 的声明与源码、测试或运行结果不一致，必须记录 finding。

示例：

- DESIGN 说某路径 CI 覆盖，但仓库没有对应 CI gate。
- DESIGN 说某 corpus 存在，但路径不存在。
- DESIGN 说某行为双向验证通过，但当前证据不可复现。
- DESIGN 的已知限制没有在 README 或用户报告中清晰表达。
