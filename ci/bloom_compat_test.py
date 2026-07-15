#!/usr/bin/env python3
"""
Bloom filter compatibility test: gemini-bloom vs RedisBloom.

Starts two redis-server instances (one with each module), sends identical
BF.* commands to both, and asserts every return value matches.
BF.SCANDUMP and BF.LOADCHUNK are excluded (different serialization format).

Usage:
    python3 bloom_compat_test.py <gemini_bloom.so> <redisbloom.so>
"""

import os
import signal
import socket
import subprocess
import sys
import time

import redis


passed = 0
failed = 0
skipped = 0
failures = []
rb_has_card = True
rb_has_info_single = True


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def start_redis(module_path, port):
    logfile = f"/tmp/redis_compat_{port}.log"
    proc = subprocess.Popen(
        [
            "redis-server",
            "--port", str(port),
            "--bind", "127.0.0.1",
            "--daemonize", "no",
            "--loglevel", "warning",
            "--save", "",
            "--loadmodule", os.path.abspath(module_path),
        ],
        stdout=open(logfile, "w"),
        stderr=subprocess.STDOUT,
    )
    for _ in range(100):
        time.sleep(0.1)
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.close()
            return proc, logfile
        except OSError:
            if proc.poll() is not None:
                with open(logfile) as f:
                    print(f.read(), file=sys.stderr)
                raise RuntimeError(f"redis-server exited with code {proc.returncode}")
    proc.kill()
    raise RuntimeError("redis-server did not start within 10s")


def stop_redis(proc):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def skip(desc):
    global skipped
    skipped += 1
    print(f"  SKIP: {desc}")


def compare(desc, gemini_val, rb_val):
    global passed, failed
    if gemini_val == rb_val:
        passed += 1
        return True
    failed += 1
    msg = f"MISMATCH: {desc}\n  gemini:     {gemini_val!r}\n  redisbloom: {rb_val!r}"
    failures.append(msg)
    print(f"  FAIL: {msg}")
    return False


def compare_both_error(desc, g_conn, rb_conn, *args):
    """Send a command expected to error on both sides. Only checks that both error."""
    g_err = rb_err = None
    try:
        g_conn.execute_command(*args)
    except redis.ResponseError as e:
        g_err = str(e)
    try:
        rb_conn.execute_command(*args)
    except redis.ResponseError as e:
        rb_err = str(e)
    both_errored = (g_err is not None) and (rb_err is not None)
    compare(desc, both_errored, True)


def both(g_conn, rb_conn, *args):
    """Execute command on both, return (gemini_result, rb_result)."""
    g = g_conn.execute_command(*args)
    rb = rb_conn.execute_command(*args)
    return g, rb


def _unwrap_info_single(val):
    """Unwrap BF.INFO single-field response: list→first element, dict→first value."""
    if isinstance(val, list):
        return val[0]
    if isinstance(val, dict):
        return next(iter(val.values()))
    return val


def compare_info(desc, g_conn, rb_conn, key, skip_items=False):
    """Compare BF.INFO results. skip_items=True for multi-layer where FP causes count diff."""
    g_raw = g_conn.execute_command("BF.INFO", key)
    rb_raw = rb_conn.execute_command("BF.INFO", key)

    def to_dict(raw):
        if isinstance(raw, dict):
            return {(k if isinstance(k, str) else k.decode()): v for k, v in raw.items()}
        d = {}
        for i in range(0, len(raw), 2):
            k = raw[i] if isinstance(raw[i], str) else raw[i].decode()
            d[k] = raw[i + 1]
        return d

    g_info = to_dict(g_raw)
    rb_info = to_dict(rb_raw)

    for field in ["Capacity", "Number of filters", "Expansion rate"]:
        compare(f"{desc} INFO {field}", g_info.get(field), rb_info.get(field))

    if not skip_items:
        compare(f"{desc} INFO Number of items inserted",
                g_info.get("Number of items inserted"),
                rb_info.get("Number of items inserted"))

    if rb_has_info_single:
        for single_field in ["Capacity", "Filters", "Expansion"]:
            g_v = g_conn.execute_command("BF.INFO", key, single_field)
            rb_v = rb_conn.execute_command("BF.INFO", key, single_field)
            g_v = _unwrap_info_single(g_v)
            rb_v = _unwrap_info_single(rb_v)
            compare(f"{desc} INFO single {single_field}", g_v, rb_v)
    else:
        skip(f"{desc} INFO single-field (not supported by this RedisBloom)")


def test_basic_scaling(g, rb):
    """Scenario: default scaling filter."""
    print("\n--- Scenario: basic scaling (0.01, 200, EXPANSION 2) ---")
    key = "compat_basic"

    g_r, rb_r = both(g, rb, "BF.RESERVE", key, 0.01, 200, "EXPANSION", 2)
    compare("RESERVE", g_r, rb_r)

    for i in range(50):
        item = f"item:{i}"
        g_r, rb_r = both(g, rb, "BF.ADD", key, item)
        compare(f"ADD {item}", g_r, rb_r)

    for i in range(50):
        item = f"item:{i}"
        g_r, rb_r = both(g, rb, "BF.EXISTS", key, item)
        compare(f"EXISTS {item}", g_r, rb_r)

    for i in range(50, 60):
        item = f"item:{i}"
        g_r, rb_r = both(g, rb, "BF.EXISTS", key, item)
        compare(f"EXISTS missing {item}", g_r, rb_r)

    if rb_has_card:
        g_r, rb_r = both(g, rb, "BF.CARD", key)
        compare("CARD", g_r, rb_r)
    else:
        skip("CARD (not supported by this RedisBloom)")

    compare_info("basic", g, rb, key)
    both(g, rb, "DEL", key)


def test_high_expansion(g, rb):
    """Scenario: high expansion factor, items within first layer only."""
    print("\n--- Scenario: high expansion (0.01, 100, EXPANSION 4) ---")
    key = "compat_exp4"

    g_r, rb_r = both(g, rb, "BF.RESERVE", key, 0.01, 100, "EXPANSION", 4)
    compare("RESERVE exp4", g_r, rb_r)

    for i in range(80):
        g_r, rb_r = both(g, rb, "BF.ADD", key, f"e4:{i}")
        compare(f"ADD e4:{i}", g_r, rb_r)

    compare_info("exp4", g, rb, key)

    if rb_has_card:
        g_r, rb_r = both(g, rb, "BF.CARD", key)
        compare("CARD exp4", g_r, rb_r)
    else:
        skip("CARD exp4 (not supported by this RedisBloom)")

    both(g, rb, "DEL", key)


def test_nonscaling(g, rb):
    """Scenario: NONSCALING filter."""
    print("\n--- Scenario: nonscaling (0.01, 500, NONSCALING) ---")
    key = "compat_ns"

    g_r, rb_r = both(g, rb, "BF.RESERVE", key, 0.01, 500, "NONSCALING")
    compare("RESERVE nonscaling", g_r, rb_r)

    for i in range(100):
        g_r, rb_r = both(g, rb, "BF.ADD", key, f"ns:{i}")
        compare(f"ADD ns:{i}", g_r, rb_r)

    compare_info("nonscaling", g, rb, key)

    if rb_has_card:
        g_r, rb_r = both(g, rb, "BF.CARD", key)
        compare("CARD nonscaling", g_r, rb_r)
    else:
        skip("CARD nonscaling (not supported by this RedisBloom)")

    both(g, rb, "DEL", key)


def test_madd(g, rb):
    """Scenario: BF.MADD batch operations."""
    print("\n--- Scenario: BF.MADD ---")
    key = "compat_madd"

    both(g, rb, "BF.RESERVE", key, 0.01, 500, "EXPANSION", 2)

    items1 = [f"ma:{i}" for i in range(20)]
    g_r, rb_r = both(g, rb, "BF.MADD", key, *items1)
    compare("MADD first batch", g_r, rb_r)

    g_r, rb_r = both(g, rb, "BF.MADD", key, *items1)
    compare("MADD duplicate batch (all 0)", g_r, rb_r)

    mixed = [f"ma:{i}" for i in range(15, 25)]
    g_r, rb_r = both(g, rb, "BF.MADD", key, *mixed)
    compare("MADD mixed batch", g_r, rb_r)

    items_check = [f"ma:{i}" for i in range(25)]
    g_r, rb_r = both(g, rb, "BF.MEXISTS", key, *items_check)
    compare("MEXISTS after MADD", g_r, rb_r)

    both(g, rb, "DEL", key)


def test_insert(g, rb):
    """Scenario: BF.INSERT with various options."""
    print("\n--- Scenario: BF.INSERT ---")

    key1 = "compat_ins1"
    items = [f"ins:{i}" for i in range(10)]
    g_r, rb_r = both(g, rb, "BF.INSERT", key1, "CAPACITY", 200, "ERROR", 0.01,
                      "EXPANSION", 2, "ITEMS", *items)
    compare("INSERT auto-create", g_r, rb_r)

    g_r, rb_r = both(g, rb, "BF.INSERT", key1, "NOCREATE", "ITEMS", *items)
    compare("INSERT NOCREATE duplicates", g_r, rb_r)

    compare_info("insert", g, rb, key1)

    key2 = "compat_ins_ns"
    items2 = [f"ins_ns:{i}" for i in range(10)]
    g_r, rb_r = both(g, rb, "BF.INSERT", key2, "CAPACITY", 500, "ERROR", 0.01,
                      "NONSCALING", "ITEMS", *items2)
    compare("INSERT NONSCALING", g_r, rb_r)

    compare_info("insert_ns", g, rb, key2)

    both(g, rb, "DEL", key1, key2)


def test_insert_nocreate_missing(g, rb):
    """Scenario: BF.INSERT NOCREATE on non-existent key — both should error."""
    print("\n--- Scenario: BF.INSERT NOCREATE on missing key ---")
    compare_both_error("INSERT NOCREATE missing key", g, rb,
                       "BF.INSERT", "compat_nokey", "NOCREATE", "ITEMS", "x")


def test_auto_create(g, rb):
    """Scenario: auto-create via BF.ADD without prior RESERVE."""
    print("\n--- Scenario: auto-create via BF.ADD ---")
    key = "compat_auto"

    g_r, rb_r = both(g, rb, "BF.ADD", key, "first")
    compare("ADD auto-create", g_r, rb_r)

    g_r, rb_r = both(g, rb, "BF.ADD", key, "second")
    compare("ADD auto second", g_r, rb_r)

    g_r, rb_r = both(g, rb, "BF.ADD", key, "first")
    compare("ADD auto duplicate", g_r, rb_r)

    g_r, rb_r = both(g, rb, "BF.EXISTS", key, "first")
    compare("EXISTS auto first", g_r, rb_r)

    g_r, rb_r = both(g, rb, "BF.EXISTS", key, "nope")
    compare("EXISTS auto missing", g_r, rb_r)

    if rb_has_card:
        g_r, rb_r = both(g, rb, "BF.CARD", key)
        compare("CARD auto", g_r, rb_r)
    else:
        skip("CARD auto (not supported by this RedisBloom)")

    both(g, rb, "DEL", key)


def test_duplicate_items(g, rb):
    """Scenario: repeated insertion, return value transitions from 1 to 0."""
    print("\n--- Scenario: duplicate item return values ---")
    key = "compat_dup"

    both(g, rb, "BF.RESERVE", key, 0.01, 200)

    g_r, rb_r = both(g, rb, "BF.ADD", key, "dup_item")
    compare("ADD first time", g_r, rb_r)

    g_r, rb_r = both(g, rb, "BF.ADD", key, "dup_item")
    compare("ADD second time (dup)", g_r, rb_r)

    g_r, rb_r = both(g, rb, "BF.ADD", key, "dup_item")
    compare("ADD third time (dup)", g_r, rb_r)

    both(g, rb, "DEL", key)


def test_exists_missing_key(g, rb):
    """Scenario: EXISTS/MEXISTS/CARD on non-existent key."""
    print("\n--- Scenario: ops on missing key ---")

    g_r, rb_r = both(g, rb, "BF.EXISTS", "no_such_key", "x")
    compare("EXISTS missing key", g_r, rb_r)

    g_r, rb_r = both(g, rb, "BF.MEXISTS", "no_such_key", "a", "b")
    compare("MEXISTS missing key", g_r, rb_r)

    if rb_has_card:
        g_r, rb_r = both(g, rb, "BF.CARD", "no_such_key")
        compare("CARD missing key", g_r, rb_r)
    else:
        skip("CARD missing key (not supported by this RedisBloom)")


def test_error_cases(g, rb):
    """Scenario: error responses — only check that both sides error."""
    print("\n--- Scenario: error cases ---")

    both(g, rb, "SET", "str_key", "hello")

    compare_both_error("ADD WRONGTYPE", g, rb, "BF.ADD", "str_key", "x")
    compare_both_error("MADD WRONGTYPE", g, rb, "BF.MADD", "str_key", "x")

    g_r, rb_r = both(g, rb, "BF.EXISTS", "str_key", "x")
    compare("EXISTS WRONGTYPE returns 0", g_r, rb_r)

    g_r, rb_r = both(g, rb, "BF.MEXISTS", "str_key", "a", "b")
    compare("MEXISTS WRONGTYPE returns zeros", g_r, rb_r)

    compare_both_error("INFO WRONGTYPE", g, rb, "BF.INFO", "str_key")
    if rb_has_card:
        compare_both_error("CARD WRONGTYPE", g, rb, "BF.CARD", "str_key")
    else:
        skip("CARD WRONGTYPE (not supported by this RedisBloom)")
    compare_both_error("RESERVE WRONGTYPE", g, rb, "BF.RESERVE", "str_key", 0.01, 100)

    compare_both_error("RESERVE bad error rate", g, rb, "BF.RESERVE", "bad1", 0, 100)
    compare_both_error("RESERVE bad capacity", g, rb, "BF.RESERVE", "bad2", 0.01, 0)

    compare_both_error("INFO missing key", g, rb, "BF.INFO", "no_such_bloom")

    both(g, rb, "DEL", "str_key")


def test_reserve_existing(g, rb):
    """Scenario: RESERVE on a key that already has a bloom filter."""
    print("\n--- Scenario: RESERVE on existing bloom key ---")
    key = "compat_exist"

    both(g, rb, "BF.RESERVE", key, 0.01, 100)
    compare_both_error("RESERVE existing bloom key", g, rb, "BF.RESERVE", key, 0.01, 100)
    both(g, rb, "DEL", key)


def test_nonscaling_overflow(g, rb):
    """Scenario: NONSCALING filter overflow — both should error."""
    print("\n--- Scenario: NONSCALING overflow ---")
    key = "compat_ns_overflow"

    both(g, rb, "BF.RESERVE", key, 0.01, 10, "NONSCALING")
    for i in range(10):
        both(g, rb, "BF.ADD", key, f"ov:{i}")

    compare_both_error("ADD overflow", g, rb, "BF.ADD", key, "ov:overflow")
    both(g, rb, "DEL", key)


def test_multi_layer_scaling(g, rb):
    """Scenario: force multi-layer. ADD return values may differ due to FP,
    so only check layer count, capacity, and that all items exist in both."""
    print("\n--- Scenario: multi-layer scaling ---")
    key = "compat_multi"

    both(g, rb, "BF.RESERVE", key, 0.01, 50, "EXPANSION", 2)
    for i in range(200):
        both(g, rb, "BF.ADD", key, f"ml:{i}")

    compare_info("multi-layer", g, rb, key, skip_items=True)

    items_check = [f"ml:{i}" for i in range(200)]
    g_r, rb_r = both(g, rb, "BF.MEXISTS", key, *items_check)
    compare("MEXISTS all multi-layer (no false negatives)", g_r, rb_r)

    both(g, rb, "DEL", key)


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <gemini_bloom.so> <redisbloom.so>", file=sys.stderr)
        sys.exit(1)

    gemini_path = sys.argv[1]
    rb_path = sys.argv[2]

    for path, name in [(gemini_path, "gemini-bloom"), (rb_path, "RedisBloom")]:
        if not os.path.isfile(path):
            print(f"{name} module not found: {path}", file=sys.stderr)
            sys.exit(1)

    port_g = find_free_port()
    port_rb = find_free_port()

    print(f"Starting gemini-bloom on port {port_g}")
    proc_g, log_g = start_redis(gemini_path, port_g)
    print(f"Starting RedisBloom on port {port_rb}")
    proc_rb, log_rb = start_redis(rb_path, port_rb)

    def cleanup(signum=None, frame=None):
        stop_redis(proc_g)
        stop_redis(proc_rb)
        for f in [log_g, log_rb]:
            try:
                os.unlink(f)
            except OSError:
                pass

    signal.signal(signal.SIGTERM, lambda s, f: (cleanup(s, f), sys.exit(1)))

    try:
        g = redis.Redis(host="127.0.0.1", port=port_g, decode_responses=False,
                        protocol=2)
        rb = redis.Redis(host="127.0.0.1", port=port_rb, decode_responses=False,
                         protocol=2)
        g.ping()
        rb.ping()

        global rb_has_card, rb_has_info_single
        rb.execute_command("BF.RESERVE", "__probe__", 0.01, 10)
        try:
            rb.execute_command("BF.CARD", "__probe__")
        except redis.ResponseError:
            rb_has_card = False
            print("NOTE: RedisBloom does not support BF.CARD — skipping CARD comparisons")
        try:
            rb.execute_command("BF.INFO", "__probe__", "Capacity")
        except redis.ResponseError:
            rb_has_info_single = False
            print("NOTE: RedisBloom does not support BF.INFO single-field — skipping")
        rb.delete("__probe__")
        g.delete("__probe__")

        test_basic_scaling(g, rb)
        test_high_expansion(g, rb)
        test_nonscaling(g, rb)
        test_madd(g, rb)
        test_insert(g, rb)
        test_insert_nocreate_missing(g, rb)
        test_auto_create(g, rb)
        test_duplicate_items(g, rb)
        test_exists_missing_key(g, rb)
        test_error_cases(g, rb)
        test_reserve_existing(g, rb)
        test_nonscaling_overflow(g, rb)
        test_multi_layer_scaling(g, rb)

        print(f"\n{'=' * 50}")
        print(f"Results: {passed} passed, {failed} failed, {skipped} skipped")
        print(f"{'=' * 50}")

        if failures:
            print("\nFailures:")
            for f in failures:
                print(f"  {f}")
            sys.exit(1)
        else:
            print("\nCOMPAT TEST PASSED")

    finally:
        cleanup()


if __name__ == "__main__":
    main()
