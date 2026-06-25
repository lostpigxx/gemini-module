#!/usr/bin/env python3
"""Additional RedisBloom compatibility probes for the gemini-bloom v5 audit.

The main matrix runner focuses on binary migration path families. This file
covers smaller but compatibility-relevant gaps: command metadata, readonly
replica behavior, incremental AOF command streams, MIGRATE, module load args,
and LOADCHUNK edge behavior.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import time
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
    check_passed,
    dump_chunks,
    json_safe,
    load_chunks,
    module_info,
    reserve_and_populate,
    safe_cmd,
    wait_check,
    wait_replica_ready,
)


EXTRA_BASE_DIR = BASE_DIR.with_name("gemini-bloom-v5-extended-audit")


def build_probe_corpora() -> dict[str, Corpus]:
    return {
        "single_layer": Corpus(
            "single_layer",
            ("0.01", "100"),
            tuple(b(f"single:{i}") for i in range(8)),
            "single-layer sanity corpus",
        ),
        "expansion1": Corpus(
            "expansion1",
            ("0.01", "5", "EXPANSION", "1"),
            tuple(b(f"exp1:{i}") for i in range(20)),
            "high false-positive corpus that exposes BF.CARD drift",
        ),
        "fixed_full": Corpus(
            "fixed_full",
            ("0.01", "2", "NONSCALING"),
            (b("fixed:a"), b("fixed:b")),
            "small fixed filter at capacity",
        ),
    }


def start_pair(
    env: CompatEnv,
    gemini_module: Path,
    ports: PortAllocator,
    workdir: Path,
) -> tuple[Server, Server, RedisClient, RedisClient]:
    gemini = Server("gemini", env.redis_server, gemini_module, ports.take(), workdir / "gemini")
    rb = Server("redisbloom", env.redis_server, env.redisbloom_module, ports.take(), workdir / "redisbloom")
    g = gemini.start()
    r = rb.start()
    return gemini, rb, g, r


def shutdown_clients(*clients: RedisClient) -> None:
    for client in clients:
        try:
            client.close()
        except Exception:
            pass


def stop_servers(*servers: Server) -> None:
    for server in servers:
        server.stop()


def command_info(client: RedisClient, name: str) -> Any:
    return safe_cmd(client, "COMMAND", "INFO", name)


def probe_command_registry(env: CompatEnv, gemini_module: Path, ports: PortAllocator, workdir: Path) -> dict[str, Any]:
    gemini, rb, g, r = start_pair(env, gemini_module, ports, workdir / "command_registry")
    try:
        result: dict[str, Any] = {
            "gemini_module": module_info(g),
            "redisbloom_module": module_info(r),
            "commands": {},
        }
        for cmd in ("BF.ADD", "BF.MADD", "BF.INSERT", "BF.INFO", "BF.CARD", "BF.SCANDUMP", "BF.LOADCHUNK", "BF.DEBUG"):
            result["commands"][cmd] = {
                "gemini": command_info(g, cmd),
                "redisbloom": command_info(r, cmd),
            }
        for client, label in ((g, "gemini"), (r, "redisbloom")):
            safe_cmd(client, "BF.RESERVE", "debug_key", "0.01", "10")
            safe_cmd(client, "BF.ADD", "debug_key", "alpha")
            result[f"{label}_bf_debug"] = safe_cmd(client, "BF.DEBUG", "debug_key")
            result[f"{label}_scandump_first"] = safe_cmd(client, "BF.SCANDUMP", "debug_key", "0")
        return result
    finally:
        shutdown_clients(g, r)
        stop_servers(gemini, rb)


def probe_readonly_scandump(
    env: CompatEnv,
    gemini_module: Path,
    ports: PortAllocator,
    workdir: Path,
) -> dict[str, Any]:
    corpus = build_probe_corpora()["single_layer"]
    result: dict[str, Any] = {}
    for label, module in (("gemini", gemini_module), ("redisbloom", env.redisbloom_module)):
        master = Server(f"{label}-master", env.redis_server, module, ports.take(), workdir / label / "master")
        replica = Server(f"{label}-replica", env.redis_server, module, ports.take(), workdir / label / "replica")
        key = f"readonly_{label}"
        try:
            master_client = master.start()
            replica_client = replica.start()
            try:
                populate = reserve_and_populate(master_client, key, corpus)
                source_check = check_filter(master_client, key, corpus)
                replicaof = safe_cmd(replica_client, "REPLICAOF", "127.0.0.1", str(master.port))
                repl_info = wait_replica_ready(replica_client)
                replica_check = wait_check(replica_client, key, corpus, expected_card=source_check.get("card"))
                scandump = safe_cmd(replica_client, "BF.SCANDUMP", key, "0")
                result[label] = {
                    "populate": populate,
                    "source_check": source_check,
                    "replicaof": replicaof,
                    "replication": {
                        "master_link_status": repl_info.get("master_link_status"),
                        "master_sync_in_progress": repl_info.get("master_sync_in_progress"),
                    },
                    "replica_check": replica_check,
                    "scandump_on_readonly_replica": scandump,
                }
            finally:
                shutdown_clients(master_client, replica_client)
        except Exception as exc:
            result[label] = {
                "error": str(exc),
                "master_log": master.log_tail(),
                "replica_log": replica.log_tail(),
            }
        finally:
            stop_servers(replica, master)
    return result


def probe_loadchunk_existing_key(
    env: CompatEnv,
    gemini_module: Path,
    ports: PortAllocator,
    workdir: Path,
) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for label, module in (("gemini", gemini_module), ("redisbloom", env.redisbloom_module)):
        server = Server(f"{label}-loadchunk-existing", env.redis_server, module, ports.take(), workdir / label)
        client = server.start()
        try:
            safe_cmd(client, "BF.RESERVE", "src", "0.01", "10")
            safe_cmd(client, "BF.ADD", "src", "new")
            safe_cmd(client, "BF.RESERVE", "dst", "0.01", "10")
            safe_cmd(client, "BF.ADD", "dst", "old")
            before = {
                "old": safe_cmd(client, "BF.EXISTS", "dst", "old"),
                "new": safe_cmd(client, "BF.EXISTS", "dst", "new"),
                "card": safe_cmd(client, "BF.CARD", "dst"),
            }
            chunks = dump_chunks(client, "src")
            header_cursor, header = chunks[0]
            load_header = safe_cmd(client, "BF.LOADCHUNK", "dst", header_cursor, header or b"")
            after_header = {
                "old": safe_cmd(client, "BF.EXISTS", "dst", "old"),
                "new": safe_cmd(client, "BF.EXISTS", "dst", "new"),
                "card": safe_cmd(client, "BF.CARD", "dst"),
            }
            remaining_replies = load_chunks(client, "dst", chunks[1:])
            after_all = {
                "old": safe_cmd(client, "BF.EXISTS", "dst", "old"),
                "new": safe_cmd(client, "BF.EXISTS", "dst", "new"),
                "card": safe_cmd(client, "BF.CARD", "dst"),
            }
            result[label] = {
                "chunks": [[cur, 0 if data is None else len(data)] for cur, data in chunks],
                "before": before,
                "load_header": load_header,
                "after_header": after_header,
                "remaining_load_replies": remaining_replies,
                "after_all": after_all,
            }
        except Exception as exc:
            result[label] = {"error": str(exc), "log": server.log_tail()}
        finally:
            shutdown_clients(client)
            stop_servers(server)
    return result


def copy_tree(src: Path, dst: Path) -> None:
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(src, dst)


def run_incremental_aof_one(
    env: CompatEnv,
    gemini_module: Path,
    corpus: Corpus,
    direction: str,
    ports: PortAllocator,
    workdir: Path,
) -> dict[str, Any]:
    source_is_rb = direction == "rb_to_gemini"
    source_module = env.redisbloom_module if source_is_rb else gemini_module
    target_module = gemini_module if source_is_rb else env.redisbloom_module
    src = Server("incremental-aof-source", env.redis_server, source_module, ports.take(), workdir / "src")
    dst = Server("incremental-aof-target", env.redis_server, target_module, ports.take(), workdir / "dst")
    key = f"iaof_{corpus.name}"
    try:
        client = src.start(appendonly=True)
        try:
            safe_cmd(client, "CONFIG", "SET", "appendfsync", "always")
            populate = reserve_and_populate(client, key, corpus)
            source_check = check_filter(client, key, corpus)
        finally:
            shutdown_clients(client)
            src.stop()
        copy_tree(src.workdir, dst.workdir)
        target = dst.start(appendonly=True, preserve_dir=True)
        try:
            check = check_filter(target, key, corpus, expected_card=source_check.get("card"))
            return {
                "populate": populate,
                "source_check": source_check,
                "check": check,
                "critical_log_count": dst.critical_log_count(),
            }
        finally:
            shutdown_clients(target)
            dst.stop()
    except Exception as exc:
        return {"error": str(exc), "source_log": src.log_tail(), "target_log": dst.log_tail()}
    finally:
        stop_servers(src, dst)


def probe_incremental_aof(
    env: CompatEnv,
    gemini_module: Path,
    ports: PortAllocator,
    workdir: Path,
) -> dict[str, Any]:
    corpora = build_probe_corpora()
    result: dict[str, Any] = {}
    for corpus_name in ("single_layer", "expansion1", "fixed_full"):
        corpus = corpora[corpus_name]
        result[corpus_name] = {}
        for direction in ("rb_to_gemini", "gemini_to_rb"):
            result[corpus_name][direction] = run_incremental_aof_one(
                env,
                gemini_module,
                corpus,
                direction,
                ports,
                workdir / f"{corpus_name}_{direction}",
            )
    return result


def probe_migrate_and_ttl(
    env: CompatEnv,
    gemini_module: Path,
    ports: PortAllocator,
    workdir: Path,
) -> dict[str, Any]:
    corpus = build_probe_corpora()["single_layer"]
    result: dict[str, Any] = {}
    for direction in ("rb_to_gemini", "gemini_to_rb"):
        source_is_rb = direction == "rb_to_gemini"
        source_module = env.redisbloom_module if source_is_rb else gemini_module
        target_module = gemini_module if source_is_rb else env.redisbloom_module
        source = Server("migrate-source", env.redis_server, source_module, ports.take(), workdir / direction / "source")
        target = Server("migrate-target", env.redis_server, target_module, ports.take(), workdir / direction / "target")
        key = f"migrate_{direction}"
        try:
            source_client = source.start()
            target_client = target.start()
            try:
                populate = reserve_and_populate(source_client, key, corpus)
                expire = safe_cmd(source_client, "PEXPIRE", key, "600000")
                source_check = check_filter(source_client, key, corpus)
                source_pttl = safe_cmd(source_client, "PTTL", key)
                migrate = safe_cmd(
                    source_client,
                    "MIGRATE",
                    "127.0.0.1",
                    str(target.port),
                    key,
                    "0",
                    "5000",
                    "COPY",
                    "REPLACE",
                )
                target_check = check_filter(target_client, key, corpus, expected_card=source_check.get("card"))
                target_pttl = safe_cmd(target_client, "PTTL", key)
                dumped = safe_cmd(source_client, "DUMP", key)
                restore_key = f"restore_ttl_{direction}"
                restore = (
                    safe_cmd(target_client, "RESTORE", restore_key, "600000", dumped, "REPLACE")
                    if isinstance(dumped, bytes)
                    else {"error": f"DUMP did not return bytes: {dumped!r}"}
                )
                restore_check = check_filter(target_client, restore_key, corpus, expected_card=source_check.get("card"))
                restore_pttl = safe_cmd(target_client, "PTTL", restore_key)
                result[direction] = {
                    "populate": populate,
                    "expire": expire,
                    "source_check": source_check,
                    "source_pttl": source_pttl,
                    "migrate": migrate,
                    "target_check": target_check,
                    "target_pttl": target_pttl,
                    "dump_len": len(dumped) if isinstance(dumped, bytes) else None,
                    "restore": restore,
                    "restore_check": restore_check,
                    "restore_pttl": restore_pttl,
                }
            finally:
                shutdown_clients(source_client, target_client)
        except Exception as exc:
            result[direction] = {
                "error": str(exc),
                "source_log": source.log_tail(),
                "target_log": target.log_tail(),
            }
        finally:
            stop_servers(source, target)
    return result


def run_scripted_commands(client: RedisClient) -> dict[str, Any]:
    result: dict[str, Any] = {}
    result["missing_info"] = safe_cmd(client, "BF.INFO", "missing")
    result["missing_scandump"] = safe_cmd(client, "BF.SCANDUMP", "missing", "0")
    result["missing_loadchunk_data"] = safe_cmd(client, "BF.LOADCHUNK", "missing", "2", b"x")
    result["missing_card"] = safe_cmd(client, "BF.CARD", "missing")
    result["reserve_expansion_zero"] = safe_cmd(client, "BF.RESERVE", "reserve_exp0", "0.01", "10", "EXPANSION", "0")
    result["reserve_unknown_option"] = safe_cmd(client, "BF.RESERVE", "reserve_unknown", "0.01", "10", "UNKNOWN", "1")
    result["reserve_nonscaling_expansion"] = safe_cmd(
        client,
        "BF.RESERVE",
        "reserve_both",
        "0.01",
        "10",
        "NONSCALING",
        "EXPANSION",
        "2",
    )
    result["insert_expansion_zero"] = safe_cmd(client, "BF.INSERT", "insert_exp0", "EXPANSION", "0", "ITEMS", "a")
    result["insert_unknown_option"] = safe_cmd(client, "BF.INSERT", "insert_unknown", "UNKNOWN", "ITEMS", "a")
    result["insert_nocreate"] = safe_cmd(client, "BF.INSERT", "insert_missing", "NOCREATE", "ITEMS", "a")
    result["madd_duplicate"] = safe_cmd(client, "BF.MADD", "dup", "a", "a")
    result["dup_card"] = safe_cmd(client, "BF.CARD", "dup")
    safe_cmd(client, "SET", "string_key", "value")
    result["wrongtype_add"] = safe_cmd(client, "BF.ADD", "string_key", "a")
    result["wrongtype_info"] = safe_cmd(client, "BF.INFO", "string_key")
    result["wrongtype_card"] = safe_cmd(client, "BF.CARD", "string_key")
    result["wrongtype_scandump"] = safe_cmd(client, "BF.SCANDUMP", "string_key", "0")
    safe_cmd(client, "BF.RESERVE", "info_key", "0.01", "10")
    result["info_unknown_field"] = safe_cmd(client, "BF.INFO", "info_key", "unknown")
    result["info_filters"] = safe_cmd(client, "BF.INFO", "info_key", "FILTERS")
    return result


def probe_command_semantics(env: CompatEnv, gemini_module: Path, ports: PortAllocator, workdir: Path) -> dict[str, Any]:
    gemini, rb, g, r = start_pair(env, gemini_module, ports, workdir / "command_semantics")
    try:
        return {
            "gemini": run_scripted_commands(g),
            "redisbloom": run_scripted_commands(r),
        }
    finally:
        shutdown_clients(g, r)
        stop_servers(gemini, rb)


def start_with_module_args(
    label: str,
    redis_server: Path,
    module: Path,
    port: int,
    workdir: Path,
    module_args: tuple[str, ...],
) -> dict[str, Any]:
    shutil.rmtree(workdir, ignore_errors=True)
    workdir.mkdir(parents=True)
    args = [
        str(redis_server),
        "--bind",
        "127.0.0.1",
        "--port",
        str(port),
        "--daemonize",
        "yes",
        "--loglevel",
        "notice",
        "--logfile",
        str(workdir / "redis.log"),
        "--dir",
        str(workdir),
        "--dbfilename",
        "dump.rdb",
        "--save",
        "",
        "--appendonly",
        "no",
        "--loadmodule",
        str(module),
        *module_args,
    ]
    proc = subprocess.run(args, text=True, capture_output=True)
    server = Server(label, redis_server, module, port, workdir)
    if proc.returncode != 0:
        return {
            "started": False,
            "returncode": proc.returncode,
            "stdout": proc.stdout,
            "stderr": proc.stderr,
            "log": server.log_tail(),
        }

    deadline = time.time() + 8
    last_error = None
    client = None
    while time.time() < deadline:
        try:
            client = RedisClient(port)
            if client.cmd("PING") == "PONG":
                break
            client.close()
            client = None
        except Exception as exc:
            last_error = str(exc)
            time.sleep(0.05)
    if client is None:
        return {
            "started": False,
            "returncode": proc.returncode,
            "stdout": proc.stdout,
            "stderr": proc.stderr,
            "last_error": last_error,
            "log": server.log_tail(),
        }

    try:
        add = safe_cmd(client, "BF.ADD", "default_key", "alpha")
        info = safe_cmd(client, "BF.INFO", "default_key")
        return {
            "started": True,
            "module_info": module_info(client),
            "default_add": add,
            "default_info": info,
        }
    finally:
        shutdown_clients(client)
        server.stop()


def probe_module_load_args(
    env: CompatEnv,
    gemini_module: Path,
    ports: PortAllocator,
    workdir: Path,
) -> dict[str, Any]:
    cases = {
        "redisbloom_common_bf_defaults": ("INITIAL_SIZE", "7", "ERROR_RATE", "0.05"),
        "gemini_only_expansion_default": ("EXPANSION", "4"),
        "redisbloom_cf_default_arg": ("CF_MAX_EXPANSIONS", "8"),
    }
    result: dict[str, Any] = {}
    for case_name, module_args in cases.items():
        result[case_name] = {}
        for label, module in (("gemini", gemini_module), ("redisbloom", env.redisbloom_module)):
            result[case_name][label] = start_with_module_args(
                f"{label}-{case_name}",
                env.redis_server,
                module,
                ports.take(),
                workdir / case_name / label,
                module_args,
            )
    return result


def summarize(result: dict[str, Any]) -> dict[str, Any]:
    summary: dict[str, Any] = {}
    env_result = next(iter(result["envs"].values()))

    command_registry = env_result["command_registry"]
    summary["bf_debug"] = {
        "gemini": "error" not in command_registry.get("gemini_bf_debug", {}),
        "redisbloom": "error" not in command_registry.get("redisbloom_bf_debug", {}),
    }

    readonly = env_result["readonly_scandump"]
    summary["readonly_scandump_ok"] = {
        label: not isinstance(value.get("scandump_on_readonly_replica"), dict)
        for label, value in readonly.items()
        if isinstance(value, dict)
    }

    loadchunk = env_result["loadchunk_existing_key"]
    summary["loadchunk_header_over_existing_key"] = {
        label: value.get("load_header")
        for label, value in loadchunk.items()
        if isinstance(value, dict)
    }

    iaof_counts = {"pass": 0, "fail": 0, "error": 0}
    for corpus_result in env_result["incremental_aof"].values():
        for cell in corpus_result.values():
            if not isinstance(cell, dict) or "error" in cell:
                iaof_counts["error"] += 1
            elif cell.get("critical_log_count", 0) > 0 or not check_passed(cell.get("check")):
                iaof_counts["fail"] += 1
            else:
                iaof_counts["pass"] += 1
    summary["incremental_aof"] = iaof_counts

    migrate_counts = {"pass": 0, "fail": 0, "error": 0}
    for cell in env_result["migrate_and_ttl"].values():
        if not isinstance(cell, dict) or "error" in cell:
            migrate_counts["error"] += 1
        elif (
            cell.get("migrate") != "OK"
            or cell.get("restore") != "OK"
            or not check_passed(cell.get("target_check"))
            or not check_passed(cell.get("restore_check"))
            or not isinstance(cell.get("target_pttl"), int)
            or cell.get("target_pttl", -1) <= 0
            or not isinstance(cell.get("restore_pttl"), int)
            or cell.get("restore_pttl", -1) <= 0
        ):
            migrate_counts["fail"] += 1
        else:
            migrate_counts["pass"] += 1
    summary["migrate_and_dump_restore_ttl"] = migrate_counts
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gemini-module", required=True, type=Path)
    parser.add_argument("--redis-server", required=True, type=Path)
    parser.add_argument("--redisbloom-module", required=True, type=Path)
    parser.add_argument("--env-name", default="redis-6.2-redisbloom-v2.4.20")
    parser.add_argument("--redis-tag", default="6.2.17")
    parser.add_argument("--redisbloom-tag", default="v2.4.20")
    parser.add_argument("--module-ver", default="20420")
    parser.add_argument("--base-port", default=49500, type=int)
    parser.add_argument(
        "--output",
        default=Path("doc/code_review/gemini-bloom/v5/extended_audit_results_redis62_redisbloom2420.json"),
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
    workdir = EXTRA_BASE_DIR / args.env_name
    shutil.rmtree(workdir, ignore_errors=True)
    workdir.mkdir(parents=True)

    env_result = {
        "env": {
            "name": env.name,
            "redis_tag": env.redis_tag,
            "redisbloom_tag": env.redisbloom_tag,
            "module_ver": env.module_ver,
            "redis_server": str(env.redis_server),
            "redisbloom_module": str(env.redisbloom_module),
            "gemini_module": str(args.gemini_module),
        },
        "command_registry": probe_command_registry(env, args.gemini_module, ports, workdir),
        "readonly_scandump": probe_readonly_scandump(env, args.gemini_module, ports, workdir / "readonly_scandump"),
        "loadchunk_existing_key": probe_loadchunk_existing_key(env, args.gemini_module, ports, workdir / "loadchunk_existing_key"),
        "incremental_aof": probe_incremental_aof(env, args.gemini_module, ports, workdir / "incremental_aof"),
        "migrate_and_ttl": probe_migrate_and_ttl(env, args.gemini_module, ports, workdir / "migrate_and_ttl"),
        "command_semantics": probe_command_semantics(env, args.gemini_module, ports, workdir),
        "module_load_args": probe_module_load_args(env, args.gemini_module, ports, workdir / "module_load_args"),
    }
    result = {"envs": {env.name: env_result}}
    result["summary"] = summarize(result)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(json_safe(result), indent=2, sort_keys=True) + "\n")
    print(json.dumps(result["summary"], indent=2, sort_keys=True))
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
