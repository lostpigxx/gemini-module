"""Thread-safe shadow model and ring-buffer operation journal."""

from __future__ import annotations

import collections
import dataclasses
import hashlib
import json
import struct
import threading
import time
from typing import Deque, Dict, List, Optional, Tuple


@dataclasses.dataclass
class KeyRecord:
    key: str
    module: str  # "json", "hash_idx", "json_idx", "vec", "bf", "cf", "cms", "topk", "tdigest", "noise"
    key_class: str  # "gold", "vol", "evc", "noise"
    version: int = 0
    digest: str = ""
    ttl_deadline: float = 0.0  # time.time() epoch seconds; 0 = no TTL
    meta: Dict[str, object] = dataclasses.field(default_factory=dict)


class OpJournal:
    """Bounded in-memory ring buffer of recent operations for failure triage."""

    def __init__(self, maxlen: int = 50000):
        self._buf: Deque[str] = collections.deque(maxlen=maxlen)
        self._lock = threading.Lock()

    def record(self, worker: str, cmd: str, key: str, detail: str = "") -> None:
        ts = time.time()
        entry = f"{ts:.3f} [{worker}] {cmd} {key} {detail}".rstrip()
        with self._lock:
            self._buf.append(entry)

    def dump(self, path: str, last_n: int = 10000) -> None:
        with self._lock:
            items = list(self._buf)[-last_n:]
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(items) + "\n")


class ShadowModel:
    """Tracks expected state for gold/vol/evc keys across all 3 modules."""

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._keys: Dict[str, KeyRecord] = {}
        self.allkeys_eviction_active: bool = False

    def put(
        self,
        key: str,
        module: str,
        key_class: str,
        version: int,
        digest: str = "",
        ttl_deadline: float = 0.0,
        meta: Optional[Dict[str, object]] = None,
    ) -> None:
        with self._lock:
            self._keys[key] = KeyRecord(
                key=key,
                module=module,
                key_class=key_class,
                version=version,
                digest=digest,
                ttl_deadline=ttl_deadline,
                meta=dict(meta) if meta else {},
            )

    def update_ttl(self, key: str, ttl_deadline: float) -> None:
        with self._lock:
            rec = self._keys.get(key)
            if rec is not None:
                rec.ttl_deadline = ttl_deadline

    def delete(self, key: str) -> None:
        with self._lock:
            self._keys.pop(key, None)

    def delete_classes(self, classes: Tuple[str, ...]) -> List[str]:
        removed: List[str] = []
        with self._lock:
            for k in list(self._keys.keys()):
                if self._keys[k].key_class in classes:
                    removed.append(k)
                    del self._keys[k]
        return removed

    def clear(self) -> None:
        with self._lock:
            self._keys.clear()

    def get(self, key: str) -> Optional[KeyRecord]:
        with self._lock:
            rec = self._keys.get(key)
            if rec is None:
                return None
            return KeyRecord(
                key=rec.key,
                module=rec.module,
                key_class=rec.key_class,
                version=rec.version,
                digest=rec.digest,
                ttl_deadline=rec.ttl_deadline,
                meta=dict(rec.meta),
            )

    def snapshot_by_module(self, module: str) -> List[KeyRecord]:
        with self._lock:
            return [
                KeyRecord(
                    key=r.key,
                    module=r.module,
                    key_class=r.key_class,
                    version=r.version,
                    digest=r.digest,
                    ttl_deadline=r.ttl_deadline,
                    meta=dict(r.meta),
                )
                for r in self._keys.values()
                if r.module == module
            ]

    def snapshot_by_class(self, key_class: str) -> List[KeyRecord]:
        with self._lock:
            return [
                KeyRecord(
                    key=r.key,
                    module=r.module,
                    key_class=r.key_class,
                    version=r.version,
                    digest=r.digest,
                    ttl_deadline=r.ttl_deadline,
                    meta=dict(r.meta),
                )
                for r in self._keys.values()
                if r.key_class == key_class
            ]

    def count(self) -> int:
        with self._lock:
            return len(self._keys)


def make_json_doc(key: str, version: int, extra_bytes: int = 0, depth: int = 0) -> dict:
    """Deterministically generate a JSON document with embedded version & key."""
    cats = ["alpha", "beta", "gamma", "delta", "epsilon"]
    words = ["fast", "redis", "search", "vector", "bloom", "json", "engine", "soak"]
    h = int(hashlib.blake2b(f"{key}:{version}".encode(), digest_size=8).hexdigest(), 16)
    doc: dict = {
        "_k": key,
        "_v": version,
        "price": (h % 10000) + 1,
        "tags": [cats[h % len(cats)], cats[(h >> 4) % len(cats)]],
        "title": f"{words[h % len(words)]} {words[(h >> 8) % len(words)]}",
        "loc": f"{((h % 200) - 100) / 10.0:.2f},{(((h >> 8) % 160) - 80) / 10.0:.2f}",
        "flag": bool(h & 1),
    }
    if extra_bytes > 0:
        doc["blob"] = ("x" * extra_bytes)
    if depth > 0:
        nested: dict = {"leaf": version}
        for d in range(depth):
            nested = {f"d{d}": nested}
        doc["deep"] = nested
    return doc


def digest_obj(obj: object) -> str:
    raw = json.dumps(obj, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.blake2b(raw, digest_size=12).hexdigest()


def make_vec_bytes(seed_key: str, version: int, dim: int) -> Tuple[bytes, List[float]]:
    """Deterministically generate a normalized float32 vector."""
    vals: List[float] = []
    norm_sq = 0.0
    for i in range(dim):
        h = hashlib.blake2b(f"{seed_key}:{version}:{i}".encode(), digest_size=4).digest()
        u = (struct.unpack("<I", h)[0] / 4294967295.0) * 2.0 - 1.0
        vals.append(u)
        norm_sq += u * u
    inv = (norm_sq ** -0.5) if norm_sq > 0 else 1.0
    vals = [v * inv for v in vals]
    return struct.pack(f"<{dim}f", *vals), vals
