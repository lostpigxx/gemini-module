# gemini-search Development Plan v8 — Deepening SIMD, Async Concurrency & CJK Semantics (Phases 52–54)

Target platform: **Redis 7** (clean-room C++20).

| Phase | Scope | Status |
|---|---|---|
| **P52** | **SIMD-Accelerated SQ8 Distance Kernels**: ARM NEON (`vmovl_u8`/`vcvtq_f32_u32`/`vfmaq_f32`) and x86-64 AVX2 (`_mm256_cvtepu8_epi32`/`_mm256_fmadd_ps`) for `Sq8Distance` (L2, Cosine, IP) + arbitrary-dimension parity tests + `search_bench` SQ8 benchmark | Done |
| **P53** | **Async Non-Blocking `FT.HYBRID` & `FT.AGGREGATE`**: Extend `SearchExecutor` + `RedisModule_BlockClient` + `shared_mutex` offload to dual-branch `FT.HYBRID` and non-cursor `FT.AGGREGATE` when `doc_store.Size() >= ASYNC_MIN_DOCS` | Pending |
| **P54** | **CJK Custom Dictionary FMM Segmentation**: Forward Maximum Matching (FMM) tokenizer (`TextIndex::TokenizeWithDict`) supporting custom Chinese/CJK domain dictionaries alongside bigram fallback, with GTest + Tcl coverage | Pending |
