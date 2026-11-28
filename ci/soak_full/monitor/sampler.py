"""Background telemetry sampler for Master and Replica."""

from __future__ import annotations

import csv
import os
import sqlite3
import threading
import time
from typing import Optional

from ..config import SoakConfig
from ..topology import MasterReplicaTopology


FIELDS = [
    "ts",
    "role",
    "port",
    "used_memory_mb",
    "rss_mb",
    "frag_ratio",
    "lazyfree_pending",
    "ops_per_sec",
    "expired_keys",
    "evicted_keys",
    "repl_offset",
    "sync_full",
    "sync_partial_ok",
    "bgsave_in_prog",
    "aof_rewrite_in_prog",
]


class TelemetrySampler:
    def __init__(self, cfg: SoakConfig, topo: MasterReplicaTopology, report_dir: str):
        self.cfg = cfg
        self.topo = topo
        self.csv_path = os.path.join(report_dir, "metrics.csv")
        self.db_path = os.path.join(report_dir, "metrics.sqlite")
        self._stop_evt = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self.max_used_mb: float = 0.0
        self.max_frag_ratio: float = 0.0
        self.sample_count: int = 0

    def start(self) -> None:
        with open(self.csv_path, "w", newline="", encoding="utf-8") as f:
            csv.writer(f).writerow(FIELDS)
        conn = sqlite3.connect(self.db_path)
        cols = ", ".join(
            f"{c} TEXT" if c in ("role",) else (f"{c} INTEGER" if c in ("port", "lazyfree_pending", "ops_per_sec", "expired_keys", "evicted_keys", "repl_offset", "sync_full", "sync_partial_ok", "bgsave_in_prog", "aof_rewrite_in_prog") else f"{c} REAL")
            for c in FIELDS
        )
        conn.execute(f"CREATE TABLE IF NOT EXISTS samples ({cols})")
        conn.commit()
        conn.close()

        self._thread = threading.Thread(target=self._loop, name="soak-sampler", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop_evt.set()
        if self._thread is not None:
            self._thread.join(timeout=5.0)

    def _loop(self) -> None:
        conn = sqlite3.connect(self.db_path)
        placeholders = ",".join("?" for _ in FIELDS)
        try:
            while not self._stop_evt.is_set():
                ts = time.time()
                rows = []
                for role, node in [("master", self.topo.master), ("replica", self.topo.replica)]:
                    if not node.is_alive():
                        continue
                    try:
                        c = node.connect(decode_responses=True, timeout=3.0)
                        mem = c.info("memory")
                        stats = c.info("stats")
                        repl = c.info("replication")
                        pers = c.info("persistence")
                        c.close()
                        used_mb = int(mem.get("used_memory", 0)) / (1024.0 * 1024.0)
                        rss_mb = int(mem.get("used_memory_rss", 0)) / (1024.0 * 1024.0)
                        frag = float(mem.get("mem_fragmentation_ratio", 0.0))
                        if role == "master":
                            self.max_used_mb = max(self.max_used_mb, used_mb)
                            self.max_frag_ratio = max(self.max_frag_ratio, frag)
                        off = int(
                            repl.get("master_repl_offset", 0)
                            if role == "master"
                            else repl.get("slave_repl_offset", 0)
                        )
                        rows.append(
                            (
                                ts,
                                role,
                                node.port,
                                round(used_mb, 3),
                                round(rss_mb, 3),
                                round(frag, 3),
                                int(mem.get("lazyfree_pending_objects", 0)),
                                int(stats.get("instantaneous_ops_per_sec", 0)),
                                int(stats.get("expired_keys", 0)),
                                int(stats.get("evicted_keys", 0)),
                                off,
                                int(stats.get("sync_full", 0)),
                                int(stats.get("sync_partial_ok", 0)),
                                int(pers.get("rdb_bgsave_in_progress", 0)),
                                int(pers.get("aof_rewrite_in_progress", 0)),
                            )
                        )
                    except Exception:
                        pass
                if rows:
                    with open(self.csv_path, "a", newline="", encoding="utf-8") as f:
                        csv.writer(f).writerows(rows)
                    conn.executemany(f"INSERT INTO samples VALUES ({placeholders})", rows)
                    conn.commit()
                    self.sample_count += len(rows)
                self._stop_evt.wait(self.cfg.sample_interval_sec)
        finally:
            conn.close()
