"""Native Redis keyspace noise workload (competes for eviction/expiration)."""

from __future__ import annotations

import random
import time

import redis

from ..config import SoakConfig
from ..shadow.model import ShadowModel


def bootstrap_noise(
    r: redis.Redis,
    cfg: SoakConfig,
    shadow: ShadowModel,
    rng: random.Random,
) -> None:
    for i in range(cfg.budget.noise_keys):
        key = f"noise:str:{i}"
        val = f"n:{i}:" + ("z" * 128)
        r.set(key, val)
        ttl_dl = 0.0
        if i % 2 == 0:
            ttl = rng.randint(60, 600)
            r.expire(key, ttl)
            ttl_dl = time.time() + ttl
        shadow.put(key, "noise", "noise", 1, ttl_deadline=ttl_dl)


def step_noise(
    r: redis.Redis,
    cfg: SoakConfig,
    shadow: ShadowModel,
    rng: random.Random,
    worker_id: int,
    num_workers: int,
) -> str:
    n = max(1, cfg.budget.noise_keys)
    idx = (worker_id + rng.randrange(max(1, n // max(1, num_workers))) * num_workers) % n
    kind = idx % 3
    if kind == 0:
        key = f"noise:str:{idx}"
        r.set(key, f"v:{rng.randrange(10000)}:" + ("a" * rng.randint(64, 512)))
        if rng.random() < 0.6:
            r.expire(key, rng.randint(30, 300))
        shadow.put(key, "noise", "noise", 1)
        return "noise.set"
    if kind == 1:
        key = f"noise:zset:{idx}"
        r.zadd(key, {f"m{rng.randrange(20)}": rng.random() * 100.0})
        if rng.random() < 0.5:
            r.expire(key, rng.randint(30, 300))
        shadow.put(key, "noise", "noise", 1)
        return "noise.zadd"
    key = f"noise:list:{idx}"
    r.lpush(key, f"el:{rng.randrange(1000)}")
    r.ltrim(key, 0, 15)
    shadow.put(key, "noise", "noise", 1)
    return "noise.lpush"
