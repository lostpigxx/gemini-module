#!/usr/bin/env python3
"""maxmemory, OOM, and eviction-policy invariants for module Bloom keys."""

import os
import shutil
import signal
import sys
import tempfile
import traceback

import redis

from bloom_persistence_test import info_dict, start_redis, stop_redis


FILTER_CAPACITY = int(os.environ.get("BLOOM_EVICTION_FILTER_CAPACITY", "20000"))
FILL_KEYS = int(os.environ.get("BLOOM_EVICTION_FILL_KEYS", "600"))
MEMORY_BUDGET = int(os.environ.get("BLOOM_EVICTION_BUDGET_MB", "4")) * 1024 * 1024

ALLKEYS_POLICIES = ("allkeys-lru", "allkeys-lfu", "allkeys-random")
VOLATILE_POLICIES = ("volatile-lru", "volatile-lfu", "volatile-random", "volatile-ttl")
LOGICAL_INFO_FIELDS = (
    b"Capacity",
    b"Number of filters",
    b"Number of items inserted",
    b"Expansion rate",
)


def logical_info(conn, key):
    raw = info_dict(conn.execute_command("BF.INFO", key))
    return {field: raw.get(field) for field in LOGICAL_INFO_FIELDS}


def create_pressure_dataset(conn, volatile):
    baseline = conn.info("memory")["used_memory"]
    sentinel = b"eviction:persistent-sentinel"
    conn.execute_command("BF.RESERVE", sentinel, 0.01, 100)
    conn.execute_command("BF.ADD", sentinel, b"keep-me")

    keys = []
    for index in range(FILL_KEYS):
        key = f"eviction:filter:{index:06d}".encode()
        conn.execute_command("BF.RESERVE", key, 0.01, FILTER_CAPACITY, "NONSCALING")
        conn.execute_command("BF.MADD", key, b"seed", str(index).encode())
        if volatile:
            ttl = 600 + (index % 300)
            if not conn.expire(key, ttl):
                raise AssertionError(f"failed to set TTL on {key!r}")
        keys.append(key)

    used = conn.info("memory")["used_memory"]
    target = baseline + MEMORY_BUDGET
    if used <= target + MEMORY_BUDGET:
        raise AssertionError(
            f"pressure dataset too small: baseline={baseline}, used={used}, target={target}"
        )
    return baseline, target, sentinel, keys


def validate_remaining_filters(conn, keys, label):
    remaining = 0
    for key in keys:
        if not conn.exists(key):
            continue
        try:
            contains_seed = conn.execute_command("BF.EXISTS", key, b"seed")
            info = logical_info(conn, key)
        except redis.ResponseError as exc:
            # Redis can continue evicting between the EXISTS and BF.INFO commands.
            # A key disappearing during validation is a valid eviction, not corruption.
            if "not found" in str(exc).lower() and not conn.exists(key):
                continue
            raise
        if not conn.exists(key):
            continue
        remaining += 1
        if contains_seed != 1:
            raise AssertionError(f"{label}: surviving key {key!r} has a false negative")
        if info[b"Number of filters"] != 1:
            raise AssertionError(f"{label}: corrupt metadata for {key!r}: {info!r}")
    return remaining


def run_eviction_policy(module_path, implementation, policy):
    directory = tempfile.mkdtemp(prefix=f"bloom-eviction-{implementation}-{policy}-")
    proc = None
    try:
        proc, conn, _ = start_redis(module_path, directory)
        volatile = policy.startswith("volatile-")
        _, target, sentinel, keys = create_pressure_dataset(conn, volatile)
        conn.config_set("maxmemory-policy", policy)
        conn.config_set("maxmemory-samples", 10)
        conn.config_set("maxmemory", target)

        conn.set("eviction:trigger", b"1")
        stats = conn.info("stats")
        evicted = stats.get("evicted_keys", 0)
        if evicted <= 0:
            raise AssertionError(f"{implementation}/{policy}: no keys were evicted")

        remaining = validate_remaining_filters(conn, keys, f"{implementation}/{policy}")
        if remaining >= len(keys):
            raise AssertionError(f"{implementation}/{policy}: all pressure keys survived")
        if volatile:
            if conn.execute_command("BF.EXISTS", sentinel, b"keep-me") != 1:
                raise AssertionError(f"{implementation}/{policy}: persistent sentinel was evicted")
            if conn.ttl(sentinel) != -1:
                raise AssertionError(f"{implementation}/{policy}: sentinel unexpectedly gained a TTL")

        return {
            "policy": policy,
            "created": len(keys),
            "remaining": remaining,
            "evicted_keys": evicted,
            "used_memory": conn.info("memory")["used_memory"],
            "maxmemory": target,
        }
    finally:
        if proc is not None:
            stop_redis(proc, conn)
        shutil.rmtree(directory, ignore_errors=True)


def expect_oom(call, label):
    try:
        call()
    except redis.ResponseError as exc:
        if "oom" not in str(exc).lower() and "maxmemory" not in str(exc).lower():
            raise AssertionError(f"{label}: expected OOM, got {exc!r}") from exc
        return str(exc)
    raise AssertionError(f"{label}: command unexpectedly succeeded above maxmemory")


def run_noeviction(module_path, implementation):
    directory = tempfile.mkdtemp(prefix=f"bloom-noeviction-{implementation}-")
    proc = None
    try:
        proc, conn, _ = start_redis(module_path, directory)
        _, target, sentinel, keys = create_pressure_dataset(conn, volatile=False)
        protected = keys[0]
        before = logical_info(conn, protected)
        conn.config_set("maxmemory-policy", "noeviction")
        conn.config_set("maxmemory", target)

        rejected_key = b"oom:must-not-exist"
        reserve_error = expect_oom(
            lambda: conn.execute_command("BF.RESERVE", rejected_key, 0.01, 100),
            f"{implementation}/noeviction BF.RESERVE",
        )
        if conn.exists(rejected_key):
            raise AssertionError(f"{implementation}/noeviction: rejected key was created")

        add_error = expect_oom(
            lambda: conn.execute_command("BF.ADD", protected, b"must-not-be-added"),
            f"{implementation}/noeviction BF.ADD",
        )
        after = logical_info(conn, protected)
        if before != after:
            raise AssertionError(
                f"{implementation}/noeviction: rejected write changed metadata: {before!r} != {after!r}"
            )
        if conn.execute_command("BF.EXISTS", protected, b"seed") != 1:
            raise AssertionError(f"{implementation}/noeviction: existing data became unreadable")
        if conn.execute_command("BF.EXISTS", sentinel, b"keep-me") != 1:
            raise AssertionError(f"{implementation}/noeviction: sentinel became unreadable")
        if conn.info("stats").get("evicted_keys", 0) != 0:
            raise AssertionError(f"{implementation}/noeviction: keys were unexpectedly evicted")

        return {
            "policy": "noeviction",
            "reserve_error": reserve_error,
            "add_error": add_error,
            "metadata_unchanged": True,
            "existing_data_readable": True,
        }
    finally:
        if proc is not None:
            stop_redis(proc, conn)
        shutil.rmtree(directory, ignore_errors=True)


def run_implementation(module_path, implementation):
    results = [run_noeviction(module_path, implementation)]
    for policy in ALLKEYS_POLICIES + VOLATILE_POLICIES:
        results.append(run_eviction_policy(module_path, implementation, policy))
    return results


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <gemini_bloom.so> <redisbloom.so>", file=sys.stderr)
        return 2

    print(
        f"Eviction configuration: filters={FILL_KEYS}, capacity={FILTER_CAPACITY}, "
        f"budget={MEMORY_BUDGET} bytes"
    )
    failures = []
    for implementation, module_path in (("gemini", sys.argv[1]), ("redisbloom", sys.argv[2])):
        print(f"\n--- {implementation} maxmemory matrix ---")
        try:
            results = run_implementation(os.path.abspath(module_path), implementation)
            for result in results:
                print(f"PASS: {implementation}: {result}")
        except Exception as exc:
            failures.append((implementation, str(exc)))
            traceback.print_exc()

    if failures:
        for implementation, error in failures:
            print(f"FAIL: {implementation}: {error}", file=sys.stderr)
        return 1
    print("\nPASS: GeminiBloom and RedisBloom satisfy the same maxmemory invariants")
    print("NOTE: exact eviction counts are intentionally not compared because memory accounting differs")
    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda _signum, _frame: sys.exit(1))
    raise SystemExit(main())
