#!/usr/bin/env python3
"""RedisBloom interoperability probe for the v5 gemini-bloom audit.

Run inside the Docker test container, for example:

  python3 doc/code_review/gemini-bloom/v5/redisbloom_interop_probe.py \
    --gemini-module /tmp/gemini-module-v5-docker-build/redis_bloom.so \
    --redisbloom-module /tmp/redisbloom-v2.8.20/bin/linux-x64-release/redisbloom.so

The probe intentionally uses a minimal RESP2 client so SCANDUMP/LOADCHUNK
binary payloads are preserved exactly.
"""

from __future__ import annotations

import argparse
import os
import shutil
import socket
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


BASE_PORT = 46380
BASE_DIR = Path("/tmp/gemini-bloom-v5-interop")


class RedisError(Exception):
    pass


class RedisClient:
    def __init__(self, port: int):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=2.0)
        self.file = self.sock.makefile("rb")

    def close(self) -> None:
        self.file.close()
        self.sock.close()

    def cmd(self, *args: Any) -> Any:
        parts = [f"*{len(args)}\r\n".encode()]
        for arg in args:
            if isinstance(arg, bytes):
                data = arg
            else:
                data = str(arg).encode()
            parts.append(f"${len(data)}\r\n".encode())
            parts.append(data)
            parts.append(b"\r\n")
        self.sock.sendall(b"".join(parts))
        return self._read(nested=False)

    def _read(self, nested: bool) -> Any:
        prefix = self.file.read(1)
        if not prefix:
            raise EOFError("connection closed")
        line = self.file.readline()
        if not line.endswith(b"\r\n"):
            raise EOFError("malformed RESP line")
        payload = line[:-2]
        if prefix == b"+":
            return payload.decode(errors="replace")
        if prefix == b"-":
            message = payload.decode(errors="replace")
            if nested:
                return {"error": message}
            raise RedisError(message)
        if prefix == b":":
            return int(payload)
        if prefix == b"$":
            length = int(payload)
            if length < 0:
                return None
            data = self.file.read(length)
            crlf = self.file.read(2)
            if crlf != b"\r\n":
                raise EOFError("malformed bulk string")
            return data
        if prefix == b"*":
            count = int(payload)
            if count < 0:
                return None
            return [self._read(nested=True) for _ in range(count)]
        raise EOFError(f"unsupported RESP prefix {prefix!r}")


@dataclass
class Server:
    name: str
    port: int
    module: Path
    workdir: Path

    def start(
        self,
        appendonly: bool = False,
        preserve_dir: bool = False,
        aof_preamble: bool = True,
    ) -> RedisClient:
        if not preserve_dir:
            shutil.rmtree(self.workdir, ignore_errors=True)
            self.workdir.mkdir(parents=True)
        else:
            self.workdir.mkdir(parents=True, exist_ok=True)
        args = [
            "redis-server",
            "--bind", "127.0.0.1",
            "--port", str(self.port),
            "--daemonize", "yes",
            "--loglevel", "notice",
            "--logfile", str(self.workdir / "redis.log"),
            "--dir", str(self.workdir),
            "--dbfilename", "dump.rdb",
            "--save", "",
            "--appendonly", "yes" if appendonly else "no",
            "--aof-use-rdb-preamble", "yes" if aof_preamble else "no",
            "--loadmodule", str(self.module),
        ]
        subprocess.run(args, check=True)
        deadline = time.time() + 5
        last_error: Exception | None = None
        while time.time() < deadline:
            try:
                client = RedisClient(self.port)
                if client.cmd("PING") == "PONG":
                    return client
            except Exception as exc:  # pragma: no cover - diagnostic path
                last_error = exc
                time.sleep(0.05)
        raise RuntimeError(f"{self.name} did not start: {last_error}; log={self.log_tail()}")

    def stop(self, save: bool = False) -> None:
        try:
            client = RedisClient(self.port)
            try:
                client.cmd("SHUTDOWN", "SAVE" if save else "NOSAVE")
            except (EOFError, RedisError):
                pass
            finally:
                client.close()
        except OSError:
            pass

    def log_tail(self, limit: int = 20) -> str:
        log = self.workdir / "redis.log"
        if not log.exists():
            return "<no log>"
        lines = log.read_text(errors="replace").splitlines()
        return "\n".join(lines[-limit:])


def safe_cmd(client: RedisClient, *args: Any) -> Any:
    try:
        return client.cmd(*args)
    except RedisError as exc:
        return {"error": str(exc)}


def dump_chunks(client: RedisClient, key: str, limit: int = 100) -> list[tuple[int, bytes | None]]:
    cursor = 0
    chunks: list[tuple[int, bytes | None]] = []
    for _ in range(limit):
        reply = client.cmd("BF.SCANDUMP", key, cursor)
        if not isinstance(reply, list) or len(reply) != 2:
            raise RuntimeError(f"unexpected SCANDUMP reply: {reply!r}")
        next_cursor = int(reply[0])
        data = reply[1]
        chunks.append((next_cursor, data))
        if next_cursor == 0:
            return chunks
        cursor = next_cursor
    raise RuntimeError("SCANDUMP did not terminate")


def load_chunks(client: RedisClient, key: str, chunks: list[tuple[int, bytes | None]]) -> list[Any]:
    replies = []
    for cursor, data in chunks:
        if cursor == 0:
            continue
        replies.append(safe_cmd(client, "BF.LOADCHUNK", key, cursor, data or b""))
    return replies


def populate(client: RedisClient, key: str, count: int = 40) -> list[str]:
    client.cmd("BF.RESERVE", key, "0.01", "10", "EXPANSION", "2")
    items = [f"item:{i}" for i in range(count)]
    for item in items:
        client.cmd("BF.ADD", key, item)
    return items


def summarize_chunks(chunks: list[tuple[int, bytes | None]]) -> list[tuple[int, int]]:
    return [(cursor, 0 if data is None else len(data)) for cursor, data in chunks]


def check_items(client: RedisClient, key: str, items: list[str]) -> tuple[int, list[str]]:
    missing = [item for item in items if client.cmd("BF.EXISTS", key, item) != 1]
    return len(items) - len(missing), missing[:5]


def rdb_cross_load(source: Server, target: Server, key: str) -> dict[str, Any]:
    shutil.rmtree(source.workdir, ignore_errors=True)
    client = source.start()
    items = populate(client, key)
    save_reply = safe_cmd(client, "SAVE")
    source.stop(save=False)

    shutil.rmtree(target.workdir, ignore_errors=True)
    shutil.copytree(source.workdir, target.workdir)
    try:
        target_client = target.start(preserve_dir=True)
    except Exception as exc:
        return {"save": save_reply, "start_error": str(exc), "log": target.log_tail()}
    try:
        card = safe_cmd(target_client, "BF.CARD", key)
        found, missing = check_items(target_client, key, items)
        return {"save": save_reply, "card": card, "found": found, "missing": missing}
    finally:
        target_client.close()
        target.stop()


def wait_for_aof_rewrite(client: RedisClient) -> None:
    client.cmd("BGREWRITEAOF")
    deadline = time.time() + 10
    while time.time() < deadline:
        info = client.cmd("INFO", "persistence")
        text = info.decode() if isinstance(info, bytes) else str(info)
        fields = dict(
            line.split(":", 1)
            for line in text.splitlines()
            if ":" in line and not line.startswith("#")
        )
        if fields.get("aof_rewrite_in_progress") == "0":
            if fields.get("aof_last_bgrewrite_status") == "ok":
                return
            raise RuntimeError(f"AOF rewrite failed: {fields.get('aof_last_bgrewrite_status')}")
        time.sleep(0.05)
    raise RuntimeError("AOF rewrite timed out")


def aof_cross_replay(
    source: Server,
    target: Server,
    key: str,
    aof_preamble: bool,
) -> dict[str, Any]:
    shutil.rmtree(source.workdir, ignore_errors=True)
    client = source.start(appendonly=True, aof_preamble=aof_preamble)
    items = populate(client, key)
    try:
        wait_for_aof_rewrite(client)
    except Exception as exc:
        source.stop()
        return {"rewrite_error": str(exc), "log": source.log_tail()}
    source.stop(save=False)

    shutil.rmtree(target.workdir, ignore_errors=True)
    shutil.copytree(source.workdir, target.workdir)
    try:
        target_client = target.start(
            appendonly=True,
            preserve_dir=True,
            aof_preamble=aof_preamble,
        )
    except Exception as exc:
        return {"start_error": str(exc), "log": target.log_tail()}
    try:
        card = safe_cmd(target_client, "BF.CARD", key)
        found, missing = check_items(target_client, key, items)
        return {"card": card, "found": found, "missing": missing}
    finally:
        target_client.close()
        target.stop()


def run_probe(gemini_module: Path, redisbloom_module: Path) -> dict[str, Any]:
    BASE_DIR.mkdir(parents=True, exist_ok=True)
    gemini = Server("gemini", BASE_PORT, gemini_module, BASE_DIR / "gemini")
    rb = Server("redisbloom", BASE_PORT + 1, redisbloom_module, BASE_DIR / "redisbloom")
    results: dict[str, Any] = {}

    gemini.stop()
    rb.stop()

    g = gemini.start()
    r = rb.start()
    try:
        for client, label in [(g, "gemini"), (r, "redisbloom")]:
            safe_cmd(client, "DEL", "info")
            safe_cmd(client, "BF.RESERVE", "info", "0.01", "100")
            safe_cmd(client, "BF.ADD", "info", "alpha")
            results[f"{label}_info_full"] = safe_cmd(client, "BF.INFO", "info")
            results[f"{label}_info_capacity"] = safe_cmd(client, "BF.INFO", "info", "CAPACITY")
            results[f"{label}_info_size"] = safe_cmd(client, "BF.INFO", "info", "SIZE")

        results["gemini_reserve_error_0_5"] = safe_cmd(g, "BF.RESERVE", "rate05", "0.5", "100")
        results["redisbloom_reserve_error_0_5"] = safe_cmd(r, "BF.RESERVE", "rate05", "0.5", "100")
        results["gemini_insert_nocreate_capacity"] = safe_cmd(
            g, "BF.INSERT", "nokey", "NOCREATE", "CAPACITY", "10", "ITEMS", "a"
        )
        results["redisbloom_insert_nocreate_capacity"] = safe_cmd(
            r, "BF.INSERT", "nokey", "NOCREATE", "CAPACITY", "10", "ITEMS", "a"
        )

        for client, label in [(g, "gemini"), (r, "redisbloom")]:
            safe_cmd(client, "DEL", "fixed")
            results[f"{label}_reserve_fixed"] = safe_cmd(
                client, "BF.RESERVE", "fixed", "0.01", "2", "NONSCALING"
            )
            results[f"{label}_madd_fixed_overflow"] = safe_cmd(
                client, "BF.MADD", "fixed", "a", "b", "c", "d"
            )
            results[f"{label}_card_after_madd"] = safe_cmd(client, "BF.CARD", "fixed")

            safe_cmd(client, "DEL", "fixed_insert")
            results[f"{label}_reserve_fixed_insert"] = safe_cmd(
                client, "BF.RESERVE", "fixed_insert", "0.01", "2", "NONSCALING"
            )
            results[f"{label}_insert_fixed_overflow"] = safe_cmd(
                client, "BF.INSERT", "fixed_insert", "ITEMS", "a", "b", "c", "d"
            )
            results[f"{label}_card_after_insert"] = safe_cmd(client, "BF.CARD", "fixed_insert")

        rb_items = populate(r, "rb_src")
        rb_chunks = dump_chunks(r, "rb_src")
        results["redisbloom_chunks"] = summarize_chunks(rb_chunks)
        results["redisbloom_to_gemini_load_replies"] = load_chunks(g, "rb_loaded", rb_chunks)
        results["redisbloom_to_gemini_card"] = safe_cmd(g, "BF.CARD", "rb_loaded")
        if not isinstance(results["redisbloom_to_gemini_card"], dict):
            results["redisbloom_to_gemini_found"] = check_items(g, "rb_loaded", rb_items)

        g.close()
        r.close()
        gemini.stop()
        rb.stop()
        g = gemini.start()
        r = rb.start()

        g_items = populate(g, "g_src")
        g_chunks = dump_chunks(g, "g_src")
        results["gemini_chunks"] = summarize_chunks(g_chunks)
        results["gemini_to_redisbloom_load_replies"] = load_chunks(r, "g_loaded", g_chunks)
        results["gemini_to_redisbloom_card"] = safe_cmd(r, "BF.CARD", "g_loaded")
        if not isinstance(results["gemini_to_redisbloom_card"], dict):
            results["gemini_to_redisbloom_found"] = check_items(r, "g_loaded", g_items)
    finally:
        g.close()
        r.close()
        gemini.stop()
        rb.stop()

    results["rdb_redisbloom_to_gemini"] = rdb_cross_load(
        Server("redisbloom-rdb-src", BASE_PORT + 2, redisbloom_module, BASE_DIR / "rdb-rb-src"),
        Server("gemini-rdb-dst", BASE_PORT + 3, gemini_module, BASE_DIR / "rdb-g-dst"),
        "rdb_rb_src",
    )
    results["rdb_gemini_to_redisbloom"] = rdb_cross_load(
        Server("gemini-rdb-src", BASE_PORT + 4, gemini_module, BASE_DIR / "rdb-g-src"),
        Server("redisbloom-rdb-dst", BASE_PORT + 5, redisbloom_module, BASE_DIR / "rdb-rb-dst"),
        "rdb_g_src",
    )
    results["aof_command_gemini_to_redisbloom"] = aof_cross_replay(
        Server("gemini-aof-src", BASE_PORT + 6, gemini_module, BASE_DIR / "aof-g-src"),
        Server("redisbloom-aof-dst", BASE_PORT + 7, redisbloom_module, BASE_DIR / "aof-rb-dst"),
        "aof_g_src",
        aof_preamble=False,
    )
    results["aof_command_redisbloom_to_gemini"] = aof_cross_replay(
        Server("redisbloom-aof-src", BASE_PORT + 8, redisbloom_module, BASE_DIR / "aof-rb-src"),
        Server("gemini-aof-dst", BASE_PORT + 9, gemini_module, BASE_DIR / "aof-g-dst"),
        "aof_rb_src",
        aof_preamble=False,
    )
    results["aof_rdb_preamble_gemini_to_redisbloom"] = aof_cross_replay(
        Server("gemini-aof-rdb-src", BASE_PORT + 10, gemini_module, BASE_DIR / "aof-rdb-g-src"),
        Server("redisbloom-aof-rdb-dst", BASE_PORT + 11, redisbloom_module, BASE_DIR / "aof-rdb-rb-dst"),
        "aof_rdb_g_src",
        aof_preamble=True,
    )
    results["aof_rdb_preamble_redisbloom_to_gemini"] = aof_cross_replay(
        Server("redisbloom-aof-rdb-src", BASE_PORT + 12, redisbloom_module, BASE_DIR / "aof-rdb-rb-src"),
        Server("gemini-aof-rdb-dst", BASE_PORT + 13, gemini_module, BASE_DIR / "aof-rdb-g-dst"),
        "aof_rdb_rb_src",
        aof_preamble=True,
    )
    return results


def printable(value: Any) -> Any:
    if isinstance(value, bytes):
        preview = value[:32].hex()
        suffix = "" if len(value) <= 32 else "..."
        return f"bytes[{len(value)}]:{preview}{suffix}"
    if isinstance(value, list):
        return [printable(v) for v in value]
    if isinstance(value, tuple):
        return tuple(printable(v) for v in value)
    if isinstance(value, dict):
        return {k: printable(v) for k, v in value.items()}
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gemini-module", required=True, type=Path)
    parser.add_argument("--redisbloom-module", required=True, type=Path)
    args = parser.parse_args()

    if not args.gemini_module.exists():
        raise SystemExit(f"gemini module not found: {args.gemini_module}")
    if not args.redisbloom_module.exists():
        raise SystemExit(f"RedisBloom module not found: {args.redisbloom_module}")

    results = run_probe(args.gemini_module, args.redisbloom_module)
    for key in sorted(results):
        print(f"{key}: {printable(results[key])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
