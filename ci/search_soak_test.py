#!/usr/bin/env python3
"""Soak test for gemini-search (Redis 7).

Usage:
    python3 ci/search_soak_test.py <path/to/redis_search.so>

Environment variables:
    SOAK_DURATION_SEC   Test duration in seconds (default: 60)
    RESP_PROTOCOL       Force RESP protocol version ("2" or "3"); auto-detect if unset.
"""

import os
import random
import shutil
import socket
import struct
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
    forced = os.environ.get("RESP_PROTOCOL")
    if forced:
        return int(forced)
    try:
        r = redis.Redis(host="127.0.0.1", port=port, protocol=3)
        r.ping()
        r.close()
        return 3
    except Exception:
        return 2


def start_redis(module_path, port, workdir):
    cmd = [
        "redis-server",
        "--port", str(port),
        "--dir", workdir,
        "--save", "",
        "--appendonly", "no",
        "--loglevel", "warning",
        "--loadmodule", os.path.abspath(module_path),
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    for _ in range(50):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return proc
        except OSError:
            time.sleep(0.1)
    proc.kill()
    raise RuntimeError(f"redis-server failed to start on port {port}")


def vec_bytes(vals):
    return struct.pack(f"<{len(vals)}f", *vals)


def main():
    if len(sys.argv) < 2:
        print("Usage: search_soak_test.py <redis_search.so>", file=sys.stderr)
        sys.exit(2)

    module_path = sys.argv[1]
    duration = int(os.environ.get("SOAK_DURATION_SEC", "60"))
    workdir = tempfile.mkdtemp(prefix="search_soak_")
    port = find_free_port()
    proc = None

    try:
        proc = start_redis(module_path, port, workdir)
        proto = detect_protocol(port)
        r = redis.Redis(host="127.0.0.1", port=port, protocol=proto, decode_responses=True)
        print(f"gemini-search soak test: duration={duration}s, protocol=RESP{proto}")

        # 1. Create ON HASH index (TAG + NUMERIC + TEXT + GEO)
        r.execute_command(
            "FT.CREATE", "soak_hash", "ON", "HASH", "PREFIX", "1", "item:",
            "SCHEMA",
            "cat", "TAG",
            "price", "NUMERIC", "SORTABLE",
            "title", "TEXT", "PHONETIC", "dm:en",
            "loc", "GEO",
        )

        # 2. Create HNSW vector index
        r.execute_command(
            "FT.CREATE", "soak_vec",
            "SCHEMA",
            "tag", "TAG",
            "emb", "VECTOR", "HNSW", "DIM", "4", "DISTANCE_METRIC", "L2",
            "M", "8", "EF_CONSTRUCTION", "64",
        )

        rng = random.Random(2026)
        cats = ["alpha", "beta", "gamma", "delta"]
        words = ["fast", "running", "redis", "search", "engine", "Jon", "vector", "geo"]
        expected_cats = {}  # key -> cat
        vec_keys = set()

        # Warm-up population (bounded keyspace: 500 hash keys, 200 vector docs)
        for i in range(200):
            cat = rng.choice(cats)
            key = f"item:{i}"
            r.hset(key, mapping={
                "cat": cat,
                "price": str(rng.randint(1, 1000)),
                "title": f"{rng.choice(words)} {rng.choice(words)}",
                "loc": f"{rng.uniform(-10, 10):.4f},{rng.uniform(-10, 10):.4f}",
            })
            expected_cats[key] = cat

        for i in range(100):
            v = [rng.random() for _ in range(4)]
            doc_id = f"v:{i}"
            r.execute_command(
                "FT.ADD", "soak_vec", doc_id, "FIELDS",
                "tag", rng.choice(cats), "emb", vec_bytes(v),
            )
            vec_keys.add(doc_id)

        mem_start = r.info("memory")["used_memory"]
        ops = 0
        checks = 0
        deadline = time.monotonic() + duration

        while time.monotonic() < deadline:
            action = rng.random()
            if action < 0.35:
                # Upsert hash doc
                idx = rng.randint(0, 499)
                key = f"item:{idx}"
                cat = rng.choice(cats)
                r.hset(key, mapping={
                    "cat": cat,
                    "price": str(rng.randint(1, 1000)),
                    "title": f"{rng.choice(words)} {rng.choice(words)}",
                    "loc": f"{rng.uniform(-10, 10):.4f},{rng.uniform(-10, 10):.4f}",
                })
                expected_cats[key] = cat
            elif action < 0.45 and expected_cats:
                # Delete hash doc
                key = rng.choice(list(expected_cats.keys()))
                r.unlink(key)
                del expected_cats[key]
            elif action < 0.55:
                # HNSW upsert/delete
                vid = f"v:{rng.randint(0, 199)}"
                if vid in vec_keys and rng.random() < 0.3:
                    r.execute_command("FT.DEL", "soak_vec", vid)
                    vec_keys.remove(vid)
                else:
                    v = [rng.random() for _ in range(4)]
                    r.execute_command(
                        "FT.ADD", "soak_vec", vid, "FIELDS",
                        "tag", rng.choice(cats), "emb", vec_bytes(v),
                    )
                    vec_keys.add(vid)
            elif action < 0.75:
                # FT.SEARCH mixed query
                cat = rng.choice(cats)
                res = r.execute_command(
                    "FT.SEARCH", "soak_hash", f"@cat:{{{cat}}}", "NOCONTENT", "LIMIT", "0", "1000"
                )
                expected_count = sum(1 for c in expected_cats.values() if c == cat)
                if int(res[0]) != expected_count:
                    raise AssertionError(
                        f"TAG count mismatch for {cat}: got {res[0]}, expected {expected_count}"
                    )
                checks += 1
            elif action < 0.85:
                # FT.AGGREGATE with cursor + full drain
                res = r.execute_command(
                    "FT.AGGREGATE", "soak_hash", "*",
                    "GROUPBY", "1", "@cat", "REDUCE", "COUNT", "0", "AS", "cnt",
                    "WITHCURSOR", "COUNT", "2", "MAXIDLE", "5000",
                )
                cid = int(res[1])
                while cid != 0:
                    res = r.execute_command("FT.CURSOR", "READ", "soak_hash", str(cid), "COUNT", "2")
                    cid = int(res[1])
                checks += 1
            elif action < 0.93 and vec_keys:
                # HNSW KNN query
                q = vec_bytes([rng.random() for _ in range(4)])
                res = r.execute_command(
                    "FT.SEARCH", "soak_vec", "*=>[KNN 5 @emb $q]",
                    "PARAMS", "2", "q", q, "NOCONTENT",
                )
                if int(res[0]) == 0:
                    raise AssertionError("Expected non-empty HNSW KNN result")
                checks += 1
            else:
                # FT.PROFILE + FT.INFO
                r.execute_command("FT.PROFILE", "soak_hash", "SEARCH", "QUERY", "*", "LIMIT", "0", "5")
                info = r.execute_command("FT.INFO", "soak_hash")
                if not info:
                    raise AssertionError(f"Invalid FT.INFO: {info}")
                checks += 1

            ops += 1

        # Final ground-truth check
        total_res = r.execute_command("FT.SEARCH", "soak_hash", "*", "NOCONTENT", "LIMIT", "0", "1000")
        if int(total_res[0]) != len(expected_cats):
            raise AssertionError(
                f"Final doc count mismatch: got {total_res[0]}, expected {len(expected_cats)}"
            )

        mem_end = r.info("memory")["used_memory"]
        mem_growth_mb = (mem_end - mem_start) / (1024 * 1024)
        print(
            f"PASS: soak completed — ops={ops}, checks={checks}, "
            f"live_docs={len(expected_cats)}, mem_delta={mem_growth_mb:+.2f} MB"
        )
        # Bounded keyspace (<=500 docs) should never grow unboundedly (>64 MB)
        if mem_growth_mb > 64.0:
            raise AssertionError(f"Excessive memory growth during soak: {mem_growth_mb:.2f} MB")

        r.close()
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
