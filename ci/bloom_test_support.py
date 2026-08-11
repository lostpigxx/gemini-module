"""Shared Redis process helpers for focused GeminiBloom regression tests."""

import os
import socket
import subprocess
import time

import redis


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def start_redis(module_path, directory, extra_args=None):
    port = find_free_port()
    logfile = os.path.join(directory, f"redis-{port}.log")
    with open(logfile, "w") as log:
        proc = subprocess.Popen(
            [
                "redis-server",
                "--port", str(port),
                "--bind", "127.0.0.1",
                "--daemonize", "no",
                "--loglevel", "warning",
                "--save", "",
                "--dir", directory,
                "--dbfilename", "dump.rdb",
                "--loadmodule", os.path.abspath(module_path),
                *(extra_args or []),
            ],
            stdout=log,
            stderr=subprocess.STDOUT,
        )

    for _ in range(600):
        time.sleep(0.05)
        if proc.poll() is not None:
            with open(logfile) as log:
                raise RuntimeError(f"redis-server exited early:\n{log.read()}")
        try:
            conn = redis.Redis(
                host="127.0.0.1", port=port, decode_responses=False, protocol=2
            )
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
        if (
            not info.get("rdb_bgsave_in_progress", 0)
            and info.get("rdb_changes_since_last_save") == 0
        ):
            if info.get("rdb_last_bgsave_status") != "ok":
                raise RuntimeError(
                    f"BGSAVE failed: {info.get('rdb_last_bgsave_status')!r}"
                )
            return
        time.sleep(0.05)
    raise RuntimeError(f"BGSAVE did not complete within {timeout}s")


def info_dict(raw):
    if isinstance(raw, dict):
        return raw
    return {raw[index]: raw[index + 1] for index in range(0, len(raw), 2)}
