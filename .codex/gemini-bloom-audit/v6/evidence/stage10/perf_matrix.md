# Stage 10 PERF_RESOURCE Matrix

Bounded audit samples only; these are not formal benchmark results.

| ID | Area | Status | Evidence | Notes |
|---|---|---|---|---|
| RL-01 | resource_limits | PASS | `resource_limits.log` | capacity 0 must reject before allocation |
| RL-02a | resource_limits | PASS | `resource_limits.log` | minimum capacity accepted |
| RL-02b | resource_limits | PASS | `resource_limits.log` | minimum capacity add |
| RL-02c | resource_limits | PASS | `resource_limits.log` | minimum capacity exists |
| RL-03-10 | resource_limits | PASS | `resource_limits.log` | capacity 10 accepted |
| RL-03-100 | resource_limits | PASS | `resource_limits.log` | capacity 100 accepted |
| RL-03-10000 | resource_limits | PASS | `resource_limits.log` | capacity 10000 accepted |
| RL-03-100000 | resource_limits | PASS | `resource_limits.log` | capacity 100000 accepted |
| RL-04 | resource_limits | PASS | `resource_limits.log` | capacity 2^30 high-error NONSCALING safety probe |
| RL-05 | resource_limits | PASS | `resource_limits.log` | capacity above DESIGN max must reject before allocation |
| RL-06-1 | resource_limits | PASS | `resource_limits.log` | expansion 1 accepted |
| RL-06-2 | resource_limits | PASS | `resource_limits.log` | expansion 2 accepted |
| RL-06-4 | resource_limits | PASS | `resource_limits.log` | expansion 4 accepted |
| RL-06-32768 | resource_limits | PASS | `resource_limits.log` | expansion 32768 accepted |
| RL-07 | resource_limits | PASS | `resource_limits.log` | expansion above DESIGN max must reject |
| RL-08a | resource_limits | PASS | `resource_limits.log` | EXPANSION 0 maps to non-scaling in BF.RESERVE |
| RL-08b | resource_limits | PASS | `resource_limits.log` | EXPANSION 0 maps to non-scaling in BF.INSERT |
| RL-09 | resource_limits | PASS | `resource_limits.log` | tiny error with max capacity should hit per-layer cap without allocation |
| RL-10 | resource_limits | PASS | `resource_limits.log` | bitsPerEntry > 1000 should reject gracefully |
| RL-11a | resource_limits | PASS | `resource_limits.log` | prepare fixed-size filter |
| RL-11b | resource_limits | PASS | `resource_limits.log` | MADD should truncate at first full error |
| RL-11c | resource_limits | PASS | `resource_limits.log` | cardinality must stay at capacity |
| RL-11d | resource_limits | PASS | `resource_limits.log` | later item after full error should not be processed |
| RL-11e | resource_limits | PASS | `resource_limits.log` | BF.INSERT fixed filter should truncate at first full error |
| LAT-01 | latency_samples | PASS | `latency_samples.csv` | bounded latency audit sample recorded |
| LAT-02 | latency_samples | PASS | `latency_samples.csv` | bounded latency audit sample recorded |
| LAT-03 | latency_samples | PASS | `latency_samples.csv` | bounded latency audit sample recorded |
| LAT-04 | latency_samples | PASS | `latency_samples.csv` | bounded latency audit sample recorded |
| LAT-05 | latency_samples | PASS | `latency_samples.csv` | bounded latency audit sample recorded |
| MEM-01 | memory_usage | PASS | `memory_usage.md` | empty reserved capacity 100 filter |
| MEM-02 | memory_usage | PASS | `memory_usage.md` | same filter after inserts below capacity |
| MEM-03 | memory_usage | PASS | `memory_usage.md` | multi-layer growth capacity 10 expansion 2 |
| MEM-04 | memory_usage | PASS | `memory_usage.md` | expansion 1 many-layer case |
| MEM-05-10000 | memory_usage | PASS | `memory_usage.md` | capacity 10000 memory accounting |
| MEM-05-100000 | memory_usage | PASS | `memory_usage.md` | capacity 100000 memory accounting |
| MEM-06 | memory_usage | PASS | `memory_usage.md` | capacity 2^30 high-error NONSCALING boundary |
| SLC-01 | scandump_loadchunk | PASS | `scandump_loadchunk.md` | private cursor sequence and ordered same-module replay |
| SLC-02 | scandump_loadchunk | PASS | `scandump_loadchunk.md` | private cursor sequence and ordered same-module replay |
| SLC-03 | scandump_loadchunk | PASS | `scandump_loadchunk.md` | private cursor sequence and ordered same-module replay |
| SLC-04 | scandump_loadchunk | PASS | `scandump_loadchunk.md` | private cursor sequence and ordered same-module replay |
| PA-01 | persistence_size | PASS | `persistence_size.md` | RDB size after moderate filters and large item hashing |
| PA-02 | persistence_size | PASS | `persistence_size.md` | AOF RDB-preamble size recorded |
| PA-02-RESTART | persistence_size | PASS | `persistence_size.md` | same-module AOF RDB-preamble restart sanity check |
| PA-03 | persistence_size | NOT_VERIFIED | `blocked_or_not_verified.md` | command-AOF aof-use-rdb-preamble no not rerun in Stage 10 |
| PA-04 | persistence_size | PASS | `persistence_size.md` | RDB/AOF preamble stores bit arrays, not raw 1MB item payloads |
