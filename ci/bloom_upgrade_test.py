#!/usr/bin/env python3
"""Redis major-version upgrade tests with gemini-bloom on both sides."""

import os
import shutil
import subprocess
import sys
import tempfile
import time
import traceback

import redis

from bloom_persistence_test import (
    CORPUS,
    assert_snapshot,
    populate,
    snapshot,
    start_redis,
    stop_redis,
    wait_for_aof_rewrite,
    wait_for_bgsave,
)


UPGRADES = [("redis5", "redis6"), ("redis5", "redis7"), ("redis6", "redis7")]


def redis_version(binary):
    output = subprocess.check_output([binary, "--version"], text=True)
    for token in output.split():
        if token.startswith("v="):
            return token[2:]
    return output.strip()


def test_rdb_upgrade(module_path, source_binary, destination_binary, label):
    directory = tempfile.mkdtemp(prefix=f"gemini-bloom-upgrade-rdb-{label}-")
    proc = None
    try:
        proc, conn, _ = start_redis(module_path, directory, server_binary=source_binary)
        populate(conn)
        ttl_key = CORPUS[0]["key"]
        conn.pexpire(ttl_key, 600000)
        expected = snapshot(conn)
        wait_for_bgsave(conn)
        stop_redis(proc, conn)
        proc = None

        proc, conn, _ = start_redis(module_path, directory, server_binary=destination_binary)
        assert_snapshot(expected, conn, f"{label} RDB load")
        ttl = conn.pttl(ttl_key)
        if ttl <= 0 or ttl > 600000:
            raise AssertionError(f"{label}: RDB upgrade did not preserve TTL: {ttl}")

        post_items = [f"post-upgrade:{label}:{i}".encode() for i in range(300)]
        conn.execute_command("BF.MADD", ttl_key, *post_items)
        if conn.execute_command("BF.MEXISTS", ttl_key, *post_items) != [1] * len(post_items):
            raise AssertionError(f"{label}: writes after RDB upgrade have false negatives")
        expected_after_write = snapshot(conn)
        wait_for_bgsave(conn)
        stop_redis(proc, conn)
        proc = None

        proc, conn, _ = start_redis(module_path, directory, server_binary=destination_binary)
        assert_snapshot(expected_after_write, conn, f"{label} destination restart")
    finally:
        if proc is not None:
            stop_redis(proc, conn)
        shutil.rmtree(directory, ignore_errors=True)


def test_dump_restore_upgrade(module_path, source_binary, destination_binary, label):
    source_dir = tempfile.mkdtemp(prefix=f"gemini-bloom-upgrade-dump-src-{label}-")
    destination_dir = tempfile.mkdtemp(prefix=f"gemini-bloom-upgrade-dump-dst-{label}-")
    source_proc = destination_proc = None
    try:
        source_proc, source, _ = start_redis(module_path, source_dir, server_binary=source_binary)
        destination_proc, destination, _ = start_redis(
            module_path, destination_dir, server_binary=destination_binary
        )
        populate(source)
        ttl_key = CORPUS[0]["key"]
        source.pexpire(ttl_key, 600000)
        expected = snapshot(source)

        for spec in CORPUS:
            key = spec["key"]
            payload = source.dump(key)
            ttl = source.pttl(key)
            destination.restore(key, max(ttl, 0), payload)

        assert_snapshot(expected, destination, f"{label} DUMP/RESTORE")
        ttl = destination.pttl(ttl_key)
        if ttl <= 0 or ttl > 600000:
            raise AssertionError(f"{label}: DUMP/RESTORE did not preserve TTL: {ttl}")
    finally:
        if destination_proc is not None:
            stop_redis(destination_proc, destination)
        if source_proc is not None:
            stop_redis(source_proc, source)
        shutil.rmtree(source_dir, ignore_errors=True)
        shutil.rmtree(destination_dir, ignore_errors=True)


def test_fullsync_upgrade(module_path, source_binary, destination_binary, label):
    source_dir = tempfile.mkdtemp(prefix=f"gemini-bloom-upgrade-master-{label}-")
    destination_dir = tempfile.mkdtemp(prefix=f"gemini-bloom-upgrade-replica-{label}-")
    source_proc = destination_proc = None
    try:
        source_proc, source, _ = start_redis(
            module_path,
            source_dir,
            ["--repl-diskless-sync-delay", "0"],
            server_binary=source_binary,
        )
        populate(source)
        expected = snapshot(source)
        source_port = source.connection_pool.connection_kwargs["port"]

        destination_proc, destination, _ = start_redis(
            module_path,
            destination_dir,
            ["--repl-diskless-sync-delay", "0", "--replicaof", "127.0.0.1", str(source_port)],
            server_binary=destination_binary,
        )

        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            info = destination.info("replication")
            if info.get("master_link_status") == "up" and destination.exists(CORPUS[0]["key"]):
                break
            time.sleep(0.1)
        else:
            raise RuntimeError(f"{label}: destination did not complete fullsync")

        assert_snapshot(expected, destination, f"{label} fullsync")
        incremental = [f"upgrade-repl:{label}:{i}".encode() for i in range(100)]
        source.execute_command("BF.MADD", CORPUS[0]["key"], *incremental)
        if source.execute_command("WAIT", 1, 10000) != 1:
            raise RuntimeError(f"{label}: destination did not acknowledge incremental replication")
        if destination.execute_command("BF.MEXISTS", CORPUS[0]["key"], *incremental) != [1] * len(incremental):
            raise AssertionError(f"{label}: incremental replication has false negatives")
    finally:
        if destination_proc is not None:
            stop_redis(destination_proc, destination)
        if source_proc is not None:
            stop_redis(source_proc, source)
        shutil.rmtree(source_dir, ignore_errors=True)
        shutil.rmtree(destination_dir, ignore_errors=True)


def test_aof_upgrade(module_path, source_binary, destination_binary, label, preamble):
    directory = tempfile.mkdtemp(prefix=f"gemini-bloom-upgrade-aof-{label}-{preamble}-")
    proc = None
    args = [
        "--appendonly", "yes",
        "--appendfilename", "appendonly.aof",
        "--aof-use-rdb-preamble", preamble,
    ]
    try:
        proc, conn, _ = start_redis(
            module_path, directory, args, server_binary=source_binary
        )
        populate(conn)
        expected = snapshot(conn)
        wait_for_aof_rewrite(conn)
        stop_redis(proc, conn)
        proc = None

        proc, conn, _ = start_redis(
            module_path, directory, args, server_binary=destination_binary
        )
        assert_snapshot(expected, conn, f"{label} AOF preamble={preamble}")
    finally:
        if proc is not None:
            stop_redis(proc, conn)
        shutil.rmtree(directory, ignore_errors=True)


def main():
    if len(sys.argv) != 5:
        print(
            f"Usage: {sys.argv[0]} <gemini_bloom.so> <redis5-server> <redis6-server> <redis7-server>",
            file=sys.stderr,
        )
        return 2

    module_path = os.path.abspath(sys.argv[1])
    binaries = {
        "redis5": os.path.abspath(sys.argv[2]),
        "redis6": os.path.abspath(sys.argv[3]),
        "redis7": os.path.abspath(sys.argv[4]),
    }
    for name, path in [("module", module_path), *binaries.items()]:
        if not os.path.isfile(path):
            print(f"Missing {name}: {path}", file=sys.stderr)
            return 2

    print("Redis binaries:")
    for name, binary in binaries.items():
        print(f"  {name}: {redis_version(binary)} ({binary})")
    print("Scope: GeminiBloom -> GeminiBloom only; RedisBloom replacement is not tested or supported here.")

    cases = []
    for source_name, destination_name in UPGRADES:
        label = f"{source_name}->{destination_name}"
        source_binary = binaries[source_name]
        destination_binary = binaries[destination_name]
        cases.extend([
            (f"{label} RDB file + post-upgrade write", lambda s=source_binary, d=destination_binary, l=label:
             test_rdb_upgrade(module_path, s, d, l)),
            (f"{label} DUMP/RESTORE", lambda s=source_binary, d=destination_binary, l=label:
             test_dump_restore_upgrade(module_path, s, d, l)),
            (f"{label} fullsync + command stream", lambda s=source_binary, d=destination_binary, l=label:
             test_fullsync_upgrade(module_path, s, d, l)),
            (f"{label} AOF RDB preamble", lambda s=source_binary, d=destination_binary, l=label:
             test_aof_upgrade(module_path, s, d, l, "yes")),
            (f"{label} AOF command rewrite", lambda s=source_binary, d=destination_binary, l=label:
             test_aof_upgrade(module_path, s, d, l, "no")),
        ])

    failures = []
    for name, case in cases:
        print(f"\n--- {name} ---")
        try:
            case()
            print(f"PASS: {name}")
        except Exception as exc:
            failures.append((name, str(exc)))
            print(f"FAIL: {name}: {exc}", file=sys.stderr)
            traceback.print_exc()

    print(f"\nUpgrade results: {len(cases) - len(failures)} passed, {len(failures)} failed")
    if failures:
        print("Failures:")
        for name, error in failures:
            print(f"  {name}: {error}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
