#!/usr/bin/env python3
"""Matrix compatibility runner for the gemini-bloom v5 audit.

This is intentionally an audit harness, not a production test runner. It uses
the Redis/RedisBloom binaries that are already present in the Docker container
and records enough detail to prove which compatibility cells pass or fail.
"""

from __future__ import annotations

import argparse
import csv
import json
import shutil
import socket
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


BASE_DIR = Path("/tmp/gemini-bloom-v5-compat-matrix")
DEFAULT_ENV_ROOT = Path("/workspace/projects/Environments/OpenSourceRedis")
DEFAULT_TSV = DEFAULT_ENV_ROOT / "redisbloom_versions.tsv"


class RedisError(Exception):
    pass


class RedisClient:
    def __init__(self, port: int):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5.0)
        self.file = self.sock.makefile("rb")

    def close(self) -> None:
        self.file.close()
        self.sock.close()

    def cmd(self, *args: Any) -> Any:
        parts = [f"*{len(args)}\r\n".encode()]
        for arg in args:
            data = arg if isinstance(arg, bytes) else str(arg).encode()
            parts.extend([f"${len(data)}\r\n".encode(), data, b"\r\n"])
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
            if self.file.read(2) != b"\r\n":
                raise EOFError("malformed bulk string")
            return data
        if prefix == b"*":
            count = int(payload)
            if count < 0:
                return None
            return [self._read(nested=True) for _ in range(count)]
        raise EOFError(f"unsupported RESP prefix {prefix!r}")


def safe_cmd(client: RedisClient, *args: Any) -> Any:
    try:
        return client.cmd(*args)
    except RedisError as exc:
        return {"error": str(exc)}


def json_safe(value: Any) -> Any:
    if isinstance(value, bytes):
        preview = value[:24].hex()
        suffix = "" if len(value) <= 24 else "..."
        return {"bytes": len(value), "hex": preview + suffix}
    if isinstance(value, list):
        return [json_safe(v) for v in value]
    if isinstance(value, tuple):
        return [json_safe(v) for v in value]
    if isinstance(value, dict):
        return {str(k): json_safe(v) for k, v in value.items()}
    return value


class PortAllocator:
    def __init__(self, start: int):
        self.next_port = start
        self.used: set[int] = set()

    def take(self) -> int:
        # Ask the container kernel for an actually free TCP port. Sequential
        # ports caused intermittent Redis bind failures in this Docker setup.
        for _ in range(100):
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                sock.bind(("127.0.0.1", 0))
                port = int(sock.getsockname()[1])
            if port not in self.used:
                self.used.add(port)
                return port
        # Fallback should be effectively unreachable, but keeps the class total.
        while self.next_port in self.used:
            self.next_port += 1
        port = self.next_port
        self.used.add(port)
        self.next_port += 1
        return port


@dataclass(frozen=True)
class CompatEnv:
    name: str
    redis_tag: str
    redisbloom_tag: str
    module_ver: str
    redis_server: Path
    redisbloom_module: Path


@dataclass
class Server:
    label: str
    redis_server: Path
    module: Path
    port: int
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
        self.stop()
        args = [
            str(self.redis_server),
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
        proc = subprocess.run(args, text=True, capture_output=True)
        if proc.returncode != 0:
            raise RuntimeError(
                f"{self.label} start command failed rc={proc.returncode}: "
                f"stdout={proc.stdout!r} stderr={proc.stderr!r}"
            )
        deadline = time.time() + 8
        last_error: Exception | None = None
        while time.time() < deadline:
            try:
                client = RedisClient(self.port)
                if client.cmd("PING") == "PONG":
                    return client
                client.close()
            except Exception as exc:
                last_error = exc
                time.sleep(0.05)
        raise RuntimeError(f"{self.label} did not start: {last_error}; log={self.log_tail()}")

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

    def log_tail(self, limit: int = 25) -> str:
        log = self.workdir / "redis.log"
        if not log.exists():
            return "<no log>"
        return "\n".join(log.read_text(errors="replace").splitlines()[-limit:])

    def critical_log_count(self) -> int:
        log = self.workdir / "redis.log"
        if not log.exists():
            return 0
        text = log.read_text(errors="replace")
        return text.count("== CRITICAL ==")


@dataclass(frozen=True)
class Corpus:
    name: str
    reserve_args: tuple[Any, ...]
    items: tuple[bytes, ...]
    note: str


def b(text: str) -> bytes:
    return text.encode()


def build_corpora(include_large: bool) -> list[Corpus]:
    corpora = [
        Corpus("empty_scaling", ("0.01", "100"), tuple(), "empty scaling filter"),
        Corpus("single_layer", ("0.01", "100"), tuple(b(f"single:{i}") for i in range(8)), "single layer"),
        Corpus("multi_exp2", ("0.01", "10", "EXPANSION", "2"), tuple(b(f"item:{i}") for i in range(40)), "multi-layer expansion 2"),
        Corpus("fixed_full", ("0.01", "8", "NONSCALING"), tuple(b(f"fixed:{i}") for i in range(8)), "fixed filter at capacity"),
        Corpus("expansion1", ("0.01", "5", "EXPANSION", "1"), tuple(b(f"exp1:{i}") for i in range(20)), "many same-sized layers"),
        Corpus("expansion4", ("0.01", "5", "EXPANSION", "4"), tuple(b(f"exp4:{i}") for i in range(40)), "larger expansion"),
        Corpus(
            "binary_items",
            ("0.01", "50"),
            (
                b"",
                b"nul:\x00:inside",
                b"crlf:\r\n:inside",
                b"space and tab\titem",
                b"tcl:{list}\\chars",
                bytes(range(1, 32)),
            ),
            "raw RESP binary item coverage",
        ),
        Corpus("long_item", ("0.01", "50"), (b("long:") + b"x" * 65536,), "large item payload"),
    ]
    if include_large:
        corpora.append(
            Corpus(
                "large_empty_16mb",
                ("0.01", "15000000"),
                tuple(),
                "empty large filter intended to force RedisBloom 16MB SCANDUMP split",
            )
        )
    return corpora


def load_envs(tsv_path: Path, names: set[str] | None) -> list[CompatEnv]:
    envs: list[CompatEnv] = []
    with tsv_path.open() as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) != 6:
                raise ValueError(f"bad env row in {tsv_path}: {line!r}")
            name, redis_tag, redisbloom_tag, module_name, module_ver, _port = parts
            if names and name not in names:
                continue
            root = tsv_path.parent / name
            envs.append(
                CompatEnv(
                    name=name,
                    redis_tag=redis_tag,
                    redisbloom_tag=redisbloom_tag,
                    module_ver=module_ver,
                    redis_server=root / "bin" / "redis-server",
                    redisbloom_module=root / "lib" / "redisbloom.so",
                )
            )
    return envs


def module_info(client: RedisClient) -> Any:
    return safe_cmd(client, "MODULE", "LIST")


def reserve_and_populate(client: RedisClient, key: str, corpus: Corpus) -> dict[str, Any]:
    result: dict[str, Any] = {"reserve": safe_cmd(client, "BF.RESERVE", key, *corpus.reserve_args)}
    adds = []
    for item in corpus.items:
        adds.append(safe_cmd(client, "BF.ADD", key, item))
    result["adds"] = adds
    return result


def check_filter(client: RedisClient, key: str, corpus: Corpus, expected_card: Any | None = None) -> dict[str, Any]:
    card = safe_cmd(client, "BF.CARD", key)
    missing: list[str] = []
    for index, item in enumerate(corpus.items):
        exists = safe_cmd(client, "BF.EXISTS", key, item)
        if exists != 1:
            missing.append(f"{index}:{item[:16].hex()}")
    return {
        "card": card,
        "expected_card": card if expected_card is None else expected_card,
        "expected_items": len(corpus.items),
        "found": len(corpus.items) - len(missing),
        "missing_count": len(missing),
        "missing_samples": missing[:5],
    }


def check_passed(check: Any) -> bool:
    return (
        isinstance(check, dict)
        and check.get("card") == check.get("expected_card")
        and check.get("missing_count") == 0
    )


def contains_redis_error(value: Any) -> bool:
    if isinstance(value, dict):
        if "error" in value:
            return True
        return any(contains_redis_error(v) for v in value.values())
    if isinstance(value, list):
        return any(contains_redis_error(v) for v in value)
    return False


def dump_chunks(client: RedisClient, key: str, limit: int = 10000) -> list[tuple[int, bytes | None]]:
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


def chunk_summary(chunks: list[tuple[int, bytes | None]]) -> list[list[int]]:
    return [[cursor, 0 if data is None else len(data)] for cursor, data in chunks]


def load_chunks(client: RedisClient, key: str, chunks: list[tuple[int, bytes | None]]) -> list[Any]:
    replies = []
    for cursor, data in chunks:
        if cursor == 0:
            continue
        replies.append(safe_cmd(client, "BF.LOADCHUNK", key, cursor, data or b""))
    return replies


def with_pair(
    env: CompatEnv,
    gemini_module: Path,
    ports: PortAllocator,
    work_prefix: Path,
    fn,
) -> dict[str, Any]:
    gemini = Server("gemini", env.redis_server, gemini_module, ports.take(), work_prefix / "gemini")
    rb = Server("redisbloom", env.redis_server, env.redisbloom_module, ports.take(), work_prefix / "redisbloom")
    try:
        return fn(gemini, rb)
    except Exception as exc:
        return {"error": str(exc)}
    finally:
        gemini.stop()
        rb.stop()


def run_command_oracle(env: CompatEnv, gemini_module: Path, ports: PortAllocator, workdir: Path) -> dict[str, Any]:
    def body(gemini: Server, rb: Server) -> dict[str, Any]:
        g = gemini.start()
        r = rb.start()
        try:
            result: dict[str, Any] = {
                "gemini_module": module_info(g),
                "redisbloom_module": module_info(r),
            }
            for client, label in [(g, "gemini"), (r, "redisbloom")]:
                safe_cmd(client, "DEL", "info")
                safe_cmd(client, "BF.RESERVE", "info", "0.01", "100")
                safe_cmd(client, "BF.ADD", "info", "alpha")
                result[f"{label}_info_full"] = safe_cmd(client, "BF.INFO", "info")
                result[f"{label}_info_capacity"] = safe_cmd(client, "BF.INFO", "info", "CAPACITY")
                result[f"{label}_info_size"] = safe_cmd(client, "BF.INFO", "info", "SIZE")
                safe_cmd(client, "DEL", "fixed")
                result[f"{label}_fixed_reserve"] = safe_cmd(client, "BF.RESERVE", "fixed", "0.01", "2", "NONSCALING")
                result[f"{label}_madd_overflow"] = safe_cmd(client, "BF.MADD", "fixed", "a", "b", "c", "d")
                safe_cmd(client, "DEL", "fixed_insert")
                result[f"{label}_fixed_insert_reserve"] = safe_cmd(client, "BF.RESERVE", "fixed_insert", "0.01", "2", "NONSCALING")
                result[f"{label}_insert_overflow"] = safe_cmd(client, "BF.INSERT", "fixed_insert", "ITEMS", "a", "b", "c", "d")
            result["gemini_insert_nocreate_capacity"] = safe_cmd(g, "BF.INSERT", "nokey", "NOCREATE", "CAPACITY", "10", "ITEMS", "a")
            result["redisbloom_insert_nocreate_capacity"] = safe_cmd(r, "BF.INSERT", "nokey", "NOCREATE", "CAPACITY", "10", "ITEMS", "a")
            return result
        finally:
            g.close()
            r.close()

    return with_pair(env, gemini_module, ports, workdir / "command_oracle", body)


def run_scandump_case(
    env: CompatEnv,
    gemini_module: Path,
    corpus: Corpus,
    direction: str,
    ports: PortAllocator,
    workdir: Path,
) -> dict[str, Any]:
    def body(gemini: Server, rb: Server) -> dict[str, Any]:
        source_server, target_server = (rb, gemini) if direction == "rb_to_gemini" else (gemini, rb)
        source = source_server.start()
        target = target_server.start()
        key_src = f"src_{corpus.name}"
        key_dst = f"dst_{corpus.name}"
        try:
            populate = reserve_and_populate(source, key_src, corpus)
            source_check = check_filter(source, key_src, corpus)
            chunks = dump_chunks(source, key_src)
            replies = load_chunks(target, key_dst, chunks)
            return {
                "populate": populate,
                "source_check": source_check,
                "chunks": chunk_summary(chunks),
                "load_replies": replies,
                "check": check_filter(target, key_dst, corpus, expected_card=source_check.get("card")),
            }
        finally:
            source.close()
            target.close()

    return with_pair(env, gemini_module, ports, workdir / f"scandump_{direction}_{corpus.name}", body)


def copy_tree(src: Path, dst: Path) -> None:
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(src, dst)


def run_rdb_case(
    env: CompatEnv,
    gemini_module: Path,
    corpus: Corpus,
    direction: str,
    ports: PortAllocator,
    workdir: Path,
) -> dict[str, Any]:
    source_is_rb = direction == "rb_to_gemini"
    source_module = env.redisbloom_module if source_is_rb else gemini_module
    target_module = gemini_module if source_is_rb else env.redisbloom_module
    src = Server("rdb-source", env.redis_server, source_module, ports.take(), workdir / "src")
    dst = Server("rdb-target", env.redis_server, target_module, ports.take(), workdir / "dst")
    key = f"rdb_{corpus.name}"
    try:
        client = src.start()
        try:
            populate = reserve_and_populate(client, key, corpus)
            source_check = check_filter(client, key, corpus)
            save_reply = safe_cmd(client, "SAVE")
        finally:
            client.close()
            src.stop()
        copy_tree(src.workdir, dst.workdir)
        target = dst.start(preserve_dir=True)
        try:
            return {
                "populate": populate,
                "source_check": source_check,
                "save": save_reply,
                "check": check_filter(target, key, corpus, expected_card=source_check.get("card")),
            }
        finally:
            target.close()
            dst.stop()
    except Exception as exc:
        return {"error": str(exc), "target_log": dst.log_tail()}
    finally:
        src.stop()
        dst.stop()


def run_dump_restore_case(
    env: CompatEnv,
    gemini_module: Path,
    corpus: Corpus,
    direction: str,
    ports: PortAllocator,
    workdir: Path,
) -> dict[str, Any]:
    def body(gemini: Server, rb: Server) -> dict[str, Any]:
        source_server, target_server = (rb, gemini) if direction == "rb_to_gemini" else (gemini, rb)
        source = source_server.start()
        target = target_server.start()
        key_src = f"dump_src_{corpus.name}"
        key_dst = f"dump_dst_{corpus.name}"
        try:
            populate = reserve_and_populate(source, key_src, corpus)
            source_check = check_filter(source, key_src, corpus)
            dumped = safe_cmd(source, "DUMP", key_src)
            restore = (
                safe_cmd(target, "RESTORE", key_dst, "0", dumped, "REPLACE")
                if isinstance(dumped, bytes)
                else {"error": f"DUMP did not return bytes: {dumped!r}"}
            )
            return {
                "populate": populate,
                "source_check": source_check,
                "dump_len": len(dumped) if isinstance(dumped, bytes) else None,
                "restore": restore,
                "check": check_filter(target, key_dst, corpus, expected_card=source_check.get("card")),
            }
        finally:
            source.close()
            target.close()

    return with_pair(env, gemini_module, ports, workdir / f"dump_restore_{direction}_{corpus.name}", body)


def wait_for_aof_rewrite(client: RedisClient) -> None:
    client.cmd("BGREWRITEAOF")
    deadline = time.time() + 15
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


def run_aof_case(
    env: CompatEnv,
    gemini_module: Path,
    corpus: Corpus,
    direction: str,
    aof_preamble: bool,
    ports: PortAllocator,
    workdir: Path,
) -> dict[str, Any]:
    source_is_rb = direction == "rb_to_gemini"
    source_module = env.redisbloom_module if source_is_rb else gemini_module
    target_module = gemini_module if source_is_rb else env.redisbloom_module
    src = Server("aof-source", env.redis_server, source_module, ports.take(), workdir / "src")
    dst = Server("aof-target", env.redis_server, target_module, ports.take(), workdir / "dst")
    key = f"aof_{corpus.name}"
    try:
        client = src.start(appendonly=True, aof_preamble=aof_preamble)
        try:
            populate = reserve_and_populate(client, key, corpus)
            source_check = check_filter(client, key, corpus)
            wait_for_aof_rewrite(client)
        finally:
            client.close()
            src.stop()
        copy_tree(src.workdir, dst.workdir)
        target = dst.start(appendonly=True, preserve_dir=True, aof_preamble=aof_preamble)
        try:
            return {
                "populate": populate,
                "source_check": source_check,
                "check": check_filter(target, key, corpus, expected_card=source_check.get("card")),
                "critical_log_count": dst.critical_log_count(),
            }
        finally:
            target.close()
            dst.stop()
    except Exception as exc:
        return {"error": str(exc), "target_log": dst.log_tail(), "source_log": src.log_tail()}
    finally:
        src.stop()
        dst.stop()


def wait_replica_ready(replica: RedisClient, timeout: float = 15.0) -> dict[str, str]:
    deadline = time.time() + timeout
    last_fields: dict[str, str] = {}
    while time.time() < deadline:
        info = replica.cmd("INFO", "replication")
        text = info.decode() if isinstance(info, bytes) else str(info)
        fields = dict(
            line.split(":", 1)
            for line in text.splitlines()
            if ":" in line and not line.startswith("#")
        )
        last_fields = fields
        if (
            fields.get("role") == "slave"
            and fields.get("master_link_status") == "up"
            and fields.get("master_sync_in_progress") in (None, "0")
        ):
            return fields
        time.sleep(0.05)
    raise RuntimeError(f"replica did not become ready: {last_fields}")


def wait_check(
    client: RedisClient,
    key: str,
    corpus: Corpus,
    expected_card: Any | None,
    timeout: float = 5.0,
) -> dict[str, Any]:
    deadline = time.time() + timeout
    last = check_filter(client, key, corpus, expected_card=expected_card)
    while time.time() < deadline:
        if check_passed(last):
            return last
        time.sleep(0.05)
        last = check_filter(client, key, corpus, expected_card=expected_card)
    return last


def run_replication_case(
    env: CompatEnv,
    gemini_module: Path,
    corpus: Corpus,
    direction: str,
    mode: str,
    ports: PortAllocator,
    workdir: Path,
) -> dict[str, Any]:
    source_is_rb = direction == "rb_to_gemini"
    master_module = env.redisbloom_module if source_is_rb else gemini_module
    replica_module = gemini_module if source_is_rb else env.redisbloom_module
    master = Server("repl-master", env.redis_server, master_module, ports.take(), workdir / "master")
    replica = Server("repl-replica", env.redis_server, replica_module, ports.take(), workdir / "replica")
    key = f"repl_{mode}_{corpus.name}"
    try:
        master_client = master.start()
        replica_client = replica.start()
        try:
            populate: dict[str, Any] | None = None
            if mode == "fullsync":
                populate = reserve_and_populate(master_client, key, corpus)
            replicaof = safe_cmd(replica_client, "REPLICAOF", "127.0.0.1", str(master.port))
            repl_info = wait_replica_ready(replica_client)
            if mode == "live":
                populate = reserve_and_populate(master_client, key, corpus)
            source_check = check_filter(master_client, key, corpus)
            check = wait_check(replica_client, key, corpus, expected_card=source_check.get("card"))
            return {
                "populate": populate,
                "source_check": source_check,
                "replicaof": replicaof,
                "replication": {
                    "master_link_status": repl_info.get("master_link_status"),
                    "master_sync_in_progress": repl_info.get("master_sync_in_progress"),
                },
                "check": check,
                "critical_log_count": replica.critical_log_count(),
            }
        finally:
            master_client.close()
            replica_client.close()
            replica.stop()
            master.stop()
    except Exception as exc:
        return {"error": str(exc), "master_log": master.log_tail(), "replica_log": replica.log_tail()}
    finally:
        replica.stop()
        master.stop()


def run_env(
    env: CompatEnv,
    gemini_module: Path,
    corpora: list[Corpus],
    base_port: int,
) -> dict[str, Any]:
    ports = PortAllocator(base_port)
    workdir = BASE_DIR / env.name
    shutil.rmtree(workdir, ignore_errors=True)
    workdir.mkdir(parents=True)

    result: dict[str, Any] = {
        "env": {
            "name": env.name,
            "redis_tag": env.redis_tag,
            "redisbloom_tag": env.redisbloom_tag,
            "module_ver": env.module_ver,
            "redis_server": str(env.redis_server),
            "redisbloom_module": str(env.redisbloom_module),
        },
        "command_oracle": run_command_oracle(env, gemini_module, ports, workdir),
        "corpora": {},
    }
    for corpus in corpora:
        cell: dict[str, Any] = {"note": corpus.note, "expected_items": len(corpus.items)}
        for direction in ("rb_to_gemini", "gemini_to_rb"):
            cell[f"rdb_{direction}"] = run_rdb_case(env, gemini_module, corpus, direction, ports, workdir / f"rdb_{direction}_{corpus.name}")
            cell[f"dump_restore_{direction}"] = run_dump_restore_case(
                env,
                gemini_module,
                corpus,
                direction,
                ports,
                workdir,
            )
            cell[f"scandump_{direction}"] = run_scandump_case(env, gemini_module, corpus, direction, ports, workdir)
            cell[f"aof_command_{direction}"] = run_aof_case(
                env,
                gemini_module,
                corpus,
                direction,
                aof_preamble=False,
                ports=ports,
                workdir=workdir / f"aof_command_{direction}_{corpus.name}",
            )
            cell[f"aof_rdb_preamble_{direction}"] = run_aof_case(
                env,
                gemini_module,
                corpus,
                direction,
                aof_preamble=True,
                ports=ports,
                workdir=workdir / f"aof_rdb_{direction}_{corpus.name}",
            )
            cell[f"repl_live_{direction}"] = run_replication_case(
                env,
                gemini_module,
                corpus,
                direction,
                mode="live",
                ports=ports,
                workdir=workdir / f"repl_live_{direction}_{corpus.name}",
            )
            cell[f"repl_fullsync_{direction}"] = run_replication_case(
                env,
                gemini_module,
                corpus,
                direction,
                mode="fullsync",
                ports=ports,
                workdir=workdir / f"repl_fullsync_{direction}_{corpus.name}",
            )
        result["corpora"][corpus.name] = cell
        print(f"finished {env.name} {corpus.name}", flush=True)
    return result


def summarize_status(result: dict[str, Any]) -> dict[str, Any]:
    summary: dict[str, Any] = {}
    for env_name, env_result in result["envs"].items():
        counts = {"pass": 0, "fail": 0, "error": 0}
        for corpus_name, cell in env_result.get("corpora", {}).items():
            for key, value in cell.items():
                if not (
                    key.startswith("rdb_")
                    or key.startswith("dump_restore_")
                    or key.startswith("scandump_")
                    or key.startswith("aof_")
                    or key.startswith("repl_")
                ):
                    continue
                if not isinstance(value, dict) or "error" in value:
                    counts["error"] += 1
                    continue
                if contains_redis_error(value) or value.get("critical_log_count", 0) > 0:
                    counts["fail"] += 1
                    continue
                check = value.get("check", {})
                if check_passed(check):
                    counts["pass"] += 1
                else:
                    counts["fail"] += 1
        summary[env_name] = counts
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gemini-module", required=True, type=Path)
    parser.add_argument("--versions-tsv", default=DEFAULT_TSV, type=Path)
    parser.add_argument("--only-env", action="append", help="Limit to one environment name; may repeat")
    parser.add_argument("--redis-server", type=Path, help="Explicit redis-server path for a single custom environment")
    parser.add_argument("--redisbloom-module", type=Path, help="Explicit RedisBloom module path for a single custom environment")
    parser.add_argument("--env-name", default="redis-6.2-redisbloom-v2.4.20")
    parser.add_argument("--redis-tag", default="6.2.17")
    parser.add_argument("--redisbloom-tag", default="v2.4.20")
    parser.add_argument("--module-ver", default="20420")
    parser.add_argument("--base-port", default=47500, type=int)
    parser.add_argument("--include-large", action="store_true")
    parser.add_argument(
        "--output",
        default=Path("doc/code_review/gemini-bloom/v5/compat_matrix_results_redis62_redisbloom2420.json"),
        type=Path,
    )
    args = parser.parse_args()

    if not args.gemini_module.exists():
        raise SystemExit(f"gemini module not found: {args.gemini_module}")
    if args.redis_server or args.redisbloom_module:
        if not args.redis_server or not args.redisbloom_module:
            raise SystemExit("--redis-server and --redisbloom-module must be passed together")
        if not args.redis_server.exists():
            raise SystemExit(f"redis-server not found: {args.redis_server}")
        if not args.redisbloom_module.exists():
            raise SystemExit(f"RedisBloom module not found: {args.redisbloom_module}")
        envs = [
            CompatEnv(
                name=args.env_name,
                redis_tag=args.redis_tag,
                redisbloom_tag=args.redisbloom_tag,
                module_ver=args.module_ver,
                redis_server=args.redis_server,
                redisbloom_module=args.redisbloom_module,
            )
        ]
    else:
        names = set(args.only_env) if args.only_env else None
        envs = load_envs(args.versions_tsv, names)
    if not envs:
        raise SystemExit("no environments selected")
    corpora = build_corpora(args.include_large)

    result = {"envs": {}}
    for index, env in enumerate(envs):
        print(f"running {env.name} ({env.redis_tag}, {env.redisbloom_tag})", flush=True)
        result["envs"][env.name] = run_env(env, args.gemini_module, corpora, args.base_port + index * 1000)
    result["summary"] = summarize_status(result)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(json_safe(result), indent=2, sort_keys=True) + "\n")
    print(json.dumps(result["summary"], indent=2, sort_keys=True))
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
