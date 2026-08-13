#!/usr/bin/env python3
"""Disk-full and corrupted RDB/AOF checks for GeminiBloom."""

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

import redis
from redis.exceptions import ConnectionError, ResponseError


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_ready(port, proc, logfile):
    conn = redis.Redis(host="127.0.0.1", port=port, decode_responses=False, protocol=2)
    for _ in range(400):
        if proc.poll() is not None:
            raise RuntimeError(f"redis exited early:\n{open(logfile).read()}")
        try:
            conn.ping()
            return conn
        except (ConnectionError, OSError):
            time.sleep(0.05)
    raise RuntimeError("redis did not become ready")


def start_redis(module, directory, appendonly=False):
    port = free_port()
    logfile = os.path.join(directory, f"redis-{port}.log")
    log = open(logfile, "a")
    args = [
        "redis-server",
        "--port", str(port),
        "--bind", "127.0.0.1",
        "--daemonize", "no",
        "--save", "",
        "--dir", directory,
        "--dbfilename", "dump.rdb",
        "--appendonly", "yes" if appendonly else "no",
        "--appendfsync", "always",
        "--aof-use-rdb-preamble", "no",
        "--aof-load-truncated", "no",
        "--loadmodule", os.path.abspath(module),
    ]
    proc = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
    return proc, wait_ready(port, proc, logfile), log, logfile


def stop_redis(proc, conn):
    if proc.poll() is None:
        try:
            conn.execute_command("SHUTDOWN", "NOSAVE")
        except (ConnectionError, ResponseError):
            pass
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.terminate()
        proc.wait(timeout=5)


def populate(conn, key=b"storage:bloom"):
    items = [f"storage-item:{index}".encode() for index in range(5000)]
    conn.execute_command("BF.RESERVE", key, 0.001, 10000)
    conn.execute_command("BF.MADD", key, *items)
    return items


def expect_startup_rejection(module, directory, appendonly, label):
    port = free_port()
    logfile = os.path.join(directory, f"rejected-{label}.log")
    with open(logfile, "w") as log:
        proc = subprocess.Popen(
            [
                "redis-server",
                "--port", str(port),
                "--bind", "127.0.0.1",
                "--daemonize", "no",
                "--save", "",
                "--dir", directory,
                "--dbfilename", "dump.rdb",
                "--appendonly", "yes" if appendonly else "no",
                "--appendfsync", "always",
                "--aof-use-rdb-preamble", "no",
                "--aof-load-truncated", "no",
                "--loadmodule", os.path.abspath(module),
            ],
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        try:
            exit_code = proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(timeout=5)
            raise AssertionError(f"{label}: Redis accepted a corrupted persistence file")
    if exit_code == 0:
        raise AssertionError(f"{label}: Redis exited successfully despite corrupted input")


def corrupt_rdb(module):
    directory = tempfile.mkdtemp(prefix="bloom-corrupt-rdb-")
    proc = conn = log = None
    try:
        proc, conn, log, _ = start_redis(module, directory)
        populate(conn)
        conn.execute_command("SAVE")
        stop_redis(proc, conn)
        proc = conn = None
        log.close()
        log = None

        path = os.path.join(directory, "dump.rdb")
        data = bytearray(open(path, "rb").read())
        if len(data) < 128:
            raise AssertionError("RDB fixture unexpectedly small")
        data[len(data) // 2] ^= 0x5A
        with open(path, "wb") as output:
            output.write(data)
        expect_startup_rejection(module, directory, False, "corrupt-rdb")
        print("PASS: corrupted RDB rejected at startup")
    finally:
        if proc is not None:
            stop_redis(proc, conn)
        if log is not None and not log.closed:
            log.close()
        shutil.rmtree(directory, ignore_errors=True)


def aof_data_files(directory):
    result = []
    for root, _dirs, files in os.walk(directory):
        for name in files:
            if ".aof" in name and not name.endswith(".manifest"):
                result.append(os.path.join(root, name))
    return result


def wait_rewrite(conn, timeout=30):
    conn.execute_command("BGREWRITEAOF")
    deadline = time.monotonic() + timeout
    started = False
    while time.monotonic() < deadline:
        state = conn.info("persistence")
        active = bool(state.get("aof_rewrite_in_progress", 0))
        started = started or active
        if started and not active:
            return state.get("aof_last_bgrewrite_status")
        time.sleep(0.05)
    raise RuntimeError("BGREWRITEAOF did not finish")


def corrupt_aof(module):
    directory = tempfile.mkdtemp(prefix="bloom-corrupt-aof-")
    proc = conn = log = None
    try:
        proc, conn, log, _ = start_redis(module, directory, appendonly=True)
        populate(conn)
        if wait_rewrite(conn) != "ok":
            raise AssertionError("failed to create AOF fixture")
        stop_redis(proc, conn)
        proc = conn = None
        log.close()
        log = None

        files = aof_data_files(directory)
        if not files:
            raise AssertionError("no AOF data file found")
        path = max(files, key=os.path.getsize)
        size = os.path.getsize(path)
        if size < 128:
            raise AssertionError("AOF fixture unexpectedly small")
        os.truncate(path, size - min(97, size // 4))
        expect_startup_rejection(module, directory, True, "corrupt-aof")
        print("PASS: truncated AOF rejected at startup")
    finally:
        if proc is not None:
            stop_redis(proc, conn)
        if log is not None and not log.closed:
            log.close()
        shutil.rmtree(directory, ignore_errors=True)


def wait_bgsave_failure(conn, timeout=30):
    before = conn.info("persistence").get("rdb_last_save_time")
    try:
        conn.execute_command("BGSAVE")
    except ResponseError:
        pass
    deadline = time.monotonic() + timeout
    started = False
    while time.monotonic() < deadline:
        state = conn.info("persistence")
        active = bool(state.get("rdb_bgsave_in_progress", 0))
        started = started or active
        completed = state.get("rdb_last_save_time") != before
        if not active and (started or completed or state.get("rdb_last_bgsave_status") == "err"):
            return state.get("rdb_last_bgsave_status")
        time.sleep(0.05)
    raise RuntimeError("BGSAVE did not finish")


def disk_full(module, root):
    directory = os.path.join(root, "rdb")
    os.makedirs(directory, exist_ok=True)
    proc = conn = log = None
    try:
        proc, conn, log, _ = start_redis(module, directory)
        items = populate(conn)
        conn.set(b"incompressible-padding", os.urandom(20 * 1024 * 1024))
        status = wait_bgsave_failure(conn)
        if status != "err":
            raise AssertionError(f"disk-full BGSAVE unexpectedly returned {status!r}")
        conn.ping()
        if conn.execute_command("BF.MEXISTS", b"storage:bloom", *items) != [1] * len(items):
            raise AssertionError("live Bloom data changed after disk-full BGSAVE")
        print("PASS: disk-full BGSAVE failed safely; live Bloom data remained intact")
    finally:
        if proc is not None:
            stop_redis(proc, conn)
        if log is not None and not log.closed:
            log.close()


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("corruption", "disk-full"):
        print(
            f"Usage: {sys.argv[0]} corruption <gemini_bloom.so> | "
            f"disk-full <gemini_bloom.so> <tmpfs-root>",
            file=sys.stderr,
        )
        return 2
    if sys.argv[1] == "corruption":
        corrupt_rdb(sys.argv[2])
        corrupt_aof(sys.argv[2])
    else:
        if len(sys.argv) != 4:
            return 2
        disk_full(sys.argv[2], sys.argv[3])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
