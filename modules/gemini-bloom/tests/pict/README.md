# gemini-bloom PICT tests

This directory contains PICT models and a Python runner for pairwise
gemini-bloom integration coverage. Generated case files, reports, Redis logs,
PICT sources, and PICT binaries are written under `build/` and are not tracked.

## Bootstrap PICT

Docker mode builds PICT inside the project container:

```bash
./tools/pict/bootstrap_pict.sh --mode docker --container 974d83bcff5c
```

The Docker binary is written to `build/tools/pict/docker/bin/pict`.

Host mode builds PICT in the local checkout:

```bash
./tools/pict/bootstrap_pict.sh --mode host
```

The host binary is written to `build/tools/pict/host/bin/pict`, so host and
Docker builds can coexist in the same checkout.

The bootstrap uses `PICT_REF=v3.7.4` by default. Override `PICT_REF` or
`PICT_REPO` if the project needs to test another Microsoft PICT tag or branch.

## Run

Build the module in Docker first:

```bash
docker exec 974d83bcff5c bash -lc 'cd /workspace/projects/VibeCoding/gemini-module && cmake -B build && cmake --build build -j$(nproc)'
```

Run all pairwise models:

```bash
./tools/pict/run_gemini_bloom_pict.sh --mode docker --container 974d83bcff5c --module ./build/redis_bloom.so --model all
```

Run a smoke subset:

```bash
./tools/pict/run_gemini_bloom_pict.sh --mode docker --container 974d83bcff5c --module ./build/redis_bloom.so --model reserve --order 2
```

Generate cases only on macOS:

```bash
./tools/pict/run_gemini_bloom_pict.sh --mode host --generate-only --model all
```

## Models

- `reserve.pict`: `BF.RESERVE` parameter and key-state combinations.
- `insert.pict`: `BF.INSERT` option order, item list, and key-state combinations.
- `commands.pict`: `BF.ADD`, `BF.EXISTS`, `BF.MADD`, `BF.MEXISTS`, `BF.INFO`, and `BF.CARD`.
- `loadchunk.pict`: `BF.SCANDUMP` and `BF.LOADCHUNK` protocol and malformed input coverage.
- `module-args.pict`: `ERROR_RATE`, `INITIAL_SIZE`, and `EXPANSION` load arguments.
- `protocol.pict`: RESP2/RESP3 coverage for boolean-style commands.
- `persistence.pict`: small RDB/AOF restart matrix.

## Reports

The runner writes:

- `build/pict/gemini-bloom/report.json`
- `build/pict/gemini-bloom/report.txt`

Failures include the model, case id, PICT row, Redis command, expected result,
actual reply or error, and Redis log path.

## What to commit

Commit the PICT models, scripts, runner, README, and `.gitignore` changes.
Do not commit generated TSV files, reports, PICT source code, PICT binaries,
Redis logs, RDB files, or AOF files.
