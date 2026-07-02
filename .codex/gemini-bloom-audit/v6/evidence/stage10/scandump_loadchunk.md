# Stage 10 SCANDUMP/LOADCHUNK Evidence

Gemini uses the DESIGN private layer-index cursor protocol, not RedisBloom byte-offset chunks.

## SLC-01: slc_single

- copy key: `slc_single_copy`
- capacity: `100`
- expansion: `2`
- inserted recorded: `10`
- source filters: `1`
- copy filters: `1`
- chunks:

  - cursor `0` -> `1`, data_len `73`, elapsed_ms `0.132`
  - cursor `1` -> `2`, data_len `144`, elapsed_ms `0.052`
  - cursor `2` -> `0`, data_len `0`, elapsed_ms `0.059`

- copy sample exists:

  - `slc_single-item-0`: `1`
  - `slc_single-item-1`: `1`
  - `slc_single-item-2`: `1`
  - `slc_single-item-3`: `1`
  - `slc_single-item-4`: `1`

## SLC-02: slc_multi_exp2

- copy key: `slc_multi_exp2_copy`
- capacity: `10`
- expansion: `2`
- inserted recorded: `40`
- source filters: `3`
- copy filters: `3`
- chunks:

  - cursor `0` -> `1`, data_len `179`, elapsed_ms `0.049`
  - cursor `1` -> `2`, data_len `128`, elapsed_ms `0.048`
  - cursor `2` -> `3`, data_len `128`, elapsed_ms `0.048`
  - cursor `3` -> `4`, data_len `128`, elapsed_ms `0.051`
  - cursor `4` -> `0`, data_len `0`, elapsed_ms `0.044`

- copy sample exists:

  - `slc_multi_exp2-item-0`: `1`
  - `slc_multi_exp2-item-1`: `1`
  - `slc_multi_exp2-item-2`: `1`
  - `slc_multi_exp2-item-3`: `1`
  - `slc_multi_exp2-item-4`: `1`

## SLC-03: slc_many_exp1

- copy key: `slc_many_exp1_copy`
- capacity: `10`
- expansion: `1`
- inserted recorded: `80`
- source filters: `8`
- copy filters: `8`
- chunks:

  - cursor `0` -> `1`, data_len `444`, elapsed_ms `0.051`
  - cursor `1` -> `2`, data_len `128`, elapsed_ms `0.051`
  - cursor `2` -> `3`, data_len `128`, elapsed_ms `0.049`
  - cursor `3` -> `4`, data_len `128`, elapsed_ms `0.047`
  - cursor `4` -> `5`, data_len `128`, elapsed_ms `0.048`
  - cursor `5` -> `6`, data_len `128`, elapsed_ms `0.046`
  - cursor `6` -> `7`, data_len `128`, elapsed_ms `0.048`
  - cursor `7` -> `8`, data_len `128`, elapsed_ms `0.048`
  - cursor `8` -> `9`, data_len `128`, elapsed_ms `0.064`
  - cursor `9` -> `0`, data_len `0`, elapsed_ms `0.045`

- copy sample exists:

  - `slc_many_exp1-item-0`: `1`
  - `slc_many_exp1-item-1`: `1`
  - `slc_many_exp1-item-2`: `1`
  - `slc_many_exp1-item-3`: `1`
  - `slc_many_exp1-item-4`: `1`

## SLC-04: slc_large_layer

- copy key: `slc_large_layer_copy`
- capacity: `100000`
- expansion: `2`
- inserted recorded: `100`
- source filters: `1`
- copy filters: `1`
- chunks:

  - cursor `0` -> `1`, data_len `73`, elapsed_ms `0.05`
  - cursor `1` -> `2`, data_len `137848`, elapsed_ms `0.285`
  - cursor `2` -> `0`, data_len `0`, elapsed_ms `0.044`

- copy sample exists:

  - `slc_large_layer-item-0`: `1`
  - `slc_large_layer-item-1`: `1`
  - `slc_large_layer-item-2`: `1`
  - `slc_large_layer-item-3`: `1`
  - `slc_large_layer-item-4`: `1`

