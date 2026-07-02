# Stage 02 TCL Summary

Command:

```bash
tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl /private/tmp/gemini-bloom-stage02-build.G7fAub/redis_bloom.so
```

Exit code: `6`.

Raw output:

- `bloom_test_tcl_stdout.log`
- `bloom_test_tcl_stderr.log`
- `bloom_test_tcl_exit_code.txt`

Summary from stdout:

```text
Results: 144 passed, 6 failed
```

All 6 failures are labeled expected design/compatibility gaps in the test names:

```text
EXPECTED RESP3 GAP: BF.ADD returns boolean type
EXPECTED RESP3 GAP: BF.EXISTS returns boolean type
EXPECTED RESP3 GAP: BF.MADD returns array of booleans
EXPECTED RESP3 GAP: BF.MEXISTS returns array of booleans
EXPECTED RESP3 GAP: BF.INFO full response returns map type
EXPECTED COMPAT GAP: SCANDUMP layer cursor should advance by byte length
```

Classification:

- 144 runtime integration checks: `PASS`.
- 6 expected-gap checks: `DESIGN_INTENDED` behavior, because DESIGN.md explicitly states RESP3 is unsupported and RedisBloom SCANDUMP/LOADCHUNK byte-offset protocol is not interoperable.
- TCL script exit status: `FAIL` / `TEST_HARNESS`, because expected gaps are still counted as failures and produce nonzero exit code, contrary to DESIGN.md's statement that expected gaps are not CI blockers.

Cleanup evidence:

- `redis-cli -p 53386 PING` after the test returned connection refused.
- `ps -axo pid,command | rg 'redis-server.*53386|redis-server.*bloom'` found no redis-server process; only the `rg` command itself appeared.
