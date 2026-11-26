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
    "cf_ops": 0, "cms_ops": 0, "topk_ops": 0, "tdigest_ops": 0,
}


def inc(key, n=1):
    stats[key] += n
    stats["total_ops"] += n


def detect_protocol(port):
    """Try RESP3 first; fall back to RESP2 if the server has no HELLO command."""
    try:
        r = redis.Redis(host="127.0.0.1", port=port, decode_responses=False,
                        protocol=3)
        r.ping()
        r.close()
        return 3
    except redis.ResponseError:
        return 2


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def start_redis(module_path, port):
    logfile = f"/tmp/redis_soak_{port}.log"
    log_fh = open(logfile, "w")
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
        stdout=log_fh,
        stderr=subprocess.STDOUT,
    )
    log_fh.close()
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
    print(f"Submodules (CF/CMS/TopK/TD): {stats['cf_ops']}/{stats['cms_ops']}/{stats['topk_ops']}/{stats['tdigest_ops']}")
    print(f"{'=' * 40}")


def run_submodules_cycle(r, cycle):
    """Exercise Cuckoo, CMS, Top-K, and T-Digest with invariant verification."""
    # 1. Cuckoo Filter: scaling + add/exists/count/del + scandump/loadchunk
    cf_k = f"soak:cf:{cycle}"
    cf_k2 = f"soak:cf_copy:{cycle}"
    r.execute_command("CF.RESERVE", cf_k, 128, "BUCKETSIZE", 4, "EXPANSION", 2)
    cf_items = [f"cf_{cycle}_{i}" for i in range(150)]
    r.execute_command("CF.INSERT", cf_k, "ITEMS", *cf_items)
    ex = r.execute_command("CF.MEXISTS", cf_k, *cf_items)
    if any(v != 1 for v in ex):
        soak_fail(f"CF false negative in cycle {cycle}")
    cursor = 0
    while True:
        res = r.execute_command("CF.SCANDUMP", cf_k, cursor)
        nxt, data = res[0], res[1]
        if nxt == 0:
            break
        r.execute_command("CF.LOADCHUNK", cf_k2, nxt, data)
        cursor = nxt
    if r.execute_command("CF.EXISTS", cf_k2, cf_items[0]) != 1:
        soak_fail(f"CF LOADCHUNK lost item in cycle {cycle}")
    if r.execute_command("CF.DEL", cf_k, cf_items[0]) != 1:
        soak_fail(f"CF DEL failed in cycle {cycle}")
    r.delete(cf_k, cf_k2)
    inc("cf_ops", 6)

    # 2. Count-Min Sketch: incrby + query + self-aliased merge
    cms_a = f"soak:cms_a:{cycle}"
    cms_b = f"soak:cms_b:{cycle}"
    r.execute_command("CMS.INITBYDIM", cms_a, 256, 4)
    r.execute_command("CMS.INITBYDIM", cms_b, 256, 4)
    r.execute_command("CMS.INCRBY", cms_a, "hot", 10, "warm", 3)
    r.execute_command("CMS.INCRBY", cms_b, "hot", 7)
    r.execute_command("CMS.MERGE", cms_a, 2, cms_a, cms_b, "WEIGHTS", 2, 3)
    q = r.execute_command("CMS.QUERY", cms_a, "hot")[0]
    if q < 41:
        soak_fail(f"CMS self-alias merge undercounted: got {q}, expected >= 41")
    r.delete(cms_a, cms_b)
    inc("cms_ops", 6)

    # 3. Top-K: add + batched incrby + query + list
    tk_k = f"soak:topk:{cycle}"
    r.execute_command("TOPK.RESERVE", tk_k, 5, 128, 4, 0.9)
    r.execute_command("TOPK.INCRBY", tk_k, "elephant", 500, "mouse", 2)
    r.execute_command("TOPK.ADD", tk_k, "a", "b", "c", "d", "e")
    if r.execute_command("TOPK.QUERY", tk_k, "elephant")[0] != 1:
        soak_fail(f"Top-K missed elephant in cycle {cycle}")
    lst = r.execute_command("TOPK.LIST", tk_k, "WITHCOUNT")
    if not lst or lst[0] != b"elephant":
        soak_fail(f"Top-K list top mismatch in cycle {cycle}: {lst[:2]}")
    r.delete(tk_k)
    inc("topk_ops", 5)

    # 4. T-Digest: add + quantile + cdf + merge OVERRIDE self-alias
    td_a = f"soak:td_a:{cycle}"
    td_b = f"soak:td_b:{cycle}"
    r.execute_command("TDIGEST.CREATE", td_a, "COMPRESSION", 100)
    r.execute_command("TDIGEST.CREATE", td_b, "COMPRESSION", 100)
    vals_a = [str(i) for i in range(1, 201)]
    vals_b = [str(i) for i in range(201, 401)]
    r.execute_command("TDIGEST.ADD", td_a, *vals_a)
    r.execute_command("TDIGEST.ADD", td_b, *vals_b)
    r.execute_command("TDIGEST.MERGE", td_a, 2, td_a, td_b, "OVERRIDE")
    q50 = float(r.execute_command("TDIGEST.QUANTILE", td_a, 0.5)[0])
    if not (190.0 <= q50 <= 210.0):
        soak_fail(f"T-Digest median out of range in cycle {cycle}: {q50}")
    c = float(r.execute_command("TDIGEST.CDF", td_a, 200.5)[0])
    if not (0.45 <= c <= 0.55):
        soak_fail(f"T-Digest CDF out of range in cycle {cycle}: {c}")
    r.delete(td_a, td_b)
    inc("tdigest_ops", 7)


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
        forced = os.environ.get("RESP_PROTOCOL")
        proto = int(forced) if forced else detect_protocol(port)
        print(f"Protocol: RESP{proto}")
        r_conn = redis.Redis(host="127.0.0.1", port=port, decode_responses=False,
                             protocol=proto)
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
                    run_submodules_cycle(r_conn, cycle)
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
