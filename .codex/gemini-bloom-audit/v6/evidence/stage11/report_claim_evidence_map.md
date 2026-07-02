# Stage 11 Report Claim Evidence Map

| Report claim | Report file | Evidence |
|---|---|---|
| Audit is not a RedisBloom drop-in compatibility claim | `00`, `01`, `04` | `.codex/gemini-bloom-audit/v6/agents/stage00/design_contract.md` |
| Redis 6.2.17 + RedisBloom v2.4.20 RDB-family paths passed | `00`, `04`, `05` | `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md`, `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |
| Runtime command semantics covered all 10 BF commands | `03` | `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/coverage_summary.md` |
| Direct GTests pass, documented target has RPATH issue | `03`, `07` | `.codex/gemini-bloom-audit/v6/evidence/stage02/gtest/gtest_summary.md` |
| TCL expected gaps are DESIGN_INTENDED but harness exits nonzero | `03`, `07`, `08` | `.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/tcl_summary.md` |
| Static RDB/wire resource gaps remain open | `02`, `06`, `07`, `09` | `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md` |
| LOADCHUNK P1 false-negative defects remain open | `00`, `05`, `06`, `07`, `09` | `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md`, `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/loading_partial_fullsync.log` |
| Sanitizer runtime coverage is blocked | `00`, `06`, `08`, `09` | `.codex/gemini-bloom-audit/v6/evidence/stage08/blocked_sanitizer.md` |
| Cluster ASK remains not verified | `05`, `08` | `.codex/gemini-bloom-audit/v6/evidence/stage09/cluster/ask_not_verified.md` |
| Stage 10 latency samples are not benchmarks | `06`, `08` | `.codex/gemini-bloom-audit/v6/evidence/stage10/perf_matrix.md`, `.codex/gemini-bloom-audit/v6/evidence/stage10/latency_samples.csv` |
| Default/low-error max capacity allocation remains unverified | `06`, `08` | `.codex/gemini-bloom-audit/v6/evidence/stage10/blocked_or_not_verified.md` |
