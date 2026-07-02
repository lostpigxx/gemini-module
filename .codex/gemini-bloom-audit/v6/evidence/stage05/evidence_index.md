# Stage 05 Evidence Index

| Evidence | Purpose |
|---|---|
| `oracle_env.txt` | Exact Redis/RedisBloom/gemini oracle environment and build notes. |
| `diff_raw_resp.log` | Focused raw RESP command-level diff evidence. |
| `diff_normalized.md` | Normalized command-level and extended diff summary. |
| `compatibility_matrix.md` | Path/corpus compatibility matrix classified against DESIGN.md. |
| `oracle_diff/compat_matrix_results_redis62_redisbloom2420.json` | Full 9-corpus transport matrix raw JSON. |
| `oracle_diff/extended_audit_results_redis62_redisbloom2420.json` | MIGRATE/TTL, metadata, incremental AOF, and extra command JSON. |
| `oracle_diff/gemini_default_build_stderr.log` | Default Linux/GCC build failure evidence for GBV6-05-001. |
| `oracle_diff/redisbloom_compat_matrix.py` | Matrix harness copied from v5 audit branch and rerun for v6. |
| `oracle_diff/redisbloom_extended_audit.py` | Extended harness copied from v5 audit branch and rerun for v6. |
| `oracle_diff/stage05_raw_resp_diff.py` | Focused raw RESP diff harness. |
| `commands.txt` | Commands run for this stage. |
| `stdout.log` | Combined stdout. |
| `stderr.log` | Combined stderr and default build failure stderr. |
| `exit_codes.txt` | Key command exit codes. |
| `env_snapshot.txt` | Environment snapshot copy. |
