# Stage 09 — REPLICA_CLUSTER_OPS


## Universal stage protocol

本 stage 必须遵守：

1. 重新读取 `modules/gemini-bloom/DESIGN.md`、`LOOP_CONTROL_BATCH.md`、所有 policies、`LOOP_STATE.md`、本 stage 文件。
2. 创建 `.codex/gemini-bloom-audit/v6/agents/stage09/rehydrate_log.md`。
3. 创建 planner 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage09/planner_output.md`。
4. 主 agent 审核 planner，然后执行。
5. 所有证据写入 `.codex/gemini-bloom-audit/v6/evidence/stage09/`。
6. 写 `.codex/gemini-bloom-audit/v6/agents/stage09/stage_result.md`。
7. 创建 reviewer 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage09/reviewer_output.md`。
8. reviewer 未 PASS 时必须补跑/修正/降级，然后重新 reviewer。
9. 更新 `LOOP_STATE.md`。
10. commit + push。
11. 标记 planner/reviewer closed。



## Goal

审计 replica、cluster、ACL、command metadata、key spec、操作性兼容。

## Main tasks

如果环境支持，执行：

- primary + replica，写入 Bloom 数据，验证 replica 无 false negative。
- fullsync 后验证 RDB snapshot 数据。
- replica 断开重连后验证一致性。
- 3 master 或 6 节点 Redis cluster 加载 module。
- cluster 下 BF 命令 key slot 路由、MOVED/ASK 行为。
- `COMMAND INFO` flags。
- `COMMAND GETKEYS` key extraction。
- ACL DRYRUN。
- readonly 命令在 replica 或 cluster read path 下的行为。

如果 cluster 环境构建成本过高或工具缺失，记录 BLOCKED_CLUSTER_ENV，但仍验证 COMMAND metadata。

## Required evidence

- `evidence/stage09/replica/`
- `evidence/stage09/cluster/`
- `evidence/stage09/command_metadata/`
- `evidence/stage09/acl/`
- `evidence/stage09/blocked_cluster.md` 如有

## Pass criteria

- replica/fullsync 与 DESIGN.md 兼容承诺对齐。
- cluster 不可用时有明确阻塞证据。
- command metadata 至少有 runtime evidence。

## Commit message

`audit(gemini-bloom): v6 stage 09 replica cluster ops`

