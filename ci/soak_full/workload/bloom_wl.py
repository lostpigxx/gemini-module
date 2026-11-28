"""gemini-bloom workload: BF, CF, CMS, TopK, TDigest + SCANDUMP/LOADCHUNK."""

from __future__ import annotations

import random
import time
from typing import List

import redis

from ..config import SoakConfig
from ..shadow.model import OpJournal, ShadowModel


def bootstrap_bloom(
    r: redis.Redis,
    cfg: SoakConfig,
    shadow: ShadowModel,
    rng: random.Random,
) -> None:
    """Populate initial gold/vol/evc filters & sketches."""
    b = cfg.budget
    for i in range(b.bloom_keys):
        kclass = "gold" if i < b.bloom_keys // 4 else ("vol" if i < (b.bloom_keys * 3) // 4 else "evc")
        key = f"bf:{kclass}:{i}"
        cap = 200
        r.execute_command("BF.RESERVE", key, 0.01, cap, "EXPANSION", 2)
        items = [f"item@{i}:{j}" for j in range(15)]
        r.execute_command("BF.MADD", key, *items)
        ttl_dl = 0.0
        if kclass == "vol":
            ttl = rng.randint(60, 600)
            r.expire(key, ttl)
            ttl_dl = time.time() + ttl
        shadow.put(key, "bf", kclass, 1, ttl_deadline=ttl_dl, meta={"items": items[:5]})

    # Large filters (for lazyfree / eviction / defrag stress)
    for i in range(b.large_bloom_count):
        key = f"bf:evc:large:{i}"
        r.execute_command("BF.RESERVE", key, 0.01, b.large_bloom_cap, "NONSCALING")
        batch = [f"lg:{i}:{j}" for j in range(50)]
        r.execute_command("BF.MADD", key, *batch)
        shadow.put(key, "bf", "evc", 1, meta={"items": batch[:5], "large": True})

    # Cuckoo, CMS, TopK, TDigest
    per_sketch = max(4, b.sketch_keys // 4)
    for i in range(per_sketch):
        kclass = "gold" if i < per_sketch // 3 else "vol"
        # CF
        cfk = f"cf:{kclass}:{i}"
        r.execute_command("CF.RESERVE", cfk, 128, "BUCKETSIZE", 4, "EXPANSION", 2)
        cf_items = [f"cfi:{i}:{j}" for j in range(10)]
        r.execute_command("CF.INSERT", cfk, "ITEMS", *cf_items)
        shadow.put(cfk, "cf", kclass, 1, meta={"items": cf_items[:4]})
        # CMS
        cmsk = f"cms:{kclass}:{i}"
        r.execute_command("CMS.INITBYDIM", cmsk, 128, 4)
        r.execute_command("CMS.INCRBY", cmsk, "hot", 10, "warm", 3)
        shadow.put(cmsk, "cms", kclass, 1, meta={"hot": 10})
        # TopK
        tkk = f"topk:{kclass}:{i}"
        r.execute_command("TOPK.RESERVE", tkk, 5, 64, 4, 0.9)
        r.execute_command("TOPK.INCRBY", tkk, "elephant", 200, "mouse", 2)
        shadow.put(tkk, "topk", kclass, 1, meta={"top": "elephant"})
        # TDigest
        tdk = f"td:{kclass}:{i}"
        r.execute_command("TDIGEST.CREATE", tdk, "COMPRESSION", 100)
        vals = [str(v) for v in range(1, 51)]
        r.execute_command("TDIGEST.ADD", tdk, *vals)
        shadow.put(tdk, "tdigest", kclass, 1, meta={"min": 1.0, "max": 50.0})


def step_bloom(
    r: redis.Redis,
    cfg: SoakConfig,
    shadow: ShadowModel,
    journal: OpJournal,
    rng: random.Random,
    worker_id: int,
    num_workers: int,
) -> str:
    """Execute one randomized gemini-bloom operation and verify inline invariants."""
    b = cfg.budget
    choice = rng.randrange(10)
    idx = worker_id + rng.randrange(max(1, b.bloom_keys // max(1, num_workers))) * num_workers
    idx %= max(1, b.bloom_keys)
    kclass = "gold" if idx < b.bloom_keys // 4 else ("vol" if idx < (b.bloom_keys * 3) // 4 else "evc")

    if choice < 4:
        # BF write + exists invariant
        key = f"bf:{kclass}:{idx}"
        rec = shadow.get(key)
        existed = bool(r.exists(key))
        ver = (rec.version + 1) if rec else 1
        item = f"item@{idx}:v{ver}"
        if not existed:
            if rec is not None and kclass == "gold" and not shadow.allkeys_eviction_active:
                raise AssertionError(f"Gold BF key disappeared before write: {key}")
            r.execute_command("BF.RESERVE", key, 0.01, 200, "EXPANSION", 2)
        r.execute_command("BF.ADD", key, item)
        journal.record(f"w{worker_id}", "BF.ADD", key, item)
        # If a key was evicted/expired between EXISTS and BF.ADD, BF.ADD auto-created
        # a brand-new filter (card == 1), so discard pre-eviction shadow items.
        recreated = (not existed) or (
            (kclass != "gold" or shadow.allkeys_eviction_active)
            and r.execute_command("BF.CARD", key) <= 1
        )
        items: List[str] = [] if (recreated or rec is None) else list(rec.meta.get("items", []))
        if len(items) < 8:
            items.append(item)
        else:
            items[ver % len(items)] = item
        ttl_dl = 0.0 if (recreated or rec is None) else rec.ttl_deadline
        if kclass == "vol" and ttl_dl == 0.0:
            ttl = rng.randint(30, 300)
            r.expire(key, ttl)
            ttl_dl = time.time() + ttl
        shadow.put(key, "bf", kclass, ver, ttl_deadline=ttl_dl, meta={"items": items})
        # Inline zero-false-negative check (guard against vol/evc expiring between BF.ADD and BF.EXISTS)
        if r.execute_command("BF.EXISTS", key, item) != 1:
            if kclass == "gold" and not shadow.allkeys_eviction_active:
                raise AssertionError(f"BF false negative on gold key {key} for {item}")
            if r.exists(key):
                raise AssertionError(f"BF false negative on live key {key} for {item}")
            shadow.delete(key)
        return "bloom.bf_write"

    if choice == 4:
        # BF.SCANDUMP -> BF.LOADCHUNK round-trip on an ephemeral key
        key = f"bf:{kclass}:{idx}"
        clone = f"bf:evc:clone:{worker_id}"
        if r.exists(key):
            r.delete(clone)
            try:
                cursor = 0
                chunks = 0
                while True:
                    rep = r.execute_command("BF.SCANDUMP", key, cursor)
                    nxt, data = rep[0], rep[1]
                    if nxt == 0 and (data is None or len(data) == 0):
                        break
                    r.execute_command("BF.LOADCHUNK", clone, nxt, data)
                    cursor = nxt
                    chunks += 1
                if chunks > 0 and r.exists(key) and r.exists(clone):
                    orig_card = r.execute_command("BF.CARD", key)
                    clone_card = r.execute_command("BF.CARD", clone)
                    if orig_card != clone_card and r.exists(key) and r.exists(clone):
                        raise AssertionError(
                            f"BF SCANDUMP/LOADCHUNK card mismatch: {orig_card} != {clone_card}"
                        )
            finally:
                try:
                    r.delete(clone)
                except Exception:
                    pass
        return "bloom.bf_scandump"

    per_sketch = max(4, b.sketch_keys // 4)
    sidx = (worker_id + rng.randrange(max(1, per_sketch // max(1, num_workers))) * num_workers) % per_sketch
    skclass = "gold" if sidx < per_sketch // 3 else "vol"

    if choice == 5:
        # CF add / count / del
        cfk = f"cf:{skclass}:{sidx}"
        rec = shadow.get(cfk)
        existed = bool(r.exists(cfk))
        ver = (rec.version + 1) if rec else 1
        item = f"cfi:{sidx}:v{ver}"
        if not existed:
            r.execute_command("CF.RESERVE", cfk, 128, "BUCKETSIZE", 4, "EXPANSION", 2)
        r.execute_command("CF.ADD", cfk, item)
        if r.execute_command("CF.EXISTS", cfk, item) != 1:
            if skclass == "gold" and not shadow.allkeys_eviction_active:
                raise AssertionError(f"CF false negative on gold {cfk} for {item}")
            if r.exists(cfk):
                raise AssertionError(f"CF false negative on live {cfk} for {item}")
            shadow.delete(cfk)
            return "bloom.cf"
        items = list(rec.meta.get("items", [])) if (rec and existed) else []
        if len(items) < 6:
            items.append(item)
        shadow.put(cfk, "cf", skclass, ver, meta={"items": items})
        return "bloom.cf"

    if choice == 6:
        # CMS incrby + query (never undercounts)
        cmsk = f"cms:{skclass}:{sidx}"
        rec = shadow.get(cmsk)
        if not r.exists(cmsk):
            r.execute_command("CMS.INITBYDIM", cmsk, 128, 4)
            hot = 0
        else:
            hot = int(rec.meta.get("hot", 0)) if rec else 0
        delta = rng.randint(1, 5)
        r.execute_command("CMS.INCRBY", cmsk, "hot", delta)
        hot += delta
        q = int(r.execute_command("CMS.QUERY", cmsk, "hot")[0])
        if q < hot:
            raise AssertionError(f"CMS undercount on {cmsk}: got {q} < {hot}")
        shadow.put(cmsk, "cms", skclass, (rec.version + 1) if rec else 1, meta={"hot": hot})
        return "bloom.cms"

    if choice == 7:
        # TopK incrby + list
        tkk = f"topk:{skclass}:{sidx}"
        if not r.exists(tkk):
            r.execute_command("TOPK.RESERVE", tkk, 5, 64, 4, 0.9)
        r.execute_command("TOPK.INCRBY", tkk, "elephant", 10, f"noise:{rng.randrange(50)}", 1)
        if int(r.execute_command("TOPK.QUERY", tkk, "elephant")[0]) != 1:
            raise AssertionError(f"TopK lost heavy hitter 'elephant' on {tkk}")
        shadow.put(tkk, "topk", skclass, 1, meta={"top": "elephant"})
        return "bloom.topk"

    if choice == 8:
        # TDigest add + quantile monotonicity + min/max
        tdk = f"td:{skclass}:{sidx}"
        if not r.exists(tdk):
            r.execute_command("TDIGEST.CREATE", tdk, "COMPRESSION", 100)
            r.execute_command("TDIGEST.ADD", tdk, "1", "50")
        v = rng.uniform(1.0, 50.0)
        r.execute_command("TDIGEST.ADD", tdk, f"{v:.3f}")
        qs = [float(x) for x in r.execute_command("TDIGEST.QUANTILE", tdk, 0.1, 0.5, 0.9)]
        if not (qs[0] <= qs[1] + 1e-6 and qs[1] <= qs[2] + 1e-6):
            raise AssertionError(f"TDigest non-monotonic quantiles on {tdk}: {qs}")
        return "bloom.tdigest"

    # choice == 9: read-only check on BF
    key = f"bf:{kclass}:{idx}"
    rec = shadow.get(key)
    if rec and rec.meta.get("items") and r.exists(key):
        for it in rec.meta["items"]:
            if r.execute_command("BF.EXISTS", key, it) != 1:
                # Guard against vol/evc key expiring or being evicted between EXISTS and BF.EXISTS
                if kclass == "gold" and not shadow.allkeys_eviction_active:
                    raise AssertionError(f"BF read false negative on gold key {key} item={it}")
                if r.exists(key):
                    raise AssertionError(f"BF read false negative on live key {key} item={it}")
                shadow.delete(key)
                break
    return "bloom.read"
