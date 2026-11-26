#!/usr/bin/env python3
"""
End-to-end client SDK compatibility test using official redis-py high-level APIs
(bf, cf, cms, topk, tdigest, json, ft) across both RESP2 and RESP3.

Usage:
  python3 ci/sdk_compat_test.py [bloom.so] [json.so] [search.so]
"""

import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import typing

if not hasattr(typing, "Self"):
    typing.Self = typing.Any  # Python 3.10 compat for redis-py 8.x

import redis
from redis.commands.json.path import Path
from redis.commands.search.aggregation import AggregateRequest
from redis.commands.search import reducers
from redis.commands.search.field import (
    GeoField,
    NumericField,
    TagField,
    TextField,
    VectorField,
)
from redis.commands.search.index_definition import IndexDefinition, IndexType
from redis.commands.search.query import Query
from redis.commands.search.suggestion import Suggestion


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def start_redis(port: int, modules: list[str], workdir: str) -> subprocess.Popen:
    cmd = [
        "redis-server",
        "--port", str(port),
        "--dir", workdir,
        "--dbfilename", "",
        "--save", "",
        "--loglevel", "warning",
    ]
    for m in modules:
        cmd.extend(["--loadmodule", os.path.abspath(m)])
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    deadline = time.time() + 10.0
    while time.time() < deadline:
        try:
            c = redis.Redis(host="127.0.0.1", port=port)
            if c.ping():
                c.close()
                return proc
        except Exception:
            time.sleep(0.1)
    proc.terminate()
    out, err = proc.communicate(timeout=5)
    raise RuntimeError(f"redis-server failed to start:\n{out.decode()}\n{err.decode()}")


def info_get(info, attr_name: str, *key_names: str):
    if hasattr(info, attr_name):
        return getattr(info, attr_name)
    if isinstance(info, dict):
        for k in key_names:
            if k in info:
                return info[k]
    if isinstance(info, list):
        for i in range(0, len(info) - 1, 2):
            if info[i] in key_names:
                return info[i + 1]
    raise AssertionError(f"Cannot find {attr_name}/{key_names} in {info!r}")


def test_bloom_sdk(r: redis.Redis) -> None:
    # 1. Bloom Filter
    bf = r.bf()
    assert bf.reserve("sdk:bf", 0.01, 1000) in (True, "OK", b"OK")
    assert bf.add("sdk:bf", "apple") == 1
    assert bf.add("sdk:bf", "apple") == 0
    assert bf.madd("sdk:bf", "banana", "cherry", "apple") == [1, 1, 0]
    assert bf.exists("sdk:bf", "apple") == 1
    assert bf.exists("sdk:bf", "missing") == 0
    assert bf.mexists("sdk:bf", "apple", "banana", "missing") == [1, 1, 0]
    assert bf.card("sdk:bf") == 3
    bf_info = bf.info("sdk:bf")
    assert int(info_get(bf_info, "insertedNum", "Number of items inserted")) == 3
    assert int(info_get(bf_info, "capacity", "Capacity")) >= 1000

    # SCANDUMP / LOADCHUNK round-trip
    it = 0
    while True:
        it, data = bf.scandump("sdk:bf", it)
        if it == 0:
            break
        bf.loadchunk("sdk:bf2", it, data)
    assert bf.mexists("sdk:bf2", "apple", "banana", "cherry", "missing") == [1, 1, 1, 0]

    # 2. Cuckoo Filter
    cf = r.cf()
    assert cf.reserve("sdk:cf", 1000) in (True, "OK", b"OK")
    assert cf.add("sdk:cf", "x") == 1
    assert cf.addnx("sdk:cf", "x") == 0
    assert cf.addnx("sdk:cf", "y") == 1
    assert cf.exists("sdk:cf", "x") == 1
    assert cf.mexists("sdk:cf", "x", "y", "z") == [1, 1, 0]
    assert cf.count("sdk:cf", "x") == 1
    assert cf.delete("sdk:cf", "x") == 1
    assert cf.exists("sdk:cf", "x") == 0
    cf_info = cf.info("sdk:cf")
    assert int(info_get(cf_info, "insertedNum", "Number of items inserted")) >= 1

    # 3. Count-Min Sketch
    cms = r.cms()
    assert cms.initbydim("sdk:cms1", 200, 5) in (True, "OK", b"OK")
    assert cms.initbydim("sdk:cms2", 200, 5) in (True, "OK", b"OK")
    assert cms.incrby("sdk:cms1", ["foo", "bar"], [3, 5]) == [3, 5]
    assert cms.incrby("sdk:cms2", ["foo", "baz"], [2, 7]) == [2, 7]
    assert cms.query("sdk:cms1", "foo", "bar", "none") == [3, 5, 0]
    assert cms.initbydim("sdk:cms_dst", 200, 5) in (True, "OK", b"OK")
    assert cms.merge("sdk:cms_dst", 2, ["sdk:cms1", "sdk:cms2"], [1, 2]) in (True, "OK", b"OK")
    assert cms.query("sdk:cms_dst", "foo", "bar", "baz") == [7, 5, 14]
    cms_info = cms.info("sdk:cms_dst")
    assert int(info_get(cms_info, "width", "width")) == 200
    assert int(info_get(cms_info, "depth", "depth")) == 5

    # 4. Top-K
    tk = r.topk()
    assert tk.reserve("sdk:tk", 3, 50, 4, 0.9) in (True, "OK", b"OK")
    tk.incrby("sdk:tk", ["hot", "warm", "cold", "tiny"], [100, 50, 20, 1])
    assert tk.query("sdk:tk", "hot", "warm", "tiny")[0] == 1
    counts = tk.count("sdk:tk", "hot", "warm")
    assert counts[0] >= 100 and counts[1] >= 50
    top_items = tk.list("sdk:tk")
    assert "hot" in [x.decode() if isinstance(x, bytes) else x for x in top_items]
    tk_info = tk.info("sdk:tk")
    assert int(info_get(tk_info, "k", "k")) == 3

    # 5. T-Digest
    td = r.tdigest()
    assert td.create("sdk:td", 100) in (True, "OK", b"OK")
    assert td.add("sdk:td", list(range(1, 101))) in (True, "OK", b"OK")
    q50 = td.quantile("sdk:td", 0.5)[0]
    assert 45.0 <= float(q50) <= 56.0, f"unexpected median {q50}"
    assert float(td.min("sdk:td")) == 1.0
    assert float(td.max("sdk:td")) == 100.0
    cdf_val = td.cdf("sdk:td", 50.0)[0]
    assert 0.4 <= float(cdf_val) <= 0.6
    tm = td.trimmed_mean("sdk:td", 0.1, 0.9)
    assert 45.0 <= float(tm) <= 56.0
    td_info = td.info("sdk:td")
    assert int(info_get(td_info, "compression", "Compression")) == 100


def as_json_val(v):
    if isinstance(v, (str, bytes)):
        return json.loads(v)
    return v


def test_json_sdk(r: redis.Redis) -> None:
    j = r.json()
    doc = {
        "user": {"name": "Alice", "age": 30, "active": True},
        "tags": ["redis", "cpp"],
        "items": [{"id": 1, "price": 10}, {"id": 2, "price": 50}],
    }
    assert j.set("sdk:doc:1", "$", doc) in (True, "OK", b"OK")
    got = as_json_val(j.get("sdk:doc:1", "$"))
    assert isinstance(got, list) and got[0]["user"]["name"] == "Alice"

    assert as_json_val(j.numincrby("sdk:doc:1", "$.user.age", 5)) == [35]
    assert j.toggle("sdk:doc:1", "$.user.active") in ([0], [False])
    assert j.arrappend("sdk:doc:1", "$.tags", "search") == [3]
    assert j.arrlen("sdk:doc:1", "$.tags") == [3]
    assert j.arrindex("sdk:doc:1", "$.tags", "cpp") == [1]
    assert j.strappend("sdk:doc:1", " Smith", "$.user.name") == [11]
    assert j.strlen("sdk:doc:1", "$.user.name") == [11]
    assert sorted(j.objkeys("sdk:doc:1", "$.user")[0]) == ["active", "age", "name"]
    assert j.objlen("sdk:doc:1", "$.user") == [3]

    # Merge + MGET + filter
    assert j.merge("sdk:doc:1", "$.user", {"role": "admin"}) in (True, "OK", b"OK")
    assert as_json_val(j.get("sdk:doc:1", "$.user.role")) == ["admin"]
    assert j.set("sdk:doc:2", "$", {"user": {"name": "Bob"}}) in (True, "OK", b"OK")
    mget_res = [as_json_val(x) for x in j.mget(["sdk:doc:1", "sdk:doc:2"], "$.user.name")]
    assert mget_res == [["Alice Smith"], ["Bob"]]

    expensive = as_json_val(j.get("sdk:doc:1", "$.items[?(@.price > 20)].id"))
    assert expensive == [2]

    assert j.delete("sdk:doc:1", "$.user.role") == 1
    assert j.clear("sdk:doc:1", "$.tags") == 1
    assert j.arrlen("sdk:doc:1", "$.tags") == [0]


def search_total_and_ids(res) -> tuple[int, list[str]]:
    if hasattr(res, "total") and hasattr(res, "docs"):
        return int(res.total), [d.id for d in res.docs]
    if isinstance(res, dict):
        return int(res["total_results"]), [d["id"] for d in res["results"]]
    if isinstance(res, list) and len(res) >= 1:
        total = int(res[0])
        ids = []
        i = 1
        while i < len(res):
            ids.append(res[i])
            i += 2  # skip fields list
        return total, ids
    raise AssertionError(f"Unexpected FT.SEARCH response: {res!r}")


def agg_row_count(res) -> int:
    if hasattr(res, "rows"):
        return len(res.rows)
    if isinstance(res, dict) and "results" in res:
        return len(res["results"])
    if isinstance(res, list) and len(res) >= 1:
        return len(res) - 1
    raise AssertionError(f"Unexpected FT.AGGREGATE response: {res!r}")


def test_search_sdk(r: redis.Redis) -> None:
    # 1. ON HASH index with TEXT, TAG, NUMERIC, GEO, VECTOR (HNSW)
    ft = r.ft("sdk:idx")
    schema = (
        TextField("title", weight=2.0),
        TagField("cat"),
        NumericField("price", sortable=True),
        GeoField("loc"),
        VectorField(
            "vec",
            "HNSW",
            {
                "TYPE": "FLOAT32",
                "DIM": 4,
                "DISTANCE_METRIC": "COSINE",
            },
        ),
    )
    ft.create_index(
        schema,
        definition=IndexDefinition(prefix=["sdk:p:"], index_type=IndexType.HASH),
    )

    v1 = struct.pack("<4f", 1.0, 0.0, 0.0, 0.0)
    v2 = struct.pack("<4f", 0.0, 1.0, 0.0, 0.0)
    v3 = struct.pack("<4f", 0.9, 0.1, 0.0, 0.0)
    r.hset("sdk:p:1", mapping={"title": "fast redis search engine", "cat": "db,tech", "price": "100", "loc": "-122.4,37.7", "vec": v1})
    r.hset("sdk:p:2", mapping={"title": "modern vector database", "cat": "ai,db", "price": "250", "loc": "-122.41,37.71", "vec": v2})
    r.hset("sdk:p:3", mapping={"title": "redis fulltext engine", "cat": "db", "price": "50", "loc": "116.4,39.9", "vec": v3})

    # Basic text + sort + paging
    total, ids = search_total_and_ids(ft.search(Query("redis").sort_by("price", asc=True).paging(0, 10)))
    assert total == 2 and ids == ["sdk:p:3", "sdk:p:1"], f"got {total}, {ids}"

    # Tag + Numeric + Geo
    t_tag, id_tag = search_total_and_ids(ft.search(Query("@cat:{ai} @price:[200 300]")))
    assert t_tag == 1 and id_tag == ["sdk:p:2"]
    t_geo, _ = search_total_and_ids(ft.search(Query("@loc:[-122.4 37.7 5 km]")))
    assert t_geo == 2

    # KNN query via redis-py Query + query_params
    q_knn = (
        Query("*=>[KNN 2 @vec $qv AS score]")
        .return_fields("title", "score")
        .dialect(2)
    )
    t_knn, id_knn = search_total_and_ids(ft.search(q_knn, query_params={"qv": v1}))
    assert t_knn == 2 and id_knn[0] == "sdk:p:1"

    # Aggregation
    agg_req = (
        AggregateRequest("*")
        .group_by(["@cat"], reducers.count().alias("cnt"), reducers.avg("@price").alias("avg_p"))
        .sort_by("@avg_p", max=10)
    )
    assert agg_row_count(ft.aggregate(agg_req)) >= 2

    # Info, Tagvals, Aliases, Synonyms, Autocomplete
    info = ft.info()
    assert int(info_get(info, "num_docs", "num_docs")) == 3
    assert int(info_get(info, "indexing", "indexing")) == 0
    tags = ft.tagvals("cat")
    assert "db" in [t.decode() if isinstance(t, bytes) else t for t in tags]

    ft.aliasadd("sdk:alias")
    assert search_total_and_ids(r.ft("sdk:alias").search(Query("redis")))[0] == 2
    ft.aliasdel("sdk:alias")

    ft.sugadd("sdk:sug", Suggestion("redis search", 10.0))
    ft.sugadd("sdk:sug", Suggestion("redis json", 5.0))
    sugs = ft.sugget("sdk:sug", "red")
    assert len(sugs) == 2

    ft.synupdate("g1", False, "fast", "quick")
    assert search_total_and_ids(ft.search(Query("quick")))[0] == 1

    # 2. ON JSON index + redis-py json() integration
    ft_json = r.ft("sdk:jidx")
    ft_json.create_index(
        (
            TextField("$.title", as_name="title"),
            TagField("$.tags[*]", as_name="tags"),
            NumericField("$.prices[*]", as_name="prices"),
        ),
        definition=IndexDefinition(prefix=["sdk:j:"], index_type=IndexType.JSON),
    )
    r.json().set(
        "sdk:j:1",
        Path.root_path(),
        {"title": "json vector search", "tags": ["fast", "cpp"], "prices": [10, 99]},
    )
    jt, jids = search_total_and_ids(ft_json.search(Query("@tags:{cpp} @prices:[90 100]")))
    assert jt == 1 and jids == ["sdk:j:1"]

    ft.dropindex()
    ft_json.dropindex()


def run_protocol(port: int, protocol: int) -> None:
    print(f"\n--- Running redis-py {redis.__version__} SDK compat (RESP{protocol}) ---")
    r = redis.Redis(
        host="127.0.0.1",
        port=port,
        protocol=protocol,
        decode_responses=True,
        encoding_errors="replace",
    )
    try:
        r.flushdb()
        test_bloom_sdk(r)
        print(f"  PASS: gemini-bloom SDK (BF/CF/CMS/TopK/T-Digest) [RESP{protocol}]")
        test_json_sdk(r)
        print(f"  PASS: gemini-json SDK (JSON.*) [RESP{protocol}]")
        test_search_sdk(r)
        print(f"  PASS: gemini-search SDK (FT.* ON HASH & ON JSON) [RESP{protocol}]")
    finally:
        r.close()


def main() -> int:
    bloom_so = sys.argv[1] if len(sys.argv) > 1 else "./build/redis_bloom.so"
    json_so = sys.argv[2] if len(sys.argv) > 2 else "./build/redis_json.so"
    search_so = sys.argv[3] if len(sys.argv) > 3 else "./build/redis_search.so"

    for p in (bloom_so, json_so, search_so):
        if not os.path.exists(p):
            print(f"ERROR: module not found: {p}", file=sys.stderr)
            return 1

    port = find_free_port()
    workdir = tempfile.mkdtemp(prefix="gemini_sdk_compat_")
    proc = None
    try:
        proc = start_redis(port, [bloom_so, json_so, search_so], workdir)
        run_protocol(port, 2)
        run_protocol(port, 3)
        print("\nAll redis-py SDK compatibility tests PASSED (RESP2 & RESP3)!")
        return 0
    finally:
        if proc:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
