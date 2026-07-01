# Policy 04 — Quality Gates

## 1. Stage gate

每个 stage 进入下一 stage 前必须满足：

- 已重新读取控制文件。
- planner_output.md 存在。
- stage_result.md 存在。
- reviewer_output.md 存在。
- reviewer verdict 为 PASS，或 stage 明确允许 BLOCKED 后继续。
- LOOP_STATE.md 已更新。
- git commit 已创建。
- commit 已 push 到远端分支。
- planner/reviewer 已标记 closed。

## 2. 报告 gate

最终报告必须满足：

- 中文。
- 每个结论有证据路径。
- 明确 DESIGN.md 设计内差异。
- 明确 DESIGN.md 未覆盖但本轮审计覆盖/未覆盖的内容。
- 明确 BLOCKED/NOT_VERIFIED。
- 不夸大兼容范围。
- 不把测试通过等同于没有 bug。

## 3. 生产代码修改 gate

本轮默认不修改生产代码。若主 agent认为必须修改生产代码才能继续审计，必须：

1. 先停止该修改。
2. 在 stage_result.md 记录原因。
3. 标记为 `REQUIRES_FIX_BRANCH`。
4. 不得把生产代码修改混入审计报告 commit。

允许新增：

- `.codex/gemini-bloom-audit/v6/**` 下的审计工具、脚本、日志、报告草稿。
- `doc/code_review/gemini-bloom/v6/**` 下的最终中文报告。

## 4. 可信度 gate

如果以下关键项 BLOCKED，最终报告可信度不能为 High：

- RedisBloom v2.4.20 oracle 对比。
- RDB/DUMP/RESTORE/MIGRATE/fullsync 至少一类迁移验证。
- 现有 GTest/TCL 测试。
- sanitizer 或内存安全替代验证。
- 报告自审。
