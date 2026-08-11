#!/usr/bin/env python3
"""Validate command-AOF rewrite under a finite Docker cgroup memory limit."""

import argparse
import os
import shutil
import sys
import tempfile
import time

from bloom_test_support import (
    start_redis,
    stop_redis,
)


LARGE_CAPACITY = int(os.environ.get("BLOOM_OOM_LARGE_CAPACITY", "10000000"))
LARGE_WRITES = int(os.environ.get("BLOOM_OOM_LARGE_WRITES", "100000"))


def cgroup_memory_limit():
    candidates = (
        "/sys/fs/cgroup/memory.max",
        "/sys/fs/cgroup/memory/memory.limit_in_bytes",
    )
    for path in candidates:
        try:
            with open(path) as handle:
                raw = handle.read().strip()
        except OSError:
            continue
        if raw and raw != "max":
            return int(raw)
    raise RuntimeError("test must run inside a Docker container with a finite memory limit")


def cgroup_memory_current():
    for path in (
        "/sys/fs/cgroup/memory.current",
        "/sys/fs/cgroup/memory/memory.usage_in_bytes",
    ):
        try:
            with open(path) as handle:
                return int(handle.read().strip())
        except OSError:
            pass
    return None


def populate(conn):
    expected = {}
    for index in range(100):
        key = f"oom:small:{index:04d}".encode()
        items = [b"alpha", b"beta", str(index).encode()]
        conn.execute_command("BF.RESERVE", key, 0.01, 100)
        conn.execute_command("BF.MADD", key, *items)
        expected[key] = items

    large_key = b"oom:large"
    conn.execute_command("BF.RESERVE", large_key, 0.001, LARGE_CAPACITY, "NONSCALING")
    large_items = [f"oom-large:{index:08d}".encode() for index in range(LARGE_WRITES)]
    for offset in range(0, len(large_items), 500):
        conn.execute_command("BF.MADD", large_key, *large_items[offset:offset + 500])
    expected[large_key] = large_items[::1000]
    return expected


def verify(conn, expected, label):
    missing_keys = []
    false_negative_keys = []
    for key, items in expected.items():
        if not conn.exists(key):
            missing_keys.append(key)
            continue
        result = conn.execute_command("BF.MEXISTS", key, *items)
        if result != [1] * len(items):
            false_negative_keys.append(key)
    if missing_keys or false_negative_keys:
        raise AssertionError(
            f"{label}: missing_keys={len(missing_keys)}, "
            f"false_negative_keys={len(false_negative_keys)}, "
            f"first_missing={missing_keys[:3]!r}"
        )


def attempt_aof_rewrite(conn, timeout=60):
    conn.execute_command("BGREWRITEAOF")
    deadline = time.monotonic() + timeout
    saw_active = False
    while time.monotonic() < deadline:
        info = conn.info("persistence")
        active = bool(info.get("aof_rewrite_in_progress", 0))
        saw_active = saw_active or active
        if saw_active and not active:
            return info.get("aof_last_bgrewrite_status")
        time.sleep(0.05)
    raise RuntimeError(f"BGREWRITEAOF did not complete within {timeout}s")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("module")
    parser.add_argument("--max-container-memory-mb", type=int, default=256)
    parser.add_argument(
        "--require-rewrite-status", choices=("ok", "err", "either"), default="either"
    )
    args = parser.parse_args()

    limit = cgroup_memory_limit()
    allowed = args.max_container_memory_mb * 1024 * 1024
    if limit > allowed:
        raise RuntimeError(
            f"container limit {limit} exceeds safety ceiling {allowed}; "
            "run with docker --memory and --memory-swap"
        )
    print(f"cgroup memory limit: {limit} bytes")

    directory = tempfile.mkdtemp(prefix="bloom-aof-oom-")
    proc = None
    try:
        redis_args = [
            "--appendonly", "yes",
            "--appendfilename", "appendonly.aof",
            "--appendfsync", "always",
            "--aof-use-rdb-preamble", "no",
        ]
        proc, conn, logfile = start_redis(
            os.path.abspath(args.module),
            directory,
            redis_args,
        )
        expected = populate(conn)
        verify(conn, expected, "before rewrite")
        current = cgroup_memory_current()
        print(
            f"before rewrite: redis_used_memory={conn.info('memory')['used_memory']}, "
            f"cgroup_memory_current={current}, large_capacity={LARGE_CAPACITY}, "
            f"large_writes={LARGE_WRITES}"
        )
        rewrite_status = attempt_aof_rewrite(conn, timeout=60)
        verify(conn, expected, "after rewrite attempt in live server")
        persistence = conn.info("persistence")
        print(
            "rewrite status: "
            f"{persistence.get('aof_last_bgrewrite_status')}, "
            f"aof_size={persistence.get('aof_current_size')}"
        )
        stop_redis(proc, conn)
        if proc.returncode != 0:
            raise AssertionError(f"redis-server exited with {proc.returncode}")
        proc = None

        with open(logfile, errors="replace") as handle:
            first_log = handle.read()

        proc, conn, _ = start_redis(
            os.path.abspath(args.module),
            directory,
            redis_args,
        )
        verify(conn, expected, "after command-AOF rewrite restart")
        if "AOF rewrite rejected an invalid layer count" in first_log:
            raise AssertionError(
                "module logged an omitted AOF key even though restart data survived"
            )
    finally:
        if proc is not None:
            stop_redis(proc, conn)
        if os.environ.get("KEEP_OOM_WORKDIR"):
            print(f"Kept OOM workdir: {directory}")
        else:
            shutil.rmtree(directory, ignore_errors=True)

    if args.require_rewrite_status != "either" and rewrite_status != args.require_rewrite_status:
        raise AssertionError(
            f"expected rewrite status {args.require_rewrite_status!r}, got {rewrite_status!r}"
        )
    if rewrite_status == "ok":
        print("PASS: constrained command-AOF rewrite completed and preserved every Bloom key")
    elif rewrite_status == "err":
        print("PASS: OOM failed the rewrite safely; old AOF preserved every Bloom key")
    else:
        raise AssertionError(f"unexpected AOF rewrite status: {rewrite_status!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
