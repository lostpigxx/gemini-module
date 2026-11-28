"""Render report.md from soak phase history, checkpoints, latencies, and telemetry."""

from __future__ import annotations

import os
from typing import Dict, List, Optional, Tuple

from ..config import SoakConfig
from ..verify.checkpoint import CheckpointResult


def render_report(
    report_dir: str,
    cfg: SoakConfig,
    passed: bool,
    elapsed_sec: float,
    total_ops: int,
    phase_log: List[Tuple[str, float, str]],
    checkpoints: List[CheckpointResult],
    latencies: Dict[str, Dict[str, float]],
    error_counts: Dict[str, int],
    max_used_mb: float,
    max_frag_ratio: float,
    leak_slope: float,
    failure_reason: Optional[str] = None,
) -> str:
    path = os.path.join(report_dir, "report.md")
    status = "PASS" if passed else "FAIL"
    lines: List[str] = [
        f"# Gemini Full-Stack Soak Report — {status}",
        "",
        "## Summary",
        "",
        "| Field | Value |",
        "|---|---|",
        f"| Mode | `{cfg.mode}` |",
        f"| Seed | `{cfg.seed}` |",
        f"| Maxmemory | `{cfg.maxmemory_mb} MB` |",
        f"| Workers | `{cfg.workers}` master + `{cfg.replica_readers}` replica |",
        f"| Elapsed | `{elapsed_sec:.1f}s` |",
        f"| Total Ops | `{total_ops}` |",
        f"| Peak Master `used_memory` | `{max_used_mb:.2f} MB` |",
        f"| Peak `mem_fragmentation_ratio` | `{max_frag_ratio:.2f}` |",
        f"| Memory Baseline Slope | `{leak_slope:+.2f} MB/h` (limit `{cfg.leak_slope_mb_per_hour} MB/h`) |",
        f"| Status | **{status}** |",
        "",
    ]

    if failure_reason:
        lines += [
            "## Failure Details",
            "",
            f"```text\n{failure_reason}\n```",
            "",
            f"Reproduce: `python3 -m ci.soak_full.orchestrator --mode {cfg.mode} --seed {cfg.seed} --build-dir {cfg.build_dir}`",
            "",
        ]

    lines += [
        "## Phase Timeline",
        "",
        "| Phase | Duration (s) | Detail |",
        "|---|---|---|",
    ]
    for pname, dur, detail in phase_log:
        lines.append(f"| `{pname}` | {dur:.1f} | {detail} |")
    lines.append("")

    lines += [
        "## Checkpoints",
        "",
        "| Checkpoint | Result | Master MB | Replica MB | Checks |",
        "|---|---|---|---|---|",
    ]
    for cp in checkpoints:
        res_s = "PASS" if cp.passed else f"FAIL: {cp.error}"
        chk_s = ", ".join(f"{k}={v}" for k, v in cp.checks.items())
        lines.append(f"| `{cp.label}` | {res_s} | {cp.master_used_mb:.1f} | {cp.replica_used_mb:.1f} | {chk_s} |")
    lines.append("")

    if latencies:
        lines += [
            "## Client Latency Summary",
            "",
            "| Command Family | Count | P50 (ms) | P99 (ms) | Max (ms) |",
            "|---|---|---|---|---|",
        ]
        for fam, st in sorted(latencies.items()):
            lines.append(
                f"| `{fam}` | {int(st['count'])} | {st['p50_ms']:.2f} | {st['p99_ms']:.2f} | {st['max_ms']:.2f} |"
            )
        lines.append("")

    if error_counts:
        lines += [
            "## Benign / Expected Chaos Errors",
            "",
            "| Error | Count |",
            "|---|---|",
        ]
        for k, v in sorted(error_counts.items()):
            lines.append(f"| `{k}` | {v} |")
        lines.append("")

    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return path
