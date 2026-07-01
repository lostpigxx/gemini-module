# Compatibility Matrix Template

| Area | Expected by DESIGN.md | Gemini actual | RedisBloom v2.4.20 actual | Verdict | Evidence | Notes |
|---|---|---|---|---|---|---|
| RDB gemini -> RedisBloom | compatible |  |  | PENDING |  |  |
| RDB RedisBloom -> gemini | compatible |  |  | PENDING |  |  |
| DUMP/RESTORE gemini -> RedisBloom | compatible |  |  | PENDING |  |  |
| DUMP/RESTORE RedisBloom -> gemini | compatible |  |  | PENDING |  |  |
| MIGRATE | compatible |  |  | PENDING |  |  |
| fullsync replication | compatible |  |  | PENDING |  |  |
| RDB-preamble AOF | compatible |  |  | PENDING |  |  |
| command-AOF rewrite cross-impl | not compatible |  |  | PENDING |  | DESIGN_INTENDED if confirmed |
| SCANDUMP/LOADCHUNK cross-impl | not compatible |  |  | PENDING |  | DESIGN_INTENDED if confirmed |
| RESP3 | not supported |  |  | PENDING |  | DESIGN_INTENDED if RESP2 shape retained |
| BF.INFO single field shape | known difference |  |  | PENDING |  |  |
| BF.INSERT EXPANSION 0 | known difference |  |  | PENDING |  |  |
