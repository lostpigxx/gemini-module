# Stage 01 Main Execution

## Planner review

Planner output reviewed: `.codex/gemini-bloom-audit/v6/agents/stage01/planner_output.md`.

Adopted planner guidance:

- Stage 01 is an environment/repository reproducibility baseline stage.
- It must not run builds, tests, Redis runtime checks, RedisBloom oracle checks, Docker containers, fuzzers, sanitizers, or performance tests.
- Tool presence is not functional PASS; it only establishes whether later stages can attempt their checks.
- Push failure at the stage gate is blocking and must stop the loop.

Main-agent additions:

- Captured `sw_vers` because this host is macOS and `/etc/os-release` is absent.
- Added `git ls-files modules/gemini-bloom` as a tracked-file cross-check because `find` sees an ignored local `__pycache__` file.
- Rechecked the Stage 00 missing compat fixture and `.github` absence as Stage 01 baseline facts.

## Execution summary

Created Stage 01 required evidence:

- `.codex/gemini-bloom-audit/v6/evidence/stage01/git_snapshot.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage01/env_snapshot.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage01/tool_versions.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage01/repo_tree_gemini_bloom.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage01/dependency_status.txt`

Created evidence policy files:

- `.codex/gemini-bloom-audit/v6/evidence/stage01/commands.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage01/stdout.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage01/stderr.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage01/exit_codes.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage01/evidence_index.md`

## Key facts

- Branch: `audit/gemini-bloom-v6`.
- Stage 01 baseline HEAD: `fa165b27fc498da23b0861f99e5f2919d89dd897`.
- Branch tracks `origin/audit/gemini-bloom-v6`.
- Host OS: macOS 26.5.1 / Darwin 25.5.0 arm64.
- CMake: 3.29.0.
- C++ compiler: Apple clang 21.0.0.
- Redis server/client: 6.2.16.
- Tcl: 8.5.9.
- Python: 3.13.7.
- Docker CLI: 27.4.0.

## Dependency and confidence notes

- Redis 6.2.16 is available, but DESIGN.md's historical RedisBloom v2.4.20 compatibility baseline names Redis 6.2.17. Later RedisBloom comparisons must record their exact Redis version and avoid claiming a Redis 6.2.17 reproduction unless that version is actually used.
- The documented `modules/gemini-bloom/tests/compat/redisbloom-2.4.20` fixture path is still absent; `GBV6-00-001` remains open.
- `.github` is absent; no GitHub Actions CI gate is visible from this checkout. Other CI systems are `NOT_VERIFIED`.
- A local ignored `modules/gemini-bloom/tests/pict/__pycache__/` file appears in the raw `find` tree but is not tracked and is not part of the audited source state.

## Non-executed scope

The following are intentionally not verified in Stage 01:

- CMake configure/build success.
- GTest and TCL tests.
- Redis server startup/module loading.
- RedisBloom oracle availability or version.
- RDB/DUMP/RESTORE/MIGRATE/fullsync/AOF behavior.
- Fuzz/fault/sanitizer/memory safety.
- Replica/cluster/ACL/COMMAND runtime behavior.
- Performance/resource behavior.
