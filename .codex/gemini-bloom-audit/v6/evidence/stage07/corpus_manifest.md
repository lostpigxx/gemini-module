# Stage 07 Corpus Manifest

## Runtime corpus

- `multi_exp2`: source check `{"card": 40, "expected_items": 40, "found": 40, "missing_count": 0, "missing_samples": []}`, chunks `[[1, 232], [2, 128], [3, 128], [4, 128], [5, 128], [0, 0]]`, num_layers `4`.
- `expansion1`: source check `{"card": 20, "expected_items": 20, "found": 20, "missing_count": 0, "missing_samples": []}`, chunks `[[1, 232], [2, 128], [3, 128], [4, 128], [5, 128], [0, 0]]`, num_layers `4`.

## Random corpus

- Seed: `2970124295` / `0xB100F007`.
- 64 random LOADCHUNK header payloads with fixed size set.

## Static corpus

- RDB/wire metadata classes from Stage 03 findings and Stage 07 planner: per-layer >2GB, total >4GB, invalid expansion, invalid flags, invalid numeric fields.
