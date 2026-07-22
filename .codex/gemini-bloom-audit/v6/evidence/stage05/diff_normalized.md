# Stage 05 RedisBloom v2.4.20 Normalized Diff

- Environment: `redis-6.2.17-redisbloom-v2.4.20`
- Exact RedisBloom oracle: Redis 6.2.17 + RedisBloom v2.4.20 (`MODULE LIST ver=20420`).
- Gemini module for runtime diff used audit-only `-include climits` build workaround because default Linux/GCC build fails.

## Command-Level Differences

| Area | Gemini actual | RedisBloom v2.4.20 actual | Classification | Evidence |
|---|---|---|---|---|
| MODULE LIST | `[[{"bytes": 4, "hex": "6e616d65"}, {"bytes": 11, "hex": "47656d696e69426c6f6f6d"}, {"bytes": 3, "hex": "766572"}, 1]]` | `[[{"bytes": 4, "hex": "6e616d65"}, {"bytes": 2, "hex": "6266"}, {"bytes": 3, "hex": "766572"}, 20420]]` | DESIGN_INTENDED | `diff_raw_resp.log#module-list` |
| BF.INFO key FIELD CAPACITY | `100` | `[100]` | DESIGN_INTENDED | `diff_raw_resp.log#info-capacity` |
| BF.INFO key FIELD SIZE | `440` | `[240]` | DESIGN_INTENDED | `diff_raw_resp.log#info-size` |
| BF.INSERT NOCREATE CAPACITY | `{"error": "ERR NOCREATE cannot be used with CAPACITY or ERROR"}` | `{"error": "ERR not found"}` | DESIGN_INTENDED | `diff_raw_resp.log#insert-nocreate-capacity` |
| BF.MADD fixed partial failure | `[1, 1, {"error": "ERR non scaling filter is full"}]` | `[1, 1, {"error": "ERR non scaling filter is full"}]` | PASS | `oracle_diff/compat_matrix_results_redis62_redisbloom2420.json` |
| BF.INSERT fixed partial failure | `[1, 1, {"error": "ERR non scaling filter is full"}]` | `[1, 1, {"error": "ERR non scaling filter is full"}]` | PASS | `oracle_diff/compat_matrix_results_redis62_redisbloom2420.json` |
| BF.DEBUG | `{"error": "ERR unknown command `BF.DEBUG`, with args beginning with: `debug_key`, "}` | `[{"bytes": 6, "hex": "73697a653a31"}, {"bytes": 70, "hex": "62797465733a313620626974733a31323820686173686573..."}]` | DESIGN_INTENDED | `oracle_diff/extended_audit_results_redis62_redisbloom2420.json` |

## Extended Audit Summary

```json
{
  "bf_debug": {
    "gemini": false,
    "redisbloom": true
  },
  "incremental_aof": {
    "error": 0,
    "fail": 2,
    "pass": 4
  },
  "loadchunk_header_over_existing_key": {
    "gemini": {
      "error": "ERR received bad data"
    },
    "redisbloom": {
      "error": "ERR received bad data"
    }
  },
  "migrate_and_dump_restore_ttl": {
    "error": 0,
    "fail": 0,
    "pass": 2
  },
  "readonly_scandump_ok": {
    "gemini": true,
    "redisbloom": true
  }
}
```

## Build Finding

Default Linux/GCC build of current gemini module failed because `modules/gemini-bloom/src/bloom_rdb.cc` uses `UINT_MAX` without including `<climits>`. Runtime oracle used an audit-only `-include climits` workaround. Evidence: `oracle_diff/gemini_default_build_stderr.log`.
