# Policy 05 — Commit / Push 策略

## 1. 分支

目标分支：

```bash
 audit/gemini-bloom-v6
```

Stage 00 开始前，如果当前分支不是该分支，主 agent 应执行：

```bash
git checkout -B audit/gemini-bloom-v6
```

如果远端已有该分支，应优先同步并避免覆盖别人提交：

```bash
git fetch origin audit/gemini-bloom-v6 || true
```

## 2. 每个 stage 一个 commit

commit message 格式：

```text
audit(gemini-bloom): v6 stage XX <short-name>
```

示例：

```text
audit(gemini-bloom): v6 stage 00 design contract
```

## 3. Commit 范围

默认只允许提交：

- `.codex/gemini-bloom-audit/v6/**`
- `doc/code_review/gemini-bloom/v6/**`

禁止提交：

- 未经用户明确要求的 `modules/gemini-bloom/src/**` 修改。
- 删除或弱化现有测试。
- 临时 build 目录。
- 大型二进制产物。

## 4. Push gate

每个 stage commit 后必须执行：

```bash
git push -u origin audit/gemini-bloom-v6
```

如果 push 失败：

1. 记录 `.codex/gemini-bloom-audit/v6/evidence/stageXX/push_failure.log`。
2. 更新 LOOP_STATE.md 为 `BLOCKED_PUSH`。
3. 停止进入下一 stage。

## 5. Dirty tree gate

进入下一 stage 前必须记录：

```bash
git status --short
```

并确保没有未解释的 dirty files。允许存在需要下一 stage 继续写入的 `.codex` 状态文件，但必须说明。
