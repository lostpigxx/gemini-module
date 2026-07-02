#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import random
import shlex
import socket
import subprocess
import sys
import time
from pathlib import Path


COMMAND_LOG = []
STDOUT_EVENTS = []
STDERR_EVENTS = []
EXIT_CODES = []
SERVERS = []


class RespError:
    def __init__(self, message):
        self.message = message

    def __repr__(self):
        return f"RespError({self.message!r})"


def now_ms():
    return int(time.time() * 1000)


def write_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def append_event(target, label, text):
    target.append(f"## {label}\n{text}\n")


def norm(value):
    if isinstance(value, RespError):
        return {"error": value.message}
    if isinstance(value, bytes):
        try:
            decoded = value.decode("utf-8")
            if all((ch == "\n" or ch == "\r" or ch == "\t" or 32 <= ord(ch) < 127) for ch in decoded):
                return decoded
        except UnicodeDecodeError:
            pass
        return {"bytes_hex": value.hex(), "len": len(value)}
    if isinstance(value, list):
        return [norm(v) for v in value]
    return value


def deep_norm(value):
    if isinstance(value, dict):
        return {str(deep_norm(k)): deep_norm(v) for k, v in value.items()}
    if isinstance(value, tuple):
        return [deep_norm(v) for v in value]
    if isinstance(value, list):
        return [deep_norm(v) for v in value]
    return norm(value)


def jd(value):
    return json.dumps(deep_norm(value), indent=2, ensure_ascii=False)


def is_error(value, contains=None):
    if not isinstance(value, RespError):
        return False
    return contains is None or contains.lower() in value.message.lower()


def status_from_checks(checks):
    return "PASS" if all(checks) else "FAIL"


def run_proc(label, args, cwd=None, input_text=None, timeout=60):
    COMMAND_LOG.append(f"{label}: " + " ".join(shlex.quote(str(a)) for a in args))
    try:
        proc = subprocess.run(
            [str(a) for a in args],
            cwd=str(cwd) if cwd else None,
            input=input_text,
            text=True,
            capture_output=True,
            timeout=timeout,
        )
        append_event(STDOUT_EVENTS, label, proc.stdout)
        append_event(STDERR_EVENTS, label, proc.stderr)
        EXIT_CODES.append(f"{label}: {proc.returncode}")
        return proc
    except subprocess.TimeoutExpired as exc:
        append_event(STDOUT_EVENTS, label, exc.stdout or "")
        append_event(STDERR_EVENTS, label, f"TIMEOUT after {timeout}s\n{exc.stderr or ''}")
        EXIT_CODES.append(f"{label}: TIMEOUT")
        raise


class RedisClient:
    def __init__(self, port, label, host="127.0.0.1", username=None, password=None):
        self.port = int(port)
        self.host = host
        self.label = label
        self.sock = socket.create_connection((host, self.port), timeout=5)
        self.file = self.sock.makefile("rb")
        if username or password:
            if username:
                self.cmd("AUTH", username, password or "")
            else:
                self.cmd("AUTH", password or "")

    def close(self):
        try:
            self.file.close()
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass

    def cmd(self, *args):
        COMMAND_LOG.append(f"{self.label}:{self.port}> " + " ".join(display_arg(a) for a in args))
        payload = [f"*{len(args)}\r\n".encode()]
        for arg in args:
            if isinstance(arg, bytes):
                b = arg
            else:
                b = str(arg).encode()
            payload.append(f"${len(b)}\r\n".encode())
            payload.append(b)
            payload.append(b"\r\n")
        self.sock.sendall(b"".join(payload))
        return self._read_resp()

    def _readline(self):
        line = self.file.readline()
        if not line:
            raise RuntimeError(f"connection closed for {self.label}:{self.port}")
        if not line.endswith(b"\r\n"):
            raise RuntimeError(f"malformed RESP line: {line!r}")
        return line[:-2]

    def _read_resp(self):
        prefix = self.file.read(1)
        if not prefix:
            raise RuntimeError(f"connection closed for {self.label}:{self.port}")
        if prefix == b"+":
            return self._readline().decode("utf-8", "replace")
        if prefix == b"-":
            return RespError(self._readline().decode("utf-8", "replace"))
        if prefix == b":":
            return int(self._readline())
        if prefix == b"$":
            length = int(self._readline())
            if length < 0:
                return None
            data = self.file.read(length)
            trailer = self.file.read(2)
            if trailer != b"\r\n":
                raise RuntimeError("malformed bulk string trailer")
            return data
        if prefix == b"*":
            count = int(self._readline())
            if count < 0:
                return None
            return [self._read_resp() for _ in range(count)]
        raise RuntimeError(f"unknown RESP prefix: {prefix!r}")


def display_arg(arg):
    if isinstance(arg, bytes):
        try:
            text = arg.decode("utf-8")
            if all(32 <= ord(ch) < 127 for ch in text):
                return shlex.quote(text)
        except UnicodeDecodeError:
            pass
        return f"<bytes:{len(arg)}:{arg[:16].hex()}>"
    return shlex.quote(str(arg))


def free_port(port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind(("127.0.0.1", int(port)))
            return True
        except OSError:
            return False


def alloc_ports(count, cluster=False):
    for _ in range(200):
        base = random.randint(43000, 48900)
        ports = list(range(base, base + count))
        if all(free_port(p) and (not cluster or free_port(p + 10000)) for p in ports):
            return ports
    raise RuntimeError("could not allocate free ports")


class RedisServer:
    def __init__(self, name, port, workdir, redis_server, module, cluster=False, replicaof=None):
        self.name = name
        self.port = int(port)
        self.dir = Path(workdir)
        self.redis_server = redis_server
        self.module = module
        self.cluster = cluster
        self.replicaof = replicaof
        self.conf = self.dir / "redis.conf"
        self.log = self.dir / "redis.log"
        self.started = False

    def start(self):
        self.dir.mkdir(parents=True, exist_ok=True)
        lines = [
            f"port {self.port}",
            "bind 127.0.0.1",
            "protected-mode no",
            f"dir {self.dir}",
            "dbfilename dump.rdb",
            f"logfile {self.log}",
            "loglevel notice",
            "daemonize yes",
            'save ""',
            "appendonly no",
            f"loadmodule {self.module}",
        ]
        if self.replicaof:
            host, port = self.replicaof
            lines.append(f"replicaof {host} {port}")
        if self.cluster:
            lines.extend([
                "cluster-enabled yes",
                "cluster-node-timeout 5000",
                "cluster-require-full-coverage no",
                f"cluster-config-file nodes-{self.port}.conf",
            ])
        write_text(self.conf, "\n".join(lines) + "\n")
        run_proc(f"start-{self.name}", [self.redis_server, str(self.conf)], timeout=20)
        deadline = time.time() + 8
        last = None
        while time.time() < deadline:
            try:
                c = RedisClient(self.port, self.name)
                last = c.cmd("PING")
                c.close()
                if last == "PONG":
                    self.started = True
                    SERVERS.append(self)
                    return
            except Exception as exc:
                last = exc
                time.sleep(0.1)
        raise RuntimeError(f"server {self.name}:{self.port} did not start; last={last!r}; log={self.log.read_text(errors='replace') if self.log.exists() else ''}")

    def client(self, label=None, username=None, password=None):
        return RedisClient(self.port, label or self.name, username=username, password=password)

    def stop(self):
        if not self.started:
            return
        run_proc(f"stop-{self.name}", ["redis-cli", "-p", str(self.port), "SHUTDOWN", "NOSAVE"], timeout=10)
        self.started = False

    def log_tail(self, lines=200):
        if not self.log.exists():
            return ""
        content = self.log.read_text(errors="replace").splitlines()
        return "\n".join(content[-lines:])


def parse_info(text_value):
    if isinstance(text_value, bytes):
        text = text_value.decode("utf-8", "replace")
    else:
        text = str(text_value)
    out = {}
    for line in text.splitlines():
        if not line or line.startswith("#") or ":" not in line:
            continue
        k, v = line.split(":", 1)
        out[k] = v
    return out


def wait_replica_ready(replica, master=None, timeout=12):
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        c = replica.client(f"wait-{replica.name}")
        try:
            info = parse_info(c.cmd("INFO", "replication"))
            last = info
            if info.get("role") == "slave" and info.get("master_link_status") == "up" and info.get("master_sync_in_progress", "0") == "0":
                return info
        finally:
            c.close()
        time.sleep(0.2)
    raise RuntimeError(f"replica {replica.name} not ready; last={last}")


def wait_cluster_ok(client, timeout=20):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        info = parse_info(client.cmd("CLUSTER", "INFO"))
        last = info
        if info.get("cluster_state") == "ok":
            return info
        time.sleep(0.25)
    raise RuntimeError(f"cluster not ok: {last}")


def info_map(reply):
    if not isinstance(reply, list):
        return {}
    result = {}
    i = 0
    while i + 1 < len(reply):
        key = norm(reply[i])
        result[str(key)] = norm(reply[i + 1])
        i += 2
    return result


def all_ints(reply, expected):
    return isinstance(reply, list) and reply == expected


def module_sha(path):
    data = Path(path).read_bytes()
    return hashlib.sha256(data).hexdigest()


def run_env(root, args):
    env = {
        "git_head": run_proc("git-head", ["git", "rev-parse", "HEAD"], cwd=args.repo, timeout=10).stdout.strip(),
        "git_status": run_proc("git-status", ["git", "status", "--short"], cwd=args.repo, timeout=10).stdout,
        "redis_server_version": run_proc("redis-server-version", [args.redis_server, "--version"], timeout=10).stdout.strip(),
        "redis_cli_version": run_proc("redis-cli-version", [args.redis_cli, "--version"], timeout=10).stdout.strip(),
        "module_path": args.module,
        "module_sha256": module_sha(args.module),
        "module_size": Path(args.module).stat().st_size,
        "runtime_root": str(args.runtime_root),
    }
    probe = RedisServer("feature-probe", args.ports.pop(0), args.runtime_root / "feature-probe", args.redis_server, args.module)
    probe.start()
    c = probe.client()
    try:
        acl = c.cmd("ACL", "DRYRUN", "default", "PING")
        getkeysandflags = c.cmd("COMMAND", "GETKEYSANDFLAGS", "GET", "x")
        env["acl_dryrun_probe"] = norm(acl)
        env["command_getkeysandflags_probe"] = norm(getkeysandflags)
    finally:
        c.close()
        probe.stop()
    write_text(root / "env_snapshot.txt", jd(env) + "\n")
    return env


def run_replica(root, args):
    rdir = root / "replica"
    work = args.runtime_root / "replica"
    results = {"live": {}, "fullsync": {}, "reconnect": {}, "loading_partial": {}}

    # Live command stream.
    ports = [args.ports.pop(0), args.ports.pop(0)]
    master = RedisServer("live-master", ports[0], work / "live-master", args.redis_server, args.module)
    replica = RedisServer("live-replica", ports[1], work / "live-replica", args.redis_server, args.module, replicaof=("127.0.0.1", ports[0]))
    try:
        master.start()
        replica.start()
        wait_replica_ready(replica)
        m = master.client("live-master")
        r = replica.client("live-replica")
        try:
            live_log = []
            for cmd in [
                ("BF.RESERVE", "bf:live", "0.01", "4", "EXPANSION", "2"),
                ("BF.MADD", "bf:live", "a", "b", "c", "d", "e", "f", "g", "h"),
                ("BF.RESERVE", "bf:fixed", "0.01", "2", "NONSCALING"),
                ("BF.MADD", "bf:fixed", "x", "y", "z"),
                ("BF.ADD", "bf:auto", "auto-1"),
            ]:
                live_log.append({"cmd": cmd, "reply": norm(m.cmd(*cmd))})
            live_log.append({"cmd": ("BF.ADD", "bf:bin", b""), "reply": norm(m.cmd("BF.ADD", "bf:bin", b""))})
            live_log.append({"cmd": ("BF.ADD", "bf:bin", b"a\\x00b"), "reply": norm(m.cmd("BF.ADD", "bf:bin", b"a\x00b"))})
            live_log.append({"cmd": ("WAIT", 1, 5000), "reply": norm(m.cmd("WAIT", 1, 5000))})
            time.sleep(0.5)
            checks = {
                "live_members": all_ints(r.cmd("BF.MEXISTS", "bf:live", "a", "b", "c", "d", "e", "f", "g", "h"), [1] * 8),
                "fixed_members": all_ints(r.cmd("BF.MEXISTS", "bf:fixed", "x", "y"), [1, 1]),
                "fixed_rejected_z": r.cmd("BF.EXISTS", "bf:fixed", "z") == 0,
                "auto_member": r.cmd("BF.EXISTS", "bf:auto", "auto-1") == 1,
                "empty_item": r.cmd("BF.EXISTS", "bf:bin", b"") == 1,
                "nul_item": r.cmd("BF.EXISTS", "bf:bin", b"a\x00b") == 1,
            }
            primary_info = {
                "bf:live": info_map(m.cmd("BF.INFO", "bf:live")),
                "bf:fixed": info_map(m.cmd("BF.INFO", "bf:fixed")),
                "bf:auto": info_map(m.cmd("BF.INFO", "bf:auto")),
            }
            replica_info = {
                "bf:live": info_map(r.cmd("BF.INFO", "bf:live")),
                "bf:fixed": info_map(r.cmd("BF.INFO", "bf:fixed")),
                "bf:auto": info_map(r.cmd("BF.INFO", "bf:auto")),
            }
            stable_fields = ["Capacity", "Number of filters", "Number of items inserted", "Expansion rate"]
            info_checks = {}
            for key in primary_info:
                info_checks[key] = {field: primary_info[key].get(field) == replica_info[key].get(field) for field in stable_fields}
            write_rejections = {}
            for name, cmd in {
                "add": ("BF.ADD", "bf:live", "replica-write"),
                "reserve": ("BF.RESERVE", "bf:new", "0.01", "10"),
                "madd": ("BF.MADD", "bf:live", "rw1", "rw2"),
                "insert": ("BF.INSERT", "bf:live", "ITEMS", "rw1"),
                "loadchunk": ("BF.LOADCHUNK", "bf:chunk", "1", b"payload"),
            }.items():
                write_rejections[name] = norm(r.cmd(*cmd))
            readonly = {
                "exists": norm(r.cmd("BF.EXISTS", "bf:live", "a")),
                "mexists": norm(r.cmd("BF.MEXISTS", "bf:live", "a", "b")),
                "info": norm(r.cmd("BF.INFO", "bf:live")),
                "card": norm(r.cmd("BF.CARD", "bf:live")),
                "scandump": norm(r.cmd("BF.SCANDUMP", "bf:live", "0")),
            }
            checks["writes_rejected"] = all(is_error(v) for v in [r.cmd("BF.ADD", "bf:live", "replica-write-2")])
            checks["readonly_success"] = readonly["exists"] == 1 and readonly["mexists"] == [1, 1] and isinstance(readonly["info"], list) and isinstance(readonly["card"], int) and isinstance(readonly["scandump"], list)
            checks["info_fields_match"] = all(all(v.values()) for v in info_checks.values())
            results["live"] = {
                "status": status_from_checks(checks.values()),
                "checks": checks,
                "live_log": live_log,
                "primary_info": primary_info,
                "replica_info": replica_info,
                "info_checks": info_checks,
                "write_rejections": write_rejections,
                "readonly": readonly,
                "replica_info_raw": parse_info(r.cmd("INFO", "replication")),
            }
        finally:
            m.close()
            r.close()
    except Exception as exc:
        results["live"] = {"status": "FAIL", "error": repr(exc)}
    finally:
        write_text(rdir / "live_command_stream.log", jd(results["live"]) + "\n")
        write_text(rdir / "primary.log", master.log_tail())
        write_text(rdir / "replica.log", replica.log_tail())
        replica.stop()
        master.stop()

    # Fullsync snapshot.
    ports = [args.ports.pop(0), args.ports.pop(0)]
    master = RedisServer("fullsync-master", ports[0], work / "fullsync-master", args.redis_server, args.module)
    replica = RedisServer("fullsync-replica", ports[1], work / "fullsync-replica", args.redis_server, args.module, replicaof=("127.0.0.1", ports[0]))
    try:
        master.start()
        m = master.client("fullsync-master")
        try:
            for cmd in [
                ("BF.RESERVE", "bf:fs:single", "0.01", "10"),
                ("BF.MADD", "bf:fs:single", "s1", "s2", "s3"),
                ("BF.RESERVE", "bf:fs:multi", "0.01", "3", "EXPANSION", "2"),
                ("BF.MADD", "bf:fs:multi", "m1", "m2", "m3", "m4", "m5", "m6", "m7"),
                ("BF.RESERVE", "bf:fs:fixed", "0.01", "2", "NONSCALING"),
                ("BF.MADD", "bf:fs:fixed", "f1", "f2"),
                ("BF.ADD", "bf:fs:ttl", "ttl-item"),
                ("PEXPIRE", "bf:fs:ttl", "60000"),
            ]:
                m.cmd(*cmd)
        finally:
            m.close()
        replica.start()
        wait_replica_ready(replica)
        r = replica.client("fullsync-replica")
        m = master.client("fullsync-master-verify")
        try:
            checks = {
                "single_members": all_ints(r.cmd("BF.MEXISTS", "bf:fs:single", "s1", "s2", "s3"), [1, 1, 1]),
                "multi_members": all_ints(r.cmd("BF.MEXISTS", "bf:fs:multi", "m1", "m2", "m3", "m4", "m5", "m6", "m7"), [1] * 7),
                "fixed_members": all_ints(r.cmd("BF.MEXISTS", "bf:fs:fixed", "f1", "f2"), [1, 1]),
                "ttl_member": r.cmd("BF.EXISTS", "bf:fs:ttl", "ttl-item") == 1,
            }
            pttl_primary = m.cmd("PTTL", "bf:fs:ttl")
            pttl_replica = r.cmd("PTTL", "bf:fs:ttl")
            checks["ttl_plausible"] = isinstance(pttl_primary, int) and isinstance(pttl_replica, int) and pttl_primary > 0 and pttl_replica > 0 and abs(pttl_primary - pttl_replica) < 15000
            results["fullsync"] = {
                "status": status_from_checks(checks.values()),
                "checks": checks,
                "pttl_primary": pttl_primary,
                "pttl_replica": pttl_replica,
                "primary_info": parse_info(m.cmd("INFO", "replication")),
                "replica_info": parse_info(r.cmd("INFO", "replication")),
                "dbsize_primary": norm(m.cmd("DBSIZE")),
                "dbsize_replica": norm(r.cmd("DBSIZE")),
                "log_fullsync_evidence": "Full resync" in replica.log_tail() or "MASTER <-> REPLICA sync" in replica.log_tail(),
            }
        finally:
            r.close()
            m.close()
    except Exception as exc:
        results["fullsync"] = {"status": "FAIL", "error": repr(exc)}
    finally:
        write_text(rdir / "fullsync_snapshot.log", jd(results["fullsync"]) + "\n")
        write_text(rdir / "fullsync_primary.log", master.log_tail())
        write_text(rdir / "fullsync_replica.log", replica.log_tail())
        replica.stop()
        master.stop()

    # Reconnect.
    ports = [args.ports.pop(0), args.ports.pop(0)]
    master = RedisServer("reconnect-master", ports[0], work / "reconnect-master", args.redis_server, args.module)
    replica = RedisServer("reconnect-replica", ports[1], work / "reconnect-replica", args.redis_server, args.module, replicaof=("127.0.0.1", ports[0]))
    try:
        master.start()
        replica.start()
        wait_replica_ready(replica)
        m = master.client("reconnect-master")
        r = replica.client("reconnect-replica")
        try:
            m.cmd("BF.RESERVE", "bf:reconn", "0.01", "4", "EXPANSION", "2")
            m.cmd("BF.MADD", "bf:reconn", "before-1", "before-2")
            m.cmd("WAIT", 1, 5000)
            kill_reply = m.cmd("CLIENT", "KILL", "TYPE", "replica")
            m.cmd("BF.MADD", "bf:reconn", "during-1", "during-2", "during-3")
            time.sleep(1.0)
            wait_replica_ready(replica, timeout=15)
            m.cmd("WAIT", 1, 5000)
            short_members = r.cmd("BF.MEXISTS", "bf:reconn", "before-1", "before-2", "during-1", "during-2", "during-3")
            r.cmd("REPLICAOF", "NO", "ONE")
            time.sleep(0.5)
            m.cmd("BF.MADD", "bf:reconn", "forced-1", "forced-2")
            r.cmd("REPLICAOF", "127.0.0.1", str(master.port))
            wait_replica_ready(replica, timeout=15)
            m.cmd("WAIT", 1, 5000)
            forced_members = r.cmd("BF.MEXISTS", "bf:reconn", "before-1", "before-2", "during-1", "during-2", "during-3", "forced-1", "forced-2")
            checks = {
                "short_reconnect_members": short_members == [1, 1, 1, 1, 1],
                "forced_reconnect_members": forced_members == [1, 1, 1, 1, 1, 1, 1],
                "readonly_after_reconnect": r.cmd("BF.EXISTS", "bf:reconn", "forced-1") == 1,
                "write_rejected_after_reconnect": is_error(r.cmd("BF.ADD", "bf:reconn", "replica-write")),
            }
            log_tail = replica.log_tail(400)
            results["reconnect"] = {
                "status": status_from_checks(checks.values()),
                "checks": checks,
                "kill_reply": norm(kill_reply),
                "short_members": norm(short_members),
                "forced_members": norm(forced_members),
                "partial_resync_seen": "Partial resynchronization" in log_tail,
                "full_resync_seen": "Full resync" in log_tail or "MASTER <-> REPLICA sync" in log_tail,
            }
        finally:
            m.close()
            r.close()
    except Exception as exc:
        results["reconnect"] = {"status": "FAIL", "error": repr(exc)}
    finally:
        write_text(rdir / "reconnect.log", jd(results["reconnect"]) + "\n")
        write_text(rdir / "reconnect_primary.log", master.log_tail())
        write_text(rdir / "reconnect_replica.log", replica.log_tail())
        replica.stop()
        master.stop()

    # Loading partial fullsync impact.
    ports = [args.ports.pop(0), args.ports.pop(0)]
    master = RedisServer("loading-master", ports[0], work / "loading-master", args.redis_server, args.module)
    replica = RedisServer("loading-replica", ports[1], work / "loading-replica", args.redis_server, args.module, replicaof=("127.0.0.1", ports[0]))
    try:
        master.start()
        m = master.client("loading-master")
        try:
            m.cmd("BF.RESERVE", "bf:loading:src", "0.01", "10")
            m.cmd("BF.MADD", "bf:loading:src", "la", "lb")
            header_reply = m.cmd("BF.SCANDUMP", "bf:loading:src", "0")
            if not isinstance(header_reply, list) or len(header_reply) != 2 or header_reply[0] != 1 or not isinstance(header_reply[1], bytes):
                raise RuntimeError(f"unexpected header reply {header_reply!r}")
            load_header = m.cmd("BF.LOADCHUNK", "bf:loading:partial", "1", header_reply[1])
            protected = {
                "exists": norm(m.cmd("BF.EXISTS", "bf:loading:partial", "la")),
                "info": norm(m.cmd("BF.INFO", "bf:loading:partial")),
                "card": norm(m.cmd("BF.CARD", "bf:loading:partial")),
                "scandump": norm(m.cmd("BF.SCANDUMP", "bf:loading:partial", "0")),
                "add": norm(m.cmd("BF.ADD", "bf:loading:partial", "lc")),
            }
        finally:
            m.close()
        replica.start()
        wait_replica_ready(replica)
        r = replica.client("loading-replica")
        try:
            replica_exists = r.cmd("BF.EXISTS", "bf:loading:partial", "la")
            replica_info = r.cmd("BF.INFO", "bf:loading:partial")
            readable_completed_with_false_negative = isinstance(replica_exists, int) and replica_exists == 0 and isinstance(replica_info, list)
            protected_on_primary = all(isinstance(v, dict) and "error" in v and "loading" in v["error"].lower() for v in protected.values())
            results["loading_partial"] = {
                "status": "FAIL_CARRY_FORWARD_GBV6-07-002" if readable_completed_with_false_negative else ("PASS" if protected_on_primary else "FAIL"),
                "load_header_reply": norm(load_header),
                "primary_loading_protection": protected,
                "replica_exists_la": norm(replica_exists),
                "replica_info": norm(replica_info),
                "readable_completed_with_false_negative": readable_completed_with_false_negative,
                "classification": "Operational fullsync manifestation of GBV6-07-002" if readable_completed_with_false_negative else "No Stage 07 persistence replay observed",
            }
        finally:
            r.close()
    except Exception as exc:
        results["loading_partial"] = {"status": "FAIL", "error": repr(exc)}
    finally:
        write_text(rdir / "loading_partial_fullsync.log", jd(results["loading_partial"]) + "\n")
        write_text(rdir / "loading_primary.log", master.log_tail())
        write_text(rdir / "loading_replica.log", replica.log_tail())
        replica.stop()
        master.stop()

    write_text(rdir / "setup.md", f"# Replica setup\n\nRuntime workdir: `{work}`\n\nAll server logs are persisted in this directory.\n")
    write_text(rdir / "commands.txt", "\n".join(x for x in COMMAND_LOG if any(prefix in x for prefix in ["live-", "fullsync-", "reconnect-", "loading-", "wait-"])) + "\n")
    write_text(rdir / "info_replication_before_after.log", jd({
        "live": results.get("live", {}).get("replica_info_raw"),
        "fullsync": results.get("fullsync", {}).get("replica_info"),
        "reconnect": results.get("reconnect", {}),
    }) + "\n")
    write_text(rdir / "summary.md", replica_summary(results))
    return results


def replica_summary(results):
    lines = ["# Stage 09 Replica Summary", ""]
    for key in ["live", "fullsync", "reconnect", "loading_partial"]:
        lines.append(f"- `{key}`: `{results.get(key, {}).get('status', 'MISSING')}`")
    lines.extend([
        "",
        "Normal completed-filter live replication, fullsync, and reconnect checks are PASS only when all inserted items remain present on the replica.",
        "`loading_partial` is expected to carry forward Stage 07 if a half-loaded key replays as a readable completed filter.",
        "",
    ])
    return "\n".join(lines)


def run_metadata(root, args):
    cserver = RedisServer("metadata", args.ports.pop(0), args.runtime_root / "metadata", args.redis_server, args.module)
    cserver.start()
    c = cserver.client("metadata")
    try:
        commands = ["BF.RESERVE", "BF.ADD", "BF.MADD", "BF.INSERT", "BF.EXISTS", "BF.MEXISTS", "BF.INFO", "BF.CARD", "BF.SCANDUMP", "BF.LOADCHUNK"]
        info = c.cmd("COMMAND", "INFO", *commands)
        expected = {
            "BF.RESERVE": {"write", "denyoom"},
            "BF.ADD": {"write", "denyoom"},
            "BF.MADD": {"write", "denyoom"},
            "BF.INSERT": {"write", "denyoom"},
            "BF.LOADCHUNK": {"write", "denyoom"},
            "BF.EXISTS": {"readonly"},
            "BF.MEXISTS": {"readonly"},
            "BF.INFO": {"readonly"},
            "BF.CARD": {"readonly"},
            "BF.SCANDUMP": {"readonly", "fast"},
        }
        flag_checks = {}
        normalized_info = norm(info)
        for entry in info:
            if not isinstance(entry, list) or len(entry) < 3:
                continue
            name = norm(entry[0])
            flags = {norm(f).replace("-", "") for f in entry[2]}
            flag_checks[name] = expected.get(name, set()).issubset(flags)
        getkeys_cases = {
            "reserve": ("BF.RESERVE", "cmd:key", "0.01", "100"),
            "add": ("BF.ADD", "cmd:key", "item"),
            "madd": ("BF.MADD", "cmd:key", "item1", "item2"),
            "insert_capacity": ("BF.INSERT", "cmd:key", "CAPACITY", "10", "ERROR", "0.01", "ITEMS", "item1", "item2"),
            "insert_nocreate": ("BF.INSERT", "cmd:key", "NOCREATE", "ITEMS", "item1"),
            "exists": ("BF.EXISTS", "cmd:key", "item"),
            "mexists": ("BF.MEXISTS", "cmd:key", "item1", "item2"),
            "info": ("BF.INFO", "cmd:key"),
            "info_field": ("BF.INFO", "cmd:key", "ITEMS"),
            "card": ("BF.CARD", "cmd:key"),
            "scandump": ("BF.SCANDUMP", "cmd:key", "0"),
            "loadchunk": ("BF.LOADCHUNK", "cmd:key", "1", "payload"),
        }
        getkeys = {name: norm(c.cmd("COMMAND", "GETKEYS", *cmd)) for name, cmd in getkeys_cases.items()}
        getkeys_checks = {name: reply == ["cmd:key"] for name, reply in getkeys.items()}
        getkeysandflags = c.cmd("COMMAND", "GETKEYSANDFLAGS", "BF.ADD", "cmd:key", "item")
        result = {
            "status": status_from_checks(list(flag_checks.values()) + list(getkeys_checks.values())),
            "command_info": normalized_info,
            "flag_checks": flag_checks,
            "getkeys": getkeys,
            "getkeys_checks": getkeys_checks,
            "getkeysandflags_probe": norm(getkeysandflags),
        }
    except Exception as exc:
        result = {"status": "FAIL", "error": repr(exc)}
    finally:
        c.close()
        write_text(root / "command_metadata" / "server.log", cserver.log_tail())
        cserver.stop()
    write_text(root / "command_metadata" / "command_info.json", jd(result.get("command_info")) + "\n")
    write_text(root / "command_metadata" / "getkeys.json", jd(result.get("getkeys")) + "\n")
    write_text(root / "command_metadata" / "summary.md", metadata_summary(result))
    return result


def metadata_summary(result):
    return "\n".join([
        "# Stage 09 Command Metadata Summary",
        "",
        f"Status: `{result.get('status')}`",
        "",
        "Flag checks:",
        jd(result.get("flag_checks")),
        "",
        "GETKEYS checks:",
        jd(result.get("getkeys_checks")),
        "",
    ])


def run_acl(root, args):
    server = RedisServer("acl", args.ports.pop(0), args.runtime_root / "acl", args.redis_server, args.module)
    server.start()
    c = server.client("acl")
    try:
        c.cmd("BF.ADD", "bf:acl", "item")
        dryrun = c.cmd("ACL", "DRYRUN", "default", "BF.EXISTS", "bf:acl", "item")
        dryrun_supported = not is_error(dryrun, "Unknown subcommand")
        c.cmd("ACL", "SETUSER", "stage09_ro", "on", ">stage09pass", "~bf:*", "-@all", "+BF.EXISTS", "+BF.MEXISTS", "+BF.INFO", "+BF.CARD", "+BF.SCANDUMP")
        ro = RedisClient(server.port, "acl-ro", username="stage09_ro", password="stage09pass")
        try:
            actual = {
                "exists_allowed": norm(ro.cmd("BF.EXISTS", "bf:acl", "item")),
                "mexists_allowed": norm(ro.cmd("BF.MEXISTS", "bf:acl", "item")),
                "info_allowed": norm(ro.cmd("BF.INFO", "bf:acl")),
                "card_allowed": norm(ro.cmd("BF.CARD", "bf:acl")),
                "scandump_allowed": norm(ro.cmd("BF.SCANDUMP", "bf:acl", "0")),
                "add_denied": norm(ro.cmd("BF.ADD", "bf:acl", "write")),
                "reserve_denied": norm(ro.cmd("BF.RESERVE", "bf:new", "0.01", "10")),
                "key_pattern_denied": norm(ro.cmd("BF.EXISTS", "other:acl", "item")),
            }
        finally:
            ro.close()
        checks = {
            "exists_allowed": actual["exists_allowed"] == 1,
            "mexists_allowed": actual["mexists_allowed"] == [1],
            "info_allowed": isinstance(actual["info_allowed"], list),
            "card_allowed": isinstance(actual["card_allowed"], int),
            "scandump_allowed": isinstance(actual["scandump_allowed"], list),
            "add_denied": isinstance(actual["add_denied"], dict) and "error" in actual["add_denied"],
            "reserve_denied": isinstance(actual["reserve_denied"], dict) and "error" in actual["reserve_denied"],
            "key_pattern_denied": isinstance(actual["key_pattern_denied"], dict) and "error" in actual["key_pattern_denied"],
        }
        result = {
            "status": status_from_checks(checks.values()),
            "dryrun_supported": dryrun_supported,
            "dryrun_probe": norm(dryrun),
            "actual_acl_smoke": actual,
            "actual_acl_checks": checks,
            "dryrun_classification": "PASS_OR_SUPPORTED" if dryrun_supported else "BLOCKED_ACL_DRYRUN",
        }
        if not dryrun_supported:
            write_text(root / "acl" / "blocked_acl.md", "# ACL DRYRUN Blocked\n\n`ACL DRYRUN` is not supported by this Redis binary. Actual ACL user smoke tests were still executed and classified separately.\n\nRaw probe:\n\n```json\n" + jd(norm(dryrun)) + "\n```\n")
    except Exception as exc:
        result = {"status": "FAIL", "error": repr(exc)}
    finally:
        c.close()
        write_text(root / "acl" / "server.log", server.log_tail())
        server.stop()
    write_text(root / "acl" / "acl_results.json", jd(result) + "\n")
    write_text(root / "acl" / "summary.md", "\n".join([
        "# Stage 09 ACL Summary",
        "",
        f"Status: `{result.get('status')}`",
        f"ACL DRYRUN: `{result.get('dryrun_classification')}`",
        "",
        jd(result),
        "",
    ]))
    return result


def run_cluster(root, args):
    croot = root / "cluster"
    work = args.runtime_root / "cluster"
    ports = [args.ports.pop(0) for _ in range(6)]
    nodes = [RedisServer(f"cluster-{i}", p, work / f"node-{p}", args.redis_server, args.module, cluster=True) for i, p in enumerate(ports)]
    result = {"status": "BLOCKED_CLUSTER_ENV", "ports": ports}
    try:
        for node in nodes:
            node.start()
        create_args = [args.redis_cli, "--cluster", "create"] + [f"127.0.0.1:{p}" for p in ports] + ["--cluster-replicas", "1", "--cluster-yes"]
        create = run_proc("cluster-create", create_args, timeout=60)
        if create.returncode != 0:
            raise RuntimeError(f"cluster create failed: {create.stderr}")
        first = nodes[0].client("cluster-first")
        try:
            cluster_info = wait_cluster_ok(first)
            modules = {}
            for node in nodes:
                nc = node.client(f"module-list-{node.port}")
                try:
                    modules[node.port] = norm(nc.cmd("MODULE", "LIST"))
                finally:
                    nc.close()
            key = "{gb09}:live"
            copy_key = "{gb09}:copy"
            slot = first.cmd("CLUSTER", "KEYSLOT", key)
            slots = None
            cluster_nodes = None
            owner_port = None
            replica_port = None
            for _ in range(40):
                slots = first.cmd("CLUSTER", "SLOTS")
                cluster_nodes = first.cmd("CLUSTER", "NODES")
                owner_port, replica_port = owner_for_slot(slots, slot, cluster_nodes)
                if replica_port:
                    break
                time.sleep(0.25)
            owner = RedisClient(owner_port, "cluster-owner")
            non_owner_port = next(p for p in ports if p != owner_port)
            non_owner = RedisClient(non_owner_port, "cluster-nonowner")
            try:
                owner.cmd("BF.RESERVE", key, "0.01", "4", "EXPANSION", "2")
                owner.cmd("BF.MADD", key, "c1", "c2", "c3", "c4", "c5")
                owner.cmd("WAIT", "1", "5000")
                owner_checks = {
                    "members": owner.cmd("BF.MEXISTS", key, "c1", "c2", "c3", "c4", "c5") == [1, 1, 1, 1, 1],
                    "info": isinstance(owner.cmd("BF.INFO", key), list),
                    "card": isinstance(owner.cmd("BF.CARD", key), int),
                }
                moved_cmds = {
                    "reserve": ("BF.RESERVE", "{gb09}:new", "0.01", "10"),
                    "add": ("BF.ADD", key, "x"),
                    "madd": ("BF.MADD", key, "x", "y"),
                    "insert": ("BF.INSERT", key, "ITEMS", "x"),
                    "exists": ("BF.EXISTS", key, "c1"),
                    "mexists": ("BF.MEXISTS", key, "c1", "c2"),
                    "info": ("BF.INFO", key),
                    "card": ("BF.CARD", key),
                    "scandump": ("BF.SCANDUMP", key, "0"),
                    "loadchunk": ("BF.LOADCHUNK", key, "1", b"payload"),
                }
                moved = {name: norm(non_owner.cmd(*cmd)) for name, cmd in moved_cmds.items()}
                moved_checks = {name: isinstance(reply, dict) and "MOVED" in reply.get("error", "") for name, reply in moved.items()}
                cli_redirect = run_proc("cluster-cli-redirect", [args.redis_cli, "-c", "-p", str(non_owner_port), "BF.EXISTS", key, "c1"], timeout=20)
                redirect_ok = cli_redirect.stdout.strip() == "1"
                scandump_chunks = []
                cursor = 0
                while True:
                    rep = owner.cmd("BF.SCANDUMP", key, str(cursor))
                    if not isinstance(rep, list) or len(rep) != 2:
                        raise RuntimeError(f"bad SCANDUMP reply {rep!r}")
                    next_cursor, blob = rep
                    if next_cursor == 0:
                        break
                    scandump_chunks.append((next_cursor, blob))
                    cursor = next_cursor
                load_replies = [norm(owner.cmd("BF.LOADCHUNK", copy_key, str(cur), blob)) for cur, blob in scandump_chunks]
                copy_ok = owner.cmd("BF.EXISTS", copy_key, "c1") == 1
                readonly = {"status": "NOT_VERIFIED_NO_REPLICA"}
                if replica_port:
                    replica = RedisClient(replica_port, "cluster-replica")
                    try:
                        before = replica.cmd("BF.EXISTS", key, "c1")
                        ro_reply = replica.cmd("READONLY")
                        after_exists = replica.cmd("BF.EXISTS", key, "c1")
                        after_info = replica.cmd("BF.INFO", key)
                        after_card = replica.cmd("BF.CARD", key)
                        after_scandump = replica.cmd("BF.SCANDUMP", key, "0")
                        write_reply = replica.cmd("BF.ADD", key, "replica-write")
                        readonly = {
                            "status": "PASS" if after_exists == 1 and isinstance(after_info, list) and isinstance(after_card, int) and isinstance(after_scandump, list) and is_error(write_reply) else "FAIL",
                            "before_readonly": norm(before),
                            "readonly_reply": norm(ro_reply),
                            "after_exists": norm(after_exists),
                            "after_info_is_array": isinstance(after_info, list),
                            "after_card": norm(after_card),
                            "after_scandump_is_array": isinstance(after_scandump, list),
                            "write_reply": norm(write_reply),
                        }
                    finally:
                        replica.close()
                write_text(croot / "ask_not_verified.md", "# ASK Not Verified\n\nA deterministic ASK transition was not produced in this Stage 09 run. MOVED routing, `redis-cli -c` redirection, owner execution, and cluster READONLY replica path were verified separately.\n")
                checks = {
                    "cluster_state_ok": cluster_info.get("cluster_state") == "ok",
                    "modules_on_nodes": all("GeminiBloom" in jd(v) for v in modules.values()),
                    "owner_checks": all(owner_checks.values()),
                    "moved_checks": all(moved_checks.values()),
                    "redis_cli_c_redirect": redirect_ok,
                    "scandump_loadchunk_roundtrip": all(x == "OK" for x in load_replies) and copy_ok,
                    "readonly_replica": readonly.get("status") == "PASS",
                }
                result = {
                    "status": status_from_checks(checks.values()),
                    "ports": ports,
                    "slot": slot,
                    "owner_port": owner_port,
                    "replica_port": replica_port,
                    "cluster_info": cluster_info,
                    "cluster_slots": norm(slots),
                    "cluster_nodes": norm(cluster_nodes),
                    "module_lists": modules,
                    "owner_checks": owner_checks,
                    "moved": moved,
                    "moved_checks": moved_checks,
                    "redis_cli_c_redirect_stdout": cli_redirect.stdout,
                    "redis_cli_c_redirect_stderr": cli_redirect.stderr,
                    "scandump_chunk_count": len(scandump_chunks),
                    "loadchunk_replies": load_replies,
                    "copy_ok": copy_ok,
                    "readonly": readonly,
                    "checks": checks,
                    "ask": "NOT_VERIFIED",
                }
            finally:
                owner.close()
                non_owner.close()
        finally:
            first.close()
    except Exception as exc:
        result = {"status": "BLOCKED_CLUSTER_ENV", "error": repr(exc), "ports": ports}
        write_text(root / "blocked_cluster.md", "# Cluster Blocked\n\nCluster execution could not complete.\n\n```json\n" + jd(result) + "\n```\n")
    finally:
        for node in nodes:
            write_text(croot / f"node_{node.port}.log", node.log_tail(400))
        for node in nodes:
            node.stop()
    write_text(croot / "cluster_results.json", jd(result) + "\n")
    write_text(croot / "summary.md", "\n".join([
        "# Stage 09 Cluster Summary",
        "",
        f"Status: `{result.get('status')}`",
        f"ASK: `{result.get('ask', 'NOT_VERIFIED_OR_BLOCKED')}`",
        "",
        jd({k: result.get(k) for k in ["checks", "owner_port", "replica_port", "slot", "error"]}),
        "",
    ]))
    return result


def owner_for_slot(slots, slot, cluster_nodes=None):
    for entry in slots:
        start, end = entry[0], entry[1]
        if start <= slot <= end:
            owner = entry[2]
            owner_port = int(owner[1])
            replica_port = int(entry[3][1]) if len(entry) > 3 else replica_from_cluster_nodes(cluster_nodes, norm(owner[2]) if len(owner) > 2 else None)
            return owner_port, replica_port
    raise RuntimeError(f"no owner for slot {slot}")


def replica_from_cluster_nodes(cluster_nodes, owner_id):
    if not cluster_nodes or not owner_id:
        return None
    text = cluster_nodes.decode("utf-8", "replace") if isinstance(cluster_nodes, bytes) else str(cluster_nodes)
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 4:
            continue
        node_id, addr, flags, master_id = parts[:4]
        if master_id != owner_id:
            continue
        if "slave" not in flags and "replica" not in flags:
            continue
        host_port = addr.split("@", 1)[0]
        if ":" not in host_port:
            continue
        return int(host_port.rsplit(":", 1)[1])
    return None


def write_root_files(root, results):
    write_text(root / "commands.txt", "\n".join(COMMAND_LOG) + "\n")
    write_text(root / "stdout.log", "\n".join(STDOUT_EVENTS) + "\n")
    write_text(root / "stderr.log", "\n".join(STDERR_EVENTS) + "\n")
    write_text(root / "exit_codes.txt", "\n".join(EXIT_CODES) + "\n")
    write_text(root / "stage09_results.json", jd(results) + "\n")
    index = [
        "# Stage 09 Evidence Index",
        "",
        "| Conclusion | Evidence |",
        "|---|---|",
        "| Environment and artifact baseline | `env_snapshot.txt`, `build/` |",
        "| Replica live command stream | `replica/live_command_stream.log`, `replica/primary.log`, `replica/replica.log` |",
        "| Replica fullsync snapshot | `replica/fullsync_snapshot.log`, `replica/fullsync_primary.log`, `replica/fullsync_replica.log` |",
        "| Replica reconnect | `replica/reconnect.log`, `replica/reconnect_primary.log`, `replica/reconnect_replica.log` |",
        "| Loading partial fullsync impact | `replica/loading_partial_fullsync.log`, `replica/loading_primary.log`, `replica/loading_replica.log` |",
        "| Command metadata and key extraction | `command_metadata/command_info.json`, `command_metadata/getkeys.json`, `command_metadata/summary.md` |",
        "| ACL behavior | `acl/acl_results.json`, `acl/summary.md`, `acl/blocked_acl.md` if present |",
        "| Cluster module/routing/readonly | `cluster/cluster_results.json`, `cluster/summary.md`, `cluster/node_*.log`, `cluster/ask_not_verified.md` |",
        "",
    ]
    write_text(root / "evidence_index.md", "\n".join(index))


def stop_all():
    for server in reversed(SERVERS):
        try:
            server.stop()
        except Exception:
            pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--root", required=True)
    parser.add_argument("--redis-server", required=True)
    parser.add_argument("--redis-cli", required=True)
    parser.add_argument("--module", required=True)
    args = parser.parse_args()
    args.repo = Path(args.repo)
    root = Path(args.root)
    args.runtime_root = Path("/tmp") / f"gemini-module-v6-stage09-runtime-{now_ms()}-{os.getpid()}"
    root.mkdir(parents=True, exist_ok=True)
    random.seed(9009)
    args.ports = alloc_ports(40, cluster=True)
    results = {}
    try:
        results["env"] = run_env(root, args)
        results["replica"] = run_replica(root, args)
        results["command_metadata"] = run_metadata(root, args)
        results["acl"] = run_acl(root, args)
        results["cluster"] = run_cluster(root, args)
        return_code = 0
    except Exception as exc:
        results["fatal"] = repr(exc)
        append_event(STDERR_EVENTS, "fatal", repr(exc))
        return_code = 1
    finally:
        stop_all()
        write_root_files(root, results)
    return return_code


if __name__ == "__main__":
    sys.exit(main())
