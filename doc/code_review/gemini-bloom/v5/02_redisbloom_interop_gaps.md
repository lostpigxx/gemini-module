# 02 - RedisBloom 互通缺口

本文件只讨论 RedisBloom 迁入迁出、RDB/AOF/SCANDUMP/DUMP/RESTORE/replication 二进制协议和 data type name 风险。

## INTEROP-01：`BF.SCANDUMP` cursor 是 layer index，不是 RedisBloom byte offset

**级别：P0**

当前实现注释和代码都使用私有 cursor 协议：

```text
SCANDUMP key 0   -> [1, header]
SCANDUMP key 1   -> [2, layer0_bits]
SCANDUMP key 2   -> [3, layer1_bits]
...
```

位置：`modules/gemini-bloom/src/bloom_commands.cc:519-571`。

本轮 TCL 失败也直接证明了差异：

```text
EXPECTED COMPAT GAP: SCANDUMP layer cursor should advance by byte length
expected byte-offset cursor 129, got 2
```

本轮 Redis 6.2.17 + RedisBloom v2.8.20 oracle 进一步确认真实 chunk 序列不同。同一类 `BF.RESERVE key 0.01 10 EXPANSION 2`、插入 40 个 item 的数据：

```text
RedisBloom SCANDUMP chunks:
  [(1, 179), (17, 16), (49, 32), (121, 72), (0, 0)]

gemini SCANDUMP chunks:
  [(1, 179), (2, 128), (3, 128), (4, 128), (0, 0)]
```

RedisBloom 的迁移协议使用 byte-offset iterator，chunk 可以位于任意 layer 的任意 offset，并且大 chunk 会被拆分。gemini 当前的 layer-index 协议只能在 gemini 自己 dump/load 时自洽，不能证明 RedisBloom 迁入迁出。

完整矩阵结果：`BF.SCANDUMP` / `BF.LOADCHUNK` 在 9 个 corpus、双向共 18 个单元上 0 pass、18 fail。

建议：

- 实现 byte-offset cursor。
- `cursor=0` 返回 header，下一 cursor 为 `1`。
- `cursor>0` 从拼接后的 layer bit arrays 中按 byte offset 返回 chunk。
- `next = cursor + chunk_len`，结束返回 `(0, empty/null)`。
- 覆盖 layer 中间、layer 边界、跨 layer、超过 16MB 拆分。

## INTEROP-02：`BF.LOADCHUNK` 只接受整层 bit array

**级别：P0/P1**

当前 `cursor > 1` 时：

```text
idx = cursor - 2
dataLen must equal layer.GetDataSize()
memcpy whole layer
```

位置：`modules/gemini-bloom/src/bloom_commands.cc:611-625`。

这不支持 RedisBloom 任意 byte-offset chunk。RedisBloom 生成的大 layer 多段 chunk 或 layer 中间 chunk 无法导入 gemini。

本轮 Redis 6.2.17 + RedisBloom v2.8.20 -> gemini 的实际导入结果：

```text
redisbloom_to_gemini_load_replies:
  ["OK",
   "ERR cursor exceeds layer count",
   "ERR cursor exceeds layer count",
   "ERR cursor exceeds layer count"]

redisbloom_to_gemini_card: 40
redisbloom_to_gemini_found: 0 / 40
```

第一块 header 被 gemini 接受并发布 key，因此 `BF.CARD` 显示 header-declared `40`；后续 byte-offset chunks 被拒绝，bit arrays 保持空，所有 inserted items 都变成 false negative。

gemini -> RedisBloom 的实际导入结果：

```text
gemini_to_redisbloom_load_replies:
  ["OK",
   "ERR received bad data",
   "ERR received bad data",
   "ERR received bad data"]

gemini_to_redisbloom_card: 40
gemini_to_redisbloom_found: 0 / 40
```

这说明双方 header 形状在该简单样本上足以创建 shell，但 data chunk 协议不互通，且失败后的半恢复对象对客户端可见。

建议：

- `LOADCHUNK key iter data` 按 `iter - dataLen` 计算写入起点。
- 根据全局 offset 找到目标 layer 和 layer 内 offset。
- 允许 chunk 覆盖单层局部、刚好到边界、跨层。
- 对 `iter < dataLen`、越界、header 未加载等情况返回明确错误。

## INTEROP-03：AOF rewrite 使用公共 `BF.LOADCHUNK` 承载私有协议

**级别：P1**

当前 AOF rewrite 输出：

```text
BF.LOADCHUNK key 1 <gemini header>
BF.LOADCHUNK key 2 <layer0 full bits>
BF.LOADCHUNK key 3 <layer1 full bits>
...
```

位置：`modules/gemini-bloom/src/bloom_rdb.cc:316-337`。

如果使用公共 RedisBloom 命令名，用户和工具会自然假设这段 AOF 可被 RedisBloom/Redis 8 Bloom 回放。当前并未成立。

本轮把 Redis 6.2.17 的 `aof-use-rdb-preamble` 关闭，强制触发 module command-AOF rewrite，结果 9 个 corpus 双向共 18 个单元全部失败。非空 corpus 双向都出现 false negative：

```text
gemini command-AOF -> RedisBloom:
  card=40, found=0/40
  Redis log:
  == CRITICAL == ... '-ERR received bad data' after processing 'bf.loadchunk'

RedisBloom command-AOF -> gemini:
  card=40, found=0/40
  Redis log:
  == CRITICAL == ... '-ERR cursor exceeds layer count' after processing 'BF.LOADCHUNK'
```

对照组：Redis 6.2.17 默认 `aof-use-rdb-preamble yes` 时，AOF rewrite 文件是 RDB preamble，不包含 `BF.LOADCHUNK`；该路径在完整矩阵 corpus 中双向通过：

```text
gemini RDB-preamble AOF -> RedisBloom v2.8.20: 9/9 corpora pass
RedisBloom v2.8.20 RDB-preamble AOF -> gemini: 9/9 corpora pass
total: 18/18 cells pass
```

因此 AOF 结论必须拆开写：RDB-preamble AOF 当前样本通过，但 command-AOF rewrite 明确失败。

建议二选一：

1. 完成官方 byte-offset `SCANDUMP/LOADCHUNK`，AOF 继续使用 `BF.LOADCHUNK`。
2. 保留私有协议，但改用私有命令名和私有 data type name，避免伪装成 RedisBloom 协议。

## INTEROP-04：复用 `MBbloom--` 但没有 official golden corpus

**级别：P0/P1**

模块注册：

```cpp
RedisModule_CreateDataType(ctx, "MBbloom--", kCurrentEncVer, &tm)
```

位置：`modules/gemini-bloom/src/redis_bloom_module.cc:19-29`。

这会让 Redis 认为该 key 是 RedisBloom Bloom 类型。只要 RDB 字段顺序、encver、flags、hash 参数、layer metadata 或 bit layout 有任何差异，就可能出现：

- RedisBloom RDB 载入 gemini 后 silent corruption。
- gemini RDB 被 RedisBloom 载入失败。
- migration tool 误把 gemini 数据当 RedisBloom 数据处理。

本轮新增正向信号：Redis 6.2.17 + RedisBloom v2.8.20 的 RDB 类迁移在 9 个 corpus 上双向通过：

```text
RDB RedisBloom v2.8.20 -> gemini:
  9/9 corpora pass

DUMP/RESTORE RedisBloom v2.8.20 -> gemini:
  9/9 corpora pass

RDB gemini -> RedisBloom v2.8.20:
  9/9 corpora pass

DUMP/RESTORE gemini -> RedisBloom v2.8.20:
  9/9 corpora pass

RDB-preamble AOF and fullsync replication:
  36/36 cells pass
```

这说明当前矩阵内的 RDB field order、hash seed、bit layout 与 RedisBloom v2.8.20 对齐，覆盖了 empty/single/multi/fixed/expansion/binary/long-item/large-empty corpus。它也覆盖了 Redis `DUMP/RESTORE`，因此单 key RDB object serialization 在本轮范围内是正向通过的。但这不能掩盖 public SCANDUMP/LOADCHUNK 与 command-AOF 失败。

仍未覆盖的是目标外版本矩阵，例如 Redis 8 Bloom 或 RedisBloom 其他版本；当前用户指定范围只要求 Redis 6.2 + RedisBloom v2.8.20。

建议：在 Redis 6.2 + RedisBloom v2.8.20 范围内，可把 RDB、`DUMP/RESTORE`、RDB-preamble AOF、fullsync replication 标为矩阵通过；但整体 RedisBloom 迁入迁出仍因 SCANDUMP/LOADCHUNK 和 command-AOF 失败而不兼容。

## INTEROP-05：缺少 RedisBloom 版本矩阵

**级别：P2**

当前代码注释多处写“intended to match RedisBloom”，但没有固定目标版本：

- RedisBloom 2.8.x independent module。
- Redis 8 内置 Bloom。
- 历史 encver 2 / 4 行为。

建议：文档和测试都写清楚目标版本。最少应覆盖 RedisBloom 2.8.x 和 Redis 8 Bloom 的一个固定版本。

本轮用户指定范围已覆盖：

```text
redis-server: 6.2.17
RedisBloom:   v2.8.20 (MODULE LIST ver=20820)
```

Redis 8 内置 Bloom 和 RedisBloom 其他版本不属于本轮更新后的目标范围。

## INTEROP-06：live replication command stream 的 `BF.CARD` 可观察状态不一致

**级别：P1/P2**

本轮完整矩阵新增 live replication 覆盖：

```text
RedisBloom master -> gemini replica
gemini master -> RedisBloom replica
```

其中 16/18 个单元通过，但 `expansion1` corpus 双向失败：

```text
RedisBloom master -> gemini replica:
  source BF.CARD=19, inserted items found=20/20
  target BF.CARD=20, inserted items found=20/20

gemini master -> RedisBloom replica:
  source BF.CARD=20, inserted items found=20/20
  target BF.CARD=19, inserted items found=20/20
```

原因是 live replication 复制的是 `BF.RESERVE` / `BF.ADD` command stream，而不是 RDB object。Bloom filter 在高 false-positive 场景下可能出现 `BF.ADD` 返回 0、`BF.CARD` 不等于 attempted item 数的合法行为；RedisBloom v2.8.20 与 gemini 在 `EXPANSION 1` 下的 `BF.CARD` 演进不同。membership 没有 false negative，但 `BF.CARD` 是 RedisBloom API 的可观察状态，因此不能把 live command replay 视作完全兼容。

对照组：fullsync replication 走 RDB snapshot，18/18 单元通过。

补充审计确认同一问题也存在于 incremental AOF command stream，而不只存在于 replication：

```text
incremental AOF command stream:
  single_layer: 2/2 pass
  fixed_full:   2/2 pass
  expansion1:   0/2 pass

RedisBloom incremental AOF -> gemini:
  source BF.CARD=19, inserted items found=20/20
  target BF.CARD=20, inserted items found=20/20

gemini incremental AOF -> RedisBloom:
  source BF.CARD=20, inserted items found=20/20
  target BF.CARD=19, inserted items found=20/20
```

因此 AOF 需要拆成三类结论：

- RDB-preamble AOF：本轮 corpus 双向通过。
- command-AOF rewrite：双向失败，原因是 `BF.LOADCHUNK` 协议不兼容。
- incremental AOF command stream：大多数普通 corpus 可回放，但 `EXPANSION 1` 下 `BF.CARD` 可观察状态不一致。

## INTEROP-07：`BF.SCANDUMP` 注册为 write，无法在 gemini 只读副本上导出

**级别：P1**

RedisBloom v2.8.20 把 `BF.SCANDUMP` 注册为 `readonly fast`，gemini 注册为 `write`：

```text
COMMAND INFO BF.SCANDUMP
  gemini:     flags=[write]
  RedisBloom: flags=[readonly, fast]
```

补充审计在同模块 master -> replica 场景中验证：

```text
RedisBloom replica:
  BF.SCANDUMP key 0 -> [1, <73-byte header>]

gemini replica:
  BF.SCANDUMP key 0 -> READONLY You can't write against a read only replica.
```

这会影响从只读 replica、只读连接、cluster replica 或只读路由执行导出的迁移工具。即使 byte-offset 协议修复，命令 flag 不修正也会让常见“从副本导出减轻主库压力”的迁移流程失败。

建议：`BF.SCANDUMP` 应注册为 `readonly fast`。如果实现内部需要分配临时 header buffer，也不应把命令标记为 write；RedisBloom v2.8.20 同样会在 SCANDUMP 中构造返回 payload，但命令语义仍是只读。

## INTEROP-08：`MIGRATE` 与带 TTL 的 RESTORE 双向通过

**级别：正向结论**

补充审计覆盖 Redis server 原生命令组合，而不仅是手工 `DUMP` / `RESTORE`：

```text
MIGRATE COPY REPLACE with PEXPIRE:
  RedisBloom -> gemini:      target card=8, found=8/8, target PTTL ~= 599996 ms
  gemini -> RedisBloom:      target card=8, found=8/8, target PTTL ~= 599996 ms

DUMP/RESTORE with explicit TTL:
  RedisBloom -> gemini:      restore card=8, found=8/8, restore PTTL ~= 599999 ms
  gemini -> RedisBloom:      restore card=8, found=8/8, restore PTTL = 600000 ms
```

这加强了 `MBbloom--` RDB object 兼容的正向证据：Redis server 级迁移命令使用的序列化对象在本轮 corpus 内双向可用，并且 TTL 由 Redis server 正确保留。该结论仍不能外推到 `SCANDUMP/LOADCHUNK` 或 command-AOF rewrite。

## INTEROP-09：module identity 不是 RedisBloom identity

**级别：P2**

虽然 data type name 复用 `MBbloom--`，模块 identity 不同：

```text
MODULE LIST
  gemini:     name=GeminiBloom, ver=1
  RedisBloom: name=bf, ver=20820
```

这意味着 RDB 能互通不等于服务实例对客户端呈现为 RedisBloom。依赖 `MODULE LIST` 检查 `bf` / `ver >= 20820` 的客户端和迁移工具会把 gemini 判定为“未加载 RedisBloom”。如果产品目标包含 drop-in client compatibility，需要决定是否调整 module name/version；如果只承诺 data migration compatibility，应在兼容声明里明确 module identity 差异。
