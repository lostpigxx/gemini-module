# Stage 11 Finding Carry-forward Matrix

            | ID | Severity | Status | Title | Evidence |
            |---|---:|---|---|---|
            | GBV6-00-001 | P3 | OPEN | DESIGN fixture path absent | `.codex/gemini-bloom-audit/v6/evidence/stage00/stderr.log` |
| GBV6-00-002 | P3 | OPEN | SCANDUMP comment conflicts with DESIGN | `.codex/gemini-bloom-audit/v6/evidence/stage00/stdout.log` |
| GBV6-02-001 | P2 | OPEN | CMake bloom_test GTest RPATH | `.codex/gemini-bloom-audit/v6/evidence/stage02/gtest/bloom_test_target_stderr.log` |
| GBV6-02-002 | P2 | OPEN | TCL expected gaps exit nonzero | `.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/tcl_summary.md` |
| GBV6-03-001 | P2 | OPEN | RDB/wire missing per-layer 2GB cap | `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md` |
| GBV6-03-002 | P2 | OPEN | RDB/wire accepts expansion >32768 | `.codex/gemini-bloom-audit/v6/evidence/stage07/crash_or_failure_repro/README.md` |
| GBV6-03-003 | P3 | OPEN | TCL per-layer cap test mismatch | `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md` |
| GBV6-05-001 | P2 | OPEN | Linux/GCC default build missing <climits> | `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/gemini_default_build_stderr.log` |
| GBV6-07-001 | P1 | OPEN | LOADCHUNK out-of-order/repeated false negatives | `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md` |
| GBV6-07-002 | P1 | OPEN | Half-loaded LOADCHUNK persists/fullsyncs corrupt | `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/loading_partial_fullsync.log` |
