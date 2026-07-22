#!/usr/bin/env python3
"""Stage 10 PERF_RESOURCE audit runner.

This script intentionally records bounded audit samples. It is not a formal
benchmark and must not be used to claim production performance numbers.
"""

from __future__ import annotations

import csv
import hashlib
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


REPO = Path(__file__).resolve().parents[5]
EVIDENCE = REPO / ".codex/gemini-bloom-audit/v6/evidence/stage10"
REDIS_DIR = Path("/workspace/projects/Environments/OpenSourceRedis/redis-6.2-redisbloom/bin")
REDIS_SERVER = REDIS_DIR / "redis-server"
BUILD_DIR = Path("/tmp/gemini-module-v6-stage10-build-workaround")
TMP_ROOT = Path("/tmp/gemini-bloom-v6-stage10")


class RedisError:
    def __init__(self, message: str) -> None:
        self.message = message

    def __repr__(self) -> str:
        return f"RedisError({self.message!r})"


class RespClient:
    def __init__(self, host: str, port: int, command_log: List[str]) -> None:
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.settimeout(30)
        self.buf = b""
        self.command_log = command_log

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

    def command(self, *args: Any, log: bool = True) -> Any:
        encoded = [self._encode_arg(a) for a in args]
        if log:
            self.command_log.append("REDIS " + " ".join(format_arg(a) for a in encoded))
        frame = b"*" + str(len(encoded)).encode() + b"\r\n"
        for arg in encoded:
            frame += b"$" + str(len(arg)).encode() + b"\r\n" + arg + b"\r\n"
        self.sock.sendall(frame)
        return self._parse()

    @staticmethod
    def _encode_arg(arg: Any) -> bytes:
        if isinstance(arg, bytes):
            return arg
        if isinstance(arg, bytearray):
            return bytes(arg)
        return str(arg).encode()

    def _read_exact(self, n: int) -> bytes:
        out = bytearray()
        while len(out) < n:
            chunk = self.sock.recv(n - len(out))
            if not chunk:
                raise ConnectionError("unexpected EOF")
            out.extend(chunk)
        return bytes(out)

    def _readline(self) -> bytes:
        line = bytearray()
        while True:
            ch = self._read_exact(1)
            line.extend(ch)
            if line.endswith(b"\r\n"):
                return bytes(line[:-2])

    def _parse(self) -> Any:
        prefix = self._read_exact(1)
        if prefix == b"+":
            return self._readline().decode("utf-8", "replace")
        if prefix == b"-":
            return RedisError(self._readline().decode("utf-8", "replace"))
        if prefix == b":":
            return int(self._readline())
        if prefix == b"$":
            length = int(self._readline())
            if length == -1:
                return None
            data = self._read_exact(length)
            crlf = self._read_exact(2)
            if crlf != b"\r\n":
                raise ValueError("bulk string missing CRLF")
            return data
        if prefix == b"*":
            count = int(self._readline())
            if count == -1:
                return None
            return [self._parse() for _ in range(count)]
        raise ValueError(f"unknown RESP prefix {prefix!r}")


def ensure_dirs() -> None:
    for path in [
        EVIDENCE,
        EVIDENCE / "module_build",
        EVIDENCE / "server_logs",
        EVIDENCE / "raw",
    ]:
        path.mkdir(parents=True, exist_ok=True)


def format_arg(arg: bytes) -> str:
    if len(arg) > 96:
        digest = hashlib.sha256(arg).hexdigest()[:16]
        return f"<bytes len={len(arg)} sha256={digest}>"
    try:
        text = arg.decode("utf-8")
        if all((32 <= ord(c) <= 126) for c in text):
            return text
    except UnicodeDecodeError:
        pass
    return repr(arg)


def jsonable(obj: Any) -> Any:
    if isinstance(obj, RedisError):
        return {"redis_error": obj.message}
    if isinstance(obj, bytes):
        return {"bytes_len": len(obj), "sha256": hashlib.sha256(obj).hexdigest()}
    if isinstance(obj, list):
        return [jsonable(x) for x in obj]
    if isinstance(obj, dict):
        return {str(k): jsonable(v) for k, v in obj.items()}
    return obj


def reply_class(reply: Any) -> str:
    if isinstance(reply, RedisError):
        return "error"
    if isinstance(reply, list) and any(isinstance(x, RedisError) for x in reply):
        return "array_with_error"
    if reply is None:
        return "null"
    return type(reply).__name__


def run_subprocess(args: List[str], stdout_path: Path, stderr_path: Path, command_log: List[str]) -> int:
    command_log.append("SUBPROCESS " + " ".join(args))
    with stdout_path.open("wb") as out, stderr_path.open("wb") as err:
        proc = subprocess.run(args, cwd=REPO, stdout=out, stderr=err)
    return proc.returncode


def find_module_artifact() -> Optional[Path]:
    matches = sorted(BUILD_DIR.rglob("redis_bloom.so"))
    return matches[0] if matches else None


def build_module(command_log: List[str], exit_codes: Dict[str, int]) -> Path:
    if BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    rc = run_subprocess(
        [
            "cmake",
            "-S",
            str(REPO),
            "-B",
            str(BUILD_DIR),
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DCMAKE_CXX_FLAGS=-include climits",
        ],
        EVIDENCE / "module_build/cmake_configure_stdout.log",
        EVIDENCE / "module_build/cmake_configure_stderr.log",
        command_log,
    )
    exit_codes["cmake_configure"] = rc
    if rc != 0:
        raise RuntimeError("cmake configure failed")

    rc = run_subprocess(
        ["cmake", "--build", str(BUILD_DIR), "-j2", "--target", "redis_bloom"],
        EVIDENCE / "module_build/cmake_build_stdout.log",
        EVIDENCE / "module_build/cmake_build_stderr.log",
        command_log,
    )
    exit_codes["cmake_build"] = rc
    if rc != 0:
        raise RuntimeError("cmake build failed")

    artifact = find_module_artifact()
    if not artifact:
        raise RuntimeError("redis_bloom.so not found after build")
    return artifact


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_for_redis(port: int, command_log: List[str], timeout: float = 10.0) -> RespClient:
    deadline = time.time() + timeout
    last_error: Optional[BaseException] = None
    while time.time() < deadline:
        try:
            client = RespClient("127.0.0.1", port, command_log)
            if client.command("PING") == "PONG":
                return client
            client.close()
        except BaseException as exc:  # noqa: BLE001 - audit script preserves startup errors
            last_error = exc
            time.sleep(0.1)
    raise RuntimeError(f"Redis did not start on port {port}: {last_error}")


def start_redis(port: int, redis_data_dir: Path, module: Path, command_log: List[str],
                suffix: str, extra_args: Optional[List[str]] = None) -> subprocess.Popen:
    redis_data_dir.mkdir(parents=True, exist_ok=True)
    args = [
        str(REDIS_SERVER),
        "--bind",
        "127.0.0.1",
        "--port",
        str(port),
        "--protected-mode",
        "no",
        "--daemonize",
        "no",
        "--loglevel",
        "notice",
        "--dir",
        str(redis_data_dir),
        "--dbfilename",
        "dump.rdb",
        "--loadmodule",
        str(module),
    ]
    if extra_args:
        args.extend(extra_args)
    command_log.append("SUBPROCESS " + " ".join(args))
    out = (EVIDENCE / f"server_logs/redis_{suffix}_stdout.log").open("wb")
    err = (EVIDENCE / f"server_logs/redis_{suffix}_stderr.log").open("wb")
    proc = subprocess.Popen(args, cwd=REPO, stdout=out, stderr=err)
    proc._stage10_files = (out, err)  # type: ignore[attr-defined]
    return proc


def stop_redis(proc: Optional[subprocess.Popen], client: Optional[RespClient]) -> None:
    if client:
        try:
            client.command("SHUTDOWN", "NOSAVE")
        except BaseException:
            pass
        try:
            client.close()
        except BaseException:
            pass
    if proc:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        for f in getattr(proc, "_stage10_files", ()):
            try:
                f.close()
            except BaseException:
                pass


def parse_info_blob(blob: Any) -> Dict[str, str]:
    if not isinstance(blob, bytes):
        return {}
    out: Dict[str, str] = {}
    for raw in blob.decode("utf-8", "replace").splitlines():
        if raw and not raw.startswith("#") and ":" in raw:
            k, v = raw.split(":", 1)
            out[k] = v
    return out


def bf_info_dict(client: RespClient, key: str) -> Dict[str, Any]:
    reply = client.command("BF.INFO", key)
    if not isinstance(reply, list):
        return {"error": jsonable(reply)}
    out: Dict[str, Any] = {}
    for idx in range(0, len(reply), 2):
        label = reply[idx]
        value = reply[idx + 1] if idx + 1 < len(reply) else None
        if isinstance(label, bytes):
            label_text = label.decode("utf-8", "replace")
        else:
            label_text = str(label)
        out[label_text] = jsonable(value)
    return out


def redis_memory(client: RespClient) -> Dict[str, Any]:
    return parse_info_blob(client.command("INFO", "memory"))


def redis_persistence(client: RespClient) -> Dict[str, Any]:
    return parse_info_blob(client.command("INFO", "persistence"))


def memory_usage(client: RespClient, key: str) -> Any:
    return client.command("MEMORY", "USAGE", key)


def redis_pid(client: RespClient) -> Optional[int]:
    info = parse_info_blob(client.command("INFO", "server"))
    try:
        return int(info.get("process_id", ""))
    except ValueError:
        return None


def os_rss_kb(pid: Optional[int]) -> Optional[int]:
    if not pid:
        return None
    proc = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        return None
    text = proc.stdout.strip()
    return int(text) if text.isdigit() else None


def is_error(reply: Any) -> bool:
    return isinstance(reply, RedisError)


def record_status(statuses: List[Dict[str, str]], scenario_id: str, area: str,
                  status: str, evidence: str, notes: str) -> None:
    statuses.append({
        "id": scenario_id,
        "area": area,
        "status": status,
        "evidence": evidence,
        "notes": notes,
    })


def write_resource_log(path: Path, rows: List[Dict[str, Any]]) -> None:
    with path.open("w", encoding="utf-8") as f:
        for row in rows:
            f.write(f"## {row['id']}\n")
            f.write(f"- command: {row['command']}\n")
            f.write(f"- elapsed_ms: {row['elapsed_ms']:.3f}\n")
            f.write(f"- status: {row['status']}\n")
            f.write(f"- reply_class: {row['reply_class']}\n")
            f.write(f"- reply: {json.dumps(jsonable(row['reply']), sort_keys=True)}\n")
            f.write(f"- used_memory_before: {row.get('used_memory_before')}\n")
            f.write(f"- used_memory_after: {row.get('used_memory_after')}\n")
            f.write(f"- used_memory_rss_before: {row.get('used_memory_rss_before')}\n")
            f.write(f"- used_memory_rss_after: {row.get('used_memory_rss_after')}\n")
            f.write(f"- notes: {row['notes']}\n\n")


def timed_command(client: RespClient, *args: Any) -> Tuple[Any, float]:
    start = time.perf_counter()
    reply = client.command(*args)
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    return reply, elapsed_ms


def run_resource_limits(client: RespClient, statuses: List[Dict[str, str]]) -> Tuple[List[Dict[str, Any]], Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    details: Dict[str, Any] = {}

    def do(sid: str, args: Iterable[Any], expect: str, notes: str) -> Any:
        before = redis_memory(client)
        start = time.perf_counter()
        reply = client.command(*args)
        elapsed = (time.perf_counter() - start) * 1000.0
        after = redis_memory(client)
        status = expect(reply)
        rows.append({
            "id": sid,
            "command": " ".join(format_arg(RespClient._encode_arg(a)) for a in args),
            "elapsed_ms": elapsed,
            "status": status,
            "reply_class": reply_class(reply),
            "reply": reply,
            "used_memory_before": before.get("used_memory"),
            "used_memory_after": after.get("used_memory"),
            "used_memory_rss_before": before.get("used_memory_rss"),
            "used_memory_rss_after": after.get("used_memory_rss"),
            "notes": notes,
        })
        return reply

    pass_if_error = lambda r: "PASS" if is_error(r) else "FAIL"
    pass_if_ok = lambda r: "PASS" if r == "OK" else "FAIL"
    pass_if_array = lambda r: "PASS" if isinstance(r, list) else "FAIL"

    do("RL-01", ["BF.RESERVE", "rl_cap_zero", "0.01", "0"], pass_if_error,
       "capacity 0 must reject before allocation")
    do("RL-02a", ["BF.RESERVE", "rl_cap_min", "0.01", "1"], pass_if_ok,
       "minimum capacity accepted")
    do("RL-02b", ["BF.ADD", "rl_cap_min", "one"], lambda r: "PASS" if r in (0, 1) else "FAIL",
       "minimum capacity add")
    do("RL-02c", ["BF.EXISTS", "rl_cap_min", "one"], lambda r: "PASS" if r == 1 else "FAIL",
       "minimum capacity exists")

    for cap in [10, 100, 10000, 100000]:
        key = f"rl_cap_{cap}"
        do(f"RL-03-{cap}", ["BF.RESERVE", key, "0.01", str(cap)], pass_if_ok,
           f"capacity {cap} accepted")
        details[key] = {
            "bf_info": bf_info_dict(client, key),
            "memory_usage": jsonable(memory_usage(client, key)),
        }

    do("RL-04", ["BF.RESERVE", "rl_cap_max_safe", "0.99", str(1 << 30), "NONSCALING"],
       pass_if_ok, "capacity 2^30 high-error NONSCALING safety probe")
    details["rl_cap_max_safe"] = {
        "bf_info": bf_info_dict(client, "rl_cap_max_safe"),
        "memory_usage": jsonable(memory_usage(client, "rl_cap_max_safe")),
    }
    do("RL-05", ["BF.RESERVE", "rl_cap_too_big", "0.01", str((1 << 30) + 1)],
       pass_if_error, "capacity above DESIGN max must reject before allocation")

    for exp in [1, 2, 4, 32768]:
        key = f"rl_exp_{exp}"
        do(f"RL-06-{exp}", ["BF.RESERVE", key, "0.01", "10", "EXPANSION", str(exp)],
           pass_if_ok, f"expansion {exp} accepted")
        details[key] = bf_info_dict(client, key)
    do("RL-07", ["BF.RESERVE", "rl_exp_too_big", "0.01", "10", "EXPANSION", "32769"],
       pass_if_error, "expansion above DESIGN max must reject")
    do("RL-08a", ["BF.RESERVE", "rl_exp_zero_reserve", "0.01", "10", "EXPANSION", "0"],
       pass_if_ok, "EXPANSION 0 maps to non-scaling in BF.RESERVE")
    details["rl_exp_zero_reserve"] = bf_info_dict(client, "rl_exp_zero_reserve")
    do("RL-08b", ["BF.INSERT", "rl_exp_zero_insert", "EXPANSION", "0", "ITEMS", "a"],
       pass_if_array, "EXPANSION 0 maps to non-scaling in BF.INSERT")
    details["rl_exp_zero_insert"] = bf_info_dict(client, "rl_exp_zero_insert")

    do("RL-09", ["BF.RESERVE", "rl_too_large_layer", "1e-7", str(1 << 30), "NONSCALING"],
       pass_if_error, "tiny error with max capacity should hit per-layer cap without allocation")
    do("RL-10", ["BF.RESERVE", "rl_bits_per_entry", "1e-300", "10"],
       pass_if_error, "bitsPerEntry > 1000 should reject gracefully")

    items = [f"item{i}" for i in range(12)]
    do("RL-11a", ["BF.RESERVE", "rl_nonscaling_full", "0.001", "10", "NONSCALING"],
       pass_if_ok, "prepare fixed-size filter")
    madd_reply = do("RL-11b", ["BF.MADD", "rl_nonscaling_full", *items],
                    lambda r: "PASS" if isinstance(r, list) and any(isinstance(x, RedisError) for x in r) else "FAIL",
                    "MADD should truncate at first full error")
    card = do("RL-11c", ["BF.CARD", "rl_nonscaling_full"],
              lambda r: "PASS" if r == 10 else "FAIL", "cardinality must stay at capacity")
    later = do("RL-11d", ["BF.EXISTS", "rl_nonscaling_full", "item11"],
               lambda r: "PASS" if r == 0 else "FAIL", "later item after full error should not be processed")
    insert_reply = do("RL-11e", ["BF.INSERT", "rl_insert_full", "CAPACITY", "10", "ERROR", "0.001",
                                 "NONSCALING", "ITEMS", *items],
                      lambda r: "PASS" if isinstance(r, list) and any(isinstance(x, RedisError) for x in r) else "FAIL",
                      "BF.INSERT fixed filter should truncate at first full error")
    details["rl_nonscaling_full"] = {
        "madd_reply": jsonable(madd_reply),
        "card": jsonable(card),
        "item11_exists": jsonable(later),
        "insert_reply": jsonable(insert_reply),
        "bf_info": bf_info_dict(client, "rl_nonscaling_full"),
    }

    for row in rows:
        record_status(statuses, row["id"], "resource_limits", row["status"],
                      "resource_limits.log", row["notes"])
    return rows, details


LATENCY_FIELDS = [
    "scenario_id",
    "command",
    "capacity",
    "expansion",
    "item_class",
    "item_size_bytes",
    "iteration",
    "elapsed_ms",
    "reply_class",
    "num_layers",
    "bf_info_size",
    "memory_usage",
    "used_memory",
    "used_memory_rss",
    "notes",
]


def append_latency(rows: List[Dict[str, Any]], client: RespClient, scenario_id: str,
                   args: List[Any], key: str, capacity: Any, expansion: Any,
                   item_class: str, item_size: int, iteration: int, notes: str) -> Any:
    reply, elapsed_ms = timed_command(client, *args)
    info = bf_info_dict(client, key) if key else {}
    mem = redis_memory(client)
    rows.append({
        "scenario_id": scenario_id,
        "command": " ".join(format_arg(RespClient._encode_arg(a)) for a in args[:4]),
        "capacity": capacity,
        "expansion": expansion,
        "item_class": item_class,
        "item_size_bytes": item_size,
        "iteration": iteration,
        "elapsed_ms": f"{elapsed_ms:.3f}",
        "reply_class": reply_class(reply),
        "num_layers": info.get("Number of filters"),
        "bf_info_size": info.get("Size"),
        "memory_usage": jsonable(memory_usage(client, key)) if key else "",
        "used_memory": mem.get("used_memory"),
        "used_memory_rss": mem.get("used_memory_rss"),
        "notes": notes,
    })
    return reply


def run_latency_samples(client: RespClient, statuses: List[Dict[str, str]]) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []

    client.command("BF.RESERVE", "lat_base", "0.01", "100", "EXPANSION", "2")
    for i in range(30):
        item = f"lat-base-{i}"
        append_latency(rows, client, "LAT-01", ["BF.ADD", "lat_base", item],
                       "lat_base", 100, 2, "small", len(item), i, "baseline ADD")
        append_latency(rows, client, "LAT-01", ["BF.EXISTS", "lat_base", item],
                       "lat_base", 100, 2, "small", len(item), i, "baseline EXISTS")
    for i in range(5):
        batch = [f"batch-{i}-{j}" for j in range(10)]
        append_latency(rows, client, "LAT-01", ["BF.MADD", "lat_base", *batch],
                       "lat_base", 100, 2, "small_batch", sum(len(x) for x in batch), i,
                       "baseline MADD batch of 10")
        append_latency(rows, client, "LAT-01", ["BF.MEXISTS", "lat_base", *batch],
                       "lat_base", 100, 2, "small_batch", sum(len(x) for x in batch), i,
                       "baseline MEXISTS batch of 10")
        append_latency(rows, client, "LAT-01", ["BF.INFO", "lat_base"],
                       "lat_base", 100, 2, "none", 0, i, "baseline INFO")
        append_latency(rows, client, "LAT-01", ["BF.CARD", "lat_base"],
                       "lat_base", 100, 2, "none", 0, i, "baseline CARD")

    for cap in [10, 100, 10000, 100000]:
        key = f"lat_cap_{cap}"
        client.command("BF.RESERVE", key, "0.01", str(cap), "EXPANSION", "2")
        for i in range(10):
            item = f"cap-{cap}-{i}"
            append_latency(rows, client, "LAT-02", ["BF.ADD", key, item],
                           key, cap, 2, "small", len(item), i, "capacity scale ADD")
            append_latency(rows, client, "LAT-02", ["BF.EXISTS", key, item],
                           key, cap, 2, "small", len(item), i, "capacity scale EXISTS")
        append_latency(rows, client, "LAT-02", ["BF.INFO", key],
                       key, cap, 2, "none", 0, 0, "capacity scale INFO")
        append_latency(rows, client, "LAT-02", ["BF.CARD", key],
                       key, cap, 2, "none", 0, 0, "capacity scale CARD")

    client.command("BF.RESERVE", "lat_exp1_many", "0.01", "10", "EXPANSION", "1")
    inserted = 0
    for i in range(90):
        reply = client.command("BF.ADD", "lat_exp1_many", f"many-{i}")
        if isinstance(reply, RedisError):
            break
        inserted += 1
    for label, item in [("oldest", "many-0"), ("newest", f"many-{inserted - 1}"), ("missing", "many-absent")]:
        for i in range(10):
            append_latency(rows, client, "LAT-03", ["BF.EXISTS", "lat_exp1_many", item],
                           "lat_exp1_many", 10, 1, label, len(item), i,
                           f"multi-layer query after {inserted} attempted inserts")

    for exp in [1, 2, 4, 32768]:
        key = f"lat_exp_cmp_{exp}"
        client.command("BF.RESERVE", key, "0.01", "10", "EXPANSION", str(exp))
        for i in range(20):
            append_latency(rows, client, "LAT-04", ["BF.ADD", key, f"exp-{exp}-{i}"],
                           key, 10, exp, "small", len(f"exp-{exp}-{i}"), i,
                           "expansion comparison")
        append_latency(rows, client, "LAT-04", ["BF.INFO", key],
                       key, 10, exp, "none", 0, 0, "expansion comparison info")

    client.command("BF.RESERVE", "lat_large_items", "0.01", "100")
    item_cases = [
        ("empty", b""),
        ("small", b"small"),
        ("binary_nul", b"bin\x00ary"),
        ("10kb", b"x" * 10_000),
        ("1mb", b"y" * 1_000_000),
    ]
    for idx, (label, payload) in enumerate(item_cases):
        iterations = 1 if label == "1mb" else (5 if label == "10kb" else 10)
        for i in range(iterations):
            actual = payload + (str(i).encode() if payload else str(i).encode())
            append_latency(rows, client, "LAT-05", ["BF.ADD", "lat_large_items", actual],
                           "lat_large_items", 100, 2, label, len(actual), i, "large item hashing ADD")
            append_latency(rows, client, "LAT-05", ["BF.EXISTS", "lat_large_items", actual],
                           "lat_large_items", 100, 2, label, len(actual), i, "large item hashing EXISTS")

    for row in rows:
        record_status(statuses, row["scenario_id"], "latency_samples", "PASS",
                      "latency_samples.csv", "bounded latency audit sample recorded")
    return rows


def write_latency_csv(rows: List[Dict[str, Any]]) -> None:
    with (EVIDENCE / "latency_samples.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=LATENCY_FIELDS)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def run_memory_usage(client: RespClient, statuses: List[Dict[str, str]]) -> Dict[str, Any]:
    pid = redis_pid(client)
    scenarios: Dict[str, Dict[str, Any]] = {}

    def snapshot(sid: str, key: str, notes: str) -> None:
        scenarios[sid] = {
            "key": key,
            "notes": notes,
            "bf_info": bf_info_dict(client, key),
            "memory_usage": jsonable(memory_usage(client, key)),
            "info_memory": {k: redis_memory(client).get(k) for k in [
                "used_memory", "used_memory_rss", "mem_fragmentation_ratio"
            ]},
            "os_rss_kb": os_rss_kb(pid),
        }
        record_status(statuses, sid, "memory_usage", "PASS", "memory_usage.md", notes)

    client.command("BF.RESERVE", "mem_empty100", "0.01", "100")
    snapshot("MEM-01", "mem_empty100", "empty reserved capacity 100 filter")
    for i in range(50):
        client.command("BF.ADD", "mem_empty100", f"mem-{i}")
    snapshot("MEM-02", "mem_empty100", "same filter after inserts below capacity")

    client.command("BF.RESERVE", "mem_exp2_growth", "0.01", "10", "EXPANSION", "2")
    for i in range(40):
        client.command("BF.ADD", "mem_exp2_growth", f"grow2-{i}")
    snapshot("MEM-03", "mem_exp2_growth", "multi-layer growth capacity 10 expansion 2")

    client.command("BF.RESERVE", "mem_exp1_many", "0.01", "10", "EXPANSION", "1")
    for i in range(80):
        reply = client.command("BF.ADD", "mem_exp1_many", f"grow1-{i}")
        if isinstance(reply, RedisError):
            break
    snapshot("MEM-04", "mem_exp1_many", "expansion 1 many-layer case")

    for cap in [10000, 100000]:
        key = f"mem_cap_{cap}"
        client.command("BF.RESERVE", key, "0.01", str(cap))
        for i in range(100):
            client.command("BF.ADD", key, f"memcap-{cap}-{i}")
        snapshot(f"MEM-05-{cap}", key, f"capacity {cap} memory accounting")

    snapshot("MEM-06", "rl_cap_max_safe", "capacity 2^30 high-error NONSCALING boundary")
    return scenarios


def write_memory_usage(scenarios: Dict[str, Any]) -> None:
    with (EVIDENCE / "memory_usage.md").open("w", encoding="utf-8") as f:
        f.write("# Stage 10 Memory Usage Evidence\n\n")
        f.write("These values are allocator- and environment-dependent audit samples.\n\n")
        for sid, data in scenarios.items():
            f.write(f"## {sid}: {data['key']}\n\n")
            f.write(f"- notes: {data['notes']}\n")
            f.write(f"- Redis MEMORY USAGE: `{data['memory_usage']}`\n")
            f.write(f"- OS RSS KB: `{data['os_rss_kb']}`\n")
            f.write("- INFO memory subset:\n\n")
            for k, v in data["info_memory"].items():
                f.write(f"  - `{k}`: `{v}`\n")
            f.write("\n- BF.INFO:\n\n")
            for k, v in data["bf_info"].items():
                f.write(f"  - `{k}`: `{v}`\n")
            f.write("\n")


def run_scandump(client: RespClient, statuses: List[Dict[str, str]],
                 latency_rows: List[Dict[str, Any]]) -> Dict[str, Any]:
    out: Dict[str, Any] = {}

    def make_filter(key: str, cap: int, exp: int, count: int) -> List[str]:
        client.command("BF.RESERVE", key, "0.01", str(cap), "EXPANSION", str(exp))
        inserted: List[str] = []
        for i in range(count):
            item = f"{key}-item-{i}"
            reply = client.command("BF.ADD", key, item)
            if isinstance(reply, RedisError):
                break
            inserted.append(item)
        return inserted

    scenarios = [
        ("SLC-01", "slc_single", 100, 2, 10),
        ("SLC-02", "slc_multi_exp2", 10, 2, 40),
        ("SLC-03", "slc_many_exp1", 10, 1, 80),
        ("SLC-04", "slc_large_layer", 100000, 2, 100),
    ]

    for sid, key, cap, exp, count in scenarios:
        inserted = make_filter(key, cap, exp, count)
        copy_key = f"{key}_copy"
        chunks: List[Dict[str, Any]] = []
        cursor = 0
        while True:
            start = time.perf_counter()
            reply = client.command("BF.SCANDUMP", key, str(cursor))
            elapsed_ms = (time.perf_counter() - start) * 1000.0
            if not isinstance(reply, list) or len(reply) != 2:
                chunks.append({"cursor_arg": cursor, "bad_reply": jsonable(reply)})
                break
            next_cursor, data = reply
            data_len = len(data) if isinstance(data, bytes) else None
            chunks.append({
                "cursor_arg": cursor,
                "next_cursor": next_cursor,
                "data_len": data_len,
                "elapsed_ms": round(elapsed_ms, 3),
            })
            latency_rows.append({
                "scenario_id": "LAT-06",
                "command": f"BF.SCANDUMP {key} {cursor}",
                "capacity": cap,
                "expansion": exp,
                "item_class": "dump_chunk",
                "item_size_bytes": data_len or 0,
                "iteration": len(chunks) - 1,
                "elapsed_ms": f"{elapsed_ms:.3f}",
                "reply_class": reply_class(reply),
                "num_layers": bf_info_dict(client, key).get("Number of filters"),
                "bf_info_size": bf_info_dict(client, key).get("Size"),
                "memory_usage": jsonable(memory_usage(client, key)),
                "used_memory": redis_memory(client).get("used_memory"),
                "used_memory_rss": redis_memory(client).get("used_memory_rss"),
                "notes": "SCANDUMP private full-layer cursor sample",
            })
            if next_cursor == 0:
                break
            start = time.perf_counter()
            load_reply = client.command("BF.LOADCHUNK", copy_key, str(next_cursor), data)
            load_elapsed = (time.perf_counter() - start) * 1000.0
            latency_rows.append({
                "scenario_id": "LAT-06",
                "command": f"BF.LOADCHUNK {copy_key} {next_cursor} <chunk>",
                "capacity": cap,
                "expansion": exp,
                "item_class": "load_chunk",
                "item_size_bytes": data_len or 0,
                "iteration": len(chunks) - 1,
                "elapsed_ms": f"{load_elapsed:.3f}",
                "reply_class": reply_class(load_reply),
                "num_layers": bf_info_dict(client, copy_key).get("Number of filters") if not is_error(load_reply) else "",
                "bf_info_size": bf_info_dict(client, copy_key).get("Size") if not is_error(load_reply) else "",
                "memory_usage": jsonable(memory_usage(client, copy_key)) if not is_error(load_reply) else "",
                "used_memory": redis_memory(client).get("used_memory"),
                "used_memory_rss": redis_memory(client).get("used_memory_rss"),
                "notes": "ordered same-module LOADCHUNK replay sample",
            })
            cursor = int(next_cursor)

        sample_checks = {item: jsonable(client.command("BF.EXISTS", copy_key, item)) for item in inserted[:5]}
        out[sid] = {
            "key": key,
            "copy_key": copy_key,
            "capacity": cap,
            "expansion": exp,
            "inserted_attempts": count,
            "inserted_recorded": len(inserted),
            "source_info": bf_info_dict(client, key),
            "copy_info": bf_info_dict(client, copy_key),
            "chunks": chunks,
            "copy_sample_exists": sample_checks,
        }
        data_chunks = [c for c in chunks if c.get("next_cursor") not in (0, 1)]
        expected = out[sid]["source_info"].get("Number of filters")
        status = "PASS" if isinstance(expected, int) and len(data_chunks) == expected else "FAIL"
        record_status(statuses, sid, "scandump_loadchunk", status, "scandump_loadchunk.md",
                      "private cursor sequence and ordered same-module replay")
    return out


def write_scandump(data: Dict[str, Any]) -> None:
    with (EVIDENCE / "scandump_loadchunk.md").open("w", encoding="utf-8") as f:
        f.write("# Stage 10 SCANDUMP/LOADCHUNK Evidence\n\n")
        f.write("Gemini uses the DESIGN private layer-index cursor protocol, not RedisBloom byte-offset chunks.\n\n")
        for sid, scenario in data.items():
            f.write(f"## {sid}: {scenario['key']}\n\n")
            f.write(f"- copy key: `{scenario['copy_key']}`\n")
            f.write(f"- capacity: `{scenario['capacity']}`\n")
            f.write(f"- expansion: `{scenario['expansion']}`\n")
            f.write(f"- inserted recorded: `{scenario['inserted_recorded']}`\n")
            f.write(f"- source filters: `{scenario['source_info'].get('Number of filters')}`\n")
            f.write(f"- copy filters: `{scenario['copy_info'].get('Number of filters')}`\n")
            f.write("- chunks:\n\n")
            for chunk in scenario["chunks"]:
                f.write(
                    f"  - cursor `{chunk.get('cursor_arg')}` -> `{chunk.get('next_cursor')}`, "
                    f"data_len `{chunk.get('data_len')}`, elapsed_ms `{chunk.get('elapsed_ms')}`\n"
                )
            f.write("\n- copy sample exists:\n\n")
            for item, reply in scenario["copy_sample_exists"].items():
                f.write(f"  - `{item}`: `{reply}`\n")
            f.write("\n")


def list_file_sizes(path: Path) -> Dict[str, int]:
    sizes: Dict[str, int] = {}
    if not path.exists():
        return sizes
    for child in sorted(path.rglob("*")):
        if child.is_file():
            sizes[str(child.relative_to(path))] = child.stat().st_size
    return sizes


def wait_persistence_idle(client: RespClient, timeout: float = 30.0) -> Dict[str, str]:
    deadline = time.time() + timeout
    info: Dict[str, str] = {}
    while time.time() < deadline:
        info = redis_persistence(client)
        if info.get("rdb_bgsave_in_progress") == "0" and info.get("aof_rewrite_in_progress") == "0":
            return info
        time.sleep(0.2)
    return info


def run_persistence(client: RespClient, statuses: List[Dict[str, str]], redis_data_dir: Path,
                    module: Path, port: int, command_log: List[str],
                    proc_holder: Dict[str, Optional[subprocess.Popen]]) -> Tuple[Dict[str, Any], RespClient, subprocess.Popen]:
    for cap in [100, 10000]:
        key = f"pa_cap_{cap}"
        client.command("BF.RESERVE", key, "0.01", str(cap))
        for i in range(min(cap, 100)):
            client.command("BF.ADD", key, f"pa-{cap}-{i}")
    client.command("BF.RESERVE", "pa_ml_exp1", "0.01", "10", "EXPANSION", "1")
    for i in range(80):
        reply = client.command("BF.ADD", "pa_ml_exp1", f"pa-exp1-{i}")
        if isinstance(reply, RedisError):
            break
    client.command("BF.RESERVE", "pa_large_items", "0.01", "100")
    client.command("BF.ADD", "pa_large_items", b"x" * 10_000)
    client.command("BF.ADD", "pa_large_items", b"y" * 1_000_000)

    save_reply = client.command("SAVE")
    rdb_size = (redis_data_dir / "dump.rdb").stat().st_size if (redis_data_dir / "dump.rdb").exists() else None
    record_status(statuses, "PA-01", "persistence_size", "PASS" if save_reply == "OK" and rdb_size else "FAIL",
                  "persistence_size.md", "RDB size after moderate filters and large item hashing")

    cfg1 = client.command("CONFIG", "SET", "aof-use-rdb-preamble", "yes")
    cfg2 = client.command("CONFIG", "SET", "appendonly", "yes")
    wait_persistence_idle(client)
    bgrewrite = client.command("BGREWRITEAOF")
    wait_info = wait_persistence_idle(client)
    aof_sizes = list_file_sizes(redis_data_dir)
    aof_related = {k: v for k, v in aof_sizes.items() if "aof" in k.lower() or "appendonly" in k.lower()}
    record_status(statuses, "PA-02", "persistence_size", "PASS" if aof_related else "FAIL",
                  "persistence_size.md", "AOF RDB-preamble size recorded")

    stop_redis(proc_holder.get("proc"), client)
    proc_holder["proc"] = None
    restart_proc = start_redis(port, redis_data_dir, module, command_log, "restart_aof",
                               ["--appendonly", "yes", "--aof-use-rdb-preamble", "yes"])
    restart_client = wait_for_redis(port, command_log)
    proc_holder["proc"] = restart_proc
    restart_checks = {
        "pa_cap_100_card": jsonable(restart_client.command("BF.CARD", "pa_cap_100")),
        "pa_large_10kb_exists": jsonable(restart_client.command("BF.EXISTS", "pa_large_items", b"x" * 10_000)),
        "pa_large_1mb_exists": jsonable(restart_client.command("BF.EXISTS", "pa_large_items", b"y" * 1_000_000)),
    }
    restart_ok = restart_checks["pa_cap_100_card"] == 100 and restart_checks["pa_large_10kb_exists"] == 1
    record_status(statuses, "PA-02-RESTART", "persistence_size", "PASS" if restart_ok else "FAIL",
                  "persistence_size.md", "same-module AOF RDB-preamble restart sanity check")
    record_status(statuses, "PA-03", "persistence_size", "NOT_VERIFIED",
                  "blocked_or_not_verified.md", "command-AOF aof-use-rdb-preamble no not rerun in Stage 10")
    record_status(statuses, "PA-04", "persistence_size", "PASS" if rdb_size and rdb_size < 1_500_000 else "FAIL",
                  "persistence_size.md", "RDB/AOF preamble stores bit arrays, not raw 1MB item payloads")

    data = {
        "save_reply": jsonable(save_reply),
        "rdb_size": rdb_size,
        "config_set_aof_preamble": jsonable(cfg1),
        "config_set_appendonly": jsonable(cfg2),
        "bgrewriteaof_reply": jsonable(bgrewrite),
        "wait_persistence_info": wait_info,
        "file_sizes": aof_sizes,
        "aof_related_sizes": aof_related,
        "restart_checks": restart_checks,
    }
    return data, restart_client, restart_proc


def write_persistence(data: Dict[str, Any], module: Path) -> None:
    with (EVIDENCE / "persistence_size.md").open("w", encoding="utf-8") as f:
        f.write("# Stage 10 Persistence Size Evidence\n\n")
        f.write(f"- module artifact: `{module}`\n")
        f.write(f"- SAVE reply: `{data['save_reply']}`\n")
        f.write(f"- RDB dump.rdb size: `{data['rdb_size']}` bytes\n")
        f.write(f"- CONFIG SET aof-use-rdb-preamble yes: `{data['config_set_aof_preamble']}`\n")
        f.write(f"- CONFIG SET appendonly yes: `{data['config_set_appendonly']}`\n")
        f.write(f"- BGREWRITEAOF reply: `{data['bgrewriteaof_reply']}`\n")
        f.write("\n## Persistence Info After Rewrite\n\n")
        for k, v in data["wait_persistence_info"].items():
            if k.startswith("aof_") or k.startswith("rdb_"):
                f.write(f"- `{k}`: `{v}`\n")
        f.write("\n## File Sizes\n\n")
        for k, v in data["file_sizes"].items():
            f.write(f"- `{k}`: `{v}` bytes\n")
        f.write("\n## Restart Checks\n\n")
        for k, v in data["restart_checks"].items():
            f.write(f"- `{k}`: `{v}`\n")
        f.write("\nCommand-AOF with `aof-use-rdb-preamble no` was not rerun in Stage 10; Stage 06 covers the same-module/design boundary.\n")


def write_static_audit() -> None:
    notes = """# Stage 10 Static Resource Audit

- `bloom_commands.cc` enforces `capacity` in `1..2^30`, `error_rate` in `(0,1)`, `EXPANSION` in `0..32768`, and registers write commands with `deny-oom`.
- `BloomLayer::Create()` rejects non-finite/invalid error rates, `bitsPerEntry > 1000`, oversized computed bit counts, zero data size, and runtime per-layer data size above 2GB before allocation.
- `ScalingBloomFilter::AppendLayer()` rejects total runtime bit-array data above 4GB.
- `BF.INFO Size` is backed by `ScalingBloomFilter::BytesUsed()` and module `mem_usage`; it accounts for the C++ object, reserved layer slots, and bit arrays, so it is not expected to equal Redis object overhead exactly.
- `BF.SCANDUMP` emits cursor `1` for the header and then one full bit-array chunk per layer; this is a DESIGN private protocol, not RedisBloom's byte-offset chunking.
- `BF.LOADCHUNK` orderly replay copies full layer bit arrays, but Stage 07 findings remain open: it does not enforce strict cursor order or reject half-loaded persisted states.
- RDB/wire deserialization validates total data size and layer count, but Stage 03 findings remain open for missing per-layer 2GB deserialization cap and expansion values above `kMaxExpansion`.
"""
    (EVIDENCE / "static_resource_audit.md").write_text(notes, encoding="utf-8")


def write_perf_matrix(statuses: List[Dict[str, str]]) -> None:
    with (EVIDENCE / "perf_matrix.md").open("w", encoding="utf-8") as f:
        f.write("# Stage 10 PERF_RESOURCE Matrix\n\n")
        f.write("Bounded audit samples only; these are not formal benchmark results.\n\n")
        f.write("| ID | Area | Status | Evidence | Notes |\n")
        f.write("|---|---|---|---|---|\n")
        seen = set()
        for row in statuses:
            key = (row["id"], row["area"], row["evidence"])
            if key in seen:
                continue
            seen.add(key)
            notes = row["notes"].replace("|", "/")
            f.write(f"| {row['id']} | {row['area']} | {row['status']} | `{row['evidence']}` | {notes} |\n")


def write_blocked_or_not_verified(statuses: List[Dict[str, str]]) -> None:
    rows = [r for r in statuses if r["status"] in ("BLOCKED", "NOT_VERIFIED")]
    with (EVIDENCE / "blocked_or_not_verified.md").open("w", encoding="utf-8") as f:
        f.write("# Stage 10 BLOCKED / NOT_VERIFIED\n\n")
        if not rows:
            f.write("No Stage 10 scenario was BLOCKED. The intentionally skipped scenarios below narrow claims rather than fail the stage.\n\n")
        for row in rows:
            f.write(f"- `{row['id']}` `{row['status']}`: {row['notes']} (evidence: `{row['evidence']}`)\n")
        f.write("- `RL-04-default-error` `NOT_VERIFIED`: `capacity=2^30` with default/low error rate was intentionally not allocated because it can require GB-scale memory; Stage 10 used the high-error safety probe instead.\n")
        f.write("- `SLC-redis-bloom-interop` `DESIGN_INTENDED`: RedisBloom SCANDUMP/LOADCHUNK byte-offset compatibility was not tested as a PASS criterion because DESIGN defines gemini's private layer cursor protocol.\n")


def write_env_snapshot(module: Optional[Path], redis_data_dir: Optional[Path]) -> None:
    def capture(args: List[str]) -> str:
        proc = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        return proc.stdout.strip()

    lines = [
        f"repo={REPO}",
        f"git_branch={capture(['git', 'rev-parse', '--abbrev-ref', 'HEAD'])}",
        f"git_head={capture(['git', 'rev-parse', 'HEAD'])}",
        f"python={sys.version.split()[0]}",
        f"redis_server={REDIS_SERVER}",
        f"redis_version={capture([str(REDIS_SERVER), '--version'])}",
        f"cmake={capture(['cmake', '--version']).splitlines()[0]}",
        f"gxx={capture(['g++', '--version']).splitlines()[0]}",
        f"module_artifact={module if module else 'NONE'}",
        f"module_size={module.stat().st_size if module and module.exists() else 'NONE'}",
        f"redis_data_dir={redis_data_dir if redis_data_dir else 'NONE'}",
        "meminfo:",
        Path("/proc/meminfo").read_text(encoding="utf-8", errors="replace")
        if Path("/proc/meminfo").exists() else "NO_PROC_MEMINFO",
    ]
    (EVIDENCE / "env_snapshot.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_evidence_index() -> None:
    files = [
        "env_snapshot.txt",
        "commands.txt",
        "stdout.log",
        "stderr.log",
        "exit_codes.txt",
        "perf_matrix.md",
        "resource_limits.log",
        "memory_usage.md",
        "latency_samples.csv",
        "scandump_loadchunk.md",
        "persistence_size.md",
        "static_resource_audit.md",
        "blocked_or_not_verified.md",
        "stage10_results.json",
        "module_build/cmake_configure_stdout.log",
        "module_build/cmake_configure_stderr.log",
        "module_build/cmake_build_stdout.log",
        "module_build/cmake_build_stderr.log",
        "server_logs/redis_initial_stdout.log",
        "server_logs/redis_initial_stderr.log",
        "server_logs/redis_restart_aof_stdout.log",
        "server_logs/redis_restart_aof_stderr.log",
    ]
    with (EVIDENCE / "evidence_index.md").open("w", encoding="utf-8") as f:
        f.write("# Stage 10 Evidence Index\n\n")
        for name in files:
            path = EVIDENCE / name
            status = "present" if path.exists() and path.stat().st_size > 0 else "missing_or_empty"
            f.write(f"- `{name}`: {status}\n")


def normalize_empty_auxiliary_logs() -> None:
    placeholders = {
        "module_build/cmake_configure_stdout.log": "NO_STDOUT\n",
        "module_build/cmake_configure_stderr.log": "NO_STDERR\n",
        "module_build/cmake_build_stdout.log": "NO_STDOUT\n",
        "module_build/cmake_build_stderr.log": "NO_STDERR\n",
        "server_logs/redis_initial_stdout.log": "NO_STDOUT\n",
        "server_logs/redis_initial_stderr.log": "NO_STDERR\n",
        "server_logs/redis_restart_aof_stdout.log": "NO_STDOUT\n",
        "server_logs/redis_restart_aof_stderr.log": "NO_STDERR\n",
    }
    for rel, text in placeholders.items():
        path = EVIDENCE / rel
        if path.exists() and path.stat().st_size == 0:
            path.write_text(text, encoding="utf-8")


def main() -> int:
    ensure_dirs()
    command_log: List[str] = []
    exit_codes: Dict[str, int] = {}
    stdout_lines: List[str] = []
    stderr_lines: List[str] = []
    statuses: List[Dict[str, str]] = []
    module: Optional[Path] = None
    redis_data_dir: Optional[Path] = None
    proc_holder: Dict[str, Optional[subprocess.Popen]] = {"proc": None}
    client: Optional[RespClient] = None

    try:
        write_static_audit()
        module = build_module(command_log, exit_codes)
        redis_data_dir = Path(tempfile.mkdtemp(prefix="redis-", dir=str(TMP_ROOT.parent)))
        port = free_port()
        proc = start_redis(port, redis_data_dir, module, command_log, "initial")
        proc_holder["proc"] = proc
        client = wait_for_redis(port, command_log)
        client.command("FLUSHALL")

        resource_rows, resource_details = run_resource_limits(client, statuses)
        write_resource_log(EVIDENCE / "resource_limits.log", resource_rows)

        latency_rows = run_latency_samples(client, statuses)
        memory_data = run_memory_usage(client, statuses)
        write_memory_usage(memory_data)
        scandump_data = run_scandump(client, statuses, latency_rows)
        write_scandump(scandump_data)
        write_latency_csv(latency_rows)

        persistence_data, client, restart_proc = run_persistence(
            client, statuses, redis_data_dir, module, port, command_log, proc_holder
        )
        proc_holder["proc"] = restart_proc
        write_persistence(persistence_data, module)

        write_perf_matrix(statuses)
        write_blocked_or_not_verified(statuses)
        write_env_snapshot(module, redis_data_dir)

        results = {
            "module": str(module),
            "redis_data_dir": str(redis_data_dir),
            "statuses": statuses,
            "resource_details": resource_details,
            "memory_scenarios": memory_data,
            "scandump": scandump_data,
            "persistence": persistence_data,
        }
        (EVIDENCE / "stage10_results.json").write_text(
            json.dumps(jsonable(results), indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        stdout_lines.append("Stage 10 PERF_RESOURCE audit runner completed.")
        stdout_lines.append(f"module={module}")
        stdout_lines.append(f"redis_data_dir={redis_data_dir}")
        stdout_lines.append(f"statuses={len(statuses)}")
        exit_codes["stage10_runner"] = 0
        return 0
    except BaseException as exc:  # noqa: BLE001 - preserve all audit failure details
        stderr_lines.append(f"{type(exc).__name__}: {exc}")
        exit_codes["stage10_runner"] = 1
        return 1
    finally:
        stop_redis(proc_holder.get("proc"), client)
        (EVIDENCE / "commands.txt").write_text("\n".join(command_log) + "\n", encoding="utf-8")
        (EVIDENCE / "stdout.log").write_text("\n".join(stdout_lines or ["NO_STDOUT"]) + "\n", encoding="utf-8")
        (EVIDENCE / "stderr.log").write_text("\n".join(stderr_lines or ["NO_STDERR"]) + "\n", encoding="utf-8")
        (EVIDENCE / "exit_codes.txt").write_text(
            "\n".join(f"{k}={v}" for k, v in sorted(exit_codes.items())) + "\n",
            encoding="utf-8",
        )
        if module or redis_data_dir:
            write_env_snapshot(module, redis_data_dir)
        normalize_empty_auxiliary_logs()
        write_evidence_index()


if __name__ == "__main__":
    raise SystemExit(main())
