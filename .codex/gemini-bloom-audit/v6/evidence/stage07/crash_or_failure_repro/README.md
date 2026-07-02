# Stage 07 Failure Reproductions

All repros use Redis 6.2.17 and `/tmp/gemini-module-v6-stage05-build-workaround/redis_bloom.so` in Docker container `strange_feynman`.

Run the full reproducible harness:

```bash
cd /workspace/projects/VibeCoding/gemini-module
python3 .codex/gemini-bloom-audit/v6/evidence/stage07/stage07_fuzz_fault_safety.py   --redis-server /workspace/projects/Environments/OpenSourceRedis/redis-6.2-redisbloom/bin/redis-server   --gemini-module /tmp/gemini-module-v6-stage05-build-workaround/redis_bloom.so   --seed 2970124295   --output .codex/gemini-bloom-audit/v6/evidence/stage07/stage07_fuzz_results.json
```

Minimized rows are in `failure_rows.json`:

- `expansion_over_limit`, `expansion_uint_max`: mutate only `WireFilterHeader.expansionFactor`; `BF.LOADCHUNK key 1 <header>` returns `OK` despite DESIGN max `32768`.
- `cursor_skip_to_final_exposes_false_negatives`: create source with multiple layers, load only header into a new key, then load the final layer cursor/data. Loading clears, `BF.CARD` reports 20, but only 5/20 inserted items are found.
- `repeat_first_chunk_for_all_layers`: create expansion=1 source, load header, replay the first same-sized layer chunk for every cursor. Loading clears, `BF.CARD` reports 20, but 15/20 inserted items are false negatives.
- `persist_half_loaded_rdb`: load only header, verify commands return `ERR filter is being loaded`, run `SAVE`, restart. The key loads as normal `MBbloom--`, `BF.CARD` reports 40, but 40/40 inserted items are false negatives.
- `persist_half_loaded_aof_no_preamble`: same half-loaded key, run successful `BGREWRITEAOF`, restart from command AOF. The key loads as normal `MBbloom--`, `BF.CARD` reports 40, but 40/40 inserted items are false negatives.

No Redis process crash was observed; these are data-integrity/safety failures, not crash repros.
