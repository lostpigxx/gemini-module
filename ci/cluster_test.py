#!/usr/bin/env python3
"""
3-node Redis 7 Cluster integration test for gemini-bloom (BF/CF/CMS/TopK/T-Digest)
and gemini-json (JSON.*).

Verifies:
  1. Multi-shard routing via redis-py RedisCluster and MOVED redirection.
  2. Redis 7 dynamic KeySpecs: same-slot multi-key commands succeed; cross-slot
     CMS.MERGE, TDIGEST.MERGE, JSON.MGET, and JSON.MSET fail with CROSSSLOT.
  3. Live slot migration (CLUSTER SETSLOT IMPORTING/MIGRATING + MIGRATE) across
     shards for all 6 module data types (BF, CF, CMS, Top-K, T-Digest, JSON),
     verifying 100% data integrity after RDB transfer.

Usage:
  python3 ci/cluster_test.py [bloom.so] [json.so]
"""

import math
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
from redis.exceptions import ClusterCrossSlotError, MovedError, ResponseError


def find_free_ports(count: int) -> list[int]:
    """Find `count` free ports whose cluster bus (port + 10000) and search RPC (port + 20000) are free."""
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
    # Divide 16384 slots across 3 masters
    ranges = [(0, 5460), (5461, 10922), (10923, 16383)]
    for c, (lo, hi) in zip(clients, ranges):
        c.execute_command("CLUSTER", "ADDSLOTS", *range(lo, hi + 1))

    # Meet all nodes
    for p in ports[1:]:
        clients[0].execute_command("CLUSTER", "MEET", "127.0.0.1", p)

    # Wait for cluster_state:ok on all nodes
    deadline = time.time() + 15.0
    while time.time() < deadline:
        ok = True
        for c in clients:
            info = c.execute_command("CLUSTER", "INFO")
            if "cluster_state:ok" not in info or "cluster_known_nodes:3" not in info:
                ok = False
                break
        if ok:
            return clients
        time.sleep(0.1)
    raise RuntimeError("Cluster did not reach cluster_state:ok in time")


def find_tag_in_slot_range(c: redis.Redis, lo: int, hi: int) -> tuple[str, int]:
    for i in range(10000):
        tag = f"tag{i}"
        slot = int(c.execute_command("CLUSTER", "KEYSLOT", f"{{{tag}}}"))
        if lo <= slot <= hi:
            return tag, slot
    raise RuntimeError(f"Could not find hash tag in [{lo}, {hi}]")


def expect_crossslot(fn, label: str) -> None:
    try:
        fn()
    except ClusterCrossSlotError:
        return
    except ResponseError as e:
        if "CROSSSLOT" in str(e):
            return
        raise AssertionError(f"{label}: expected CROSSSLOT error, got: {e}") from e
    raise AssertionError(f"{label}: expected CROSSSLOT error, but command succeeded")


def test_routing_and_keyspecs(rc: RedisCluster, nodes: list[redis.Redis]) -> None:
    # Find one hash tag on each of the 3 shards
    t0, s0 = find_tag_in_slot_range(nodes[0], 0, 5460)
    t1, s1 = find_tag_in_slot_range(nodes[0], 5461, 10922)
    t2, s2 = find_tag_in_slot_range(nodes[0], 10923, 16383)
    print(f"  Hash tags: {{{t0}}}->slot {s0} (node0), {{{t1}}}->slot {s1} (node1), {{{t2}}}->slot {s2} (node2)")

    # 1. Single-key ops routed across all 3 shards via RedisCluster
    for tag in (t0, t1, t2):
        assert rc.execute_command("BF.ADD", f"{{{tag}}}:bf", "hello") == 1
        assert rc.execute_command("BF.EXISTS", f"{{{tag}}}:bf", "hello") == 1
        assert rc.execute_command("CF.ADD", f"{{{tag}}}:cf", "world") == 1
        assert rc.execute_command("CF.EXISTS", f"{{{tag}}}:cf", "world") == 1
        assert rc.execute_command("JSON.SET", f"{{{tag}}}:js", "$", '{"k":42}') == "OK"
        assert rc.execute_command("JSON.GET", f"{{{tag}}}:js", "$.k") == "[42]"

    # Verify direct MOVED error when hitting wrong shard
    try:
        nodes[1].execute_command("BF.EXISTS", f"{{{t0}}}:bf", "hello")
        raise AssertionError("Expected MovedError when querying node0 key on node1")
    except MovedError as e:
        assert int(e.slot_id) == s0, f"Expected slot {s0}, got {e.slot_id}"
    print("  PASS: Multi-shard routing & MOVED redirection")

    # 2. Same-slot multi-key commands (must succeed)
    # CMS.MERGE same-slot
    rc.execute_command("CMS.INITBYDIM", f"{{{t0}}}:cms1", 100, 5)
    rc.execute_command("CMS.INITBYDIM", f"{{{t0}}}:cms2", 100, 5)
    rc.execute_command("CMS.INITBYDIM", f"{{{t0}}}:cms_dst", 100, 5)
    rc.execute_command("CMS.INCRBY", f"{{{t0}}}:cms1", "a", 3)
    rc.execute_command("CMS.INCRBY", f"{{{t0}}}:cms2", "a", 4)
    assert rc.execute_command(
        "CMS.MERGE", f"{{{t0}}}:cms_dst", 2, f"{{{t0}}}:cms1", f"{{{t0}}}:cms2", "WEIGHTS", 1, 2
    ) == "OK"
    assert rc.execute_command("CMS.QUERY", f"{{{t0}}}:cms_dst", "a") == [11]

    # TDIGEST.MERGE same-slot
    rc.execute_command("TDIGEST.CREATE", f"{{{t1}}}:td1", "COMPRESSION", 100)
    rc.execute_command("TDIGEST.CREATE", f"{{{t1}}}:td2", "COMPRESSION", 100)
    rc.execute_command("TDIGEST.ADD", f"{{{t1}}}:td1", 10, 20, 30)
    rc.execute_command("TDIGEST.ADD", f"{{{t1}}}:td2", 40, 50, 60)
    assert rc.execute_command(
        "TDIGEST.MERGE", f"{{{t1}}}:td_dst", 2, f"{{{t1}}}:td1", f"{{{t1}}}:td2"
    ) == "OK"
    assert float(rc.execute_command("TDIGEST.MIN", f"{{{t1}}}:td_dst")) == 10.0
    assert float(rc.execute_command("TDIGEST.MAX", f"{{{t1}}}:td_dst")) == 60.0

    # JSON.MSET & JSON.MGET same-slot
    assert rc.execute_command(
        "JSON.MSET", f"{{{t2}}}:j1", "$", '{"v":1}', f"{{{t2}}}:j2", "$", '{"v":2}'
    ) == "OK"
    assert rc.execute_command("JSON.MGET", f"{{{t2}}}:j1", f"{{{t2}}}:j2", "$.v") == ["[1]", "[2]"]
    print("  PASS: Same-slot multi-key commands (CMS.MERGE, TDIGEST.MERGE, JSON.MSET/MGET)")

    # 3. Cross-slot multi-key commands (Redis 7 dynamic KeySpecs must enforce CROSSSLOT!)
    expect_crossslot(
        lambda: nodes[0].execute_command(
            "CMS.MERGE", f"{{{t0}}}:cms_dst", 2, f"{{{t0}}}:cms1", f"{{{t1}}}:cms_other"
        ),
        "CMS.MERGE cross-slot",
    )
    expect_crossslot(
        lambda: nodes[1].execute_command(
            "TDIGEST.MERGE", f"{{{t1}}}:td_dst", 2, f"{{{t1}}}:td1", f"{{{t2}}}:td_other"
        ),
        "TDIGEST.MERGE cross-slot",
    )
    expect_crossslot(
        lambda: nodes[0].execute_command("JSON.MGET", f"{{{t0}}}:js", f"{{{t1}}}:js", "$.k"),
        "JSON.MGET cross-slot",
    )
    expect_crossslot(
        lambda: nodes[0].execute_command(
            "JSON.MSET", f"{{{t0}}}:js", "$", '{"k":1}', f"{{{t1}}}:js", "$", '{"k":2}'
        ),
        "JSON.MSET cross-slot",
    )
    print("  PASS: Cross-slot rejection via Redis 7 KeySpecs (CROSSSLOT)")


def test_live_slot_migration(rc: RedisCluster, nodes: list[redis.Redis], ports: list[int]) -> None:
    # Pick a dedicated hash tag on node0 (slots 0..5460) and populate all 6 module types
    mtag, mslot = find_tag_in_slot_range(nodes[0], 100, 5000)
    k_bf = f"{{{mtag}}}:bf"
    k_cf = f"{{{mtag}}}:cf"
    k_cms = f"{{{mtag}}}:cms"
    k_topk = f"{{{mtag}}}:topk"
    k_td = f"{{{mtag}}}:td"
    k_json = f"{{{mtag}}}:json"
    all_keys = [k_bf, k_cf, k_cms, k_topk, k_td, k_json]

    # Populate all 6 data structures on node0
    nodes[0].execute_command("BF.RESERVE", k_bf, "0.01", 1000)
    nodes[0].execute_command("BF.MADD", k_bf, "alpha", "beta", "gamma")

    nodes[0].execute_command("CF.RESERVE", k_cf, 1000)
    nodes[0].execute_command("CF.ADD", k_cf, "c1")
    nodes[0].execute_command("CF.ADD", k_cf, "c2")

    nodes[0].execute_command("CMS.INITBYDIM", k_cms, 200, 5)
    nodes[0].execute_command("CMS.INCRBY", k_cms, "hot", 42, "cold", 7)

    nodes[0].execute_command("TOPK.RESERVE", k_topk, 3, 50, 4, "0.9")
    nodes[0].execute_command("TOPK.INCRBY", k_topk, "king", 100, "queen", 50, "rook", 25)

    nodes[0].execute_command("TDIGEST.CREATE", k_td, "COMPRESSION", 100)
    nodes[0].execute_command("TDIGEST.ADD", k_td, *[float(x) for x in range(1, 101)])

    nodes[0].execute_command(
        "JSON.SET", k_json, "$", '{"user":"alice","scores":[10,20,30],"nested":{"ok":true}}'
    )

    node0_id = nodes[0].execute_command("CLUSTER", "MYID")
    node1_id = nodes[1].execute_command("CLUSTER", "MYID")

    # Standard Redis Cluster live slot migration: node0 -> node1
    assert nodes[1].execute_command("CLUSTER", "SETSLOT", mslot, "IMPORTING", node0_id) == "OK"
    assert nodes[0].execute_command("CLUSTER", "SETSLOT", mslot, "MIGRATING", node1_id) == "OK"

    res = nodes[0].execute_command(
        "MIGRATE", "127.0.0.1", ports[1], "", 0, 5000, "KEYS", *all_keys
    )
    assert res == "OK", f"MIGRATE failed: {res}"

    for n in nodes:
        assert n.execute_command("CLUSTER", "SETSLOT", mslot, "NODE", node1_id) == "OK"

    # Verify node0 redirects with MOVED to node1
    try:
        nodes[0].execute_command("BF.EXISTS", k_bf, "alpha")
        raise AssertionError("Expected MovedError on node0 after migration")
    except MovedError as e:
        assert int(e.slot_id) == mslot and int(e.port) == ports[1], f"Unexpected MovedError: {e}"

    # Verify all 6 keys on node1 directly (100% RDB fidelity after MIGRATE)
    assert nodes[1].execute_command("BF.MEXISTS", k_bf, "alpha", "beta", "gamma", "missing") == [1, 1, 1, 0]
    assert nodes[1].execute_command("CF.MEXISTS", k_cf, "c1", "c2", "c3") == [1, 1, 0]
    assert nodes[1].execute_command("CMS.QUERY", k_cms, "hot", "cold", "none") == [42, 7, 0]
    assert nodes[1].execute_command("TOPK.QUERY", k_topk, "king", "queen", "none") == [1, 1, 0]
    assert float(nodes[1].execute_command("TDIGEST.MIN", k_td)) == 1.0
    assert float(nodes[1].execute_command("TDIGEST.MAX", k_td)) == 100.0
    q50 = float(nodes[1].execute_command("TDIGEST.QUANTILE", k_td, "0.5")[0])
    assert 45.0 <= q50 <= 56.0, f"Unexpected migrated T-Digest median: {q50}"
    assert nodes[1].execute_command("JSON.GET", k_json, "$.user") == '["alice"]'
    assert nodes[1].execute_command("JSON.GET", k_json, "$.scores[1]") == "[20]"

    # Verify RedisCluster client transparently follows MOVED to node1
    assert rc.execute_command("BF.EXISTS", k_bf, "alpha") == 1
    assert rc.execute_command("JSON.GET", k_json, "$.nested.ok") == "[true]"
    print(f"  PASS: Live slot {mslot} migration (node0 -> node1) for all 6 module types (BF/CF/CMS/TopK/T-Digest/JSON)")


def kv_list_to_dict(raw) -> dict:
    if isinstance(raw, dict):
        return raw
    return {raw[i]: raw[i + 1] for i in range(0, len(raw), 2)}


def test_search_cluster_phase66(nodes: list[redis.Redis], ports: list[int]) -> None:
    """Verify gemini-search Phase 66: topology discovery, Cluster Bus hello, and binary RPC PING/PONG."""
    t0 = time.time()
    deadline = t0 + 3.0
    discovered_all = False
    last_infos = []

    while time.time() < deadline:
        last_infos = [kv_list_to_dict(n.execute_command("FT._DEBUG", "CLUSTER")) for n in nodes]
        ok = True
        for info in last_infos:
            if (
                info.get("mode") != "cluster"
                or info.get("rpc_server_running") != 1
                or info.get("scatter_enabled") != 1
                or info.get("master_count") != 3
                or info.get("cluster_size") != 3
            ):
                ok = False
                break
            node_entries = [kv_list_to_dict(x) for x in info.get("nodes", [])]
            if len(node_entries) != 3 or any(
                ne.get("discovered") != 1 or int(ne.get("rpc_port", 0)) <= 0 for ne in node_entries
            ):
                ok = False
                break
        if ok:
            discovered_all = True
            break
        time.sleep(0.05)

    elapsed_ms = (time.time() - t0) * 1000.0
    assert discovered_all, f"Search cluster RPC discovery timed out; last state: {last_infos}"

    # Verify each node's discovered rpc_port table matches actual bound ports on all peers
    my_rpc_by_id = {info["my_node_id"]: int(info["my_rpc_port"]) for info in last_infos}
    assert len(my_rpc_by_id) == 3
    for idx, info in enumerate(last_infos):
        assert int(info["my_rpc_port"]) > 0
        assert int(info["my_redis_port"]) == ports[idx]
        for raw_ne in info["nodes"]:
            ne = kv_list_to_dict(raw_ne)
            assert ne["role"] == "master"
            assert int(ne["healthy"]) == 1
            assert int(ne["discovered"]) == 1
            assert int(ne["rpc_port"]) == my_rpc_by_id[ne["node_id"]]

    print(
        f"  PASS: gemini-search Phase 66 Cluster Bus topology & RPC port discovery "
        f"({elapsed_ms:.1f}ms, ports={list(my_rpc_by_id.values())})"
    )

    # Verify full-mesh binary RPC PING/PONG from every node to all remote peers
    for idx, n in enumerate(nodes):
        assert n.execute_command("FT._DEBUG") == "GeminiSearch OK"
        pong_raw = n.execute_command("FT._DEBUG", "CLUSTER", "PING", "2000")
        assert len(pong_raw) == 2, f"Expected 2 remote peer pongs from node{idx}, got {pong_raw}"
        for entry_raw in pong_raw:
            entry = kv_list_to_dict(entry_raw)
            assert entry["status"] == "OK", f"Node{idx} RPC ping failed: {entry}"
            assert int(entry["rpc_port"]) == my_rpc_by_id[entry["node_id"]]
            assert entry["error"] == ""

    # Verify server/client RPC counters incremented
    after_info = kv_list_to_dict(nodes[0].execute_command("FT._DEBUG", "CLUSTER"))
    assert int(after_info["rpc_server_requests_handled"]) >= 2
    assert int(after_info["rpc_client_requests_succeeded"]) >= 2
    print("  PASS: gemini-search Phase 66 full-mesh binary RPC PING/PONG (6/6 peer links OK)")

    # Verify FT.CONFIG CLUSTER_* dynamic toggle & validation
    assert nodes[0].execute_command("FT.CONFIG", "SET", "CLUSTER_ENABLED", "0") == "OK"
    disabled_info = kv_list_to_dict(nodes[0].execute_command("FT._DEBUG", "CLUSTER"))
    assert int(disabled_info["cluster_enabled"]) == 0
    assert int(disabled_info["scatter_enabled"]) == 0

    assert nodes[0].execute_command("FT.CONFIG", "SET", "CLUSTER_ENABLED", "1") == "OK"
    enabled_info = kv_list_to_dict(nodes[0].execute_command("FT._DEBUG", "CLUSTER"))
    assert int(enabled_info["cluster_enabled"]) == 1
    assert int(enabled_info["scatter_enabled"]) == 1

    for bad_cmd in (
        ("FT.CONFIG", "SET", "CLUSTER_ENABLED", "invalid"),
        ("FT.CONFIG", "SET", "CLUSTER_QUERY_TIMEOUT_MS", "0"),
        ("FT.CONFIG", "SET", "CLUSTER_RPC_PORT_OFFSET", "60000"),
        ("FT.CONFIG", "SET", "CLUSTER_MAX_SCATTER_LIMIT", "-1"),
    ):
        try:
            nodes[0].execute_command(*bad_cmd)
            raise AssertionError(f"Expected ResponseError for {bad_cmd}")
        except ResponseError:
            pass
    print("  PASS: gemini-search Phase 66 FT.CONFIG CLUSTER_* parameters & dynamic toggle")


def test_search_cluster_phase67(
    rc: RedisCluster,
    nodes: list[redis.Redis],
    procs: list[subprocess.Popen],
    ports: list[int],
    modules: list[str],
    workdir: str,
) -> None:
    """Verify gemini-search Phase 67: DDL broadcast, rollback, aux_save2 restart recovery, and 4th-node sync."""
    # 1. Pre-populate Hash and JSON docs across all 3 shards BEFORE FT.CREATE
    t0, _ = find_tag_in_slot_range(nodes[0], 0, 5460)
    t1, _ = find_tag_in_slot_range(nodes[0], 5461, 10922)
    t2, _ = find_tag_in_slot_range(nodes[0], 10923, 16383)

    v0 = struct.pack("<4f", 1.0, 0.0, 0.0, 0.0)
    v1 = struct.pack("<4f", 0.0, 1.0, 0.0, 0.0)
    v2 = struct.pack("<4f", 0.0, 0.0, 1.0, 0.0)

    nodes_raw = [redis.Redis(host="127.0.0.1", port=p, decode_responses=False) for p in ports]
    try:
        nodes_raw[0].hset(f"p67h:{{{t0}}}:1", mapping={"title": "redis cluster", "price": "10", "cat": "db", "vec": v0})
        nodes_raw[1].hset(f"p67h:{{{t1}}}:2", mapping={"title": "vector hnsw", "price": "20", "cat": "ai", "vec": v1})
        nodes_raw[2].hset(f"p67h:{{{t2}}}:3", mapping={"title": "search engine", "price": "30", "cat": "db", "vec": v2})
    finally:
        for nr in nodes_raw:
            nr.close()

    rc.execute_command("JSON.SET", f"p67j:{{{t0}}}:1", "$", '{"name":"alpha","score":11}')
    rc.execute_command("JSON.SET", f"p67j:{{{t1}}}:2", "$", '{"name":"beta","score":22}')
    rc.execute_command("JSON.SET", f"p67j:{{{t2}}}:3", "$", '{"name":"gamma","score":33}')

    # 2. FT.CREATE from node0 -> immediately visible & auto-indexed on ALL 3 nodes, NO bare DB key!
    assert nodes[0].execute_command(
        "FT.CREATE", "p67_hash", "ON", "HASH", "PREFIX", "1", "p67h:",
        "SCHEMA", "title", "TEXT", "price", "NUMERIC", "cat", "TAG",
        "vec", "VECTOR", "HNSW", "6", "TYPE", "FLOAT32", "DIM", "4", "DISTANCE_METRIC", "L2",
    ) == "OK"

    assert nodes[1].execute_command(
        "FT.CREATE", "p67_json", "ON", "JSON", "PREFIX", "1", "p67j:",
        "SCHEMA", "$.name", "AS", "name", "TEXT", "$.score", "AS", "score", "NUMERIC",
    ) == "OK"

    total_hash_docs = 0
    total_json_docs = 0
    for idx, n in enumerate(nodes):
        listed = sorted(n.execute_command("FT._LIST"))
        assert listed == ["p67_hash", "p67_json"], f"Node{idx} FT._LIST mismatch: {listed}"
        # Obstacle 1 check: no bare GmSearch- DB keys on any shard (KEYS inspects local DB regardless of slot)
        assert n.execute_command("KEYS", "p67_hash") == []
        assert n.execute_command("KEYS", "p67_json") == []
        hinfo = kv_list_to_dict(n.execute_command("FT.INFO", "p67_hash"))
        jinfo = kv_list_to_dict(n.execute_command("FT.INFO", "p67_json"))
        assert int(hinfo["num_docs"]) == 1, f"Node{idx} expected 1 hash doc, got {hinfo}"
        assert int(jinfo["num_docs"]) == 1, f"Node{idx} expected 1 json doc, got {jinfo}"
        total_hash_docs += int(hinfo["num_docs"])
        total_json_docs += int(jinfo["num_docs"])
    assert rc.execute_command("EXISTS", "p67_hash") == 0
    assert rc.execute_command("EXISTS", "p67_json") == 0
    assert total_hash_docs == 3 and total_json_docs == 3

    # 3. Broadcast FT.ALTER, FT.ALIAS*, FT.SYNUPDATE, FT.DICTADD across all 3 nodes
    assert nodes[2].execute_command("FT.ALTER", "p67_hash", "SCHEMA", "ADD", "brand", "TAG") == "OK"
    assert nodes[0].execute_command("FT.ALIASADD", "p67_alias", "p67_hash") == "OK"
    assert nodes[1].execute_command("FT.SYNUPDATE", "p67_hash", "g1", "fast", "quick") == "OK"
    assert nodes[2].execute_command("FT.DICTADD", "p67_dict", "redis", "gemini") == 2

    for idx, n in enumerate(nodes):
        syn = n.execute_command("FT.SYNDUMP", "p67_alias")
        assert "g1" in syn and "fast" in syn and "quick" in syn, f"Node{idx} syndump mismatch: {syn}"
        dd = sorted(n.execute_command("FT.DICTDUMP", "p67_dict"))
        assert dd == ["gemini", "redis"], f"Node{idx} dictdump mismatch: {dd}"

    # 4. FT.ADD / FT.DEL rejected in multi-shard cluster mode
    for bad in (
        ("FT.ADD", "p67_hash", "doc:bad", "FIELDS", "title", "x"),
        ("FT.DEL", "p67_hash", "doc:bad"),
    ):
        try:
            nodes[0].execute_command(*bad)
            raise AssertionError(f"Expected ResponseError for {bad} in cluster mode")
        except ResponseError as e:
            assert "not supported in cluster mode" in str(e), f"Unexpected error: {e}"
    print("  PASS: gemini-search Phase 67 DDL broadcast (CREATE/ALTER/ALIAS/SYN/DICT) & no-bare-key isolation")

    # 5. Simulate partial failure on node2 (pre-existing conflict) -> verify rollback on node0 & node1
    assert nodes[2].execute_command("FT.CONFIG", "SET", "CLUSTER_ENABLED", "0") == "OK"
    assert nodes[2].execute_command(
        "FT.CREATE", "p67_conflict", "ON", "HASH", "PREFIX", "1", "cf:", "SCHEMA", "a", "TEXT"
    ) == "OK"
    assert nodes[2].execute_command("FT.CONFIG", "SET", "CLUSTER_ENABLED", "1") == "OK"

    try:
        nodes[0].execute_command(
            "FT.CREATE", "p67_conflict", "ON", "HASH", "PREFIX", "1", "cf:", "SCHEMA", "a", "TEXT"
        )
        raise AssertionError("Expected conflict DDL error from node2")
    except ResponseError as e:
        assert "index already exists" in str(e) or "cluster DDL failed" in str(e)

    # Node0 and Node1 MUST have zero dirty p67_conflict index after automatic rollback
    assert "p67_conflict" not in nodes[0].execute_command("FT._LIST")
    assert "p67_conflict" not in nodes[1].execute_command("FT._LIST")

    # Clean up simulated local conflict on node2
    assert nodes[2].execute_command("FT.CONFIG", "SET", "CLUSTER_ENABLED", "0") == "OK"
    assert nodes[2].execute_command("FT.DROPINDEX", "p67_conflict") == "OK"
    assert nodes[2].execute_command("FT.CONFIG", "SET", "CLUSTER_ENABLED", "1") == "OK"
    print("  PASS: gemini-search Phase 67 two-step DDL compensation rollback on shard conflict")

    # 6. Node1 SAVE + restart -> verify aux_save2 (encver 2) restores Schema, HNSW & local docs
    assert nodes[1].save() is True
    nodes[1].close()
    procs[1].terminate()
    procs[1].wait(timeout=5)

    procs[1] = start_cluster_node(ports[1], modules, os.path.join(workdir, "node1"))
    nodes[1] = redis.Redis(host="127.0.0.1", port=ports[1], decode_responses=True)

    # Wait briefly for node1 to rejoin cluster_state:ok and rediscover peers
    deadline = time.time() + 10.0
    while time.time() < deadline:
        info = nodes[1].execute_command("CLUSTER", "INFO")
        dbg = kv_list_to_dict(nodes[1].execute_command("FT._DEBUG", "CLUSTER"))
        if "cluster_state:ok" in info and int(dbg.get("master_count", 0)) == 3:
            nes = [kv_list_to_dict(x) for x in dbg.get("nodes", [])]
            if len(nes) == 3 and all(int(ne.get("discovered", 0)) == 1 for ne in nes):
                break
        time.sleep(0.05)

    assert sorted(nodes[1].execute_command("FT._LIST")) == ["p67_hash", "p67_json"]
    n1_hinfo = kv_list_to_dict(nodes[1].execute_command("FT.INFO", "p67_hash"))
    assert int(n1_hinfo["num_docs"]) == 1, f"Restarted node1 lost hash doc: {n1_hinfo}"
    # ON JSON recovery triggers right after cluster_state:ok (<= 50ms timer)
    n1_jinfo = {}
    j_deadline = time.time() + 2.0
    while time.time() < j_deadline:
        n1_jinfo = kv_list_to_dict(nodes[1].execute_command("FT.INFO", "p67_json"))
        if int(n1_jinfo["num_docs"]) == 1:
            break
        time.sleep(0.05)
    assert int(n1_jinfo["num_docs"]) == 1, f"Restarted node1 lost json doc: {n1_jinfo}"
    assert sorted(nodes[1].execute_command("FT.DICTDUMP", "p67_dict")) == ["gemini", "redis"]
    assert "fast" in nodes[1].execute_command("FT.SYNDUMP", "p67_alias")
    # Verify HNSW KNN still works on restarted node1 (deserialized directly from aux_save2)
    n1_raw = redis.Redis(host="127.0.0.1", port=ports[1], decode_responses=False)
    try:
        knn = n1_raw.execute_command(
            "FT.SEARCH", "p67_hash", "*=>[KNN 1 @vec $qv]", "PARAMS", "2", "qv", v1, "NOCONTENT", "DIALECT", "2"
        )
        assert knn[0] == 1 and knn[1] == f"p67h:{{{t1}}}:2".encode(), f"Unexpected KNN after restart: {knn}"
    finally:
        n1_raw.close()
    print("  PASS: gemini-search Phase 67 node1 SAVE + restart aux_save2 (encver 2) + HNSW recovery")

    # 7. Dynamic 4th node join -> automatic kRpcSchemaSyncReq/Resp self-healing
    port4 = find_free_ports(1)[0]
    proc4 = start_cluster_node(port4, modules, os.path.join(workdir, "node3"))
    node4 = redis.Redis(host="127.0.0.1", port=port4, decode_responses=True)
    node4_id = node4.execute_command("CLUSTER", "MYID")
    try:
        node4.execute_command("CLUSTER", "MEET", "127.0.0.1", ports[0])
        deadline = time.time() + 10.0
        synced = False
        while time.time() < deadline:
            try:
                listed4 = sorted(node4.execute_command("FT._LIST"))
                if listed4 == ["p67_hash", "p67_json"]:
                    dd4 = sorted(node4.execute_command("FT.DICTDUMP", "p67_dict"))
                    syn4 = node4.execute_command("FT.SYNDUMP", "p67_alias")
                    if dd4 == ["gemini", "redis"] and "fast" in syn4:
                        synced = True
                        break
            except Exception:
                pass
            time.sleep(0.1)
        assert synced, f"4th node failed to auto-sync schema; FT._LIST={node4.execute_command('FT._LIST')}"
        print("  PASS: gemini-search Phase 67 dynamic 4th-node join automatic SchemaSync self-healing")

        # 8. Broadcast FT.DROPINDEX DD across all masters (including 4th node)
        assert nodes[0].execute_command("FT.DROPINDEX", "p67_hash", "DD") == "OK"
        assert nodes[0].execute_command("FT.DROPINDEX", "p67_json") == "OK"
        for idx, n in enumerate(nodes + [node4]):
            assert n.execute_command("FT._LIST") == [], f"Node{idx} still has indices after DROPINDEX"
        assert nodes[0].execute_command("EXISTS", f"p67h:{{{t0}}}:1") == 0
        assert nodes[1].execute_command("EXISTS", f"p67h:{{{t1}}}:2") == 0
        assert nodes[2].execute_command("EXISTS", f"p67h:{{{t2}}}:3") == 0
        print("  PASS: gemini-search Phase 67 FT.DROPINDEX DD broadcast across all shards")
    finally:
        node4.close()
        proc4.terminate()
        try:
            proc4.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc4.kill()
        for n in nodes:
            try:
                n.execute_command("CLUSTER", "FORGET", node4_id)
            except Exception:
                pass


def start_standalone_node(port: int, modules: list[str], node_dir: str) -> subprocess.Popen:
    os.makedirs(node_dir, exist_ok=True)
    cmd = [
        "redis-server",
        "--port", str(port),
        "--bind", "127.0.0.1",
        "--dir", node_dir,
        "--dbfilename", "",
        "--save", "",
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
            time.sleep(0.05)
    proc.terminate()
    out, err = proc.communicate(timeout=5)
    raise RuntimeError(f"Standalone node on {port} failed:\n{out.decode()}\n{err.decode()}")


def make_unit_vec(dim: int, seed: int) -> bytes:
    rng = random.Random(seed)
    vals = [rng.gauss(0.0, 1.0) for _ in range(dim)]
    norm = math.sqrt(sum(x * x for x in vals)) or 1.0
    vals = [x / norm for x in vals]
    return struct.pack(f"<{dim}f", *vals)


def test_search_cluster_phase68_golden_parity(
    rc: RedisCluster,
    nodes: list[redis.Redis],
    ports: list[int],
    modules: list[str],
    workdir: str,
) -> None:
    """Phase 68 golden parity: 3,000 identical Hash+JSON+128d/768d vector docs on Standalone vs 3-node Cluster."""
    sa_port = find_free_ports(1)[0]
    sa_proc = start_standalone_node(sa_port, modules, os.path.join(workdir, "standalone_p68"))
    sa = redis.Redis(host="127.0.0.1", port=sa_port, decode_responses=True)
    sa_raw = redis.Redis(host="127.0.0.1", port=sa_port, decode_responses=False)
    rc_raw = RedisCluster(host="127.0.0.1", port=ports[0], decode_responses=False)
    nodes_raw = [redis.Redis(host="127.0.0.1", port=p, decode_responses=False) for p in ports]
    try:
        # 1. Create identical schemas on Standalone and 3-node Cluster
        hash_schema = [
            "FT.CREATE", "p68_hash", "ON", "HASH", "PREFIX", "1", "p68h:",
            "SCHEMA",
            "title", "TEXT", "WEIGHT", "2.0",
            "body", "TEXT",
            "cat", "TAG",
            "price", "NUMERIC", "SORTABLE",
            "sku", "TEXT", "SORTABLE",
            "loc", "GEO",
            "v_flat", "VECTOR", "FLAT", "6", "TYPE", "FLOAT32", "DIM", "128", "DISTANCE_METRIC", "L2",
            "v_hnsw", "VECTOR", "HNSW", "10", "TYPE", "FLOAT32", "DIM", "128", "DISTANCE_METRIC", "COSINE",
            "M", "16", "EF_CONSTRUCTION", "128",
            "v_sq8", "VECTOR", "HNSW", "12", "TYPE", "FLOAT32", "DIM", "128", "DISTANCE_METRIC", "L2",
            "QUANTIZATION", "SQ8", "M", "16", "EF_CONSTRUCTION", "128",
            "v_bq", "VECTOR", "HNSW", "16", "TYPE", "FLOAT32", "DIM", "128", "DISTANCE_METRIC", "COSINE",
            "QUANTIZATION", "BQ", "RERANK", "FP16", "OVERSAMPLE", "8", "M", "16", "EF_CONSTRUCTION", "128",
        ]
        json_schema = [
            "FT.CREATE", "p68_json", "ON", "JSON", "PREFIX", "1", "p68j:",
            "SCHEMA",
            "$.title", "AS", "title", "TEXT",
            "$.cat", "AS", "cat", "TAG",
            "$.price", "AS", "price", "NUMERIC", "SORTABLE",
            "$.rating", "AS", "rating", "NUMERIC", "SORTABLE",
        ]
        vec768_schema = [
            "FT.CREATE", "p68_v768", "ON", "HASH", "PREFIX", "1", "p68v:",
            "SCHEMA",
            "cat", "TAG",
            "emb", "VECTOR", "HNSW", "10", "TYPE", "FLOAT32", "DIM", "768", "DISTANCE_METRIC", "COSINE",
            "M", "16", "EF_CONSTRUCTION", "100",
        ]

        for schema_cmd in (hash_schema, json_schema, vec768_schema):
            assert sa.execute_command(*schema_cmd) == "OK"
            assert nodes[0].execute_command(*schema_cmd) == "OK"

        # 2. Populate 3,000 identical docs (2,000 Hash + 1,000 JSON) + 64 x 768d docs
        cats = ["electronics", "books", "garden", "sports", "toys"]
        adjs = ["fast", "compact", "durable", "smart", "classic", "modern", "portable", "quiet"]
        nouns = ["router", "engine", "sensor", "widget", "camera", "speaker", "battery", "display"]

        # Store a few known vectors for KNN / VECTOR_RANGE queries
        saved_v128: dict[int, bytes] = {}
        saved_v768: dict[int, bytes] = {}

        sa_pipe = sa_raw.pipeline(transaction=False)
        for i in range(2000):
            key = f"p68h:{i:04d}"
            cat = cats[i % len(cats)]
            adj = adjs[i % len(adjs)]
            noun = nouns[(i // len(adjs)) % len(nouns)]
            title = f"{adj} {noun} model{i % 25}"
            body = (
                f"high performance {adj} {noun} built for distributed redis search cluster "
                f"batch{i % 10} reliable low latency indexing"
            )
            price = str((i * 37) % 1000 + (i % 7) * 0.25)
            sku = f"sku_{cat[:3]}_{i:04d}"
            lon = -122.5 + (i % 50) * 0.01
            lat = 37.5 + ((i // 50) % 40) * 0.01
            v = make_unit_vec(128, 10000 + i)
            if i in (0, 42, 777):
                saved_v128[i] = v
            mapping = {
                b"title": title.encode(),
                b"body": body.encode(),
                b"cat": cat.encode(),
                b"price": price.encode(),
                b"sku": sku.encode(),
                b"loc": f"{lon:.4f},{lat:.4f}".encode(),
                b"v_flat": v,
                b"v_hnsw": v,
                b"v_sq8": v,
                b"v_bq": v,
            }
            sa_pipe.hset(key, mapping=mapping)
            rc_raw.hset(key, mapping=mapping)
            if (i + 1) % 250 == 0:
                sa_pipe.execute()
                sa_pipe = sa_raw.pipeline(transaction=False)
        sa_pipe.execute()

        sa_jpipe = sa.pipeline(transaction=False)
        for i in range(1000):
            key = f"p68j:{i:04d}"
            cat = cats[i % len(cats)]
            adj = adjs[i % len(adjs)]
            noun = nouns[i % len(nouns)]
            doc_json = (
                f'{{"title":"{adj} {noun} json{i % 20}","cat":"{cat}",'
                f'"price":{(i * 19) % 500},"rating":{(i % 50) / 10.0}}}'
            )
            sa_jpipe.execute_command("JSON.SET", key, "$", doc_json)
            rc.execute_command("JSON.SET", key, "$", doc_json)
            if (i + 1) % 250 == 0:
                sa_jpipe.execute()
                sa_jpipe = sa.pipeline(transaction=False)
        sa_jpipe.execute()

        for i in range(64):
            key = f"p68v:{i:03d}"
            v768 = make_unit_vec(768, 90000 + i)
            if i == 17:
                saved_v768[i] = v768
            mapping = {b"cat": cats[i % 2].encode(), b"emb": v768}
            sa_raw.hset(key, mapping=mapping)
            rc_raw.hset(key, mapping=mapping)

        # Verify all 3,000 docs landed across all 3 cluster shards
        shard_hash_counts = [
            int(kv_list_to_dict(n.execute_command("FT.INFO", "p68_hash"))["num_docs"])
            for n in nodes
        ]
        shard_json_counts = [
            int(kv_list_to_dict(n.execute_command("FT.INFO", "p68_json"))["num_docs"])
            for n in nodes
        ]
        assert sum(shard_hash_counts) == 2000 and all(c > 400 for c in shard_hash_counts), shard_hash_counts
        assert sum(shard_json_counts) == 1000 and all(c > 200 for c in shard_json_counts), shard_json_counts

        # Helper to compare standalone vs cluster exact RESP replies
        def assert_exact_parity(label: str, cmd_args: list, client_idx: int = 0, use_rc: bool = False):
            exp = sa_raw.execute_command(*cmd_args)
            if use_rc:
                got = rc_raw.execute_command(*cmd_args)
            else:
                got = nodes_raw[client_idx % 3].execute_command(*cmd_args)
            assert got == exp, f"Parity mismatch on '{label}':\n  expected={exp[:12]}\n  got     ={got[:12]}"

        # 3. Execute 32+ golden parity queries!
        q_idx = 0
        # (1-4) Pure counts (LIMIT 0 0) across Hash, JSON, filtered, and GEOFILTER
        assert_exact_parity("hash_count_all", ["FT.SEARCH", "p68_hash", "*", "LIMIT", "0", "0"], 0)
        assert_exact_parity("hash_count_tag", ["FT.SEARCH", "p68_hash", "@cat:{electronics|books}", "LIMIT", "0", "0"], 1)
        assert_exact_parity("json_count_all", ["FT.SEARCH", "p68_json", "*", "LIMIT", "0", "0"], 2)
        assert_exact_parity("hash_count_geofilter", ["FT.SEARCH", "p68_hash", "*", "GEOFILTER", "loc", "-122.4", "37.6", "15", "km", "LIMIT", "0", "0"], 0, use_rc=True)
        q_idx += 4

        # (5-12) SORTBY numeric ASC/DESC & string ASC/DESC with pagination, RETURN AS, WITHSORTKEYS, NOCONTENT
        assert_exact_parity("sort_price_asc_p1", ["FT.SEARCH", "p68_hash", "*", "SORTBY", "price", "ASC", "RETURN", "4", "title", "price", "AS", "cost", "LIMIT", "0", "15"], 0)
        assert_exact_parity("sort_price_desc_p2", ["FT.SEARCH", "p68_hash", "*", "SORTBY", "price", "DESC", "RETURN", "2", "title", "price", "LIMIT", "25", "15"], 1)
        assert_exact_parity("sort_price_deep", ["FT.SEARCH", "p68_hash", "*", "SORTBY", "price", "ASC", "NOCONTENT", "LIMIT", "1985", "20"], 2)
        assert_exact_parity("sort_price_oob", ["FT.SEARCH", "p68_hash", "*", "SORTBY", "price", "ASC", "NOCONTENT", "LIMIT", "2500", "10"], 0)
        assert_exact_parity("sort_sku_asc_withsortkeys", ["FT.SEARCH", "p68_hash", "@cat:{sports}", "SORTBY", "sku", "ASC", "WITHSORTKEYS", "RETURN", "1", "sku", "LIMIT", "10", "12"], 1)
        assert_exact_parity("sort_sku_desc_nocontent", ["FT.SEARCH", "p68_hash", "@cat:{garden}", "SORTBY", "sku", "DESC", "WITHSORTKEYS", "NOCONTENT", "LIMIT", "5", "10"], 2)
        assert_exact_parity("sort_filter_excl", ["FT.SEARCH", "p68_hash", "@cat:{electronics}", "FILTER", "price", "(100", "500", "SORTBY", "price", "ASC", "RETURN", "2", "cat", "price", "LIMIT", "0", "20"], 0, use_rc=True)
        assert_exact_parity("sort_geofilter_combo", ["FT.SEARCH", "p68_hash", "@cat:{books|toys}", "GEOFILTER", "loc", "-122.35", "37.65", "20", "km", "SORTBY", "price", "DESC", "RETURN", "2", "sku", "price", "LIMIT", "0", "15"], 1)
        q_idx += 8

        # (13-20) Full-text, phrase, boolean AND/OR/NOT, INFIELDS, INKEYS, HIGHLIGHT, SUMMARIZE, DOCSCORE+EXPLAINSCORE
        assert_exact_parity("text_sortby_hl", ["FT.SEARCH", "p68_hash", "fast router", "SORTBY", "sku", "ASC", "RETURN", "2", "title", "body", "HIGHLIGHT", "FIELDS", "1", "title", "TAGS", "<em>", "</em>", "LIMIT", "0", "10"], 2)
        assert_exact_parity("text_summarize_hl", ["FT.SEARCH", "p68_hash", "distributed cluster", "SORTBY", "sku", "ASC", "RETURN", "1", "body", "SUMMARIZE", "FIELDS", "1", "body", "FRAGS", "1", "LEN", "6", "SEPARATOR", "...", "HIGHLIGHT", "FIELDS", "1", "body", "LIMIT", "0", "8"], 0)
        assert_exact_parity("phrase_search", ["FT.SEARCH", "p68_hash", '"low latency indexing"', "SORTBY", "sku", "ASC", "NOCONTENT", "LIMIT", "0", "20"], 1)
        assert_exact_parity("bool_and_not", ["FT.SEARCH", "p68_hash", "@cat:{electronics} -@title:compact @price:[50 400]", "SORTBY", "price", "ASC", "RETURN", "2", "title", "price", "LIMIT", "0", "15"], 2)
        assert_exact_parity("infields_title", ["FT.SEARCH", "p68_hash", "smart", "INFIELDS", "1", "title", "SORTBY", "sku", "ASC", "NOCONTENT", "LIMIT", "0", "15"], 0)
        assert_exact_parity("inkeys_subset", ["FT.SEARCH", "p68_hash", "*", "INKEYS", "5", "p68h:0001", "p68h:0500", "p68h:1000", "p68h:1500", "p68h:1999", "SORTBY", "sku", "ASC", "RETURN", "1", "title"], 1)
        assert_exact_parity("docscore_withscores_explain", ["FT.SEARCH", "p68_hash", "fast engine", "WITHSCORES", "EXPLAINSCORE", "SCORER", "DOCSCORE", "RETURN", "1", "title", "LIMIT", "0", "10"], 2)
        assert_exact_parity("withscores_sortby_override", ["FT.SEARCH", "p68_hash", "smart camera", "WITHSCORES", "SCORER", "DOCSCORE", "SORTBY", "price", "ASC", "RETURN", "2", "title", "price", "LIMIT", "0", "10"], 0)
        q_idx += 8

        # (21-24) ON JSON queries (SORTBY, RETURN, HIGHLIGHT, FILTER, LIMIT)
        assert_exact_parity("json_sort_price_asc", ["FT.SEARCH", "p68_json", "@cat:{electronics}", "SORTBY", "price", "ASC", "RETURN", "3", "title", "price", "rating", "LIMIT", "0", "15"], 1)
        assert_exact_parity("json_sort_rating_desc", ["FT.SEARCH", "p68_json", "compact", "SORTBY", "rating", "DESC", "HIGHLIGHT", "FIELDS", "1", "title", "RETURN", "2", "title", "rating", "LIMIT", "5", "10"], 2)
        assert_exact_parity("json_filter_count", ["FT.SEARCH", "p68_json", "@cat:{books|sports}", "FILTER", "price", "100", "300", "LIMIT", "0", "0"], 0, use_rc=True)
        assert_exact_parity("json_return_alias", ["FT.SEARCH", "p68_json", "*", "SORTBY", "price", "DESC", "RETURN", "4", "title", "AS", "heading", "price", "LIMIT", "10", "10"], 1)
        q_idx += 4

        # (25-32) Vector KNN (FLAT, HNSW FP32, SQ8, BQ+Rerank, 768d HNSW, Filtered HNSW, KNN LIMIT 0 0, VECTOR_RANGE)
        qv42 = saved_v128[42]
        qv777 = saved_v128[777]
        qv768 = saved_v768[17]

        assert_exact_parity("knn_flat_128d", ["FT.SEARCH", "p68_hash", "*=>[KNN 10 @v_flat $qv AS dist]", "PARAMS", "2", "qv", qv42, "RETURN", "2", "title", "price", "DIALECT", "2"], 0)
        assert_exact_parity("knn_flat_limit_slice", ["FT.SEARCH", "p68_hash", "*=>[KNN 15 @v_flat $qv]", "PARAMS", "2", "qv", qv777, "NOCONTENT", "LIMIT", "3", "7", "DIALECT", "2"], 1)
        assert_exact_parity("knn_flat_limit_0_0", ["FT.SEARCH", "p68_hash", "*=>[KNN 12 @v_flat $qv]", "PARAMS", "2", "qv", qv42, "LIMIT", "0", "0", "DIALECT", "2"], 2)
        assert_exact_parity("knn_hnsw_fp32_128d", ["FT.SEARCH", "p68_hash", "*=>[KNN 5 @v_hnsw $qv EF_RUNTIME 128]", "PARAMS", "2", "qv", qv42, "RETURN", "1", "sku", "DIALECT", "2"], 0, use_rc=True)
        assert_exact_parity("knn_filtered_hnsw_phase65", ["FT.SEARCH", "p68_hash", "(@cat:{electronics|books|garden})=>[KNN 8 @v_hnsw $qv EF_RUNTIME 128]", "PARAMS", "2", "qv", qv42, "RETURN", "2", "cat", "sku", "DIALECT", "2"], 1)
        assert_exact_parity("knn_hnsw_768d", ["FT.SEARCH", "p68_v768", "*=>[KNN 5 @emb $qv EF_RUNTIME 64]", "PARAMS", "2", "qv", qv768, "RETURN", "1", "cat", "DIALECT", "2"], 2)

        # SQ8 and BQ+Rerank HNSW KNN: verify exact #1 match (distance ~0 to self) & valid top-5 across shards
        for vfield, extra in (("v_sq8", "EF_RUNTIME 128"), ("v_bq", "EF_RUNTIME 128 OVERSAMPLE 8")):
            res_c = nodes_raw[0].execute_command(
                "FT.SEARCH", "p68_hash", f"*=>[KNN 5 @{vfield} $qv {extra}]",
                "PARAMS", "2", "qv", qv42, "NOCONTENT", "DIALECT", "2",
            )
            assert res_c[0] == 5 and res_c[1] == b"p68h:0042", f"Unexpected {vfield} KNN: {res_c}"

        # VECTOR_RANGE with $YIELD_DISTANCE_AS
        assert_exact_parity(
            "vector_range_yield",
            ["FT.SEARCH", "p68_hash", "@v_flat:[VECTOR_RANGE 0.05 $qv]=>{$YIELD_DISTANCE_AS: vdist}",
             "PARAMS", "2", "qv", qv42, "SORTBY", "sku", "ASC", "RETURN", "1", "sku", "DIALECT", "2"],
            0,
        )
        q_idx += 9

        # 4. Verify CLUSTER_MAX_SCATTER_LIMIT protection
        assert nodes[0].execute_command("FT.CONFIG", "SET", "CLUSTER_MAX_SCATTER_LIMIT", "50") == "OK"
        try:
            nodes[0].execute_command("FT.SEARCH", "p68_hash", "*", "LIMIT", "40", "20")
            raise AssertionError("Expected CLUSTER_MAX_SCATTER_LIMIT rejection")
        except ResponseError as e:
            assert "CLUSTER_MAX_SCATTER_LIMIT" in str(e)
        finally:
            nodes[0].execute_command("FT.CONFIG", "SET", "CLUSTER_MAX_SCATTER_LIMIT", "10000")

        # Cleanup Phase 68 indices
        for idx_name in ("p68_hash", "p68_json", "p68_v768"):
            assert nodes[0].execute_command("FT.DROPINDEX", idx_name) == "OK"

        print(
            f"  PASS: gemini-search Phase 68 golden parity ({q_idx} queries, 3,000 Hash+JSON + 128d/768d "
            f"FLAT/HNSW/SQ8/BQ/Filtered-HNSW/VECTOR_RANGE, 100% match vs standalone)"
        )
    finally:
        for nr in nodes_raw:
            nr.close()
        rc_raw.close()
        sa_raw.close()
        sa.close()
        sa_proc.terminate()
        try:
            sa_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            sa_proc.kill()


def main() -> int:
    bloom_so = sys.argv[1] if len(sys.argv) > 1 else "./build/redis_bloom.so"
    json_so = sys.argv[2] if len(sys.argv) > 2 else "./build/redis_json.so"
    search_so = sys.argv[3] if len(sys.argv) > 3 else "./build/redis_search.so"

    modules = [bloom_so, json_so]
    if os.path.exists(search_so):
        modules.append(search_so)

    for p in (bloom_so, json_so):
        if not os.path.exists(p):
            print(f"ERROR: module not found: {p}", file=sys.stderr)
            return 1

    ports = find_free_ports(3)
    workdir = tempfile.mkdtemp(prefix="gemini_cluster_test_")
    procs: list[subprocess.Popen] = []
    nodes: list[redis.Redis] = []
    rc = None
    try:
        print(f"Starting 3-node Redis 7 Cluster on ports {ports} (modules={len(modules)})...")
        for idx, p in enumerate(ports):
            procs.append(start_cluster_node(p, modules, os.path.join(workdir, f"node{idx}")))
        nodes = setup_3node_cluster(ports)
        rc = RedisCluster(host="127.0.0.1", port=ports[0], decode_responses=True)

        test_routing_and_keyspecs(rc, nodes)
        test_live_slot_migration(rc, nodes, ports)
        if os.path.exists(search_so):
            test_search_cluster_phase66(nodes, ports)
            test_search_cluster_phase67(rc, nodes, procs, ports, modules, workdir)
            test_search_cluster_phase68_golden_parity(rc, nodes, ports, modules, workdir)
        print("\nAll Redis 7 Cluster integration tests PASSED!")
        return 0
    finally:
        if rc:
            rc.close()
        for c in nodes:
            c.close()
        for proc in procs:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
