"""Master + Replica Redis 7 topology lifecycle with all 3 Gemini modules loaded."""

from __future__ import annotations

import os
import shutil
import socket
import subprocess
import tempfile
import threading
import time
from typing import Dict, Optional, Tuple

import redis

from .config import SoakConfig


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class RedisNode:
    """Manages a single redis-server process (master or replica)."""

    def __init__(self, name: str, port: int, workdir: str, cfg: SoakConfig):
        self.name = name
        self.port = port
        self.workdir = workdir
        self.cfg = cfg
        self.proc: Optional[subprocess.Popen] = None
        self.conf_path = os.path.join(workdir, f"{name}.conf")
        self.log_path = os.path.join(workdir, f"{name}.log")
        self.is_jemalloc = False

    def write_conf(
        self,
        replicaof: Optional[Tuple[str, int]] = None,
        appendonly: bool = True,
        aof_use_rdb_preamble: bool = True,
        maxmemory_policy: str = "volatile-lru",
        extra_lines: Optional[list[str]] = None,
    ) -> None:
        os.makedirs(self.workdir, exist_ok=True)
        aof_min = max(32, self.cfg.maxmemory_mb // 8)
        repl_backlog = max(16, min(256, self.cfg.maxmemory_mb // 16))
        lines = [
            f"port {self.port}",
            "bind 127.0.0.1",
            "protected-mode no",
            f"dir {self.workdir}",
            f"logfile {self.log_path}",
            "loglevel notice",
            "dbfilename dump.rdb",
            f"maxmemory {self.cfg.maxmemory_mb}mb",
            f"maxmemory-policy {maxmemory_policy}",
            "maxmemory-samples 5",
            "maxmemory-eviction-tenacity 10",
            "replica-ignore-maxmemory yes",
            # Lazyfree: exercise module free callbacks in bio threads
            "lazyfree-lazy-eviction yes",
            "lazyfree-lazy-expire yes",
            "lazyfree-lazy-server-del yes",
            "lazyfree-lazy-user-del yes",
            "lazyfree-lazy-user-flush yes",
            "replica-lazy-flush yes",
            # Persistence
            f"appendonly {'yes' if appendonly else 'no'}",
            "appendfsync everysec",
            f"aof-use-rdb-preamble {'yes' if aof_use_rdb_preamble else 'no'}",
            "auto-aof-rewrite-percentage 0",  # Orchestrator triggers AOF rewrite explicitly to enforce fork mutex
            f"auto-aof-rewrite-min-size {aof_min}mb",
            'save ""',  # Orchestrator triggers BGSAVE explicitly to enforce fork mutex
            # Replication
            f"repl-backlog-size {repl_backlog}mb",
            "repl-diskless-sync yes",
            "repl-diskless-sync-delay 0",
            "repl-diskless-load disabled",
            "client-output-buffer-limit replica 1gb 512mb 60",
            # Observability & debug
            "hz 10",
            "latency-monitor-threshold 50",
            "slowlog-log-slower-than 10000",
            "enable-debug-command local",
            # Load order: json -> bloom -> search (search depends on json notifications)
            f"loadmodule {self.cfg.json_so}",
            f"loadmodule {self.cfg.bloom_so}",
            f"loadmodule {self.cfg.search_so}",
        ]
        if replicaof is not None:
            lines.append(f"replicaof {replicaof[0]} {replicaof[1]}")
        if extra_lines:
            lines.extend(extra_lines)
        with open(self.conf_path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")

    def start(self, timeout_sec: float = 30.0) -> None:
        if self.proc is not None and self.proc.poll() is None:
            return
        self.proc = subprocess.Popen(
            [self.cfg.redis_server, self.conf_path],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        deadline = time.monotonic() + timeout_sec
        last_err: Optional[Exception] = None
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                err_tail = self.read_log_tail(40)
                raise RuntimeError(
                    f"{self.name} exited prematurely (rc={self.proc.returncode}):\n{err_tail}"
                )
            try:
                c = self.connect(decode_responses=True, timeout=2.0)
                if c.ping():
                    info_mem = c.info("memory")
                    alloc = str(info_mem.get("mem_allocator", "")).lower()
                    self.is_jemalloc = "jemalloc" in alloc
                    c.close()
                    return
            except Exception as e:
                last_err = e
                time.sleep(0.1)
        raise RuntimeError(f"{self.name} failed to start on port {self.port}: {last_err}")

    def connect(
        self,
        protocol: int = 2,
        decode_responses: bool = False,
        timeout: float = 10.0,
    ) -> redis.Redis:
        kwargs: Dict[str, object] = {
            "host": "127.0.0.1",
            "port": self.port,
            "protocol": protocol,
            "decode_responses": decode_responses,
            "socket_timeout": timeout,
            "socket_connect_timeout": timeout,
        }
        try:
            from redis.maint_notifications import MaintNotificationsConfig

            kwargs["maint_notifications_config"] = MaintNotificationsConfig(enabled=False)
        except Exception:
            pass
        return redis.Redis(**kwargs)

    def is_alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def pid(self) -> Optional[int]:
        return self.proc.pid if self.proc is not None else None

    def stop(self, save: bool = False, force_kill: bool = False) -> None:
        if self.proc is None:
            return
        if force_kill:
            try:
                self.proc.kill()
                self.proc.wait(timeout=5)
            except Exception:
                pass
            self.proc = None
            return
        try:
            c = self.connect(timeout=5.0)
            c.execute_command("SHUTDOWN", "SAVE" if save else "NOSAVE")
            c.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.proc = None

    def read_log_tail(self, lines: int = 80) -> str:
        if not os.path.isfile(self.log_path):
            return ""
        try:
            with open(self.log_path, "r", encoding="utf-8", errors="replace") as f:
                data = f.readlines()
            return "".join(data[-lines:])
        except OSError:
            return ""

    def check_log_for_bugs(self) -> list[str]:
        """Scan redis log for crash/bug markers."""
        hits: list[str] = []
        if not os.path.isfile(self.log_path):
            return hits
        markers = (
            "REDIS BUG REPORT",
            "AddressSanitizer",
            "UndefinedBehaviorSanitizer",
            "ThreadSanitizer",
            "Segmentation fault",
            "SIGABRT",
            "SIGSEGV",
            "ASSERTION FAILED",
        )
        try:
            with open(self.log_path, "r", encoding="utf-8", errors="replace") as f:
                for idx, line in enumerate(f, 1):
                    for m in markers:
                        if m in line:
                            hits.append(f"{self.name}.log:{idx}: {line.strip()}")
                            break
        except OSError:
            pass
        return hits


class MasterReplicaTopology:
    """Coordinates a Master + Replica pair with fork mutex and role-swap support."""

    def __init__(self, cfg: SoakConfig, base_dir: Optional[str] = None):
        self.cfg = cfg
        self.base_dir = base_dir or tempfile.mkdtemp(prefix="gemini_full_soak_")
        self.owned_dir = base_dir is None
        self.node_a = RedisNode(
            "node_a", find_free_port(), os.path.join(self.base_dir, "node_a"), cfg
        )
        self.node_b = RedisNode(
            "node_b", find_free_port(), os.path.join(self.base_dir, "node_b"), cfg
        )
        self.master: RedisNode = self.node_a
        self.replica: RedisNode = self.node_b
        self.role_lock = threading.RLock()
        self.fork_lock = threading.Lock()

    def start(self) -> None:
        self.master.write_conf(replicaof=None)
        self.master.start()
        self.replica.write_conf(replicaof=("127.0.0.1", self.master.port))
        self.replica.start()
        self.wait_replica_online(timeout_sec=30.0)
        # Enable activedefrag automatically when running under jemalloc
        if self.master.is_jemalloc:
            for node in (self.master, self.replica):
                c = node.connect(decode_responses=True)
                try:
                    c.config_set("activedefrag", "yes")
                    c.config_set("active-defrag-ignore-bytes", "64mb")
                    c.config_set("active-defrag-threshold-lower", "10")
                except Exception:
                    pass
                finally:
                    c.close()

    def wait_replica_online(self, timeout_sec: float = 30.0) -> None:
        deadline = time.monotonic() + timeout_sec
        while time.monotonic() < deadline:
            try:
                rc = self.replica.connect(decode_responses=True, timeout=3.0)
                info = rc.info("replication")
                rc.close()
                if info.get("master_link_status") == "up":
                    return
            except Exception:
                pass
            time.sleep(0.2)
        raise RuntimeError("Replica failed to reach master_link_status:up")

    def wait_repl_synced(self, timeout_sec: float = 30.0) -> bool:
        """Wait until replica_repl_offset == master_repl_offset."""
        deadline = time.monotonic() + timeout_sec
        while time.monotonic() < deadline:
            try:
                mc = self.master.connect(decode_responses=True, timeout=3.0)
                rc = self.replica.connect(decode_responses=True, timeout=3.0)
                minfo = mc.info("replication")
                rinfo = rc.info("replication")
                mc.close()
                rc.close()
                m_off = int(minfo.get("master_repl_offset", 0))
                r_off = int(rinfo.get("slave_repl_offset", -1))
                if rinfo.get("master_link_status") == "up" and r_off >= m_off:
                    return True
            except Exception:
                pass
            time.sleep(0.15)
        return False

    def wait_no_fork_in_progress(self, timeout_sec: float = 60.0) -> None:
        """Ensure neither master nor replica has an active BGSAVE or AOF rewrite."""
        deadline = time.monotonic() + timeout_sec
        while time.monotonic() < deadline:
            busy = False
            for node in (self.master, self.replica):
                if not node.is_alive():
                    continue
                try:
                    c = node.connect(decode_responses=True, timeout=3.0)
                    p = c.info("persistence")
                    c.close()
                    if (
                        int(p.get("rdb_bgsave_in_progress", 0))
                        or int(p.get("aof_rewrite_in_progress", 0))
                        or int(p.get("loading", 0))
                    ):
                        busy = True
                except Exception:
                    busy = True
            if not busy:
                return
            time.sleep(0.2)
        raise RuntimeError("Timed out waiting for background fork/load to complete")

    def wait_lazyfree_drained(self, node: Optional[RedisNode] = None, timeout_sec: float = 30.0) -> None:
        targets = [node] if node is not None else [self.master, self.replica]
        deadline = time.monotonic() + timeout_sec
        while time.monotonic() < deadline:
            all_done = True
            for n in targets:
                if not n.is_alive():
                    continue
                try:
                    c = n.connect(decode_responses=True, timeout=3.0)
                    mem = c.info("memory")
                    c.close()
                    if int(mem.get("lazyfree_pending_objects", 0)) > 0:
                        all_done = False
                except Exception:
                    all_done = False
            if all_done:
                return
            time.sleep(0.1)

    def swap_roles(self) -> None:
        with self.role_lock:
            self.master, self.replica = self.replica, self.master

    def stop(self) -> None:
        self.replica.stop(save=False)
        self.master.stop(save=False)
        if self.owned_dir and not self.cfg.keep_on_fail:
            shutil.rmtree(self.base_dir, ignore_errors=True)

    def snapshot_info(self) -> Dict[str, Dict[str, object]]:
        out: Dict[str, Dict[str, object]] = {}
        for label, node in [("master", self.master), ("replica", self.replica)]:
            try:
                c = node.connect(decode_responses=True, timeout=3.0)
                out[label] = {
                    "port": node.port,
                    "memory": c.info("memory"),
                    "stats": c.info("stats"),
                    "replication": c.info("replication"),
                    "persistence": c.info("persistence"),
                }
                c.close()
            except Exception as e:
                out[label] = {"port": node.port, "error": str(e)}
        return out
