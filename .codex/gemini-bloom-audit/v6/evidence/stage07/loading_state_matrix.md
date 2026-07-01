# Loading State Matrix

| Area | Status | Evidence |
|---|---|---|
| Header-only key blocks ADD/MADD/INSERT/EXISTS/MEXISTS/INFO/CARD/SCANDUMP | PASS | `stage07_fuzz_results.json` section `loading_state.blocked_commands` |
| Bad data chunk keeps key non-queryable | PASS | `stage07_fuzz_results.json` section `loading_state.bad_chunk` and `blocked_after_bad` |
| Full ordered chunk replay clears Loading and preserves membership | PASS | `missing_count=0` |
| Completed key rejects cursor>1 overwrite | PASS | `stage07_fuzz_results.json` section `loading_state.overwrite_after_completion` |
