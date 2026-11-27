#!/usr/bin/env python3
"""Cluster vs Standalone Performance Benchmark for gemini-search (Phase 72).

Measures on identical 15,000-doc corpus (Hash + 128d HNSW + 2KB payload):
  1. Concurrent throughput scaling (1-node vs 3-node, 6 worker threads)
  2. Coordinator overhead latency (P50 / P99, single-client sequential)
  3. Query-Then-Fetch bandwidth savings (LIMIT 0 10 and LIMIT 90 10 on 2KB docs)

Usage:
    python3 ci/search_cluster_bench.py [path/to/redis_search.so]
"""

import os
import random
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time

import redis
from redis.cluster import RedisCluster


def parse_kv_list(arr: list) -> dict:
    return {arr[i]: arr[i + 1] for i in range(0, len(arr), 2)}


def find_free_ports(count: int) -> list[int]:
    ports: list[int] = []
    sockets: list[socket.socket] = []
    try:
        while len(ports) < count:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.bind(("127.0.0.1", 0))
            p = s.getsockname()[1]
            if p >= 40000 or p in ports:
                s.close()
                continue
            bus = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            rpc = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                bus.bind(("127.0.0.1", p + 10000))
                rpc.bind(("127.0.0.1", p + 20000))
            except OSError:
                rpc.close()
                bus.close()
                s.close()
                continue
            sockets.extend([s, bus, rpc])
            ports.append(p)
    finally:
        for s in sockets:
            s.close()
    return ports


def start_node(port: int, search_so: str, node_dir: str, cluster: bool) -> subprocess.Popen:
    os.makedirs(node_dir, exist_ok=True)
    cmd = [
        "redis-server",
        "--port", str(port),
        "--bind", "127.0.0.1",
        "--dir", node_dir,
        "--save", "",
        "--appendonly", "no",
        "--loglevel", "warning",
        "--loadmodule", os.path.abspath(search_so),
    ]
    if cluster:
        cmd.extend([
            "--cluster-enabled", "yes",
            "--cluster-config-file", f"nodes-{port}.conf",
            "--cluster-node-timeout", "5000",
        ])
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    deadline = time.time() + 10.0
    while time.time() < deadline:
        try:
            c = redis.Redis(host="127.0.0.1", port=port)
            if c.ping():
                c.close()
                return proc
        except Exception:
            time.sleep(0.1)
    proc.terminate()
    raise RuntimeError(f"redis-server failed to start on {port}")


def setup_cluster(ports: list[int]) -> list[redis.Redis]:
    clients = [redis.Redis(host="127.0.0.1", port=p, decode_responses=True) for p in ports]
    for c, (lo, hi) in zip(clients, [(0, 5460), (5461, 10922), (10923, 16383)]):
        c.execute_command("CLUSTER", "ADDSLOTS", *range(lo, hi + 1))
    for p in ports[1:]:
        clients[0].execute_command("CLUSTER", "MEET", "127.0.0.1", p)
    deadline = time.time() + 15.0
    while time.time() < deadline:
        if all("cluster_state:ok" in c.execute_command("CLUSTER", "INFO") for c in clients):
            break
        time.sleep(0.1)
    deadline = time.time() + 10.0
    while time.time() < deadline:
        if all(
            all(parse_kv_list(n)["discovered"] == 1
                for n in parse_kv_list(c.execute_command("FT._DEBUG", "CLUSTER"))["nodes"])
            for c in clients
        ):
            return clients
        time.sleep(0.1)
    raise RuntimeError("Cluster RPC peers did not converge")


def percentile(samples: list[float], pct: float) -> float:
    s = sorted(samples)
    idx = min(len(s) - 1, int(len(s) * pct / 100.0))
    return s[idx]


def bench_latency(client: redis.Redis, cmd: list, iters: int) -> tuple[float, float]:
    # Warm-up
    for _ in range(10):
        client.execute_command(*cmd)
    lat_ms: list[float] = []
    for _ in range(iters):
        t0 = time.perf_counter()
        client.execute_command(*cmd)
        lat_ms.append((time.perf_counter() - t0) * 1000.0)
    return percentile(lat_ms, 50.0), percentile(lat_ms, 99.0)


def bench_qps(ports: list[int], cmd: list, threads: int = 6, duration_s: float = 3.0) -> float:
    stop = threading.Event()
    counts = [0] * threads

    def worker(tid: int) -> None:
        c = redis.Redis(host="127.0.0.1", port=ports[tid % len(ports)], decode_responses=False)
        try:
            while not stop.is_set():
                c.execute_command(*cmd)
                counts[tid] += 1
        finally:
            c.close()

    ws = [threading.Thread(target=worker, args=(i,)) for i in range(threads)]
    t0 = time.perf_counter()
    for w in ws:
        w.start()
    time.sleep(duration_s)
    stop.set()
    for w in ws:
        w.join()
    elapsed = time.perf_counter() - t0
    return sum(counts) / elapsed


def main() -> int:
    search_so = sys.argv[1] if len(sys.argv) > 1 else "./build/redis_search.so"
    num_docs = int(os.environ.get("BENCH_DOCS", "12000"))
    workdir = tempfile.mkdtemp(prefix="search_cluster_bench_")
    ports = find_free_ports(4)
    sa_port = ports[0]
    cl_ports = ports[1:]
    procs: list[subprocess.Popen] = []

    try:
        procs.append(start_node(sa_port, search_so, os.path.join(workdir, "sa"), cluster=False))
        for i, p in enumerate(cl_ports):
            procs.append(start_node(p, search_so, os.path.join(workdir, f"c{i}"), cluster=True))

        sa = redis.Redis(host="127.0.0.1", port=sa_port, decode_responses=False)
        cl_nodes = setup_cluster(cl_ports)
        cl_nodes_raw = [redis.Redis(host="127.0.0.1", port=p, decode_responses=False) for p in cl_ports]
        cl_rc = RedisCluster(host="127.0.0.1", port=cl_ports[0], decode_responses=False)

        create_cmd = [
            "FT.CREATE", "bidx", "ON", "HASH", "PREFIX", "1", "b:",
            "SCHEMA",
            "cat", "TAG",
            "price", "NUMERIC", "SORTABLE",
            "title", "TEXT",
            "blob", "TEXT", "NOINDEX",
            "emb", "VECTOR", "HNSW", "10", "TYPE", "FLOAT32", "DIM", "128",
            "DISTANCE_METRIC", "L2", "M", "16", "EF_CONSTRUCTION", "100",
        ]
        assert sa.execute_command(*create_cmd) == b"OK"
        assert cl_nodes[0].execute_command(*create_cmd) == "OK"

        rng = random.Random(7200)
        cats = [f"cat{i}" for i in range(10)]
        vocab = [
            "redis", "cluster", "scatter", "gather", "vector", "hnsw", "search",
            "engine", "distributed", "index", "query", "hybrid", "ranking", "fast",
        ]
        payload_2kb = ("X" * 2048).encode()
        qvec = struct.pack("<128f", *[rng.uniform(-1.0, 1.0) for _ in range(128)])

        print(f"Populating {num_docs} docs (128d HNSW + 2KB payload) on Standalone & 3-node Cluster...")
        pipe_sa = sa.pipeline(transaction=False)
        for i in range(num_docs):
            k = f"b:{i:05d}"
            title = f"{vocab[i % len(vocab)]} {vocab[(i * 7) % len(vocab)]} {vocab[(i * 13) % len(vocab)]}"
            vblob = struct.pack("<128f", *[rng.uniform(-1.0, 1.0) for _ in range(128)])
            mapping = {
                b"cat": cats[i % len(cats)].encode(),
                b"price": str(10 + (i * 37) % 10000).encode(),
                b"title": title.encode(),
                b"blob": payload_2kb,
                b"emb": vblob,
            }
            pipe_sa.hset(k, mapping=mapping)
            cl_rc.hset(k, mapping=mapping)
            if (i + 1) % 500 == 0:
                pipe_sa.execute()
                pipe_sa = sa.pipeline(transaction=False)
        pipe_sa.execute()

        assert sa.execute_command("FT.SEARCH", "bidx", "*", "LIMIT", "0", "0") == [num_docs]
        assert cl_nodes[0].execute_command("FT.SEARCH", "bidx", "*", "LIMIT", "0", "0") == [num_docs]

        queries = [
            ("FT.SEARCH BM25 (LIMIT 0 10)", ["FT.SEARCH", "bidx", "redis cluster", "LIMIT", "0", "10"]),
            ("FT.SEARCH Deep (LIMIT 90 10)", ["FT.SEARCH", "bidx", "*", "SORTBY", "price", "ASC", "LIMIT", "90", "10"]),
            ("FT.SEARCH HNSW KNN-10", ["FT.SEARCH", "bidx", "*=>[KNN 10 @emb $qv]", "PARAMS", "2", "qv", qvec, "DIALECT", "2"]),
            ("FT.HYBRID RRF (K=20, L=10)", [
                "FT.HYBRID", "bidx", "SEARCH", "redis vector", "VSIM", "@emb", "$qv", "KNN", "20",
                "COMBINE", "RRF", "PARAMS", "2", "qv", qvec, "LIMIT", "0", "10", "DIALECT", "2",
            ]),
            ("FT.AGGREGATE GROUPBY 10 cats", [
                "FT.AGGREGATE", "bidx", "*", "GROUPBY", "1", "@cat",
                "REDUCE", "COUNT", "0", "AS", "c", "REDUCE", "AVG", "1", "@price", "AS", "avg_p",
                "SORTBY", "2", "@cat", "ASC",
            ]),
        ]

        print("\n=== Latency (P50 / P99 ms, single-client) & Throughput (6-client QPS) ===")
        print(f"{'Query':<32} | {'1N P50/P99 (ms)':<18} | {'3N P50/P99 (ms)':<18} | {'1N QPS':>8} | {'3N QPS':>8} | {'Scale':>6}")
        print("-" * 105)
        for label, cmd in queries:
            sa_p50, sa_p99 = bench_latency(sa, cmd, iters=80)
            cl_p50, cl_p99 = bench_latency(cl_nodes_raw[0], cmd, iters=80)
            sa_qps = bench_qps([sa_port], cmd, threads=6, duration_s=2.5)
            cl_qps = bench_qps(cl_ports, cmd, threads=6, duration_s=2.5)
            ratio = cl_qps / sa_qps if sa_qps > 0 else 0.0
            print(
                f"{label:<32} | {sa_p50:6.2f} / {sa_p99:6.2f}    | {cl_p50:6.2f} / {cl_p99:6.2f}    | "
                f"{sa_qps:8.1f} | {cl_qps:8.1f} | {ratio:5.2f}x"
            )

        # Query-Then-Fetch bandwidth savings calculation on 2KB docs
        # Naive full-doc scatter: each of 3 shards sends (offset + count) full 2.5KB docs -> 3*(offset+count)*2.5KB
        # QTF: Phase 1 sends lightweight (doc_id + sort_key ~40B) * (offset+count) * 3; Phase 2 fetches only `count` winners (~2.5KB each)
        doc_bytes = 2560  # 2KB blob + 512B 128d vector + metadata
        p1_item_bytes = 40
        for offset, count in [(0, 10), (90, 10), (490, 10)]:
            naive = 3 * (offset + count) * doc_bytes
            qtf = 3 * (offset + count) * p1_item_bytes + count * doc_bytes
            saved_pct = (1.0 - qtf / naive) * 100.0
            print(
                f"Bandwidth LIMIT {offset:>3} {count:<2} (2.5KB doc): naive={naive/1024:.1f} KB, "
                f"QTF={qtf/1024:.1f} KB -> saved {saved_pct:.1f}%"
            )

        sa.close()
        cl_rc.close()
        for c in cl_nodes_raw:
            c.close()
        for c in cl_nodes:
            c.close()
        return 0
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill()
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
