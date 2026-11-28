"""Configuration presets and runtime parameters for the full-stack soak test."""

from __future__ import annotations

import dataclasses
import os
from typing import List, Optional


ALL_CHAOS_PHASES = [
    "P2_expire",
    "P3_evict",
    "P4_persist",
    "P5_repl",
    "P6_failover",
    "P7_admin",
    "P8_defrag",
]


@dataclasses.dataclass
class ScaleBudget:
    """Data volume scale for each preset."""

    json_plain_keys: int
    json_idx_keys: int
    hash_idx_keys: int
    vec_keys: int
    vec_dim: int
    bloom_keys: int
    large_bloom_count: int
    large_bloom_cap: int
    sketch_keys: int
    noise_keys: int
    large_json_bytes: int
    evict_fill_value_bytes: int


@dataclasses.dataclass
class SoakConfig:
    mode: str = "full"
    seed: int = 20261128
    duration_sec: int = 86400
    maxmemory_mb: int = 4096
    evict_drop_mb: int = 3072
    workers: int = 16
    replica_readers: int = 4
    sample_interval_sec: float = 5.0
    steady_sec: int = 1800
    phase_sec: int = 600
    phases: List[str] = dataclasses.field(default_factory=lambda: list(ALL_CHAOS_PHASES))
    build_dir: str = "build"
    redis_server: str = "redis-server"
    report_dir: str = ""
    keep_on_fail: bool = False
    verify_sample_size: int = 500
    leak_slope_mb_per_hour: float = 5.0
    budget: ScaleBudget = dataclasses.field(
        default_factory=lambda: ScaleBudget(
            json_plain_keys=20000,
            json_idx_keys=20000,
            hash_idx_keys=40000,
            vec_keys=10000,
            vec_dim=64,
            bloom_keys=4000,
            large_bloom_count=4,
            large_bloom_cap=2_000_000,
            sketch_keys=2000,
            noise_keys=5000,
            large_json_bytes=262144,
            evict_fill_value_bytes=65536,
        )
    )

    @property
    def json_so(self) -> str:
        return os.path.abspath(os.path.join(self.build_dir, "redis_json.so"))

    @property
    def bloom_so(self) -> str:
        return os.path.abspath(os.path.join(self.build_dir, "redis_bloom.so"))

    @property
    def search_so(self) -> str:
        return os.path.abspath(os.path.join(self.build_dir, "redis_search.so"))


def preset_config(mode: str) -> SoakConfig:
    """Return a SoakConfig tuned for smoke / mini / full / marathon."""
    if mode == "smoke":
        # Fast local end-to-end verification (~60-90s, 128MB) covering all phases
        return SoakConfig(
            mode="smoke",
            duration_sec=90,
            maxmemory_mb=128,
            evict_drop_mb=80,
            workers=4,
            replica_readers=2,
            sample_interval_sec=2.0,
            steady_sec=8,
            phase_sec=6,
            verify_sample_size=80,
            leak_slope_mb_per_hour=50.0,
            budget=ScaleBudget(
                json_plain_keys=120,
                json_idx_keys=120,
                hash_idx_keys=200,
                vec_keys=80,
                vec_dim=16,
                bloom_keys=40,
                large_bloom_count=1,
                large_bloom_cap=50_000,
                sketch_keys=20,
                noise_keys=60,
                large_json_bytes=16384,
                evict_fill_value_bytes=32768,
            ),
        )
    if mode == "mini":
        # 1GB / 1h PR-gating or ASan run
        return SoakConfig(
            mode="mini",
            duration_sec=3600,
            maxmemory_mb=1024,
            evict_drop_mb=768,
            workers=8,
            replica_readers=2,
            sample_interval_sec=5.0,
            steady_sec=300,
            phase_sec=180,
            verify_sample_size=250,
            leak_slope_mb_per_hour=10.0,
            budget=ScaleBudget(
                json_plain_keys=5000,
                json_idx_keys=5000,
                hash_idx_keys=10000,
                vec_keys=2000,
                vec_dim=32,
                bloom_keys=1000,
                large_bloom_count=2,
                large_bloom_cap=500_000,
                sketch_keys=500,
                noise_keys=2000,
                large_json_bytes=65536,
                evict_fill_value_bytes=65536,
            ),
        )
    if mode == "4g1h":
        # 4GB / 1h: full 4GB memory budget & dataset scale in a 1-hour window (~2-3 full rounds)
        cfg = preset_config("full")
        cfg.mode = "4g1h"
        cfg.duration_sec = 3600
        cfg.steady_sec = 300
        cfg.phase_sec = 180
        cfg.leak_slope_mb_per_hour = 10.0
        return cfg
    if mode == "marathon":
        cfg = preset_config("full")
        cfg.mode = "marathon"
        cfg.duration_sec = 72 * 3600
        return cfg
    # Default: "full" (4GB / 24h)
    return SoakConfig(mode="full")


def validate_config(cfg: SoakConfig) -> Optional[str]:
    for label, path in [
        ("redis_json.so", cfg.json_so),
        ("redis_bloom.so", cfg.bloom_so),
        ("redis_search.so", cfg.search_so),
    ]:
        if not os.path.isfile(path):
            return f"Missing module binary {label} at {path}"
    if cfg.maxmemory_mb < 32:
        return f"maxmemory_mb too small: {cfg.maxmemory_mb}"
    if cfg.workers < 1:
        return f"workers must be >= 1, got {cfg.workers}"
    return None
