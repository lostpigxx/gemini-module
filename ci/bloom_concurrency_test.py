#!/usr/bin/env python3
"""Concurrent-client and allocation-churn checks for gemini-bloom."""

import concurrent.futures
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

import redis


WORKERS = 8
ITEMS_PER_WORKER = 1000


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def start_redis(module_path, directory):
    port = find_free_port()
    logfile = os.path.join(directory, "redis.log")
    log_fh = open(logfile, "w")
    proc = subprocess.Popen(
        [
            "redis-server", "--port", str(port), "--bind", "127.0.0.1",
            "--daemonize", "no", "--loglevel", "warning", "--save", "",
            "--dir", directory, "--loadmodule", os.path.abspath(module_path),
        ],
        stdout=log_fh,
        stderr=subprocess.STDOUT,
    )
    log_fh.close()
    for _ in range(200):
        time.sleep(0.05)
        if proc.poll() is not None:
            with open(logfile) as log:
                raise RuntimeError(f"redis-server exited early:\n{log.read()}")
        try:
            conn = redis.Redis(host="127.0.0.1", port=port, decode_responses=False, protocol=2)
            conn.ping()
            return proc, conn, port
        except redis.ConnectionError:
            pass
    proc.kill()
    proc.wait()
    raise RuntimeError("redis-server did not start")


def stop_redis(proc, conn):
    try:
        conn.execute_command("SHUTDOWN", "NOSAVE")
    except (redis.ConnectionError, redis.ResponseError):
        pass
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def worker_add(port, worker_id):
    conn = redis.Redis(host="127.0.0.1", port=port, decode_responses=False, protocol=2)
    inserted = 0
    items = [f"worker:{worker_id}:item:{i}".encode() for i in range(ITEMS_PER_WORKER)]
    for offset in range(0, len(items), 50):
        replies = conn.execute_command("BF.MADD", b"concurrent:shared", *items[offset:offset + 50])
        inserted += sum(reply == 1 for reply in replies)
    return inserted, items


def worker_duplicate(port, items):
    conn = redis.Redis(host="127.0.0.1", port=port, decode_responses=False, protocol=2)
    unexpected_new = 0
    for offset in range(0, len(items), 50):
        replies = conn.execute_command("BF.MADD", b"concurrent:shared", *items[offset:offset + 50])
        unexpected_new += sum(reply == 1 for reply in replies)
    return unexpected_new


def assert_all_present(conn, key, items):
    for offset in range(0, len(items), 500):
        chunk = items[offset:offset + 500]
        replies = conn.execute_command("BF.MEXISTS", key, *chunk)
        if replies != [1] * len(chunk):
            missing = [chunk[i] for i, reply in enumerate(replies) if reply != 1]
            raise AssertionError(f"false negatives for {missing[:5]!r}")


def test_shared_key_concurrency(conn, port):
    conn.execute_command("BF.RESERVE", b"concurrent:shared", 1e-9, 10000, "NONSCALING")
    with concurrent.futures.ThreadPoolExecutor(max_workers=WORKERS) as executor:
        results = list(executor.map(lambda worker: worker_add(port, worker), range(WORKERS)))

    inserted = sum(result[0] for result in results)
    items = [item for result in results for item in result[1]]
    assert_all_present(conn, b"concurrent:shared", items)
    card = conn.execute_command("BF.CARD", b"concurrent:shared")
    if card != inserted:
        raise AssertionError(f"CARD={card}, successful insert replies={inserted}")

    duplicate_set = items[:500]
    with concurrent.futures.ThreadPoolExecutor(max_workers=WORKERS) as executor:
        duplicate_results = list(executor.map(lambda _worker: worker_duplicate(port, duplicate_set), range(WORKERS)))
    if sum(duplicate_results) != 0:
        raise AssertionError(f"duplicate concurrent inserts reported {sum(duplicate_results)} new items")
    if conn.execute_command("BF.CARD", b"concurrent:shared") != card:
        raise AssertionError("CARD changed after duplicate-only concurrent writes")


def memory_after_cleanup(conn):
    try:
        conn.execute_command("MEMORY", "PURGE")
    except redis.ResponseError:
        pass
    time.sleep(0.05)
    return int(conn.info("memory").get("used_memory", 0))


def churn_once(conn, cycle):
    keys = []
    for index in range(8):
        key = f"churn:{cycle}:{index}".encode()
        keys.append(key)
        conn.execute_command("BF.RESERVE", key, 0.001, 1000, "EXPANSION", 2)
        items = [key + b":" + str(item).encode() for item in range(1200)]
        for offset in range(0, len(items), 100):
            conn.execute_command("BF.MADD", key, *items[offset:offset + 100])
    conn.delete(*keys)


def test_allocation_churn(conn):
    for cycle in range(3):
        churn_once(conn, cycle)
    baseline = memory_after_cleanup(conn)

    for cycle in range(3, 23):
        churn_once(conn, cycle)
    final = memory_after_cleanup(conn)

    allowance = max(16 * 1024 * 1024, baseline * 2)
    growth = final - baseline
    print(f"Memory after warm-up={baseline}, after churn={final}, growth={growth}, allowance={allowance}")
    if growth > allowance:
        raise AssertionError(f"used_memory grew by {growth} bytes after all churn keys were deleted")


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <gemini_bloom.so>", file=sys.stderr)
        return 2
    module_path = sys.argv[1]
    directory = tempfile.mkdtemp(prefix="gemini-bloom-concurrency-")
    proc = None
    try:
        proc, conn, port = start_redis(module_path, directory)
        print("--- shared-key concurrent clients ---")
        test_shared_key_concurrency(conn, port)
        print("PASS: shared-key concurrent clients")
        conn.delete(b"concurrent:shared")

        print("\n--- allocation churn ---")
        test_allocation_churn(conn)
        print("PASS: allocation churn")
        print("\nConcurrency results: 2 passed, 0 failed")
        return 0
    finally:
        if proc is not None:
            stop_redis(proc, conn)
        shutil.rmtree(directory, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
