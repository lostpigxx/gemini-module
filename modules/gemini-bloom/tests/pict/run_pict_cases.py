#!/usr/bin/env python3
import argparse
import csv
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


MODEL_ORDER = [
    "reserve",
    "insert",
    "commands",
    "loadchunk",
    "module-args",
    "protocol",
    "persistence",
]

DEFAULT_CAPACITY = 100
DEFAULT_EXPANSION = 2


class RedisError(Exception):
    pass


class RedisClient:
    def __init__(self, host, port):
      self.sock = socket.create_connection((host, port), timeout=3)
      self.file = self.sock.makefile("rb")

    def close(self):
      try:
        self.file.close()
      finally:
        self.sock.close()

    def command(self, *args):
      payload = [b"*" + str(len(args)).encode() + b"\r\n"]
      for arg in args:
        if isinstance(arg, bytes):
          data = arg
        else:
          data = str(arg).encode()
        payload.append(b"$" + str(len(data)).encode() + b"\r\n")
        payload.append(data + b"\r\n")
      self.sock.sendall(b"".join(payload))
      return self._read_reply()

    def _line(self):
      line = self.file.readline()
      if not line:
        raise ConnectionError("connection closed")
      if not line.endswith(b"\r\n"):
        raise ConnectionError(f"malformed RESP line: {line!r}")
      return line[:-2]

    def _read_reply(self):
      prefix = self.file.read(1)
      if not prefix:
        raise ConnectionError("connection closed")
      line = self._line()
      if prefix == b"+":
        return line.decode()
      if prefix == b"-":
        raise RedisError(line.decode(errors="replace"))
      if prefix == b":":
        return int(line)
      if prefix == b"$":
        size = int(line)
        if size == -1:
          return None
        data = self.file.read(size)
        trailer = self.file.read(2)
        if trailer != b"\r\n":
          raise ConnectionError("malformed bulk string trailer")
        return data
      if prefix == b"*":
        count = int(line)
        if count == -1:
          return None
        return [self._read_reply() for _ in range(count)]
      if prefix == b"#":
        return line == b"t"
      if prefix == b"_":
        return None
      if prefix == b",":
        return float(line)
      if prefix == b"%":
        count = int(line)
        result = {}
        for _ in range(count):
          key = self._read_reply()
          val = self._read_reply()
          result[key] = val
        return result
      if prefix == b"~":
        count = int(line)
        return [self._read_reply() for _ in range(count)]
      if prefix == b">":
        count = int(line)
        return [self._read_reply() for _ in range(count)]
      if prefix == b"=":
        size = int(line)
        data = self.file.read(size)
        trailer = self.file.read(2)
        if trailer != b"\r\n":
          raise ConnectionError("malformed verbatim string trailer")
        return data
      if prefix == b"(":
        return int(line)
      raise ConnectionError(f"unsupported RESP prefix {prefix!r}")


class RedisServer:
    def __init__(self, args, name, module_args=(), appendonly=False):
      self.args = args
      self.name = name
      self.module_args = list(module_args)
      self.appendonly = appendonly
      self.port = free_port()
      self.dir = (Path(args.out_dir) / "tmp" / name).resolve()
      self.log_path = self.dir / "redis.log"
      self.proc = None
      self.stderr = None

    def start(self, allow_fail=False):
      self.dir.mkdir(parents=True, exist_ok=True)
      cmd = [
          self.args.redis_server,
          "--port", str(self.port),
          "--bind", "127.0.0.1",
          "--protected-mode", "no",
          "--daemonize", "no",
          "--loglevel", "warning",
          "--logfile", str(self.log_path),
          "--dir", str(self.dir),
          "--dbfilename", "dump.rdb",
          "--save", "",
      ]
      if self.appendonly:
        cmd += ["--appendonly", "yes"]
      cmd += ["--loadmodule", self.args.module]
      cmd += self.module_args

      self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
      deadline = time.time() + 5
      while time.time() < deadline:
        if self.proc.poll() is not None:
          return False if allow_fail else self._raise_start_failed()
        try:
          client = RedisClient("127.0.0.1", self.port)
          client.command("PING")
          client.close()
          return True
        except (OSError, ConnectionError, RedisError):
          time.sleep(0.05)
      return False if allow_fail else self._raise_start_failed()

    def _raise_start_failed(self):
      log = read_text(self.log_path)
      stderr = ""
      if self.proc:
        try:
          _, err = self.proc.communicate(timeout=0.1)
          stderr = err.decode(errors="replace") if err else ""
        except subprocess.TimeoutExpired:
          pass
      raise RuntimeError(
          f"redis-server failed to start, log={self.log_path}\n{log}\n{stderr}")

    def client(self):
      return RedisClient("127.0.0.1", self.port)

    def stop(self, save=False):
      if not self.proc or self.proc.poll() is not None:
        return
      try:
        client = self.client()
        try:
          client.command("SHUTDOWN", "SAVE" if save else "NOSAVE")
        except (ConnectionError, RedisError):
          pass
        client.close()
      except OSError:
        pass
      try:
        self.proc.wait(timeout=3)
      except subprocess.TimeoutExpired:
        self.proc.terminate()
        try:
          self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
          self.proc.kill()
          self.proc.wait(timeout=3)


def free_port():
  with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(("127.0.0.1", 0))
    return sock.getsockname()[1]


def read_text(path):
  try:
    return Path(path).read_text(errors="replace")
  except OSError:
    return ""


def as_text(value):
  if isinstance(value, bytes):
    return value.decode(errors="replace")
  return value


def fail(message, command=None, expected=None, actual=None, log_path=None):
  raise AssertionError(json.dumps({
      "message": message,
      "command": printable_command(command) if command else None,
      "expected": expected,
      "actual": printable_value(actual),
      "redis_log": str(log_path) if log_path else None,
  }, ensure_ascii=True))


def printable_value(value):
  if isinstance(value, bytes):
    text = value.decode(errors="replace")
    hex_value = value.hex()
    if len(hex_value) > 256:
      hex_value = hex_value[:256] + "..."
    if len(text) > 256:
      text = text[:256] + "..."
    return {"bytes_len": len(value), "bytes_hex": hex_value, "text": text}
  if isinstance(value, list):
    return [printable_value(v) for v in value]
  if isinstance(value, dict):
    return {str(printable_value(k)): printable_value(v) for k, v in value.items()}
  return value


def printable_command(command):
  return [printable_value(arg) for arg in command]


def expect_error(client, command, contains=None, log_path=None):
  try:
    reply = client.command(*command)
  except RedisError as err:
    if contains and contains not in str(err):
      fail("error text mismatch", command, contains, str(err), log_path)
    return str(err)
  fail("expected Redis error", command, "error", reply, log_path)


def expect_ok(client, command, log_path=None):
  try:
    return client.command(*command)
  except RedisError as err:
    fail("unexpected Redis error", command, "success", str(err), log_path)


def value_is_valid(value):
  return value.startswith("valid") or value == "default" or value == "absent"


def rate_arg(value):
  return {
      "valid_small": "0.01",
      "valid_tiny": "0.001",
      "zero": "0",
      "one": "1",
      "negative": "-0.1",
      "text": "abc",
      "default": None,
      "absent": None,
  }[value]


def capacity_arg(value):
  return {
      "valid_small": "10",
      "valid_medium": "100",
      "zero": "0",
      "negative": "-1",
      "text": "abc",
      "default": None,
      "absent": None,
  }[value]


def expansion_args(value):
  if value == "absent":
    return [], False, True
  if value == "valid_1":
    return ["EXPANSION", "1"], True, True
  if value == "valid_2":
    return ["EXPANSION", "2"], True, True
  if value == "valid_4":
    return ["EXPANSION", "4"], True, True
  if value == "zero":
    return ["EXPANSION", "0"], True, False
  if value == "negative":
    return ["EXPANSION", "-1"], True, False
  if value == "text":
    return ["EXPANSION", "abc"], True, False
  if value == "missing_value":
    return ["EXPANSION"], True, False
  if value == "duplicate":
    return ["EXPANSION", "2", "EXPANSION", "4"], True, True
  raise ValueError(value)


def item_values(shape):
  if shape in ("one_ascii", "ascii_new", "new_item", "ascii"):
    return [b"new-item"]
  if shape in ("two_ascii",):
    return [b"one", b"two"]
  if shape in ("duplicate", "ascii_existing", "existing_item"):
    return [b"known", b"known"]
  if shape in ("empty",):
    return [b""]
  if shape in ("binary", "binary_item"):
    return [b"bin\x00x\r\n\xff"]
  if shape in ("long",):
    return [b"x" * 10000]
  if shape in ("missing_item",):
    return [b"missing"]
  raise ValueError(shape)


def seed_key(client, key, state):
  expect_ok(client, ["DEL", key])
  if state in ("missing",):
    return
  if state in ("existing_string", "string"):
    expect_ok(client, ["SET", key, "plain-string"])
    return
  if state in ("existing_bloom", "bloom_empty"):
    expect_ok(client, ["BF.RESERVE", key, "0.01", "100"])
    return
  if state in ("bloom_with_items",):
    expect_ok(client, ["BF.RESERVE", key, "0.01", "100"])
    expect_ok(client, ["BF.ADD", key, b"known"])
    expect_ok(client, ["BF.ADD", key, b"other"])
    return
  raise ValueError(state)


def run_reserve_case(args, row, idx):
  server = RedisServer(args, f"reserve-{idx}")
  server.start()
  client = server.client()
  key = f"pict_reserve_{idx}"
  try:
    seed_key(client, key, row["KeyState"])
    cmd = ["BF.RESERVE", key, rate_arg(row["Rate"]), capacity_arg(row["Capacity"])]
    exp, exp_set, exp_valid = expansion_args(row["Expansion"])
    ns_present = row["NonScaling"] in ("present", "duplicate")
    ns = []
    if row["NonScaling"] == "present":
      ns = ["NONSCALING"]
    elif row["NonScaling"] == "duplicate":
      ns = ["NONSCALING", "NONSCALING"]
    opts = exp + ns if row["OptionOrder"] == "expansion_first" else ns + exp
    if row["UnknownOption"] == "present":
      opts.append("FOOBAR")
    cmd += opts

    valid = (row["Rate"].startswith("valid") and row["Capacity"].startswith("valid") and
             exp_valid and row["UnknownOption"] == "absent" and
             not (ns_present and exp_set) and row["KeyState"] == "missing")
    if valid:
      reply = expect_ok(client, cmd, server.log_path)
      if reply != "OK":
        fail("BF.RESERVE reply mismatch", cmd, "OK", reply, server.log_path)
      info = expect_ok(client, ["BF.INFO", key], server.log_path)
      if not isinstance(info, list) or len(info) != 10:
        fail("BF.INFO shape mismatch after reserve", ["BF.INFO", key],
             "10-element array", info, server.log_path)
    else:
      expect_error(client, cmd, log_path=server.log_path)
  finally:
    client.close()
    server.stop()


def run_insert_case(args, row, idx):
  server = RedisServer(args, f"insert-{idx}")
  server.start()
  client = server.client()
  key = f"pict_insert_{idx}"
  try:
    seed_key(client, key, row["KeyState"])
    cmd = ["BF.INSERT", key]

    if row["NoCreate"] == "present":
      cmd.append("NOCREATE")
    elif row["NoCreate"] == "duplicate":
      cmd += ["NOCREATE", "NOCREATE"]

    if row["ErrorRate"] != "default":
      cmd += ["ERROR", rate_arg(row["ErrorRate"])]
    if row["Capacity"] != "default":
      cmd += ["CAPACITY", capacity_arg(row["Capacity"])]
    exp, exp_set, exp_valid = expansion_args(row["Expansion"])
    ns_present = row["NonScaling"] == "present"
    cmd += exp
    if ns_present:
      cmd.append("NONSCALING")
    if row["UnknownOption"] == "present":
      cmd.append("FOOBAR")

    items = item_values(row["ItemShape"])
    if row["ItemsPosition"] == "normal":
      cmd.append("ITEMS")
      cmd += items
    elif row["ItemsPosition"] == "missing_keyword":
      cmd += items
    elif row["ItemsPosition"] == "no_items":
      cmd.append("ITEMS")
    elif row["ItemsPosition"] == "option_after_items":
      cmd.append("ITEMS")
      cmd += items + ["EXPANSION", "2"]
      items = items + [b"EXPANSION", b"2"]

    parse_valid = (
        row["UnknownOption"] == "absent" and
        row["ErrorRate"] in ("default", "valid_small", "valid_tiny") and
        row["Capacity"] in ("default", "valid_small", "valid_medium") and
        exp_valid and
        row["ItemsPosition"] in ("normal", "option_after_items") and
        not (ns_present and exp_set)
    )
    state_valid = row["KeyState"] != "existing_string"
    nocreate = row["NoCreate"] in ("present", "duplicate")
    valid = parse_valid and state_valid and not (row["KeyState"] == "missing" and nocreate)

    if valid:
      reply = expect_ok(client, cmd, server.log_path)
      if not isinstance(reply, list) or len(reply) != len(items):
        fail("BF.INSERT reply shape mismatch", cmd,
             f"{len(items)} array entries", reply, server.log_path)
    else:
      expect_error(client, cmd, log_path=server.log_path)
  finally:
    client.close()
    server.stop()


def command_args(row, key):
  raw_command = row["Command"]
  command = raw_command if raw_command.startswith("BF.") else f"BF.{raw_command}"
  op = command[3:]
  items = item_values(row["ItemShape"])
  field = row["InfoField"]

  if row["Arity"] == "too_few":
    return [command] if op in ("CARD", "INFO") else [command, key]

  if op == "ADD":
    args = [command, key, items[0]]
  elif op == "EXISTS":
    args = [command, key, items[0]]
  elif op == "MADD":
    args = [command, key] + items
  elif op == "MEXISTS":
    args = [command, key] + items
  elif op == "INFO":
    args = [command, key] if field == "none" else [command, key, field]
  elif op == "CARD":
    args = [command, key]
  else:
    raise ValueError(raw_command)

  if row["Arity"] == "too_many":
    args.append("extra")
  return args


def run_commands_case(args, row, idx):
  server = RedisServer(args, f"commands-{idx}")
  server.start()
  client = server.client()
  key = f"pict_commands_{idx}"
  try:
    seed_key(client, key, row["KeyState"])
    cmd = command_args(row, key)
    variadic = row["Command"] in ("MADD", "MEXISTS")
    arity_ok = row["Arity"] == "valid" or (row["Arity"] == "too_many" and variadic)
    if not arity_ok:
      expect_error(client, cmd, log_path=server.log_path)
      return

    if row["KeyState"] == "existing_string":
      expect_error(client, cmd, "WRONGTYPE", server.log_path)
      return

    if row["Command"] == "INFO" and row["KeyState"] == "missing":
      expect_error(client, cmd, log_path=server.log_path)
      return
    if row["Command"] == "INFO" and row["InfoField"] == "BadField":
      expect_error(client, cmd, log_path=server.log_path)
      return

    reply = expect_ok(client, cmd, server.log_path)
    if row["Command"] in ("MADD", "MEXISTS") and not isinstance(reply, list):
      fail("expected array reply", cmd, "array", reply, server.log_path)
    if row["Command"] in ("ADD", "EXISTS", "CARD") and not isinstance(reply, int):
      fail("expected integer reply", cmd, "integer", reply, server.log_path)
  finally:
    client.close()
    server.stop()


def make_source_filter(client, key, shape):
  expect_ok(client, ["DEL", key])
  cap = "5" if shape == "multi_layer" else "100"
  expect_ok(client, ["BF.RESERVE", key, "0.01", cap])
  count = 30 if shape == "multi_layer" else 3
  for i in range(count):
    expect_ok(client, ["BF.ADD", key, f"dump-{i}"])
  return count


def scandump_chunks(client, key):
  cursor = 0
  chunks = []
  while True:
    reply = expect_ok(client, ["BF.SCANDUMP", key, str(cursor)])
    if not isinstance(reply, list) or len(reply) != 2:
      raise AssertionError("SCANDUMP returned unexpected shape")
    next_cursor, data = reply
    if next_cursor == 0 and data == b"":
      break
    chunks.append((next_cursor, data))
    cursor = next_cursor
  return chunks


def zero_layers_header():
  return struct.pack("<QIII", 0, 0, 5, 2)


def run_loadchunk_case(args, row, idx):
  server = RedisServer(args, f"loadchunk-{idx}")
  server.start()
  client = server.client()
  src = f"pict_lc_src_{idx}"
  dst = f"pict_lc_dst_{idx}"
  try:
    expected_count = make_source_filter(client, src, row["SourceShape"])
    chunks = scandump_chunks(client, src)
    header_cursor, header = chunks[0]
    data_cursor, data = chunks[1] if len(chunks) > 1 else (2, b"")
    seed_key(client, dst, {
        "missing": "missing",
        "existing_bloom": "existing_bloom",
        "existing_string": "existing_string",
    }[row["TargetState"]])

    flow = row["Flow"]
    if flow == "official_roundtrip":
      if row["TargetState"] == "existing_string":
        expect_error(client, ["BF.LOADCHUNK", dst, header_cursor, header],
                     "WRONGTYPE", server.log_path)
        return
      for cursor, payload in chunks:
        expect_ok(client, ["BF.LOADCHUNK", dst, cursor, payload], server.log_path)
      card = expect_ok(client, ["BF.CARD", dst], server.log_path)
      if card != expected_count:
        fail("LOADCHUNK round-trip card mismatch", ["BF.CARD", dst],
             expected_count, card, server.log_path)
      return

    if flow == "header_only":
      command = ["BF.LOADCHUNK", dst, header_cursor, header]
      if row["TargetState"] == "existing_string":
        expect_error(client, command, "WRONGTYPE", server.log_path)
      else:
        expect_ok(client, command, server.log_path)
      return

    if flow in ("data_before_header", "missing_key_data"):
      expect_ok(client, ["DEL", dst])
      expect_error(client, ["BF.LOADCHUNK", dst, data_cursor, data],
                   log_path=server.log_path)
      return

    if flow == "malformed_header":
      expect_error(client, ["BF.LOADCHUNK", dst, "1", b"short"],
                   log_path=server.log_path)
      return

    if flow == "zero_layers_header":
      expect_error(client, ["BF.LOADCHUNK", dst, "1", zero_layers_header()],
                   log_path=server.log_path)
      return

    if flow == "wrong_size_chunk":
      expect_ok(client, ["DEL", dst])
      expect_ok(client, ["BF.LOADCHUNK", dst, header_cursor, header], server.log_path)
      expect_error(client, ["BF.LOADCHUNK", dst, data_cursor, b"short"],
                   "data length mismatch", server.log_path)
      return

    if flow == "cursor_zero":
      expect_error(client, ["BF.LOADCHUNK", dst, "0", b"data"],
                   "cursor", server.log_path)
      return
    if flow == "cursor_negative":
      expect_error(client, ["BF.LOADCHUNK", dst, "-1", b"data"],
                   "cursor", server.log_path)
      return
    if flow == "cursor_text":
      expect_error(client, ["BF.LOADCHUNK", dst, "abc", b"data"],
                   "cursor", server.log_path)
      return
    if flow == "string_key_header":
      expect_ok(client, ["DEL", dst])
      expect_ok(client, ["SET", dst, "value"])
      expect_error(client, ["BF.LOADCHUNK", dst, header_cursor, header],
                   "WRONGTYPE", server.log_path)
      return
    if flow == "existing_bloom_malformed":
      expect_ok(client, ["DEL", dst])
      expect_ok(client, ["BF.RESERVE", dst, "0.01", "100"])
      expect_ok(client, ["BF.ADD", dst, "keep"])
      expect_error(client, ["BF.LOADCHUNK", dst, "1", b"short"],
                   log_path=server.log_path)
      card = expect_ok(client, ["BF.CARD", dst], server.log_path)
      if card != 1:
        fail("malformed header corrupted existing bloom key", ["BF.CARD", dst],
             1, card, server.log_path)
      return
    raise ValueError(flow)
  finally:
    client.close()
    server.stop()


def module_args_from_row(row):
  args = []
  entries = []
  if row["ErrorRate"] != "absent":
    entries.append(("ERROR_RATE", rate_arg(row["ErrorRate"])))
  if row["InitialSize"] != "absent":
    entries.append(("INITIAL_SIZE", capacity_arg(row["InitialSize"])))
  if row["Expansion"] != "absent":
    exp, _, _ = expansion_args(row["Expansion"])
    if exp:
      entries.append((exp[0], exp[1] if len(exp) > 1 else None))
  if row["ArgOrder"] == "reversed":
    entries.reverse()
  for key, val in entries:
    args.append(key)
    if row["MissingValue"].lower() == key.lower():
      continue
    if val is not None:
      args.append(val)
  if row["MissingValue"] == "error_rate" and "ERROR_RATE" not in args:
    args.append("ERROR_RATE")
  if row["MissingValue"] == "initial_size" and "INITIAL_SIZE" not in args:
    args.append("INITIAL_SIZE")
  if row["MissingValue"] == "expansion" and "EXPANSION" not in args:
    args.append("EXPANSION")
  if row["UnknownArg"] == "present":
    args += ["UNKNOWN_ARG", "1"]
  return args


def module_args_valid(row):
  return (
      row["ErrorRate"] in ("absent", "valid_small", "valid_tiny") and
      row["InitialSize"] in ("absent", "valid_small", "valid_medium") and
      row["Expansion"] in ("absent", "valid_1", "valid_4") and
      row["UnknownArg"] == "absent" and
      row["MissingValue"] == "none"
  )


def run_module_args_case(args, row, idx):
  module_args = module_args_from_row(row)
  server = RedisServer(args, f"module-args-{idx}", module_args=module_args)
  ready = server.start(allow_fail=True)
  expected_ready = module_args_valid(row)
  if not expected_ready:
    if ready:
      server.stop()
      fail("module args should have failed server startup",
           ["redis-server", "--loadmodule", args.module] + module_args,
           "startup failure", "started", server.log_path)
    return
  if not ready:
    fail("module args should have allowed server startup",
         ["redis-server", "--loadmodule", args.module] + module_args,
         "started", "startup failed", server.log_path)

  client = server.client()
  try:
    if row["AutoCreateCheck"] == "yes":
      key = f"pict_auto_{idx}"
      expect_ok(client, ["BF.ADD", key, "x"], server.log_path)
      cap = expect_ok(client, ["BF.INFO", key, "Capacity"], server.log_path)
      expected_cap = DEFAULT_CAPACITY
      if row["InitialSize"] == "valid_small":
        expected_cap = 10
      elif row["InitialSize"] == "valid_medium":
        expected_cap = 100
      if cap != expected_cap:
        fail("auto-created capacity mismatch", ["BF.INFO", key, "Capacity"],
             expected_cap, cap, server.log_path)
      exp = expect_ok(client, ["BF.INFO", key, "Expansion"], server.log_path)
      expected_exp = DEFAULT_EXPANSION
      if row["Expansion"] == "valid_1":
        expected_exp = 1
      elif row["Expansion"] == "valid_4":
        expected_exp = 4
      if exp != expected_exp:
        fail("auto-created expansion mismatch", ["BF.INFO", key, "Expansion"],
             expected_exp, exp, server.log_path)
  finally:
    client.close()
    server.stop()


def run_protocol_case(args, row, idx):
  server = RedisServer(args, f"protocol-{idx}")
  server.start()
  client = server.client()
  key = f"pict_protocol_{idx}"
  try:
    if row["RespVersion"] == "resp3":
      client.command("HELLO", "3")
    seed_state = "bloom_with_items" if row["KeyState"] == "bloom_with_item" else row["KeyState"]
    seed_key(client, key, seed_state)
    command_row = {
        "Command": "BF." + row["Command"],
        "KeyState": row["KeyState"],
        "ItemShape": {
            "new_item": "ascii_new",
            "existing_item": "ascii_existing",
            "missing_item": "missing_item",
            "binary_item": "binary",
        }[row["ItemCase"]],
        "InfoField": "none",
        "Arity": row["Arity"],
    }
    cmd = command_args(command_row, key)
    if row["Arity"] == "too_few":
      expect_error(client, cmd, log_path=server.log_path)
      return
    if row["KeyState"] == "existing_string":
      expect_error(client, cmd, "WRONGTYPE", server.log_path)
      return
    reply = expect_ok(client, cmd, server.log_path)
    if row["Command"] in ("ADD", "EXISTS") and not isinstance(reply, int):
      fail("boolean command did not return integer", cmd, "integer", reply,
           server.log_path)
    if row["Command"] in ("MADD", "MEXISTS"):
      if not isinstance(reply, list) or not all(isinstance(v, int) for v in reply):
        fail("multi boolean command did not return integer array", cmd,
             "integer array", reply, server.log_path)
  finally:
    client.close()
    server.stop()


def wait_for_aof_rewrite(client):
  deadline = time.time() + 10
  while time.time() < deadline:
    info = as_text(client.command("INFO", "persistence"))
    if "aof_rewrite_in_progress:0" in info:
      return
    time.sleep(0.1)
  raise AssertionError("AOF rewrite did not finish")


def run_persistence_case(args, row, idx):
  appendonly = row["Mode"] == "aof"
  server = RedisServer(args, f"persistence-{idx}", appendonly=appendonly)
  server.start()
  client = server.client()
  key = f"pict_persist_{idx}"
  try:
    cap = "5" if row["Shape"] == "multi_layer" and row["Scaling"] == "scaling" else "50"
    reserve = ["BF.RESERVE", key, "0.01", cap]
    if row["Scaling"] == "non_scaling":
      reserve.append("NONSCALING")
    expect_ok(client, reserve, server.log_path)
    count = 30 if row["Shape"] == "multi_layer" and row["Scaling"] == "scaling" else 8
    items = []
    for i in range(count):
      item = f"persist-{i}".encode()
      if row["ItemShape"] == "binary":
        item = b"persist\x00" + str(i).encode() + b"\xff"
      items.append(item)
      expect_ok(client, ["BF.ADD", key, item], server.log_path)

    if row["Mode"] == "rdb":
      expect_ok(client, ["SAVE"], server.log_path)
    else:
      expect_ok(client, ["BGREWRITEAOF"], server.log_path)
      wait_for_aof_rewrite(client)
  finally:
    client.close()
    server.stop(save=row["RestartStyle"] == "shutdown_save")

  restarted = RedisServer(args, f"persistence-{idx}", appendonly=appendonly)
  restarted.dir = server.dir
  restarted.log_path = server.log_path
  restarted.start()
  client = restarted.client()
  try:
    card = expect_ok(client, ["BF.CARD", key], restarted.log_path)
    if card != len(items):
      fail("persistence card mismatch", ["BF.CARD", key], len(items), card,
           restarted.log_path)
    for item in items:
      exists = expect_ok(client, ["BF.EXISTS", key, item], restarted.log_path)
      if exists != 1:
        fail("false negative after persistence restart",
             ["BF.EXISTS", key, item], 1, exists, restarted.log_path)
  finally:
    client.close()
    restarted.stop()


RUNNERS = {
    "reserve": run_reserve_case,
    "insert": run_insert_case,
    "commands": run_commands_case,
    "loadchunk": run_loadchunk_case,
    "module-args": run_module_args_case,
    "protocol": run_protocol_case,
    "persistence": run_persistence_case,
}


def generate_cases(args, model):
  pict = Path(args.pict_bin)
  model_path = Path("modules/gemini-bloom/tests/pict") / f"{model}.pict"
  out_dir = Path(args.out_dir)
  cases_dir = out_dir / "cases"
  cases_dir.mkdir(parents=True, exist_ok=True)
  out_path = cases_dir / f"{model}.order{args.order}.tsv"

  attempts = [
      [str(pict), str(model_path), f"/o:{args.order}"],
      [str(pict), str(model_path), f"-o:{args.order}"],
      [str(pict), str(model_path)],
  ]
  last = None
  for cmd in attempts:
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True)
    if result.returncode == 0 and result.stdout.strip():
      out_path.write_text(result.stdout)
      return out_path
    last = result
  stderr = last.stderr if last else ""
  raise RuntimeError(f"PICT generation failed for {model}: {stderr}")


def load_cases(path):
  with Path(path).open(newline="") as f:
    return list(csv.DictReader(f, delimiter="\t"))


def selected_models(name):
  if name == "all":
    return MODEL_ORDER
  if name not in MODEL_ORDER:
    raise ValueError(f"unknown model: {name}")
  return [name]


def run_model(args, model, report):
  cases_path = generate_cases(args, model)
  cases = load_cases(cases_path)
  report["models"][model] = {
      "cases_file": str(cases_path),
      "case_count": len(cases),
      "passed": 0,
      "failed": 0,
  }
  if args.generate_only:
    return

  runner = RUNNERS[model]
  for index, row in enumerate(cases, 1):
    try:
      shutil.rmtree((Path(args.out_dir) / "tmp" / f"{model}-{index}").resolve(),
                    ignore_errors=True)
      runner(args, row, index)
      report["models"][model]["passed"] += 1
    except Exception as err:
      report["models"][model]["failed"] += 1
      detail = str(err)
      parsed = None
      try:
        parsed = json.loads(detail)
      except json.JSONDecodeError:
        pass
      report["failures"].append({
          "model": model,
          "case_id": index,
          "row": row,
          "detail": parsed if parsed is not None else detail,
      })

  if not args.keep_cases and not args.generate_only:
    try:
      cases_path.unlink()
    except OSError:
      pass


def write_reports(args, report):
  out_dir = Path(args.out_dir)
  out_dir.mkdir(parents=True, exist_ok=True)
  json_path = out_dir / "report.json"
  txt_path = out_dir / "report.txt"
  json_path.write_text(json.dumps(report, indent=2, sort_keys=True))

  lines = []
  lines.append("gemini-bloom PICT report")
  lines.append(f"model selection: {args.model}")
  lines.append(f"order: {args.order}")
  lines.append("")
  for model, stats in report["models"].items():
    lines.append(
        f"{model}: {stats['passed']} passed, {stats['failed']} failed, "
        f"{stats['case_count']} generated")
  if report["failures"]:
    lines.append("")
    lines.append("Failures:")
    for failure in report["failures"]:
      lines.append(json.dumps(failure, sort_keys=True, ensure_ascii=True))
  txt_path.write_text("\n".join(lines) + "\n")
  return json_path, txt_path


def validate_args(args):
  if args.order not in (2, 3):
    raise SystemExit("--order must be 2 or 3")
  if not Path(args.pict_bin).exists():
    raise SystemExit(
        f"PICT binary not found at {args.pict_bin}; run tools/pict/bootstrap_pict.sh")
  if not args.generate_only and not Path(args.module).exists():
    raise SystemExit(f"module not found at {args.module}; build redis_bloom.so first")
  args.pict_bin = str(Path(args.pict_bin).resolve())
  if not args.generate_only:
    args.module = str(Path(args.module).resolve())


def parse_args():
  parser = argparse.ArgumentParser()
  parser.add_argument("--pict-bin", default="build/tools/pict/docker/bin/pict")
  parser.add_argument("--module", default="./build/redis_bloom.so")
  parser.add_argument("--model", default="all")
  parser.add_argument("--order", type=int, default=2)
  parser.add_argument("--out-dir", default="build/pict/gemini-bloom")
  parser.add_argument("--redis-server", default="redis-server")
  parser.add_argument("--keep-cases", action="store_true")
  parser.add_argument("--generate-only", action="store_true")
  return parser.parse_args()


def main():
  args = parse_args()
  validate_args(args)
  Path(args.out_dir).mkdir(parents=True, exist_ok=True)
  Path(args.out_dir, "tmp").mkdir(parents=True, exist_ok=True)

  report = {
      "model": args.model,
      "order": args.order,
      "module": args.module,
      "pict_bin": args.pict_bin,
      "models": {},
      "failures": [],
  }

  try:
    for model in selected_models(args.model):
      run_model(args, model, report)
  finally:
    json_path, txt_path = write_reports(args, report)

  total_failed = sum(stats["failed"] for stats in report["models"].values())
  total_passed = sum(stats["passed"] for stats in report["models"].values())
  total_cases = sum(stats["case_count"] for stats in report["models"].values())
  print(f"PICT generated {total_cases} cases")
  if args.generate_only:
    print(f"Generated cases under {Path(args.out_dir) / 'cases'}")
    return 0
  print(f"PICT results: {total_passed} passed, {total_failed} failed")
  print(f"Reports: {json_path}, {txt_path}")
  return 1 if total_failed else 0


if __name__ == "__main__":
  sys.exit(main())
