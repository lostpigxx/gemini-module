# gemini-bloom v6 Codex 审计 Loop Control Batch

本目录是给 Codex App Goal Mode 使用的文件驱动审计控制包。它不安装任何依赖，不修改生产代码，不替代审计结果；它只定义 Codex 应该如何完成一轮可复现、可追责、可反审计的 gemini-bloom 全面审计。

## 核心目标

在 `lostpigxx/gemini-module` 仓库的 `main` 分支基础上，审计 `modules/gemini-bloom` 的：

- `modules/gemini-bloom/DESIGN.md` 中明确写出的设计约束、兼容性边界、已知限制是否被代码和测试真实满足。
- `DESIGN.md` 没有覆盖但工程审计必须覆盖的风险：内存安全、UB、资源耗尽、持久化损坏、复制/集群行为、恶意输入、测试漏洞、报告证据完整性。
- 静态源码、运行时行为、RedisBloom v2.4.20 对比、RDB/DUMP/RESTORE/MIGRATE/fullsync/RDB-preamble AOF、私有 SCANDUMP/LOADCHUNK、sanitizer、fuzz、压力、操作性场景。

## 目录规则

- 给 agent 看的控制文件、流程文件、计划、每个 stage 的 agent 输出、原始证据、运行日志，全部放在：
  - `.codex/gemini-bloom-audit/v6/`
- 面向人的最终中文报告，全部放在：
  - `doc/code_review/gemini-bloom/v6/`
- 不允许把中间 agent 长文本输出散落到仓库其它位置。
- 不允许把运行证据只留在 Codex 对话上下文里。

## 入口文件

在 Codex App Goal Mode 中粘贴：

- `.codex/gemini-bloom-audit/v6/CODEX_APP_START_PROMPT.md`

Codex 启动后必须先读：

1. `modules/gemini-bloom/DESIGN.md`
2. `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`
3. `.codex/gemini-bloom-audit/v6/policies/*.md`
4. `.codex/gemini-bloom-audit/v6/stages/STAGE_00_DESIGN_CONTRACT.md`
5. `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`

每个 stage 开始时都必须重新读取以上控制文件和当前 stage 文件，不能依赖上一个 stage 的对话上下文。
