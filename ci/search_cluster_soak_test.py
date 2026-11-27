#!/usr/bin/env python3
"""3-node Redis 7 Cluster Soak Test for gemini-search (+ gemini-json).

Continuously runs mixed writes (HSET, JSON.SET, DEL), distributed queries
(FT.SEARCH, FT.HYBRID, FT.AGGREGATE, cross-node FT.CURSOR), and live slot
MIGRATE across 3 cluster masters, asserting zero crashes, zero duplicate
doc_ids, bounded memory growth, and exact post-soak index/keyspace consistency.

Usage:
    python3 ci/search_cluster_soak_test.py <redis_json.so> <redis_search.so>

Environment variables:
    SOAK_DURATION_SEC   Test duration in seconds (default: 30)
"""

from __future__ import annotations

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
from redis.cluster import RedisCluster
from redis.exceptions import ResponseError


def parse_kv_list(arr: list) -> dict:
    return {arr[i]: arr[i + 1] for i in range(0, len(arr), 2)}


def find_free_ports(count: int) -> list[int]:
    """Find `count` free ports whose cluster bus (port + 10000) and search RPC (port + 20000) are free."""
    ports: list[int] = []
    sockets: list[socket.socket] = []
    candidates = list(range(20000, 30000))
    random.shuffle(candidates)
    try:
        for p in candidates:
            if len(ports) >= count:
                break
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            bus = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            rpc = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                s.bind(("127.0.0.1", p))
                bus.bind(("127.0.0.1", p + 10000))
                rpc.bind(("127.0.0.1", p + 20000))
            except OSError:
                rpc.close()
                bus.close()
                s.close()
                continue
            sockets.extend([s, bus, rpc])
            ports.append(p)
        if len(ports) < count:
            raise RuntimeError(f"Could not find {count} free port triplets in [20000, 29999]")
    finally:
        for s in sockets:
            s.close()
    return ports


def start_cluster_node(port: int, modules: list[str], node_dir: str) -> subprocess.Popen:
    os.makedirs(node_dir, exist_ok=True)
    cmd = [
        "redis-server",
        "--port", str(port),
        "--bind", "127.0.0.1",
        "--dir", node_dir,
        "--dbfilename", f"dump-{port}.rdb",
        "--save", "",
        "--cluster-enabled", "yes",
        "--cluster-config-file", f"nodes-{port}.conf",
        "--cluster-node-timeout", "5000",
        "--loglevel", "warning",
    ]
    for m in modules:
        cmd.extend(["--loadmodule", os.path.abspath(m)])
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
    out, err = proc.communicate(timeout=5)
    raise RuntimeError(f"Cluster node on {port} failed:\n{out.decode()}\n{err.decode()}")


def setup_3node_cluster(ports: list[int]) -> list[redis.Redis]:
    clients = [redis.Redis(host="127.0.0.1", port=p, decode_responses=True) for p in ports]
    ranges = [(0, 5460), (5461, 10922), (10923, 16383)]
    for c, (lo, hi) in zip(clients, ranges):
        c.execute_command("CLUSTER", "ADDSLOTS", *range(lo, hi + 1))
    for p in ports[1:]:
        clients[0].execute_command("CLUSTER", "MEET", "127.0.0.1", p)

    deadline = time.time() + 15.0
    while time.time() < deadline:
        ok = True
        for c in clients:
            info = c.execute_command("CLUSTER", "INFO")
            if "cluster_state:ok" not in info or "cluster_known_nodes:3" not in info:
                ok = False
                break
        if ok:
            break
        time.sleep(0.1)
    else:
        raise RuntimeError("Cluster did not reach cluster_state:ok in time")

    # Wait for full RPC peer discovery
    deadline = time.time() + 10.0
    while time.time() < deadline:
        if all(
            all(parse_kv_list(n)["discovered"] == 1
                for n in parse_kv_list(c.execute_command("FT._DEBUG", "CLUSTER"))["nodes"])
            for c in clients
        ):
            return clients
        time.sleep(0.1)
    raise RuntimeError("Cluster RPC peers did not discover in time")


def total_cluster_memory_bytes(nodes: list[redis.Redis]) -> int:
    return sum(int(n.info("memory")["used_memory"]) for n in nodes)


def migrate_slot(
    nodes: list[redis.Redis],
    ports: list[int],
    node_ids: list[str],
    slot: int,
    src_idx: int,
    dst_idx: int,
) -> int:
    src = nodes[src_idx]
    dst = nodes[dst_idx]
    src_id = node_ids[src_idx]
    dst_id = node_ids[dst_idx]
    dst.execute_command("CLUSTER", "SETSLOT", slot, "IMPORTING", src_id)
    src.execute_command("CLUSTER", "SETSLOT", slot, "MIGRATING", dst_id)
    moved = 0
    while True:
        keys = src.execute_command("CLUSTER", "GETKEYSINSLOT", slot, 100)
        if not keys:
            break
        src.execute_command("MIGRATE", "127.0.0.1", ports[dst_idx], "", 0, 5000, "KEYS", *keys)
        moved += len(keys)
    for n in nodes:
        n.execute_command("CLUSTER", "SETSLOT", slot, "NODE", dst_id)
    return moved


def main() -> int:
    if len(sys.argv) < 3:
        print("Usage: search_cluster_soak_test.py <redis_json.so> <redis_search.so>", file=sys.stderr)
        return 2

    json_so = sys.argv[1]
    search_so = sys.argv[2]
    duration = int(os.environ.get("SOAK_DURATION_SEC", "30"))
    modules = [json_so, search_so]

    ports = find_free_ports(3)
    workdir = tempfile.mkdtemp(prefix="search_cluster_soak_")
    procs: list[subprocess.Popen] = []
    nodes: list[redis.Redis] = []
    nodes_raw: list[redis.Redis] = []
    rc = None
    rc_raw = None

    try:
        for i, p in enumerate(ports):
            procs.append(start_cluster_node(p, modules, os.path.join(workdir, f"n{i}")))
        nodes = setup_3node_cluster(ports)
        nodes_raw = [redis.Redis(host="127.0.0.1", port=p, decode_responses=False) for p in ports]
        rc = RedisCluster(host="127.0.0.1", port=ports[0], decode_responses=True)
        rc_raw = RedisCluster(host="127.0.0.1", port=ports[0], decode_responses=False)
        node_ids = [n.execute_command("CLUSTER", "MYID") for n in nodes]

        print(f"gemini-search cluster soak test: 3 nodes, ports={ports}, duration={duration}s")

        # Create ON HASH (TEXT + TAG + NUMERIC + VECTOR FLAT 8d) and ON JSON indices
        assert nodes[0].execute_command(
            "FT.CREATE", "csoak_h", "ON", "HASH", "PREFIX", "1", "csh:",
            "SCHEMA",
            "cat", "TAG",
            "price", "NUMERIC", "SORTABLE",
            "title", "TEXT",
            "emb", "VECTOR", "FLAT", "6", "TYPE", "FLOAT32", "DIM", "8", "DISTANCE_METRIC", "L2",
        ) == "OK"
        assert nodes[1].execute_command(
            "FT.CREATE", "csoak_j", "ON", "JSON", "PREFIX", "1", "csj:",
            "SCHEMA",
            "$.cat", "AS", "cat", "TAG",
            "$.price", "AS", "price", "NUMERIC", "SORTABLE",
            "$.title", "AS", "title", "TEXT",
        ) == "OK"

        rng = random.Random(727272)
        cats = ["alpha", "beta", "gamma", "delta", "omega"]
        words = ["distributed", "cluster", "vector", "hybrid", "search", "redis", "fast", "index"]
        hash_pool = 300
        json_pool = 150
        live_hash: set[str] = set()
        live_json: set[str] = set()
        qvec = struct.pack("<8f", *[0.15] * 8)

        # Warm-up population
        for i in range(hash_pool):
            k = f"csh:{i:04d}"
            v = struct.pack("<8f", *[rng.uniform(-1.0, 1.0) for _ in range(8)])
            rc_raw.hset(k, mapping={
                b"cat": rng.choice(cats).encode(),
                b"price": str(rng.randint(10, 5000)).encode(),
                b"title": f"{rng.choice(words)} {rng.choice(words)} item{i}".encode(),
                b"emb": v,
            })
            live_hash.add(k)

        for i in range(json_pool):
            k = f"csj:{i:04d}"
            doc = f'{{"cat":"{rng.choice(cats)}","price":{rng.randint(10, 5000)},"title":"{rng.choice(words)} doc{i}"}}'
            rc.execute_command("JSON.SET", k, "$", doc)
            live_json.add(k)

        assert nodes[2].execute_command("FT.SEARCH", "csoak_h", "*", "LIMIT", "0", "0") == [len(live_hash)]
        assert nodes[0].execute_command("FT.SEARCH", "csoak_j", "*", "LIMIT", "0", "0") == [len(live_json)]

        start = time.time()
        end = start + duration
        warmup_mark = start + max(2.0, duration * 0.2)
        mem_baseline: int | None = None

        # Track a small set of slots to bounce between node 0 and node 1 periodically
        mig_slots = list(range(10, 30))
        slot_owner = {s: 0 for s in mig_slots}
        next_mig_at = start + 2.0
        mig_cursor = 0

        ops = {
            "hset": 0, "hdel": 0, "jset": 0, "jdel": 0,
            "search": 0, "hybrid": 0, "agg": 0, "cursor": 0, "migrate": 0,
        }

        while time.time() < end:
            now = time.time()
            if mem_baseline is None and now >= warmup_mark:
                mem_baseline = total_cluster_memory_bytes(nodes)

            # Periodic live slot migration (every ~2s)
            if now >= next_mig_at:
                s = mig_slots[mig_cursor % len(mig_slots)]
                mig_cursor += 1
                src_i = slot_owner[s]
                dst_i = 1 - src_i
                migrate_slot(nodes, ports, node_ids, s, src_i, dst_i)
                slot_owner[s] = dst_i
                ops["migrate"] += 1
                next_mig_at = now + 2.0

            pick = rng.random()
            coord = rng.randrange(3)

            if pick < 0.22:
                # HSET upsert
                i = rng.randrange(hash_pool)
                k = f"csh:{i:04d}"
                v = struct.pack("<8f", *[rng.uniform(-1.0, 1.0) for _ in range(8)])
                rc_raw.hset(k, mapping={
                    b"cat": rng.choice(cats).encode(),
                    b"price": str(rng.randint(10, 5000)).encode(),
                    b"title": f"{rng.choice(words)} {rng.choice(words)} item{i}".encode(),
                    b"emb": v,
                })
                live_hash.add(k)
                ops["hset"] += 1
            elif pick < 0.27:
                # DEL hash key
                i = rng.randrange(hash_pool)
                k = f"csh:{i:04d}"
                rc_raw.delete(k)
                live_hash.discard(k)
                ops["hdel"] += 1
            elif pick < 0.40:
                # JSON.SET upsert
                i = rng.randrange(json_pool)
                k = f"csj:{i:04d}"
                doc = f'{{"cat":"{rng.choice(cats)}","price":{rng.randint(10, 5000)},"title":"{rng.choice(words)} doc{i}"}}'
                rc.execute_command("JSON.SET", k, "$", doc)
                live_json.add(k)
                ops["jset"] += 1
            elif pick < 0.44:
                # DEL json key
                i = rng.randrange(json_pool)
                k = f"csj:{i:04d}"
                rc.delete(k)
                live_json.discard(k)
                ops["jdel"] += 1
            elif pick < 0.68:
                # Distributed FT.SEARCH (Hash or JSON, SORTBY / KNN / text)
                sub = rng.randrange(3)
                if sub == 0:
                    res = nodes[coord].execute_command(
                        "FT.SEARCH", "csoak_h", f"@cat:{{{rng.choice(cats)}}}",
                        "SORTBY", "price", "ASC", "NOCONTENT", "LIMIT", "0", "25",
                    )
                    ids = res[1:]
                    assert len(ids) == len(set(ids)), f"Duplicate doc_ids in Hash FT.SEARCH: {ids}"
                elif sub == 1:
                    res = nodes_raw[coord].execute_command(
                        "FT.SEARCH", "csoak_h", "*=>[KNN 10 @emb $qv]",
                        "PARAMS", "2", "qv", qvec, "NOCONTENT", "DIALECT", "2",
                    )
                    ids = res[1:]
                    assert len(ids) == len(set(ids)), f"Duplicate doc_ids in KNN FT.SEARCH: {ids}"
                else:
                    res = nodes[coord].execute_command(
                        "FT.SEARCH", "csoak_j", rng.choice(words),
                        "SORTBY", "price", "DESC", "NOCONTENT", "LIMIT", "0", "15",
                    )
                    ids = res[1:]
                    assert len(ids) == len(set(ids)), f"Duplicate doc_ids in JSON FT.SEARCH: {ids}"
                ops["search"] += 1
            elif pick < 0.82:
                # Distributed FT.HYBRID RRF
                hres = nodes_raw[coord].execute_command(
                    "FT.HYBRID", "csoak_h", "SEARCH", rng.choice(words),
                    "VSIM", "@emb", "$qv", "KNN", "15",
                    "COMBINE", "RRF", "PARAMS", "2", "qv", qvec,
                    "NOCONTENT", "LIMIT", "0", "15", "DIALECT", "2",
                )
                hids = hres[1:]
                assert len(hids) == len(set(hids)), f"Duplicate doc_ids in FT.HYBRID: {hids}"
                ops["hybrid"] += 1
            elif pick < 0.94:
                # Distributed FT.AGGREGATE Map-Reduce
                ares = nodes[coord].execute_command(
                    "FT.AGGREGATE", "csoak_h", "*",
                    "GROUPBY", "1", "@cat",
                    "REDUCE", "COUNT", "0", "AS", "cnt",
                    "REDUCE", "AVG", "1", "@price", "AS", "avg_p",
                    "SORTBY", "2", "@cat", "ASC",
                )
                assert isinstance(ares, list)
                ops["agg"] += 1
            else:
                # Cross-node FT.CURSOR open -> READ on another node -> DEL on third node
                first_batch, cid = nodes[coord].execute_command(
                    "FT.AGGREGATE", "csoak_h", "*",
                    "GROUPBY", "1", "@cat",
                    "REDUCE", "COUNT", "0", "AS", "cnt",
                    "SORTBY", "2", "@cat", "ASC",
                    "WITHCURSOR", "COUNT", "2", "MAXIDLE", "5000",
                )
                assert isinstance(first_batch, list)
                if cid > 0:
                    other = (coord + 1) % 3
                    third = (coord + 2) % 3
                    _, cid2 = nodes[other].execute_command("FT.CURSOR", "READ", "csoak_h", str(cid), "COUNT", "2")
                    if cid2 > 0:
                        try:
                            nodes[third].execute_command("FT.CURSOR", "DEL", "csoak_h", str(cid2))
                        except ResponseError:
                            pass
                ops["cursor"] += 1

        # Post-soak exact consistency verification across ALL 3 nodes
        for idx, n in enumerate(nodes):
            hc = n.execute_command("FT.SEARCH", "csoak_h", "*", "LIMIT", "0", "0")
            jc = n.execute_command("FT.SEARCH", "csoak_j", "*", "LIMIT", "0", "0")
            assert hc == [len(live_hash)], f"Node {idx} Hash count mismatch: got={hc} exp={[len(live_hash)]}"
            assert jc == [len(live_json)], f"Node {idx} JSON count mismatch: got={jc} exp={[len(live_json)]}"
            dbg = parse_kv_list(n.execute_command("FT._DEBUG", "CLUSTER"))
            assert int(dbg["rpc_server_rejected_frames"]) == 0, f"Node {idx} rejected frames: {dbg}"

        info_h = parse_kv_list(nodes[0].execute_command("FT.INFO", "csoak_h"))
        info_j = parse_kv_list(nodes[1].execute_command("FT.INFO", "csoak_j"))
        assert int(info_h["num_docs"]) == len(live_hash)
        assert int(info_j["num_docs"]) == len(live_json)

        mem_final = total_cluster_memory_bytes(nodes)
        mem_delta_mb = (mem_final - (mem_baseline or mem_final)) / (1024.0 * 1024.0)
        assert mem_delta_mb < 25.0, f"Excessive cluster memory growth during soak: +{mem_delta_mb:.2f} MB"

        total_ops = sum(ops.values())
        print(
            f"PASS: gemini-search cluster soak ({duration}s, {total_ops} ops, "
            f"ops={ops}, live_hash={len(live_hash)}, live_json={len(live_json)}, "
            f"mem_delta={mem_delta_mb:+.2f} MB)"
        )
        return 0
    finally:
        if rc_raw:
            rc_raw.close()
        if rc:
            rc.close()
        for nr in nodes_raw:
            nr.close()
        for n in nodes:
            n.close()
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
