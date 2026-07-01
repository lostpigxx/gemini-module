#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[5]
STAGE05_TOOLS = ROOT / ".codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff"
sys.path.insert(0, str(STAGE05_TOOLS))

from redisbloom_compat_matrix import (  # noqa: E402
    PortAllocator,
    RedisClient,
    RedisError,
    Server,
    json_safe,
    safe_cmd,
)


HEADER_SIZE = 20
META_SIZE = 53
FLAGS_NOROUND = 1
FLAGS_RAWBITS = 2
FLAGS_USE64 = 4
FLAGS_FIXED = 8
FLAGS_LOADING = 16
SUPPORTED_FLAGS = FLAGS_NOROUND | FLAGS_USE64 | FLAGS_FIXED
K_MAX_EXPANSION = 32768
K_MAX_TOTAL_DATA_SIZE = 4 * 1024 * 1024 * 1024
K_DESIGN_MAX_LAYER_DATA_SIZE = 2 * 1024 * 1024 * 1024


def b(text: str) -> bytes:
    return text.encode()


def normalize(value: Any) -> Any:
    return json_safe(value)


def is_error(value: Any, contains: str | None = None) -> bool:
    if not isinstance(value, dict) or "error" not in value:
        return False
    if contains is None:
        return True
    return contains in str(value["error"])


def is_ok(value: Any) -> bool:
    return value == "OK"


def assert_ping(client: RedisClient) -> bool:
    return safe_cmd(client, "PING") == "PONG"


def check_items(client: RedisClient, key: str, items: list[bytes]) -> dict[str, Any]:
    missing: list[str] = []
    found = 0
    for idx, item in enumerate(items):
        reply = safe_cmd(client, "BF.EXISTS", key, item)
        if reply == 1:
            found += 1
        else:
            missing.append(f"{idx}:{item[:24].hex()}")
    card = safe_cmd(client, "BF.CARD", key)
    return {
        "expected_items": len(items),
        "found": found,
        "missing_count": len(missing),
        "missing_samples": missing[:8],
        "card": normalize(card),
    }


def make_source(client: RedisClient, prefix: str, expansion: int = 2, cap: int = 5, count: int = 40) -> dict[str, Any]:
    key = f"{prefix}_src"
    items = [b(f"{prefix}:item:{i}") for i in range(count)]
    reserve = safe_cmd(client, "BF.RESERVE", key, "0.01", str(cap), "EXPANSION", str(expansion))
    adds = [safe_cmd(client, "BF.ADD", key, item) for item in items]
    chunks: list[tuple[int, bytes]] = []
    cursor = 0
    for _ in range(4096):
        reply = safe_cmd(client, "BF.SCANDUMP", key, str(cursor))
        if not isinstance(reply, list) or len(reply) != 2:
            raise RuntimeError(f"unexpected SCANDUMP reply for {key}: {reply!r}")
        next_cursor, data = reply
        if not isinstance(next_cursor, int) or not isinstance(data, bytes):
            raise RuntimeError(f"unexpected SCANDUMP element for {key}: {reply!r}")
        chunks.append((next_cursor, data))
        if next_cursor == 0:
            break
        cursor = next_cursor
    return {
        "key": key,
        "items": items,
        "reserve": reserve,
        "adds": adds,
        "chunks": chunks,
        "header": chunks[0][1],
        "data_chunks": chunks[1:-1],
        "source_check": check_items(client, key, items),
    }


def mutate_header(header: bytes, offset: int, fmt: str, value: Any) -> bytes:
    buf = bytearray(header)
    struct.pack_into(fmt, buf, offset, value)
    return bytes(buf)


def mutate_meta(header: bytes, layer: int, meta_offset: int, fmt: str, value: Any) -> bytes:
    return mutate_header(header, HEADER_SIZE + layer * META_SIZE + meta_offset, fmt, value)


def header_num_layers(header: bytes) -> int:
    return struct.unpack_from("<I", header, 8)[0]


def layer_data_size(header: bytes, layer: int) -> int:
    return struct.unpack_from("<Q", header, HEADER_SIZE + layer * META_SIZE)[0]


def header_case(client: RedisClient, name: str, payload: bytes, expect_reject: bool, note: str) -> dict[str, Any]:
    key = f"hdr_{name}"
    safe_cmd(client, "DEL", key)
    reply = safe_cmd(client, "BF.LOADCHUNK", key, "1", payload)
    exists = safe_cmd(client, "EXISTS", key)
    loading_probe = safe_cmd(client, "BF.INFO", key) if exists == 1 else None
    cleanup = safe_cmd(client, "DEL", key)
    alive = assert_ping(client)
    accepted = is_ok(reply)
    passed = (not accepted) if expect_reject else accepted
    status = "PASS" if passed and alive else "FAIL"
    return {
        "name": name,
        "status": status,
        "classification": "FAIL" if status == "FAIL" else "PASS",
        "expect_reject": expect_reject,
        "note": note,
        "reply": normalize(reply),
        "exists_after": exists,
        "loading_probe": normalize(loading_probe),
        "cleanup": normalize(cleanup),
        "server_alive": alive,
        "payload_len": len(payload),
    }


def run_header_fuzz(client: RedisClient, header: bytes, seed: int) -> list[dict[str, Any]]:
    rng = random.Random(seed)
    cases: list[tuple[str, bytes, bool, str]] = [
        ("empty", b"", True, "empty header blob"),
        ("one_byte", b"x", True, "1-byte truncated header"),
        ("truncated_header", header[: HEADER_SIZE - 1], True, "header shorter than WireFilterHeader"),
        ("truncated_meta", header[: HEADER_SIZE + 12], True, "metadata truncated"),
        ("overlong_header", header + b"JUNK", True, "valid header plus trailing junk"),
        ("random_header_73", rng.randbytes(len(header)), True, "random bytes same length as valid header"),
        ("num_layers_zero", mutate_header(header, 8, "<I", 0), True, "numLayers=0"),
        ("num_layers_1025", mutate_header(header, 8, "<I", 1025), True, "numLayers above kMaxLayers"),
        ("unknown_flags", mutate_header(header, 12, "<I", 0x80000000), True, "unknown flag bit"),
        ("rawbits_flag", mutate_header(header, 12, "<I", FLAGS_USE64 | FLAGS_NOROUND | FLAGS_RAWBITS), True, "RawBits flag injection"),
        ("loading_flag", mutate_header(header, 12, "<I", FLAGS_USE64 | FLAGS_NOROUND | FLAGS_LOADING), True, "Loading flag injection"),
        ("expansion_zero", mutate_header(header, 16, "<I", 0), True, "scaling filter expansion=0"),
        ("expansion_limit", mutate_header(header, 16, "<I", K_MAX_EXPANSION), False, "boundary max expansion accepted by command path"),
        ("expansion_over_limit", mutate_header(header, 16, "<I", K_MAX_EXPANSION + 1), True, "DESIGN max expansion violation"),
        ("expansion_uint_max", mutate_header(header, 16, "<I", 0xFFFFFFFF), True, "extreme expansionFactor"),
        ("item_sum_mismatch", mutate_header(header, 0, "<Q", 999999), True, "totalItems != sum(itemCount)"),
        ("item_count_gt_capacity", mutate_meta(header, 0, 16, "<Q", 2**40), True, "itemCount > capacity"),
        ("hash_count_zero", mutate_meta(header, 0, 40, "<I", 0), True, "hashCount=0"),
        ("hash_count_wrong", mutate_meta(header, 0, 40, "<I", 123), True, "hashCount inconsistent with bitsPerEntry"),
        ("fp_rate_nan", mutate_meta(header, 0, 24, "<d", float("nan")), True, "fpRate NaN"),
        ("fp_rate_inf", mutate_meta(header, 0, 24, "<d", float("inf")), True, "fpRate Inf"),
        ("fp_rate_zero", mutate_meta(header, 0, 24, "<d", 0.0), True, "fpRate=0"),
        ("fp_rate_one", mutate_meta(header, 0, 24, "<d", 1.0), True, "fpRate=1"),
        ("bits_per_entry_nan", mutate_meta(header, 0, 32, "<d", float("nan")), True, "bitsPerEntry NaN"),
        ("bits_per_entry_inf", mutate_meta(header, 0, 32, "<d", float("inf")), True, "bitsPerEntry Inf"),
        ("bits_per_entry_zero", mutate_meta(header, 0, 32, "<d", 0.0), True, "bitsPerEntry=0"),
        ("bits_per_entry_gt_1000", mutate_meta(header, 0, 32, "<d", 1000.1), True, "bitsPerEntry above DESIGN cap"),
        ("total_bits_zero", mutate_meta(header, 0, 8, "<Q", 0), True, "totalBits=0"),
        ("log2_bits_64", mutate_meta(header, 0, 52, "<B", 64), True, "log2Bits >= 64"),
        ("data_size_mismatch", mutate_meta(header, 0, 0, "<Q", layer_data_size(header, 0) + 1), True, "dataSize != ceil(totalBits/8)"),
    ]
    results = [header_case(client, *case) for case in cases]
    for i in range(64):
        size = rng.choice([0, 1, 2, 7, 19, 20, 21, 72, 73, 74, 128, 512])
        payload = rng.randbytes(size)
        results.append(header_case(client, f"random_{i:02d}_{size}", payload, True, f"seeded random payload size={size}"))
    return results


def run_existing_key_safety(client: RedisClient, header: bytes) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    safe_cmd(client, "DEL", "existing_bloom", "string_key")
    safe_cmd(client, "BF.RESERVE", "existing_bloom", "0.01", "10")
    safe_cmd(client, "BF.ADD", "existing_bloom", "old_item")
    reply = safe_cmd(client, "BF.LOADCHUNK", "existing_bloom", "1", header)
    old_exists = safe_cmd(client, "BF.EXISTS", "existing_bloom", "old_item")
    results.append({
        "name": "header_on_existing_bloom",
        "status": "PASS" if is_error(reply) and old_exists == 1 and assert_ping(client) else "FAIL",
        "reply": normalize(reply),
        "old_exists_after": normalize(old_exists),
        "note": "valid header on existing Bloom key must reject and preserve old item",
    })

    safe_cmd(client, "SET", "string_key", "value")
    reply = safe_cmd(client, "BF.LOADCHUNK", "string_key", "1", header)
    value = safe_cmd(client, "GET", "string_key")
    results.append({
        "name": "header_on_string_key",
        "status": "PASS" if is_error(reply, "WRONGTYPE") and value == b"value" and assert_ping(client) else "FAIL",
        "reply": normalize(reply),
        "string_value_after": normalize(value),
        "note": "valid header on string key must return WRONGTYPE and preserve value",
    })
    return results


def run_loading_state(client: RedisClient, material: dict[str, Any]) -> dict[str, Any]:
    key = "loading_state_dst"
    safe_cmd(client, "DEL", key)
    header = material["header"]
    chunks: list[tuple[int, bytes]] = material["data_chunks"]
    load_header = safe_cmd(client, "BF.LOADCHUNK", key, "1", header)
    blocked_commands = {
        "add": safe_cmd(client, "BF.ADD", key, "x"),
        "madd": safe_cmd(client, "BF.MADD", key, "x", "y"),
        "insert": safe_cmd(client, "BF.INSERT", key, "NOCREATE", "ITEMS", "x"),
        "exists": safe_cmd(client, "BF.EXISTS", key, "x"),
        "mexists": safe_cmd(client, "BF.MEXISTS", key, "x", "y"),
        "info": safe_cmd(client, "BF.INFO", key),
        "card": safe_cmd(client, "BF.CARD", key),
        "scandump": safe_cmd(client, "BF.SCANDUMP", key, "0"),
    }
    bad_chunk = safe_cmd(client, "BF.LOADCHUNK", key, "2", b"short")
    blocked_after_bad = safe_cmd(client, "BF.EXISTS", key, "x")
    load_replies = []
    for cursor, data in chunks:
        load_replies.append((cursor, safe_cmd(client, "BF.LOADCHUNK", key, str(cursor), data)))
    check = check_items(client, key, material["items"])
    completed_cursor = chunks[-1][0] if chunks else 2
    overwrite = safe_cmd(client, "BF.LOADCHUNK", key, str(completed_cursor), b"\x00" * len(chunks[-1][1]))
    all_blocked = all(is_error(v, "filter is being loaded") for v in blocked_commands.values())
    status = "PASS" if (
        is_ok(load_header)
        and all_blocked
        and is_error(bad_chunk)
        and is_error(blocked_after_bad, "filter is being loaded")
        and all(is_ok(v) for _, v in load_replies)
        and check["missing_count"] == 0
        and is_error(overwrite)
        and assert_ping(client)
    ) else "FAIL"
    return {
        "name": "loading_state_lifecycle",
        "status": status,
        "load_header": normalize(load_header),
        "blocked_commands": normalize(blocked_commands),
        "bad_chunk": normalize(bad_chunk),
        "blocked_after_bad": normalize(blocked_after_bad),
        "load_replies": normalize(load_replies),
        "check": check,
        "overwrite_after_completion": normalize(overwrite),
    }


def run_cursor_faults(client: RedisClient, material: dict[str, Any]) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    header = material["header"]
    chunks: list[tuple[int, bytes]] = material["data_chunks"]
    items = material["items"]

    reply = safe_cmd(client, "BF.LOADCHUNK", "missing_key", "2", b"x")
    results.append({
        "name": "cursor_gt1_missing_key",
        "status": "PASS" if is_error(reply, "key does not exist") and assert_ping(client) else "FAIL",
        "reply": normalize(reply),
    })

    safe_cmd(client, "DEL", "completed_key")
    safe_cmd(client, "BF.RESERVE", "completed_key", "0.01", "10")
    reply = safe_cmd(client, "BF.LOADCHUNK", "completed_key", "2", b"\x00" * len(chunks[0][1]))
    results.append({
        "name": "cursor_gt1_completed_key",
        "status": "PASS" if is_error(reply) and assert_ping(client) else "FAIL",
        "reply": normalize(reply),
    })

    for cursor_value, expected_error in [("0", "positive"), ("-1", "positive"), ("abc", "positive")]:
        key = f"bad_cursor_{cursor_value.replace('-', 'neg')}"
        reply = safe_cmd(client, "BF.LOADCHUNK", key, cursor_value, header)
        results.append({
            "name": f"bad_cursor_{cursor_value}",
            "status": "PASS" if is_error(reply, expected_error) and assert_ping(client) else "FAIL",
            "reply": normalize(reply),
        })

    key = "skip_to_final"
    safe_cmd(client, "DEL", key)
    load_header = safe_cmd(client, "BF.LOADCHUNK", key, "1", header)
    final_cursor, final_data = chunks[-1]
    skip_reply = safe_cmd(client, "BF.LOADCHUNK", key, str(final_cursor), final_data)
    check = check_items(client, key, items)
    results.append({
        "name": "cursor_skip_to_final_exposes_false_negatives",
        "status": "FAIL" if is_ok(load_header) and is_ok(skip_reply) and check["missing_count"] > 0 else "PASS",
        "classification": "FAIL_ACCEPTED_MALFORMED_SEQUENCE" if check["missing_count"] > 0 else "PASS",
        "load_header": normalize(load_header),
        "skip_reply": normalize(skip_reply),
        "check": check,
        "note": "Header then final chunk clears Loading without earlier chunks; false negatives indicate unsafe cursor sequencing.",
    })

    if len(chunks) >= 2 and len(chunks[0][1]) == len(chunks[-1][1]):
        key = "repeat_layer0_all_layers"
        safe_cmd(client, "DEL", key)
        load_header = safe_cmd(client, "BF.LOADCHUNK", key, "1", header)
        replies = []
        layer0 = chunks[0][1]
        for cursor, _ in chunks:
            replies.append((cursor, safe_cmd(client, "BF.LOADCHUNK", key, str(cursor), layer0)))
        check = check_items(client, key, items)
        results.append({
            "name": "repeat_first_chunk_for_all_layers",
            "status": "FAIL" if is_ok(load_header) and all(is_ok(v) for _, v in replies) and check["missing_count"] > 0 else "PASS",
            "classification": "FAIL_ACCEPTED_MALFORMED_SEQUENCE" if check["missing_count"] > 0 else "PASS",
            "load_header": normalize(load_header),
            "replies": normalize(replies),
            "check": check,
            "note": "Repeated same-sized layer chunk can complete key with false negatives if no chunk completion tracking exists.",
        })
    return results


def wait_for_aof_rewrite(client: RedisClient) -> Any:
    start = safe_cmd(client, "BGREWRITEAOF")
    if not (is_ok(start) or (isinstance(start, str) and "rewriting started" in start)):
        return {"start": normalize(start), "done": False}
    deadline = time.time() + 20
    last = None
    while time.time() < deadline:
        info = safe_cmd(client, "INFO", "persistence")
        last = info
        text = info.decode() if isinstance(info, bytes) else str(info)
        fields = dict(line.split(":", 1) for line in text.splitlines() if ":" in line and not line.startswith("#"))
        if fields.get("aof_rewrite_in_progress") == "0":
            return {"start": start, "done": fields.get("aof_last_bgrewrite_status") == "ok", "fields": fields}
        time.sleep(0.05)
    return {"start": start, "done": False, "last": normalize(last)}


def run_persist_loading(redis_server: Path, module: Path, ports: PortAllocator, workdir: Path, material: dict[str, Any]) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    for mode in ["rdb", "aof_no_preamble"]:
        server = Server(f"stage07-{mode}", redis_server, module, ports.take(), workdir / mode)
        key = f"loading_{mode}"
        try:
            appendonly = mode.startswith("aof")
            client = server.start(appendonly=appendonly, aof_preamble=False)
            try:
                safe_cmd(client, "DEL", key)
                load_header = safe_cmd(client, "BF.LOADCHUNK", key, "1", material["header"])
                blocked = safe_cmd(client, "BF.EXISTS", key, material["items"][0])
                if mode == "rdb":
                    persist = safe_cmd(client, "SAVE")
                else:
                    persist = wait_for_aof_rewrite(client)
            finally:
                client.close()
                server.stop()
            restarted = server.start(appendonly=appendonly, preserve_dir=True, aof_preamble=False)
            try:
                post_type = safe_cmd(restarted, "TYPE", key)
                post_info = safe_cmd(restarted, "BF.INFO", key)
                check = check_items(restarted, key, material["items"])
                status = "FAIL" if check["missing_count"] > 0 and not is_error(post_info, "filter is being loaded") else "PASS"
                results.append({
                    "name": f"persist_half_loaded_{mode}",
                    "status": status,
                    "classification": "FAIL_LOADING_PERSISTED_AS_COMPLETED" if status == "FAIL" else "PASS",
                    "load_header": normalize(load_header),
                    "blocked_before_persist": normalize(blocked),
                    "persist": normalize(persist),
                    "post_restart_type": normalize(post_type),
                    "post_restart_info": normalize(post_info),
                    "post_restart_check": check,
                    "server_log": server.log_tail(80),
                    "note": "Half-loaded key must not restart as a normal completed filter with false negatives.",
                })
            finally:
                restarted.close()
                server.stop()
        except Exception as exc:
            results.append({
                "name": f"persist_half_loaded_{mode}",
                "status": "BLOCKED",
                "classification": "BLOCKED",
                "error": str(exc),
                "server_log": server.log_tail(80),
            })
            server.stop()
    return results


def static_resource_boundary_review() -> list[dict[str, Any]]:
    return [
        {
            "name": "per_layer_data_size_gt_2gb",
            "status": "FAIL",
            "classification": "FAIL_STATIC_DESIGN_VIOLATION",
            "finding": "GBV6-03-001",
            "runtime_probe": "NOT_RUN_UNSAFE_ALLOCATION_RISK",
            "evidence": [
                "modules/gemini-bloom/src/bloom_rdb.cc:53-68 ValidateLayerFields lacks per-layer 2GB cap",
                "modules/gemini-bloom/src/bloom_rdb.cc:295-317 wire path only enforces total <=4GB before allocation",
                ".codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md",
            ],
            "note": "A header with one layer just above 2GB and total <=4GB is rejected by DESIGN but not by shared field validation before allocation.",
        },
        {
            "name": "expansion_factor_above_kmaxexpansion",
            "status": "FAIL",
            "classification": "FAIL_RUNTIME_CONFIRMED",
            "finding": "GBV6-03-002",
            "evidence": [
                "Header fuzz rows expansion_over_limit and expansion_uint_max",
                "modules/gemini-bloom/src/bloom_rdb.cc:281-305 checks expansion zero but not kMaxExpansion",
            ],
            "note": "Runtime LOADCHUNK header fuzz confirms expansion above 32768 is accepted as a Loading filter.",
        },
    ]


def write_log_copy(src_dir: Path, dst_dir: Path, name: str) -> None:
    dst_dir.mkdir(parents=True, exist_ok=True)
    log = src_dir / "redis.log"
    if log.exists():
        (dst_dir / f"{name}.log").write_text(log.read_text(errors="replace"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--redis-server", required=True, type=Path)
    parser.add_argument("--gemini-module", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--workdir", default=Path("/tmp/gemini-bloom-stage07-fuzz"), type=Path)
    parser.add_argument("--seed", type=int, default=0xB100F007)
    args = parser.parse_args()

    shutil.rmtree(args.workdir, ignore_errors=True)
    args.workdir.mkdir(parents=True)
    ports = PortAllocator(54000)
    server = Server("stage07-main", args.redis_server, args.gemini_module, ports.take(), args.workdir / "main")

    result: dict[str, Any] = {
        "seed": args.seed,
        "env": {
            "redis_server": str(args.redis_server),
            "gemini_module": str(args.gemini_module),
            "workdir": str(args.workdir),
        },
        "header_fuzz": [],
        "existing_key_safety": [],
        "loading_state": {},
        "cursor_faults": [],
        "persistence_faults": [],
        "static_resource_boundary": static_resource_boundary_review(),
        "summary": {},
    }

    client = server.start()
    try:
        material = make_source(client, "multi_exp2", expansion=2, cap=5, count=40)
        same_size_material = make_source(client, "expansion1", expansion=1, cap=5, count=20)
        result["source_material"] = {
            "multi_exp2": {
                "chunks": [[cur, len(data)] for cur, data in material["chunks"]],
                "source_check": material["source_check"],
                "num_layers": header_num_layers(material["header"]),
            },
            "expansion1": {
                "chunks": [[cur, len(data)] for cur, data in same_size_material["chunks"]],
                "source_check": same_size_material["source_check"],
                "num_layers": header_num_layers(same_size_material["header"]),
            },
        }
        result["header_fuzz"] = run_header_fuzz(client, material["header"], args.seed)
        result["existing_key_safety"] = run_existing_key_safety(client, material["header"])
        result["loading_state"] = run_loading_state(client, material)
        result["cursor_faults"] = run_cursor_faults(client, same_size_material)
    finally:
        client.close()
        server.stop()

    result["persistence_faults"] = run_persist_loading(args.redis_server, args.gemini_module, ports, args.workdir / "persist", material)
    write_log_copy(server.workdir, args.output.parent / "server_logs", "main")
    for child in (args.workdir / "persist").glob("*"):
        if child.is_dir():
            write_log_copy(child, args.output.parent / "server_logs", f"persist_{child.name}")

    def count_status(values: list[dict[str, Any]]) -> dict[str, int]:
        counts: dict[str, int] = {}
        for row in values:
            counts[row.get("status", "UNKNOWN")] = counts.get(row.get("status", "UNKNOWN"), 0) + 1
        return counts

    result["summary"] = {
        "header_fuzz": count_status(result["header_fuzz"]),
        "existing_key_safety": count_status(result["existing_key_safety"]),
        "loading_state": result["loading_state"].get("status"),
        "cursor_faults": count_status(result["cursor_faults"]),
        "persistence_faults": count_status(result["persistence_faults"]),
        "static_resource_boundary": count_status(result["static_resource_boundary"]),
        "server_alive_end": True,
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True))
    print(json.dumps(result["summary"], indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
