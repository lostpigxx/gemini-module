# Stage 01 Evidence Index

| Evidence | Supports |
|---|---|
| `commands.txt` | Reproducible list of Stage 01 baseline commands. |
| `git_snapshot.txt` | HEAD, branch, dirty status, remotes, branch tracking, and recent commit history. |
| `env_snapshot.txt` | OS and working-directory baseline. |
| `tool_versions.txt` | CMake, compiler, Redis, Tcl, Python, and Docker availability/versions. |
| `repo_tree_gemini_bloom.txt` | `modules/gemini-bloom` filesystem and tracked file tree. |
| `dependency_status.txt` | Dependency availability, missing assets, and impact routing to later stages. |
| `stdout.log` | Summary of important stdout facts and pointers to split evidence files. |
| `stderr.log` | Missing `/etc/os-release`, missing compat fixture path, and missing `.github` evidence. |
| `exit_codes.txt` | Exit code for each baseline command. |

## Stage 01 conclusions supported

- Current audit branch is `audit/gemini-bloom-v6`.
- Current Stage 01 baseline commit is `fa165b27fc498da23b0861f99e5f2919d89dd897`.
- Required baseline tools are available on this host, but Redis is 6.2.16 rather than DESIGN.md's historical Redis 6.2.17 reference.
- `modules/gemini-bloom/tests/compat/redisbloom-2.4.20` is absent, preserving `GBV6-00-001`.
- `.github` is absent, so a GitHub Actions CI gate is not visible in this checkout.
