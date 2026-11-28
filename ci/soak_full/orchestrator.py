#!/usr/bin/env python3
"""Orchestrator for the Gemini Full-Stack Soak Test (Master + Replica, 3 modules).

Standalone entry point — intentionally NOT invoked by ci/run_tests.sh.
Use ci/run_full_soak.sh or `python3 -m ci.soak_full.orchestrator --mode <smoke|mini|full|marathon>`.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import random
import sys
import time
import traceback
from typing import List, Optional, Tuple

from .chaos.engine import ChaosEngine
from .config import ALL_CHAOS_PHASES, SoakConfig, preset_config, validate_config
from .monitor.sampler import TelemetrySampler
from .report.render import render_report
from .shadow.model import OpJournal, ShadowModel
from .topology import MasterReplicaTopology
from .verify.checkpoint import CheckpointResult, CheckpointVerifier
from .workload.bloom_wl import bootstrap_bloom
from .workload.json_wl import bootstrap_json
from .workload.noise_wl import bootstrap_noise
from .workload.runner import LatencyTracker, WorkerPool
from .workload.search_wl import bootstrap_search


def parse_args(argv: Optional[List[str]] = None) -> SoakConfig:
    p = argparse.ArgumentParser(description="Gemini Full-Stack Soak Test")
    p.add_argument(
        "--mode",
        choices=["smoke", "mini", "4g1h", "full", "marathon"],
        default="full",
        help="Preset scale & duration (default: full = 4GB / 24h)",
    )
    p.add_argument("--seed", type=int, default=None)
    p.add_argument("--duration", type=int, default=None, help="Override total duration (seconds)")
    p.add_argument("--maxmemory-mb", type=int, default=None, help="Override per-node maxmemory (MB)")
    p.add_argument("--workers", type=int, default=None, help="Master worker threads")
    p.add_argument("--replica-readers", type=int, default=None, help="Replica read worker threads")
    p.add_argument(
        "--phases",
        type=str,
        default=None,
        help=f"Comma-separated chaos phases (default: {','.join(ALL_CHAOS_PHASES)})",
    )
    p.add_argument("--build-dir", type=str, default="build", help="Directory containing redis_*.so")
    p.add_argument("--redis-server", type=str, default="redis-server")
    p.add_argument("--report-dir", type=str, default="")
    p.add_argument("--keep-on-fail", action="store_true", help="Keep workdir on failure for triage")
    args = p.parse_args(argv)

    cfg = preset_config(args.mode)
    if args.seed is not None:
        cfg.seed = args.seed
    if args.duration is not None:
        cfg.duration_sec = args.duration
    if args.maxmemory_mb is not None:
        cfg.maxmemory_mb = args.maxmemory_mb
        cfg.evict_drop_mb = max(32, int(args.maxmemory_mb * 0.75))
    if args.workers is not None:
        cfg.workers = args.workers
    if args.replica_readers is not None:
        cfg.replica_readers = args.replica_readers
    if args.phases is not None:
        cfg.phases = [s.strip() for s in args.phases.split(",") if s.strip()]
    cfg.build_dir = args.build_dir
    cfg.redis_server = args.redis_server
    cfg.keep_on_fail = args.keep_on_fail

    if args.report_dir:
        cfg.report_dir = os.path.abspath(args.report_dir)
    else:
        stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        cfg.report_dir = os.path.abspath(os.path.join("soak_reports", f"{cfg.mode}_{stamp}"))
    return cfg


def _freeze_and_collect(
    report_dir: str,
    topo: MasterReplicaTopology,
    journal: OpJournal,
    reason: str,
) -> None:
    try:
        journal.dump(os.path.join(report_dir, "op_journal_tail.log"), last_n=10000)
    except Exception:
        pass
    try:
        snap = topo.snapshot_info()
        with open(os.path.join(report_dir, "failure_info_snapshot.json"), "w", encoding="utf-8") as f:
            json.dump({"reason": reason, "nodes": snap}, f, indent=2, default=str)
    except Exception:
        pass
    for node in (topo.master, topo.replica):
        try:
            tail = node.read_log_tail(120)
            with open(os.path.join(report_dir, f"{node.name}_log_tail.txt"), "w", encoding="utf-8") as f:
                f.write(tail)
        except Exception:
            pass


def run_soak(cfg: SoakConfig) -> int:
    err = validate_config(cfg)
    if err:
        print(f"ERROR: {err}", file=sys.stderr)
        return 2

    os.makedirs(cfg.report_dir, exist_ok=True)
    rng = random.Random(cfg.seed)
    topo = MasterReplicaTopology(cfg)
    shadow = ShadowModel()
    journal = OpJournal()
    latencies = LatencyTracker()
    sampler = TelemetrySampler(cfg, topo, cfg.report_dir)

    phase_log: List[Tuple[str, float, str]] = []
    checkpoints: List[CheckpointResult] = []
    passed = True
    failure_reason: Optional[str] = None
    start_ts = time.time()
    deadline = start_ts + cfg.duration_sec

    print(
        f"[soak] starting mode={cfg.mode} seed={cfg.seed} maxmemory={cfg.maxmemory_mb}MB "
        f"duration={cfg.duration_sec}s workers={cfg.workers}+{cfg.replica_readers} "
        f"report_dir={cfg.report_dir}",
        flush=True,
    )

    pool: Optional[WorkerPool] = None
    try:
        # ── P0: Bootstrap topology & initial dataset ───────────────────────
        t0 = time.monotonic()
        topo.start()
        sampler.start()
        mc = topo.master.connect(decode_responses=False, timeout=30.0)
        try:
            bootstrap_bloom(mc, cfg, shadow, rng)
            bootstrap_json(mc, cfg, shadow, rng)
            bootstrap_search(mc, cfg, shadow, rng)
            bootstrap_noise(mc, cfg, shadow, rng)
        finally:
            mc.close()
        topo.wait_repl_synced(timeout_sec=60.0)
        phase_log.append(("P0_bootstrap", time.monotonic() - t0, f"shadow_keys={shadow.count()}"))
        print(f"[soak] P0_bootstrap done ({time.monotonic() - t0:.1f}s, keys={shadow.count()})", flush=True)

        pool = WorkerPool(cfg, topo, shadow, journal, latencies)
        verifier = CheckpointVerifier(cfg, topo, shadow, pool, rng)
        chaos = ChaosEngine(cfg, topo, shadow, journal, pool, rng)

        # Initial baseline checkpoint
        cp0 = verifier.run_checkpoint("P0_after_bootstrap", full_scan=False)
        checkpoints.append(cp0)
        if not cp0.passed:
            raise AssertionError(f"P0 checkpoint failed: {cp0.error}")
        b0 = verifier.record_baseline(start_ts, is_warmup=True)
        print(f"[soak] P0 checkpoint PASS (master={b0:.2f} MB)", flush=True)

        pool.start()

        # ── Main loop: [P1_steady -> shuffled chaos phases] × K rounds ────
        round_idx = 0
        while time.time() < deadline:
            round_idx += 1

            # P1: Steady mixed workload
            t_p1 = time.monotonic()
            steady_end = min(deadline, time.time() + cfg.steady_sec)
            while time.time() < steady_end:
                if pool.fatal_error is not None:
                    who, exc = pool.fatal_error
                    raise AssertionError(f"Fatal error in {who}: {exc}") from exc
                time.sleep(0.5)
            phase_log.append((f"R{round_idx}:P1_steady", time.monotonic() - t_p1, f"ops={latencies.total_ops()}"))
            print(f"[soak] R{round_idx}:P1_steady done (ops={latencies.total_ops()})", flush=True)

            # Shuffled chaos phases
            order = list(cfg.phases)
            rng.shuffle(order)
            for ph in order:
                if time.time() >= deadline and round_idx > 1:
                    break
                if pool.fatal_error is not None:
                    who, exc = pool.fatal_error
                    raise AssertionError(f"Fatal error in {who}: {exc}") from exc

                t_ph = time.monotonic()
                print(f"[soak] R{round_idx}:{ph} starting...", flush=True)
                detail = chaos.run_phase(ph, round_idx)
                dur_ph = time.monotonic() - t_ph
                phase_log.append((f"R{round_idx}:{ph}", dur_ph, detail))
                print(f"[soak] R{round_idx}:{ph} done ({dur_ph:.1f}s): {detail}", flush=True)

                cp = verifier.run_checkpoint(f"R{round_idx}:{ph}", full_scan=False)
                checkpoints.append(cp)
                if not cp.passed:
                    raise AssertionError(f"Checkpoint R{round_idx}:{ph} failed: {cp.error}")
                print(
                    f"[soak] checkpoint R{round_idx}:{ph} PASS "
                    f"(master={cp.master_used_mb:.1f}MB, replica={cp.replica_used_mb:.1f}MB)",
                    flush=True,
                )

            # Only record round baseline after a complete round (skip partial final round)
            if time.time() < deadline:
                base_mb = verifier.record_baseline(start_ts)
                ok_leak, slope = verifier.check_leak_regression()
                print(f"[soak] R{round_idx} baseline={base_mb:.2f}MB slope={slope:+.2f}MB/h", flush=True)
                if not ok_leak:
                    raise AssertionError(
                        f"I10: Memory leak detected: slope={slope:+.2f} MB/h > {cfg.leak_slope_mb_per_hour} MB/h"
                    )

            if cfg.mode == "smoke":
                break

        # ── P9: Final full checkpoint + cleanup memory reclamation check ───
        if pool.fatal_error is not None:
            who, exc = pool.fatal_error
            raise AssertionError(f"Fatal error in {who}: {exc}") from exc

        t_p9 = time.monotonic()
        cp_final = verifier.run_checkpoint("P9_final_full", full_scan=True)
        checkpoints.append(cp_final)
        if not cp_final.passed:
            raise AssertionError(f"P9 final checkpoint failed: {cp_final.error}")

        pool.stop()
        pool = None

        # Verify FLUSHALL reclaims memory cleanly and leaves 0 indices
        mc = topo.master.connect(decode_responses=True, timeout=20.0)
        try:
            mc.flushall()
            topo.wait_lazyfree_drained(timeout_sec=30.0)
            topo.wait_repl_synced(timeout_sec=30.0)
            rem_indices = mc.execute_command("FT._LIST")
            if rem_indices:
                raise AssertionError(f"P9: Indices remained after FLUSHALL: {rem_indices}")
            end_mb = int(mc.info("memory").get("used_memory", 0)) / (1024.0 * 1024.0)
        finally:
            mc.close()
        phase_log.append(("P9_final", time.monotonic() - t_p9, f"post_flush_mb={end_mb:.2f}"))
        print(f"[soak] P9_final PASS (post-FLUSHALL master={end_mb:.2f}MB)", flush=True)

    except BaseException as exc:
        passed = False
        failure_reason = f"{type(exc).__name__}: {exc}\n{traceback.format_exc()}"
        print(f"[soak] FAILED: {exc}", file=sys.stderr, flush=True)
        _freeze_and_collect(cfg.report_dir, topo, journal, failure_reason)
    finally:
        if pool is not None:
            pool.stop()
        sampler.stop()
        _, slope = (True, 0.0)
        try:
            _, slope = verifier.check_leak_regression()  # type: ignore[possibly-undefined]
        except Exception:
            pass
        report_path = render_report(
            report_dir=cfg.report_dir,
            cfg=cfg,
            passed=passed,
            elapsed_sec=time.time() - start_ts,
            total_ops=latencies.total_ops(),
            phase_log=phase_log,
            checkpoints=checkpoints,
            latencies=latencies.summary(),
            error_counts=latencies.error_counts(),
            max_used_mb=sampler.max_used_mb,
            max_frag_ratio=sampler.max_frag_ratio,
            leak_slope=slope,
            failure_reason=failure_reason,
        )
        print(f"[soak] report written to {report_path}", flush=True)
        if not (not passed and cfg.keep_on_fail):
            topo.stop()
        else:
            print(f"[soak] --keep-on-fail set; instances left in {topo.base_dir}", flush=True)

    return 0 if passed else 1


def main() -> int:
    cfg = parse_args()
    return run_soak(cfg)


if __name__ == "__main__":
    sys.exit(main())
