#!/usr/bin/env python3
"""Exercise single Bloom keys with tens and hundreds of millions of items."""

import json
import os
import shutil
import signal
import struct
import sys
import tempfile
import time

from bloom_test_support import info_dict, start_redis, stop_redis, wait_for_bgsave


KEY_SIZES = tuple(
    int(value)
    for value in os.environ.get("BLOOM_HUGE_KEY_SIZES", "10000000,100000000").split(",")
)
ERROR_RATE = float(os.environ.get("BLOOM_HUGE_ERROR_RATE", "0.001"))
WRITE_BATCH = int(os.environ.get("BLOOM_HUGE_WRITE_BATCH", "10000"))
READ_BATCH = int(os.environ.get("BLOOM_HUGE_READ_BATCH", "20000"))
MISSING_READS = int(os.environ.get("BLOOM_HUGE_MISSING_READS", "1000000"))
RESTART_SAMPLE_STEP = int(os.environ.get("BLOOM_HUGE_RESTART_SAMPLE_STEP", "1000"))
PROGRESS_EVERY = int(os.environ.get("BLOOM_HUGE_PROGRESS_EVERY", "5000000"))


def item(value):
    return struct.pack(">Q", value)


def batched_values(start, stop, batch_size):
    for offset in range(start, stop, batch_size):
        end = min(offset + batch_size, stop)
        yield offset, [item(value) for value in range(offset, end)]


def progress(label, completed, total, started):
    if completed == total or completed % PROGRESS_EVERY < WRITE_BATCH:
        elapsed = time.monotonic() - started
        rate = completed / elapsed if elapsed else 0
        print(
            f"  {label}: {completed:,}/{total:,} "
            f"({rate:,.0f} items/s, {elapsed:.1f}s)",
            flush=True,
        )


def write_all(conn, key, count):
    started = time.monotonic()
    inserted = 0
    for offset, values in batched_values(0, count, WRITE_BATCH):
        result = conn.execute_command("BF.MADD", key, *values)
        if len(result) != len(values):
            raise AssertionError(
                f"{key!r}: short BF.MADD reply at {offset}: "
                f"{len(result)} != {len(values)}"
            )
        inserted += sum(result)
        progress("write", offset + len(values), count, started)
    return inserted, time.monotonic() - started


def verify_all(conn, key, count):
    started = time.monotonic()
    checked = 0
    for offset, values in batched_values(0, count, READ_BATCH):
        result = conn.execute_command("BF.MEXISTS", key, *values)
        if result != [1] * len(values):
            first = next(index for index, value in enumerate(result) if value != 1)
            raise AssertionError(f"{key!r}: false negative at item {offset + first}")
        checked += len(values)
        progress("read", checked, count, started)
    return time.monotonic() - started


def count_false_positives(conn, key, count):
    start = 1 << 63
    false_positives = 0
    for _, values in batched_values(start, start + count, READ_BATCH):
        false_positives += sum(conn.execute_command("BF.MEXISTS", key, *values))
    return false_positives


def verify_restart_sample(conn, key, count):
    values = [item(value) for value in range(0, count, RESTART_SAMPLE_STEP)]
    if not values or values[-1] != item(count - 1):
        values.append(item(count - 1))
    for offset in range(0, len(values), READ_BATCH):
        batch = values[offset:offset + READ_BATCH]
        result = conn.execute_command("BF.MEXISTS", key, *batch)
        if result != [1] * len(batch):
            raise AssertionError(f"{key!r}: false negative in restart sample")
    return len(values)


def logical_snapshot(conn, key):
    raw = info_dict(conn.execute_command("BF.INFO", key))
    return {
        "capacity": int(raw[b"Capacity"]),
        "filters": int(raw[b"Number of filters"]),
        "inserted": int(raw[b"Number of items inserted"]),
        "expansion": raw[b"Expansion rate"],
        "size": int(raw[b"Size"]),
        "memory_usage": int(conn.execute_command("MEMORY", "USAGE", key)),
    }


def run_implementation(module_path, label):
    directory = tempfile.mkdtemp(prefix=f"bloom-huge-{label}-")
    proc = None
    results = []
    try:
        proc, conn, _ = start_redis(os.path.abspath(module_path), directory)
        for count in KEY_SIZES:
            key = f"huge:{count}".encode()
            print(f"\n--- {label}: capacity/items={count:,} ---", flush=True)
            conn.execute_command("BF.RESERVE", key, ERROR_RATE, count, "NONSCALING")
            returned_inserted, write_seconds = write_all(conn, key, count)
            read_seconds = verify_all(conn, key, count)
            false_positives = count_false_positives(conn, key, MISSING_READS)
            snapshot = logical_snapshot(conn, key)
            if snapshot["capacity"] != count or snapshot["filters"] != 1:
                raise AssertionError(f"{label}: unexpected BF.INFO: {snapshot!r}")
            if snapshot["inserted"] != returned_inserted:
                raise AssertionError(
                    f"{label}: BF.INFO inserted={snapshot['inserted']} but "
                    f"BF.MADD returned {returned_inserted} inserts"
                )
            result = {
                "attempted_items": count,
                "inserted_items": returned_inserted,
                "write_seconds": round(write_seconds, 3),
                "read_seconds": round(read_seconds, 3),
                "write_items_per_second": round(count / write_seconds),
                "read_items_per_second": round(count / read_seconds),
                "missing_reads": MISSING_READS,
                "false_positives": false_positives,
                **snapshot,
            }
            results.append(result)
            print(json.dumps(result, sort_keys=True), flush=True)

        print(f"\n--- {label}: BGSAVE and restart ---", flush=True)
        wait_for_bgsave(conn, timeout=300)
        stop_redis(proc, conn)
        proc = None
        proc, conn, _ = start_redis(os.path.abspath(module_path), directory)
        if conn.dbsize() != len(KEY_SIZES):
            raise AssertionError(
                f"{label}: expected {len(KEY_SIZES)} keys after restart, got {conn.dbsize()}"
            )
        for result in results:
            count = result["attempted_items"]
            key = f"huge:{count}".encode()
            sampled = verify_restart_sample(conn, key, count)
            restarted = logical_snapshot(conn, key)
            for field in ("capacity", "filters", "inserted", "expansion"):
                if restarted[field] != result[field]:
                    raise AssertionError(
                        f"{label}/{count}: restart changed {field}: "
                        f"{result[field]!r} -> {restarted[field]!r}"
                    )
            result["restart_sampled_items"] = sampled
            result["restart_size"] = restarted["size"]
            result["restart_memory_usage"] = restarted["memory_usage"]
        return results
    finally:
        if proc is not None:
            stop_redis(proc, conn)
        shutil.rmtree(directory, ignore_errors=True)


def comparable(result):
    return {
        field: result[field]
        for field in (
            "attempted_items",
            "inserted_items",
            "missing_reads",
            "false_positives",
            "capacity",
            "filters",
            "inserted",
            "expansion",
            "restart_sampled_items",
        )
    }


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <gemini_bloom.so> <redisbloom.so>", file=sys.stderr)
        return 2
    if not KEY_SIZES or any(count <= 0 for count in KEY_SIZES):
        raise ValueError("BLOOM_HUGE_KEY_SIZES must contain positive integers")
    if min(WRITE_BATCH, READ_BATCH, MISSING_READS, RESTART_SAMPLE_STEP) <= 0:
        raise ValueError("batch, read, and sample settings must be positive")

    print(
        "Huge-key configuration: "
        f"sizes={KEY_SIZES}, error_rate={ERROR_RATE}, write_batch={WRITE_BATCH}, "
        f"read_batch={READ_BATCH}, missing_reads={MISSING_READS}",
        flush=True,
    )
    gemini = run_implementation(sys.argv[1], "gemini")
    reference = run_implementation(sys.argv[2], "redisbloom")
    for gemini_result, reference_result in zip(gemini, reference):
        if comparable(gemini_result) != comparable(reference_result):
            raise AssertionError(
                "GeminiBloom and RedisBloom logical results differ:\n"
                f"gemini={comparable(gemini_result)!r}\n"
                f"redisbloom={comparable(reference_result)!r}"
            )

    print("\n=== Huge-key comparison ===")
    print(json.dumps({"gemini": gemini, "redisbloom": reference}, indent=2, sort_keys=True))
    print(
        f"PASS: single-key writes, reads, and restart checks for {KEY_SIZES} "
        "match RedisBloom"
    )
    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda _signum, _frame: sys.exit(1))
    raise SystemExit(main())
