#!/usr/bin/env bash
set -euo pipefail

mode="docker"
container="${GEMINI_MODULE_CONTAINER:-974d83bcff5c}"
repo_in_container="${GEMINI_MODULE_REPO_IN_CONTAINER:-/workspace/projects/VibeCoding/gemini-module}"
pict_repo="${PICT_REPO:-https://github.com/microsoft/pict.git}"
pict_ref="${PICT_REF:-v3.7.4}"

usage() {
  cat <<'USAGE'
Usage: tools/pict/bootstrap_pict.sh [--mode docker|host] [--container ID] [--repo-in-container PATH]

Builds Microsoft PICT into build/tools/pict/bin/pict.

Modes:
  docker  Run this bootstrap inside the configured Docker container.
  host    Build in the current checkout on the host.

Environment overrides:
  PICT_REPO  Git repository to clone. Default: https://github.com/microsoft/pict.git
  PICT_REF   Git tag or branch to checkout. Default: v3.7.4
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

  local src_dir="build/tools/pict/src"
  local build_dir="build/tools/pict/build"
  local bin_dir="build/tools/pict/bin"

  mkdir -p "$(dirname "$src_dir")" "$build_dir" "$bin_dir"

  if [[ ! -d "$src_dir/.git" ]]; then
    rm -rf "$src_dir"
    git clone --depth 1 --branch "$pict_ref" "$pict_repo" "$src_dir"
  else
    git -C "$src_dir" fetch --depth 1 origin "$pict_ref"
    git -C "$src_dir" checkout --detach FETCH_HEAD
  fi

  cmake -S "$src_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$build_dir" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"

  local built
  built="$(find "$build_dir" -type f \( -name pict -o -name pict.exe \) -perm -111 | head -n 1 || true)"
  if [[ -z "$built" ]]; then
    built="$(find "$build_dir" -type f \( -name pict -o -name pict.exe \) | head -n 1 || true)"
  fi
  if [[ -z "$built" ]]; then
    echo "Could not find built pict executable under $build_dir" >&2
    exit 1
  fi

  cp "$built" "$bin_dir/pict"
  chmod +x "$bin_dir/pict"
  "$bin_dir/pict" /? >/dev/null 2>&1 || true
  echo "PICT built at $root/$bin_dir/pict"
}

case "$mode" in
  host)
    run_host
    ;;
  docker)
    cmd="cd $(quote "$repo_in_container") && PICT_REPO=$(quote "$pict_repo") PICT_REF=$(quote "$pict_ref") tools/pict/bootstrap_pict.sh --mode host"
    docker exec "$container" bash -lc "$cmd"
    ;;
  *)
    echo "Invalid --mode: $mode" >&2
    usage >&2
    exit 2
    ;;
esac
