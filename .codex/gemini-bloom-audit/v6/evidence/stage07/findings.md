# Stage 07 Findings

## GBV6-07-001 — BF.LOADCHUNK accepts out-of-order or repeated data chunks and can complete a filter with false negatives

- Severity: P1
- Status: OPEN
- Evidence: `.codex/gemini-bloom-audit/v6/evidence/stage07/stage07_fuzz_results.json` rows `cursor_skip_to_final_exposes_false_negatives` and `repeat_first_chunk_for_all_layers`.
- Actual: Header plus final chunk clears Loading without earlier chunks; `BF.CARD` reports 20 but only 5/20 inserted corpus items are found. Repeating the first same-sized chunk for every layer also completes the key with 15/20 false negatives.
- Expected: Malformed or out-of-order LOADCHUNK sequences must not create queryable filters with inserted-item false negatives.
- Suggested fix direction: Track which layer chunks have been loaded, require exact cursor order or a completion bitmap, and clear Loading only after every layer has been received exactly once.

## GBV6-07-002 — Half-loaded LOADCHUNK keys persist/replay as completed filters with false negatives

- Severity: P1
- Status: OPEN
- Evidence: `.codex/gemini-bloom-audit/v6/evidence/stage07/stage07_fuzz_results.json` rows `persist_half_loaded_rdb` and `persist_half_loaded_aof_no_preamble`.
- Actual: Header-only Loading key blocks commands before persistence, but after `SAVE`/restart or successful `BGREWRITEAOF`/restart, the key loads as normal `MBbloom--`; `BF.CARD` reports 40 while 40/40 inserted corpus items are false negatives.
- Expected: Runtime-only Loading state must not be serialized as a completed Bloom filter. Persistence should reject, skip, preserve Loading as non-queryable, or otherwise avoid exposing a corrupt completed filter.
- Suggested fix direction: Prevent RDB/AOF rewrite for Loading filters or persist enough load-state/chunk-completion metadata to keep the key non-queryable after restart. Consider deleting incomplete keys on persistence/replay if that is the documented behavior.

## Existing findings confirmed

- `GBV6-03-001`: Stage 07 static resource-boundary review confirms the per-layer 2GB cap is still missing from shared RDB/wire validation before allocation.
- `GBV6-03-002`: Stage 07 runtime header fuzz confirms `BF.LOADCHUNK` accepts `expansionFactor=32769` and `UINT_MAX`.
