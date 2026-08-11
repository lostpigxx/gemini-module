#!/usr/bin/env bash
set -euo pipefail

IMAGE=${1:?Usage: run_bloom_oom_matrix.sh <docker-image>}

run_case() {
  local label=$1
  local module=$2
  local memory=$3
  local expected_status=$4

  printf '\n--- %s: memory=%s, expected rewrite=%s ---\n' \
    "$label" "$memory" "$expected_status"
  docker run --rm \
    --memory="$memory" \
    --memory-swap="$memory" \
    --entrypoint python3 \
    "$IMAGE" \
    ci/bloom_aof_oom_test.py "$module" \
    --require-rewrite-status "$expected_status"
}

printf '\n========== AOF rewrite: forced cgroup OOM containment (%s) ==========\n' "$IMAGE"
run_case GeminiBloom ./build/redis_bloom.so 64m err
run_case RedisBloom /opt/redisbloom.so 64m err

printf '\n========== AOF rewrite: constrained success (%s) ==========\n' "$IMAGE"
run_case GeminiBloom ./build/redis_bloom.so 128m ok
run_case RedisBloom /opt/redisbloom.so 128m ok
