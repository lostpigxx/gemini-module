#!/usr/bin/env python3
"""Redis Cluster routing and slot-migration checks for GeminiBloom."""

import binascii
import os
import shutil
import subprocess
import sys
import tempfile
import time

import redis
from redis.exceptions import ConnectionError, ResponseError


HOST = "127.0.0.1"
PORTS = (17000, 17001, 17002)


def slot_for(key):
    start = key.find(b"{")
    if start >= 0:
        end = key.find(b"}", start + 1)
        if end > start + 1:
            key = key[start + 1:end]
    return binascii.crc_hqx(key, 0) % 16384


def wait_ready(port, proc, logfile):
    conn = redis.Redis(host=HOST, port=port, decode_responses=False, protocol=2)
    for _ in range(400):
        if proc.poll() is not None:
            raise RuntimeError(f"redis:{port} exited early:\n{open(logfile).read()}")
        try:
            conn.ping()
            return conn
        except (ConnectionError, OSError):
            time.sleep(0.05)
    raise RuntimeError(f"redis:{port} did not become ready")


def start_node(module, root, port):
    directory = os.path.join(root, str(port))
    os.makedirs(directory)
    logfile = os.path.join(directory, "redis.log")
    log = open(logfile, "w")
    proc = subprocess.Popen(
        [
            "redis-server",
            "--port", str(port),
            "--bind", HOST,
            "--protected-mode", "no",
            "--daemonize", "no",
            "--save", "",
            "--appendonly", "no",
            "--dir", directory,
            "--cluster-enabled", "yes",
            "--cluster-config-file", f"nodes-{port}.conf",
            "--cluster-node-timeout", "2000",
            "--loadmodule", os.path.abspath(module),
        ],
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    return proc, wait_ready(port, proc, logfile), log


def wait_cluster_ok(conns):
    for _ in range(600):
        states = []
        for conn in conns:
            try:
                raw = conn.execute_command("CLUSTER", "INFO")
                fields = dict(line.split(b":", 1) for line in raw.splitlines() if b":" in line)
                states.append(fields.get(b"cluster_state") == b"ok")
            except (ConnectionError, ResponseError):
                states.append(False)
        if all(states):
            return
        time.sleep(0.05)
    raise RuntimeError("cluster did not reach cluster_state:ok")


def configure_cluster(conns):
    for conn in conns[1:]:
        conn.execute_command("CLUSTER", "MEET", HOST, PORTS[0])

    for _ in range(200):
        if all(len(conn.execute_command("CLUSTER", "NODES").splitlines()) == 3 for conn in conns):
            break
        time.sleep(0.05)
    else:
        raise RuntimeError("cluster nodes did not discover one another")

    ranges = ((0, 5460), (5461, 10922), (10923, 16383))
    for conn, (start, end) in zip(conns, ranges):
        for offset in range(start, end + 1, 1000):
            conn.execute_command("CLUSTER", "ADDSLOTS", *range(offset, min(offset + 1000, end + 1)))
    wait_cluster_ok(conns)


def response_kind(exc):
    return f"{type(exc).__name__} {exc}".upper()


def expect_redirect(conn, expected, *command):
    try:
        conn.execute_command(*command)
    except ResponseError as exc:
        if expected not in response_kind(exc):
            raise AssertionError(f"expected {expected}, got {type(exc).__name__}: {exc}")
        return exc
    raise AssertionError(f"expected {expected} redirect, command succeeded")


def optional_card(conn, key):
    try:
        return conn.execute_command("BF.CARD", key)
    except ResponseError as exc:
        if "unknown command" in str(exc).lower():
            return None
        raise


def find_key_for_range(start, end):
    for index in range(100000):
        key = f"cluster:bloom:{index}".encode()
        if start <= slot_for(key) <= end:
            return key
    raise RuntimeError("failed to find key for slot range")


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <gemini_bloom.so>", file=sys.stderr)
        return 2

    root = tempfile.mkdtemp(prefix="bloom-cluster-")
    processes = []
    logs = []
    try:
        started = [start_node(sys.argv[1], root, port) for port in PORTS]
        processes = [item[0] for item in started]
        conns = [item[1] for item in started]
        logs = [item[2] for item in started]
        configure_cluster(conns)

        node_ids = [conn.execute_command("CLUSTER", "MYID").decode() for conn in conns]
        key = find_key_for_range(0, 5460)
        slot = slot_for(key)
        source, target, wrong = conns[0], conns[1], conns[2]

        expect_redirect(wrong, "MOVED", "BF.ADD", key, b"alpha")

        items = [f"cluster-item:{index}".encode() for index in range(1000)]
        source.execute_command("BF.RESERVE", key, 0.001, 2000)
        source.execute_command("BF.MADD", key, *items)
        before_card = optional_card(source, key)
        if before_card is not None and before_card != 1000:
            raise AssertionError("unexpected CARD before slot migration")

        target.execute_command("CLUSTER", "SETSLOT", slot, "IMPORTING", node_ids[0])
        source.execute_command("CLUSTER", "SETSLOT", slot, "MIGRATING", node_ids[1])
        source.execute_command("MIGRATE", HOST, PORTS[1], key, 0, 10000)

        expect_redirect(source, "ASK", "BF.EXISTS", key, items[0])
        target.execute_command("ASKING")
        asking_card = optional_card(target, key)
        if asking_card is not None and asking_card != 1000:
            raise AssertionError("CARD mismatch through ASKING route")
        target.execute_command("ASKING")
        if target.execute_command("BF.MEXISTS", key, *items) != [1] * len(items):
            raise AssertionError("membership mismatch through ASKING route")

        for conn in conns:
            conn.execute_command("CLUSTER", "SETSLOT", slot, "NODE", node_ids[1])
        wait_cluster_ok(conns)

        expect_redirect(source, "MOVED", "BF.EXISTS", key, items[0])
        if target.execute_command("BF.MEXISTS", key, *items) != [1] * len(items):
            raise AssertionError("membership mismatch after slot ownership transfer")
        if target.execute_command("BF.ADD", key, b"post-migration") != 1:
            raise AssertionError("write failed after slot ownership transfer")
        final_card = optional_card(target, key)
        if final_card is not None and final_card != 1001:
            raise AssertionError("CARD mismatch after post-migration write")

        print(f"PASS: MOVED, ASK, and slot migration (slot={slot})")
        return 0
    finally:
        for conn in locals().get("conns", []):
            try:
                conn.execute_command("SHUTDOWN", "NOSAVE")
            except Exception:
                pass
        for proc in processes:
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        for log in logs:
            log.close()
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
