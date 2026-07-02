#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import time
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[6]
STAGE05_TOOLS = ROOT / ".codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff"
sys.path.insert(0, str(STAGE05_TOOLS))

from redisbloom_compat_matrix import (  # noqa: E402
    Corpus,
    PortAllocator,
    RedisClient,
    Server,
    b,
    check_filter,
    check_passed,
    dump_chunks,
    load_chunks,
    reserve_and_populate,
    safe_cmd,
    wait_check,
    wait_replica_ready,
)


def build_corpora(include_large: bool) -> list[Corpus]:
    corpora = [
        Corpus("empty_scaling", ("0.01", "100"), tuple(), "empty scaling filter"),
        Corpus("single_layer", ("0.01", "100"), tuple(b(f"single:{i}") for i in range(8)), "single layer"),
        Corpus("multi_exp2", ("0.01", "10", "EXPANSION", "2"), tuple(b(f"item:{i}") for i in range(40)), "multi-layer expansion 2"),
        Corpus("fixed_full", ("0.01", "8", "NONSCALING"), tuple(b(f"fixed:{i}") for i in range(8)), "fixed filter at capacity"),
        Corpus("expansion1", ("0.01", "5", "EXPANSION", "1"), tuple(b(f"exp1:{i}") for i in range(20)), "many same-sized layers"),
        Corpus("expansion4", ("0.01", "5", "EXPANSION", "4"), tuple(b(f"exp4:{i}") for i in range(40)), "larger expansion"),
        Corpus(
            "binary_items",
            ("0.01", "50"),
            (
                b"",
                b"nul:\x00:inside",
                b"crlf:\r\n:inside",
                b"space and tab\titem",
                b"tcl:{list}\\chars",
                bytes(range(1, 32)),
            ),
            "raw RESP binary item coverage",
        ),
        Corpus("long_item", ("0.01", "50"), (b("long:") + b"x" * 65536,), "large item payload"),
    ]
    if include_large:
        corpora.append(Corpus("large_empty_16mb", ("0.01", "15000000"), tuple(), "empty large filter"))
    return corpora


def file_manifest(path: Path) -> dict[str, Any]:
    if not path.exists() or not path.is_file():
        return {"exists": False, "path": str(path)}
    data = path.read_bytes()
    return {
        "exists": True,
        "path": str(path),
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def check_ok(result: dict[str, Any]) -> bool:
    return check_passed(result.get("check", {})) and result.get("critical_log_count", 0) == 0 and "error" not in result


def wait_for_aof_rewrite(client: RedisClient) -> None:
    client.cmd("BGREWRITEAOF")
    deadline = time.time() + 20
    while time.time() < deadline:
        info = client.cmd("INFO", "persistence")
        text = info.decode() if isinstance(info, bytes) else str(info)
        fields = dict(line.split(":", 1) for line in text.splitlines() if ":" in line and not line.startswith("#"))
        if fields.get("aof_rewrite_in_progress") == "0":
            if fields.get("aof_last_bgrewrite_status") == "ok":
                return
            raise RuntimeError(f"AOF rewrite failed: {fields.get('aof_last_bgrewrite_status')}")
        time.sleep(0.05)
    raise RuntimeError("AOF rewrite timed out")


def copy_tree(src: Path, dst: Path) -> None:
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(src, dst)


def run_rdb_self(redis_server: Path, module: Path, corpus: Corpus, ports: PortAllocator, workdir: Path) -> dict[str, Any]:
    server = Server("gemini-rdb-self", redis_server, module, ports.take(), workdir / corpus.name)
    key = f"rdb_self_{corpus.name}"
    try:
        client = server.start()
        try:
            populate = reserve_and_populate(client, key, corpus)
            source_check = check_filter(client, key, corpus)
            save = safe_cmd(client, "SAVE")
        finally:
            client.close()
            server.stop()
        target = server.start(preserve_dir=True)
        try:
            check = check_filter(target, key, corpus, expected_card=source_check.get("card"))
            info = safe_cmd(target, "BF.INFO", key)
            return {
                "populate": populate,
                "source_check": source_check,
                "save": save,
                "check": check,
                "info": info,
                "artifact": file_manifest(server.workdir / "dump.rdb"),
                "critical_log_count": server.critical_log_count(),
            }
        finally:
            target.close()
            server.stop()
    except Exception as exc:
        return {"error": str(exc), "log": server.log_tail()}
    finally:
        server.stop()


def run_dump_restore_self(redis_server: Path, module: Path, corpus: Corpus, ports: PortAllocator, workdir: Path) -> dict[str, Any]:
    server = Server("gemini-dump-self", redis_server, module, ports.take(), workdir / corpus.name)
    key = f"dump_self_{corpus.name}"
    dst = f"dump_self_dst_{corpus.name}"
    try:
        client = server.start()
        try:
            populate = reserve_and_populate(client, key, corpus)
            expire = safe_cmd(client, "PEXPIRE", key, "600000")
            source_check = check_filter(client, key, corpus)
            source_pttl = safe_cmd(client, "PTTL", key)
            dumped = safe_cmd(client, "DUMP", key)
            restore = safe_cmd(client, "RESTORE", dst, "600000", dumped, "REPLACE") if isinstance(dumped, bytes) else {"error": "DUMP did not return bytes"}
            check = check_filter(client, dst, corpus, expected_card=source_check.get("card"))
            target_pttl = safe_cmd(client, "PTTL", dst)
            dump_path = workdir / f"{corpus.name}.dump"
            if isinstance(dumped, bytes):
                dump_path.write_bytes(dumped)
            return {
                "populate": populate,
                "expire": expire,
                "source_check": source_check,
                "source_pttl": source_pttl,
                "restore": restore,
                "check": check,
                "target_pttl": target_pttl,
                "artifact": file_manifest(dump_path),
                "critical_log_count": server.critical_log_count(),
            }
        finally:
            client.close()
            server.stop()
    except Exception as exc:
        return {"error": str(exc), "log": server.log_tail()}
    finally:
        server.stop()


def run_fullsync_self(redis_server: Path, module: Path, corpus: Corpus, ports: PortAllocator, workdir: Path) -> dict[str, Any]:
    master = Server("gemini-fullsync-master", redis_server, module, ports.take(), workdir / corpus.name / "master")
    replica = Server("gemini-fullsync-replica", redis_server, module, ports.take(), workdir / corpus.name / "replica")
    key = f"fullsync_self_{corpus.name}"
    try:
        m = master.start()
        r = replica.start()
        try:
            populate = reserve_and_populate(m, key, corpus)
            source_check = check_filter(m, key, corpus)
            replicaof = safe_cmd(r, "REPLICAOF", "127.0.0.1", str(master.port))
            repl_info = wait_replica_ready(r)
            check = wait_check(r, key, corpus, expected_card=source_check.get("card"))
            return {
                "populate": populate,
                "source_check": source_check,
                "replicaof": replicaof,
                "replication": {
                    "master_link_status": repl_info.get("master_link_status"),
                    "master_sync_in_progress": repl_info.get("master_sync_in_progress"),
                },
                "check": check,
                "critical_log_count": replica.critical_log_count(),
            }
        finally:
            m.close()
            r.close()
            replica.stop()
            master.stop()
    except Exception as exc:
        return {"error": str(exc), "master_log": master.log_tail(), "replica_log": replica.log_tail()}
    finally:
        replica.stop()
        master.stop()


def run_aof_self(redis_server: Path, module: Path, corpus: Corpus, preamble: bool, ports: PortAllocator, workdir: Path) -> dict[str, Any]:
    server = Server("gemini-aof-self", redis_server, module, ports.take(), workdir / corpus.name)
    key = f"aof_self_{'yes' if preamble else 'no'}_{corpus.name}"
    try:
        client = server.start(appendonly=True, aof_preamble=preamble)
        try:
            populate = reserve_and_populate(client, key, corpus)
            source_check = check_filter(client, key, corpus)
            wait_for_aof_rewrite(client)
        finally:
            client.close()
            server.stop()
        target = server.start(appendonly=True, preserve_dir=True, aof_preamble=preamble)
        try:
            check = check_filter(target, key, corpus, expected_card=source_check.get("card"))
            artifacts = [file_manifest(p) for p in sorted(server.workdir.rglob("*")) if p.is_file() and ("aof" in p.name.lower() or p.suffix == ".rdb")]
            return {
                "populate": populate,
                "source_check": source_check,
                "check": check,
                "artifacts": artifacts,
                "critical_log_count": server.critical_log_count(),
            }
        finally:
            target.close()
            server.stop()
    except Exception as exc:
        return {"error": str(exc), "log": server.log_tail()}
    finally:
        server.stop()


def run_scandump_self(redis_server: Path, module: Path, corpus: Corpus, ports: PortAllocator, workdir: Path) -> dict[str, Any]:
    server = Server("gemini-scandump-self", redis_server, module, ports.take(), workdir / corpus.name)
    src = f"sd_self_src_{corpus.name}"
    dst = f"sd_self_dst_{corpus.name}"
    try:
        client = server.start()
        try:
            populate = reserve_and_populate(client, src, corpus)
            source_check = check_filter(client, src, corpus)
            chunks = dump_chunks(client, src)
            load_replies = load_chunks(client, dst, chunks)
            check = check_filter(client, dst, corpus, expected_card=source_check.get("card"))
            chunks_path = workdir / f"{corpus.name}.chunks.json"
            chunks_path.write_text(json.dumps([[cur, 0 if data is None else len(data), hashlib.sha256(data or b'').hexdigest()] for cur, data in chunks], indent=2))
            return {
                "populate": populate,
                "source_check": source_check,
                "chunks": [[cur, 0 if data is None else len(data)] for cur, data in chunks],
                "load_replies": load_replies,
                "check": check,
                "artifact": file_manifest(chunks_path),
                "critical_log_count": server.critical_log_count(),
            }
        finally:
            client.close()
            server.stop()
    except Exception as exc:
        return {"error": str(exc), "log": server.log_tail()}
    finally:
        server.stop()


def run_scandump_safety(redis_server: Path, module: Path, ports: PortAllocator, workdir: Path) -> dict[str, Any]:
    corpus = Corpus("safety", ("0.01", "8", "EXPANSION", "1"), tuple(b(f"safety:{i}") for i in range(20)), "safety multi-layer")
    server = Server("gemini-scandump-safety", redis_server, module, ports.take(), workdir / "safety")
    try:
        client = server.start()
        try:
            reserve_and_populate(client, "src", corpus)
            chunks = dump_chunks(client, "src")
            header_cursor, header = chunks[0]
            load_header = safe_cmd(client, "BF.LOADCHUNK", "loading", header_cursor, header or b"")
            loading_rejections = {
                "add": safe_cmd(client, "BF.ADD", "loading", "x"),
                "madd": safe_cmd(client, "BF.MADD", "loading", "x", "y"),
                "insert": safe_cmd(client, "BF.INSERT", "loading", "NOCREATE", "ITEMS", "x"),
                "exists": safe_cmd(client, "BF.EXISTS", "loading", "x"),
                "mexists": safe_cmd(client, "BF.MEXISTS", "loading", "x", "y"),
                "info": safe_cmd(client, "BF.INFO", "loading"),
                "card": safe_cmd(client, "BF.CARD", "loading"),
                "scandump": safe_cmd(client, "BF.SCANDUMP", "loading", "0"),
            }
            bad_chunk = safe_cmd(client, "BF.LOADCHUNK", "loading", "2", b"short")
            finish = load_chunks(client, "loading", chunks[1:])
            completed_cursor2 = safe_cmd(client, "BF.LOADCHUNK", "loading", "2", b"\x00" * 16)
            safe_cmd(client, "SET", "string_key", "value")
            wrongtype_header = safe_cmd(client, "BF.LOADCHUNK", "string_key", "1", header or b"")
            safe_cmd(client, "BF.RESERVE", "existing", "0.01", "8")
            safe_cmd(client, "BF.ADD", "existing", "old")
            existing_header = safe_cmd(client, "BF.LOADCHUNK", "existing", "1", header or b"")
            existing_old = safe_cmd(client, "BF.EXISTS", "existing", "old")
            check = check_filter(client, "loading", corpus, expected_card=check_filter(client, "src", corpus).get("card"))
            return {
                "load_header": load_header,
                "loading_rejections": loading_rejections,
                "bad_chunk": bad_chunk,
                "finish": finish,
                "completed_cursor2": completed_cursor2,
                "wrongtype_header": wrongtype_header,
                "existing_header": existing_header,
                "existing_old_after_reject": existing_old,
                "check": check,
                "critical_log_count": server.critical_log_count(),
            }
        finally:
            client.close()
            server.stop()
    except Exception as exc:
        return {"error": str(exc), "log": server.log_tail()}
    finally:
        server.stop()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--redis-server", required=True, type=Path)
    parser.add_argument("--gemini-module", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--workdir", default=Path("/tmp/gemini-bloom-stage06-self"), type=Path)
    parser.add_argument("--include-large", action="store_true")
    args = parser.parse_args()

    shutil.rmtree(args.workdir, ignore_errors=True)
    args.workdir.mkdir(parents=True)
    ports = PortAllocator(52000)
    corpora = build_corpora(args.include_large)
    result: dict[str, Any] = {
        "env": {
            "redis_server": str(args.redis_server),
            "gemini_module": str(args.gemini_module),
            "workdir": str(args.workdir),
            "include_large": args.include_large,
        },
        "corpora": {},
        "scandump_safety": run_scandump_safety(args.redis_server, args.gemini_module, ports, args.workdir / "scandump_safety"),
    }
    for corpus in corpora:
        print(f"stage06 self {corpus.name}", flush=True)
        result["corpora"][corpus.name] = {
            "note": corpus.note,
            "rdb_self": run_rdb_self(args.redis_server, args.gemini_module, corpus, ports, args.workdir / "rdb"),
            "dump_restore_self": run_dump_restore_self(args.redis_server, args.gemini_module, corpus, ports, args.workdir / "dump_restore"),
            "fullsync_self": run_fullsync_self(args.redis_server, args.gemini_module, corpus, ports, args.workdir / "replication"),
            "aof_preamble_yes_self": run_aof_self(args.redis_server, args.gemini_module, corpus, True, ports, args.workdir / "aof_preamble_yes"),
            "aof_preamble_no_self": run_aof_self(args.redis_server, args.gemini_module, corpus, False, ports, args.workdir / "aof_preamble_no"),
            "scandump_self": run_scandump_self(args.redis_server, args.gemini_module, corpus, ports, args.workdir / "scandump_loadchunk"),
        }

    result["summary"] = {}
    for path_name in ["rdb_self", "dump_restore_self", "fullsync_self", "aof_preamble_yes_self", "aof_preamble_no_self", "scandump_self"]:
        counts = {"pass": 0, "fail": 0, "error": 0}
        for cell in result["corpora"].values():
            value = cell[path_name]
            if not isinstance(value, dict) or "error" in value:
                counts["error"] += 1
            elif check_ok(value):
                counts["pass"] += 1
            else:
                counts["fail"] += 1
        result["summary"][path_name] = counts
    safety = result["scandump_safety"]
    result["summary"]["scandump_safety"] = "pass" if isinstance(safety, dict) and "error" not in safety and check_ok(safety) else "fail"

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True))
    print(json.dumps(result["summary"], indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
