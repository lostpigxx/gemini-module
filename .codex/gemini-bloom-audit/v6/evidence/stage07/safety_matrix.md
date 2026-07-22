# Cursor Fault Matrix

| Case | Status | Classification | Evidence |
|---|---|---|---|
| `cursor_gt1_missing_key` | PASS | PASS | `stage07_fuzz_results.json` section `cursor_faults` |
| `cursor_gt1_completed_key` | PASS | PASS | `stage07_fuzz_results.json` section `cursor_faults` |
| `bad_cursor_0` | PASS | PASS | `stage07_fuzz_results.json` section `cursor_faults` |
| `bad_cursor_-1` | PASS | PASS | `stage07_fuzz_results.json` section `cursor_faults` |
| `bad_cursor_abc` | PASS | PASS | `stage07_fuzz_results.json` section `cursor_faults` |
| `cursor_skip_to_final_exposes_false_negatives` | FAIL | FAIL_ACCEPTED_MALFORMED_SEQUENCE | `stage07_fuzz_results.json` section `cursor_faults` |
| `repeat_first_chunk_for_all_layers` | FAIL | FAIL_ACCEPTED_MALFORMED_SEQUENCE | `stage07_fuzz_results.json` section `cursor_faults` |

## Overall Summary

{
  "cursor_faults": {
    "FAIL": 2,
    "PASS": 5
  },
  "existing_key_safety": {
    "PASS": 2
  },
  "header_fuzz": {
    "FAIL": 2,
    "PASS": 92
  },
  "loading_state": "PASS",
  "persistence_faults": {
    "FAIL": 2
  },
  "server_alive_end": true,
  "static_resource_boundary": {
    "FAIL": 2
  }
}
