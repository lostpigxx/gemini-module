# Stage 00 Evidence Index

| Evidence | Supports |
|---|---|
| `commands.txt` | Reproducible list of Stage 00 rehydrate, branch, static inventory, and line-evidence commands. |
| `stdout.log` | Branch baseline, module/test inventory, line evidence for DESIGN-derived contract, and source comment mismatch context. |
| `stderr.log` | Missing compat fixture path, missing `.github`, and sandbox write escalation evidence. |
| `exit_codes.txt` | Command outcomes, including missing path checks and non-run test scope. |
| `env_snapshot.txt` | Stage 00 branch/source baseline and static environment summary. |
| `.codex/gemini-bloom-audit/v6/agents/stage00/rehydrate_log.md` | Required rehydrate record and Stage 00 scope boundaries. |
| `.codex/gemini-bloom-audit/v6/agents/stage00/planner_output.md` | Required planner subagent plan reviewed by main agent. |

## Findings supported by this evidence

- `GBV6-00-001`: DESIGN.md claims `tests/compat/redisbloom-2.4.20/` stores RedisBloom fixture files, but the path is missing in the audited tree.
- `GBV6-00-002`: `src/sb_chain.h` says SCANDUMP/LOADCHUNK wire structures match RedisBloom for cross-implementation compatibility, conflicting with DESIGN.md's private non-interoperable protocol boundary.
