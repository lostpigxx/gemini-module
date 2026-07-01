# Stage 01 — ENV_REPO_BASELINE


## Universal stage protocol

本 stage 必须遵守：

1. 重新读取 `modules/gemini-bloom/DESIGN.md`、`LOOP_CONTROL_BATCH.md`、所有 policies、`LOOP_STATE.md`、本 stage 文件。
2. 创建 `.codex/gemini-bloom-audit/v6/agents/stage01/rehydrate_log.md`。
3. 创建 planner 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage01/planner_output.md`。
4. 主 agent 审核 planner，然后执行。
5. 所有证据写入 `.codex/gemini-bloom-audit/v6/evidence/stage01/`。
6. 写 `.codex/gemini-bloom-audit/v6/agents/stage01/stage_result.md`。
7. 创建 reviewer 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage01/reviewer_output.md`。
8. reviewer 未 PASS 时必须补跑/修正/降级，然后重新 reviewer。
9. 更新 `LOOP_STATE.md`。
10. commit + push。
11. 标记 planner/reviewer closed。



## Goal

建立仓库、分支、依赖、运行环境基线，确保后续结果可复现。

## Required evidence

- `evidence/stage01/git_snapshot.txt`
- `evidence/stage01/env_snapshot.txt`
- `evidence/stage01/tool_versions.txt`
- `evidence/stage01/repo_tree_gemini_bloom.txt`
- `evidence/stage01/dependency_status.txt`

## Main tasks

运行并记录：

```bash
git rev-parse HEAD
git branch --show-current
git status --short
git remote -v
uname -a || true
cat /etc/os-release || true
cmake --version || true
c++ --version || true
g++ --version || true
clang++ --version || true
redis-server --version || true
redis-cli --version || true
tclsh <<< 'puts [info patchlevel]' || true
python3 --version || true
docker --version || true
find modules/gemini-bloom -maxdepth 4 -type f | sort
```

分支要求：

```bash
git checkout -B audit/gemini-bloom-v6
git push -u origin audit/gemini-bloom-v6
```

如果 push 失败，本 stage 必须 BLOCKED，且不得进入 Stage 02。

## Pass criteria

- 当前 commit、branch、dirty status、工具版本已落盘。
- 目标分支存在并已 push。
- reviewer 确认证据足够复现。

## Commit message

`audit(gemini-bloom): v6 stage 01 env repo baseline`

