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

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

import redis
from redis.cluster import RedisCluster
from redis.exceptions import ClusterCrossSlotError, MovedError, ResponseError


def find_free_ports(count: int) -> list[int]:
    """Find `count` free ports whose cluster bus port (port + 10000) is also free."""
    ports: list[int] = []
    sockets: list[socket.socket] = []
    try:
        while len(ports) < count:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.bind(("127.0.0.1", 0))
            p = s.getsockname()[1]
            if p >= 50000 or p in ports:
                s.close()
                continue
            bus = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                bus.bind(("127.0.0.1", p + 10000))
            except OSError:
                bus.close()
                s.close()
                continue
            sockets.extend([s, bus])
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
        "--dbfilename", "",
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


def main() -> int:
    bloom_so = sys.argv[1] if len(sys.argv) > 1 else "./build/redis_bloom.so"
    json_so = sys.argv[2] if len(sys.argv) > 2 else "./build/redis_json.so"

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
        print(f"Starting 3-node Redis 7 Cluster on ports {ports}...")
        for idx, p in enumerate(ports):
            procs.append(start_cluster_node(p, [bloom_so, json_so], os.path.join(workdir, f"node{idx}")))
        nodes = setup_3node_cluster(ports)
        rc = RedisCluster(host="127.0.0.1", port=ports[0], decode_responses=True)

        test_routing_and_keyspecs(rc, nodes)
        test_live_slot_migration(rc, nodes, ports)
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
