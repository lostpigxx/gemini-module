#!/usr/bin/env python3
"""Quantify GeminiBloom and RedisBloom memory cost across key and layer shapes."""

import json
import os
import shutil
import signal
import sys
import tempfile

from bloom_test_support import info_dict, start_redis, stop_redis, wait_for_bgsave


CHECKPOINTS = tuple(
    int(value) for value in os.environ.get(
        "BLOOM_MEMORY_KEY_COUNTS", "1000,5000,20000,50000"
    ).split(",")
)
SMALL_CAPACITY = int(os.environ.get("BLOOM_MEMORY_SMALL_CAPACITY", "100"))
CAPACITY_CURVE = (1, 100, 1000, 10000, 100000, 1000000)
BATCH_KEYS = 250
MAX_EXTRA_BYTES_PER_KEY = float(
    os.environ.get("BLOOM_MEMORY_MAX_EXTRA_BYTES_PER_KEY", "64")
)

LOGICAL_INFO_FIELDS = (
    b"Capacity",
    b"Number of filters",
    b"Number of items inserted",
    b"Expansion rate",
)


def memory_snapshot(conn):
    memory = conn.info("memory")
    return {
        "used_memory": int(memory["used_memory"]),
        "used_memory_dataset": int(memory.get("used_memory_dataset", 0)),
        "used_memory_overhead": int(memory.get("used_memory_overhead", 0)),
        "allocator_allocated": int(memory.get("allocator_allocated", 0)),
        "allocator_active": int(memory.get("allocator_active", 0)),
        "allocator_resident": int(memory.get("allocator_resident", 0)),
    }


def bloom_snapshot(conn, key):
    info = info_dict(conn.execute_command("BF.INFO", key))
    logical = {
        field.decode(): info.get(field)
        for field in LOGICAL_INFO_FIELDS
    }
    return {
        "logical": logical,
        "bf_info_size": int(info[b"Size"]),
        "memory_usage": int(conn.execute_command("MEMORY", "USAGE", key)),
    }


def add_small_keys(conn, start, stop):
    pipe = conn.pipeline(transaction=False)
    pending = 0
    for index in range(start, stop):
        key = f"memory:small:{index:08d}".encode()
        pipe.execute_command("BF.RESERVE", key, 0.01, SMALL_CAPACITY)
        pipe.execute_command("BF.MADD", key, b"alpha", b"beta", str(index).encode())
        pending += 2
        if pending >= BATCH_KEYS * 2:
            pipe.execute()
            pipe = conn.pipeline(transaction=False)
            pending = 0
    if pending:
        pipe.execute()


def verify_small_keys(conn, count, label):
    for base in range(0, count, 500):
        stop = min(base + 500, count)
        pipe = conn.pipeline(transaction=False)
        for index in range(base, stop):
            key = f"memory:small:{index:08d}".encode()
            pipe.execute_command("BF.MEXISTS", key, b"alpha", b"beta", str(index).encode())
        for index, result in zip(range(base, stop), pipe.execute()):
            if result != [1, 1, 1]:
                raise AssertionError(f"{label}: false negative at small key {index}: {result!r}")


def run_cardinality(module_path, label):
    directory = tempfile.mkdtemp(prefix=f"bloom-memory-cardinality-{label}-")
    proc = None
    try:
        proc, conn, _ = start_redis(module_path, directory)
        baseline = memory_snapshot(conn)
        points = []
        previous = 0
        for count in CHECKPOINTS:
            if count <= previous:
                raise ValueError("BLOOM_MEMORY_KEY_COUNTS must be strictly increasing")
            add_small_keys(conn, previous, count)
            current = memory_snapshot(conn)
            sample = bloom_snapshot(conn, b"memory:small:00000000")
            points.append({
                "keys": count,
                "used_memory": current["used_memory"],
                "used_memory_delta": current["used_memory"] - baseline["used_memory"],
                "dataset_memory": current["used_memory_dataset"],
                "dataset_memory_delta": (
                    current["used_memory_dataset"] - baseline["used_memory_dataset"]
                ),
                "sample": sample,
            })
            previous = count

        verify_small_keys(conn, CHECKPOINTS[-1], f"{label} before restart")
        wait_for_bgsave(conn)
        stop_redis(proc, conn)
        proc = None

        proc, conn, _ = start_redis(module_path, directory)
        if conn.dbsize() != CHECKPOINTS[-1]:
            raise AssertionError(
                f"{label}: expected {CHECKPOINTS[-1]} keys after restart, got {conn.dbsize()}"
            )
        verify_small_keys(conn, CHECKPOINTS[-1], f"{label} after restart")
        restarted_memory = memory_snapshot(conn)
        restarted = {
            "keys": CHECKPOINTS[-1],
            "used_memory": restarted_memory["used_memory"],
            "used_memory_delta_vs_empty_baseline": (
                restarted_memory["used_memory"] - baseline["used_memory"]
            ),
            "dataset_memory": restarted_memory["used_memory_dataset"],
            "dataset_memory_delta_vs_empty_baseline": (
                restarted_memory["used_memory_dataset"] - baseline["used_memory_dataset"]
            ),
            "sample": bloom_snapshot(conn, b"memory:small:00000000"),
        }
        return {"baseline": baseline, "fresh": points, "restarted": restarted}
    finally:
        if proc is not None:
            stop_redis(proc, conn)
        shutil.rmtree(directory, ignore_errors=True)


def run_shape_curves(module_path, label):
    directory = tempfile.mkdtemp(prefix=f"bloom-memory-shapes-{label}-")
    proc = None
    try:
        proc, conn, _ = start_redis(module_path, directory)
        capacities = []
        for capacity in CAPACITY_CURVE:
            key = f"memory:capacity:{capacity}".encode()
            conn.execute_command("BF.RESERVE", key, 0.01, capacity, "NONSCALING")
            conn.execute_command("BF.MADD", key, b"alpha", b"beta", b"gamma")
            capacities.append({"capacity": capacity, **bloom_snapshot(conn, key)})

        layers = []
        for item_count in (0, 100, 500, 1000):
            key = f"memory:layers:{item_count}".encode()
            conn.execute_command("BF.RESERVE", key, 0.01, 100, "EXPANSION", 1)
            items = [f"layer-item:{item_count}:{index}".encode() for index in range(item_count)]
            for offset in range(0, item_count, 100):
                conn.execute_command("BF.MADD", key, *items[offset:offset + 100])
            if items:
                result = conn.execute_command("BF.MEXISTS", key, *items)
                if result != [1] * len(items):
                    raise AssertionError(f"{label}: layer curve false negative at {item_count}")
            layers.append({"attempted_items": item_count, **bloom_snapshot(conn, key)})
        return {"capacities": capacities, "layers": layers}
    finally:
        if proc is not None:
            stop_redis(proc, conn)
        shutil.rmtree(directory, ignore_errors=True)


def slope(points, field):
    xs = [point["keys"] for point in points]
    ys = [point[field] for point in points]
    x_mean = sum(xs) / len(xs)
    y_mean = sum(ys) / len(ys)
    denominator = sum((value - x_mean) ** 2 for value in xs)
    return sum((x - x_mean) * (y - y_mean) for x, y in zip(xs, ys)) / denominator


def compare(results):
    gemini = results["gemini"]
    reference = results["redisbloom"]
    for gemini_point, reference_point in zip(
        gemini["cardinality"]["fresh"], reference["cardinality"]["fresh"]
    ):
        if gemini_point["keys"] != reference_point["keys"]:
            raise AssertionError("cardinality checkpoints differ")
        if gemini_point["sample"]["logical"] != reference_point["sample"]["logical"]:
            raise AssertionError("small-key logical metadata differs")

    gemini_slope = slope(gemini["cardinality"]["fresh"], "used_memory_delta")
    reference_slope = slope(reference["cardinality"]["fresh"], "used_memory_delta")
    final_count = CHECKPOINTS[-1]
    fresh_gemini = gemini["cardinality"]["fresh"][-1]
    fresh_reference = reference["cardinality"]["fresh"][-1]
    restarted_gemini = gemini["cardinality"]["restarted"]
    restarted_reference = reference["cardinality"]["restarted"]
    return {
        "fresh_used_memory_bytes_per_key": {
            "gemini": round(gemini_slope, 3),
            "redisbloom": round(reference_slope, 3),
            "gemini_extra": round(gemini_slope - reference_slope, 3),
        },
        "fresh_total_extra_bytes_at_max_keys": (
            fresh_gemini["used_memory_delta"] - fresh_reference["used_memory_delta"]
        ),
        "fresh_sample_bf_info_size_difference": (
            fresh_gemini["sample"]["bf_info_size"]
            - fresh_reference["sample"]["bf_info_size"]
        ),
        "restart_total_extra_bytes_at_max_keys": (
            restarted_gemini["used_memory_delta_vs_empty_baseline"]
            - restarted_reference["used_memory_delta_vs_empty_baseline"]
        ),
        "restart_sample_bf_info_size_difference": (
            restarted_gemini["sample"]["bf_info_size"]
            - restarted_reference["sample"]["bf_info_size"]
        ),
        "max_keys": final_count,
    }


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <gemini_bloom.so> <redisbloom.so>", file=sys.stderr)
        return 2

    results = {}
    for label, module_path in (("gemini", sys.argv[1]), ("redisbloom", sys.argv[2])):
        module_path = os.path.abspath(module_path)
        print(f"\n--- {label}: key-cardinality profile ---", flush=True)
        cardinality = run_cardinality(module_path, label)
        print(json.dumps(cardinality, sort_keys=True), flush=True)
        print(f"\n--- {label}: capacity/layer profile ---", flush=True)
        shapes = run_shape_curves(module_path, label)
        print(json.dumps(shapes, sort_keys=True), flush=True)
        results[label] = {"cardinality": cardinality, "shapes": shapes}

    analysis = compare(results)
    print("\n--- comparison ---")
    print(json.dumps(analysis, indent=2, sort_keys=True))
    extra = analysis["fresh_used_memory_bytes_per_key"]["gemini_extra"]
    if extra > MAX_EXTRA_BYTES_PER_KEY:
        raise AssertionError(
            f"Gemini uses {extra:.3f} extra bytes per small key; "
            f"limit is {MAX_EXTRA_BYTES_PER_KEY:.3f}"
        )
    print("PASS: memory profile completed with matching logical data")
    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda _signum, _frame: sys.exit(1))
    raise SystemExit(main())
