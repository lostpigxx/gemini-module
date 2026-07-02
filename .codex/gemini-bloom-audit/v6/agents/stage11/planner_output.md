# Stage 11 Planner Output

## Stage 11 objective

Stage 11 must synthesize the persisted Stage 00-10 audit record into the final human-readable Chinese report under `doc/code_review/gemini-bloom/v6/`.

This planner is plan-only. Stage 11 execution must not run new product tests, must not edit production code, and must not weaken or delete existing tests. The main agent should only read persisted audit inputs, produce Stage 11 evidence, write the required Chinese report files, write `stage_result.md`, then hand the report to the Stage 11 reviewer.

The final report must be a synthesis of evidence already collected in Stages 00-10 plus any Stage 11 report-synthesis evidence files. It must not silently upgrade any `BLOCKED`, `NOT_VERIFIED`, `DESIGN_INTENDED`, or carry-forward `OPEN` finding into `PASS`.

## DESIGN-first report rules

`modules/gemini-bloom/DESIGN.md` remains the highest-priority design standard for final-report wording.

Report rules the main agent must enforce:

- Lead with the DESIGN compatibility boundary before discussing RedisBloom comparison.
- Do not write "完全兼容 RedisBloom" or equivalent drop-in language.
- Limit RedisBloom compatibility claims to the exact evidence scope: Redis 6.2.17 + RedisBloom v2.4.20, tested corpora and paths, and only DESIGN-promised RDB-family transport paths.
- Treat these as `DESIGN_INTENDED`, not bugs: RESP3 unsupported, BF.DEBUG unsupported, RedisBloom SCANDUMP/LOADCHUNK non-interoperability, command-AOF cross-implementation non-compatibility when `aof-use-rdb-preamble no`, `BF.INFO FIELD` scalar shape, `BF.INFO Size` accounting differences, stricter parser behavior, module name/version differences, and live command-stream `BF.CARD` drift with no inserted-item false negatives.
- Separate DESIGN-intended private-protocol boundaries from real gemini self-protocol defects. `GBV6-07-001` and `GBV6-07-002` are not RedisBloom-interop bugs; they are gemini LOADCHUNK data-integrity bugs.
- Preserve every `BLOCKED` and `NOT_VERIFIED` item in the final report and confidence rating.
- Every substantive conclusion must cite concrete `.codex/gemini-bloom-audit/v6/evidence/...` evidence paths.
- The final confidence must be downgraded because Stage 08 sanitizer runtime coverage is `BLOCKED`, Stage 07 found P1 LOADCHUNK data-integrity failures, Stage 09 confirms fullsync impact for one P1, ACL DRYRUN remains blocked, cluster ASK remains `NOT_VERIFIED`, and max-capacity default/low-error allocation remains `NOT_VERIFIED`.
- The final report may state that DESIGN-promised RDB/DUMP/RESTORE/MIGRATE/fullsync/RDB-preamble AOF paths passed in the exact Stage 05/06 oracle scope, but must not generalize to untested RedisBloom versions, Redis 8 built-in Bloom, RESP3, RedisBloom SCANDUMP/LOADCHUNK, or command-AOF cross replay.
- `Policy 06` is stricter than the Stage 11 stage file: it additionally requires `doc/code_review/gemini-bloom/v6/10_报告自审结果.md`. Stage 11 should create a placeholder/pre-audit self-review handoff file for Stage 12 to update, not omit it.

## Inputs to summarize

The main agent should re-read and summarize these exact inputs into Stage 11 synthesis evidence:

| Input group | Required files | How Stage 11 should use them |
|---|---|---|
| Design and controls | `modules/gemini-bloom/DESIGN.md`; `.codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`; all `.codex/gemini-bloom-audit/v6/policies/*.md`; `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`; `.codex/gemini-bloom-audit/v6/stages/STAGE_11_FINAL_REPORT_SYNTHESIS.md` | Establish report scope, file requirements, severity/status definitions, and confidence downgrade rules. |
| Stage outputs | Stage 00-10 `planner_output.md`, `stage_result.md`, `reviewer_output.md` | Use `stage_result.md` and reviewer outputs as authoritative stage conclusions; use planner outputs for expected coverage and risk intent. |
| Evidence indexes | `.codex/gemini-bloom-audit/v6/evidence/stage00/evidence_index.md` through `stage10/evidence_index.md` | Build the final `doc/.../evidence_index.md` and verify every report conclusion has a backing path. |
| Findings | `.codex/gemini-bloom-audit/v6/agents/stage00/findings.md`; `.codex/gemini-bloom-audit/v6/agents/stage02/findings.md`; `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md`; `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md`; `LOOP_STATE.md` global findings table | Build `07_问题清单与复现.md`, global severity summary, and repair-priority section. |
| Compatibility and transport matrices | Stage 05 `compatibility_matrix.md`; Stage 06 `transport_matrix.md`; Stage 04 runtime matrix summary/failures; Stage 09 replica/cluster/ACL evidence; Stage 10 perf/resource matrix | Populate command behavior, RedisBloom compatibility, persistence, replica/cluster, and resource chapters. |
| Blocker and NOT_VERIFIED evidence | Stage 04 `runtime_matrix/failures.md`; Stage 08 `blocked_sanitizer.md`; Stage 09 `acl/blocked_acl.md`, `cluster/ask_not_verified.md`; Stage 10 `blocked_or_not_verified.md`; Stage 07 fault matrix | Populate `08_测试覆盖与未覆盖.md` and final confidence caveats. |

## Required report files and proposed contents

Stage 11 execution should create or update these files under `doc/code_review/gemini-bloom/v6/`.

| Report file | Proposed contents |
|---|---|
| `00_审计总览.md` | Chinese executive summary; audited branch/commit scope; overall verdict; confidence rating; top PASS areas; top open risks; explicit "not RedisBloom drop-in" boundary; evidence pointers to Stage 01, 05, 06, 07, 08, 09, 10. |
| `01_DESIGN约束与结论对齐.md` | DESIGN compatibility commitments, DESIGN-intended differences, known limitations, and a claim-to-stage outcome table based on Stage 00 `design_claims_matrix.md` plus Stage 03/05/06/09/10 conclusions. |
| `02_源码实现审计.md` | Static audit summary from Stage 03: Bloom math/hash/RAII/parser/RDB alignment PASS items; open static findings `GBV6-03-001`, `GBV6-03-002`, `GBV6-03-003`; source-comment finding `GBV6-00-002`; no production fixes made. |
| `03_运行时测试结果.md` | Stage 02 build/GTest/TCL baseline; Stage 04 raw RESP command semantics for all 10 BF commands; Stage 09 command metadata and ACL smoke; distinguish direct/fallback runs from documented-command failures. |
| `04_RedisBloom兼容性矩阵.md` | Exact Redis 6.2.17 + RedisBloom v2.4.20 oracle scope; 9 corpora; RDB/DUMP/RESTORE/MIGRATE/fullsync/RDB-preamble AOF PASS; RedisBloom SCANDUMP/LOADCHUNK and command-AOF cross failures as `DESIGN_INTENDED`; live command-stream BF.CARD drift as known limit. |
| `05_持久化迁移复制审计.md` | Stage 06 transport matrix and Stage 09 completed-filter replica/cluster findings; normal completed-filter RDB-family PASS; half-loaded LOADCHUNK persistence/fullsync defect `GBV6-07-002`; TTL evidence; AOF preamble vs command-AOF boundary. |
| `06_安全与资源边界.md` | Stage 07 fuzz/fault safety, Stage 08 sanitizer blocker, Stage 10 resource/perf samples; open P1/P2 safety findings; large allocation and resource-boundary caveats; state that latency is audit sample only. |
| `07_问题清单与复现.md` | Complete finding table with severity/status/impact/evidence/repro/suggested fix direction for all open findings: `GBV6-00-001`, `GBV6-00-002`, `GBV6-02-001`, `GBV6-02-002`, `GBV6-03-001`, `GBV6-03-002`, `GBV6-03-003`, `GBV6-05-001`, `GBV6-07-001`, `GBV6-07-002`. Include blockers and non-finding coverage gaps separately. |
| `08_测试覆盖与未覆盖.md` | Coverage by stage; test counts and evidence; blocked/not-verified matrix; expected gaps; sanitizer/valgrind blocker; ACL DRYRUN blocker; ASK not verified; max-capacity default/low-error allocation not verified; kill-during-BGSAVE not verified. |
| `09_最终结论与修复优先级.md` | Final confidence rating and release risk posture; prioritized fixes: P1 LOADCHUNK integrity first, P2 RDB/wire resource enforcement and Linux/GCC build next, test harness/doc cleanup after; criteria for re-audit. |
| `10_报告自审结果.md` | Stage 11 pre-self-audit handoff for Stage 12: list report files, known caveats, and checks Stage 12 must perform. Mark as "待 Stage 12 自审更新" rather than a final PASS. |
| `evidence_index.md` | Final report evidence index mapping every chapter and major conclusion to Stage 00-10 evidence paths and Stage 11 synthesis evidence paths. |

## Evidence files to produce under `.codex/gemini-bloom-audit/v6/evidence/stage11/`

Stage 11 should create a report-synthesis evidence directory even though no new product tests are run.

Required Stage 11 evidence:

| Evidence file | Purpose |
|---|---|
| `commands.txt` | List all read/synthesis/write commands used in Stage 11; explicitly state no product tests were run. |
| `stdout.log` | Summaries of file inventories, matrix extraction, report file creation, and validation command output. |
| `stderr.log` | Any read/write/validation errors. If none, use an explicit `NO_STDERR` note. |
| `exit_codes.txt` | Exit codes for synthesis and validation commands. |
| `env_snapshot.txt` | cwd, git HEAD/branch/status, date/time, and report output path baseline. |
| `evidence_index.md` | Index of Stage 11 synthesis evidence and the final report files. |
| `input_inventory.md` | Checklist showing all required Stage 11 inputs were re-read: DESIGN, controls, policies, state, Stage 00-10 outputs, evidence indexes, findings, matrices. |
| `stage_summary_matrix.md` | One row per Stage 00-10 with status, reviewer verdict, main evidence, findings/blockers, and final-report impact. |
| `design_alignment_summary.md` | Condensed DESIGN claim outcomes for final report `01_DESIGN约束与结论对齐.md`. |
| `finding_carry_forward_matrix.md` | Canonical open finding table with severity, status, impact, evidence, repro/fix direction. |
| `blocked_not_verified_matrix.md` | Canonical blocker/NOT_VERIFIED table with confidence impact. |
| `report_file_manifest.md` | Final report file list, byte counts, and purpose; must include `10_报告自审结果.md`. |
| `report_claim_evidence_map.md` | Map of report claims to exact evidence paths; used to prevent unsupported final-report conclusions. |
| `final_report_validation.md` | Checklist that report files exist, are Chinese, include evidence paths, preserve DESIGN boundaries, and carry forward blockers/findings. |

Do not create new runtime/fuzz/performance evidence in Stage 11. If a needed conclusion lacks evidence, mark it `NOT_VERIFIED` in the report rather than running new tests from the planner/synthesis stage.

## Finding/blocker/NOT_VERIFIED carry-forward matrix

The final report must carry forward at least this matrix.

### Open findings

| ID | Severity | Status | Final-report classification | Required report handling |
|---|---:|---|---|---|
| `GBV6-00-001` | P3 | OPEN | Documentation/evidence gap | Report that DESIGN claims checked-in RedisBloom v2.4.20 fixtures exist, but the path is absent. Note Stage 05/06 provide oracle evidence by other means, not checked-in fixtures. |
| `GBV6-00-002` | P3 | OPEN | Source comment/design conflict | Report `sb_chain.h` SCANDUMP/LOADCHUNK comment conflicts with DESIGN private protocol boundary. Do not classify private protocol itself as a bug. |
| `GBV6-02-001` | P2 | OPEN | Test/build infrastructure | Report macOS CMake `bloom_test` target execution fails due missing GTest dylib RPATH; direct GTests pass 114/114 with library path. |
| `GBV6-02-002` | P2 | OPEN | Test harness expected-gap handling | Report TCL returns nonzero for 6 DESIGN_INTENDED expected gaps; product behavior is not failed by those gaps, but default harness is not a clean CI command. |
| `GBV6-03-001` | P2 | OPEN | RDB/wire resource-boundary violation | Report shared RDB/wire validation lacks DESIGN per-layer 2GB cap, confirmed by Stage 03/07 static evidence. |
| `GBV6-03-002` | P2 | OPEN | RDB/wire expansion-boundary violation | Report RDB/wire accepts expansion values above `32768`; Stage 07 header fuzz confirms accepted over-limit values. |
| `GBV6-03-003` | P3 | OPEN | Test coverage/comment mismatch | Report TCL test name/comment overclaims per-layer cap rejection and mentions stale 512MB vs 2GB design. |
| `GBV6-05-001` | P2 | OPEN | Linux/GCC build portability | Report default Linux/GCC build fails due missing `<climits>` for `UINT_MAX`; Stage 05-10 runtime builds used audit-only `-include climits` workaround where noted. |
| `GBV6-07-001` | P1 | OPEN | LOADCHUNK data-integrity bug | Report out-of-order/repeated chunks can complete filters with inserted-item false negatives. This is a primary fix priority. |
| `GBV6-07-002` | P1 | OPEN | LOADCHUNK persistence/fullsync data-integrity bug | Report half-loaded LOADCHUNK keys can persist/replay/fullsync as completed filters with false negatives. Include Stage 09 operational fullsync evidence. |

### Blockers and degraded coverage

| ID / item | Status | Required final-report handling |
|---|---|---|
| `GBV6-04-BLOCK-001` | BLOCKED | Stage 04 could not verify ACL DRYRUN on Redis 6.2.16 because the subcommand is unavailable. Stage 09 on Redis 6.2.17 also records DRYRUN unavailable but adds actual ACL smoke evidence. Do not claim ACL DRYRUN PASS. |
| `GBV6-08-BLOCK-001` | BLOCKED | ASAN/UBSAN runtime, sanitizer GTests/TCL, and valgrind did not run. Static fallback found no new concrete memory/UB issue, but final report must not claim sanitizer memory-safety PASS. |
| `GBV6-09-NV-001` | NOT_VERIFIED | Cluster ASK routing was not deterministically produced. Owner, MOVED, `redis-cli -c`, same-slot SCANDUMP/LOADCHUNK, and READONLY replica path passed, but ASK must remain not verified. |
| `GBV6-10-NV-001` | NOT_VERIFIED | Runtime allocation for default/low-error `capacity=2^30` was intentionally skipped for safety. Only high-error `NONSCALING` max-capacity probe passed. |
| `GBV6-10-NV-002` | NOT_VERIFIED | Stage 10 did not rerun command-AOF `aof-use-rdb-preamble no`; rely on Stage 06 same-module/design-boundary evidence and keep Stage 10 row not verified. |
| Stage 07 `kill_during_bgsave` | NOT_VERIFIED | Direct process-kill fault injection was not run; deterministic half-loaded persistence faults were run instead. |
| Stage 07 direct `bloom_rdb_test` rerun | BLOCKED | Reused Stage 05 module-only build lacked GTest target; existing Stage 02 GTest evidence remains the GTest baseline. |
| Stage 08 UBSAN findings | NOT_VERIFIED | `ubsan_findings.md` records absence of runtime evidence, not absence of UB. |
| `COMMAND GETKEYSANDFLAGS` | NOT_VERIFIED/unsupported | Redis 6.2.17 environment does not support it; Stage 09 `COMMAND GETKEYS` is the verified baseline. |

## False PASS / false FAIL risks for the final report

False PASS risks to prevent:

- Treating overall Stage 07 or Stage 09 `PASS` as product-clean PASS. Both passed audit gates while preserving open P1 LOADCHUNK findings.
- Saying sanitizer/UBSAN/valgrind passed because Stage 08 static fallback found no new issue. Runtime sanitizer coverage is `BLOCKED`.
- Saying ACL is fully verified because actual ACL smoke passed. `ACL DRYRUN` remains blocked and actual smoke did not cover every write command denial.
- Saying cluster routing is fully verified. ASK is `NOT_VERIFIED`.
- Saying default Linux/GCC build passes. Runtime comparison used `-include climits`; `GBV6-05-001` remains open.
- Saying checked-in RedisBloom fixture corpus exists. The exact path remains missing despite Stage 05/06 oracle evidence.
- Saying RedisBloom compatibility includes SCANDUMP/LOADCHUNK, RESP3, BF.DEBUG, or command-AOF cross replay. DESIGN excludes those.
- Saying capacity `2^30` is broadly safe. Only high-error `NONSCALING` was safely run; default/low-error allocation remains `NOT_VERIFIED`.
- Treating Stage 10 latency samples as production benchmarks or SLO evidence.
- Treating ordered same-module LOADCHUNK replay PASS as resolving out-of-order/repeated/half-loaded LOADCHUNK failures.

False FAIL risks to prevent:

- Reporting RESP3 unsupported, BF.DEBUG unsupported, RedisBloom SCANDUMP/LOADCHUNK incompatibility, scalar `BF.INFO FIELD`, BF.INFO Size mismatch, strict parser behavior, or command-AOF cross incompatibility as bugs rather than `DESIGN_INTENDED`.
- Treating Bloom false positives as failures. Only inserted-item false negatives are data-integrity failures.
- Treating live command-stream cross-implementation `BF.CARD` drift in the expansion-1 corpus as a compatibility failure when membership has no inserted-item false negatives and DESIGN documents the limit.
- Treating Redis 6.2.x lack of `ACL DRYRUN` as module failure.
- Treating missing ASK reproduction as evidence of broken ASK routing.
- Treating macOS stale `./build` cache or GTest dylib RPATH as production implementation failure; these are test/build infrastructure findings.
- Treating RSS, `MEMORY USAGE`, and `BF.INFO Size` mismatch as resource-accounting bugs without internal inconsistency evidence.

## PASS/BLOCKED criteria

Stage 11 should be marked `PASS` only if all conditions below are true:

- All required inputs listed in the planner prompt and Stage 11 file are re-read and recorded in `input_inventory.md`.
- All required final report files exist under `doc/code_review/gemini-bloom/v6/`, including the stricter policy-required `10_报告自审结果.md`.
- Final report files are in Chinese except for technical identifiers, paths, commands, and status labels.
- Each major conclusion cites concrete `.codex/gemini-bloom-audit/v6/evidence/...` paths or Stage 11 synthesis evidence paths.
- The open finding table includes every global open finding from `LOOP_STATE.md` and does not close any finding without evidence.
- The blocker/NOT_VERIFIED table includes Stage 04 ACL DRYRUN, Stage 08 sanitizer, Stage 09 ASK, Stage 10 max-capacity default/low-error allocation, and Stage 10 command-AOF no-preamble rerun gaps.
- DESIGN_INTENDED differences are clearly separated from product defects.
- The final confidence rating is downgraded consistently with open P1 findings and blocked sanitizer coverage.
- `stage_result.md` states planner adoption, report files, evidence files, carry-forward findings/blockers, confidence impact, reviewer handoff, and planner closed state.
- Reviewer can audit Stage 11 without rerunning tests.

Stage 11 should be `BLOCKED` if any of these occurs:

- Required Stage 00-10 outputs or evidence indexes cannot be read and no persisted substitute exists.
- The final report directory cannot be written.
- Any required report file cannot be produced.
- The main agent cannot produce a claim-to-evidence map sufficient to avoid unsupported conclusions.
- The report would need new runtime testing to support a required claim; in that case, do not run tests from Stage 11. Mark the claim `NOT_VERIFIED`, or if the missing evidence prevents final synthesis entirely, block with concrete evidence.

Stage 11 should not be `FAIL` merely because earlier stages found bugs. It should fail only if the synthesis itself is incorrect, unsupported, overclaims, omits required report files, or violates DESIGN-first reporting rules.
