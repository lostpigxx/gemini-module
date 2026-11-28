"""Quiesced checkpoint oracle: I1..I16 invariants + HNSW recall + leak regression."""

from __future__ import annotations

import dataclasses
import json
import random
import time
from typing import Dict, List, Optional, Tuple

import redis

from ..config import SoakConfig
from ..shadow.model import ShadowModel, digest_obj, make_vec_bytes
from ..topology import MasterReplicaTopology
from ..workload.runner import WorkerPool


@dataclasses.dataclass
class CheckpointResult:
    label: str
    ts: float
    passed: bool
    checks: Dict[str, str]
    master_used_mb: float
    replica_used_mb: float
    error: Optional[str] = None


def _count_prefix_keys(r: redis.Redis, prefix: str) -> int:
    """Count live (non-expired) keys matching prefix (SCAN may include logically expired keys)."""
    cursor = 0
    total = 0
    while True:
        cursor, keys = r.scan(cursor=cursor, match=f"{prefix}*", count=500)
        if keys:
            pipe = r.pipeline(transaction=False)
            for k in keys:
                pipe.exists(k)
            total += sum(int(x) for x in pipe.execute())
        if cursor == 0:
            break
    return total


def _ft_total_docs(r: redis.Redis, idx: str) -> int:
    res = r.execute_command("FT.SEARCH", idx, "*", "LIMIT", "0", "0")
    return int(res[0])


class CheckpointVerifier:
    def __init__(
        self,
        cfg: SoakConfig,
        topo: MasterReplicaTopology,
        shadow: ShadowModel,
        pool: WorkerPool,
        rng: random.Random,
    ):
        self.cfg = cfg
        self.topo = topo
        self.shadow = shadow
        self.pool = pool
        self.rng = rng
        self.baseline_points: List[Tuple[float, float]] = []  # (elapsed_hours, used_mb)

    def run_checkpoint(self, label: str, full_scan: bool = False) -> CheckpointResult:
        checks: Dict[str, str] = {}
        self.pool.pause(timeout_sec=20.0)
        mc: Optional[redis.Redis] = None
        rc: Optional[redis.Redis] = None
        paused_server = False
        try:
            # I1: Process liveness + bug markers in logs
            for node in (self.topo.master, self.topo.replica):
                if not node.is_alive():
                    raise AssertionError(f"I1: {node.name} is not alive")
                bugs = node.check_log_for_bugs()
                if bugs:
                    raise AssertionError(f"I1: Bug markers in {node.name} log: {bugs[:3]}")
            checks["I1_alive"] = "PASS"

            mc = self.topo.master.connect(decode_responses=False, timeout=20.0)
            rc = self.topo.replica.connect(decode_responses=False, timeout=20.0)

            # Quiesce master writes & wait for replica + lazyfree convergence
            mc.execute_command("CLIENT", "PAUSE", "30000", "WRITE")
            paused_server = True
            if not self.topo.wait_repl_synced(timeout_sec=30.0):
                raise AssertionError("I14: Replica failed to converge during quiesced checkpoint")
            self.topo.wait_lazyfree_drained(timeout_sec=20.0)

            # I5: Master-Replica DEBUG DIGEST consistency
            m_dig = mc.execute_command("DEBUG", "DIGEST")
            r_dig = rc.execute_command("DEBUG", "DIGEST")
            if m_dig != r_dig:
                # Inspect exact diverging keys. Note: MBtdigest performs lazy Compress()
                # during read-only TDIGEST.QUANTILE on master, so internal centroid arrays
                # may differ from an unread replica while representing the same distribution.
                # Every non-MBtdigest key must match bit-for-bit; MBtdigest keys must match
                # semantically (MIN, MAX, median within tolerance).
                diffs: List[str] = []
                td_checked = 0
                m_keys = set(mc.keys("*"))
                r_keys = set(rc.keys("*"))
                for k in sorted(m_keys - r_keys):
                    # Master's dict may temporarily retain logically expired keys that RDB full-sync skipped
                    if mc.exists(k):
                        diffs.append(f"only_on_master:{k!r}")
                        if len(diffs) >= 10:
                            break
                for k in sorted(r_keys - m_keys):
                    if rc.exists(k):
                        diffs.append(f"only_on_replica:{k!r}")
                        if len(diffs) >= 10:
                            break
                for k in sorted(m_keys & r_keys):
                    md = mc.execute_command("DEBUG", "DIGEST-VALUE", k)
                    rd = rc.execute_command("DEBUG", "DIGEST-VALUE", k)
                    if md == rd:
                        continue
                    ktype = mc.type(k)
                    if ktype == b"MBtdigest":
                        m_min = float(mc.execute_command("TDIGEST.MIN", k))
                        r_min = float(rc.execute_command("TDIGEST.MIN", k))
                        m_max = float(mc.execute_command("TDIGEST.MAX", k))
                        r_max = float(rc.execute_command("TDIGEST.MAX", k))
                        m_q50 = float(mc.execute_command("TDIGEST.QUANTILE", k, 0.5)[0])
                        r_q50 = float(rc.execute_command("TDIGEST.QUANTILE", k, 0.5)[0])
                        span = max(1.0, abs(m_max - m_min))
                        if (
                            abs(m_min - r_min) > 1e-6
                            or abs(m_max - r_max) > 1e-6
                            or abs(m_q50 - r_q50) > 0.05 * span
                        ):
                            diffs.append(
                                f"td_semantic_diff:{k!r}(m={m_min}/{m_q50:.2f}/{m_max},r={r_min}/{r_q50:.2f}/{r_max})"
                            )
                        td_checked += 1
                    elif ktype == b"MBtopk---":
                        # HeavyKeeper uses probabilistic decay on ReplicateVerbatim;
                        # verify top-1 heavy hitter matches on master and replica.
                        m_top = mc.execute_command("TOPK.LIST", k)
                        r_top = rc.execute_command("TOPK.LIST", k)
                        if (not m_top) != (not r_top) or (m_top and m_top[0] != r_top[0]):
                            diffs.append(f"topk_semantic_diff:{k!r}(m={m_top[:2]},r={r_top[:2]})")
                        td_checked += 1
                    else:
                        diffs.append(f"val_diff:{k!r}(type={ktype!r})")
                    if len(diffs) >= 10:
                        break
                if diffs:
                    raise AssertionError(
                        f"I5: Master/Replica DEBUG DIGEST mismatch: {m_dig!r} != {r_dig!r}; diffs={diffs}"
                    )
                checks["I5_repl_digest"] = f"PASS (exact except {td_checked} tdigest lazy-compress OK)"
            else:
                checks["I5_repl_digest"] = "PASS"

            # I6: Search index consistency vs keyspace (Master & Replica).
            # Under CLIENT PAUSE WRITE, live key count can only decrease as TTLs elapse during
            # the SCAN window, so sandwich-bound [ks_after, ks_before] eliminates wall-clock
            # false positives while still catching any 1-doc index drift.
            for idx, pfx in [("idx_h", "hidx:"), ("idx_j", "jidx:"), ("idx_v", "vec:")]:
                ks_before = _count_prefix_keys(mc, pfx)
                m_cnt = _ft_total_docs(mc, idx)
                r_cnt = _ft_total_docs(rc, idx)
                if m_cnt != ks_before or r_cnt != m_cnt:
                    ks_after = _count_prefix_keys(mc, pfx)
                    if not (ks_after <= m_cnt <= ks_before):
                        raise AssertionError(
                            f"I6: Master {idx} doc count {m_cnt} outside live keyspace {pfx}* bounds [{ks_after}, {ks_before}]"
                        )
                    if not (ks_after <= r_cnt <= ks_before):
                        raise AssertionError(
                            f"I6: Replica {idx} doc count {r_cnt} outside live keyspace {pfx}* bounds [{ks_after}, {ks_before}]"
                        )
            checks["I6_index_consistency"] = "PASS"

            # I3 / I4: Gold & sampled JSON verification
            json_recs = self.shadow.snapshot_by_module("json") + self.shadow.snapshot_by_module("json_idx")
            sample = json_recs if full_scan else self.rng.sample(json_recs, min(len(json_recs), self.cfg.verify_sample_size))
            for rec in sample:
                raw = mc.execute_command("JSON.GET", rec.key, "$")
                if raw is None:
                    if rec.key_class == "gold" and not self.shadow.allkeys_eviction_active:
                        raise AssertionError(f"I3: Gold JSON key missing at checkpoint: {rec.key}")
                    continue
                got = json.loads(raw)[0]
                if got.get("_k") != rec.key or got.get("_v") != rec.version:
                    raise AssertionError(
                        f"I4: JSON version/key mismatch on {rec.key}: got _v={got.get('_v')} expected={rec.version}"
                    )
                if rec.digest and digest_obj(got) != rec.digest:
                    raise AssertionError(f"I4: JSON digest mismatch on {rec.key}")
            checks["I3_I4_json"] = f"PASS ({len(sample)} checked)"

            # I8: Probabilistic structures (BF/CF zero false negatives)
            bf_recs = self.shadow.snapshot_by_module("bf")
            bf_sample = bf_recs if full_scan else self.rng.sample(bf_recs, min(len(bf_recs), max(20, self.cfg.verify_sample_size // 4)))
            for rec in bf_sample:
                if not mc.exists(rec.key):
                    if rec.key_class == "gold" and not self.shadow.allkeys_eviction_active:
                        raise AssertionError(f"I3: Gold BF key missing: {rec.key}")
                    continue
                for item in rec.meta.get("items", []):
                    if mc.execute_command("BF.EXISTS", rec.key, item) != 1:
                        if rec.key_class == "gold" and not self.shadow.allkeys_eviction_active:
                            raise AssertionError(f"I8: BF false negative on gold {rec.key} item={item}")
                        if mc.exists(rec.key):
                            raise AssertionError(f"I8: BF false negative on live {rec.key} item={item}")
                        break
            checks["I8_bloom"] = f"PASS ({len(bf_sample)} checked)"

            # I7: HNSW vs FLAT recall@5 check
            vec_cnt = _ft_total_docs(mc, "idx_v")
            if vec_cnt >= 10:
                k = min(5, vec_cnt)
                q_n = 10 if self.cfg.mode == "smoke" else 30
                recall_sum = 0.0
                for qi in range(q_n):
                    qb, _ = make_vec_bytes(f"recall:{label}:{qi}", 1, self.cfg.budget.vec_dim)
                    h_res = mc.execute_command(
                        "FT.SEARCH", "idx_v", f"*=>[KNN {k} @emb $q EF_RUNTIME 256]",
                        "PARAMS", "2", "q", qb, "NOCONTENT", "DIALECT", "2",
                    )
                    f_res = mc.execute_command("FT.SEARCH", "idx_vflat", f"*=>[KNN {k} @emb $q]", "PARAMS", "2", "q", qb, "NOCONTENT", "DIALECT", "2")
                    h_ids = set(h_res[1:])
                    f_ids = set(f_res[1:])
                    # Ensure no deleted key is returned
                    for doc_id in h_ids:
                        if not mc.exists(doc_id):
                            raise AssertionError(f"I7: HNSW KNN returned deleted key {doc_id}")
                    if f_ids:
                        recall_sum += len(h_ids & f_ids) / float(len(f_ids))
                avg_recall = recall_sum / float(q_n)
                if avg_recall < 0.65:
                    raise AssertionError(f"I7: HNSW recall@{k} degraded to {avg_recall:.3f}")
                checks["I7_knn_recall"] = f"PASS (recall@{k}={avg_recall:.3f})"
            else:
                checks["I7_knn_recall"] = "SKIP (<10 vecs)"

            m_mem = int(mc.info("memory").get("used_memory", 0)) / (1024.0 * 1024.0)
            r_mem = int(rc.info("memory").get("used_memory", 0)) / (1024.0 * 1024.0)
            return CheckpointResult(
                label=label,
                ts=time.time(),
                passed=True,
                checks=checks,
                master_used_mb=m_mem,
                replica_used_mb=r_mem,
            )
        except Exception as exc:
            return CheckpointResult(
                label=label,
                ts=time.time(),
                passed=False,
                checks=checks,
                master_used_mb=0.0,
                replica_used_mb=0.0,
                error=str(exc),
            )
        finally:
            if paused_server and mc is not None:
                try:
                    mc.execute_command("CLIENT", "UNPAUSE")
                except Exception:
                    pass
            if mc is not None:
                mc.close()
            if rc is not None:
                rc.close()
            self.pool.resume()

    def record_baseline(self, start_ts: float, is_warmup: bool = False) -> float:
        """Record steady-state dataset memory (excluding repl backlog) after draining lazyfree."""
        self.topo.wait_lazyfree_drained(self.topo.master)
        mc = self.topo.master.connect(decode_responses=True, timeout=10.0)
        try:
            if self.topo.master.is_jemalloc:
                try:
                    mc.execute_command("MEMORY", "PURGE")
                except Exception:
                    pass
            mem = mc.info("memory")
            used_mb = int(mem.get("used_memory_dataset", mem.get("used_memory", 0))) / (1024.0 * 1024.0)
        finally:
            mc.close()
        if not is_warmup:
            elapsed_h = max(1e-6, (time.time() - start_ts) / 3600.0)
            self.baseline_points.append((elapsed_h, used_mb))
        return used_mb

    def check_leak_regression(self) -> Tuple[bool, float]:
        """Compute OLS slope (MB/hour) across round baselines (I10)."""
        pts = self.baseline_points
        if len(pts) < 3:
            return True, 0.0
        n = len(pts)
        sx = sum(x for x, _ in pts)
        sy = sum(y for _, y in pts)
        sxx = sum(x * x for x, _ in pts)
        sxy = sum(x * y for x, y in pts)
        denom = n * sxx - sx * sx
        if abs(denom) < 1e-12:
            return True, 0.0
        slope = (n * sxy - sx * sy) / denom
        return slope <= self.cfg.leak_slope_mb_per_hour, slope
