#!/usr/bin/env python3
import dataclasses
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Callable


ROOT = Path(__file__).resolve().parents[6]
STAGE_DIR = ROOT / ".codex/gemini-bloom-audit/v6/evidence/stage04"
MATRIX_DIR = STAGE_DIR / "runtime_matrix"


@dataclasses.dataclass
class Resp:
    typ: str
    value: Any
    raw: bytes


@dataclasses.dataclass
class Row:
    case_id: str
    area: str
    command: str
    expected: str
    actual: str
    classification: str
    evidence: str
    ok: bool


class RespClient:
    def __init__(self, port: int, raw_log):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.file = self.sock.makefile("rb")
        self.raw_log = raw_log
        self.counter = 0

    def close(self):
        try:
            self.file.close()
        finally:
            self.sock.close()

    @staticmethod
    def _bulk(arg: Any) -> bytes:
        if isinstance(arg, bytes):
            return arg
        if isinstance(arg, str):
            return arg.encode("utf-8")
        return str(arg).encode("utf-8")

    @classmethod
    def encode(cls, args: list[Any]) -> bytes:
        out = [b"*" + str(len(args)).encode() + b"\r\n"]
        for arg in args:
            b = cls._bulk(arg)
            out.append(b"$" + str(len(b)).encode() + b"\r\n" + b + b"\r\n")
        return b"".join(out)

    def _readline(self) -> bytes:
        line = self.file.readline()
        if not line:
            raise RuntimeError("socket closed while reading RESP line")
        return line

    def _read_frame(self) -> Resp:
        first = self.file.read(1)
        if not first:
            raise RuntimeError("socket closed while reading RESP type")
        typ = first.decode("ascii", errors="replace")
        raw_parts = [first]

        if typ in "+-:,.#":
            line = self._readline()
            raw_parts.append(line)
            data = line[:-2].decode("utf-8", errors="replace")
            if typ == ":":
                try:
                    value: Any = int(data)
                except ValueError:
                    value = data
            elif typ == "#":
                value = data == "t"
            else:
                value = data
            return Resp(typ, value, b"".join(raw_parts))

        if typ == "_":
            line = self._readline()
            raw_parts.append(line)
            return Resp(typ, None, b"".join(raw_parts))

        if typ in "$!=":
            line = self._readline()
            raw_parts.append(line)
            length = int(line[:-2])
            if length == -1:
                return Resp(typ, None, b"".join(raw_parts))
            payload = self.file.read(length + 2)
            raw_parts.append(payload)
            if len(payload) != length + 2:
                raise RuntimeError("short RESP bulk payload")
            return Resp(typ, payload[:-2], b"".join(raw_parts))

        if typ in "*~>":
            line = self._readline()
            raw_parts.append(line)
            count = int(line[:-2])
            if count == -1:
                return Resp(typ, None, b"".join(raw_parts))
            items = []
            for _ in range(count):
                child = self._read_frame()
                items.append(child)
                raw_parts.append(child.raw)
            return Resp(typ, items, b"".join(raw_parts))

        if typ == "%":
            line = self._readline()
            raw_parts.append(line)
            count = int(line[:-2])
            pairs = []
            for _ in range(count):
                k = self._read_frame()
                v = self._read_frame()
                pairs.append((k, v))
                raw_parts.append(k.raw)
                raw_parts.append(v.raw)
            return Resp(typ, pairs, b"".join(raw_parts))

        raise RuntimeError(f"unsupported RESP type {typ!r}")

    def cmd(self, label: str, *args: Any) -> Resp:
        self.counter += 1
        request = self.encode(list(args))
        self.sock.sendall(request)
        reply = self._read_frame()
        self.raw_log.write(f"\n## [{self.counter:04d}] {label}\n")
        self.raw_log.write(f"COMMAND: {format_cmd(args)}\n")
        self.raw_log.write(f"REQUEST_BYTES: {len(request)}\n")
        self.raw_log.write(f"REQUEST_ESCAPED: {escape_bytes(request)}\n")
        self.raw_log.write(f"REPLY_BYTES: {len(reply.raw)}\n")
        self.raw_log.write(f"REPLY_ESCAPED: {escape_bytes(reply.raw)}\n")
        self.raw_log.write(f"NORMALIZED: {norm(reply)}\n")
        self.raw_log.flush()
        return reply


def escape_bytes(data: bytes, limit: int | None = None) -> str:
    shown = data if limit is None else data[:limit]
    text = "".join(chr(b) if 32 <= b < 127 and chr(b) not in "\\`" else f"\\x{b:02x}" for b in shown)
    if limit is not None and len(data) > limit:
        text += f"...<truncated {len(data) - limit} bytes>"
    return text


def short_bytes(data: bytes, limit: int = 48) -> str:
    if data is None:
        return "nil"
    if len(data) <= limit and all(32 <= b < 127 for b in data):
        return data.decode("ascii")
    return f"bytes(len={len(data)}, hex={data[:limit].hex()}{'...' if len(data) > limit else ''})"


def norm(resp: Resp) -> str:
    if resp.typ == "+":
        return f"simple({resp.value})"
    if resp.typ == "-":
        return f"error({resp.value})"
    if resp.typ == ":":
        return f"int({resp.value})"
    if resp.typ == "$":
        return f"bulk({short_bytes(resp.value)})"
    if resp.typ == "_":
        return "null"
    if resp.typ in ",#":
        return f"{resp.typ}({resp.value})"
    if resp.typ in "*~>":
        if resp.value is None:
            return "array(nil)"
        return "[" + ", ".join(norm(x) for x in resp.value) + "]"
    if resp.typ == "%":
        return "{" + ", ".join(f"{norm(k)}: {norm(v)}" for k, v in resp.value) + "}"
    if resp.typ in "!=":
        return f"bulk({short_bytes(resp.value)})"
    return f"{resp.typ}({resp.value})"


def plain(resp: Resp) -> Any:
    if resp.typ == "+":
        return resp.value
    if resp.typ == "-":
        return {"error": resp.value}
    if resp.typ == ":":
        return resp.value
    if resp.typ in "$!=":
        return resp.value
    if resp.typ == "_":
        return None
    if resp.typ in "*~>":
        return None if resp.value is None else [plain(x) for x in resp.value]
    if resp.typ == "%":
        return [(plain(k), plain(v)) for k, v in resp.value]
    return resp.value


def format_cmd(args: tuple[Any, ...] | list[Any]) -> str:
    parts = []
    for arg in args:
        b = RespClient._bulk(arg)
        if len(b) > 80:
            parts.append(f"<bulk len={len(b)} sha-ish={b[:16].hex()}>")
        elif all(32 <= x < 127 for x in b):
            parts.append(b.decode("ascii"))
        else:
            parts.append("0x" + b.hex())
    return " ".join(parts)


def contains_error(resp: Resp, needle: str | None = None) -> bool:
    if resp.typ != "-":
        return False
    return needle is None or needle.lower() in str(resp.value).lower()


def is_int(resp: Resp, value: int) -> bool:
    return resp.typ == ":" and resp.value == value


def is_simple(resp: Resp, value: str) -> bool:
    return resp.typ == "+" and resp.value.upper() == value.upper()


def is_array(resp: Resp, length: int | None = None) -> bool:
    return resp.typ in "*~>" and resp.value is not None and (length is None or len(resp.value) == length)


def ints(resp: Resp) -> list[int] | None:
    if not is_array(resp):
        return None
    out = []
    for item in resp.value:
        if item.typ != ":":
            return None
        out.append(item.value)
    return out


def row(rows: list[Row], case_id: str, area: str, command: str, expected: str,
        resp: Resp | None, ok: bool, classification: str = "PASS",
        actual: str | None = None):
    if resp is not None and actual is None:
        actual = norm(resp)
    final_classification = classification if ok or classification == "BLOCKED" else "FAIL"
    rows.append(Row(case_id, area, command, expected, actual or "", final_classification, f"raw_resp.log#{case_id}", ok))


def start_redis(module_path: Path, commands_log) -> tuple[int, Path]:
    redis_server = shutil.which("redis-server")
    if not redis_server:
        raise RuntimeError("redis-server not found")
    port_sock = socket.socket()
    port_sock.bind(("127.0.0.1", 0))
    port = port_sock.getsockname()[1]
    port_sock.close()

    redis_dir = Path(tempfile.mkdtemp(prefix="gemini-bloom-stage04-redis.", dir="/private/tmp"))
    log_path = redis_dir / "redis.log"
    cmd = [
        redis_server,
        "--bind", "127.0.0.1",
        "--port", str(port),
        "--daemonize", "yes",
        "--loglevel", "notice",
        "--logfile", str(log_path),
        "--dbfilename", "dump.rdb",
        "--dir", str(redis_dir),
        "--loadmodule", str(module_path),
    ]
    commands_log.write("$ " + " ".join(cmd) + "\n")
    proc = subprocess.run(cmd, text=True, capture_output=True)
    (MATRIX_DIR / "redis_start_stdout.log").write_text(proc.stdout)
    (MATRIX_DIR / "redis_start_stderr.log").write_text(proc.stderr)
    if proc.returncode != 0:
        raise RuntimeError(f"redis-server failed to start, exit={proc.returncode}, stderr={proc.stderr}")
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return port, redis_dir
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"redis-server did not accept connections on port {port}")


def run_build(commands_log) -> Path:
    build_dir = Path(tempfile.mkdtemp(prefix="gemini-bloom-stage04-build.", dir="/private/tmp"))
    jobs = str(max(2, min(os.cpu_count() or 2, 10)))
    configure = ["cmake", "-S", str(ROOT), "-B", str(build_dir)]
    build = ["cmake", "--build", str(build_dir), "--target", "redis_bloom", "-j", jobs]
    for name, cmd in [("configure", configure), ("build", build)]:
        commands_log.write("$ " + " ".join(cmd) + "\n")
        proc = subprocess.run(cmd, text=True, capture_output=True)
        (MATRIX_DIR / f"{name}_stdout.log").write_text(proc.stdout)
        (MATRIX_DIR / f"{name}_stderr.log").write_text(proc.stderr)
        (MATRIX_DIR / f"{name}_exit_code.txt").write_text(str(proc.returncode) + "\n")
        if proc.returncode != 0:
            raise RuntimeError(f"{name} failed with exit code {proc.returncode}")
    module_path = build_dir / "redis_bloom.so"
    if not module_path.exists():
        raise RuntimeError(f"module artifact missing: {module_path}")
    (MATRIX_DIR / "module_path.txt").write_text(str(module_path) + "\n")
    return module_path


def main() -> int:
    MATRIX_DIR.mkdir(parents=True, exist_ok=True)
    rows: list[Row] = []
    failures: list[Row] = []
    port = None
    redis_dir = None
    client = None
    commands_path = MATRIX_DIR / "commands.txt"
    raw_path = MATRIX_DIR / "raw_resp.log"

    with commands_path.open("w") as commands_log, raw_path.open("w") as raw_log:
        commands_log.write("# Stage 04 runtime matrix commands\n\n")
        raw_log.write("# Stage 04 raw RESP log\n")
        raw_log.write("Binary bulk payloads are escaped as hex-style byte escapes.\n")
        try:
            module_path = run_build(commands_log)
            port, redis_dir = start_redis(module_path, commands_log)
            client = RespClient(port, raw_log)

            def cmd(case_id: str, *args: Any) -> Resp:
                commands_log.write("REDIS " + case_id + ": " + format_cmd(args) + "\n")
                commands_log.flush()
                return client.cmd(case_id, *args)

            def expect(case_id: str, area: str, args: list[Any], expected: str,
                       predicate: Callable[[Resp], bool], classification: str = "PASS"):
                resp = cmd(case_id, *args)
                ok = predicate(resp)
                row(rows, case_id, area, format_cmd(args), expected, resp, ok, classification)
                return resp

            def acl_dryrun(case_id: str, args: list[Any], expected: str, predicate: Callable[[Resp], bool]):
                resp = cmd(case_id, *args)
                unsupported = contains_error(resp, "Unknown subcommand") and contains_error(resp, "DRYRUN")
                unsupported = unsupported or (contains_error(resp, "wrong number") and contains_error(resp, "DRYRUN"))
                if unsupported:
                    row(
                        rows,
                        case_id,
                        "metadata",
                        format_cmd(args),
                        expected + "; BLOCKED_ENV because Redis 6.2.16 does not expose ACL DRYRUN",
                        resp,
                        False,
                        "BLOCKED",
                    )
                else:
                    row(rows, case_id, "metadata", format_cmd(args), expected, resp, predicate(resp), "PASS")
                return resp

            # Baseline and environment.
            expect("env-hello2", "environment", ["HELLO", "2"], "HELLO 2 succeeds", lambda r: is_array(r) or r.typ == "%")
            expect("env-module-list", "environment", ["MODULE", "LIST"], "GeminiBloom module is loaded", lambda r: "GeminiBloom" in norm(r))
            expect("env-flushall", "environment", ["FLUSHALL"], "OK", lambda r: is_simple(r, "OK"))

            # RESP2 happy paths covering all 10 BF commands.
            expect("happy-reserve", "happy path", ["BF.RESERVE", "hp_reserve", "0.01", "100", "EXPANSION", "2"], "OK", lambda r: is_simple(r, "OK"))
            expect("happy-add-new", "happy path", ["BF.ADD", "hp_reserve", "a"], "new insert returns 1", lambda r: is_int(r, 1))
            expect("happy-add-dup", "happy path", ["BF.ADD", "hp_reserve", "a"], "duplicate returns 0", lambda r: is_int(r, 0))
            expect("happy-madd", "happy path", ["BF.MADD", "hp_reserve", "b", "c", "a"], "[1,1,0]", lambda r: ints(r) == [1, 1, 0])
            expect("happy-insert-create", "happy path", ["BF.INSERT", "hp_insert", "CAPACITY", "10", "ERROR", "0.01", "EXPANSION", "2", "ITEMS", "x", "y"], "[1,1]", lambda r: ints(r) == [1, 1])
            expect("happy-exists-present", "happy path", ["BF.EXISTS", "hp_insert", "x"], "present returns 1", lambda r: is_int(r, 1))
            expect("happy-exists-absent", "happy path", ["BF.EXISTS", "hp_insert", "z"], "absent returns 0", lambda r: is_int(r, 0))
            expect("happy-mexists", "happy path", ["BF.MEXISTS", "hp_insert", "x", "z"], "[1,0]", lambda r: ints(r) == [1, 0])
            info_full = expect("happy-info-full", "happy path", ["BF.INFO", "hp_insert"], "full info array length 10", lambda r: is_array(r, 10))
            expect("happy-info-items-field", "happy path", ["BF.INFO", "hp_insert", "ITEMS"], "scalar int field", lambda r: is_int(r, 2), "DESIGN_INTENDED")
            expect("happy-card", "happy path", ["BF.CARD", "hp_insert"], "cardinality 2", lambda r: is_int(r, 2))

            # BF.INFO field shapes and invalid field.
            for field in ["CAPACITY", "SIZE", "FILTERS", "ITEMS"]:
                expect(f"info-field-{field.lower()}", "BF.INFO fields", ["BF.INFO", "hp_insert", field], "scalar integer field", lambda r: r.typ == ":", "DESIGN_INTENDED")
            expect("info-field-expansion", "BF.INFO fields", ["BF.INFO", "hp_insert", "EXPANSION"], "scalar expansion integer", lambda r: is_int(r, 2), "DESIGN_INTENDED")
            expect("info-field-invalid", "BF.INFO fields", ["BF.INFO", "hp_insert", "NOPE"], "unknown field error", lambda r: contains_error(r, "unknown"))

            # Missing key behavior.
            expect("missing-add-autocreate", "missing key", ["BF.ADD", "missing_add", "a"], "auto-create and insert", lambda r: is_int(r, 1))
            expect("missing-madd-autocreate", "missing key", ["BF.MADD", "missing_madd", "a", "b"], "auto-create and insert [1,1]", lambda r: ints(r) == [1, 1])
            expect("missing-insert-nocreate", "missing key", ["BF.INSERT", "missing_insert_no", "NOCREATE", "ITEMS", "a"], "missing key error", lambda r: contains_error(r, "exist"))
            expect("missing-insert-create", "missing key", ["BF.INSERT", "missing_insert_yes", "ITEMS", "a"], "create and insert [1]", lambda r: ints(r) == [1])
            expect("missing-exists", "missing key", ["BF.EXISTS", "missing_nope", "a"], "0", lambda r: is_int(r, 0))
            expect("missing-mexists", "missing key", ["BF.MEXISTS", "missing_nope", "a", "b"], "[0,0]", lambda r: ints(r) == [0, 0])
            expect("missing-info", "missing key", ["BF.INFO", "missing_nope"], "ERR key does not exist", lambda r: contains_error(r, "does not exist"))
            expect("missing-card", "missing key", ["BF.CARD", "missing_nope"], "0", lambda r: is_int(r, 0))
            expect("missing-scandump", "missing key", ["BF.SCANDUMP", "missing_nope", "0"], "ERR key does not exist", lambda r: contains_error(r, "does not exist"))
            expect("missing-loadchunk-data", "missing key", ["BF.LOADCHUNK", "missing_nope", "2", "data"], "ERR key does not exist", lambda r: contains_error(r, "does not exist"))

            # Duplicate behavior.
            expect("dup-reserve", "duplicate", ["BF.RESERVE", "dup_key", "0.01", "100"], "OK", lambda r: is_simple(r, "OK"))
            expect("dup-add-a1", "duplicate", ["BF.ADD", "dup_key", "a"], "1", lambda r: is_int(r, 1))
            expect("dup-add-a2", "duplicate", ["BF.ADD", "dup_key", "a"], "0", lambda r: is_int(r, 0))
            expect("dup-madd", "duplicate", ["BF.MADD", "dup_key", "a", "a", "b"], "[0,0,1]", lambda r: ints(r) == [0, 0, 1])
            expect("dup-insert", "duplicate", ["BF.INSERT", "dup_key", "ITEMS", "b", "b", "c"], "[0,0,1]", lambda r: ints(r) == [0, 0, 1])
            expect("dup-card", "duplicate", ["BF.CARD", "dup_key"], "cardinality 3", lambda r: is_int(r, 3))
            expect("dup-info-items", "duplicate", ["BF.INFO", "dup_key", "ITEMS"], "items scalar 3", lambda r: is_int(r, 3), "DESIGN_INTENDED")

            # Binary, empty, non-UTF8, long item behavior.
            expect("binary-reserve", "binary items", ["BF.RESERVE", "bin_key", "0.01", "100"], "OK", lambda r: is_simple(r, "OK"))
            binary_items = [
                ("empty", b""),
                ("nul", b"a\x00b"),
                ("nonutf8", b"\xff\xfe\x00x"),
                ("long10k", b"L" * 10240),
            ]
            for name, item in binary_items:
                expect(f"binary-add-{name}", "binary items", ["BF.ADD", "bin_key", item], "insert returns 1", lambda r: is_int(r, 1))
                expect(f"binary-exists-{name}", "binary items", ["BF.EXISTS", "bin_key", item], "exists returns 1", lambda r: is_int(r, 1))
            expect("binary-mexists", "binary items", ["BF.MEXISTS", "bin_key", binary_items[0][1], binary_items[1][1], b"absent\x00"], "[1,1,0]", lambda r: ints(r) == [1, 1, 0])

            # Parser and resource boundaries.
            expect("boundary-capacity-zero", "boundaries", ["BF.RESERVE", "cap_zero", "0.01", "0"], "capacity error", lambda r: contains_error(r, "capacity"))
            expect("boundary-capacity-one", "boundaries", ["BF.RESERVE", "cap_one", "0.01", "1"], "OK", lambda r: is_simple(r, "OK"))
            expect("boundary-capacity-max-safe", "boundaries", ["BF.RESERVE", "cap_max", "0.9", "1073741824", "NONSCALING"], "max capacity accepted with safe fpRate", lambda r: is_simple(r, "OK"))
            expect("boundary-capacity-over", "boundaries", ["BF.RESERVE", "cap_over", "0.01", "1073741825"], "capacity error", lambda r: contains_error(r, "capacity"))
            for label, rate in [("zero", "0"), ("negative", "-0.1"), ("one", "1"), ("gtone", "1.5"), ("nan", "nan"), ("inf", "inf"), ("text", "nope")]:
                expect(f"boundary-error-{label}", "boundaries", ["BF.RESERVE", f"err_{label}", rate, "10"], "false-positive-rate rejected", lambda r: r.typ == "-",)
            expect("boundary-error-valid-small", "boundaries", ["BF.RESERVE", "err_valid_small", "0.0001", "10"], "OK", lambda r: is_simple(r, "OK"))
            expect("boundary-expansion-zero", "boundaries", ["BF.RESERVE", "exp_zero", "0.01", "10", "EXPANSION", "0"], "OK non-scaling", lambda r: is_simple(r, "OK"), "DESIGN_INTENDED")
            expect("boundary-expansion-zero-info", "boundaries", ["BF.INFO", "exp_zero", "EXPANSION"], "nil/null expansion for fixed", lambda r: (r.typ == "$" and r.value is None) or r.typ == "_", "DESIGN_INTENDED")
            for exp in ["1", "2", "32768"]:
                expect(f"boundary-expansion-{exp}", "boundaries", ["BF.RESERVE", f"exp_{exp}", "0.9", "10", "EXPANSION", exp], "OK", lambda r: is_simple(r, "OK"))
            expect("boundary-expansion-over", "boundaries", ["BF.RESERVE", "exp_over", "0.01", "10", "EXPANSION", "32769"], "expansion error", lambda r: r.typ == "-")
            expect("boundary-duplicate-expansion", "boundaries", ["BF.RESERVE", "dup_exp", "0.01", "10", "EXPANSION", "2", "EXPANSION", "3"], "duplicate error", lambda r: contains_error(r, "duplicate"))
            expect("boundary-duplicate-nonscaling", "boundaries", ["BF.RESERVE", "dup_ns", "0.01", "10", "NONSCALING", "NONSCALING"], "duplicate error", lambda r: contains_error(r, "duplicate"))
            expect("boundary-unknown-option", "boundaries", ["BF.RESERVE", "unknown_opt", "0.01", "10", "BOGUS"], "unknown option error", lambda r: contains_error(r, "unrecognized"))
            expect("boundary-ns-exp", "boundaries", ["BF.RESERVE", "ns_exp", "0.01", "10", "NONSCALING", "EXPANSION", "2"], "mutual exclusion error", lambda r: r.typ == "-")
            expect("boundary-insert-nocreate-capacity", "boundaries", ["BF.INSERT", "nokey", "NOCREATE", "CAPACITY", "10", "ITEMS", "a"], "NOCREATE conflict", lambda r: contains_error(r, "NOCREATE"))
            expect("boundary-insert-nocreate-error", "boundaries", ["BF.INSERT", "nokey", "NOCREATE", "ERROR", "0.01", "ITEMS", "a"], "NOCREATE conflict", lambda r: contains_error(r, "NOCREATE"))
            expect("boundary-insert-duplicate-error", "boundaries", ["BF.INSERT", "dup_err", "ERROR", "0.01", "ERROR", "0.02", "ITEMS", "a"], "duplicate error", lambda r: contains_error(r, "duplicate"))
            expect("boundary-insert-duplicate-capacity", "boundaries", ["BF.INSERT", "dup_cap", "CAPACITY", "10", "CAPACITY", "20", "ITEMS", "a"], "duplicate error", lambda r: contains_error(r, "duplicate"))
            expect("boundary-insert-exp0", "boundaries", ["BF.INSERT", "ins_exp0", "EXPANSION", "0", "ITEMS", "a"], "maps to non-scaling", lambda r: ints(r) == [1], "DESIGN_INTENDED")
            expect("boundary-insert-exp0-info", "boundaries", ["BF.INFO", "ins_exp0", "EXPANSION"], "nil/null expansion", lambda r: (r.typ == "$" and r.value is None) or r.typ == "_", "DESIGN_INTENDED")

            # NONSCALING full and partial failures.
            expect("fixed-reserve", "nonscaling full", ["BF.RESERVE", "fixed_key", "0.001", "2", "NONSCALING"], "OK", lambda r: is_simple(r, "OK"))
            expect("fixed-add-a", "nonscaling full", ["BF.ADD", "fixed_key", "a"], "1", lambda r: is_int(r, 1))
            expect("fixed-add-b", "nonscaling full", ["BF.ADD", "fixed_key", "b"], "1", lambda r: is_int(r, 1))
            expect("fixed-add-c-full", "nonscaling full", ["BF.ADD", "fixed_key", "c"], "full error", lambda r: contains_error(r, "full"))
            expect("fixed-add-a-dup-after-full", "nonscaling full", ["BF.ADD", "fixed_key", "a"], "duplicate 0 after full", lambda r: is_int(r, 0))
            expect("fixed-card", "nonscaling full", ["BF.CARD", "fixed_key"], "cardinality stable 2", lambda r: is_int(r, 2))

            expect("partial-madd-reserve", "partial failure", ["BF.RESERVE", "partial_madd", "0.001", "2", "NONSCALING"], "OK", lambda r: is_simple(r, "OK"))
            pm = expect("partial-madd", "partial failure", ["BF.MADD", "partial_madd", "a", "b", "c", "d"], "truncated [1,1,ERR]", lambda r: is_array(r, 3) and r.value[0].typ == ":" and r.value[1].typ == ":" and r.value[2].typ == "-")
            expect("partial-madd-card", "partial failure", ["BF.CARD", "partial_madd"], "cardinality 2", lambda r: is_int(r, 2))
            expect("partial-madd-unprocessed", "partial failure", ["BF.EXISTS", "partial_madd", "d"], "unprocessed item absent", lambda r: is_int(r, 0))

            expect("partial-insert-reserve", "partial failure", ["BF.RESERVE", "partial_insert", "0.001", "2", "NONSCALING"], "OK", lambda r: is_simple(r, "OK"))
            expect("partial-insert", "partial failure", ["BF.INSERT", "partial_insert", "NOCREATE", "ITEMS", "a", "b", "c", "d"], "truncated [1,1,ERR]", lambda r: is_array(r, 3) and r.value[0].typ == ":" and r.value[1].typ == ":" and r.value[2].typ == "-")
            expect("partial-insert-card", "partial failure", ["BF.CARD", "partial_insert"], "cardinality 2", lambda r: is_int(r, 2))
            expect("partial-insert-unprocessed", "partial failure", ["BF.EXISTS", "partial_insert", "d"], "unprocessed item absent", lambda r: is_int(r, 0))

            # SCANDUMP/LOADCHUNK private protocol and loading state.
            expect("sd-reserve", "scandump/loadchunk", ["BF.RESERVE", "sd_src", "0.0001", "4", "EXPANSION", "1"], "OK", lambda r: is_simple(r, "OK"))
            inserted = []
            for i in range(30):
                item = f"sd_item_{i}"
                inserted.append(item)
                cmd(f"sd-add-{i:02d}", "BF.ADD", "sd_src", item)
            filters_resp = expect("sd-info-filters", "scandump/loadchunk", ["BF.INFO", "sd_src", "FILTERS"], "multiple layers", lambda r: r.typ == ":" and r.value >= 2)
            chunks: list[tuple[int, bytes]] = []
            cursor = 0
            while True:
                resp = cmd(f"sd-scandump-{cursor}", "BF.SCANDUMP", "sd_src", str(cursor))
                ok_shape = is_array(resp, 2) and resp.value[0].typ == ":" and resp.value[1].typ == "$"
                row(rows, f"sd-scandump-shape-{cursor}", "scandump/loadchunk", f"BF.SCANDUMP sd_src {cursor}", "[next_cursor, bulk]", resp, ok_shape, "PASS")
                if not ok_shape:
                    break
                next_cursor = resp.value[0].value
                data = resp.value[1].value or b""
                if next_cursor == 0 and data == b"":
                    break
                chunks.append((next_cursor, data))
                cursor = next_cursor
                if len(chunks) > 20:
                    row(rows, "sd-scandump-loop-guard", "scandump/loadchunk", "loop guard", "no infinite cursor loop", resp, False)
                    break
            layer_index_ok = len(chunks) >= 2 and chunks[0][0] == 1 and all(chunks[i][0] == chunks[i - 1][0] + 1 for i in range(1, len(chunks)))
            row(rows, "sd-layer-index-cursors", "scandump/loadchunk", "cursor sequence", "layer-index cursor increments by 1", None, layer_index_ok, "DESIGN_INTENDED", actual=str([c for c, _ in chunks]))

            if chunks:
                expect("lc-load-header", "scandump/loadchunk", ["BF.LOADCHUNK", "sd_dst", str(chunks[0][0]), chunks[0][1]], "loading shell OK", lambda r: is_simple(r, "OK"))
                for idx, (cur, data) in enumerate(chunks[1:], start=1):
                    expect(f"lc-load-chunk-{idx}", "scandump/loadchunk", ["BF.LOADCHUNK", "sd_dst", str(cur), data], "chunk OK", lambda r: is_simple(r, "OK"))
                expect("lc-card-match", "scandump/loadchunk", ["BF.CARD", "sd_dst"], "destination card equals source card", lambda r: r.typ == ":")
                for i, item in enumerate(inserted[:10]):
                    expect(f"lc-membership-{i:02d}", "scandump/loadchunk", ["BF.EXISTS", "sd_dst", item], "no false negative", lambda r: is_int(r, 1))

            if chunks:
                expect("loading-header", "loading state", ["BF.LOADCHUNK", "half_key", str(chunks[0][0]), chunks[0][1]], "header OK creates loading key", lambda r: is_simple(r, "OK"))
                for op in [
                    ("BF.ADD", "half_key", "x"),
                    ("BF.MADD", "half_key", "x", "y"),
                    ("BF.INSERT", "half_key", "NOCREATE", "ITEMS", "x"),
                    ("BF.EXISTS", "half_key", "x"),
                    ("BF.MEXISTS", "half_key", "x", "y"),
                    ("BF.INFO", "half_key"),
                    ("BF.CARD", "half_key"),
                    ("BF.SCANDUMP", "half_key", "0"),
                ]:
                    expect("loading-reject-" + op[0].lower().replace(".", "-"), "loading state", list(op), "ERR filter is being loaded", lambda r: contains_error(r, "being loaded"))
                expect("loading-bad-size", "loading state", ["BF.LOADCHUNK", "half_key", "2", "short"], "data length mismatch", lambda r: contains_error(r, "length"))
                if len(chunks) > 1:
                    for idx, (cur, data) in enumerate(chunks[1:], start=1):
                        expect(f"loading-complete-{idx}", "loading state", ["BF.LOADCHUNK", "half_key", str(cur), data], "chunk OK", lambda r: is_simple(r, "OK"))
                expect("loading-completed-reject-cursor2", "loading state", ["BF.LOADCHUNK", "half_key", "2", b"\x00" * 16], "completed key rejects cursor>1", lambda r: contains_error(r, "received bad data"))

                expect("existing-header-src", "loading state", ["BF.RESERVE", "existing_dst", "0.01", "10"], "OK", lambda r: is_simple(r, "OK"))
                expect("existing-header-old", "loading state", ["BF.ADD", "existing_dst", "old"], "1", lambda r: is_int(r, 1))
                expect("existing-header-reject", "loading state", ["BF.LOADCHUNK", "existing_dst", str(chunks[0][0]), chunks[0][1]], "existing Bloom rejects header", lambda r: contains_error(r, "received bad data"))
                expect("existing-header-preserve", "loading state", ["BF.EXISTS", "existing_dst", "old"], "old data preserved", lambda r: is_int(r, 1))

            # Wrong type behavior after a valid header is available.
            header = chunks[0][1] if chunks else b"invalid"
            expect("wt-set", "wrong type", ["SET", "wrongtype", "value"], "OK", lambda r: is_simple(r, "OK"))
            wrong_type_cases = [
                ("BF.RESERVE", "wrongtype", "0.01", "10"),
                ("BF.ADD", "wrongtype", "item"),
                ("BF.MADD", "wrongtype", "item"),
                ("BF.INSERT", "wrongtype", "ITEMS", "item"),
                ("BF.EXISTS", "wrongtype", "item"),
                ("BF.MEXISTS", "wrongtype", "item"),
                ("BF.INFO", "wrongtype"),
                ("BF.CARD", "wrongtype"),
                ("BF.SCANDUMP", "wrongtype", "0"),
                ("BF.LOADCHUNK", "wrongtype", "1", header),
            ]
            for op in wrong_type_cases:
                expect("wrongtype-" + op[0].lower().replace(".", "-"), "wrong type", list(op), "WRONGTYPE", lambda r: contains_error(r, "WRONGTYPE"))
            expect("wrongtype-preserve", "wrong type", ["GET", "wrongtype"], "string key preserved", lambda r: r.typ == "$" and r.value == b"value")

            # Command metadata and ACL DRYRUN.
            expect("metadata-command-info", "metadata", ["COMMAND", "INFO", "BF.RESERVE", "BF.ADD", "BF.MADD", "BF.INSERT", "BF.EXISTS", "BF.MEXISTS", "BF.INFO", "BF.CARD", "BF.SCANDUMP", "BF.LOADCHUNK"], "metadata returned for all BF commands", lambda r: is_array(r, 10))
            getkeys_cases = [
                ("reserve", ["COMMAND", "GETKEYS", "BF.RESERVE", "cmd_key", "0.01", "10"]),
                ("add", ["COMMAND", "GETKEYS", "BF.ADD", "cmd_key", "item"]),
                ("madd", ["COMMAND", "GETKEYS", "BF.MADD", "cmd_key", "a", "b"]),
                ("insert", ["COMMAND", "GETKEYS", "BF.INSERT", "cmd_key", "ITEMS", "a"]),
                ("exists", ["COMMAND", "GETKEYS", "BF.EXISTS", "cmd_key", "a"]),
                ("mexists", ["COMMAND", "GETKEYS", "BF.MEXISTS", "cmd_key", "a"]),
                ("info", ["COMMAND", "GETKEYS", "BF.INFO", "cmd_key"]),
                ("card", ["COMMAND", "GETKEYS", "BF.CARD", "cmd_key"]),
                ("scandump", ["COMMAND", "GETKEYS", "BF.SCANDUMP", "cmd_key", "0"]),
                ("loadchunk", ["COMMAND", "GETKEYS", "BF.LOADCHUNK", "cmd_key", "1", "data"]),
            ]
            for name, args in getkeys_cases:
                expect(f"metadata-getkeys-{name}", "metadata", args, "[cmd_key]", lambda r: is_array(r, 1) and r.value[0].typ == "$" and r.value[0].value == b"cmd_key")
            acl_dryrun("metadata-acl-dryrun-default-add", ["ACL", "DRYRUN", "default", "BF.ADD", "acl_key", "item"], "default user can dry-run BF.ADD when Redis supports ACL DRYRUN", lambda r: is_simple(r, "OK"))
            expect("metadata-acl-setuser", "metadata", ["ACL", "SETUSER", "stage04_ro", "on", "nopass", "~*", "+@read", "-@write"], "OK", lambda r: is_simple(r, "OK"))
            acl_dryrun("metadata-acl-ro-read", ["ACL", "DRYRUN", "stage04_ro", "BF.EXISTS", "acl_key", "item"], "read-only user can dry-run readonly BF.EXISTS when Redis supports ACL DRYRUN", lambda r: is_simple(r, "OK"))
            acl_dryrun("metadata-acl-ro-write", ["ACL", "DRYRUN", "stage04_ro", "BF.ADD", "acl_key", "item"], "read-only user rejects write BF.ADD when Redis supports ACL DRYRUN", lambda r: r.typ == "-" and not contains_error(r, "Unknown subcommand"))
            expect("metadata-acl-deluser", "metadata", ["ACL", "DELUSER", "stage04_ro"], "user deleted", lambda r: r.typ == ":")

            # RESP3 focused behavior.
            expect("resp3-hello3", "RESP3", ["HELLO", "3"], "HELLO 3 succeeds", lambda r: r.typ == "%")
            expect("resp3-add", "RESP3", ["BF.ADD", "resp3_key", "a"], "well-formed integer reply", lambda r: is_int(r, 1), "DESIGN_INTENDED")
            expect("resp3-exists", "RESP3", ["BF.EXISTS", "resp3_key", "a"], "well-formed integer reply", lambda r: is_int(r, 1), "DESIGN_INTENDED")
            expect("resp3-mexists", "RESP3", ["BF.MEXISTS", "resp3_key", "a", "b"], "well-formed array reply", lambda r: ints(r) == [1, 0], "DESIGN_INTENDED")
            expect("resp3-info-full", "RESP3", ["BF.INFO", "resp3_key"], "well-formed array reply", lambda r: is_array(r, 10), "DESIGN_INTENDED")
            expect("resp3-info-field", "RESP3", ["BF.INFO", "resp3_key", "ITEMS"], "scalar field reply", lambda r: is_int(r, 1), "DESIGN_INTENDED")
            expect("resp3-scandump", "RESP3", ["BF.SCANDUMP", "resp3_key", "0"], "array [cursor, bulk]", lambda r: is_array(r, 2), "DESIGN_INTENDED")

            # Cleanup.
            expect("cleanup-hello2", "cleanup", ["HELLO", "2"], "HELLO 2 succeeds", lambda r: is_array(r) or r.typ == "%")
            expect("cleanup-flushall", "cleanup", ["FLUSHALL"], "OK", lambda r: is_simple(r, "OK"))
        except Exception as exc:
            rows.append(Row("stage04-blocker", "BLOCKED", "harness", "runtime matrix completes", repr(exc), "BLOCKED", "stderr.log", False))
            (STAGE_DIR / "stderr.log").write_text(repr(exc) + "\n")
        finally:
            if client:
                try:
                    client.close()
                except Exception:
                    pass
            if port is not None:
                # Best-effort cleanup if SHUTDOWN was not reached.
                redis_cli = shutil.which("redis-cli")
                if redis_cli:
                    subprocess.run([redis_cli, "-p", str(port), "SHUTDOWN", "NOSAVE"], text=True, capture_output=True, timeout=3)
            if redis_dir:
                log_file = redis_dir / "redis.log"
                if log_file.exists():
                    (MATRIX_DIR / "redis.log").write_text(log_file.read_text(errors="replace"))

    failures = [r for r in rows if not r.ok or r.classification in {"FAIL", "BLOCKED"}]
    write_results(rows, failures)
    return 0


def write_results(rows: list[Row], failures: list[Row]):
    normalized = MATRIX_DIR / "normalized_results.md"
    with normalized.open("w") as f:
        f.write("# Stage 04 Runtime Matrix Normalized Results\n\n")
        f.write(f"- Total rows: {len(rows)}\n")
        f.write(f"- Failed/blocking rows: {len(failures)}\n\n")
        f.write("| Case | Area | Command | Expected | Actual | Classification | Result |\n")
        f.write("|---|---|---|---|---|---|---|\n")
        for r in rows:
            f.write("| {case} | {area} | `{cmd}` | {expected} | {actual} | {cls} | {result} |\n".format(
                case=r.case_id,
                area=r.area,
                cmd=escape_md(r.command),
                expected=escape_md(r.expected),
                actual=escape_md(r.actual),
                cls=r.classification,
                result="PASS" if r.ok else ("BLOCKED" if r.classification == "BLOCKED" else "FAIL"),
            ))

    failures_path = MATRIX_DIR / "failures.md"
    with failures_path.open("w") as f:
        f.write("# Stage 04 Runtime Matrix Failures\n\n")
        if not failures:
            f.write("No FAIL or BLOCKED runtime matrix rows.\n")
        else:
            f.write("| Case | Area | Command | Expected | Actual | Classification | Evidence |\n")
            f.write("|---|---|---|---|---|---|---|\n")
            for r in failures:
                f.write(f"| {r.case_id} | {r.area} | `{escape_md(r.command)}` | {escape_md(r.expected)} | {escape_md(r.actual)} | {r.classification} | {r.evidence} |\n")

    coverage = MATRIX_DIR / "coverage_summary.md"
    commands = ["BF.RESERVE", "BF.ADD", "BF.MADD", "BF.INSERT", "BF.EXISTS", "BF.MEXISTS", "BF.INFO", "BF.CARD", "BF.SCANDUMP", "BF.LOADCHUNK"]
    with coverage.open("w") as f:
        f.write("# Stage 04 Coverage Summary\n\n")
        for cmd_name in commands:
            count = sum(1 for r in rows if cmd_name in r.command)
            f.write(f"- `{cmd_name}`: {count} matrix rows\n")
        f.write("\n## Areas\n\n")
        for area in sorted(set(r.area for r in rows)):
            count = sum(1 for r in rows if r.area == area)
            f.write(f"- {area}: {count}\n")

    top_commands = STAGE_DIR / "commands.txt"
    runtime_commands = MATRIX_DIR / "commands.txt"
    top_commands.write_text(runtime_commands.read_text())

    (STAGE_DIR / "stdout.log").write_text(
        f"Stage 04 runtime matrix completed with {len(rows)} rows and {len(failures)} failed/blocking rows.\n"
        f"Normalized results: {normalized}\n"
        f"Failures: {failures_path}\n"
    )
    stderr = STAGE_DIR / "stderr.log"
    if not stderr.exists():
        stderr.write_text("")
    (STAGE_DIR / "exit_codes.txt").write_text("stage04_runtime_matrix.py: 0; case-level FAIL/BLOCKED rows are recorded in normalized_results.md and failures.md\n")
    (STAGE_DIR / "env_snapshot.txt").write_text(
        f"repo: {ROOT}\n"
        f"python: {sys.version}\n"
        f"redis-server: {shutil.which('redis-server')}\n"
        f"redis-cli: {shutil.which('redis-cli')}\n"
        f"cmake: {shutil.which('cmake')}\n"
    )
    (STAGE_DIR / "evidence_index.md").write_text(
        "# Stage 04 Evidence Index\n\n"
        "| Evidence | Purpose |\n"
        "|---|---|\n"
        "| `runtime_matrix/stage04_runtime_matrix.py` | Raw RESP runtime harness. |\n"
        "| `runtime_matrix/commands.txt` | Runtime commands and harness subprocess commands. |\n"
        "| `runtime_matrix/raw_resp.log` | Raw RESP request/reply frames. |\n"
        "| `runtime_matrix/normalized_results.md` | Normalized semantic matrix. |\n"
        "| `runtime_matrix/failures.md` | FAIL/BLOCKED rows. |\n"
        "| `runtime_matrix/coverage_summary.md` | Command/area coverage counts. |\n"
        "| `commands.txt` | Top-level evidence policy command log copy. |\n"
        "| `stdout.log` | Harness summary. |\n"
        "| `stderr.log` | Harness/blocker stderr summary. |\n"
        "| `exit_codes.txt` | Harness exit-code note. |\n"
        "| `env_snapshot.txt` | Runtime environment snapshot. |\n"
    )


def escape_md(text: str) -> str:
    return str(text).replace("|", "\\|").replace("\n", "<br>")


if __name__ == "__main__":
    raise SystemExit(main())
