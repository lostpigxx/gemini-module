# Fault Injection Matrix

| Case | Status | Classification | Evidence |
|---|---|---|---|
| `persist_half_loaded_rdb` | FAIL | FAIL_LOADING_PERSISTED_AS_COMPLETED | `stage07_fuzz_results.json` section `persistence_faults`; `server_logs/` |
| `persist_half_loaded_aof_no_preamble` | FAIL | FAIL_LOADING_PERSISTED_AS_COMPLETED | `stage07_fuzz_results.json` section `persistence_faults`; `server_logs/` |
| `kill_during_bgsave` | NOT_VERIFIED | NOT_VERIFIED | Not run to avoid nondeterministic host-level process kill; half-loaded RDB/AOF persistence fault injection was run instead. |
