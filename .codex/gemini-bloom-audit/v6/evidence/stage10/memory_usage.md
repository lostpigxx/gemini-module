# Stage 10 Memory Usage Evidence

These values are allocator- and environment-dependent audit samples.

## MEM-01: mem_empty100

- notes: empty reserved capacity 100 filter
- Redis MEMORY USAGE: `488`
- OS RSS KB: `9384`
- INFO memory subset:

  - `used_memory`: `5551128`
  - `used_memory_rss`: `9465856`
  - `mem_fragmentation_ratio`: `2.07`

- BF.INFO:

  - `Capacity`: `100`
  - `Size`: `440`
  - `Number of filters`: `1`
  - `Number of items inserted`: `0`
  - `Expansion rate`: `2`

## MEM-02: mem_empty100

- notes: same filter after inserts below capacity
- Redis MEMORY USAGE: `488`
- OS RSS KB: `9384`
- INFO memory subset:

  - `used_memory`: `5551128`
  - `used_memory_rss`: `9465856`
  - `mem_fragmentation_ratio`: `2.07`

- BF.INFO:

  - `Capacity`: `100`
  - `Size`: `440`
  - `Number of filters`: `1`
  - `Number of items inserted`: `50`
  - `Expansion rate`: `2`

## MEM-03: mem_exp2_growth

- notes: multi-layer growth capacity 10 expansion 2
- Redis MEMORY USAGE: `728`
- OS RSS KB: `9384`
- INFO memory subset:

  - `used_memory`: `5551936`
  - `used_memory_rss`: `9465856`
  - `mem_fragmentation_ratio`: `2.07`

- BF.INFO:

  - `Capacity`: `70`
  - `Size`: `680`
  - `Number of filters`: `3`
  - `Number of items inserted`: `40`
  - `Expansion rate`: `2`

## MEM-04: mem_exp1_many

- notes: expansion 1 many-layer case
- Redis MEMORY USAGE: `1624`
- OS RSS KB: `9384`
- INFO memory subset:

  - `used_memory`: `5553680`
  - `used_memory_rss`: `9465856`
  - `mem_fragmentation_ratio`: `2.07`

- BF.INFO:

  - `Capacity`: `80`
  - `Size`: `1576`
  - `Number of filters`: `8`
  - `Number of items inserted`: `80`
  - `Expansion rate`: `1`

## MEM-05-10000: mem_cap_10000

- notes: capacity 10000 memory accounting
- Redis MEMORY USAGE: `14136`
- OS RSS KB: `9384`
- INFO memory subset:

  - `used_memory`: `5567880`
  - `used_memory_rss`: `9609216`
  - `mem_fragmentation_ratio`: `1.73`

- BF.INFO:

  - `Capacity`: `10000`
  - `Size`: `14088`
  - `Number of filters`: `1`
  - `Number of items inserted`: `100`
  - `Expansion rate`: `2`

## MEM-05-100000: mem_cap_100000

- notes: capacity 100000 memory accounting
- Redis MEMORY USAGE: `138192`
- OS RSS KB: `9512`
- INFO memory subset:

  - `used_memory`: `5706128`
  - `used_memory_rss`: `9609216`
  - `mem_fragmentation_ratio`: `1.73`

- BF.INFO:

  - `Capacity`: `100000`
  - `Size`: `138144`
  - `Number of filters`: `1`
  - `Number of items inserted`: `100`
  - `Expansion rate`: `2`

## MEM-06: rl_cap_max_safe

- notes: capacity 2^30 high-error NONSCALING boundary
- Redis MEMORY USAGE: `2807976`
- OS RSS KB: `9512`
- INFO memory subset:

  - `used_memory`: `5706128`
  - `used_memory_rss`: `9609216`
  - `mem_fragmentation_ratio`: `1.73`

- BF.INFO:

  - `Capacity`: `1073741824`
  - `Size`: `2807928`
  - `Number of filters`: `1`
  - `Number of items inserted`: `0`
  - `Expansion rate`: `None`

