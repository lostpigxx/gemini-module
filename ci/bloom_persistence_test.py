#!/usr/bin/env python3
"""Single-Redis-version persistence tests for gemini-bloom.

This suite deliberately uses gemini-bloom on both sides. RedisBloom to
gemini-bloom replacement is not an upgrade path covered by this test.
"""

import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import traceback

import redis


CORPUS = [
    {"key": b"persist:scaling", "reserve": [0.01, 50, "EXPANSION", 2], "count": 240},
    {"key": b"persist:fixed", "reserve": [0.001, 300, "NONSCALING"], "count": 250},
    {"key": b"persist:auto", "reserve": None, "count": 120},
    {"key": b"persist:empty", "reserve": [0.01, 100], "count": 0},
    {"key": b"persist:exp1", "reserve": [0.01, 30, "EXPANSION", 1], "count": 180},
    {"key": b"persist:exp4", "reserve": [0.0001, 80, "EXPANSION", 4], "count": 320},
    {
        "key": b"persist:binary",
        "reserve": [0.01, 100, "EXPANSION", 2],
        "items": [b"", b"nul\x00item", b"line\r\nbreak", bytes(range(256))],
    },
]

LOGICAL_INFO_FIELDS = (
    b"Capacity",
    b"Number of filters",
    b"Number of items inserted",
    b"Expansion rate",
)


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def start_redis(
    module_path,
    directory,
    extra_args=None,
    server_binary="redis-server",
    process_env=None,
    command_prefix=None,
):
    port = find_free_port()
    logfile = os.path.join(directory, f"redis-{port}.log")
    log_fh = open(logfile, "w")
    command = [
        server_binary,
        "--port", str(port),
        "--bind", "127.0.0.1",
        "--daemonize", "no",
        "--loglevel", "warning",
        "--save", "",
        "--dir", directory,
        "--dbfilename", "dump.rdb",
        "--loadmodule", os.path.abspath(module_path),
    ]
    if extra_args:
        command.extend(extra_args)
    if command_prefix:
        command = list(command_prefix) + command
    proc = subprocess.Popen(
        command,
        stdout=log_fh,
        stderr=subprocess.STDOUT,
        env=process_env,
    )
    log_fh.close()

    for _ in range(600):
        time.sleep(0.05)
        if proc.poll() is not None:
            with open(logfile) as log:
                raise RuntimeError(f"redis-server exited early:\n{log.read()}")
        try:
            conn = redis.Redis(host="127.0.0.1", port=port, decode_responses=False, protocol=2)
            conn.ping()
            return proc, conn, logfile
        except (redis.ConnectionError, OSError):
            pass

    proc.kill()
    proc.wait()
    raise RuntimeError(f"redis-server did not start; see {logfile}")


def stop_redis(proc, conn=None):
    if proc.poll() is not None:
        return
    if conn is not None:
        try:
            conn.execute_command("SHUTDOWN", "NOSAVE")
        except (redis.ConnectionError, redis.ResponseError):
            pass
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def wait_for_bgsave(conn, timeout=30):
    conn.execute_command("BGSAVE")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        info = conn.info("persistence")
        if not info.get("rdb_bgsave_in_progress", 0) and info.get("rdb_changes_since_last_save") == 0:
            if info.get("rdb_last_bgsave_status") != "ok":
                raise RuntimeError(f"BGSAVE failed: {info.get('rdb_last_bgsave_status')!r}")
            return
        time.sleep(0.05)
    raise RuntimeError(f"BGSAVE did not complete within {timeout}s")


def wait_for_aof_rewrite(conn, timeout=30):
    conn.execute_command("BGREWRITEAOF")
    deadline = time.monotonic() + timeout
    saw_active = False
    while time.monotonic() < deadline:
        info = conn.info("persistence")
        active = bool(info.get("aof_rewrite_in_progress", 0))
        saw_active = saw_active or active
        if saw_active and not active:
            if info.get("aof_last_bgrewrite_status") != "ok":
                raise RuntimeError(f"BGREWRITEAOF failed: {info.get('aof_last_bgrewrite_status')!r}")
            return
        time.sleep(0.05)
    raise RuntimeError(f"BGREWRITEAOF did not complete within {timeout}s")


def corpus_items(spec):
    if "items" in spec:
        return spec["items"]
    return [spec["key"] + b":item:" + str(i).encode() for i in range(spec["count"])]


def populate(conn):
    for spec in CORPUS:
        key = spec["key"]
        reserve = spec["reserve"]
        if reserve:
            conn.execute_command("BF.RESERVE", key, *reserve)
        items = corpus_items(spec)
        if not items:
            continue
        if reserve is None:
            for item in items:
                conn.execute_command("BF.ADD", key, item)
        else:
            for offset in range(0, len(items), 100):
                conn.execute_command("BF.MADD", key, *items[offset:offset + 100])


def info_dict(raw):
    if isinstance(raw, dict):
        return raw
    return {raw[i]: raw[i + 1] for i in range(0, len(raw), 2)}


def snapshot(conn):
    result = {}
    for spec in CORPUS:
        key = spec["key"]
        items = corpus_items(spec)
        probes = [key + b":probe:" + str(i).encode() for i in range(128)]
        pipe = conn.pipeline(transaction=False)
        for item in items + probes:
            pipe.execute_command("BF.EXISTS", key, item)
        memberships = pipe.execute()
        result[key] = {
            "card": conn.execute_command("BF.CARD", key),
            "info": info_dict(conn.execute_command("BF.INFO", key)),
            "memberships": memberships,
        }
    return result


def assert_snapshot(expected, conn, label):
    actual = snapshot(conn)
    for key, expected_state in expected.items():
        actual_state = actual.get(key)
        if actual_state is None:
            raise AssertionError(f"{label}: missing key {key!r}")

        expected_info = expected_state["info"]
        actual_info = actual_state["info"]
        expected_logical = {field: expected_info[field] for field in LOGICAL_INFO_FIELDS}
        actual_logical = {field: actual_info[field] for field in LOGICAL_INFO_FIELDS}
        if (
            expected_state["card"] != actual_state["card"]
            or expected_state["memberships"] != actual_state["memberships"]
            or expected_logical != actual_logical
        ):
            raise AssertionError(
                f"{label}: logical snapshot mismatch for {key!r}\n"
                f"expected={expected_state!r}\nactual={actual_state!r}"
            )

        actual_size = actual_info.get(b"Size")
        if not isinstance(actual_size, int) or actual_size <= 0:
            raise AssertionError(f"{label}: invalid BF.INFO Size for {key!r}: {actual_size!r}")
        expected_size = expected_info.get(b"Size")
        if expected_size != actual_size:
            print(
                f"NOTE: {label}: BF.INFO Size for {key!r} changed "
                f"from {expected_size} to {actual_size}; Size is runtime allocation, "
                "not persisted logical state"
            )


def with_tempdir(test_fn):
    directory = tempfile.mkdtemp(prefix="gemini-bloom-persist-")
    try:
        test_fn(directory)
    finally:
        shutil.rmtree(directory, ignore_errors=True)


def test_rdb_restart(module_path):
    def run(directory):
        proc, conn, _ = start_redis(module_path, directory)
        try:
            populate(conn)
            expected = snapshot(conn)
            wait_for_bgsave(conn)
        finally:
            stop_redis(proc, conn)

        proc, conn, _ = start_redis(module_path, directory)
        try:
            assert_snapshot(expected, conn, "RDB restart")
        finally:
            stop_redis(proc, conn)

    with_tempdir(run)


def test_dump_restore(module_path):
    src_dir = tempfile.mkdtemp(prefix="gemini-bloom-dump-src-")
    dst_dir = tempfile.mkdtemp(prefix="gemini-bloom-dump-dst-")
    src_proc = dst_proc = None
    try:
        src_proc, src, _ = start_redis(module_path, src_dir)
        dst_proc, dst, _ = start_redis(module_path, dst_dir)
        populate(src)
        src.pexpire(CORPUS[0]["key"], 120000)
        expected = snapshot(src)

        for spec in CORPUS:
            key = spec["key"]
            payload = src.dump(key)
            ttl = src.pttl(key)
            dst.restore(key, max(ttl, 0), payload)

        assert_snapshot(expected, dst, "DUMP/RESTORE")
        ttl = dst.pttl(CORPUS[0]["key"])
        if ttl <= 0 or ttl > 120000:
            raise AssertionError(f"DUMP/RESTORE did not preserve TTL: {ttl}")
    finally:
        if src_proc:
            stop_redis(src_proc, src)
        if dst_proc:
            stop_redis(dst_proc, dst)
        shutil.rmtree(src_dir, ignore_errors=True)
        shutil.rmtree(dst_dir, ignore_errors=True)


def test_migrate(module_path):
    src_dir = tempfile.mkdtemp(prefix="gemini-bloom-migrate-src-")
    dst_dir = tempfile.mkdtemp(prefix="gemini-bloom-migrate-dst-")
    src_proc = dst_proc = None
    try:
        src_proc, src, _ = start_redis(module_path, src_dir)
        dst_proc, dst, _ = start_redis(module_path, dst_dir)
        populate(src)
        src.pexpire(CORPUS[0]["key"], 120000)
        expected = snapshot(src)
        keys = [spec["key"] for spec in CORPUS]
        result = src.execute_command(
            "MIGRATE", "127.0.0.1", dst.connection_pool.connection_kwargs["port"],
            "", 0, 30000, "KEYS", *keys,
        )
        if result != b"OK":
            raise AssertionError(f"MIGRATE returned {result!r}")
        assert_snapshot(expected, dst, "MIGRATE")
        if any(src.exists(key) for key in keys):
            raise AssertionError("MIGRATE left source keys behind")
        ttl = dst.pttl(CORPUS[0]["key"])
        if ttl <= 0 or ttl > 120000:
            raise AssertionError(f"MIGRATE did not preserve TTL: {ttl}")
    finally:
        if src_proc:
            stop_redis(src_proc, src)
        if dst_proc:
            stop_redis(dst_proc, dst)
        shutil.rmtree(src_dir, ignore_errors=True)
        shutil.rmtree(dst_dir, ignore_errors=True)


def test_fullsync(module_path):
    master_dir = tempfile.mkdtemp(prefix="gemini-bloom-master-")
    replica_dir = tempfile.mkdtemp(prefix="gemini-bloom-replica-")
    master_proc = replica_proc = None
    try:
        master_proc, master, _ = start_redis(module_path, master_dir, ["--repl-diskless-sync-delay", "0"])
        populate(master)
        expected = snapshot(master)
        master_port = master.connection_pool.connection_kwargs["port"]
        replica_proc, replica, _ = start_redis(
            module_path,
            replica_dir,
            ["--repl-diskless-sync-delay", "0", "--replicaof", "127.0.0.1", str(master_port)],
        )

        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            try:
                if replica.info("replication").get("master_link_status") == "up" and replica.exists(CORPUS[0]["key"]):
                    break
            except redis.ResponseError:
                pass
            time.sleep(0.1)
        else:
            raise RuntimeError("replica did not finish full synchronization")

        assert_snapshot(expected, replica, "fullsync")
        key = CORPUS[0]["key"]
        new_items = [b"post-fullsync:" + str(i).encode() for i in range(100)]
        master.execute_command("BF.MADD", key, *new_items)
        if master.execute_command("WAIT", 1, 10000) != 1:
            raise RuntimeError("replica did not acknowledge incremental writes")
        if replica.execute_command("BF.MEXISTS", key, *new_items) != [1] * len(new_items):
            raise AssertionError("incremental writes missing after fullsync")
    finally:
        if replica_proc:
            stop_redis(replica_proc, replica)
        if master_proc:
            stop_redis(master_proc, master)
        shutil.rmtree(master_dir, ignore_errors=True)
        shutil.rmtree(replica_dir, ignore_errors=True)


def test_aof_roundtrip(module_path, preamble):
    def run(directory):
        args = [
            "--appendonly", "yes",
            "--appendfilename", "appendonly.aof",
            "--aof-use-rdb-preamble", preamble,
        ]
        proc, conn, _ = start_redis(module_path, directory, args)
        try:
            populate(conn)
            expected = snapshot(conn)
            wait_for_aof_rewrite(conn)
        finally:
            stop_redis(proc, conn)

        proc, conn, _ = start_redis(module_path, directory, args)
        try:
            assert_snapshot(expected, conn, f"AOF preamble={preamble}")
        finally:
            stop_redis(proc, conn)

    with_tempdir(run)


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <gemini_bloom.so>", file=sys.stderr)
        return 2
    module_path = os.path.abspath(sys.argv[1])
    if not os.path.isfile(module_path):
        print(f"Module not found: {module_path}", file=sys.stderr)
        return 2

    tests = [
        ("RDB restart", lambda: test_rdb_restart(module_path)),
        ("DUMP/RESTORE with TTL", lambda: test_dump_restore(module_path)),
        ("MIGRATE with TTL", lambda: test_migrate(module_path)),
        ("fullsync plus incremental replication", lambda: test_fullsync(module_path)),
        ("AOF RDB preamble", lambda: test_aof_roundtrip(module_path, "yes")),
        ("AOF command rewrite", lambda: test_aof_roundtrip(module_path, "no")),
    ]

    failures = []
    for name, test_fn in tests:
        print(f"\n--- {name} ---")
        try:
            test_fn()
            print(f"PASS: {name}")
        except Exception as exc:
            failures.append((name, str(exc)))
            print(f"FAIL: {name}: {exc}", file=sys.stderr)
            traceback.print_exc()

    print(f"\nPersistence results: {len(tests) - len(failures)} passed, {len(failures)} failed")
    if failures:
        return 1
    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda _signum, _frame: sys.exit(1))
    sys.exit(main())
