# Stage 11 DESIGN Alignment Summary

| DESIGN claim/difference | Final classification | Evidence |
|---|---|---|
| Not RedisBloom drop-in | DESIGN_BOUNDARY | `.codex/gemini-bloom-audit/v6/agents/stage00/design_contract.md` |
| RDB-family RedisBloom v2.4.20 compatibility | PASS within exact scope | `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md`, `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |
| SCANDUMP/LOADCHUNK RedisBloom non-interoperability | DESIGN_INTENDED | `.codex/gemini-bloom-audit/v6/evidence/stage06/scandump_loadchunk/summary.md` |
| command-AOF no-preamble cross incompatibility | DESIGN_INTENDED | `.codex/gemini-bloom-audit/v6/evidence/stage06/aof_preamble_no/summary.md` |
| RESP3 unsupported | DESIGN_INTENDED | `.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/tcl_summary.md` |
| BF.DEBUG unsupported | DESIGN_INTENDED | `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_normalized.md` |
| Command resource bounds | PASS for command path | `.codex/gemini-bloom-audit/v6/evidence/stage10/resource_limits.log` |
| RDB/wire untrusted input resource bounds | FAIL for per-layer cap and expansion | `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md`, `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md` |
| Loading-state integrity | FAIL for abnormal/persisted paths | `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md`, `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/loading_partial_fullsync.log` |
