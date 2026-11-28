"""gemini-json workload: full JSON.* commands, large docs, deep nesting, Lua/MULTI."""

from __future__ import annotations

import json
import random
import time

import redis

from ..config import SoakConfig
from ..shadow.model import OpJournal, ShadowModel, digest_obj, make_json_doc


def bootstrap_json(
    r: redis.Redis,
    cfg: SoakConfig,
    shadow: ShadowModel,
    rng: random.Random,
) -> None:
    b = cfg.budget
    for i in range(b.json_plain_keys):
        kclass = "gold" if i < b.json_plain_keys // 4 else ("vol" if i < (b.json_plain_keys * 3) // 4 else "evc")
        key = f"json:{kclass}:{i}"
        extra = b.large_json_bytes if (i % 100 == 0 and kclass != "gold") else 0
        depth = 40 if (i % 97 == 1) else 0
        doc = make_json_doc(key, 1, extra_bytes=extra, depth=depth)
        r.execute_command("JSON.SET", key, "$", json.dumps(doc))
        ttl_dl = 0.0
        if kclass == "vol":
            ttl = rng.randint(60, 600)
            r.expire(key, ttl)
            ttl_dl = time.time() + ttl
        shadow.put(key, "json", kclass, 1, digest=digest_obj(doc), ttl_deadline=ttl_dl)

    # JSON keys indexed by gemini-search (prefix jidx:)
    for i in range(b.json_idx_keys):
        kclass = "gold" if i < b.json_idx_keys // 4 else ("vol" if i < (b.json_idx_keys * 3) // 4 else "evc")
        key = f"jidx:{kclass}:{i}"
        doc = make_json_doc(key, 1)
        r.execute_command("JSON.SET", key, "$", json.dumps(doc))
        ttl_dl = 0.0
        if kclass == "vol":
            ttl = rng.randint(60, 600)
            r.expire(key, ttl)
            ttl_dl = time.time() + ttl
        shadow.put(
            key,
            "json_idx",
            kclass,
            1,
            digest=digest_obj(doc),
            ttl_deadline=ttl_dl,
            meta={"tags": doc["tags"], "price": doc["price"]},
        )


def step_json(
    r: redis.Redis,
    cfg: SoakConfig,
    shadow: ShadowModel,
    journal: OpJournal,
    rng: random.Random,
    worker_id: int,
    num_workers: int,
) -> str:
    b = cfg.budget
    indexed = rng.random() < 0.5
    total = b.json_idx_keys if indexed else b.json_plain_keys
    idx = (worker_id + rng.randrange(max(1, total // max(1, num_workers))) * num_workers) % max(1, total)
    kclass = "gold" if idx < total // 4 else ("vol" if idx < (total * 3) // 4 else "evc")
    prefix = "jidx" if indexed else "json"
    module = "json_idx" if indexed else "json"
    key = f"{prefix}:{kclass}:{idx}"

    action = rng.randrange(10)
    rec = shadow.get(key)

    if action < 3 or rec is None:
        ver = (rec.version + 1) if rec else 1
        extra = b.large_json_bytes if (not indexed and kclass == "evc" and rng.random() < 0.03) else 0
        depth = 48 if (not indexed and rng.random() < 0.02) else 0
        doc = make_json_doc(key, ver, extra_bytes=extra, depth=depth)
        r.execute_command("JSON.SET", key, "$", json.dumps(doc))
        journal.record(f"w{worker_id}", "JSON.SET", key, f"v={ver}")
        ttl_dl = rec.ttl_deadline if rec else 0.0
        if kclass == "vol" and ttl_dl == 0.0:
            ttl = rng.randint(30, 300)
            r.expire(key, ttl)
            ttl_dl = time.time() + ttl
        meta = {"tags": doc["tags"], "price": doc["price"]} if indexed else {}
        shadow.put(key, module, kclass, ver, digest=digest_obj(doc), ttl_deadline=ttl_dl, meta=meta)
        return "json.set"

    if action == 3:
        # JSON.NUMINCRBY + JSON.TOGGLE + JSON.ARRAPPEND + JSON.ARRTRIM in MULTI/EXEC
        raw = r.execute_command("JSON.GET", key, "$")
        if raw is None:
            if kclass == "gold" and not shadow.allkeys_eviction_active:
                raise AssertionError(f"Gold JSON key disappeared: {key}")
            shadow.delete(key)
            return "json.miss"
        doc = json.loads(raw)[0]
        delta = rng.randint(1, 15)
        new_ver = doc["_v"] + 1
        pipe = r.pipeline(transaction=True)
        pipe.execute_command("JSON.NUMINCRBY", key, "$.price", str(delta))
        pipe.execute_command("JSON.SET", key, "$._v", str(new_ver))
        pipe.execute_command("JSON.TOGGLE", key, "$.flag")
        pipe.execute_command("JSON.ARRAPPEND", key, "$.tags", json.dumps("gamma"))
        pipe.execute_command("JSON.ARRTRIM", key, "$.tags", "0", "3")
        pipe.execute()
        doc["price"] += delta
        doc["_v"] = new_ver
        doc["flag"] = not doc["flag"]
        doc["tags"] = (doc["tags"] + ["gamma"])[:4]
        meta = {"tags": doc["tags"], "price": doc["price"]} if indexed else {}
        shadow.put(key, module, kclass, new_ver, digest=digest_obj(doc), ttl_deadline=rec.ttl_deadline, meta=meta)
        journal.record(f"w{worker_id}", "JSON.MULTI", key, f"v={new_ver}")
        return "json.multi_mutate"

    if action == 4:
        # JSON.MERGE
        raw = r.execute_command("JSON.GET", key, "$")
        if raw is None:
            shadow.delete(key)
            return "json.miss"
        doc = json.loads(raw)[0]
        new_ver = doc["_v"] + 1
        # Include an indexed field ('price') so even if a vol key expires between JSON.GET
        # and JSON.MERGE, the recreated doc still satisfies idx_j's schema.
        patch = {"_v": new_ver, "price": doc["price"], "flag": True}
        r.execute_command("JSON.MERGE", key, "$", json.dumps(patch))
        doc["_v"] = new_ver
        doc["flag"] = True
        meta = {"tags": doc["tags"], "price": doc["price"]} if indexed else {}
        shadow.put(key, module, kclass, new_ver, digest=digest_obj(doc), ttl_deadline=rec.ttl_deadline, meta=meta)
        return "json.merge"

    if action == 5:
        # Lua script calling JSON.SET (exercises effects replication)
        ver = rec.version + 1
        doc = make_json_doc(key, ver)
        lua = "return redis.call('JSON.SET', KEYS[1], '$', ARGV[1])"
        r.eval(lua, 1, key, json.dumps(doc))
        meta = {"tags": doc["tags"], "price": doc["price"]} if indexed else {}
        shadow.put(key, module, kclass, ver, digest=digest_obj(doc), ttl_deadline=rec.ttl_deadline, meta=meta)
        return "json.lua_set"

    if action == 6 and kclass != "gold":
        r.unlink(key)
        shadow.delete(key)
        journal.record(f"w{worker_id}", "UNLINK", key)
        return "json.unlink"

    # action >= 7: read & verify version/digest
    raw = r.execute_command("JSON.GET", key, "$")
    if raw is None:
        if kclass == "gold" and not shadow.allkeys_eviction_active:
            raise AssertionError(f"Gold JSON key missing on read: {key}")
        shadow.delete(key)
        return "json.read_miss"
    got = json.loads(raw)[0]
    if got.get("_k") != key:
        raise AssertionError(f"Corrupted JSON _k on {key}: got={got.get('_k')}")
    if got.get("_v") != rec.version:
        raise AssertionError(f"Stale/mismatched JSON _v on {key}: got={got.get('_v')} expected={rec.version}")
    if digest_obj(got) != rec.digest:
        raise AssertionError(f"JSON digest mismatch on {key}")
    return "json.read"
