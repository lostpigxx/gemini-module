#!/usr/bin/env python3
"""Partial-load (BF.LOADCHUNK) persistence regression tests.

A filter that has received its SCANDUMP header but not all of its layer
chunks is in Loading state. Persisting it as a *complete* filter is a
data-loss bug: the header declares an item count, but the missing layers
are zero-filled, so the restored filter reports cardinality while
answering BF.EXISTS with false negatives.

These tests cover the three persistence paths:

  command-aof   appendonly yes, aof-use-rdb-preamble no
  rdb-preamble  appendonly yes, aof-use-rdb-preamble yes  (Redis 6/7 default)
  rdb           BGSAVE snapshot, no AOF

For the two AOF paths the remaining chunks are applied *after* the rewrite
fork, so they land in the incremental tail. Restoring the base and replaying
the tail must produce a complete, correct filter.
"""

import argparse
import os
import shutil
import sys
import tempfile
import time

import redis

from bloom_test_support import (
    info_dict,
    start_redis,
    stop_redis,
    wait_for_bgsave,
)

ITEM_COUNT = int(os.environ.get("PARTIAL_LOAD_ITEMS", "100"))
PADDING_KEYS = int(os.environ.get("PARTIAL_LOAD_PADDING", "30000"))
SOURCE_KEY = b"partial:source"
TARGET_KEY = b"partial:target"

AOF_ERROR_MARKERS = (
    "received bad data",
    "Bad data format",
    "filter is being loaded",
)


def items():
    return [f"partial-item:{index:06d}".encode() for index in range(ITEM_COUNT)]


def scandump(conn, key):
    """Return the full [(cursor, payload)] chunk list for key."""
    chunks = []
    cursor = 0
    while True:
        cursor, payload = conn.execute_command("BF.SCANDUMP", key, cursor)
        cursor = int(cursor)
        if cursor == 0:
            return chunks
        chunks.append((cursor, payload))


def build_source(conn, capacity, error_rate, expansion=None):
    args = ["BF.RESERVE", SOURCE_KEY, error_rate, capacity]
    if expansion is not None:
        args += ["EXPANSION", expansion]
    conn.execute_command(*args)
    conn.execute_command("BF.MADD", SOURCE_KEY, *items())
    return scandump(conn, SOURCE_KEY)


def add_padding(conn):
    """Enough keys that the rewrite child stays alive long enough to race."""
    pipe = conn.pipeline(transaction=False)
    for index in range(PADDING_KEYS):
        pipe.set(f"pad:{index:07d}".encode(), b"x" * 64)
        if index % 5000 == 4999:
            pipe.execute()
            pipe = conn.pipeline(transaction=False)
    pipe.execute()


def rewrite_in_progress(conn):
    return bool(info_dict(conn.info("persistence")).get("aof_rewrite_in_progress", 0))


def wait_for_rewrite(conn, timeout=120):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        info = info_dict(conn.info("persistence"))
        if not info.get("aof_rewrite_in_progress", 0):
            return info.get("aof_last_bgrewrite_status")
        time.sleep(0.02)
    raise RuntimeError("BGREWRITEAOF did not finish in time")


def apply_chunks_during_rewrite(conn, chunks):
    """Start BGREWRITEAOF and apply chunks strictly inside the fork window.

    Returns the rewrite status. Raises if the race could not be won, because
    a chunk that landed in the *base* file would make the test pass vacuously.
    """
    conn.execute_command("BGREWRITEAOF")

    deadline = time.monotonic() + 30
    while not rewrite_in_progress(conn):
        if time.monotonic() > deadline:
            raise RuntimeError("rewrite never reported in progress")
        time.sleep(0.001)

    if not rewrite_in_progress(conn):
        raise RuntimeError("rewrite finished before chunks were applied")
    for cursor, payload in chunks:
        conn.execute_command("BF.LOADCHUNK", TARGET_KEY, cursor, payload)
    if not rewrite_in_progress(conn):
        raise RuntimeError(
            "rewrite finished while chunks were being applied; "
            "increase PARTIAL_LOAD_PADDING so the post-fork window stays open"
        )

    return wait_for_rewrite(conn)


def assert_complete(conn, label):
    """The restored filter must answer every inserted item with no false negatives."""
    if not conn.exists(TARGET_KEY):
        raise AssertionError(f"{label}: key vanished after restart")

    try:
        card = int(conn.execute_command("BF.CARD", TARGET_KEY))
    except redis.ResponseError as exc:
        raise AssertionError(f"{label}: filter still unusable after restart: {exc}")

    result = conn.execute_command("BF.MEXISTS", TARGET_KEY, *items())
    misses = [index for index, value in enumerate(result) if value != 1]
    if misses:
        raise AssertionError(
            f"{label}: {len(misses)} false negatives after restart "
            f"(card={card}, first_missing={misses[:5]})"
        )
    if card != ITEM_COUNT:
        raise AssertionError(f"{label}: BF.CARD is {card}, expected {ITEM_COUNT}")


def assert_still_loading(conn, label):
    """An incomplete filter must stay incomplete, never a lying complete one."""
    if not conn.exists(TARGET_KEY):
        return  # dropping the half-built key is also honest
    try:
        card = conn.execute_command("BF.CARD", TARGET_KEY)
    except redis.ResponseError:
        return  # still Loading and correctly rejecting: the desired outcome

    result = conn.execute_command("BF.MEXISTS", TARGET_KEY, *items())
    misses = [index for index, value in enumerate(result) if value != 1]
    raise AssertionError(
        f"{label}: incomplete filter was restored as a queryable filter "
        f"(BF.CARD={card}) with {len(misses)} false negatives"
    )


def check_log(logfile, label):
    with open(logfile, errors="replace") as handle:
        text = handle.read()
    hits = [marker for marker in AOF_ERROR_MARKERS if marker in text]
    if hits:
        raise AssertionError(f"{label}: persistence replay logged {hits}")


def run_aof_case(module, use_rdb_preamble, preloaded_chunks, capacity,
                 expansion, label):
    """Header (+ optional prefix of layers) before the fork, rest after it."""
    directory = tempfile.mkdtemp(prefix="bloom-partial-")
    proc = conn = None
    try:
        redis_args = [
            "--appendonly", "yes",
            "--appendfilename", "appendonly.aof",
            "--appendfsync", "always",
            "--aof-use-rdb-preamble", "yes" if use_rdb_preamble else "no",
        ]
        proc, conn, logfile = start_redis(module, directory, redis_args)

        chunks = build_source(conn, capacity, 0.01, expansion)
        if len(chunks) < 2 + preloaded_chunks:
            raise RuntimeError(
                f"{label}: source produced only {len(chunks)} chunks; "
                "need a header plus enough layers for this case"
            )

        # Header, then however many layers this case wants applied pre-fork.
        for cursor, payload in chunks[: 1 + preloaded_chunks]:
            conn.execute_command("BF.LOADCHUNK", TARGET_KEY, cursor, payload)

        add_padding(conn)
        status = apply_chunks_during_rewrite(conn, chunks[1 + preloaded_chunks:])
        if status != "ok":
            raise AssertionError(f"{label}: rewrite status {status!r}")

        # Live server must already answer correctly.
        live = conn.execute_command("BF.MEXISTS", TARGET_KEY, *items())
        if any(value != 1 for value in live):
            raise AssertionError(f"{label}: live filter has false negatives")

        stop_redis(proc, conn)
        proc = None

        proc, conn, logfile2 = start_redis(module, directory, redis_args)
        assert_complete(conn, label)
        check_log(logfile2, label)
    finally:
        if proc is not None:
            stop_redis(proc, conn)
        shutil.rmtree(directory, ignore_errors=True)
    print(f"  PASS  {label}")


def run_rdb_case(module, capacity, label):
    """A header-only filter snapshotted by BGSAVE must not come back complete."""
    directory = tempfile.mkdtemp(prefix="bloom-partial-rdb-")
    proc = conn = None
    try:
        proc, conn, _ = start_redis(module, directory, [])
        chunks = build_source(conn, capacity, 0.01)
        header_cursor, header = chunks[0]
        conn.execute_command("BF.LOADCHUNK", TARGET_KEY, header_cursor, header)
        wait_for_bgsave(conn)
        stop_redis(proc, conn)
        proc = None

        proc, conn, logfile = start_redis(module, directory, [])
        assert_still_loading(conn, label)
        check_log(logfile, label)

        # And it must still be completable from the remaining chunks.
        for cursor, payload in chunks[1:]:
            conn.execute_command("BF.LOADCHUNK", TARGET_KEY, cursor, payload)
        assert_complete(conn, f"{label} (resumed)")
    finally:
        if proc is not None:
            stop_redis(proc, conn)
        shutil.rmtree(directory, ignore_errors=True)
    print(f"  PASS  {label}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("module")
    args = parser.parse_args()
    module = os.path.abspath(args.module)

    print(f"partial-load persistence tests ({ITEM_COUNT} items, "
          f"{PADDING_KEYS} padding keys)")

    failures = []
    cases = [
        ("command-AOF, header-only before fork",
         lambda: run_aof_case(module, False, 0, 1000, None,
                              "command-AOF header-only")),
        ("command-AOF, multi-layer partial prefix",
         lambda: run_aof_case(module, False, 1, 20, 2,
                              "command-AOF partial prefix")),
        ("RDB-preamble AOF, header-only before fork",
         lambda: run_aof_case(module, True, 0, 1000, None,
                              "RDB-preamble header-only")),
        ("RDB-preamble AOF, multi-layer partial prefix",
         lambda: run_aof_case(module, True, 1, 20, 2,
                              "RDB-preamble partial prefix")),
        ("RDB snapshot of a header-only filter",
         lambda: run_rdb_case(module, 1000, "RDB header-only")),
    ]

    for name, case in cases:
        try:
            case()
        except (AssertionError, RuntimeError, redis.RedisError) as exc:
            failures.append(name)
            print(f"  FAIL  {name}: {exc}")

    if failures:
        print(f"\nFAILED {len(failures)}/{len(cases)}: {failures}")
        return 1
    print(f"\nPASS: all {len(cases)} partial-load persistence cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
