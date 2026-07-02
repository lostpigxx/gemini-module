# Stage 10 BLOCKED / NOT_VERIFIED

- `PA-03` `NOT_VERIFIED`: command-AOF aof-use-rdb-preamble no not rerun in Stage 10 (evidence: `blocked_or_not_verified.md`)
- `RL-04-default-error` `NOT_VERIFIED`: `capacity=2^30` with default/low error rate was intentionally not allocated because it can require GB-scale memory; Stage 10 used the high-error safety probe instead.
- `SLC-redis-bloom-interop` `DESIGN_INTENDED`: RedisBloom SCANDUMP/LOADCHUNK byte-offset compatibility was not tested as a PASS criterion because DESIGN defines gemini's private layer cursor protocol.
