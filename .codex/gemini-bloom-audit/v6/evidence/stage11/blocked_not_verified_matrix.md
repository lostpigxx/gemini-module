# Stage 11 BLOCKED / NOT_VERIFIED Matrix

            | ID / Item | Status | Confidence impact | Evidence |
            |---|---|---|---|
            | GBV6-04-BLOCK-001 | BLOCKED | ACL DRYRUN unavailable on Redis 6.2.x | `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/failures.md` |
| GBV6-08-BLOCK-001 | BLOCKED | ASAN/UBSAN runtime and valgrind unavailable/incomplete | `.codex/gemini-bloom-audit/v6/evidence/stage08/blocked_sanitizer.md` |
| GBV6-09-NV-001 | NOT_VERIFIED | Cluster ASK not deterministically produced | `.codex/gemini-bloom-audit/v6/evidence/stage09/cluster/ask_not_verified.md` |
| GBV6-10-NV-001 | NOT_VERIFIED | Default/low-error capacity 2^30 allocation skipped | `.codex/gemini-bloom-audit/v6/evidence/stage10/blocked_or_not_verified.md` |
| GBV6-10-NV-002 | NOT_VERIFIED | Stage 10 command-AOF no-preamble rerun skipped | `.codex/gemini-bloom-audit/v6/evidence/stage10/blocked_or_not_verified.md` |
| Stage07-kill-during-bgsave | NOT_VERIFIED | Process-kill fault injection not run | `.codex/gemini-bloom-audit/v6/agents/stage07/stage_result.md` |
| Stage07-direct-bloom-rdb-test | BLOCKED | Module-only build lacked direct GTest target | `.codex/gemini-bloom-audit/v6/agents/stage07/stage_result.md` |
| Stage08-UBSAN | NOT_VERIFIED | No UBSAN runtime execution | `.codex/gemini-bloom-audit/v6/evidence/stage08/ubsan_findings.md` |
