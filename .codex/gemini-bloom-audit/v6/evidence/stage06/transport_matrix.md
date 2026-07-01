# Stage 06 Transport Matrix

## Verdict

Stage 06 status: PASS. DESIGN-promised persistence/transport paths pass under the exact Redis 6.2.17 + RedisBloom v2.4.20 oracle. DESIGN-private paths are classified as intentional incompatibility, not product bugs, and gemini self-transport passes.

## Raw Evidence

- `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix/cross_impl_transport_results.json`
- `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix/extended_transport_results.json`
- `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix/gemini_self_transport_results.json`
- `.codex/gemini-bloom-audit/v6/evidence/stage06/env_snapshot.txt`
- `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.json`

## Matrix

| Path | Direction | Verdict | Evidence summary |
|---|---|---|---|
| RDB save/restart | gemini -> gemini | PASS | 9/9 self corpora loaded after restart |
| RDB file | RedisBloom -> gemini | PASS | 9/9 corpora, no false negatives |
| RDB file | gemini -> RedisBloom | PASS | 9/9 corpora, no false negatives |
| DUMP/RESTORE | gemini -> gemini | PASS | 9/9 corpora, TTL path exercised |
| DUMP/RESTORE | RedisBloom -> gemini | PASS | 9/9 corpora, TTL verified in extended audit |
| DUMP/RESTORE | gemini -> RedisBloom | PASS | 9/9 corpora, TTL verified in extended audit |
| MIGRATE | bidirectional | PASS | 2/2 directions, PTTL preserved around 600s |
| Fullsync replication | gemini -> gemini | PASS | 9/9 corpora |
| Fullsync replication | RedisBloom -> gemini | PASS | 9/9 corpora |
| Fullsync replication | gemini -> RedisBloom | PASS | 9/9 corpora |
| AOF RDB preamble yes | gemini -> gemini | PASS | 9/9 corpora |
| AOF RDB preamble yes | RedisBloom -> gemini | PASS | 9/9 corpora |
| AOF RDB preamble yes | gemini -> RedisBloom | PASS | 9/9 corpora |
| AOF no preamble | gemini -> gemini | PASS | 9/9 corpora via private LOADCHUNK |
| AOF no preamble | cross implementation | DESIGN_INTENDED | private LOADCHUNK not cross-compatible; non-empty corpora fail cleanly |
| SCANDUMP/LOADCHUNK | gemini -> gemini | PASS | 9/9 corpora plus loading-state safety |
| SCANDUMP/LOADCHUNK | cross implementation | DESIGN_INTENDED | private cursor/chunk protocol rejects or misses items without crash |
| Live command replay | supplemental cross | DESIGN_INTENDED_LIMITATION | expansion1 BF.CARD drift but no inserted-item false negatives |

## Notes

- The gemini audit module is the Stage 05 workaround build (`-include climits`); the default Linux/GCC build failure remains open as `GBV6-05-001`.
- `aof-use-rdb-preamble no` and `BF.SCANDUMP`/`BF.LOADCHUNK` are not cross-implementation compatibility promises in DESIGN.md. Cross failures are expected when payloads are non-empty or RedisBloom leaves a gemini key in loading state.
- `expansion1` live command replay and incremental AOF show BF.CARD differences with no inserted-item false negatives. This matches the DESIGN-known compatibility limit and does not affect RDB/fullsync compatibility.
