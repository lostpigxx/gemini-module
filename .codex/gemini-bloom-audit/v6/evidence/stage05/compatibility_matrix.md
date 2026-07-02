# Stage 05 Compatibility Matrix

- Environment: `redis-6.2.17-redisbloom-v2.4.20`
- Corpora: binary_items, empty_scaling, expansion1, expansion4, fixed_full, large_empty_16mb, long_item, multi_exp2, single_layer
- Raw result JSON: `oracle_diff/compat_matrix_results_redis62_redisbloom2420.json`

| Path family | DESIGN expectation | PASS | FAIL | ERROR | Classification | Evidence |
|---|---|---:|---:|---:|---|---|
| RDB file RedisBloom -> gemini | COMPAT_PROMISED | 9 | 0 | 0 | PASS | `oracle_diff/compat_matrix_results_redis62_redisbloom2420.json` |
| RDB file gemini -> RedisBloom | COMPAT_PROMISED | 9 | 0 | 0 | PASS | `oracle_diff/compat_matrix_results_redis62_redisbloom2420.json` |
| DUMP/RESTORE RedisBloom -> gemini | COMPAT_PROMISED | 9 | 0 | 0 | PASS | `oracle_diff/compat_matrix_results_redis62_redisbloom2420.json` |
| DUMP/RESTORE gemini -> RedisBloom | COMPAT_PROMISED | 9 | 0 | 0 | PASS | `oracle_diff/compat_matrix_results_redis62_redisbloom2420.json` |
| SCANDUMP/LOADCHUNK RedisBloom -> gemini | DESIGN_INTENDED_INCOMPAT | 0 | 9 | 0 | DESIGN_INTENDED | `oracle_diff/compat_matrix_results_redis62_redisbloom2420.json` |
| SCANDUMP/LOADCHUNK gemini -> RedisBloom | DESIGN_INTENDED_INCOMPAT | 0 | 9 | 0 | DESIGN_INTENDED | `oracle_diff/compat_matrix_results_redis62_redisbloom2420.json` |
| command-AOF rewrite RedisBloom -> gemini | DESIGN_INTENDED_INCOMPAT | 0 | 9 | 0 | DESIGN_INTENDED | `oracle_diff/compat_matrix_results_redis62_redisbloom2420.json` |
| command-AOF rewrite gemini -> RedisBloom | DESIGN_INTENDED_INCOMPAT | 0 | 9 | 0 | DESIGN_INTENDED | `oracle_diff/compat_matrix_results_redis62_redisbloom2420.json` |
| RDB-preamble AOF RedisBloom -> gemini | COMPAT_PROMISED | 9 | 0 | 0 | PASS | `oracle_diff/compat_matrix_results_redis62_redisbloom2420.json` |
| RDB-preamble AOF gemini -> RedisBloom | COMPAT_PROMISED | 9 | 0 | 0 | PASS | `oracle_diff/compat_matrix_results_redis62_redisbloom2420.json` |
| live replication command stream RedisBloom -> gemini | KNOWN_LIMIT | 8 | 1 | 0 | DESIGN_INTENDED | `oracle_diff/compat_matrix_results_redis62_redisbloom2420.json` |
| live replication command stream gemini -> RedisBloom | KNOWN_LIMIT | 8 | 1 | 0 | DESIGN_INTENDED | `oracle_diff/compat_matrix_results_redis62_redisbloom2420.json` |
| fullsync replication RedisBloom -> gemini | COMPAT_PROMISED | 9 | 0 | 0 | PASS | `oracle_diff/compat_matrix_results_redis62_redisbloom2420.json` |
| fullsync replication gemini -> RedisBloom | COMPAT_PROMISED | 9 | 0 | 0 | PASS | `oracle_diff/compat_matrix_results_redis62_redisbloom2420.json` |

## Failure Examples For Non-PASS Families

### SCANDUMP/LOADCHUNK RedisBloom -> gemini

- `binary_items`: check={"card": {"error": "ERR filter is being loaded"}, "expected_card": 6, "expected_items": 6, "found": 0, "missing_count": 6, "missing_samples": ["0:", "1:6e756c3a003a696e73696465", "2:63726c663a0d0a3a696e73696465", "3:737061636520616e6420746162096974", "4:74636c3a7b6c6973747d5c6368617273"]}, load_replies=["OK", {"error": "ERR cursor exceeds layer count"}], critical_log_count=None
- `empty_scaling`: check={"card": {"error": "ERR filter is being loaded"}, "expected_card": 0, "expected_items": 0, "found": 0, "missing_count": 0, "missing_samples": []}, load_replies=["OK", {"error": "ERR cursor exceeds layer count"}], critical_log_count=None
- `expansion1`: check={"card": {"error": "ERR filter is being loaded"}, "expected_card": 19, "expected_items": 20, "found": 0, "missing_count": 20, "missing_samples": ["0:657870313a30", "1:657870313a31", "2:657870313a32", "3:657870313a33", "4:657870313a34"]}, load_replies=["OK", {"error": "ERR cursor exceeds layer count"}, {"error": "ERR cursor exceeds layer count"}, {"error": "ERR cursor exceeds layer count"}, {"error": "ERR cursor exceeds layer count"}], critical_log_count=None

### SCANDUMP/LOADCHUNK gemini -> RedisBloom

- `binary_items`: check={"card": 6, "expected_card": 6, "expected_items": 6, "found": 0, "missing_count": 6, "missing_samples": ["0:", "1:6e756c3a003a696e73696465", "2:63726c663a0d0a3a696e73696465", "3:737061636520616e6420746162096974", "4:74636c3a7b6c6973747d5c6368617273"]}, load_replies=["OK", {"error": "ERR received bad data"}], critical_log_count=None
- `empty_scaling`: check={"card": 0, "expected_card": 0, "expected_items": 0, "found": 0, "missing_count": 0, "missing_samples": []}, load_replies=["OK", {"error": "ERR received bad data"}], critical_log_count=None
- `expansion1`: check={"card": 20, "expected_card": 20, "expected_items": 20, "found": 0, "missing_count": 20, "missing_samples": ["0:657870313a30", "1:657870313a31", "2:657870313a32", "3:657870313a33", "4:657870313a34"]}, load_replies=["OK", {"error": "ERR received bad data"}, {"error": "ERR received bad data"}, {"error": "ERR received bad data"}, {"error": "ERR received bad data"}], critical_log_count=None

### command-AOF rewrite RedisBloom -> gemini

- `binary_items`: check={"card": {"error": "ERR filter is being loaded"}, "expected_card": 6, "expected_items": 6, "found": 0, "missing_count": 6, "missing_samples": ["0:", "1:6e756c3a003a696e73696465", "2:63726c663a0d0a3a696e73696465", "3:737061636520616e6420746162096974", "4:74636c3a7b6c6973747d5c6368617273"]}, load_replies=null, critical_log_count=1
- `empty_scaling`: check={"card": {"error": "ERR filter is being loaded"}, "expected_card": 0, "expected_items": 0, "found": 0, "missing_count": 0, "missing_samples": []}, load_replies=null, critical_log_count=1
- `expansion1`: check={"card": {"error": "ERR filter is being loaded"}, "expected_card": 19, "expected_items": 20, "found": 0, "missing_count": 20, "missing_samples": ["0:657870313a30", "1:657870313a31", "2:657870313a32", "3:657870313a33", "4:657870313a34"]}, load_replies=null, critical_log_count=4

### command-AOF rewrite gemini -> RedisBloom

- `binary_items`: check={"card": 6, "expected_card": 6, "expected_items": 6, "found": 0, "missing_count": 6, "missing_samples": ["0:", "1:6e756c3a003a696e73696465", "2:63726c663a0d0a3a696e73696465", "3:737061636520616e6420746162096974", "4:74636c3a7b6c6973747d5c6368617273"]}, load_replies=null, critical_log_count=1
- `empty_scaling`: check={"card": 0, "expected_card": 0, "expected_items": 0, "found": 0, "missing_count": 0, "missing_samples": []}, load_replies=null, critical_log_count=1
- `expansion1`: check={"card": 20, "expected_card": 20, "expected_items": 20, "found": 0, "missing_count": 20, "missing_samples": ["0:657870313a30", "1:657870313a31", "2:657870313a32", "3:657870313a33", "4:657870313a34"]}, load_replies=null, critical_log_count=4

### live replication command stream RedisBloom -> gemini

- `expansion1`: check={"card": 20, "expected_card": 19, "expected_items": 20, "found": 20, "missing_count": 0, "missing_samples": []}, load_replies=null, critical_log_count=0

### live replication command stream gemini -> RedisBloom

- `expansion1`: check={"card": 19, "expected_card": 20, "expected_items": 20, "found": 20, "missing_count": 0, "missing_samples": []}, load_replies=null, critical_log_count=0

## MIGRATE / TTL

Extended audit `migrate_and_dump_restore_ttl`: {"error": 0, "fail": 0, "pass": 2}. Evidence: `oracle_diff/extended_audit_results_redis62_redisbloom2420.json`.
