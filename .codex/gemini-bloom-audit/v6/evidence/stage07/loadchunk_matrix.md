# LOADCHUNK Header Matrix

| Case | Status | Reply/Note |
|---|---|---|
| `empty` | PASS | empty header blob / `{"error": "ERR corrupted header payload"}` |
| `num_layers_zero` | PASS | numLayers=0 / `{"error": "ERR corrupted header payload"}` |
| `rawbits_flag` | PASS | RawBits flag injection / `{"error": "ERR corrupted header payload"}` |
| `loading_flag` | PASS | Loading flag injection / `{"error": "ERR corrupted header payload"}` |
| `expansion_limit` | PASS | boundary max expansion accepted by command path / `"OK"` |
| `expansion_over_limit` | FAIL | DESIGN max expansion violation / `"OK"` |
| `expansion_uint_max` | FAIL | extreme expansionFactor / `"OK"` |
