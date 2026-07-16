#!/usr/bin/env bash
set -uo pipefail

green()  { printf '\033[32m%s\033[0m\n' "$*"; }
red()    { printf '\033[31m%s\033[0m\n' "$*"; }
header() { printf '\n\033[1;36m========== %s ==========\033[0m\n' "$*"; }

FAIL=0

for env in redis5 redis6 redis7; do
  header "Building $env"
  if ! docker build -f "Dockerfile.$env" -t "gemini-module:$env" .; then
    red "FAIL: docker build $env"
    FAIL=1
    continue
  fi

  header "Testing $env"
  if ! docker run --rm ${SOAK_DURATION_SEC:+-e SOAK_DURATION_SEC="$SOAK_DURATION_SEC"} "gemini-module:$env"; then
    red "FAIL: tests $env"
    FAIL=1
  else
    green "PASS: $env"
  fi
done

echo
if [ "$FAIL" -ne 0 ]; then
  red "Some environments FAILED"
  exit 1
else
  green "All environments PASSED"
fi
