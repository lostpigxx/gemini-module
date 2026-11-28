"""Chaos Engine executing phases P2_expire through P8_defrag."""

from __future__ import annotations

import json
import random
import struct
import time
from typing import List

import redis

from ..config import SoakConfig
from ..shadow.model import OpJournal, ShadowModel
from ..topology import MasterReplicaTopology
from ..workload.runner import WorkerPool
from ..workload.search_wl import exercise_temp_index_lifecycle


EVICTION_POLICIES = [
    "noeviction",
    "volatile-lru",
    "volatile-lfu",
    "volatile-ttl",
    "volatile-random",
    "allkeys-lru",
    "allkeys-lfu",
    "allkeys-random",
]


class ChaosEngine:
    def __init__(
        self,
        cfg: SoakConfig,
        topo: MasterReplicaTopology,
        shadow: ShadowModel,
        journal: OpJournal,
        pool: WorkerPool,
        rng: random.Random,
    ):
        self.cfg = cfg
        self.topo = topo
        self.shadow = shadow
        self.journal = journal
        self.pool = pool
        self.rng = rng
        self._evict_round = 0

    def run_phase(self, phase: str, round_idx: int) -> str:
        handlers = {
            "P2_expire": self._p2_expire,
            "P3_evict": self._p3_evict,
            "P4_persist": self._p4_persist,
            "P5_repl": self._p5_repl,
            "P6_failover": self._p6_failover,
            "P7_admin": self._p7_admin,
            "P8_defrag": self._p8_defrag,
        }
        fn = handlers.get(phase)
        if fn is None:
            raise ValueError(f"Unknown chaos phase: {phase}")
        return fn(round_idx)

    # ── P2: Expiration (E1..E7) ────────────────────────────────────────────
    def _p2_expire(self, round_idx: int) -> str:
        mc = self.topo.master.connect(decode_responses=False, timeout=15.0)
        rc = self.topo.replica.connect(decode_responses=False, timeout=15.0)
        try:
            # E1: Active expiration storm with hz=100
            mc.config_set("hz", "100")
            storm_n = 200 if self.cfg.mode == "smoke" else 2000
            pipe = mc.pipeline(transaction=False)
            for i in range(storm_n):
                k = f"hidx:vol:expstorm:{round_idx}:{i}"
                pipe.hset(k, mapping={"cat": "alpha", "price": "10", "title": "storm"})
                pipe.pexpire(k, 300)
                jk = f"jidx:vol:expstorm:{round_idx}:{i}"
                pipe.execute_command("JSON.SET", jk, "$", '{"tags":["alpha"],"price":10,"title":"storm"}')
                pipe.pexpire(jk, 300)
                bk = f"bf:vol:expstorm:{round_idx}:{i}"
                pipe.execute_command("BF.ADD", bk, f"v{i}")
                pipe.pexpire(bk, 300)
            pipe.execute()
            time.sleep(0.6)
            mc.config_set("hz", "10")

            # E2 & E5: Strict logical expiration on Master AND Replica (active expire OFF)
            mc.execute_command("DEBUG", "SET-ACTIVE-EXPIRE", "0")
            try:
                k_live = f"hidx:vol:lexp_live:{round_idx}"
                k_dead = f"hidx:vol:lexp_dead:{round_idx}"
                k_pers = f"hidx:vol:lexp_pers:{round_idx}"
                tag = f"lexp_{round_idx}"
                mc.hset(k_live, mapping={"cat": tag, "price": "1", "title": "live"})
                mc.hset(k_dead, mapping={"cat": tag, "price": "2", "title": "dead"})
                mc.hset(k_pers, mapping={"cat": tag, "price": "3", "title": "pers"})
                mc.pexpire(k_dead, 200)
                mc.pexpire(k_pers, 200)
                mc.persist(k_pers)

                # Wait for replica to sync before TTL elapses
                self.topo.wait_repl_synced(timeout_sec=10.0)
                time.sleep(0.35)

                # Master strict assertion
                m_res = mc.execute_command("FT.SEARCH", "idx_h", f"@cat:{{{tag}}}", "NOCONTENT")
                if int(m_res[0]) != 2:
                    raise AssertionError(f"E2 Master returned logically expired doc: {m_res}")

                # Replica strict assertion (before Master sends DEL)
                r_res = rc.execute_command("FT.SEARCH", "idx_h", f"@cat:{{{tag}}}", "NOCONTENT")
                if int(r_res[0]) != 2:
                    raise AssertionError(f"E5 Replica returned logically expired doc: {r_res}")

                # E3: JSON.SET preserves existing TTL
                jt = f"json:vol:ttlkeep:{round_idx}"
                mc.execute_command("JSON.SET", jt, "$", '{"a":1}')
                mc.expire(jt, 120)
                mc.execute_command("JSON.SET", jt, "$.a", "2")
                if mc.ttl(jt) <= 0:
                    raise AssertionError(f"E3 JSON.SET cleared TTL on {jt}")
                mc.unlink(k_live, k_dead, k_pers, jt)
            finally:
                mc.execute_command("DEBUG", "SET-ACTIVE-EXPIRE", "1")

            # E7: Same-name key isolation with EXPIRE/DEL
            mc.set("idx_h", "soak_user_key", ex=1)
            if mc.get("idx_h") != b"soak_user_key":
                raise AssertionError("E7 SET idx_h failed")
            mc.delete("idx_h")
            if not mc.execute_command("FT.INFO", "idx_h"):
                raise AssertionError("E7 DEL idx_h corrupted index")
            return f"storm={storm_n*3} keys, E2/E5 strict expiry OK"
        finally:
            mc.close()
            rc.close()

    # ── P3: Eviction (8 policies + maxmemory pressure) ─────────────────────
    def _p3_evict(self, round_idx: int) -> str:
        mc = self.topo.master.connect(decode_responses=False, timeout=15.0)
        # Rotate through 2-3 policies per round (or all 8 across rounds)
        policies_this_round: List[str] = []
        step_n = 3 if self.cfg.mode == "smoke" else 4
        for _ in range(step_n):
            policies_this_round.append(EVICTION_POLICIES[self._evict_round % len(EVICTION_POLICIES)])
            self._evict_round += 1

        try:
            orig_mm = f"{self.cfg.maxmemory_mb}mb"
            drop_mm = f"{self.cfg.evict_drop_mb}mb"
            fill_val = b"E" * self.cfg.budget.evict_fill_value_bytes
            evicted_before = int(mc.info("stats").get("evicted_keys", 0))

            for pol in policies_this_round:
                mc.config_set("maxmemory-policy", pol)
                self.shadow.allkeys_eviction_active = pol.startswith("allkeys-")
                if pol == "noeviction":
                    # Lower maxmemory temporarily to trigger OOM on writes, verify reads still work
                    used = int(mc.info("memory").get("used_memory", 0))
                    tight_mb = max(16, int(used // (1024 * 1024)))
                    mc.config_set("maxmemory", f"{tight_mb}mb")
                    oom_seen = False
                    for i in range(60):
                        try:
                            mc.set(f"evc:oom:{round_idx}:{i}", fill_val)
                        except redis.ResponseError as e:
                            if "OOM" in str(e) or "maxmemory" in str(e):
                                oom_seen = True
                                break
                            raise
                    # Readonly commands must still succeed under noeviction OOM
                    mc.execute_command("FT.SEARCH", "idx_h", "*", "LIMIT", "0", "1", "NOCONTENT")
                    mc.config_set("maxmemory", orig_mm)
                    # Cleanup any OOM filler keys
                    pipe = mc.pipeline(transaction=False)
                    for i in range(60):
                        pipe.unlink(f"evc:oom:{round_idx}:{i}")
                    pipe.execute()
                    self.journal.record("chaos", "EVICT_NOEVICT", pol, f"oom_seen={oom_seen}")
                else:
                    used_mb = max(16, int(mc.info("memory").get("used_memory", 0)) // (1024 * 1024))
                    headroom = 8 if self.cfg.mode == "smoke" else 128
                    target_mb = min(self.cfg.evict_drop_mb, used_mb + headroom)
                    mc.config_set("maxmemory", f"{target_mb}mb")
                    mc.config_set("maxmemory-eviction-tenacity", "100")
                    # Fill evictable data to push used_memory past target_mb and trigger real evictions
                    fill_iters = 600 if self.cfg.mode == "smoke" else 4000
                    deadline = time.monotonic() + max(2.0, self.cfg.phase_sec / max(1, len(policies_this_round)))
                    for i in range(fill_iters):
                        if time.monotonic() >= deadline:
                            break
                        k = f"evc:fill:{round_idx}:{pol}:{i}"
                        try:
                            mc.set(k, fill_val, ex=120)
                        except redis.ResponseError:
                            break
                    # Verify search index definition survived allkeys-* eviction!
                    info = mc.execute_command("FT.INFO", "idx_h")
                    if not info:
                        raise AssertionError(f"idx_h lost under eviction policy {pol}")
                    mc.config_set("maxmemory-eviction-tenacity", "10")
                    mc.config_set("maxmemory", orig_mm)

            # Reset to default volatile-lru and remove temporary eviction filler keys
            mc.config_set("maxmemory-policy", "volatile-lru")
            mc.config_set("maxmemory", orig_mm)
            cursor = 0
            while True:
                cursor, keys = mc.scan(cursor=cursor, match=f"evc:fill:{round_idx}:*", count=500)
                if keys:
                    mc.unlink(*keys)
                if cursor == 0:
                    break
            self.topo.wait_lazyfree_drained(self.topo.master)
            # If allkeys-* ran this round, reconcile shadow for gold keys legitimately evicted
            if any(p.startswith("allkeys-") for p in policies_this_round):
                self.pool.pause()
                try:
                    gold_recs = self.shadow.snapshot_by_class("gold")
                    for i in range(0, len(gold_recs), 500):
                        batch = gold_recs[i : i + 500]
                        pipe = mc.pipeline(transaction=False)
                        for rec in batch:
                            pipe.exists(rec.key)
                        for rec, ex in zip(batch, pipe.execute()):
                            if not ex:
                                self.shadow.delete(rec.key)
                finally:
                    self.shadow.allkeys_eviction_active = False
                    self.pool.resume()
            else:
                self.shadow.allkeys_eviction_active = False
            evicted_after = int(mc.info("stats").get("evicted_keys", 0))
            return f"policies={policies_this_round}, evicted_delta={evicted_after - evicted_before}"
        finally:
            try:
                mc.config_set("maxmemory-policy", "volatile-lru")
                mc.config_set("maxmemory", f"{self.cfg.maxmemory_mb}mb")
                mc.config_set("maxmemory-eviction-tenacity", "10")
            except Exception:
                pass
            self.shadow.allkeys_eviction_active = False
            mc.close()

    # ── P4: Persistence (BGSAVE, BGREWRITEAOF, DEBUG RELOAD, DUMP/RESTORE) ─
    def _p4_persist(self, round_idx: int) -> str:
        with self.topo.fork_lock:
            self.topo.wait_no_fork_in_progress()
            mc = self.topo.master.connect(decode_responses=False, timeout=30.0)
            try:
                # PS1: BGSAVE under live traffic
                mc.bgsave()
                self.topo.wait_no_fork_in_progress()

                # PS2: BGREWRITEAOF (with RDB preamble -> exercises AuxSaveSearch)
                mc.bgrewriteaof()
                self.topo.wait_no_fork_in_progress()

                # PS8: DUMP / RESTORE round-trip for Bloom + JSON + indexed HASH
                test_pairs = [
                    (f"bf:evc:dr_src:{round_idx}", f"bf:evc:dr_dst:{round_idx}", "bf"),
                    (f"json:evc:dr_src:{round_idx}", f"json:evc:dr_dst:{round_idx}", "json"),
                    (f"hidx:evc:dr_src:{round_idx}", f"hidx:evc:dr_dst:{round_idx}", "hash"),
                ]
                mc.execute_command("BF.ADD", test_pairs[0][0], "dr_item")
                mc.execute_command("JSON.SET", test_pairs[1][0], "$", '{"dr":42}')
                mc.hset(test_pairs[2][0], mapping={"cat": "alpha", "price": "77", "title": "restored"})
                for src, dst, kind in test_pairs:
                    blob = mc.dump(src)
                    if not blob:
                        raise AssertionError(f"PS8 DUMP returned empty for {src}")
                    mc.restore(dst, 60000, blob, replace=True)
                    if kind == "bf" and mc.execute_command("BF.EXISTS", dst, "dr_item") != 1:
                        raise AssertionError("PS8 BF RESTORE lost item")
                    if kind == "json" and json.loads(mc.execute_command("JSON.GET", dst, "$"))[0]["dr"] != 42:
                        raise AssertionError("PS8 JSON RESTORE mismatch")
                mc.unlink(*[k for pair in test_pairs for k in pair[:2]])

                # PS3: DEBUG RELOAD on master (pause workers briefly to avoid spurious disconnect).
                # Note: DEBUG RELOAD silently drops expired keys locally without sending DEL to
                # replicas, so force a replica resync afterward to keep replication consistent.
                self.pool.pause()
                self.pool.allow_transient_disconnect = True
                try:
                    self.topo.wait_no_fork_in_progress()
                    mc.execute_command("DEBUG", "RELOAD")
                    # Verify search index survived DEBUG RELOAD via AUX v3
                    if not mc.execute_command("FT.INFO", "idx_h"):
                        raise AssertionError("PS3 idx_h missing after DEBUG RELOAD")
                    rc = self.topo.replica.connect(decode_responses=True, timeout=15.0)
                    try:
                        rc.replicaof("NO", "ONE")
                        time.sleep(0.2)
                        rc.replicaof("127.0.0.1", self.topo.master.port)
                    finally:
                        rc.close()
                    self.topo.wait_replica_online(timeout_sec=60.0)
                    self.topo.wait_no_fork_in_progress()
                finally:
                    self.pool.allow_transient_disconnect = False
                    self.pool.resume()

                return "BGSAVE + BGREWRITEAOF + DUMP/RESTORE + DEBUG RELOAD OK"
            finally:
                mc.close()

    # ── P5: Replication (partial sync + diskless/disk full sync) ───────────
    def _p5_repl(self, round_idx: int) -> str:
        with self.topo.fork_lock:
            self.topo.wait_no_fork_in_progress()
            mc = self.topo.master.connect(decode_responses=True, timeout=15.0)
            rc = self.topo.replica.connect(decode_responses=True, timeout=15.0)
            try:
                # RP3: Partial resync via CLIENT KILL TYPE replica
                p_before = int(mc.info("stats").get("sync_partial_ok", 0))
                mc.execute_command("CLIENT", "KILL", "TYPE", "replica")
                self.topo.wait_replica_online(timeout_sec=30.0)
                p_after = int(mc.info("stats").get("sync_partial_ok", 0))

                # RP1 / RP2: Full sync alternating diskless=yes (swapdb) vs diskless=no
                diskless = "yes" if (round_idx % 2 == 0) else "no"
                mc.config_set("repl-diskless-sync", diskless)
                rc.config_set("repl-diskless-load", "swapdb" if diskless == "yes" else "disabled")
                rc.replicaof("NO", "ONE")
                time.sleep(0.3)
                rc.replicaof("127.0.0.1", self.topo.master.port)
                self.topo.wait_replica_online(timeout_sec=60.0)
                self.topo.wait_no_fork_in_progress()
                return f"partial_ok_delta={p_after - p_before}, full_sync(diskless={diskless}) OK"
            finally:
                mc.close()
                rc.close()

    # ── P6: Failover (Master <-> Replica role swap) ────────────────────────
    def _p6_failover(self, round_idx: int) -> str:
        with self.topo.fork_lock:
            self.topo.wait_no_fork_in_progress()
            self.pool.pause()
            self.pool.allow_transient_disconnect = True
            try:
                self.topo.wait_repl_synced(timeout_sec=30.0)
                old_master = self.topo.master
                old_replica = self.topo.replica

                # Promote replica and demote old master
                rc = old_replica.connect(decode_responses=True, timeout=15.0)
                rc.replicaof("NO", "ONE")
                rc.close()

                mc = old_master.connect(decode_responses=True, timeout=15.0)
                mc.replicaof("127.0.0.1", old_replica.port)
                mc.close()

                self.topo.swap_roles()
                self.topo.wait_replica_online(timeout_sec=45.0)

                # Verify new master is writable & search index is intact
                nmc = self.topo.master.connect(decode_responses=True, timeout=15.0)
                try:
                    nmc.set(f"noise:failover:{round_idx}", "ok", ex=60)
                    if not nmc.execute_command("FT.INFO", "idx_h"):
                        raise AssertionError("P6 idx_h missing on promoted master")
                finally:
                    nmc.close()
                return f"swapped master -> {self.topo.master.name}:{self.topo.master.port}"
            finally:
                self.pool.allow_transient_disconnect = False
                self.pool.resume()

    # ── P7: Admin & Keyspace ops (RENAME, COPY, UNLINK, DDL churn, PAUSE) ──
    def _p7_admin(self, round_idx: int) -> str:
        mc = self.topo.master.connect(decode_responses=False, timeout=15.0)
        try:
            # AD3: RENAME indexed -> non-indexed -> indexed
            k1 = f"hidx:evc:ren_a:{round_idx}"
            k2 = f"other:evc:ren_b:{round_idx}"
            k3 = f"hidx:evc:ren_c:{round_idx}"
            tag = f"ren_{round_idx}"
            mc.hset(k1, mapping={"cat": tag, "price": "50", "title": "rename"})
            mc.rename(k1, k2)
            if int(mc.execute_command("FT.SEARCH", "idx_h", f"@cat:{{{tag}}}", "NOCONTENT")[0]) != 0:
                raise AssertionError("AD3 RENAME out of prefix left stale doc in idx_h")
            mc.rename(k2, k3)
            if int(mc.execute_command("FT.SEARCH", "idx_h", f"@cat:{{{tag}}}", "NOCONTENT")[0]) != 1:
                raise AssertionError("AD3 RENAME into prefix did not index doc")
            mc.unlink(k3)

            # AD4: COPY deep-copy for Bloom & JSON
            bsrc, bdst = f"bf:evc:cp_s:{round_idx}", f"bf:evc:cp_d:{round_idx}"
            mc.execute_command("BF.ADD", bsrc, "orig")
            mc.copy(bsrc, bdst, replace=True)
            mc.execute_command("BF.ADD", bdst, "only_dst")
            if mc.execute_command("BF.EXISTS", bsrc, "only_dst") != 0:
                raise AssertionError("AD4 BF COPY is not independent")
            jsrc, jdst = f"json:evc:cp_s:{round_idx}", f"json:evc:cp_d:{round_idx}"
            mc.execute_command("JSON.SET", jsrc, "$", '{"x":1}')
            mc.copy(jsrc, jdst, replace=True)
            mc.execute_command("JSON.SET", jdst, "$.x", "2")
            if json.loads(mc.execute_command("JSON.GET", jsrc, "$"))[0]["x"] != 1:
                raise AssertionError("AD4 JSON COPY is not independent")
            mc.unlink(bsrc, bdst, jsrc, jdst)

            # Temp index CREATE + ALTER + DROPINDEX DD churn
            for t in range(3):
                exercise_temp_index_lifecycle(mc, f"r{round_idx}_{t}")

            # AD7: Brief CLIENT PAUSE WRITE
            mc.execute_command("CLIENT", "PAUSE", "200", "WRITE")
            time.sleep(0.25)
            return "RENAME + COPY + DDL churn + CLIENT PAUSE OK"
        finally:
            mc.close()

    # ── P8: Defrag & Memory Purge ──────────────────────────────────────────
    def _p8_defrag(self, round_idx: int) -> str:
        mc = self.topo.master.connect(decode_responses=False, timeout=15.0)
        try:
            n = 300 if self.cfg.mode == "smoke" else 3000
            pipe = mc.pipeline(transaction=False)
            for i in range(n):
                pipe.execute_command("JSON.SET", f"json:evc:frag:{round_idx}:{i}", "$", json.dumps({"s": "a" * (32 + (i % 512))}))
            pipe.execute()
            # Delete 50% to create holes
            pipe = mc.pipeline(transaction=False)
            for i in range(0, n, 2):
                pipe.unlink(f"json:evc:frag:{round_idx}:{i}")
            pipe.execute()
            if self.topo.master.is_jemalloc:
                try:
                    mc.execute_command("MEMORY", "PURGE")
                except Exception:
                    pass
            # Clean remaining frag keys
            pipe = mc.pipeline(transaction=False)
            for i in range(1, n, 2):
                pipe.unlink(f"json:evc:frag:{round_idx}:{i}")
            pipe.execute()
            self.topo.wait_lazyfree_drained(self.topo.master)
            frag = float(mc.info("memory").get("mem_fragmentation_ratio", 0.0))
            return f"frag_ratio={frag:.2f}, jemalloc={self.topo.master.is_jemalloc}"
        finally:
            mc.close()
