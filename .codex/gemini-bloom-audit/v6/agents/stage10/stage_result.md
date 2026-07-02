# Stage 10 Result - PERF_RESOURCE

Status: `PASS`

## Summary

Stage 10 completed bounded performance/resource audit samples on Redis 6.2.17 with a freshly built gemini-bloom module. The runtime build used the known Stage 05 workaround, `-DCMAKE_CXX_FLAGS=-include climits`; this does not close `GBV6-05-001`.

The resource-limit matrix matched the DESIGN command boundaries: capacity `0` and `2^30 + 1` were rejected, capacity `2^30` was accepted only through a high-error `NONSCALING` safety probe, expansion `32768` was accepted, expansion `32769` was rejected, bits-per-entry overflow and a >2GB per-layer runtime creation attempt were rejected without memory growth, and fixed filters truncated `MADD`/`BF.INSERT` at the first full error.

Latency, memory, SCANDUMP/LOADCHUNK, and persistence-size evidence were recorded as audit samples only. They are not formal benchmark results and must not be reported as production performance guarantees.

Planner adoption and main execution details are recorded in `.codex/gemini-bloom-audit/v6/agents/stage10/main_execution.md`.

## Classifications

| Area | Status | Evidence |
|---|---|---|
| Runtime module build | `PASS_WITH_WORKAROUND` | `.codex/gemini-bloom-audit/v6/evidence/stage10/module_build/` |
| Resource limits | `PASS` | `.codex/gemini-bloom-audit/v6/evidence/stage10/resource_limits.log` |
| Capacity `2^30` default/low-error large allocation | `NOT_VERIFIED` | `.codex/gemini-bloom-audit/v6/evidence/stage10/blocked_or_not_verified.md` |
| Latency audit samples | `PASS_AUDIT_SAMPLE_ONLY` | `.codex/gemini-bloom-audit/v6/evidence/stage10/latency_samples.csv` |
| Memory accounting samples | `PASS` | `.codex/gemini-bloom-audit/v6/evidence/stage10/memory_usage.md` |
| SCANDUMP/LOADCHUNK private protocol | `PASS_DESIGN_INTENDED` | `.codex/gemini-bloom-audit/v6/evidence/stage10/scandump_loadchunk.md` |
| Ordered same-module LOADCHUNK replay | `PASS` | `.codex/gemini-bloom-audit/v6/evidence/stage10/scandump_loadchunk.md` |
| RedisBloom byte-offset SCANDUMP compatibility | `DESIGN_INTENDED_NOT_APPLICABLE` | `.codex/gemini-bloom-audit/v6/evidence/stage10/blocked_or_not_verified.md` |
| RDB/AOF RDB-preamble size and same-module restart | `PASS` | `.codex/gemini-bloom-audit/v6/evidence/stage10/persistence_size.md` |
| Command-AOF `aof-use-rdb-preamble no` rerun | `NOT_VERIFIED` | `.codex/gemini-bloom-audit/v6/evidence/stage10/blocked_or_not_verified.md` |

## Key Evidence Points

- `RL-04`: `BF.RESERVE rl_cap_max_safe 0.99 1073741824 NONSCALING` returned `OK`; `BF.INFO Capacity` reported `1073741824`, `Size` was about 2.8 MB, and RSS did not spike in this sample.
- `RL-09`: `BF.RESERVE rl_too_large_layer 1e-7 1073741824 NONSCALING` returned `ERR allocation failure` with unchanged `used_memory`, matching the runtime per-layer cap path.
- `RL-10`: `BF.RESERVE rl_bits_per_entry 1e-300 10` returned `ERR allocation failure`, matching `bitsPerEntry > 1000` rejection.
- `RL-11`: fixed filters returned ten successful inserts followed by `ERR non scaling filter is full`; `BF.CARD` stayed at `10` and the item after the full error was not processed.
- `MEM-06`: high-error max-capacity filter reported `BF.INFO Size` about 2.8 MB and `MEMORY USAGE` about 2.8 MB; this is a safe-boundary probe only.
- `SLC-01..04`: SCANDUMP returned header cursor `1`, then one full bit-array chunk per layer, then cursor `0`; ordered same-module LOADCHUNK copies preserved sampled membership.
- `PA-01..02`: RDB and AOF RDB-preamble files were both `72473` bytes in this environment, and same-module AOF restart preserved sampled membership including 10KB and 1MB input-derived bits.

## Findings

No new Stage 10 finding ID is opened.

Carry-forward findings remain open:

- `GBV6-03-001`: RDB/wire deserialization still lacks DESIGN's per-layer 2GB data-size cap.
- `GBV6-03-002`: RDB/wire deserialization still accepts expansionFactor values above `kMaxExpansion`.
- `GBV6-05-001`: Linux/GCC default build still requires the Stage 05 workaround in this audit path.
- `GBV6-07-001` and `GBV6-07-002`: orderly Stage 10 LOADCHUNK replay does not mitigate out-of-order/repeated/half-loaded LOADCHUNK integrity failures.

## Evidence

See `.codex/gemini-bloom-audit/v6/evidence/stage10/evidence_index.md`.

Required Stage 10 evidence files are present:

- `.codex/gemini-bloom-audit/v6/evidence/stage10/perf_matrix.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/resource_limits.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/memory_usage.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage10/latency_samples.csv`

## Confidence Impact

Stage 10 improves confidence that normal runtime resource checks and small operational samples match DESIGN boundaries in the audited Redis 6.2.17 environment. It does not improve confidence for sanitizer coverage, RedisBloom SCANDUMP compatibility, default low-error `2^30` allocation behavior, or Stage 07 LOADCHUNK corruption classes.

## Reviewer Retry Note

The first Stage 10 reviewer returned `FAIL` only for missing `.codex/gemini-bloom-audit/v6/agents/stage10/main_execution.md`. The correction is recorded in `.codex/gemini-bloom-audit/v6/agents/stage10/retry_1.md`; runtime evidence was not rerun because the reviewer found the technical claims otherwise supported.

## Agent Closure Note

Planner closed: yes.
Reviewer closed: yes.
Next stage may only use persisted files, not live subagent context.
