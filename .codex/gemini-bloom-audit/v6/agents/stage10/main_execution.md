# Stage 10 Main Execution

Reviewed the planner output and adopted its bounded audit-sample plan. The main execution kept the planner's safety boundary: do not allocate a default/low-error `capacity=2^30` filter, do not treat latency samples as a formal benchmark, and do not treat ordered same-module `LOADCHUNK` success as resolving Stage 07 findings.

Planner adjustments:

1. Built a fresh Stage 10 module with the known Stage 05 workaround, `-DCMAKE_CXX_FLAGS=-include climits`, because the default Linux/GCC build failure remains tracked as `GBV6-05-001`.
2. Used Redis 6.2.17 in Docker and an isolated temporary Redis data directory.
3. Treated command-AOF `aof-use-rdb-preamble no` as `NOT_VERIFIED` for Stage 10 because Stage 06 already covered the same-module/design boundary and Stage 10's persistence-size focus used RDB-preamble AOF.
4. Marked default/low-error `capacity=2^30` runtime allocation as `NOT_VERIFIED` for safety; Stage 10 only ran the high-error `NONSCALING` safety probe.

Executed:

1. Wrote `.codex/gemini-bloom-audit/v6/evidence/stage10/stage10_perf_resource.py` as the reproducible audit runner.
2. Built `/tmp/gemini-module-v6-stage10-build-workaround/redis_bloom.so`.
3. Started isolated Redis 6.2.17 instances with the Stage 10 module artifact.
4. Ran resource-limit probes for capacity, error rate, expansion, bits-per-entry, per-layer cap, and fixed-filter full behavior.
5. Recorded bounded latency samples for `ADD`, `MADD`, `EXISTS`, `MEXISTS`, `INFO`, `CARD`, `SCANDUMP`, and `LOADCHUNK`.
6. Recorded memory accounting samples comparing `BF.INFO Size`, Redis `MEMORY USAGE`, `INFO memory`, and OS RSS.
7. Ran SCANDUMP/LOADCHUNK private cursor protocol samples for single-layer, multi-layer, expansion-1 many-layer, and capacity-100000 filters.
8. Recorded RDB and AOF RDB-preamble file sizes and performed a same-module restart sanity check.
9. Captured static resource-audit notes for parser limits, `deny-oom`, runtime allocation caps, `BF.INFO` accounting, and carry-forward RDB/wire gaps.

Evidence paths:

- `.codex/gemini-bloom-audit/v6/evidence/stage10/evidence_index.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/commands.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/stdout.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/stderr.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/exit_codes.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/env_snapshot.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/perf_matrix.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/resource_limits.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/memory_usage.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/latency_samples.csv`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/scandump_loadchunk.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/persistence_size.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/static_resource_audit.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/blocked_or_not_verified.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/stage10_results.json`

Result:

- Stage status proposed: `PASS` for audit completion.
- `stage10_results.json` records 393 `PASS` classifications and one `NOT_VERIFIED` classification, with no `FAIL` or `BLOCKED`.
- `PA-03` is `NOT_VERIFIED`: command-AOF `aof-use-rdb-preamble no` was not rerun in Stage 10.
- Default/low-error `capacity=2^30` allocation remains `NOT_VERIFIED` by safety policy.
- RedisBloom byte-offset SCANDUMP/LOADCHUNK compatibility is `DESIGN_INTENDED_NOT_APPLICABLE`, not a Stage 10 pass criterion.
- No new Stage 10 finding ID is opened.

Carry-forward impact:

- `GBV6-03-001` remains open: RDB/wire deserialization lacks DESIGN's per-layer 2GB data-size cap.
- `GBV6-03-002` remains open: RDB/wire deserialization accepts expansionFactor values above `kMaxExpansion`.
- `GBV6-05-001` remains open: Stage 10 runtime build used the workaround, not the default Linux/GCC build.
- `GBV6-07-001` and `GBV6-07-002` remain open: Stage 10 ordered LOADCHUNK replay does not mitigate out-of-order/repeated/half-loaded LOADCHUNK integrity failures.

First reviewer result:

- First Stage 10 reviewer returned `FAIL` only because this `main_execution.md` protocol artifact was missing.
- The technical evidence and classifications were otherwise judged supported.
- The correction is recorded in `.codex/gemini-bloom-audit/v6/agents/stage10/retry_1.md`.

Planner closed: yes.
Reviewer closed: yes.
Next stage may only use persisted files, not live subagent context.
