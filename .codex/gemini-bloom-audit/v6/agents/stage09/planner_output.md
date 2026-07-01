# Stage 09 Planner Output - REPLICA_CLUSTER_OPS

Planner scope: plan only. Do not execute tests from this planner context. Do not edit production code. The main agent should execute the selected runtime checks and write all evidence under `.codex/gemini-bloom-audit/v6/evidence/stage09/`.

## Stage Objective

Audit operational behavior for `gemini-bloom` in Redis replication and cluster deployments, plus ACL, command metadata, key extraction, and readonly paths.

The objective is not to prove drop-in RedisBloom compatibility. It is to verify that the implementation's Redis operational contracts match `modules/gemini-bloom/DESIGN.md`, especially:

- completed Bloom filters retain zero false negatives across same-module live replication, fullsync snapshot replication, and reconnect;
- cluster routing can identify the single Bloom key for every `BF.*` command;
- command flags support ACL and readonly routing expectations;
- unsupported or unavailable environment features are explicitly marked `BLOCKED` or `NOT_VERIFIED`, not silently treated as PASS.

## Rehydrated Inputs

Required files read before planning:

- `modules/gemini-bloom/DESIGN.md`
- `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`
- `.codex/gemini-bloom-audit/v6/policies/00_design_first_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/01_rehydration_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/02_subagent_protocol.md`
- `.codex/gemini-bloom-audit/v6/policies/03_evidence_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/04_quality_gates.md`
- `.codex/gemini-bloom-audit/v6/policies/05_commit_push_policy.md`
- `.codex/gemini-bloom-audit/v6/policies/06_final_report_policy.md`
- `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`
- `.codex/gemini-bloom-audit/v6/stages/STAGE_09_REPLICA_CLUSTER_OPS.md`
- `.codex/gemini-bloom-audit/v6/agents/stage08/stage_result.md`
- `.codex/gemini-bloom-audit/v6/agents/stage08/reviewer_output.md`

## Relevant DESIGN Constraints

### Design commitments to verify

- `psync / fullsync replication` is listed as compatible because the RDB snapshot path is compatible.
- Supported migration paths include primary/replica replication based on psync fullsync RDB snapshots.
- RDB serialization uses data type name `MBbloom--`, encver 2/4, and must preserve bit arrays and metadata without introducing false negatives.
- DUMP/RESTORE, MIGRATE, RDB file load, RDB-preamble AOF, and fullsync replication are the compatibility surfaces. Stage 09 should focus on replication and ops metadata, not re-run the whole Stage 06 matrix unless needed for diagnosis.
- Command flags in DESIGN:
  - write + deny-oom: `BF.RESERVE`, `BF.ADD`, `BF.MADD`, `BF.INSERT`, `BF.LOADCHUNK`
  - readonly: `BF.EXISTS`, `BF.MEXISTS`, `BF.INFO`, `BF.CARD`
  - readonly + fast: `BF.SCANDUMP`
- Command key model: each `BF.*` command operates on exactly one Redis key, the command's `key` argument. Items, cursors, option values, and chunk payloads must not be extracted as keys.
- Type safety and command-layer defenses apply in ops paths too: Redis should reject writes on replicas, wrong types should remain wrong-type errors, and OOM-denied write commands should be flagged correctly.

### Design-intended boundaries not to misclassify

- `gemini-bloom` is not a RedisBloom drop-in replacement.
- SCANDUMP/LOADCHUNK use a private gemini layer-index cursor protocol and are intentionally not RedisBloom-compatible.
- command-AOF rewrite without RDB preamble emits gemini private `BF.LOADCHUNK` commands and is intentionally not cross-implementation compatible.
- RESP3 is not supported; Stage 09 should not report RESP3 gaps as bugs.
- `BF.INFO Size` has a different accounting model from RedisBloom by design.
- live replication command stream may produce RedisBloom-vs-gemini `BF.CARD` differences in high false-positive cases such as `EXPANSION 1`; membership is the correctness anchor. Same-module gemini primary-to-replica should still be internally consistent for completed filters.

### Design-covered risks that Stage 09 must carry forward

- Loading flag is runtime-only and stripped from persistent/wire formats.
- Stage 07 already found:
  - `GBV6-07-001`: `BF.LOADCHUNK` accepts out-of-order or repeated data chunks and can complete a filter with false negatives.
  - `GBV6-07-002`: half-loaded `LOADCHUNK` keys can persist/replay as completed filters with false negatives.
- Stage 09 should include an operational LOADCHUNK-loading replication/fullsync scenario, but should classify it as carry-forward evidence unless it exposes a distinct replica/cluster behavior.

## Exact Runtime Scenarios To Test

### 0. Environment and artifact baseline

Record once under `evidence/stage09/`:

- `redis-server --version`
- `redis-cli --version`
- module artifact path, size, SHA256, and build provenance
- `git rev-parse HEAD`
- `git status --short`
- OS, shell, temporary root directory, selected ports
- whether `ACL DRYRUN`, `COMMAND GETKEYSANDFLAGS`, and cluster mode are supported by the Redis binary

If the module artifact is missing, the main agent may build it only as needed for runtime checks and must record build command/output. Planner does not run that build.

### 1. Replica live command stream

Setup:

- Start primary with `redis_bloom.so`.
- Start replica with the same module and `--replicaof 127.0.0.1 <primary_port>`.
- Wait until `INFO replication` on the replica reports `master_link_status:up` and offsets have caught up, or record timeout evidence.

Commands on primary:

- `BF.RESERVE bf:live 0.01 4 EXPANSION 2`
- `BF.MADD bf:live a b c d e f g h` to force growth beyond the initial capacity.
- `BF.RESERVE bf:fixed 0.01 2 NONSCALING`
- `BF.MADD bf:fixed x y z` and preserve the partial failure reply. Expected shape is successful inserts followed by the first full error, with no later elements processed.
- `BF.ADD bf:auto auto-1`
- binary item coverage if the harness supports raw RESP safely: add an empty string item and an item containing NUL bytes.

Assertions on replica after sync:

- `BF.MEXISTS bf:live a b c d e f g h` returns all `1`.
- `BF.MEXISTS bf:fixed x y` returns all `1`; do not require `z` to be present because the primary write should fail at the first full error.
- `BF.EXISTS bf:auto auto-1` returns `1`.
- `BF.CARD` and stable `BF.INFO` fields match primary for the same keys after offsets match. Prefer comparing known deterministic fields: item count, capacity, expansion rate, filters/layers, and not `Size` across different builds.
- Write commands issued directly to the replica, including `BF.ADD`, `BF.RESERVE`, `BF.MADD`, `BF.INSERT`, and `BF.LOADCHUNK`, are rejected by Redis replica semantics. Record actual error strings; do not require a specific string unless Redis version makes it stable.
- Readonly commands on the replica, including `BF.EXISTS`, `BF.MEXISTS`, `BF.INFO`, `BF.CARD`, and `BF.SCANDUMP`, succeed for completed keys.

Classification:

- Any inserted item returning `0` on the replica is FAIL/P1 unless the key is intentionally a known corrupt LOADCHUNK scenario.
- Replication lag without eventual catch-up is BLOCKED or FAIL depending on logs: environment/network timeout is BLOCKED; module/server crash or persistent inconsistency is FAIL.

### 2. Replica fullsync snapshot

Setup:

- Start primary with module, no replica attached.
- Create a dataset before attaching the replica.

Dataset:

- `bf:fs:single`: reserve and insert several normal items.
- `bf:fs:multi`: reserve small capacity and insert enough unique items to create multiple layers.
- `bf:fs:fixed`: non-scaling filter with successful inserts up to capacity.
- `bf:fs:ttl`: Bloom key with an expiry, if timing can be controlled without flakiness.
- Optional binary item key via raw RESP if available.

Attach replica after dataset creation:

- Start replica or issue `REPLICAOF`.
- Wait for initial synchronization.
- Capture primary and replica `INFO replication`, Redis logs, and `DBSIZE`/key list snapshots.

Assertions:

- All inserted items from every completed key return `1` on the replica.
- `BF.INFO`/`BF.CARD` match primary for deterministic fields.
- TTL key exists and has a plausible `PTTL` on the replica if TTL was tested; do not fail for small timing deltas.
- Server logs should show fullsync or initial synchronization. If logs do not prove fullsync but data is correct, mark the fullsync-mechanism proof `NOT_VERIFIED` while preserving data-result evidence.

### 3. Replica disconnect and reconnect

Run two reconnect shapes if feasible:

Short reconnect / likely partial resync:

- Start primary and replica, wait for sync.
- Create `bf:reconn` and insert `before-1 before-2`.
- Break the replication link by `CLIENT KILL TYPE replica` on the primary or an equivalent controlled link drop.
- Insert `during-1 during-2 during-3` on the primary while the link is down or immediately after kill.
- Wait for automatic reconnect.
- Verify `before-*` and `during-*` all return `1` on the replica.
- Inspect logs for partial resync evidence. If logs only show full resync, classify data reconnect PASS but partial-resync-specific coverage `NOT_VERIFIED`.

Forced reconnect/full resync:

- Use `REPLICAOF NO ONE` on the replica, write additional items on primary, then `REPLICAOF 127.0.0.1 <primary_port>`.
- Wait for sync and verify all inserted items return `1`.
- Record whether Redis performed fullsync or partial resync from logs/INFO.

Assertions:

- No completed Bloom key may lose membership after reconnect.
- Replica write rejection and readonly command success should still hold after reconnect.

### 4. LOADCHUNK loading state across replication/fullsync

Purpose: operationally connect Stage 07 findings to replica/fullsync without confusing them with normal completed-filter replication.

Scenario:

- On primary, create a valid source key and use `BF.SCANDUMP source 0` to obtain a valid gemini header.
- Create `bf:loading:partial` with only `BF.LOADCHUNK bf:loading:partial 1 <header>`.
- Before completing data chunks, verify on primary that normal `BF.EXISTS`, `BF.INFO`, `BF.CARD`, `BF.SCANDUMP`, and writes return `ERR filter is being loaded`.
- Attach or reconnect a replica to force snapshot transfer of this half-loaded key.
- On replica, check whether `bf:loading:partial` is absent, loading-protected, or incorrectly readable as a completed filter.

Expected design-safe behavior is ambiguous because DESIGN says the Loading flag is runtime-only and does not persist, while Stage 07 found data-integrity failures. Classification rule:

- If a half-loaded key becomes a readable completed filter with false negatives on replica/fullsync, classify as FAIL tied to `GBV6-07-002` and record Stage 09 operational impact.
- If Redis refuses to persist/replicate the loading key or it remains protected, classify this scenario PASS with evidence.
- If raw header extraction is not possible in the runtime harness, mark this specific scenario `NOT_VERIFIED` and reference Stage 07 evidence.

### 5. COMMAND metadata

For each command, capture raw `COMMAND INFO`, normalized flags, arity, first/last/step or key-spec fields, and optional `COMMAND DOCS`/`COMMAND GETKEYSANDFLAGS` if supported.

Expected flag matrix:

| Command | Expected operational flags |
|---|---|
| `BF.RESERVE` | `write`, `denyoom` |
| `BF.ADD` | `write`, `denyoom` |
| `BF.MADD` | `write`, `denyoom` |
| `BF.INSERT` | `write`, `denyoom` |
| `BF.LOADCHUNK` | `write`, `denyoom` |
| `BF.EXISTS` | `readonly` |
| `BF.MEXISTS` | `readonly` |
| `BF.INFO` | `readonly` |
| `BF.CARD` | `readonly` |
| `BF.SCANDUMP` | `readonly`, `fast` |

Do not require `fast` for `BF.INFO` or `BF.CARD`; DESIGN explicitly notes they are `readonly`, not RedisBloom's `readonly fast`.

Expected `COMMAND GETKEYS` cases:

- `BF.RESERVE key 0.01 100` -> `key`
- `BF.ADD key item` -> `key`
- `BF.MADD key item1 item2` -> `key`
- `BF.INSERT key CAPACITY 10 ERROR 0.01 ITEMS item1 item2` -> `key`
- `BF.INSERT key NOCREATE ITEMS item1` -> `key`
- `BF.EXISTS key item` -> `key`
- `BF.MEXISTS key item1 item2` -> `key`
- `BF.INFO key` -> `key`
- `BF.INFO key ITEMS` -> `key`
- `BF.CARD key` -> `key`
- `BF.SCANDUMP key 0` -> `key`
- `BF.LOADCHUNK key 1 payload` -> `key`

Assertions:

- Every supported `BF.*` command extracts exactly one key.
- No item, cursor, field name, option, or payload is extracted as a key.
- Invalid arity behavior should be recorded but is not a key-extraction FAIL unless valid commands extract incorrectly.

### 6. ACL behavior

First detect support:

- Run a harmless `ACL DRYRUN` probe and record whether Redis supports it. LOOP_STATE already carries `GBV6-04-BLOCK-001`: Redis 6.2.16 did not expose `ACL DRYRUN`.

If `ACL DRYRUN` is supported:

- Create/define a read-only ACL test user with key pattern `~bf:*`, read categories or explicit read command grants, and no write permission.
- Dry-run read commands:
  - `BF.EXISTS bf:acl item`
  - `BF.MEXISTS bf:acl item1 item2`
  - `BF.INFO bf:acl`
  - `BF.CARD bf:acl`
  - `BF.SCANDUMP bf:acl 0`
- Dry-run write commands:
  - `BF.RESERVE bf:acl 0.01 100`
  - `BF.ADD bf:acl item`
  - `BF.MADD bf:acl item1 item2`
  - `BF.INSERT bf:acl ITEMS item1`
  - `BF.LOADCHUNK bf:acl 1 payload`
- Dry-run key-pattern denial with a key outside `bf:*`.

If `ACL DRYRUN` is not supported:

- Write `evidence/stage09/acl/blocked_acl.md` explaining feature absence with Redis version and raw error.
- Optionally run an actual temporary ACL-user smoke test if safe, but classify it as partial ACL runtime evidence, not DRYRUN PASS.

Assertions:

- Readonly commands should be allow-listable independently from write commands.
- Write commands should not pass under read-only/category-only users.
- Key pattern restrictions should apply to the extracted Bloom key.
- If Redis version lacks DRYRUN, classify DRYRUN as `BLOCKED_ACL_DRYRUN`, not FAIL.

### 7. Cluster module loading and routing

If Redis cluster support is available and setup cost is acceptable, run a 3-master or 6-node cluster. Prefer 6 nodes with replicas if readonly replica path is in scope:

- Start each node with `cluster-enabled yes`, unique dirs, unique `cluster-config-file`, and `--loadmodule <redis_bloom.so>`.
- Create cluster via `redis-cli --cluster create ... --cluster-replicas 1 --cluster-yes` for 6 nodes, or `--cluster-replicas 0` for 3 masters.
- Capture `CLUSTER INFO`, `CLUSTER NODES`, `MODULE LIST` on every node, and slot ownership.

Cluster command scenarios:

- Select tagged keys for deterministic slots, for example `{gb09}:live`, `{gb09}:fixed`, and `{gb09}:scan`.
- On the correct slot owner:
  - run write and read Bloom commands;
  - verify known inserted items return `1`;
  - verify `BF.SCANDUMP`/`BF.LOADCHUNK` same-cluster round-trip for a completed key if safe.
- On a non-owner without `redis-cli -c`:
  - each valid `BF.*` command should return `MOVED` for the key slot, or an equivalent cluster routing error before module command execution.
- With `redis-cli -c`:
  - valid commands should redirect to the owner and succeed.

ASK behavior:

- Attempt deterministic ASK coverage by placing a slot into `MIGRATING` on the source and `IMPORTING` on the destination, or by running a controlled reshard while issuing repeated `BF.EXISTS`/`BF.ADD` commands for the migrating slot.
- Record raw `ASK` replies if captured.
- If ASK cannot be deterministically produced, write `evidence/stage09/cluster/ask_not_verified.md` and classify only ASK coverage as `NOT_VERIFIED`; do not fail the cluster stage solely for missing ASK if MOVED and normal cluster routing are verified.

Cluster READONLY replica path:

- For a key whose slot belongs to a master with a replica, connect directly to the replica.
- Before `READONLY`, record the response for `BF.EXISTS` and `BF.INFO` on the key. Redis may return `MOVED`; record exact behavior.
- Issue `READONLY`.
- Verify `BF.EXISTS`, `BF.MEXISTS`, `BF.INFO`, `BF.CARD`, and `BF.SCANDUMP` can be served from the replica for local-master slots after replication catches up.
- Verify write commands on the cluster replica do not succeed. Depending on Redis version and route, the error may be `MOVED`, `READONLY`, or another cluster write rejection; record exact strings and classify by whether the write was prevented.

If cluster setup is unavailable:

- Write `evidence/stage09/blocked_cluster.md` with exact blocker: missing `redis-cli --cluster`, cluster-disabled binary, port bind failure, module load failure, or timeout.
- Still run COMMAND metadata and non-cluster replica checks if possible.

## Required Evidence Files

Root stage evidence:

- `.codex/gemini-bloom-audit/v6/evidence/stage09/commands.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/stdout.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/stderr.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/exit_codes.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/env_snapshot.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/evidence_index.md`

Replica evidence:

- `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/setup.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/commands.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/primary.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/replica.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/info_replication_before_after.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/live_command_stream.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/fullsync_snapshot.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/reconnect.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/readonly_on_replica.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/loadchunk_loading_replication.log` if attempted
- `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/result_matrix.md`

Cluster evidence:

- `.codex/gemini-bloom-audit/v6/evidence/stage09/cluster/setup.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/cluster/cluster_create.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/cluster/cluster_info.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/cluster/cluster_nodes.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/cluster/module_list_all_nodes.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/cluster/moved_routing.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/cluster/redirect_with_c_flag.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/cluster/ask_behavior.log` or `ask_not_verified.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/cluster/readonly_replica.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/cluster/result_matrix.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/blocked_cluster.md` if cluster cannot be constructed

Command metadata evidence:

- `.codex/gemini-bloom-audit/v6/evidence/stage09/command_metadata/command_info_raw.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/command_metadata/command_info_normalized.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/command_metadata/command_getkeys_raw.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/command_metadata/command_getkeys_matrix.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/command_metadata/command_getkeysandflags.log` if supported

ACL evidence:

- `.codex/gemini-bloom-audit/v6/evidence/stage09/acl/acl_support_probe.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage09/acl/acl_dryrun_matrix.md` if supported
- `.codex/gemini-bloom-audit/v6/evidence/stage09/acl/acl_actual_user_smoke.log` if fallback is run
- `.codex/gemini-bloom-audit/v6/evidence/stage09/acl/blocked_acl.md` if DRYRUN is unavailable

## Classification Rules

Use only these classifications in Stage 09 result tables:

- `PASS`: scenario executed or statically/runtime-verified with evidence and matched DESIGN.
- `FAIL`: verified behavior violates DESIGN or Redis operational requirements, with a reproduction and logs.
- `BLOCKED`: environment, Redis feature, dependency, permission, port, or tool failure prevents verification. Must include blocker evidence.
- `NOT_VERIFIED`: scenario was not covered or could not be conclusively distinguished even though the broader environment ran. Must be explicit in final report confidence.
- `DESIGN_INTENDED`: behavior differs from RedisBloom or generic expectations but is explicitly within DESIGN boundaries.

Specific downgrade rules:

- `ACL DRYRUN` missing on Redis 6.2.x is `BLOCKED_ACL_DRYRUN`, not FAIL.
- Cluster unavailable because the local Redis binary cannot create clusters is `BLOCKED_CLUSTER_ENV`, not FAIL.
- ASK not captured after MOVED routing is verified is `NOT_VERIFIED_ASK`, not full cluster FAIL.
- Reconnect data correct but logs cannot distinguish partial resync from full resync is `PASS_RECONNECT_DATA` plus `NOT_VERIFIED_PARTIAL_PSYNC`.
- Binary item coverage not attempted because the harness cannot safely emit raw RESP is `NOT_VERIFIED_BINARY_ITEM_OPS`.
- Known half-loaded LOADCHUNK false-negative behavior should reference `GBV6-07-002`; open a new Stage 09 finding only if replication/cluster introduces a distinct impact or a new reproduction surface.

## Risks and False PASS Cases

- Checking only `INFO replication` or offsets is not enough; membership must be checked on the replica for inserted items.
- Testing a replica that was attached before data creation does not prove fullsync snapshot transfer. Fullsync needs data created before replica attach or clear log evidence.
- Using only one small single-layer key can miss multi-layer serialization and expansion metadata issues.
- Asserting absent items return `0` can create false failures because Bloom filters may have false positives. Stage 09 should assert only no false negatives for known inserted items.
- `redis-cli -c` masks MOVED replies. MOVED must be tested without auto-redirection.
- Cluster tests that only target the slot owner do not verify key extraction or routing.
- READONLY cluster behavior must be tested by connecting to a replica, not by reading from the master.
- ACL category behavior depends on command flags; do not infer it from COMMAND INFO alone if ACL runtime is available.
- Treating Stage 04 metadata evidence as current Stage 09 evidence would violate evidence policy; Stage 09 needs fresh runtime evidence.
- The existing Stage 07 LOADCHUNK findings can make normal replication look bad if the dataset includes half-loaded keys. Keep completed-filter replication and half-loaded LOADCHUNK tests separate.

## Risks and False FAIL Cases

- Replication lag can look like data loss. Always wait for link up and offset catch-up or record timeout separately.
- Redis version differences affect error strings. Classify by semantics unless DESIGN requires exact text.
- Redis 6.2 lacking `ACL DRYRUN` is a feature blocker, not a module ACL failure.
- `COMMAND GETKEYSANDFLAGS` may be unavailable on older Redis versions; `COMMAND GETKEYS` remains the required baseline.
- Cluster ASK is timing-sensitive and may be hard to capture deterministically. Missing ASK evidence alone should downgrade ASK coverage, not fail MOVED/key-slot routing.
- Writes issued to a cluster replica may return `MOVED` rather than `READONLY`; either is acceptable if the write is not executed and data remains unchanged.
- A RedisBloom-vs-gemini `BF.CARD` difference in cross-implementation live command streams is DESIGN-known and not a Stage 09 same-module replica failure unless same-module gemini diverges.
- Module load conflict with RedisBloom or Redis 8 built-in Bloom is DESIGN-known mutual exclusion. If such a conflict is encountered, classify as `DESIGN_INTENDED` with environment notes, not a gemini cluster-load bug.

## Pass Criteria

Stage 09 can be accepted as PASS only if all non-blocked mandatory areas meet the criteria below:

- Replica live command stream: completed Bloom keys written on primary have no false negatives on replica; write commands are rejected on replica; readonly commands work on completed keys.
- Fullsync snapshot: completed Bloom keys created before replica attach have no false negatives after initial sync, with log or INFO evidence tying the scenario to fullsync. If fullsync mechanism proof is missing, data result may pass but fullsync proof must be downgraded.
- Reconnect: completed Bloom keys retain membership after link break/reconnect. Partial-vs-full resync must be classified according to available log evidence.
- LOADCHUNK loading ops: attempted and classified, or explicitly `NOT_VERIFIED` with Stage 07 carry-forward references.
- Cluster: module loads on all cluster nodes and normal owner/MOVED/redirect behavior passes, or cluster is `BLOCKED_CLUSTER_ENV` with exact evidence. ASK may be separately `NOT_VERIFIED` if not captured.
- Cluster readonly path: readonly `BF.*` commands are served from a cluster replica after `READONLY` when cluster replicas are available, or this is separately blocked/not verified.
- COMMAND metadata: runtime `COMMAND INFO` flags match DESIGN for all ten commands.
- Key extraction: runtime `COMMAND GETKEYS` returns exactly one key for all valid `BF.*` command shapes listed above.
- ACL: `ACL DRYRUN` read/write/key-pattern behavior passes if supported; if not supported, `BLOCKED_ACL_DRYRUN` evidence exists and the final confidence is degraded accordingly.
- Evidence policy: every conclusion in `stage_result.md` references a concrete file under `.codex/gemini-bloom-audit/v6/evidence/stage09/`.

Stage 09 should be `BLOCKED`, not PASS, if replica runtime cannot be started at all, command metadata cannot be obtained, or cluster/ACL blockers consume major required areas without alternative evidence. Because Stage 09 is continuable, a blocked result may still proceed if it is fully evidenced and LOOP_STATE/final-report confidence are updated.
