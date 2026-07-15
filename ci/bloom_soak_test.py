#!/usr/bin/env python3
"""
Bloom filter soak/stress test for gemini-bloom module.

Starts a redis-server with the module loaded, then continuously exercises
all BF.* commands for a configurable duration, checking invariants after
every cycle. Any false negative, crash, or data inconsistency causes an
immediate exit with nonzero status.

Usage:
    python3 bloom_soak_test.py <path/to/redis_bloom.so> [duration_seconds]

Environment:
    SOAK_DURATION_SEC  — test duration in seconds (default 300)
"""

import os
import random
import signal
import socket
import subprocess
import sys
import time

import redis

SCENARIOS = [
    {"name": "scaling_default",  "error_rate": 0.01,   "capacity": 200,  "expansion": 2, "items": 500,  "batch": 50},
    {"name": "scaling_exp4",     "error_rate": 0.01,   "capacity": 100,  "expansion": 4, "items": 400,  "batch": 20},
    {"name": "scaling_exp1",     "error_rate": 0.05,   "capacity": 50,   "expansion": 1, "items": 300,  "batch": 10},
    {"name": "nonscaling_small", "error_rate": 0.01,   "capacity": 100,  "expansion": 0, "items": 90,   "batch": 15},
    {"name": "tight_error",      "error_rate": 0.001,  "capacity": 500,  "expansion": 2, "items": 1000, "batch": 100},
    {"name": "large_capacity",   "error_rate": 0.01,   "capacity": 5000, "expansion": 2, "items": 8000, "batch": 200},
    {"name": "nonscaling_large", "error_rate": 0.01,   "capacity": 1000, "expansion": 0, "items": 950,  "batch": 50},
    {"name": "low_error_small",  "error_rate": 0.0001, "capacity": 200,  "expansion": 2, "items": 600,  "batch": 30},
]

stats = {
    "total_ops": 0, "cycles": 0, "filters_created": 0,
    "items_inserted": 0, "scandump_roundtrips": 0,
    "bf_reserve": 0, "bf_add": 0, "bf_madd": 0, "bf_insert": 0,
    "bf_exists": 0, "bf_mexists": 0, "bf_info": 0, "bf_card": 0,
    "bf_scandump": 0, "bf_loadchunk": 0,
}


def inc(key, n=1):
    stats[key] += n
    stats["total_ops"] += n


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def start_redis(module_path, port):
    logfile = f"/tmp/redis_soak_{port}.log"
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


def soak_fail(msg):
    elapsed = time.time() - start_time
    print(f"\nSOAK FAIL after {elapsed:.0f}s, {stats['cycles']} cycles, "
          f"{stats['total_ops']} ops", file=sys.stderr)
    print(f"  {msg}", file=sys.stderr)
    print_stats()
    sys.exit(1)


def check_server_alive(r):
    try:
        if r.ping():
            return
    except Exception:
        pass
    soak_fail("redis-server is not responding (crash?)")


def get_used_memory(r):
    info = r.info("memory")
    return info.get("used_memory", 0)


def parse_bf_info(r, key):
    raw = r.execute_command("BF.INFO", key)
    inc("bf_info")
    if isinstance(raw, dict):
        return {(k if isinstance(k, str) else k.decode()): v for k, v in raw.items()}
    info = {}
    for i in range(0, len(raw), 2):
        k = raw[i] if isinstance(raw[i], str) else raw[i].decode()
        info[k] = raw[i + 1]
    return info


def run_scenario(r, scenario, cycle):
    name = scenario["name"]
    error_rate = scenario["error_rate"]
    capacity = scenario["capacity"]
    expansion = scenario["expansion"]
    target_items = scenario["items"]
    batch = scenario["batch"]
    nonscaling = expansion == 0

    key = f"soak_{name}_{cycle}"
    inserted_items = []

    # Phase 1: BF.RESERVE
    if nonscaling:
        r.execute_command("BF.RESERVE", key, error_rate, capacity, "NONSCALING")
    else:
        r.execute_command("BF.RESERVE", key, error_rate, capacity, "EXPANSION", expansion)
    inc("bf_reserve")
    inc("filters_created")

    # Phase 2: Mixed writes — rotate BF.ADD / BF.MADD / BF.INSERT
    idx = 0
    while idx < target_items:
        method = idx % 3
        try:
            if method == 0:
                item = f"{key}:i:{idx}"
                r.execute_command("BF.ADD", key, item)
                inserted_items.append(item)
                idx += 1
                inc("bf_add")
            elif method == 1:
                end = min(idx + batch, target_items)
                items = [f"{key}:i:{j}" for j in range(idx, end)]
                r.execute_command("BF.MADD", key, *items)
                inserted_items.extend(items)
                idx = end
                inc("bf_madd")
            else:
                end = min(idx + batch, target_items)
                items = [f"{key}:i:{j}" for j in range(idx, end)]
                r.execute_command("BF.INSERT", key, "NOCREATE", "ITEMS", *items)
                inserted_items.extend(items)
                idx = end
                inc("bf_insert")
        except redis.ResponseError as e:
            msg = str(e)
            if "non scaling filter is full" in msg or "filter expansion failed" in msg:
                break
            raise

    inc("items_inserted", len(inserted_items))

    if not inserted_items:
        r.delete(key)
        return

    # Phase 3: BF.EXISTS + BF.MEXISTS — verify zero false negatives
    sample = random.sample(inserted_items, min(50, len(inserted_items)))
    for item in sample:
        result = r.execute_command("BF.EXISTS", key, item)
        inc("bf_exists")
        if result != 1:
            soak_fail(f"FALSE NEGATIVE: BF.EXISTS {key} {item} returned {result}")

    for i in range(0, len(inserted_items), 100):
        chunk = inserted_items[i:i + 100]
        results = r.execute_command("BF.MEXISTS", key, *chunk)
        inc("bf_mexists")
        for j, val in enumerate(results):
            if val != 1:
                soak_fail(f"FALSE NEGATIVE: BF.MEXISTS {key} {chunk[j]} returned {val}")

    # Phase 4: BF.INFO + BF.CARD consistency
    info = parse_bf_info(r, key)
    card = r.execute_command("BF.CARD", key)
    inc("bf_card")

    info_items = info["Number of items inserted"]
    info_filters = info["Number of filters"]
    info_size = info["Size"]
    info_capacity = info["Capacity"]
    info_expansion = info["Expansion rate"]

    if card != info_items:
        soak_fail(f"CARD/INFO mismatch: BF.CARD={card}, INFO Items={info_items}")
    if info_filters < 1:
        soak_fail(f"BF.INFO Filters={info_filters}, expected >= 1")
    if info_size <= 0:
        soak_fail(f"BF.INFO Size={info_size}, expected > 0")
    if info_capacity < capacity:
        soak_fail(f"BF.INFO Capacity={info_capacity} < initial {capacity}")
    if nonscaling:
        if info_expansion is not None:
            soak_fail(f"BF.INFO Expansion should be None for NONSCALING, got {info_expansion}")
    else:
        if info_expansion != expansion:
            soak_fail(f"BF.INFO Expansion={info_expansion}, expected {expansion}")

    cap_single = r.execute_command("BF.INFO", key, "Capacity")
    inc("bf_info")
    if isinstance(cap_single, list):
        cap_single = cap_single[0]
    if cap_single != info_capacity:
        soak_fail(f"BF.INFO single Capacity={cap_single} != full {info_capacity}")

    items_single = r.execute_command("BF.INFO", key, "Items")
    inc("bf_info")
    if isinstance(items_single, list):
        items_single = items_single[0]
    if items_single != info_items:
        soak_fail(f"BF.INFO single Items={items_single} != full {info_items}")

    # Phase 5: BF.SCANDUMP / BF.LOADCHUNK round-trip
    clone_key = f"{key}_clone"
    cursor = 0
    while True:
        reply = r.execute_command("BF.SCANDUMP", key, cursor)
        inc("bf_scandump")
        next_cursor = reply[0]
        chunk_data = reply[1]

        if next_cursor == 0 and (chunk_data is None or len(chunk_data) == 0):
            break

        r.execute_command("BF.LOADCHUNK", clone_key, next_cursor, chunk_data)
        inc("bf_loadchunk")
        cursor = next_cursor

    inc("scandump_roundtrips")

    clone_card = r.execute_command("BF.CARD", clone_key)
    inc("bf_card")
    if clone_card != card:
        soak_fail(f"SCANDUMP clone CARD={clone_card} != original {card}")

    clone_info = parse_bf_info(r, clone_key)
    if clone_info["Number of filters"] != info_filters:
        soak_fail(f"Clone Filters={clone_info['Number of filters']} != original {info_filters}")

    verify_count = min(len(inserted_items), 200)
    verify_items = random.sample(inserted_items, verify_count)
    for item in verify_items:
        result = r.execute_command("BF.EXISTS", clone_key, item)
        inc("bf_exists")
        if result != 1:
            soak_fail(f"FALSE NEGATIVE in clone: BF.EXISTS {clone_key} {item} returned {result}")

    # Phase 6: Cleanup
    r.delete(key, clone_key)

    check_server_alive(r)


def print_stats():
    elapsed = time.time() - start_time
    print(f"\n{'=' * 40}")
    print(f"Soak Test Statistics")
    print(f"{'=' * 40}")
    print(f"Duration:              {elapsed:.0f}s")
    print(f"Cycles:                {stats['cycles']}")
    print(f"Total ops:             {stats['total_ops']}")
    print(f"  BF.RESERVE:          {stats['bf_reserve']}")
    print(f"  BF.ADD:              {stats['bf_add']}")
    print(f"  BF.MADD:            {stats['bf_madd']}")
    print(f"  BF.INSERT:           {stats['bf_insert']}")
    print(f"  BF.EXISTS:           {stats['bf_exists']}")
    print(f"  BF.MEXISTS:          {stats['bf_mexists']}")
    print(f"  BF.INFO:             {stats['bf_info']}")
    print(f"  BF.CARD:             {stats['bf_card']}")
    print(f"  BF.SCANDUMP:         {stats['bf_scandump']}")
    print(f"  BF.LOADCHUNK:        {stats['bf_loadchunk']}")
    print(f"Filters created:       {stats['filters_created']}")
    print(f"Items inserted:        {stats['items_inserted']}")
    print(f"SCANDUMP round-trips:  {stats['scandump_roundtrips']}")
    print(f"{'=' * 40}")


def main():
    global start_time

    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <path/to/redis_bloom.so> [duration_seconds]",
              file=sys.stderr)
        sys.exit(1)

    module_path = sys.argv[1]
    duration = int(os.environ.get("SOAK_DURATION_SEC", 300))
    if len(sys.argv) > 2:
        duration = int(sys.argv[2])

    if not os.path.isfile(module_path):
        print(f"Module not found: {module_path}", file=sys.stderr)
        sys.exit(1)

    port = find_free_port()
    print(f"Starting redis-server on port {port} with module {module_path}")
    proc, logfile = start_redis(module_path, port)

    def cleanup(signum=None, frame=None):
        stop_redis(proc)
        try:
            os.unlink(logfile)
        except OSError:
            pass

    signal.signal(signal.SIGTERM, lambda s, f: (cleanup(s, f), sys.exit(1)))

    try:
        r_conn = redis.Redis(host="127.0.0.1", port=port, decode_responses=False,
                             protocol=2)
        r_conn.ping()

        initial_memory = get_used_memory(r_conn)
        start_time = time.time()
        deadline = start_time + duration
        cycle = 0

        print(f"Starting soak test for {duration}s...")
        print(f"Initial memory: {initial_memory} bytes")

        while time.time() < deadline:
            for scenario in SCENARIOS:
                if time.time() >= deadline:
                    break
                cycle += 1
                stats["cycles"] = cycle

                try:
                    run_scenario(r_conn, scenario, cycle)
                except redis.ConnectionError:
                    soak_fail("Lost connection to redis-server (crash?)")
                except redis.ResponseError as e:
                    soak_fail(f"Unexpected ResponseError in cycle {cycle}: {e}")

                if cycle % 10 == 0:
                    elapsed = time.time() - start_time
                    mem = get_used_memory(r_conn)
                    print(f"  cycle={cycle} elapsed={elapsed:.0f}s "
                          f"ops={stats['total_ops']} mem={mem}")

        # Final checks
        final_memory = get_used_memory(r_conn)
        print(f"Final memory: {final_memory} bytes (initial: {initial_memory})")
        if initial_memory > 0:
            ratio = final_memory / initial_memory
            if ratio > 5.0:
                print(f"WARNING: Memory grew {ratio:.1f}x during soak test (possible leak)")

        check_server_alive(r_conn)
        print_stats()
        print("\nSOAK TEST PASSED")

    finally:
        cleanup()


start_time = time.time()

if __name__ == "__main__":
    main()
