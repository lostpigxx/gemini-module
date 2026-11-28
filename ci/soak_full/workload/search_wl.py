"""gemini-search workload: HASH/JSON/VECTOR indices, queries, cursors, hybrid, DDL."""

from __future__ import annotations

import random
import struct
import time
from typing import List

import redis

from ..config import SoakConfig
from ..shadow.model import OpJournal, ShadowModel, make_vec_bytes

CATS = ["alpha", "beta", "gamma", "delta", "epsilon"]
WORDS = ["fast", "redis", "search", "vector", "bloom", "json", "engine", "soak"]


def create_core_indices(r: redis.Redis, cfg: SoakConfig) -> None:
    """Create the permanent soak indices (idempotent if already present)."""
    existing = set(
        x.decode() if isinstance(x, bytes) else str(x)
        for x in r.execute_command("FT._LIST")
    )
    dim = cfg.budget.vec_dim

    if "idx_h" not in existing:
        r.execute_command(
            "FT.CREATE", "idx_h", "ON", "HASH", "PREFIX", "1", "hidx:",
            "SCHEMA",
            "cat", "TAG",
            "price", "NUMERIC", "SORTABLE",
            "title", "TEXT", "PHONETIC", "dm:en",
            "loc", "GEO",
        )
    if "idx_j" not in existing:
        r.execute_command(
            "FT.CREATE", "idx_j", "ON", "JSON", "PREFIX", "1", "jidx:",
            "SCHEMA",
            "$.tags[*]", "AS", "tags", "TAG",
            "$.price", "AS", "price", "NUMERIC", "SORTABLE",
            "$.title", "AS", "title", "TEXT",
            "$.loc", "AS", "loc", "GEO",
        )
    if "idx_v" not in existing:
        r.execute_command(
            "FT.CREATE", "idx_v", "ON", "HASH", "PREFIX", "1", "vec:",
            "SCHEMA",
            "cat", "TAG",
            "emb", "VECTOR", "HNSW", "DIM", str(dim), "DISTANCE_METRIC", "L2",
            "M", "16", "EF_CONSTRUCTION", "128",
        )
    if "idx_vflat" not in existing:
        r.execute_command(
            "FT.CREATE", "idx_vflat", "ON", "HASH", "PREFIX", "1", "vec:",
            "SCHEMA",
            "cat", "TAG",
            "emb", "VECTOR", "FLAT", "DIM", str(dim), "DISTANCE_METRIC", "L2",
        )
    # Alias + suggest + synonym + dict
    try:
        r.execute_command("FT.ALIASUPDATE", "alias_live", "idx_h")
    except Exception:
        pass
    r.execute_command("FT.SYNUPDATE", "idx_h", "g_fast", "fast", "quick", "rapid")
    r.execute_command("FT.SUGADD", "sug_soak", "redis search", 10.0, "PAYLOAD", "p1")
    r.execute_command("FT.DICTADD", "dict_soak", "gemini", "soaktest")


def bootstrap_search(
    r: redis.Redis,
    cfg: SoakConfig,
    shadow: ShadowModel,
    rng: random.Random,
) -> None:
    """Create indices & populate HASH and VECTOR documents (testing both incremental and initial scan)."""
    b = cfg.budget
    # Populate half of HASH docs BEFORE creating idx_h (exercises initial scan)
    pre_count = b.hash_idx_keys // 2
    for i in range(pre_count):
        _write_hash_doc(r, shadow, rng, i, b.hash_idx_keys, version=1)

    create_core_indices(r, cfg)

    # Populate remaining HASH docs AFTER creating idx_h (exercises keyspace notifications)
    for i in range(pre_count, b.hash_idx_keys):
        _write_hash_doc(r, shadow, rng, i, b.hash_idx_keys, version=1)

    # Populate VECTOR docs
    for i in range(b.vec_keys):
        _write_vec_doc(r, cfg, shadow, rng, i, version=1)


def _write_hash_doc(
    r: redis.Redis,
    shadow: ShadowModel,
    rng: random.Random,
    idx: int,
    total: int,
    version: int,
) -> str:
    kclass = "gold" if idx < total // 4 else ("vol" if idx < (total * 3) // 4 else "evc")
    key = f"hidx:{kclass}:{idx}"
    cat = CATS[(idx + version) % len(CATS)]
    price = ((idx * 37 + version * 13) % 10000) + 1
    title = f"{WORDS[(idx + version) % len(WORDS)]} {WORDS[(idx * 3 + version) % len(WORDS)]}"
    lon = ((idx % 200) - 100) / 10.0
    lat = (((idx * 7) % 160) - 80) / 10.0
    loc = f"{lon:.2f},{lat:.2f}"
    r.hset(
        key,
        mapping={
            "_k": key,
            "_v": str(version),
            "cat": cat,
            "price": str(price),
            "title": title,
            "loc": loc,
        },
    )
    ttl_dl = 0.0
    if kclass == "vol":
        ttl = rng.randint(60, 600)
        r.expire(key, ttl)
        ttl_dl = time.time() + ttl
    shadow.put(
        key,
        "hash_idx",
        kclass,
        version,
        ttl_deadline=ttl_dl,
        meta={"cat": cat, "price": price, "title": title},
    )
    return key


def _write_vec_doc(
    r: redis.Redis,
    cfg: SoakConfig,
    shadow: ShadowModel,
    rng: random.Random,
    idx: int,
    version: int,
) -> str:
    total = cfg.budget.vec_keys
    kclass = "gold" if idx < total // 4 else ("vol" if idx < (total * 3) // 4 else "evc")
    key = f"vec:{kclass}:{idx}"
    cat = CATS[(idx + version) % len(CATS)]
    vbytes, _ = make_vec_bytes(key, version, cfg.budget.vec_dim)
    r.hset(key, mapping={"_k": key, "_v": str(version), "cat": cat, "emb": vbytes})
    ttl_dl = 0.0
    if kclass == "vol":
        ttl = rng.randint(60, 600)
        r.expire(key, ttl)
        ttl_dl = time.time() + ttl
    shadow.put(key, "vec", kclass, version, ttl_deadline=ttl_dl, meta={"cat": cat})
    return key


def step_search_write(
    r: redis.Redis,
    cfg: SoakConfig,
    shadow: ShadowModel,
    journal: OpJournal,
    rng: random.Random,
    worker_id: int,
    num_workers: int,
) -> str:
    b = cfg.budget
    choice = rng.randrange(10)

    if choice < 4:
        # Upsert HASH indexed doc
        idx = (worker_id + rng.randrange(max(1, b.hash_idx_keys // max(1, num_workers))) * num_workers) % max(1, b.hash_idx_keys)
        kclass = "gold" if idx < b.hash_idx_keys // 4 else ("vol" if idx < (b.hash_idx_keys * 3) // 4 else "evc")
        key = f"hidx:{kclass}:{idx}"
        rec = shadow.get(key)
        ver = (rec.version + 1) if rec else 1
        _write_hash_doc(r, shadow, rng, idx, b.hash_idx_keys, ver)
        journal.record(f"w{worker_id}", "HSET", key, f"v={ver}")
        return "search.hset"

    if choice == 4:
        # Delete non-gold HASH doc
        idx = (worker_id + rng.randrange(max(1, b.hash_idx_keys // max(1, num_workers))) * num_workers) % max(1, b.hash_idx_keys)
        if idx >= b.hash_idx_keys // 4:
            kclass = "vol" if idx < (b.hash_idx_keys * 3) // 4 else "evc"
            key = f"hidx:{kclass}:{idx}"
            r.unlink(key)
            shadow.delete(key)
            journal.record(f"w{worker_id}", "UNLINK", key)
        return "search.unlink_hash"

    if choice < 7:
        # Upsert/delete VECTOR doc
        vidx = (worker_id + rng.randrange(max(1, b.vec_keys // max(1, num_workers))) * num_workers) % max(1, b.vec_keys)
        kclass = "gold" if vidx < b.vec_keys // 4 else ("vol" if vidx < (b.vec_keys * 3) // 4 else "evc")
        key = f"vec:{kclass}:{vidx}"
        if kclass != "gold" and rng.random() < 0.25:
            r.unlink(key)
            shadow.delete(key)
            return "search.unlink_vec"
        rec = shadow.get(key)
        ver = (rec.version + 1) if rec else 1
        _write_vec_doc(r, cfg, shadow, rng, vidx, ver)
        journal.record(f"w{worker_id}", "HSET_VEC", key, f"v={ver}")
        return "search.vec_upsert"

    # choice >= 7: read queries on master
    return step_search_read(r, cfg, rng)


def step_search_read(
    r: redis.Redis,
    cfg: SoakConfig,
    rng: random.Random,
) -> str:
    """Execute read-only search queries (safe on both Master and Replica)."""
    action = rng.randrange(8)
    cat = rng.choice(CATS)
    word = rng.choice(WORDS)

    if action == 0:
        r.execute_command(
            "FT.SEARCH", "idx_h", f"@cat:{{{cat}}} @price:[100 5000]",
            "SORTBY", "price", "DESC", "LIMIT", "0", "10",
        )
        return "search.ft_search_hash"

    if action == 1:
        r.execute_command(
            "FT.SEARCH", "alias_live", word,
            "WITHSCORES", "HIGHLIGHT", "LIMIT", "0", "10",
        )
        return "search.ft_search_text"

    if action == 2:
        r.execute_command(
            "FT.SEARCH", "idx_j", f"@tags:{{{cat}}}",
            "RETURN", "2", "price", "title", "LIMIT", "0", "10",
        )
        return "search.ft_search_json"

    if action == 3:
        # AGGREGATE + WITHCURSOR drain
        res = r.execute_command(
            "FT.AGGREGATE", "idx_h", "*",
            "GROUPBY", "1", "@cat", "REDUCE", "COUNT", "0", "AS", "cnt",
            "SORTBY", "2", "@cnt", "DESC",
            "WITHCURSOR", "COUNT", "2", "MAXIDLE", "5000",
        )
        if isinstance(res, (list, tuple)) and len(res) >= 2:
            cid = int(res[1])
            steps = 0
            while cid != 0 and steps < 20:
                res = r.execute_command("FT.CURSOR", "READ", "idx_h", str(cid), "COUNT", "2")
                cid = int(res[1]) if isinstance(res, (list, tuple)) and len(res) >= 2 else 0
                steps += 1
        return "search.aggregate_cursor"

    if action == 4:
        # HNSW KNN query (filtered & unfiltered)
        qbytes, _ = make_vec_bytes(f"q:{rng.randrange(1000)}", 1, cfg.budget.vec_dim)
        if rng.random() < 0.5:
            q = "*=>[KNN 5 @emb $q]"
        else:
            q = f"(@cat:{{{cat}}})=>[KNN 5 @emb $q]"
        r.execute_command(
            "FT.SEARCH", "idx_v", q,
            "PARAMS", "2", "q", qbytes, "NOCONTENT", "DIALECT", "2",
        )
        return "search.knn"

    if action == 5:
        # FT.HYBRID on idx_v / idx_h or FT.PROFILE
        r.execute_command("FT.PROFILE", "idx_h", "SEARCH", "QUERY", f"@cat:{{{cat}}}", "LIMIT", "0", "5")
        return "search.profile"

    if action == 6:
        r.execute_command("FT.TAGVALS", "idx_h", "cat")
        r.execute_command("FT.SUGGET", "sug_soak", "red", "WITHPAYLOADS")
        return "search.aux_read"

    # action == 7: FT.INFO
    info = r.execute_command("FT.INFO", "idx_h")
    if not info:
        raise AssertionError("FT.INFO idx_h returned empty")
    return "search.info"


def exercise_temp_index_lifecycle(r: redis.Redis, tag: str) -> None:
    """Create, ALTER, query, and DROP a temporary index (leak/DDL stress)."""
    idx_name = f"idx_tmp_{tag}"
    try:
        r.execute_command(
            "FT.CREATE", idx_name, "ON", "HASH", "PREFIX", "1", f"tmp:{tag}:",
            "SCHEMA", "a", "TAG",
        )
        r.hset(f"tmp:{tag}:1", mapping={"a": "x", "b": "42"})
        r.execute_command("FT.ALTER", idx_name, "SCHEMA", "ADD", "b", "NUMERIC")
        r.hset(f"tmp:{tag}:2", mapping={"a": "y", "b": "99"})
        res = r.execute_command("FT.SEARCH", idx_name, "@b:[1 100]", "NOCONTENT")
        if int(res[0]) < 1:
            raise AssertionError(f"FT.ALTER numeric field not searchable on {idx_name}: {res}")
    finally:
        try:
            r.execute_command("FT.DROPINDEX", idx_name, "DD")
        except Exception:
            pass
