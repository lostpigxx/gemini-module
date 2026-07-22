# RDB Payload Matrix

| Case | Status | Classification | Evidence |
|---|---|---|---|
| `existing invalid RDB/unit validation coverage` | PASS_STATIC | STATIC_COVERED | `modules/gemini-bloom/tests/bloom_rdb_test.cc`, `.codex/gemini-bloom-audit/v6/evidence/stage07/static_inspection.log`, `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md` |
| `per_layer_data_size_gt_2gb` | FAIL | FAIL_STATIC_DESIGN_VIOLATION | `modules/gemini-bloom/src/bloom_rdb.cc:53-68 ValidateLayerFields lacks per-layer 2GB cap`, `modules/gemini-bloom/src/bloom_rdb.cc:295-317 wire path only enforces total <=4GB before allocation`, `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md` |
| `expansion_factor_above_kmaxexpansion` | FAIL | FAIL_RUNTIME_CONFIRMED | `Header fuzz rows expansion_over_limit and expansion_uint_max`, `modules/gemini-bloom/src/bloom_rdb.cc:281-305 checks expansion zero but not kMaxExpansion` |
