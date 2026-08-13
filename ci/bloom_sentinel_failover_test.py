#!/usr/bin/env python3
"""Redis Sentinel automatic-failover checks for GeminiBloom."""

import os
import shutil
import subprocess
import sys
import tempfile
import time

import redis
from redis.exceptions import ConnectionError, ResponseError


HOST = "127.0.0.1"
REDIS_PORTS = (18000, 18001, 18002)
SENTINEL_PORTS = (18100, 18101, 18102)
MASTER_NAME = "bloom-master"


def wait_ready(port, proc, logfile):
    conn = redis.Redis(host=HOST, port=port, decode_responses=False, protocol=2)
    for _ in range(400):
        if proc.poll() is not None:
            raise RuntimeError(f"process:{port} exited early:\n{open(logfile).read()}")
        try:
            conn.ping()
            return conn
        except (ConnectionError, OSError):
            time.sleep(0.05)
    raise RuntimeError(f"process:{port} did not become ready")


def start_redis(module, root, port):
    directory = os.path.join(root, f"redis-{port}")
    os.makedirs(directory, exist_ok=True)
    logfile = os.path.join(directory, "redis.log")
    log = open(logfile, "a")
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
            "--loadmodule", os.path.abspath(module),
        ],
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    return proc, wait_ready(port, proc, logfile), log


def start_sentinel(root, port):
    directory = os.path.join(root, f"sentinel-{port}")
    os.makedirs(directory)
    config = os.path.join(directory, "sentinel.conf")
    logfile = os.path.join(directory, "sentinel.log")
    with open(config, "w") as output:
        output.write(
            f"port {port}\n"
            f"bind {HOST}\n"
            "protected-mode no\n"
            "daemonize no\n"
            f"dir {directory}\n"
            f"sentinel monitor {MASTER_NAME} {HOST} {REDIS_PORTS[0]} 2\n"
            f"sentinel down-after-milliseconds {MASTER_NAME} 1000\n"
            f"sentinel failover-timeout {MASTER_NAME} 15000\n"
            f"sentinel parallel-syncs {MASTER_NAME} 1\n"
        )
    log = open(logfile, "w")
    proc = subprocess.Popen(
        ["redis-server", config, "--sentinel"],
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    return proc, wait_ready(port, proc, logfile), log


def info(conn, section="replication"):
    result = conn.info(section)
    if isinstance(result, dict):
        return result
    return {result[index]: result[index + 1] for index in range(0, len(result), 2)}


def wait_replica(conn, master_port, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            state = info(conn)
            role = state.get("role") or state.get(b"role")
            link = state.get("master_link_status") or state.get(b"master_link_status")
            port = state.get("master_port") or state.get(b"master_port")
            if role in ("slave", b"slave") and link in ("up", b"up") and int(port) == master_port:
                return
        except (ConnectionError, ResponseError, TypeError):
            pass
        time.sleep(0.1)
    raise RuntimeError(f"replica did not synchronize with master:{master_port}")


def sentinel_master(conn):
    reply = conn.execute_command("SENTINEL", "get-master-addr-by-name", MASTER_NAME)
    if not reply:
        return None
    return reply[0].decode(), int(reply[1])


def optional_card(conn, key):
    try:
        return conn.execute_command("BF.CARD", key)
    except ResponseError as exc:
        if "unknown command" in str(exc).lower():
            return None
        raise


def wait_failover(sentinels, old_port, timeout=60):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        answers = []
        for sentinel in sentinels:
            try:
                answers.append(sentinel_master(sentinel))
            except (ConnectionError, ResponseError):
                answers.append(None)
        candidates = [answer for answer in answers if answer and answer[1] != old_port]
        if candidates and len(set(candidates)) == 1:
            new_master = redis.Redis(
                host=candidates[0][0], port=candidates[0][1],
                decode_responses=False, protocol=2,
            )
            try:
                role = info(new_master).get("role")
                if role == "master":
                    return candidates[0][1], new_master
            except (ConnectionError, ResponseError):
                pass
        time.sleep(0.2)
    raise RuntimeError("Sentinel did not complete automatic failover")


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <gemini_bloom.so>", file=sys.stderr)
        return 2

    root = tempfile.mkdtemp(prefix="bloom-sentinel-")
    redis_procs = {}
    redis_logs = {}
    sentinel_procs = []
    sentinel_logs = []
    try:
        redis_conns = {}
        for port in REDIS_PORTS:
            proc, conn, log = start_redis(sys.argv[1], root, port)
            redis_procs[port] = proc
            redis_conns[port] = conn
            redis_logs[port] = log

        for port in REDIS_PORTS[1:]:
            redis_conns[port].execute_command("SLAVEOF", HOST, REDIS_PORTS[0])
            wait_replica(redis_conns[port], REDIS_PORTS[0])

        sentinels = []
        for port in SENTINEL_PORTS:
            proc, conn, log = start_sentinel(root, port)
            sentinel_procs.append(proc)
            sentinels.append(conn)
            sentinel_logs.append(log)

        for _ in range(300):
            if all(sentinel_master(conn) == (HOST, REDIS_PORTS[0]) for conn in sentinels):
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("Sentinels did not agree on the initial master")

        master = redis_conns[REDIS_PORTS[0]]
        key = b"sentinel:bloom"
        items = [f"sentinel-item:{index}".encode() for index in range(2000)]
        master.execute_command("BF.RESERVE", key, 0.001, 5000)
        master.execute_command("BF.MADD", key, *items)
        if master.execute_command("WAIT", 2, 15000) != 2:
            raise AssertionError("initial Bloom data did not reach both replicas")

        for port in REDIS_PORTS[1:]:
            if redis_conns[port].execute_command("BF.MEXISTS", key, *items) != [1] * len(items):
                raise AssertionError(f"replica:{port} membership mismatch before failover")

        old_proc = redis_procs[REDIS_PORTS[0]]
        old_proc.terminate()
        old_proc.wait(timeout=10)
        new_port, new_master = wait_failover(sentinels, REDIS_PORTS[0])

        if new_master.execute_command("BF.MEXISTS", key, *items) != [1] * len(items):
            raise AssertionError("membership mismatch on promoted master")
        promoted_card = optional_card(new_master, key)
        if promoted_card is not None and promoted_card != len(items):
            raise AssertionError("CARD mismatch on promoted master")
        if new_master.execute_command("BF.ADD", key, b"after-failover") != 1:
            raise AssertionError("write failed on promoted master")

        surviving_replica_ports = [port for port in REDIS_PORTS[1:] if port != new_port]
        if surviving_replica_ports:
            wait_replica(redis_conns[surviving_replica_ports[0]], new_port, timeout=45)
            if new_master.execute_command("WAIT", 1, 15000) < 1:
                raise AssertionError("post-failover write did not replicate")

        print(f"PASS: Sentinel promoted redis:{new_port} and replicated post-failover data")
        return 0
    finally:
        for conn in locals().get("sentinels", []):
            try:
                conn.execute_command("SHUTDOWN", "NOSAVE")
            except Exception:
                pass
        for proc in sentinel_procs:
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        for conn in locals().get("redis_conns", {}).values():
            try:
                conn.execute_command("SHUTDOWN", "NOSAVE")
            except Exception:
                pass
        for proc in redis_procs.values():
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        for log in list(redis_logs.values()) + sentinel_logs:
            if not log.closed:
                log.close()
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
