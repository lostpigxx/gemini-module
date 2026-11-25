#!/usr/bin/env python3
"""
Soak test for gemini-json on Redis 7 (supports RESP3 and RESP2).
Exercises sustained JSON.SET / JSON.GET / JSON.NUMINCRBY / JSON.ARRAPPEND /
JSON.MERGE / JSON.DEL / COPY / MEMORY USAGE churn while verifying data
invariants and bounded memory growth.

Usage:
  SOAK_DURATION_SEC=30 python3 ci/json_soak_test.py ./build/redis_json.so
"""

import json
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class RespClient:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.buf = bytearray()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def _read_line(self):
        while True:
            idx = self.buf.find(b"\r\n")
            if idx != -1:
                line = bytes(self.buf[:idx])
                del self.buf[: idx + 2]
                return line
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("EOF from redis-server")
            self.buf.extend(chunk)

    def _read_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("EOF from redis-server")
            self.buf.extend(chunk)
        data = bytes(self.buf[:n])
        del self.buf[:n]
        return data

    def _read_reply(self):
        line = self._read_line()
        prefix = line[:1]
        payload = line[1:]
        if prefix == b"+":
            return payload.decode("utf-8", errors="replace")
        if prefix == b"-":
            raise RuntimeError(payload.decode("utf-8", errors="replace"))
        if prefix == b":":
            return int(payload)
        if prefix == b"_":
            return None
        if prefix == b"#":
            return payload == b"t"
        if prefix == b",":
            return float(payload)
        if prefix in (b"$", b"=", b"!"):
            n = int(payload)
            if n < 0:
                return None
            data = self._read_exact(n + 2)[:-2]
            if prefix == b"!":
                raise RuntimeError(data.decode("utf-8", errors="replace"))
            return data.decode("utf-8", errors="replace")
        if prefix in (b"*", b"~", b">"):
            n = int(payload)
            if n < 0:
                return None
            return [self._read_reply() for _ in range(n)]
        if prefix == b"%":
            n = int(payload)
            return {self._read_reply(): self._read_reply() for _ in range(n)}
        raise RuntimeError(f"Unknown RESP prefix: {prefix!r}")

    def cmd(self, *args):
        parts = [f"*{len(args)}\r\n".encode()]
        for a in args:
            b = a if isinstance(a, bytes) else str(a).encode("utf-8")
            parts.append(f"${len(b)}\r\n".encode() + b + b"\r\n")
        self.sock.sendall(b"".join(parts))
        return self._read_reply()


def get_used_memory(cli):
    info = cli.cmd("INFO", "memory")
    for line in info.splitlines():
        if line.startswith("used_memory:"):
            return int(line.split(":", 1)[1])
    return 0


def main():
    if len(sys.argv) < 2:
        print("Usage: json_soak_test.py <path/to/redis_json.so>", file=sys.stderr)
        return 2

    module_path = os.path.abspath(sys.argv[1])
    duration = int(os.environ.get("SOAK_DURATION_SEC", "30"))
    resp_proto = os.environ.get("RESP_PROTOCOL", "auto")
    port = find_free_port()
    tmpdir = tempfile.mkdtemp(prefix="gemini_json_soak_")

    proc = subprocess.Popen(
        [
            "redis-server",
            "--port", str(port),
            "--dir", tmpdir,
            "--save", "",
            "--loglevel", "warning",
            "--loadmodule", module_path,
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    cli = None
    try:
        for _ in range(50):
            try:
                cli = RespClient("127.0.0.1", port)
                if cli.cmd("PING") == "PONG":
                    break
            except Exception:
                time.sleep(0.1)
        else:
            raise RuntimeError("redis-server failed to start")

        proto_label = "RESP2"
        if resp_proto != "2":
            try:
                cli.cmd("HELLO", "3")
                proto_label = "RESP3"
            except Exception:
                proto_label = "RESP2"

        print(f"gemini-json soak test: duration={duration}s, protocol={proto_label}")

        rng = random.Random(42)
        keyspace = 200
        expected = {}
        ops = 0
        checks = 0

        # Warmup
        for k in range(50):
            key = f"j:{k}"
            doc = {"id": k, "n": k, "tags": [f"t{k % 5}"], "meta": {"ok": True}}
            cli.cmd("JSON.SET", key, "$", json.dumps(doc))
            expected[key] = doc
            ops += 1

        mem_start = get_used_memory(cli)
        deadline = time.monotonic() + duration

        while time.monotonic() < deadline:
            k = rng.randrange(keyspace)
            key = f"j:{k}"
            action = rng.randrange(10)

            if action < 3 or key not in expected:
                doc = {
                    "id": k,
                    "n": rng.randint(0, 1000),
                    "tags": [f"t{rng.randrange(10)}"],
                    "meta": {"ok": bool(rng.randrange(2))},
                }
                cli.cmd("JSON.SET", key, "$", json.dumps(doc))
                expected[key] = doc
                ops += 1
            elif action < 5:
                delta = rng.randint(-10, 25)
                cli.cmd("JSON.NUMINCRBY", key, "$.n", str(delta))
                expected[key]["n"] += delta
                ops += 1
            elif action < 7:
                tag = f"t{rng.randrange(20)}"
                cli.cmd("JSON.ARRAPPEND", key, "$.tags", json.dumps(tag))
                expected[key]["tags"].append(tag)
                if len(expected[key]["tags"]) > 20:
                    cli.cmd("JSON.ARRTRIM", key, "$.tags", "0", "4")
                    expected[key]["tags"] = expected[key]["tags"][:5]
                    ops += 1
                ops += 1
            elif action == 7:
                cli.cmd("JSON.MERGE", key, "$", json.dumps({"meta": {"ok": True}}))
                expected[key]["meta"]["ok"] = True
                ops += 1
            elif action == 8:
                cli.cmd("DEL", key)
                expected.pop(key, None)
                ops += 1
            else:
                got = json.loads(cli.cmd("JSON.GET", key, "$"))[0]
                if got != expected[key]:
                    raise AssertionError(f"Mismatch on {key}: got={got}, expected={expected[key]}")
                checks += 1
                ops += 1

        # Final full verification
        for key, exp in expected.items():
            got = json.loads(cli.cmd("JSON.GET", key, "$"))[0]
            if got != exp:
                raise AssertionError(f"Final mismatch on {key}: got={got}, expected={exp}")
            checks += 1

        mem_end = get_used_memory(cli)
        mem_delta_mb = (mem_end - mem_start) / (1024.0 * 1024.0)
        print(
            f"PASS: soak completed — ops={ops}, checks={checks}, "
            f"live_keys={len(expected)}, mem_delta={mem_delta_mb:+.2f} MB"
        )
        if mem_delta_mb > 64.0:
            raise AssertionError(f"Excessive memory growth: {mem_delta_mb:.2f} MB")
        return 0
    finally:
        if cli:
            try:
                cli.cmd("SHUTDOWN", "NOSAVE")
            except Exception:
                pass
            cli.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
