#!/usr/bin/env bash
set -euo pipefail

mode="docker"
container="${GEMINI_MODULE_CONTAINER:-974d83bcff5c}"
repo_in_container="${GEMINI_MODULE_REPO_IN_CONTAINER:-/workspace/projects/VibeCoding/gemini-module}"
module="./build/redis_bloom.so"
model="all"
order="2"
keep_cases="false"
generate_only="false"
out_dir="build/pict/gemini-bloom"
pict_bin="build/tools/pict/bin/pict"

usage() {
  cat <<'USAGE'
Usage: tools/pict/run_gemini_bloom_pict.sh [options]

Options:
  --mode docker|host
  --container ID
  --repo-in-container PATH
  --module PATH
  --model all|reserve|insert|commands|loadchunk|module-args|protocol|persistence
  --order 2|3
  --out-dir PATH
  --pict-bin PATH
  --keep-cases
  --generate-only
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      mode="${2:?missing value for --mode}"
      shift 2
      ;;
    --container)
      container="${2:?missing value for --container}"
      shift 2
      ;;
    --repo-in-container)
      repo_in_container="${2:?missing value for --repo-in-container}"
      shift 2
      ;;
    --module)
      module="${2:?missing value for --module}"
      shift 2
      ;;
    --model)
      model="${2:?missing value for --model}"
      shift 2
      ;;
    --order)
      order="${2:?missing value for --order}"
      shift 2
      ;;
    --out-dir)
      out_dir="${2:?missing value for --out-dir}"
      shift 2
      ;;
    --pict-bin)
      pict_bin="${2:?missing value for --pict-bin}"
      shift 2
      ;;
    --keep-cases)
      keep_cases="true"
      shift
      ;;
    --generate-only)
      generate_only="true"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

quote() {
  printf "%q" "$1"
}

run_host() {
  local root
  root="$(git rev-parse --show-toplevel)"
  cd "$root"

  local args=(
    python3 modules/gemini-bloom/tests/pict/run_pict_cases.py
    --pict-bin "$pict_bin"
    --module "$module"
    --model "$model"
    --order "$order"
    --out-dir "$out_dir"
  )
  if [[ "$keep_cases" == "true" ]]; then
    args+=(--keep-cases)
  fi
  if [[ "$generate_only" == "true" ]]; then
    args+=(--generate-only)
  fi

  "${args[@]}"
}

case "$mode" in
  host)
    run_host
    ;;
  docker)
    cmd="cd $(quote "$repo_in_container") && python3 modules/gemini-bloom/tests/pict/run_pict_cases.py --pict-bin $(quote "$pict_bin") --module $(quote "$module") --model $(quote "$model") --order $(quote "$order") --out-dir $(quote "$out_dir")"
    if [[ "$keep_cases" == "true" ]]; then
      cmd+=" --keep-cases"
    fi
    if [[ "$generate_only" == "true" ]]; then
      cmd+=" --generate-only"
    fi
    docker exec "$container" bash -lc "$cmd"
    ;;
  *)
    echo "Invalid --mode: $mode" >&2
    usage >&2
    exit 2
    ;;
esac
