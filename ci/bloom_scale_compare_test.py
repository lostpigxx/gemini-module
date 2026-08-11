#!/usr/bin/env python3
"""Large key-count and large-filter comparison against RedisBloom."""

import os
import shutil
import signal
import sys
import tempfile
import time
import traceback

import redis

from bloom_persistence_test import info_dict, start_redis, stop_redis, wait_for_bgsave


KEY_COUNT = int(os.environ.get("BLOOM_SCALE_KEY_COUNT", "20000"))
BIG_ITEMS = int(os.environ.get("BLOOM_SCALE_BIG_ITEMS", "1000000"))
MISS_READS = int(os.environ.get("BLOOM_SCALE_MISS_READS", "100000"))
BATCH_SIZE = 500

LOGICAL_INFO_FIELDS = (
    b"Capacity",
    b"Number of filters",
    b"Number of items inserted",
    b"Expansion rate",
)


def big_item(index):
    return f"large-item:{index:010d}".encode()


def miss_item(index):
    return f"definitely-missing:{index:010d}".encode()


def populate_many_keys(conn):
    pipe = conn.pipeline(transaction=False)
    pending = 0
    for index in range(KEY_COUNT):
        key = f"many:{index:08d}".encode()
        pipe.execute_command("BF.RESERVE", key, 0.01, 100)
        pipe.execute_command("BF.MADD", key, b"alpha", b"beta", str(index).encode())
        pending += 2
        if pending >= 500:
            pipe.execute()
            pipe = conn.pipeline(transaction=False)
            pending = 0
    if pending:
        pipe.execute()


def populate_big_key(conn):
    conn.execute_command("BF.RESERVE", b"large:filter", 0.001, BIG_ITEMS, "NONSCALING")
    for offset in range(0, BIG_ITEMS, BATCH_SIZE):
        items = [big_item(index) for index in range(offset, min(offset + BATCH_SIZE, BIG_ITEMS))]
        conn.execute_command("BF.MADD", b"large:filter", *items)


def verify_many_keys(conn, label):
    if conn.dbsize() != KEY_COUNT + 1:
        raise AssertionError(f"{label}: expected {KEY_COUNT + 1} keys, got {conn.dbsize()}")
    for base in range(0, KEY_COUNT, 500):
        stop = min(base + 500, KEY_COUNT)
        pipe = conn.pipeline(transaction=False)
        for index in range(base, stop):
            key = f"many:{index:08d}".encode()
            pipe.execute_command("BF.MEXISTS", key, b"alpha", b"beta", str(index).encode())
        for index, result in zip(range(base, stop), pipe.execute()):
            if result != [1, 1, 1]:
                key = f"many:{index:08d}".encode()
                raise AssertionError(f"{label}: false negative for {key!r}")


def verify_big_key(conn, label):
    checked = 0
    for offset in range(0, BIG_ITEMS, BATCH_SIZE):
        items = [big_item(index) for index in range(offset, min(offset + BATCH_SIZE, BIG_ITEMS))]
        result = conn.execute_command("BF.MEXISTS", b"large:filter", *items)
        if result != [1] * len(items):
            raise AssertionError(f"{label}: false negative in large key near offset {offset}")
        checked += len(items)

    false_positives = 0
    for offset in range(0, MISS_READS, BATCH_SIZE):
        items = [miss_item(index) for index in range(offset, min(offset + BATCH_SIZE, MISS_READS))]
        result = conn.execute_command("BF.MEXISTS", b"large:filter", *items)
        false_positives += sum(result)

    return checked, false_positives


def logical_info(conn):
    raw = info_dict(conn.execute_command("BF.INFO", b"large:filter"))
    return {field: raw.get(field) for field in LOGICAL_INFO_FIELDS}


def optional_card(conn):
    try:
        return conn.execute_command("BF.CARD", b"large:filter")
    except redis.ResponseError as exc:
        if "unknown command" in str(exc).lower():
            return None
        raise


def run_implementation(module_path, label):
    directory = tempfile.mkdtemp(prefix=f"bloom-scale-{label}-")
    proc = None
    started = time.monotonic()
    try:
        proc, conn, _ = start_redis(module_path, directory)
        populate_many_keys(conn)
        populate_big_key(conn)
        verify_many_keys(conn, f"{label} before restart")
        checked, false_positives = verify_big_key(conn, f"{label} before restart")
        before_info = logical_info(conn)
        card = optional_card(conn)
        memory = conn.info("memory").get("used_memory")
        wait_for_bgsave(conn)
        stop_redis(proc, conn)
        proc = None

        proc, conn, _ = start_redis(module_path, directory)
        verify_many_keys(conn, f"{label} after restart")
        checked_after, false_positives_after = verify_big_key(conn, f"{label} after restart")
        after_info = logical_info(conn)
        if before_info != after_info:
            raise AssertionError(
                f"{label}: logical BF.INFO changed after restart: "
                f"{before_info!r} != {after_info!r}"
            )
        if false_positives != false_positives_after:
            raise AssertionError(
                f"{label}: missing-item results changed after restart: "
                f"{false_positives} != {false_positives_after}"
            )
        elapsed = time.monotonic() - started
        return {
            "keys": conn.dbsize(),
            "checked_hits": checked + checked_after,
            "false_positives": false_positives,
            "info": before_info,
            "card": card,
            "used_memory": memory,
            "elapsed_seconds": round(elapsed, 3),
        }
    finally:
        if proc is not None:
            stop_redis(proc, conn)
        shutil.rmtree(directory, ignore_errors=True)


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <gemini_bloom.so> <redisbloom.so>", file=sys.stderr)
        return 2

    print(
        f"Scale configuration: keys={KEY_COUNT}, big_items={BIG_ITEMS}, "
        f"missing_reads={MISS_READS}"
    )
    failures = []
    results = {}
    for label, module_path in (("gemini", sys.argv[1]), ("redisbloom", sys.argv[2])):
        print(f"\n--- {label} scale workload ---")
        try:
            results[label] = run_implementation(os.path.abspath(module_path), label)
            print(f"PASS: {label}: {results[label]}")
        except Exception as exc:
            failures.append((label, str(exc)))
            traceback.print_exc()

    if not failures:
        gemini = results["gemini"]
        reference = results["redisbloom"]
        comparable_fields = ("keys", "checked_hits", "false_positives", "info")
        mismatches = {
            field: (gemini[field], reference[field])
            for field in comparable_fields
            if gemini[field] != reference[field]
        }
        if gemini["card"] is not None and reference["card"] is not None:
            if gemini["card"] != reference["card"]:
                mismatches["card"] = (gemini["card"], reference["card"])
        if mismatches:
            failures.append(("comparison", repr(mismatches)))

    print(f"\nScale comparison: {2 - len([f for f in failures if f[0] != 'comparison'])}/2 implementations passed")
    if failures:
        for label, error in failures:
            print(f"FAIL: {label}: {error}", file=sys.stderr)
        return 1
    print("PASS: GeminiBloom and RedisBloom scale results match")
    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda _signum, _frame: sys.exit(1))
    raise SystemExit(main())
