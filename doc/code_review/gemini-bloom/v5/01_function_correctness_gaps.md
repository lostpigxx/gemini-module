# 01 - 功能正确性缺口

本文件只记录功能行为层面的正确性缺口。RedisBloom 二进制互通详见 `02_redisbloom_interop_gaps.md`，RDB/wire 安全详见 `03_persistence_and_safety_gaps.md`。

## FUNC-01：`BF.INFO key FIELD` 的 RESP2 形状未与 RedisBloom 对齐

**级别：P1/P2**

当前单字段查询直接返回标量：

```text
BF.INFO key Capacity -> integer
BF.INFO key Expansion -> integer/null
```

位置：`modules/gemini-bloom/src/bloom_commands.cc:463-480`。

本轮 Redis 6.2.17 + RedisBloom v2.4.20 oracle 已确认 RESP2 差异：

```text
gemini:     BF.INFO info CAPACITY -> 100
RedisBloom: BF.INFO info CAPACITY -> [100]

gemini:     BF.INFO info SIZE -> 440
RedisBloom: BF.INFO info SIZE -> [240]
```

建议：产品层先明确是否追求 RedisBloom RESP2 shape 兼容。如果追求，修改实现和现有测试；如果不追求，文档需要显式说明这是兼容差异。

## FUNC-02：multi-item fixed filter partial failure 语义与 RedisBloom 不一致

**级别：P1/P2**

当前 `BF.MADD` / `BF.INSERT` 在遇到 full 后不再插入后续元素，但仍为剩余元素逐个回复 error，并在前缀有成功时 `ReplicateVerbatim()`：

- `modules/gemini-bloom/src/bloom_commands.cc:201-224`
- `modules/gemini-bloom/src/bloom_commands.cc:371-392`

现有测试只证明当前实现自洽：

- `BF.MADD on full NONSCALING filter returns errors`
- `BF.CARD after partial MADD matches actual insertions`
- `BF.INSERT on full filter stops and returns errors`

本轮 Redis 6.2.17 + RedisBloom v2.4.20 oracle 已确认数组长度和停止点不一致：

```text
BF.RESERVE fixed 0.01 2 NONSCALING

gemini BF.MADD fixed a b c d:
  [1, 1, ERR non scaling filter is full, ERR non scaling filter is full]

RedisBloom BF.MADD fixed a b c d:
  [1, 1, ERR non scaling filter is full]

gemini BF.INSERT fixed_insert ITEMS a b c d:
  [1, 1, ERR non scaling filter is full, ERR non scaling filter is full]

RedisBloom BF.INSERT fixed_insert ITEMS a b c d:
  [1, 1, ERR non scaling filter is full]

Both end with BF.CARD == 2.
```

影响：

- RedisBloom 客户端如果按返回数组长度判断处理进度，会在 gemini 上看到额外 error 元素。
- AOF / replication 里 partial success 命令仍会被 verbatim 复制；主从重放能自洽，但不等于 RedisBloom 语义。
- 客户端按 RedisBloom 语义重试剩余 item 时，gemini 的“剩余 item 都报错”会改变错误定位。

建议：如果目标是 RedisBloom 兼容，应在第一个 full error 后停止 reply，并用 postponed array length 返回实际元素个数；否则需要把当前差异写入兼容性文档。

## FUNC-03：`BF.RESERVE` / `BF.INSERT` option 兼容边界仍需产品化确认

**级别：P2**

当前 parser 已覆盖重复 option、缺值、负数、overflow、`NOCREATE + CAPACITY/ERROR`、`NONSCALING + EXPANSION` 等大量错误路径。

本轮已确认：

- RedisBloom v2.4.20 与 gemini 都接受 `BF.RESERVE key 0.5 100`。
- RedisBloom v2.4.20 与 gemini 都接受 `NONSCALING`。
- RedisBloom v2.4.20 与 gemini 都接受 `BF.RESERVE key 0.01 10 EXPANSION 0`。
- `BF.INSERT NOCREATE + CAPACITY` 的错误语义不一致：

```text
gemini:
  BF.INSERT nokey NOCREATE CAPACITY 10 ITEMS a
  -> ERR NOCREATE cannot be used with CAPACITY or ERROR

RedisBloom v2.4.20:
  BF.INSERT nokey NOCREATE CAPACITY 10 ITEMS a
  -> ERR not found
```

本轮补充审计继续确认 option parser 差异：

```text
BF.RESERVE reserve_exp0 0.01 10 EXPANSION 0
  gemini:     OK
  RedisBloom: OK

BF.INSERT insert_exp0 EXPANSION 0 ITEMS a
  gemini:     [1]
  RedisBloom: ERR Bad argument received

BF.RESERVE reserve_unknown 0.01 10 UNKNOWN 1
  gemini:     ERR unrecognized option
  RedisBloom: OK

BF.INSERT insert_missing NOCREATE ITEMS a
  gemini:     ERR key does not exist
  RedisBloom: ERR not found
```

因此在 RedisBloom v2.4.20 目标下，`BF.RESERVE EXPANSION 0` 不是差异；双方都接受。仍然存在差异的是 `BF.INSERT EXPANSION 0`：gemini 把它映射到 fixed/non-scaling 并创建 filter，RedisBloom v2.4.20 拒绝。RedisBloom 对 `BF.RESERVE` 的未知额外 option 还表现出宽松接受行为，gemini 更严格；这不一定值得复刻，但需要作为 client compatibility 差异记录。

仍需产品化确认：

- `BF.RESERVE` 是否要完全复刻 RedisBloom 的 arity 上限和错误消息。
- module load args 的 `EXPANSION 0` 当前拒绝，而命令层允许 `EXPANSION 0` 表示 non-scaling，配置语义和命令语义不一致。

相关位置：

- `modules/gemini-bloom/src/bloom_commands.cc:87-166`
- `modules/gemini-bloom/src/bloom_commands.cc:238-330`
- `modules/gemini-bloom/src/bloom_config.cc:39-50`

## FUNC-04：RESP3 不作为当前目标，但测试中仍保留失败用例

**级别：P3，按当前目标不阻断**

用户已明确不需要 RESP3。因此下面行为不计入必须修复项：

- RESP3 下 `BF.ADD` / `BF.EXISTS` 返回 integer 而不是 boolean。
- RESP3 下 multi 命令数组元素返回 integer 而不是 boolean。
- RESP3 下 `BF.INFO` full response 返回 array 而不是 map。

这些失败用例仍记录在 `05_known_failing_cases.md`，用于区分“目标外红灯”和“目标内红灯”。

## FUNC-05：`BF.INFO Size` 数值不是 RedisBloom 兼容值

**级别：P2**

同一 trace 下，Redis 6.2.17 + RedisBloom v2.4.20 与 gemini 的 `BF.INFO` full response 字段名相同，但 `Size` 数值不同：

```text
BF.RESERVE info 0.01 100
BF.ADD info alpha

gemini full BF.INFO:
  Capacity=100, Size=440, Number of filters=1, Items=1, Expansion rate=2

RedisBloom full BF.INFO:
  Capacity=100, Size=240, Number of filters=1, Items=1, Expansion rate=2
```

位置：`modules/gemini-bloom/src/sb_chain.cc:134-141`。

gemini 的 `BytesUsed()` 包含 `ScalingBloomFilter` 对象和预留 layer slot 容量；RedisBloom 的 `Size` 更接近 upstream chain/filter 内部内存统计。该差异不会导致迁移 false negative，但会影响容量监控、告警阈值和客户端兼容性。

建议：如果 `BF.INFO Size` 要兼容 RedisBloom，按 RedisBloom 的统计口径重算并加入 oracle test；如果保留内部估算，需要文档化这是 gemini-specific size。

## FUNC-06：RedisBloom command surface 与 command metadata 不完整

**级别：P1/P2**

RedisBloom v2.4.20 注册 `BF.DEBUG`，gemini 没有：

```text
COMMAND INFO BF.DEBUG
  gemini:     [null]
  RedisBloom: flags=[readonly, fast]

BF.DEBUG debug_key
  gemini:     ERR unknown command
  RedisBloom: [size:1, bytes:16 bits:128 hashes:9 ...]
```

如果目标是“Bloom Filter 数据可迁入迁出”，`BF.DEBUG` 可被视为非核心命令。但如果目标是 RedisBloom client compatibility，缺失命令会影响诊断工具、测试工具和依赖 `COMMAND INFO` 探测能力的客户端。

补充审计还发现只读命令 metadata 不一致：

```text
COMMAND INFO BF.INFO
  gemini:     flags=[readonly]
  RedisBloom: flags=[readonly, fast]

COMMAND INFO BF.CARD
  gemini:     flags=[readonly]
  RedisBloom: flags=[readonly, fast]

COMMAND INFO BF.SCANDUMP
  gemini:     flags=[write]
  RedisBloom: flags=[readonly, fast]
```

`BF.INFO` / `BF.CARD` 少 `fast` 主要影响客户端路由和命令分类；`BF.SCANDUMP` 的 `write` flag 会直接影响只读 replica，详见 `02_redisbloom_interop_gaps.md`。

## FUNC-07：module identity 与 module load args 不是 RedisBloom drop-in

**级别：P2**

补充审计确认：

```text
MODULE LIST
  gemini:     name=GeminiBloom, ver=1
  RedisBloom: name=bf, ver=20420
```

这不影响 `MBbloom--` RDB data type 互通，但会影响：

- 通过 `MODULE LIST` 检测 RedisBloom 是否已加载的客户端。
- 通过 module version gate 判断 RedisBloom 功能集的迁移工具。
- 依赖 RedisBloom module name `bf` 的运维脚本。

module load args 也不是完整 drop-in：

```text
--loadmodule <module> INITIAL_SIZE 7 ERROR_RATE 0.05
  gemini:     starts; default BF.ADD creates Capacity=7
  RedisBloom: starts; default BF.ADD creates Capacity=7

--loadmodule <module> EXPANSION 4
  gemini:     starts; default BF.ADD has Expansion rate=4
  RedisBloom: fails module load with "Unrecognized option"

--loadmodule <module> CF_MAX_EXPANSIONS 8
  gemini:     fails module load with "Unrecognized config argument"
  RedisBloom: starts
```

`CF_MAX_EXPANSIONS` 属于 RedisBloom 的 Cuckoo Filter surface，按当前 gemini-bloom 只实现 Bloom Filter 的产品边界可以不支持；但如果部署脚本复用 RedisBloom module args，替换模块会启动失败。建议文档显式列出支持的 module args，并把 module identity 差异写入兼容声明。
