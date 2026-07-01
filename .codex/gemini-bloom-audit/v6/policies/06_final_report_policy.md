# Policy 06 — 最终中文报告策略

## 1. 最终报告路径

所有人类可读最终报告放在：

```text
doc/code_review/gemini-bloom/v6/
```

## 2. 必须生成的文件

Stage 11 必须生成以下中文文件：

```text
doc/code_review/gemini-bloom/v6/
  00_审计总览.md
  01_DESIGN约束与结论对齐.md
  02_源码实现审计.md
  03_运行时测试结果.md
  04_RedisBloom兼容性矩阵.md
  05_持久化迁移复制审计.md
  06_安全与资源边界.md
  07_问题清单与复现.md
  08_测试覆盖与未覆盖.md
  09_最终结论与修复优先级.md
  10_报告自审结果.md
  evidence_index.md
```

## 3. 报告内容约束

报告必须：

- 先写 DESIGN.md 的结论边界，再写 RedisBloom 对比。
- 区分 `DESIGN_INTENDED`、`FAIL`、`BLOCKED`、`NOT_VERIFIED`。
- 每个 finding 给出：严重级别、影响、相关文件/函数、复现命令、expected、actual、证据路径、建议修复方向。
- 对每个运行测试写清楚环境、命令、结果、证据路径。
- 对没有跑通的测试写清楚为什么没有跑通，以及可信度影响。

## 4. 报告自审

Stage 12 必须审计 Stage 11 的最终报告，并更新：

```text
doc/code_review/gemini-bloom/v6/10_报告自审结果.md
```

自审必须检查：

- 每个结论是否有证据路径。
- 是否误判 DESIGN_INTENDED 为 bug。
- 是否把未运行项写成已验证。
- 是否遗漏 BF 命令、RDB/AOF/replication/cluster/fuzz/sanitizer/perf。
- severity 是否合理。
- 最终可信度评级是否匹配证据覆盖。
