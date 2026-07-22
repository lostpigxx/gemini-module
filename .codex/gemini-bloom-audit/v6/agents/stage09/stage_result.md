# Stage 09 Result — REPLICA_CLUSTER_OPS

Status: `PASS`

## Summary

Stage 09 completed operational runtime checks for replica, cluster, command metadata, and ACL behavior. Completed Bloom filters preserved membership across live replication, fullsync snapshot, and reconnect. A 6-node Redis cluster loaded the module on all nodes and passed owner execution, MOVED routing, `redis-cli -c` redirect, same-slot SCANDUMP/LOADCHUNK round-trip, and cluster replica `READONLY` read behavior.

The stage also reproduced the Stage 07 half-loaded `LOADCHUNK` persistence defect through a fullsync operational path: the primary protects a loading key, but after fullsync the replica exposes it as a readable completed filter with false negatives. This is additional evidence for `GBV6-07-002`, not a separate new finding.

## Classifications

| Area | Status | Evidence |
|---|---|---|
| Module build for runtime | `PASS_WITH_WORKAROUND` | `evidence/stage09/build/artifact_info.txt` |
| Replica live command stream | `PASS` | `evidence/stage09/replica/live_command_stream.log` |
| Replica fullsync snapshot | `PASS` | `evidence/stage09/replica/fullsync_snapshot.log` |
| Replica reconnect | `PASS` | `evidence/stage09/replica/reconnect.log` |
| Half-loaded LOADCHUNK fullsync | `FAIL_CARRY_FORWARD_GBV6-07-002` | `evidence/stage09/replica/loading_partial_fullsync.log` |
| COMMAND INFO flags | `PASS` | `evidence/stage09/command_metadata/command_info.json` |
| COMMAND GETKEYS extraction | `PASS` | `evidence/stage09/command_metadata/getkeys.json` |
| ACL actual user smoke | `PASS` | `evidence/stage09/acl/acl_results.json` |
| ACL DRYRUN | `BLOCKED_ACL_DRYRUN` | `evidence/stage09/acl/blocked_acl.md` |
| Cluster owner/MOVED/redirect/round-trip | `PASS` | `evidence/stage09/cluster/cluster_results.json` |
| Cluster READONLY replica read path | `PASS` | `evidence/stage09/cluster/cluster_results.json` |
| Cluster ASK | `NOT_VERIFIED` | `evidence/stage09/cluster/ask_not_verified.md` |

## Findings

No new Stage 09 finding ID is opened.

Carry-forward evidence:

- `GBV6-07-002` remains P1 OPEN. Stage 09 shows operational fullsync impact for the same defect.
- `GBV6-04-BLOCK-001` remains a blocker for ACL DRYRUN verification in Redis 6.2.x. Stage 09 adds Redis 6.2.17 evidence and actual ACL smoke coverage.

## Evidence

See `.codex/gemini-bloom-audit/v6/evidence/stage09/evidence_index.md`.

## Confidence Impact

Operational confidence for completed filters is improved: same-module replication, cluster routing, readonly metadata, and actual ACL behavior passed in Redis 6.2.17. Final report must still downgrade:

- half-loaded `LOADCHUNK` persistence/fullsync due `GBV6-07-002`;
- ACL DRYRUN because Redis 6.2.x lacks the subcommand;
- ASK routing because it was not deterministically exercised;
- default Linux/GCC build because Stage 09 runtime used the Stage 05 `-include climits` workaround.

## Agent Closure Note

Planner closed: yes.
Reviewer closed: yes.
Next stage may only use persisted files, not live subagent context.
