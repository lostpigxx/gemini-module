# Stage 04 Main Execution

## Planner review

Planner output reviewed: `.codex/gemini-bloom-audit/v6/agents/stage04/planner_output.md`.

The planner's recommended raw RESP runtime matrix was adopted. The main agent tightened one point during execution: Redis 6.2.16 in this environment does not expose `ACL DRYRUN`, so ACL dry-run rows are classified as `BLOCKED` with raw Redis error evidence instead of being accepted by a broad `+/-` predicate.

## Execution summary

Runtime harness:

- `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/stage04_runtime_matrix.py`

Executed command:

```text
python3 .codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/stage04_runtime_matrix.py
```

The first sandboxed run failed before execution because the managed sandbox denied writes under `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/commands.txt`. The command was rerun with approved escalation so the stage-local evidence files could be regenerated.

## Runtime evidence

Primary evidence:

- `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/commands.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/raw_resp.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/normalized_results.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/failures.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/coverage_summary.md`

Evidence policy files:

- `.codex/gemini-bloom-audit/v6/evidence/stage04/commands.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/stdout.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/stderr.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/exit_codes.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/env_snapshot.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/evidence_index.md`

## Matrix result

The runtime matrix produced 182 normalized rows:

- 179 rows: `PASS` or `DESIGN_INTENDED`.
- 3 rows: `BLOCKED`, all for `ACL DRYRUN` on Redis 6.2.16.
- 0 rows: product-behavior `FAIL`.

Required command coverage from Stage 04 was present:

- `BF.RESERVE`: 32 rows
- `BF.ADD`: 21 rows
- `BF.MADD`: 8 rows
- `BF.INSERT`: 14 rows
- `BF.EXISTS`: 26 rows
- `BF.MEXISTS`: 8 rows
- `BF.INFO`: 19 rows
- `BF.CARD`: 11 rows
- `BF.SCANDUMP`: 16 rows
- `BF.LOADCHUNK`: 25 rows

Covered semantic areas:

- RESP2 happy path
- RESP3 focused checks
- wrong type
- missing key
- duplicate item
- binary, empty, NUL-containing, non-UTF8, and 10KB items
- capacity, error-rate, and expansion parser/resource boundaries
- NONSCALING full filters
- `BF.MADD` / `BF.INSERT` partial failure arrays
- `BF.INFO` full and field shapes
- `COMMAND INFO` and `COMMAND GETKEYS`
- `BF.SCANDUMP` / `BF.LOADCHUNK` private layer-index protocol
- loading-state read/write rejection

## Design-intended classifications

The following runtime differences were classified as `DESIGN_INTENDED` because they match `modules/gemini-bloom/DESIGN.md`:

- `BF.INFO key FIELD` returns scalar replies, not RedisBloom singleton arrays.
- RESP3 mode keeps RESP2-style command reply shapes while remaining well-formed.
- `BF.SCANDUMP` cursors follow gemini's private layer-index protocol.
- `EXPANSION 0` maps to non-scaling behavior.

## Blocked item

`GBV6-04-BLOCK-001`: `ACL DRYRUN` could not be verified in Stage 04 because the local Redis server is Redis 6.2.16 and returns:

```text
ERR Unknown subcommand or wrong number of arguments for 'DRYRUN'. Try ACL HELP.
```

Evidence:

- `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/failures.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/raw_resp.log`
- `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/normalized_results.md`

Impact:

- `COMMAND INFO` and `COMMAND GETKEYS` metadata were verified.
- ACL dry-run permission effects remain `BLOCKED` for Stage 04 and should be revisited in Stage 09 if Redis 7+ is available or an equivalent ACL verification path is established.

## New findings

No new product findings were opened in Stage 04.

Stage 04 does not close or weaken Stage 03 findings about RDB/wire deserialization resource-boundary enforcement.
