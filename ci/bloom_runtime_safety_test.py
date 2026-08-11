#!/usr/bin/env python3
"""Focused Redis integration workload for sanitizer and Valgrind runs."""

import argparse
import concurrent.futures
import os
import shutil
import tempfile

import redis

from bloom_persistence_test import (
    info_dict,
    start_redis,
    stop_redis,
    wait_for_aof_rewrite,
    wait_for_bgsave,
)


def client_for(conn):
    kwargs = conn.connection_pool.connection_kwargs
    return redis.Redis(
        host=kwargs["host"],
        port=kwargs["port"],
        decode_responses=False,
        protocol=2,
    )


def assert_members(conn, key, items, label):
    for offset in range(0, len(items), 500):
        batch = items[offset:offset + 500]
        result = conn.execute_command("BF.MEXISTS", key, *batch)
        if result != [1] * len(batch):
            missing = sum(value != 1 for value in result)
            raise AssertionError(f"{label}: {missing} false negatives")


def exercise(conn):
    shared_key = b"safety:shared"
    conn.execute_command("BF.RESERVE", shared_key, 0.001, 20000, "NONSCALING")

    workers = 6
    per_worker = 1000

    def writer(worker_id):
        worker = client_for(conn)
        items = [f"worker:{worker_id}:{i}".encode() for i in range(per_worker)]
        for offset in range(0, len(items), 100):
            worker.execute_command("BF.MADD", shared_key, *items[offset:offset + 100])
        return items

    all_items = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        for items in pool.map(writer, range(workers)):
            all_items.extend(items)
    assert_members(conn, shared_key, all_items, "concurrent MADD")

    binary_key = b"safety:binary"
    binary_items = [b"", b"nul\x00value", b"line\r\nvalue", bytes(range(256))]
    conn.execute_command("BF.RESERVE", binary_key, 0.01, 100)
    conn.execute_command("BF.MADD", binary_key, *binary_items)
    assert_members(conn, binary_key, binary_items, "binary values")

    scaling_key = b"safety:scaling"
    # Ten same-sized layers exercise layer-array relocation without reaching the
    # implementation's minimum per-layer false-positive-rate guard.
    conn.execute_command("BF.RESERVE", scaling_key, 0.01, 100, "EXPANSION", 1)
    scaling_items = [f"scale:{i}".encode() for i in range(1000)]
    for offset in range(0, len(scaling_items), 100):
        replies = conn.execute_command(
            "BF.MADD", scaling_key, *scaling_items[offset:offset + 100]
        )
        errors = [reply for reply in replies if isinstance(reply, redis.ResponseError)]
        if errors:
            raise AssertionError(f"multi-layer scaling write failed: {errors[0]}")
    assert_members(conn, scaling_key, scaling_items, "multi-layer scaling")

    for index in range(300):
        key = f"safety:churn:{index}".encode()
        conn.execute_command("BF.RESERVE", key, 0.01, 100)
        conn.execute_command("BF.MADD", key, b"a", b"b", b"c")
    conn.delete(*[f"safety:churn:{index}".encode() for index in range(300)])

    info = info_dict(conn.execute_command("BF.INFO", shared_key))
    if info.get(b"Number of filters", 0) != 1:
        raise AssertionError(f"unexpected shared filter metadata: {info!r}")

    return {
        "shared_key": shared_key,
        "shared_sample": all_items[::97],
        "binary_key": binary_key,
        "binary_items": binary_items,
        "scaling_key": scaling_key,
        "scaling_sample": scaling_items[::29],
    }


def verify_after_restart(conn, expected):
    assert_members(conn, expected["shared_key"], expected["shared_sample"], "restart shared")
    assert_members(conn, expected["binary_key"], expected["binary_items"], "restart binary")
    assert_members(conn, expected["scaling_key"], expected["scaling_sample"], "restart scaling")


def check_log(logfile, label, require_valgrind_summary):
    with open(logfile, errors="replace") as log:
        content = log.read()
    bad_markers = (
        "ERROR: AddressSanitizer",
        "SUMMARY: AddressSanitizer",
        "runtime error:",
        "WARNING: ThreadSanitizer",
        "SUMMARY: ThreadSanitizer",
    )
    for marker in bad_markers:
        if marker in content:
            raise AssertionError(f"{label}: detected {marker!r}; see {logfile}")
    if require_valgrind_summary and "ERROR SUMMARY: 0 errors" not in content:
        raise AssertionError(f"{label}: missing clean Valgrind summary; see {logfile}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("module")
    parser.add_argument("--label", required=True)
    parser.add_argument("--preload")
    parser.add_argument("--valgrind", action="store_true")
    args = parser.parse_args()

    module_path = os.path.abspath(args.module)
    directory = tempfile.mkdtemp(prefix=f"bloom-safety-{args.label}-")
    process_env = os.environ.copy()
    if args.preload:
        process_env["LD_PRELOAD"] = args.preload

    command_prefix = None
    if args.valgrind:
        command_prefix = [
            "valgrind",
            "--leak-check=full",
            "--show-leak-kinds=definite",
            "--errors-for-leak-kinds=definite",
            "--error-exitcode=86",
        ]

    redis_args = [
        "--appendonly", "yes",
        "--appendfilename", "appendonly.aof",
        "--aof-use-rdb-preamble", "no",
    ]
    proc = None
    logfiles = []
    try:
        proc, conn, logfile = start_redis(
            module_path,
            directory,
            redis_args,
            process_env=process_env,
            command_prefix=command_prefix,
        )
        logfiles.append(logfile)
        expected = exercise(conn)
        # Redis forks for BGSAVE/BGREWRITEAOF do not complete reliably under
        # Memcheck. Sanitizer runs cover those forked paths; Valgrind still
        # validates live commands plus command-AOF replay on restart.
        if not args.valgrind:
            wait_for_bgsave(conn)
            wait_for_aof_rewrite(conn)
        stop_redis(proc, conn)
        if proc.returncode != 0:
            raise AssertionError(f"{args.label}: redis-server exited with {proc.returncode}")
        proc = None
        check_log(logfile, args.label, args.valgrind)

        proc, conn, logfile = start_redis(
            module_path,
            directory,
            redis_args,
            process_env=process_env,
            command_prefix=command_prefix,
        )
        logfiles.append(logfile)
        verify_after_restart(conn, expected)
        stop_redis(proc, conn)
        if proc.returncode != 0:
            raise AssertionError(f"{args.label} restart: redis-server exited with {proc.returncode}")
        proc = None
        check_log(logfile, f"{args.label} restart", args.valgrind)
    finally:
        if proc is not None:
            stop_redis(proc, conn)
        if os.environ.get("KEEP_SAFETY_WORKDIR"):
            print(f"Kept safety workdir: {directory}")
        else:
            shutil.rmtree(directory, ignore_errors=True)

    print(f"PASS: {args.label} runtime safety workload")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
