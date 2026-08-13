#!/usr/bin/env python3
"""AOF short-write and fsync-error injection for GeminiBloom."""

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


def start_redis(module, directory, preload=None, mode=None):
    port = free_port()
    logfile = os.path.join(directory, f"redis-{port}.log")
    env = os.environ.copy()
    if preload:
        env.update(
            {
                "LD_PRELOAD": os.path.abspath(preload),
                "BLOOM_IO_FAULT_CONTROL": os.path.join(directory, "inject.control"),
                "BLOOM_IO_FAULT_HIT": os.path.join(directory, "inject.hit"),
                "BLOOM_IO_FAULT_MATCH": "appendonly",
                "BLOOM_IO_FAULT_MODE": mode,
            }
        )
    log = open(logfile, "a")
    proc = subprocess.Popen(
        [
            "redis-server",
            "--port", str(port),
            "--bind", "127.0.0.1",
            "--daemonize", "no",
            "--save", "",
            "--dir", directory,
            "--appendonly", "yes",
            "--appendfsync", "always",
            "--aof-use-rdb-preamble", "no",
            "--aof-load-truncated", "no",
            "--loadmodule", os.path.abspath(module),
        ],
        stdout=log,
        stderr=subprocess.STDOUT,
        env=env,
    )
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


def run_case(module, library, mode):
    directory = tempfile.mkdtemp(prefix=f"bloom-{mode}-")
    proc = conn = log = None
    try:
        proc, conn, log, logfile = start_redis(module, directory, library, mode)
        key = f"io-fault:{mode}".encode()
        conn.execute_command("BF.RESERVE", key, 0.001, 10000)
        conn.execute_command("BF.ADD", key, b"before-fault")

        open(os.path.join(directory, "inject.control"), "w").close()
        command_error = None
        try:
            conn.execute_command("BF.ADD", key, b"fault-window")
        except (ConnectionError, ResponseError) as exc:
            command_error = exc

        hit_path = os.path.join(directory, "inject.hit")
        for _ in range(200):
            if os.path.exists(hit_path):
                break
            time.sleep(0.01)
        else:
            raise AssertionError(f"{mode}: fault injector did not intercept the AOF descriptor")

        os.unlink(os.path.join(directory, "inject.control"))
        if mode == "fsync-error":
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass

        exited_on_fault = proc.poll() is not None
        if exited_on_fault:
            log.close()
            log = None
            proc, conn, log, logfile = start_redis(module, directory)

        conn.ping()
        if conn.execute_command("BF.EXISTS", key, b"before-fault") != 1:
            raise AssertionError(f"{mode}: existing Bloom data was damaged")
        if command_error is None and conn.execute_command("BF.EXISTS", key, b"fault-window") != 1:
            raise AssertionError(f"{mode}: acknowledged fault-window write was lost")
        if conn.execute_command("BF.ADD", key, b"after-recovery") != 1:
            raise AssertionError(f"{mode}: writes did not recover after one-shot fault")

        stop_redis(proc, conn)
        proc = conn = None
        log.close()
        log = None

        proc, conn, log, logfile = start_redis(module, directory)
        required = [b"before-fault", b"after-recovery"]
        if command_error is None:
            required.append(b"fault-window")
        if conn.execute_command("BF.MEXISTS", key, *required) != [1] * len(required):
            raise AssertionError(f"{mode}: AOF replay lost acknowledged Bloom data")
        stop_redis(proc, conn)
        proc = conn = None
        behavior = "Redis exited and restarted" if exited_on_fault else "Redis stayed available"
        print(f"PASS: {mode}, {behavior}; acknowledged data replayed successfully")
    finally:
        if proc is not None:
            stop_redis(proc, conn)
        if log is not None and not log.closed:
            log.close()
        shutil.rmtree(directory, ignore_errors=True)


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <gemini_bloom.so> <fault_inject.so>", file=sys.stderr)
        return 2
    for mode in ("short-write", "fsync-error"):
        run_case(sys.argv[1], sys.argv[2], mode)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
