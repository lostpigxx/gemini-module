# Stage 09 Main Execution

Reviewed planner output and adopted its runtime-first plan with one implementation detail: the Stage 09 module was built from current source using the known Stage 05 `-include climits` workaround because the default Linux/GCC build is already tracked as `GBV6-05-001`.

Executed:

1. Captured Redis/module/git environment and feature probes.
2. Built `/tmp/gemini-module-v6-stage09-build-workaround/redis_bloom.so`.
3. Ran completed-filter replica live command stream checks.
4. Ran replica fullsync snapshot checks with single-layer, multi-layer, fixed, and TTL keys.
5. Ran reconnect checks covering link kill and forced `REPLICAOF` reset.
6. Ran half-loaded `LOADCHUNK` fullsync scenario to connect Stage 07 to operational replication impact.
7. Verified `COMMAND INFO` flags and `COMMAND GETKEYS` extraction for all BF commands.
8. Verified actual ACL user behavior with readonly command grants, write denial, and key-pattern denial.
9. Ran a 6-node Redis cluster with the module loaded on every node, checking slot owner execution, MOVED routing, `redis-cli -c` redirect, same-slot SCANDUMP/LOADCHUNK round-trip, and cluster replica `READONLY` read path.

Planner adjustment:

- `ASK` was not deterministically produced. It is recorded as `NOT_VERIFIED` in `cluster/ask_not_verified.md`; MOVED and normal cluster redirect behavior were verified.
- `ACL DRYRUN` is unavailable on Redis 6.2.17. Actual ACL smoke coverage was added and passed, while DRYRUN remains blocked.

Result:

- Stage status proposed: `PASS` for audit completion with blocked/not-verified subcoverage recorded.
- No new finding ID is opened.
- `GBV6-07-002` receives additional Stage 09 operational evidence: a half-loaded key replicated by fullsync becomes readable as a completed filter with false negatives.
- `GBV6-04-BLOCK-001` remains applicable: ACL DRYRUN is unavailable in the Redis 6.2.x audit environment.

Planner closed: yes.
Reviewer closed: yes.
Next stage may only use persisted files, not live subagent context.
