# Numeric Edge Matrix

| Class | Status | Evidence |
|---|---|---|
| `expansion_zero` | PASS | `stage07_fuzz_results.json` header_fuzz row; reply `{"error": "ERR corrupted header payload"}` |
| `expansion_limit` | PASS | `stage07_fuzz_results.json` header_fuzz row; reply `"OK"` |
| `expansion_over_limit` | FAIL | `stage07_fuzz_results.json` header_fuzz row; reply `"OK"` |
| `expansion_uint_max` | FAIL | `stage07_fuzz_results.json` header_fuzz row; reply `"OK"` |
| `item_count_gt_capacity` | PASS | `stage07_fuzz_results.json` header_fuzz row; reply `{"error": "ERR corrupted header payload"}` |
| `hash_count_zero` | PASS | `stage07_fuzz_results.json` header_fuzz row; reply `{"error": "ERR corrupted header payload"}` |
| `hash_count_wrong` | PASS | `stage07_fuzz_results.json` header_fuzz row; reply `{"error": "ERR corrupted header payload"}` |
| `fp_rate_nan` | PASS | `stage07_fuzz_results.json` header_fuzz row; reply `{"error": "ERR corrupted header payload"}` |
| `fp_rate_inf` | PASS | `stage07_fuzz_results.json` header_fuzz row; reply `{"error": "ERR corrupted header payload"}` |
| `fp_rate_zero` | PASS | `stage07_fuzz_results.json` header_fuzz row; reply `{"error": "ERR corrupted header payload"}` |
| `fp_rate_one` | PASS | `stage07_fuzz_results.json` header_fuzz row; reply `{"error": "ERR corrupted header payload"}` |
| `bits_per_entry_nan` | PASS | `stage07_fuzz_results.json` header_fuzz row; reply `{"error": "ERR corrupted header payload"}` |
| `bits_per_entry_inf` | PASS | `stage07_fuzz_results.json` header_fuzz row; reply `{"error": "ERR corrupted header payload"}` |
| `bits_per_entry_zero` | PASS | `stage07_fuzz_results.json` header_fuzz row; reply `{"error": "ERR corrupted header payload"}` |
| `bits_per_entry_gt_1000` | PASS | `stage07_fuzz_results.json` header_fuzz row; reply `{"error": "ERR corrupted header payload"}` |
| `total_bits_zero` | PASS | `stage07_fuzz_results.json` header_fuzz row; reply `{"error": "ERR corrupted header payload"}` |
| `log2_bits_64` | PASS | `stage07_fuzz_results.json` header_fuzz row; reply `{"error": "ERR corrupted header payload"}` |
| `data_size_mismatch` | PASS | `stage07_fuzz_results.json` header_fuzz row; reply `{"error": "ERR corrupted header payload"}` |
| `per_layer_data_size_gt_2gb` | FAIL_STATIC | `rdb_payload_matrix.md`, Stage 03 finding `GBV6-03-001` |
