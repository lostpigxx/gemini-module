#!/usr/bin/env python3
"""Black-box BF.LOADCHUNK malicious wire-payload audit for v5.

This complements bloom_rdb_wire_fuzz_audit.cc.  The C++ harness exercises the
decoder functions directly; this script exercises the Redis command surface and
checks whether malformed wire payloads crash the server or corrupt existing
keys.  It intentionally records raw behavioral differences instead of trying to
normalize RedisBloom error text.
"""

from __future__ import annotations

import argparse
import json
import random
import shutil
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from redisbloom_compat_matrix import (
    BASE_DIR,
    CompatEnv,
    Corpus,
    PortAllocator,
    RedisClient,
    RedisError,
    Server,
    b,
    check_filter,
    dump_chunks,
    json_safe,
    module_info,
    reserve_and_populate,
    safe_cmd as compat_safe_cmd,
)


WIRE_BASE_DIR = BASE_DIR.with_name("gemini-bloom-v5-malicious-wire-audit")
DEFAULT_SEED = 0x2420B1007


@dataclass(frozen=True)
class PayloadCase:
    name: str
    payload: bytes
    family: str


def safe_cmd(client: RedisClient, *args: Any) -> Any:
    try:
        return compat_safe_cmd(client, *args)
    except (EOFError, OSError, ConnectionError, BrokenPipeError, ConnectionResetError) as exc:
        return {"connection_error": f"{type(exc).__name__}: {exc}"}


def build_corpus() -> Corpus:
    return Corpus(
        "wire_probe",
        ("0.01", "10", "EXPANSION", "2"),
        tuple(b(f"wire:{i}") for i in range(40)),
        "multi-layer corpus for native SCANDUMP/LOADCHUNK payloads",
    )


def mutate(payload: bytes, rng: random.Random, max_flips: int = 8) -> bytes:
    if not payload:
        return payload
    data = bytearray(payload)
    for _ in range(1 + rng.randrange(max_flips)):
        data[rng.randrange(len(data))] = rng.randrange(256)
    return bytes(data)


def random_bytes(rng: random.Random, max_len: int) -> bytes:
    return bytes(rng.randrange(256) for _ in range(rng.randrange(max_len + 1)))


def gemini_wire_header(
    *,
    total_items: int = 0,
    num_layers: int = 1,
    flags: int = 5,
    expansion: int = 2,
    data_size: int = 16,
    total_bits: int = 128,
    item_count: int = 0,
    fp_rate: float = 0.01,
    bits_per_entry: float = 9.585058377367439,
    hash_count: int = 7,
    capacity: int = 10,
    log2_bits: int = 0,
) -> bytes:
    header = struct.pack("<QIII", total_items, num_layers, flags, expansion)
    if num_layers == 0:
        return header
    layer = struct.pack(
        "<QQQddIQB",
        data_size,
        total_bits,
        item_count,
        fp_rate,
        bits_per_entry,
        hash_count,
        capacity,
        log2_bits,
    )
    return header + layer


def build_header_cases(native_header: bytes, rng: random.Random, random_cases: int) -> list[PayloadCase]:
    cases: list[PayloadCase] = []
    for length in (0, 1, 2, 8, 16, max(0, len(native_header) - 1)):
        cases.append(PayloadCase(f"native_truncated_{length}", native_header[:length], "native_structured"))
    cases.append(PayloadCase("native_extra_byte", native_header + b"\x00", "native_structured"))
    for i in range(random_cases):
        if i % 2 == 0:
            payload = mutate(native_header, rng)
            family = "native_mutation"
        else:
            payload = random_bytes(rng, max(512, len(native_header) * 2))
            family = "random"
        cases.append(PayloadCase(f"{family}_{i:05d}", payload, family))

    structured = {
        "gemini_valid_empty_header": gemini_wire_header(),
        "gemini_zero_layers": gemini_wire_header(num_layers=0),
        "gemini_too_many_layers": gemini_wire_header(num_layers=1025),
        "gemini_unknown_flags": gemini_wire_header(flags=0x80),
        "gemini_rawbits_flag": gemini_wire_header(flags=2),
        "gemini_scaling_expansion_zero": gemini_wire_header(expansion=0),
        "gemini_total_items_mismatch": gemini_wire_header(total_items=1, item_count=0),
        "gemini_capacity_zero": gemini_wire_header(capacity=0),
        "gemini_fp_nan": gemini_wire_header(fp_rate=float("nan")),
        "gemini_fp_inf": gemini_wire_header(fp_rate=float("inf")),
        "gemini_fp_zero": gemini_wire_header(fp_rate=0.0),
        "gemini_fp_one": gemini_wire_header(fp_rate=1.0),
        "gemini_fp_negative": gemini_wire_header(fp_rate=-0.1),
        "gemini_bits_per_entry_zero": gemini_wire_header(bits_per_entry=0.0),
        "gemini_bits_per_entry_inf": gemini_wire_header(bits_per_entry=float("inf")),
        "gemini_bits_per_entry_negative": gemini_wire_header(bits_per_entry=-1.0),
        "gemini_hash_count_zero": gemini_wire_header(hash_count=0),
        "gemini_hash_count_inconsistent": gemini_wire_header(hash_count=10),
        "gemini_total_bits_zero": gemini_wire_header(data_size=0, total_bits=0),
        "gemini_log2_bits_64": gemini_wire_header(log2_bits=64),
        "gemini_log2_mismatch": gemini_wire_header(data_size=32, total_bits=256, log2_bits=7),
        "gemini_data_size_short": gemini_wire_header(data_size=15, total_bits=128),
        "gemini_data_size_long": gemini_wire_header(data_size=17, total_bits=128),
        "gemini_item_count_gt_capacity": gemini_wire_header(total_items=11, item_count=11),
        "gemini_fixed_expansion_zero": gemini_wire_header(flags=13, expansion=0),
    }
    for name, payload in structured.items():
        cases.append(PayloadCase(name, payload, "gemini_structured"))
    return cases


def build_data_cases(native_data: bytes, rng: random.Random, random_cases: int) -> list[PayloadCase]:
    cases: list[PayloadCase] = []
    for length in (0, 1, 2, 8, 16, max(0, len(native_data) - 1), len(native_data) + 1):
        payload = native_data[:length]
        if length > len(native_data):
            payload = native_data + b"\x00"
        cases.append(PayloadCase(f"data_len_{length}", payload, "data_structured"))
    for i in range(random_cases):
        if i % 2 == 0:
            payload = mutate(native_data, rng)
            family = "data_mutation"
        else:
            payload = random_bytes(rng, max(512, len(native_data) * 2))
            family = "data_random"
        cases.append(PayloadCase(f"{family}_{i:05d}", payload, family))
    return cases


def command_status(reply: Any) -> str:
    if isinstance(reply, dict) and "error" in reply:
        return "error"
    if isinstance(reply, dict) and "connection_error" in reply:
        return "connection_error"
    return "ok"


def ping_alive(client: RedisClient) -> bool:
    try:
        return client.cmd("PING") == "PONG"
    except Exception:
        return False


def close_quietly(client: RedisClient | None) -> None:
    if client is None:
        return
    try:
        client.close()
    except Exception:
        pass


def probe_loaded_key(client: RedisClient, key: str) -> dict[str, Any]:
    return {
        "type": safe_cmd(client, "TYPE", key),
        "bf_info": safe_cmd(client, "BF.INFO", key),
        "bf_card": safe_cmd(client, "BF.CARD", key),
        "bf_exists_probe": safe_cmd(client, "BF.EXISTS", key, "wire:0"),
    }


def run_one_loadchunk_case(
    client: RedisClient,
    key: str,
    cursor: int,
    payload: bytes,
    *,
    probe_key: bool,
) -> dict[str, Any]:
    reply = safe_cmd(client, "BF.LOADCHUNK", key, cursor, payload)
    alive_after_reply = ping_alive(client)
    result: dict[str, Any] = {
        "reply": reply,
        "status": command_status(reply),
        "alive_after_reply": alive_after_reply,
    }
    if alive_after_reply and probe_key and command_status(reply) == "ok":
        result["loaded_key_probe"] = probe_loaded_key(client, key)
        result["alive_after_probe"] = ping_alive(client)
    if alive_after_reply:
        safe_cmd(client, "DEL", key)
        result["alive_after_del"] = ping_alive(client)
    return result


def run_existing_key_case(
    client: RedisClient,
    key: str,
    cursor: int,
    payload: bytes,
) -> dict[str, Any]:
    safe_cmd(client, "DEL", key)
    reserve = safe_cmd(client, "BF.RESERVE", key, "0.01", "10")
    add_old = safe_cmd(client, "BF.ADD", key, "old")
    before = {
        "old": safe_cmd(client, "BF.EXISTS", key, "old"),
        "new": safe_cmd(client, "BF.EXISTS", key, "wire:0"),
        "card": safe_cmd(client, "BF.CARD", key),
    }
    reply = safe_cmd(client, "BF.LOADCHUNK", key, cursor, payload)
    alive = ping_alive(client)
    after = {
        "old": safe_cmd(client, "BF.EXISTS", key, "old") if alive else "server_dead",
        "new": safe_cmd(client, "BF.EXISTS", key, "wire:0") if alive else "server_dead",
        "card": safe_cmd(client, "BF.CARD", key) if alive else "server_dead",
        "type": safe_cmd(client, "TYPE", key) if alive else "server_dead",
    }
    safe_cmd(client, "DEL", key) if alive else None
    return {
        "reserve": reserve,
        "add_old": add_old,
        "before": before,
        "reply": reply,
        "status": command_status(reply),
        "alive_after_reply": alive,
        "after": after,
    }


def summarize_case_results(results: list[dict[str, Any]]) -> dict[str, Any]:
    summary = {
        "cases": len(results),
        "ok": 0,
        "error": 0,
        "connection_error": 0,
        "dead_after_reply": 0,
        "dead_after_probe": 0,
        "dead_after_del": 0,
        "accepted_with_probe_error": 0,
        "families": {},
        "accepted_samples": [],
        "dead_samples": [],
    }
    for item in results:
        status = item.get("status")
        family = item.get("family", "unknown")
        family_stats = summary["families"].setdefault(family, {"cases": 0, "ok": 0, "error": 0, "connection_error": 0})
        family_stats["cases"] += 1
        if status == "ok":
            summary["ok"] += 1
            family_stats["ok"] += 1
            if len(summary["accepted_samples"]) < 10:
                summary["accepted_samples"].append(item["name"])
            probe = item.get("loaded_key_probe")
            if isinstance(probe, dict) and any(isinstance(v, dict) and "error" in v for v in probe.values()):
                summary["accepted_with_probe_error"] += 1
        elif status == "connection_error":
            summary["connection_error"] += 1
            family_stats["connection_error"] += 1
        else:
            summary["error"] += 1
            family_stats["error"] += 1
        if item.get("alive_after_reply") is False:
            summary["dead_after_reply"] += 1
            if len(summary["dead_samples"]) < 10:
                summary["dead_samples"].append(item["name"])
        if item.get("alive_after_probe") is False:
            summary["dead_after_probe"] += 1
        if item.get("alive_after_del") is False:
            summary["dead_after_del"] += 1
    return summary


def run_module_audit(
    label: str,
    env: CompatEnv,
    module: Path,
    ports: PortAllocator,
    workdir: Path,
    random_cases: int,
    data_random_cases: int,
    seed: int,
) -> dict[str, Any]:
    rng = random.Random(seed)
    server = Server(label, env.redis_server, module, ports.take(), workdir / label)
    client: RedisClient | None = None
    restart_events: list[dict[str, Any]] = []

    def restart_client(current: RedisClient | None, reason: str) -> RedisClient:
        restart_events.append({"reason": reason, "log_tail_before_restart": server.log_tail(20)})
        close_quietly(current)
        try:
            server.stop()
        except Exception:
            pass
        return server.start(preserve_dir=True)

    try:
        client = server.start()
        corpus = build_corpus()
        source_key = f"{label}:source"
        populate = reserve_and_populate(client, source_key, corpus)
        source_check = check_filter(client, source_key, corpus)
        chunks = dump_chunks(client, source_key)
        if not chunks or not isinstance(chunks[0][1], bytes):
            raise RuntimeError(f"{label}: missing header chunk: {chunks!r}")
        header_cursor, native_header = chunks[0]
        data_chunk = next(((cursor, data) for cursor, data in chunks[1:] if cursor != 0 and isinstance(data, bytes)), None)

        header_results: list[dict[str, Any]] = []
        header_cases = build_header_cases(native_header, rng, random_cases)
        for index, case in enumerate(header_cases):
            key = f"{label}:hdr:{index}"
            result = run_one_loadchunk_case(client, key, int(header_cursor), case.payload, probe_key=True)
            result.update({
                "name": case.name,
                "family": case.family,
                "payload_len": len(case.payload),
            })
            header_results.append(result)
            if not result.get("alive_after_reply", False):
                client = restart_client(client, f"header:{case.name}")

        existing_results: list[dict[str, Any]] = []
        for case in header_cases[:8] + [c for c in header_cases if c.name in {
            "gemini_valid_empty_header",
            "gemini_bits_per_entry_zero",
            "gemini_hash_count_inconsistent",
            "gemini_fixed_expansion_zero",
        }]:
            result = run_existing_key_case(client, f"{label}:existing:{case.name}", int(header_cursor), case.payload)
            result.update({
                "name": case.name,
                "family": case.family,
                "payload_len": len(case.payload),
                "old_preserved": result.get("after", {}).get("old") == 1,
            })
            existing_results.append(result)
            if not result.get("alive_after_reply", False):
                client = restart_client(client, f"existing:{case.name}")

        data_results: list[dict[str, Any]] = []
        if data_chunk is not None:
            data_cursor, native_data = data_chunk
            data_cases = build_data_cases(native_data, rng, data_random_cases)
            for index, case in enumerate(data_cases):
                key = f"{label}:data:{index}"
                header_reply = safe_cmd(client, "BF.LOADCHUNK", key, int(header_cursor), native_header)
                result = {
                    "name": case.name,
                    "family": case.family,
                    "payload_len": len(case.payload),
                    "header_reply": header_reply,
                    "header_status": command_status(header_reply),
                }
                if command_status(header_reply) == "ok":
                    result.update(run_one_loadchunk_case(client, key, int(data_cursor), case.payload, probe_key=True))
                else:
                    result.update({"reply": {"error": "valid native header was rejected"}, "status": "error", "alive_after_reply": ping_alive(client)})
                    safe_cmd(client, "DEL", key)
                data_results.append(result)
                if not result.get("alive_after_reply", False):
                    client = restart_client(client, f"data:{case.name}")

        return {
            "module_info": module_info(client),
            "populate": populate,
            "source_check": source_check,
            "native_chunks": [[cursor, 0 if data is None else len(data)] for cursor, data in chunks],
            "header_cursor": int(header_cursor),
            "data_cursor": int(data_chunk[0]) if data_chunk else None,
            "header_summary": summarize_case_results(header_results),
            "existing_key_summary": {
                "cases": len(existing_results),
                "old_preserved": sum(1 for r in existing_results if r.get("old_preserved") is True),
                "old_lost": sum(1 for r in existing_results if r.get("old_preserved") is False),
                "ok": sum(1 for r in existing_results if r.get("status") == "ok"),
                "error": sum(1 for r in existing_results if r.get("status") == "error"),
                "connection_error": sum(1 for r in existing_results if r.get("status") == "connection_error"),
                "dead_after_reply": sum(1 for r in existing_results if r.get("alive_after_reply") is False),
                "old_lost_samples": [r["name"] for r in existing_results if r.get("old_preserved") is False][:10],
            },
            "data_summary": summarize_case_results(data_results),
            "interesting_header_results": [
                json_safe(r)
                for r in header_results
                if r.get("status") == "ok" or r.get("alive_after_reply") is False
            ][:50],
            "structured_header_results": [
                json_safe(r)
                for r in header_results
                if r.get("family") in ("native_structured", "gemini_structured")
            ],
            "connection_error_header_results": [
                json_safe(r)
                for r in header_results
                if r.get("status") == "connection_error" or r.get("alive_after_reply") is False
            ],
            "interesting_existing_key_results": [
                json_safe(r)
                for r in existing_results
                if r.get("status") == "ok" or r.get("old_preserved") is False or r.get("alive_after_reply") is False
            ],
            "interesting_data_results": [
                json_safe(r)
                for r in data_results
                if r.get("status") == "ok" or r.get("alive_after_reply") is False
            ][:50],
            "restart_events": restart_events,
            "critical_log_count": server.critical_log_count(),
            "log_tail": server.log_tail(10),
        }
    finally:
        close_quietly(client)
        server.stop()


def summarize_modules(modules: dict[str, Any]) -> dict[str, Any]:
    return {
        label: {
            "header": value.get("header_summary"),
            "existing_key": value.get("existing_key_summary"),
            "data": value.get("data_summary"),
            "critical_log_count": value.get("critical_log_count"),
        }
        for label, value in modules.items()
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gemini-module", required=True, type=Path)
    parser.add_argument("--redis-server", required=True, type=Path)
    parser.add_argument("--redisbloom-module", required=True, type=Path)
    parser.add_argument("--env-name", default="redis-6.2-redisbloom-v2.4.20")
    parser.add_argument("--redis-tag", default="6.2.17")
    parser.add_argument("--redisbloom-tag", default="v2.4.20")
    parser.add_argument("--module-ver", default="20420")
    parser.add_argument("--base-port", default=51500, type=int)
    parser.add_argument("--seed", default=DEFAULT_SEED, type=lambda s: int(s, 0))
    parser.add_argument("--random-cases", default=5000, type=int)
    parser.add_argument("--data-random-cases", default=2000, type=int)
    parser.add_argument(
        "--output",
        default=Path("doc/code_review/gemini-bloom/v5/malicious_wire_audit_results_redis62_redisbloom2420.json"),
        type=Path,
    )
    args = parser.parse_args()

    for path_arg, label in (
        (args.gemini_module, "gemini module"),
        (args.redis_server, "redis-server"),
        (args.redisbloom_module, "RedisBloom module"),
    ):
        if not path_arg.exists():
            raise SystemExit(f"{label} not found: {path_arg}")

    env = CompatEnv(
        name=args.env_name,
        redis_tag=args.redis_tag,
        redisbloom_tag=args.redisbloom_tag,
        module_ver=args.module_ver,
        redis_server=args.redis_server,
        redisbloom_module=args.redisbloom_module,
    )
    ports = PortAllocator(args.base_port)
    workdir = WIRE_BASE_DIR / args.env_name
    shutil.rmtree(workdir, ignore_errors=True)
    workdir.mkdir(parents=True)

    modules = {
        "gemini": run_module_audit(
            "gemini",
            env,
            args.gemini_module,
            ports,
            workdir,
            args.random_cases,
            args.data_random_cases,
            args.seed ^ 0x1111,
        ),
        "redisbloom": run_module_audit(
            "redisbloom",
            env,
            args.redisbloom_module,
            ports,
            workdir,
            args.random_cases,
            args.data_random_cases,
            args.seed ^ 0x2222,
        ),
    }
    result = {
        "env": {
            "name": env.name,
            "redis_tag": env.redis_tag,
            "redisbloom_tag": env.redisbloom_tag,
            "module_ver": env.module_ver,
            "redis_server": str(env.redis_server),
            "redisbloom_module": str(env.redisbloom_module),
            "gemini_module": str(args.gemini_module),
        },
        "seed": args.seed,
        "random_cases_per_module": args.random_cases,
        "data_random_cases_per_module": args.data_random_cases,
        "modules": modules,
    }
    result["summary"] = summarize_modules(modules)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(json_safe(result), indent=2, sort_keys=True) + "\n")
    print(json.dumps(json_safe(result["summary"]), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
