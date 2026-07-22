# Stage 10 Persistence Size Evidence

- module artifact: `/tmp/gemini-module-v6-stage10-build-workaround/redis_bloom.so`
- SAVE reply: `OK`
- RDB dump.rdb size: `72473` bytes
- CONFIG SET aof-use-rdb-preamble yes: `OK`
- CONFIG SET appendonly yes: `OK`
- BGREWRITEAOF reply: `Background append only file rewriting started`

## Persistence Info After Rewrite

- `rdb_changes_since_last_save`: `0`
- `rdb_bgsave_in_progress`: `0`
- `rdb_last_save_time`: `1782908993`
- `rdb_last_bgsave_status`: `ok`
- `rdb_last_bgsave_time_sec`: `-1`
- `rdb_current_bgsave_time_sec`: `-1`
- `rdb_last_cow_size`: `0`
- `aof_enabled`: `1`
- `aof_rewrite_in_progress`: `0`
- `aof_rewrite_scheduled`: `0`
- `aof_last_rewrite_time_sec`: `0`
- `aof_current_rewrite_time_sec`: `-1`
- `aof_last_bgrewrite_status`: `ok`
- `aof_last_write_status`: `ok`
- `aof_last_cow_size`: `1204224`
- `aof_current_size`: `72473`
- `aof_base_size`: `72473`
- `aof_pending_rewrite`: `0`
- `aof_buffer_length`: `0`
- `aof_rewrite_buffer_length`: `0`
- `aof_pending_bio_fsync`: `0`
- `aof_delayed_fsync`: `0`

## File Sizes

- `appendonly.aof`: `72473` bytes
- `dump.rdb`: `72473` bytes

## Restart Checks

- `pa_cap_100_card`: `100`
- `pa_large_10kb_exists`: `1`
- `pa_large_1mb_exists`: `1`

Command-AOF with `aof-use-rdb-preamble no` was not rerun in Stage 10; Stage 06 covers the same-module/design boundary.
