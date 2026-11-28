"""Concurrent worker pool (write workers + replica read workers) with pause/resume."""

from __future__ import annotations

import collections
import random
import threading
import time
from typing import Dict, List, Optional, Tuple

import redis

from ..config import SoakConfig
from ..shadow.model import OpJournal, ShadowModel
from ..topology import MasterReplicaTopology
from .bloom_wl import step_bloom
from .json_wl import step_json
from .noise_wl import step_noise
from .search_wl import step_search_read, step_search_write


# Errors that are expected during specific chaos phases (OOM, LOADING, failover/restart)
BENIGN_ERROR_SUBSTRINGS = (
    "OOM command not allowed",
    "command not allowed when used memory > 'maxmemory'",
    "LOADING Redis is loading",
    "READONLY You can't write against a read only replica",
    "non scaling filter is full",
    "filter expansion failed",
    "cursor not found",
    "Cursor not found",
    "not found",
    "does not exist",
    "BUSY",
)


class LatencyTracker:
    """Lightweight reservoir-based latency tracker per command family."""

    def __init__(self, max_samples: int = 4096):
        self._lock = threading.Lock()
        self._max = max_samples
        self._samples: Dict[str, List[float]] = collections.defaultdict(list)
        self._counts: Dict[str, int] = collections.Counter()
        self._errors: Dict[str, int] = collections.Counter()

    def record(self, family: str, elapsed_ms: float) -> None:
        with self._lock:
            self._counts[family] += 1
            buf = self._samples[family]
            if len(buf) < self._max:
                buf.append(elapsed_ms)
            else:
                idx = self._counts[family] % self._max
                buf[idx] = elapsed_ms

    def record_error(self, label: str) -> None:
        with self._lock:
            self._errors[label] += 1

    def total_ops(self) -> int:
        with self._lock:
            return sum(self._counts.values())

    def summary(self) -> Dict[str, Dict[str, float]]:
        out: Dict[str, Dict[str, float]] = {}
        with self._lock:
            for fam, buf in sorted(self._samples.items()):
                if not buf:
                    continue
                s = sorted(buf)
                n = len(s)
                out[fam] = {
                    "count": float(self._counts[fam]),
                    "p50_ms": s[int(n * 0.50)],
                    "p99_ms": s[min(n - 1, int(n * 0.99))],
                    "max_ms": s[-1],
                }
        return out

    def error_counts(self) -> Dict[str, int]:
        with self._lock:
            return dict(self._errors)


class WorkerPool:
    """Runs concurrent master write/read workers and replica read-only workers."""

    def __init__(
        self,
        cfg: SoakConfig,
        topo: MasterReplicaTopology,
        shadow: ShadowModel,
        journal: OpJournal,
        latencies: LatencyTracker,
    ):
        self.cfg = cfg
        self.topo = topo
        self.shadow = shadow
        self.journal = journal
        self.latencies = latencies

        self._stop_evt = threading.Event()
        self._run_evt = threading.Event()
        self._run_evt.set()
        self._pause_ack_lock = threading.Lock()
        self._in_flight = 0
        self._all_idle = threading.Condition(self._pause_ack_lock)

        self._threads: List[threading.Thread] = []
        self._fatal_lock = threading.Lock()
        self.fatal_error: Optional[Tuple[str, BaseException]] = None
        self.allow_transient_disconnect: bool = False

    def start(self) -> None:
        for wid in range(self.cfg.workers):
            proto = 3 if (wid % 2 == 0) else 2
            t = threading.Thread(
                target=self._master_worker,
                args=(wid, proto),
                name=f"soak-w{wid}-r{proto}",
                daemon=True,
            )
            self._threads.append(t)
            t.start()

        for rid in range(self.cfg.replica_readers):
            proto = 3 if (rid % 2 == 0) else 2
            t = threading.Thread(
                target=self._replica_worker,
                args=(rid, proto),
                name=f"soak-rep{rid}-r{proto}",
                daemon=True,
            )
            self._threads.append(t)
            t.start()

    def pause(self, timeout_sec: float = 15.0) -> None:
        self._run_evt.clear()
        deadline = time.monotonic() + timeout_sec
        with self._all_idle:
            while self._in_flight > 0:
                rem = deadline - time.monotonic()
                if rem <= 0:
                    break
                self._all_idle.wait(timeout=min(0.2, rem))

    def resume(self) -> None:
        self._run_evt.set()

    def stop(self) -> None:
        self._stop_evt.set()
        self._run_evt.set()
        for t in self._threads:
            t.join(timeout=5.0)

    def _record_fatal(self, who: str, exc: BaseException) -> None:
        with self._fatal_lock:
            if self.fatal_error is None:
                self.fatal_error = (who, exc)
        self._stop_evt.set()

    def _is_benign_error(self, exc: Exception) -> bool:
        if isinstance(exc, (redis.ConnectionError, redis.TimeoutError)):
            return self.allow_transient_disconnect
        msg = str(exc)
        return any(sub in msg for sub in BENIGN_ERROR_SUBSTRINGS)

    def _master_worker(self, wid: int, proto: int) -> None:
        rng = random.Random(self.cfg.seed + wid * 7919)
        conn: Optional[redis.Redis] = None
        cur_port = -1

        while not self._stop_evt.is_set():
            if not self._run_evt.is_set():
                time.sleep(0.05)
                continue

            with self._pause_ack_lock:
                if not self._run_evt.is_set():
                    continue
                self._in_flight += 1

            try:
                target_port = self.topo.master.port
                if conn is None or cur_port != target_port:
                    if conn is not None:
                        try:
                            conn.close()
                        except Exception:
                            pass
                    conn = self.topo.master.connect(protocol=proto, decode_responses=False, timeout=10.0)
                    cur_port = target_port

                t0 = time.monotonic()
                r = rng.random()
                if r < 0.28:
                    fam = step_json(conn, self.cfg, self.shadow, self.journal, rng, wid, self.cfg.workers)
                elif r < 0.62:
                    fam = step_search_write(conn, self.cfg, self.shadow, self.journal, rng, wid, self.cfg.workers)
                elif r < 0.88:
                    fam = step_bloom(conn, self.cfg, self.shadow, self.journal, rng, wid, self.cfg.workers)
                else:
                    fam = step_noise(conn, self.cfg, self.shadow, rng, wid, self.cfg.workers)
                self.latencies.record(fam, (time.monotonic() - t0) * 1000.0)

            except AssertionError as ae:
                self._record_fatal(f"worker-{wid}", ae)
                break
            except Exception as exc:
                if self._is_benign_error(exc):
                    self.latencies.record_error(type(exc).__name__)
                    if isinstance(exc, (redis.ConnectionError, redis.TimeoutError)):
                        conn = None
                        time.sleep(0.1)
                else:
                    self._record_fatal(f"worker-{wid}", exc)
                    break
            finally:
                with self._pause_ack_lock:
                    self._in_flight -= 1
                    if self._in_flight == 0:
                        self._all_idle.notify_all()

        if conn is not None:
            try:
                conn.close()
            except Exception:
                pass

    def _replica_worker(self, rid: int, proto: int) -> None:
        rng = random.Random(self.cfg.seed + 10000 + rid * 104729)
        conn: Optional[redis.Redis] = None
        cur_port = -1

        while not self._stop_evt.is_set():
            if not self._run_evt.is_set():
                time.sleep(0.05)
                continue

            with self._pause_ack_lock:
                if not self._run_evt.is_set():
                    continue
                self._in_flight += 1

            try:
                target_port = self.topo.replica.port
                if conn is None or cur_port != target_port:
                    if conn is not None:
                        try:
                            conn.close()
                        except Exception:
                            pass
                    conn = self.topo.replica.connect(protocol=proto, decode_responses=False, timeout=10.0)
                    cur_port = target_port

                t0 = time.monotonic()
                fam = "replica." + step_search_read(conn, self.cfg, rng)
                self.latencies.record(fam, (time.monotonic() - t0) * 1000.0)
            except AssertionError as ae:
                self._record_fatal(f"replica-reader-{rid}", ae)
                break
            except Exception as exc:
                # Replica may temporarily be loading/reconnecting during replication/failover chaos
                if self._is_benign_error(exc) or isinstance(exc, (redis.ConnectionError, redis.TimeoutError)):
                    self.latencies.record_error(f"replica.{type(exc).__name__}")
                    conn = None
                    time.sleep(0.15)
                else:
                    self._record_fatal(f"replica-reader-{rid}", exc)
                    break
            finally:
                with self._pause_ack_lock:
                    self._in_flight -= 1
                    if self._in_flight == 0:
                        self._all_idle.notify_all()

        if conn is not None:
            try:
                conn.close()
            except Exception:
                pass
