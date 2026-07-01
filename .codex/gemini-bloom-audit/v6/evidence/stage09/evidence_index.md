# Stage 09 Evidence Index

| Conclusion | Evidence |
|---|---|
| Environment and artifact baseline | `env_snapshot.txt`, `build/` |
| Replica live command stream | `replica/live_command_stream.log`, `replica/primary.log`, `replica/replica.log` |
| Replica fullsync snapshot | `replica/fullsync_snapshot.log`, `replica/fullsync_primary.log`, `replica/fullsync_replica.log` |
| Replica reconnect | `replica/reconnect.log`, `replica/reconnect_primary.log`, `replica/reconnect_replica.log` |
| Loading partial fullsync impact | `replica/loading_partial_fullsync.log`, `replica/loading_primary.log`, `replica/loading_replica.log` |
| Command metadata and key extraction | `command_metadata/command_info.json`, `command_metadata/getkeys.json`, `command_metadata/summary.md` |
| ACL behavior | `acl/acl_results.json`, `acl/summary.md`, `acl/blocked_acl.md` if present |
| Cluster module/routing/readonly | `cluster/cluster_results.json`, `cluster/summary.md`, `cluster/node_*.log`, `cluster/ask_not_verified.md` |
