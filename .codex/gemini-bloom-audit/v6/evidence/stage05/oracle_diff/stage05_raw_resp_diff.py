#!/usr/bin/env python3
import argparse
import os
import shutil
import socket
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any


class RespClient:
    def __init__(self, port: int):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.file = self.sock.makefile("rb")

    def close(self):
        self.file.close()
        self.sock.close()

    @staticmethod
    def _bulk(arg: Any) -> bytes:
        if isinstance(arg, bytes):
            return arg
        return str(arg).encode()

    @classmethod
    def encode(cls, args: list[Any]) -> bytes:
        out = [b"*" + str(len(args)).encode() + b"\r\n"]
        for arg in args:
            b = cls._bulk(arg)
            out += [b"$" + str(len(b)).encode() + b"\r\n", b, b"\r\n"]
        return b"".join(out)

    def _readline(self) -> bytes:
        line = self.file.readline()
        if not line:
            raise RuntimeError("socket closed")
        return line

    def _read_frame(self) -> bytes:
        first = self.file.read(1)
        if not first:
            raise RuntimeError("socket closed")
        raw = [first]
        typ = first.decode(errors="replace")
        if typ in "+-:,.#":
            raw.append(self._readline())
        elif typ == "_":
            raw.append(self._readline())
        elif typ in "$!=":
            line = self._readline()
            raw.append(line)
            length = int(line[:-2])
            if length >= 0:
                raw.append(self.file.read(length + 2))
        elif typ in "*~>":
            line = self._readline()
            raw.append(line)
            count = int(line[:-2])
            if count >= 0:
                for _ in range(count):
                    raw.append(self._read_frame())
        elif typ == "%":
            line = self._readline()
            raw.append(line)
            count = int(line[:-2])
            for _ in range(count * 2):
                raw.append(self._read_frame())
        else:
            raise RuntimeError(f"unsupported RESP type {typ!r}")
        return b"".join(raw)

    def cmd_raw(self, args: list[Any]) -> tuple[bytes, bytes]:
        req = self.encode(args)
        self.sock.sendall(req)
        return req, self._read_frame()


def esc(data: bytes, limit: int = 4096) -> str:
    shown = data[:limit]
    text = "".join(chr(b) if 32 <= b < 127 and chr(b) not in "\\`" else f"\\x{b:02x}" for b in shown)
    if len(data) > limit:
        text += f"...<truncated {len(data) - limit} bytes>"
    return text


def start(redis_server: Path, module: Path, label: str) -> tuple[int, Path]:
    port_sock = socket.socket()
    port_sock.bind(("127.0.0.1", 0))
    port = port_sock.getsockname()[1]
    port_sock.close()
    workdir = Path(tempfile.mkdtemp(prefix=f"stage05-{label}-", dir="/tmp"))
    cmd = [
        str(redis_server),
        "--bind", "127.0.0.1",
        "--port", str(port),
        "--daemonize", "yes",
        "--loglevel", "notice",
        "--logfile", str(workdir / "redis.log"),
        "--dir", str(workdir),
        "--dbfilename", "dump.rdb",
        "--save", "",
        "--appendonly", "no",
        "--loadmodule", str(module),
    ]
    proc = subprocess.run(cmd, text=True, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"{label} start failed: {proc.stderr}")
    deadline = time.time() + 8
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return port, workdir
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"{label} did not start")


def stop(port: int):
    redis_cli = shutil.which("redis-cli")
    if redis_cli:
        subprocess.run([redis_cli, "-p", str(port), "SHUTDOWN", "NOSAVE"], capture_output=True, text=True, timeout=3)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--redis-server", required=True, type=Path)
    ap.add_argument("--gemini-module", required=True, type=Path)
    ap.add_argument("--redisbloom-module", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    args = ap.parse_args()

    g_port = rb_port = None
    g_client = rb_client = None
    try:
        g_port, g_dir = start(args.redis_server, args.gemini_module, "gemini")
        rb_port, rb_dir = start(args.redis_server, args.redisbloom_module, "redisbloom")
        g_client = RespClient(g_port)
        rb_client = RespClient(rb_port)
        cases: list[tuple[str, list[Any]]] = [
            ("module-list", ["MODULE", "LIST"]),
            ("command-info", ["COMMAND", "INFO", "BF.RESERVE", "BF.ADD", "BF.INFO", "BF.SCANDUMP", "BF.LOADCHUNK", "BF.DEBUG"]),
            ("hello3", ["HELLO", "3"]),
            ("reserve", ["BF.RESERVE", "key", "0.01", "10"]),
            ("add", ["BF.ADD", "key", "alpha"]),
            ("info-full", ["BF.INFO", "key"]),
            ("info-capacity", ["BF.INFO", "key", "CAPACITY"]),
            ("info-size", ["BF.INFO", "key", "SIZE"]),
            ("info-missing", ["BF.INFO", "missing"]),
            ("reserve-unknown-option", ["BF.RESERVE", "unknown", "0.01", "10", "BOGUS"]),
            ("insert-expansion-zero", ["BF.INSERT", "insert_exp0", "EXPANSION", "0", "ITEMS", "a"]),
            ("insert-nocreate-capacity", ["BF.INSERT", "nokey", "NOCREATE", "CAPACITY", "10", "ITEMS", "a"]),
            ("fixed-reserve", ["BF.RESERVE", "fixed", "0.01", "2", "NONSCALING"]),
            ("madd-partial", ["BF.MADD", "fixed", "a", "b", "c", "d"]),
            ("scandump-first", ["BF.SCANDUMP", "key", "0"]),
            ("bf-debug", ["BF.DEBUG", "key"]),
        ]
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("w") as f:
            f.write("# Stage 05 Focused Raw RESP Diff\n\n")
            f.write(f"Redis server: {args.redis_server}\n")
            f.write(f"Gemini module: {args.gemini_module} port={g_port} dir={g_dir}\n")
            f.write(f"RedisBloom module: {args.redisbloom_module} port={rb_port} dir={rb_dir}\n")
            for label, argv in cases:
                f.write(f"\n## {label}\n")
                f.write(f"COMMAND: {' '.join(str(a) for a in argv)}\n")
                for name, client in (("gemini", g_client), ("redisbloom", rb_client)):
                    req, resp = client.cmd_raw(argv)
                    f.write(f"\n### {name}\n")
                    f.write(f"REQUEST_BYTES: {len(req)}\n")
                    f.write(f"REQUEST_ESCAPED: {esc(req)}\n")
                    f.write(f"REPLY_BYTES: {len(resp)}\n")
                    f.write(f"REPLY_ESCAPED: {esc(resp)}\n")
        return 0
    finally:
        for client in (g_client, rb_client):
            if client:
                client.close()
        for port in (g_port, rb_port):
            if port:
                stop(port)


if __name__ == "__main__":
    raise SystemExit(main())
