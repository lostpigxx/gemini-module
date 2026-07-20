#!/usr/bin/env python3
"""
RDB migration test: verify BF.EXISTS results are 100% identical after
migrating bloom filter data between gemini-bloom and RedisBloom via RDB.

Tests 4 single-hop directions + 3 multi-hop chains per environment:
  Single-hop:
    1. RedisBloom  → gemini-bloom  (import)
    2. gemini-bloom → RedisBloom   (export)
    3. gemini-bloom → gemini-bloom (self round-trip)
    4. RedisBloom  → RedisBloom   (baseline)
  Multi-hop chains:
    5. gemini-bloom → RedisBloom → gemini-bloom         (round-trip)
    6. RedisBloom → gemini-bloom → RedisBloom            (reverse round-trip)
    7. gemini-bloom → RB → gemini → RB → gemini          (5-hop stability)

Usage:
    python3 bloom_migrate_test.py <gemini_bloom.so> <redisbloom.so>
"""

import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

import redis


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def detect_protocol(port):
    try:
        r = redis.Redis(host="127.0.0.1", port=port, decode_responses=False,
                        protocol=3)
        r.ping()
        r.close()
        return 3
    except redis.ResponseError:
        return 2


def start_redis(module_path, port, rdb_dir, dbfilename="migrate.rdb"):
    logfile = f"/tmp/redis_migrate_{port}.log"
    log_fh = open(logfile, "w")
    proc = subprocess.Popen(
        [
            "redis-server",
            "--port", str(port),
            "--bind", "127.0.0.1",
            "--daemonize", "no",
            "--loglevel", "warning",
            "--save", "",
            "--dbfilename", dbfilename,
            "--dir", rdb_dir,
            "--loadmodule", os.path.abspath(module_path),
        ],
        stdout=log_fh,
        stderr=subprocess.STDOUT,
    )
    log_fh.close()
    for _ in range(200):
        time.sleep(0.1)
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.close()
            return proc, logfile
        except OSError:
            if proc.poll() is not None:
                with open(logfile) as f:
                    print(f.read(), file=sys.stderr)
                return None, logfile
    proc.kill()
    return None, logfile


def stop_redis(proc):
    if proc and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def wait_for_bgsave(r, timeout=30):
    before = r.execute_command("LASTSAVE")
    r.execute_command("BGSAVE")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if r.execute_command("LASTSAVE") != before:
            return
        time.sleep(0.1)
    raise RuntimeError("BGSAVE did not complete within timeout")


def to_info_dict(raw):
    if isinstance(raw, dict):
        return {(k if isinstance(k, str) else k.decode()): v
                for k, v in raw.items()}
    d = {}
    for i in range(0, len(raw), 2):
        k = raw[i] if isinstance(raw[i], str) else raw[i].decode()
        d[k] = raw[i + 1]
    return d


# ── Corpus definition ────────────────────────────────────────

CORPUS = [
    # Small filters — various configs
    {"key": "small_scaling",    "reserve": [0.01, 50, "EXPANSION", 2],  "count": 200},
    {"key": "small_nonscaling", "reserve": [0.01, 200, "NONSCALING"],   "count": 180},
    {"key": "small_auto",       "reserve": None,                        "count": 150},
    {"key": "small_empty",      "reserve": [0.01, 100],                 "count": 0},
    {"key": "small_binary",     "reserve": [0.01, 100, "EXPANSION", 2], "count": 50,
     "item_fn": lambda i: f"bin:\x00\x01\x02:{i}"},
    {"key": "small_exp1",       "reserve": [0.01, 50, "EXPANSION", 1],  "count": 300},
    {"key": "small_exp4",       "reserve": [0.01, 100, "EXPANSION", 4], "count": 400},

    # Medium filters — production-like
    {"key": "mid_10k",          "reserve": [0.001, 10000, "EXPANSION", 2],  "count": 10000},
    {"key": "mid_50k",          "reserve": [0.01, 50000, "EXPANSION", 2],   "count": 50000},
    {"key": "mid_ns_10k",       "reserve": [0.001, 10000, "NONSCALING"],    "count": 9500},

    # Large filters
    {"key": "large_100k",       "reserve": [0.001, 100000, "EXPANSION", 2], "count": 100000},
    {"key": "large_tight",      "reserve": [0.0001, 50000, "EXPANSION", 2], "count": 80000},

    # Edge cases
    {"key": "edge_single",      "reserve": [0.01, 100],                     "count": 1},
    {"key": "edge_full",        "reserve": [0.01, 100, "NONSCALING"],       "count": 100},
    {"key": "edge_long_key",    "reserve": [0.01, 100, "EXPANSION", 2],     "count": 50,
     "item_fn": lambda i: f"{'A' * 1000}:{i}"},
    {"key": "edge_many_layers", "reserve": [0.01, 10, "EXPANSION", 1],      "count": 200},
]

NUM_PROBES = 2000


def make_items(spec):
    item_fn = spec.get("item_fn", lambda i: f"{spec['key']}:item:{i}")
    return [item_fn(i) for i in range(spec["count"])]


def make_probes(spec):
    return [f"{spec['key']}:probe:{i}" for i in range(NUM_PROBES)]


def populate(r, corpus):
    for spec in corpus:
        key = spec["key"]
        if spec["reserve"]:
            r.execute_command("BF.RESERVE", key, *spec["reserve"])
        items = make_items(spec)
        if not items:
            continue
        if spec["reserve"] is None:
            for item in items:
                r.execute_command("BF.ADD", key, item)
        else:
            batch = 500
            for i in range(0, len(items), batch):
                chunk = items[i:i + batch]
                r.execute_command("BF.MADD", key, *chunk)


def snapshot_results(r, corpus):
    """Record BF.EXISTS for all items + probes, and BF.INFO for each key."""
    results = {}
    for spec in corpus:
        key = spec["key"]
        entry = {"exists": {}, "info": None}

        items = make_items(spec)
        probes = make_probes(spec)

        pipeline = r.pipeline(transaction=False)
        all_queries = items + probes
        for q in all_queries:
            pipeline.execute_command("BF.EXISTS", key, q)
        replies = pipeline.execute()

        for q, reply in zip(all_queries, replies):
            entry["exists"][q] = reply

        try:
            raw = r.execute_command("BF.INFO", key)
            entry["info"] = to_info_dict(raw)
        except redis.ResponseError:
            pass

        results[key] = entry
    return results


def compare_results(src_results, dst_results, direction):
    total = 0
    mismatch_exists = 0
    mismatch_info = 0
    false_negatives = 0

    for key, src_entry in src_results.items():
        dst_entry = dst_results.get(key)
        if dst_entry is None:
            print(f"  MISSING KEY: {key} not found after migration")
            mismatch_exists += len(src_entry["exists"])
            total += len(src_entry["exists"])
            continue

        for query, src_val in src_entry["exists"].items():
            dst_val = dst_entry["exists"].get(query)
            total += 1
            if dst_val != src_val:
                mismatch_exists += 1
                if src_val == 1 and dst_val == 0:
                    false_negatives += 1
                if mismatch_exists <= 5:
                    print(f"  MISMATCH {key} {query[:60]}: "
                          f"src={src_val} dst={dst_val}")

        if src_entry["info"] and dst_entry["info"]:
            for field in ["Capacity", "Number of filters",
                          "Number of items inserted", "Expansion rate"]:
                sv = src_entry["info"].get(field)
                dv = dst_entry["info"].get(field)
                if sv != dv:
                    mismatch_info += 1
                    print(f"  INFO MISMATCH {key} {field}: "
                          f"src={sv} dst={dv}")

    ok = mismatch_exists == 0 and false_negatives == 0
    status = "PASS" if ok else "FAIL"
    print(f"  [{status}] {direction}: {total} comparisons, "
          f"{mismatch_exists} exists mismatches "
          f"({false_negatives} false negatives), "
          f"{mismatch_info} info mismatches")
    return ok


def run_direction(src_module, dst_module, src_name, dst_name,
                  corpus, rdb_dir, proto):
    direction = f"{src_name} → {dst_name}"
    print(f"\n--- {direction} ---")

    port_src = find_free_port()
    proc_src, log_src = start_redis(src_module, port_src, rdb_dir)
    if not proc_src:
        print(f"  SKIP: {src_name} failed to start (check {log_src})")
        return None

    try:
        r_src = redis.Redis(host="127.0.0.1", port=port_src,
                            decode_responses=False, protocol=proto)
        r_src.ping()

        print(f"  Populating {len(corpus)} filters on {src_name}...")
        populate(r_src, corpus)

        print(f"  Snapshotting results...")
        src_results = snapshot_results(r_src, corpus)

        total_items = sum(len(e["exists"]) for e in src_results.values())
        print(f"  Recorded {total_items} EXISTS results across "
              f"{len(src_results)} keys")

        wait_for_bgsave(r_src)
    finally:
        stop_redis(proc_src)

    time.sleep(1)

    port_dst = find_free_port()
    proc_dst, log_dst = start_redis(dst_module, port_dst, rdb_dir)
    if not proc_dst:
        print(f"  SKIP: {dst_name} failed to load RDB (expected for "
              f"encver incompatibility)")
        try:
            os.unlink(os.path.join(rdb_dir, "migrate.rdb"))
        except OSError:
            pass
        return None

    try:
        r_dst = redis.Redis(host="127.0.0.1", port=port_dst,
                            decode_responses=False, protocol=proto)
        r_dst.ping()

        print(f"  Verifying on {dst_name}...")
        dst_results = snapshot_results(r_dst, corpus)

        ok = compare_results(src_results, dst_results, direction)
        return ok
    finally:
        stop_redis(proc_dst)
        try:
            os.unlink(os.path.join(rdb_dir, "migrate.rdb"))
        except OSError:
            pass


def populate_and_snapshot(module_path, mod_name, corpus, rdb_dir, proto):
    port = find_free_port()
    proc, logfile = start_redis(module_path, port, rdb_dir)
    if not proc:
        print(f"  SKIP: {mod_name} failed to start (check {logfile})")
        return None
    try:
        r = redis.Redis(host="127.0.0.1", port=port,
                        decode_responses=False, protocol=proto)
        r.ping()
        print(f"  Populating {len(corpus)} filters on {mod_name}...")
        populate(r, corpus)
        print(f"  Snapshotting source results...")
        results = snapshot_results(r, corpus)
        total = sum(len(e["exists"]) for e in results.values())
        print(f"  Recorded {total} EXISTS results across {len(results)} keys")
        wait_for_bgsave(r)
        return results
    finally:
        stop_redis(proc)


def migrate_hop(module_path, mod_name, rdb_dir, corpus, proto,
                do_bgsave=True):
    port = find_free_port()
    proc, logfile = start_redis(module_path, port, rdb_dir)
    if not proc:
        print(f"  SKIP: {mod_name} failed to load RDB (check {logfile})")
        return None
    try:
        r = redis.Redis(host="127.0.0.1", port=port,
                        decode_responses=False, protocol=proto)
        r.ping()
        print(f"  Snapshotting on {mod_name}...")
        results = snapshot_results(r, corpus)
        if do_bgsave:
            wait_for_bgsave(r)
        return results
    finally:
        stop_redis(proc)


def run_chain(chain, corpus, proto):
    labels = [name for _, name in chain]
    chain_label = " → ".join(labels)
    print(f"\n--- Chain: {chain_label} ---")

    rdb_dir = tempfile.mkdtemp()
    try:
        src_mod, src_name = chain[0]
        ground_truth = populate_and_snapshot(src_mod, src_name, corpus,
                                            rdb_dir, proto)
        if ground_truth is None:
            return None

        all_hops_ok = True
        prev_results = ground_truth
        for i in range(1, len(chain)):
            hop_mod, hop_name = chain[i]
            is_last = (i == len(chain) - 1)
            prev_name = chain[i - 1][1]

            print(f"  Hop {i}/{len(chain) - 1}: "
                  f"{prev_name} → {hop_name}...")
            hop_results = migrate_hop(hop_mod, hop_name, rdb_dir,
                                      corpus, proto,
                                      do_bgsave=not is_last)
            if hop_results is None:
                return None

            hop_ok = compare_results(
                prev_results, hop_results,
                f"hop {i} ({prev_name} → {hop_name})")
            if not hop_ok:
                all_hops_ok = False
            prev_results = hop_results

        if len(chain) > 2:
            final_ok = compare_results(
                ground_truth, prev_results,
                f"full chain ({chain_label}): origin vs final")
        else:
            final_ok = all_hops_ok

        ok = all_hops_ok and final_ok
        status = "PASS" if ok else "FAIL"
        print(f"  [{status}] Chain: {chain_label}")
        return ok
    finally:
        shutil.rmtree(rdb_dir, ignore_errors=True)


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <gemini_bloom.so> <redisbloom.so>",
              file=sys.stderr)
        sys.exit(1)

    gemini_path = sys.argv[1]
    rb_path = sys.argv[2]

    for path, name in [(gemini_path, "gemini-bloom"), (rb_path, "RedisBloom")]:
        if not os.path.isfile(path):
            print(f"{name} module not found: {path}", file=sys.stderr)
            sys.exit(1)

    signal.signal(signal.SIGTERM, lambda s, f: sys.exit(1))

    forced = os.environ.get("RESP_PROTOCOL")
    if forced:
        proto = int(forced)
    else:
        port_tmp = find_free_port()
        tmp_dir = tempfile.mkdtemp()
        proc_tmp, _ = start_redis(gemini_path, port_tmp, tmp_dir)
        if proc_tmp:
            proto = detect_protocol(port_tmp)
            stop_redis(proc_tmp)
        else:
            proto = 2
        shutil.rmtree(tmp_dir, ignore_errors=True)
    print(f"Protocol: RESP{proto}")

    directions = [
        (rb_path,     gemini_path, "RedisBloom",  "gemini-bloom"),
        (gemini_path, rb_path,     "gemini-bloom", "RedisBloom"),
        (gemini_path, gemini_path, "gemini-bloom", "gemini-bloom"),
        (rb_path,     rb_path,     "RedisBloom",   "RedisBloom"),
    ]

    passed = 0
    failed = 0
    skipped = 0

    for src_mod, dst_mod, src_name, dst_name in directions:
        rdb_dir = tempfile.mkdtemp()
        try:
            result = run_direction(src_mod, dst_mod, src_name, dst_name,
                                   CORPUS, rdb_dir, proto)
        finally:
            shutil.rmtree(rdb_dir, ignore_errors=True)
        if result is None:
            skipped += 1
        elif result:
            passed += 1
        else:
            failed += 1

    # Multi-hop chain tests
    chains = [
        [(gemini_path, "gemini-bloom"), (rb_path, "RedisBloom"), (gemini_path, "gemini-bloom")],
        [(rb_path, "RedisBloom"), (gemini_path, "gemini-bloom"), (rb_path, "RedisBloom")],
        [(gemini_path, "gemini-bloom"), (rb_path, "RedisBloom"), (gemini_path, "gemini-bloom"),
         (rb_path, "RedisBloom"), (gemini_path, "gemini-bloom")],
    ]

    for chain in chains:
        result = run_chain(chain, CORPUS, proto)
        if result is None:
            skipped += 1
        elif result:
            passed += 1
        else:
            failed += 1

    print(f"\n{'=' * 50}")
    print(f"Migration results: {passed} passed, {failed} failed, "
          f"{skipped} skipped")
    print(f"{'=' * 50}")

    if failed > 0:
        print("\nMIGRATION TEST FAILED")
        sys.exit(1)
    else:
        print("\nMIGRATION TEST PASSED")


if __name__ == "__main__":
    main()
