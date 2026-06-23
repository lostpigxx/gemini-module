# 02 - 与 RedisBloom 兼容性问题

本文件只讨论与 Redis 官方命令文档、RedisBloom upstream 行为、RESP 返回形状、RDB/AOF/SCANDUMP 互通相关的问题。

## COMPAT-01：`BF.SCANDUMP` cursor 语义仍是私有 layer index，不是 RedisBloom byte offset

**级别：P0/P1**

### 证据

gemini-bloom 当前协议写在注释里：

```text
SCANDUMP key 0   -> [1, header]
SCANDUMP key 1   -> [2, layer0_bits]
SCANDUMP key 2   -> [3, layer1_bits]
...
```

实现位置：`modules/gemini-bloom/src/bloom_commands.cc:479-530`。

Redis 官方文档要求 `BF.SCANDUMP` 返回连续 `(iter, data)`，直到 `(0, NULL)`；`iter` 应使用上一次返回值继续调用。

RedisBloom upstream：

- `SB_CHUNKITER_INIT = 1`
- `SBChain_GetEncodedChunk()` 基于 `curIter` 返回增量 chunk。
- `maxChunkSize` 限制单次返回大小。

### 差异

gemini 的 cursor 是“第几个 layer + 1”。RedisBloom 的 cursor 是全局数据流 byte offset 递进值。小 filter 下“把返回 cursor 直接传给 LOADCHUNK”可以在 gemini 内自洽，但 cursor 数值和 chunk 边界都不兼容 RedisBloom。

### 影响

- RedisBloom 生成的 SCANDUMP 不能可靠导入 gemini。
- gemini 生成的 SCANDUMP 不能可靠导入 RedisBloom / Redis 8 Bloom。
- 任何依赖官方 byte-offset iterator 的迁移工具都会失败。

### 修复建议

按 RedisBloom 实现 byte-offset cursor：

```text
cursor 0 -> header, next = 1
cursor > 0 -> 从拼接后的 bit arrays 中按 offset 返回最多 maxChunkSize 字节
next = cursor + chunk_len
done = (0, empty/null)
```

## COMPAT-02：`BF.LOADCHUNK` 只接受整层 bit array，不支持官方任意 chunk

**级别：P1**

### 证据

gemini `cursor > 1` 时：

- `idx = cursor - 2`
- 要求 `dataLen == layer.bloom.GetDataSize()`
- 直接复制到该层起始地址

位置：`modules/gemini-bloom/src/bloom_commands.cc:571-586`。

RedisBloom upstream `SBChain_LoadEncodedChunk()`：

- 检查 `iter > 0 && iter >= bufLen`
- `iter -= bufLen`
- 用 `getLinkPos()` 找到目标 layer 和 offset
- 支持 chunk 是某层中的任意连续片段

### 影响

- 大 layer 被 RedisBloom 切成多段时，gemini 无法导入。
- gemini 的 LOADCHUNK 是 header + whole-layer blob 的私有协议。

### 修复建议

实现官方 offset loader，至少支持：

- chunk 小于 layer size
- chunk 位于 layer 中间
- chunk 处于 layer 边界前后
- iter/dataLen 不合法时返回 `ERR received bad data` 类错误

## COMPAT-03：AOF rewrite 使用 public `BF.LOADCHUNK` 命令承载私有 cursor 协议

**级别：P1**

### 证据

`AofRewriteBloom()` 输出：

```text
BF.LOADCHUNK key 1 <gemini header>
BF.LOADCHUNK key 2 <layer0 full bits>
BF.LOADCHUNK key 3 <layer1 full bits>
...
```

位置：`modules/gemini-bloom/src/bloom_rdb.cc:271-291`。

### 影响

生成的 AOF 命令名是 RedisBloom 公共命令，但参数语义不是官方协议。用户或工具自然会假设这段 AOF 可被 RedisBloom/Redis 8 回放，实际会失败或产生错误数据。

### 修复建议

二选一：

1. 实现官方 SCANDUMP/LOADCHUNK byte-offset 协议，让 AOF 使用标准 `BF.LOADCHUNK`。
2. 保持私有协议，但换成私有命令名，例如 `GEMINI.BF.LOADCHUNK`，并使用私有 data type name。

## COMPAT-04：`BF.INFO` 返回形状不兼容 RESP2/RESP3

**级别：P1**

### 证据

gemini 单字段直接返回标量：

```cpp
BF.INFO key Capacity -> integer
BF.INFO key Expansion -> integer/null
```

位置：`modules/gemini-bloom/src/bloom_commands.cc:423-440`。

完整信息始终返回 array：

位置：`modules/gemini-bloom/src/bloom_commands.cc:443-458`。

Redis 文档要求：

- RESP2 单字段返回 singleton array，例如 `BF.INFO bf1 CAPACITY` 返回 `1) (integer) 100`。
- RESP2 完整信息返回 string/integer pair array。
- RESP3 完整信息返回 map。

当前 TCL 测试反而把标量当作正确结果：

位置：`modules/gemini-bloom/tests/tcl/bloom_test.tcl:370-376`。

### 修复建议

- RESP2 single field：`ReplyWithArray(ctx, 1)` 后回复 value。
- RESP3 full info：`RedisModule_ReplyWithMap()`。
- RESP3 single field：按官方文档补 exact shape 测试。

## COMPAT-05：RESP3 boolean 返回未实现

**级别：P1/P2**

### 证据

gemini 始终用 integer reply：

- `BF.ADD`：`bloom_commands.cc:190-191`
- `BF.MADD` / `BF.INSERT`：`PutAndReply()` in `bloom_commands.cc:88-96`
- `BF.EXISTS`：`bloom_commands.cc:371-373`
- `BF.MEXISTS`：`bloom_commands.cc:397-402`

Redis 文档要求：

- RESP2：integer `0/1`
- RESP3：boolean `false/true`

RedisBloom upstream 也在 RESP3 下调用 `RedisModule_ReplyWithBool()`。

### 修复建议

增加 RESP version helper，并集中封装：

```cpp
static int ReplyBoolCompat(RedisModuleCtx* ctx, bool v) {
  if (IsResp3(ctx)) return RedisModule_ReplyWithBool(ctx, v);
  return RedisModule_ReplyWithLongLong(ctx, v ? 1 : 0);
}
```

## COMPAT-06：`BF.RESERVE` parser 接受 RedisBloom 不接受的重复/超长 option 组合

**级别：P2**

### 证据

gemini 只检查 `argc < 4`，之后循环解析任意数量 option：

位置：`modules/gemini-bloom/src/bloom_commands.cc:100-147`。

这会接受或以非官方方式处理：

```text
BF.RESERVE k 0.01 100 EXPANSION 2 EXPANSION 3
BF.RESERVE k 0.01 100 NONSCALING NONSCALING
BF.RESERVE k 0.01 100 EXPANSION 2 FOO BAR ...
```

RedisBloom upstream 对 `BF.RESERVE` 有 `argc < 4 || argc > 7` 检查，并通过 parser 处理 option。

### 修复建议

如果目标是兼容 RedisBloom，显式拒绝：

- 过长 argc
- 重复 `EXPANSION`
- 重复 `NONSCALING`
- `EXPANSION` 与 `NONSCALING` 任意顺序共存

## COMPAT-07：配置机制和配置范围未对齐 RedisBloom

**级别：P2**

### 证据

gemini 只支持 loadmodule argv：

```text
ERROR_RATE
INITIAL_SIZE
EXPANSION
```

位置：`modules/gemini-bloom/src/bloom_config.cc:9-57`。

RedisBloom upstream 注册 Redis module config：

```text
bf-error-rate
bf-initial-size
bf-expansion-factor
```

并定义：

```text
bf-initial-size:      1 .. 1<<30
bf-expansion-factor: 0 .. 32768
```

gemini 的 `EXPANSION` 配置拒绝 0，命令层却允许 `EXPANSION 0` 表示 non-scaling。

### 修复建议

- 注册现代 Redis module config。
- 保留 legacy load args 作为兼容入口。
- 统一 config 与命令的范围：`EXPANSION 0` 如果表示 non-scaling，配置也应允许。

## COMPAT-08：`BF.SCANDUMP` command flags/ACL category 与 Redis 文档不匹配

**级别：P2**

### 证据

gemini 注册 `BF.SCANDUMP` 为 `"readonly"`：

位置：`modules/gemini-bloom/src/bloom_commands.cc:601-611`。

Redis 文档把 `BF.SCANDUMP` 标为：

```text
ACL categories: @bloom, @write, @slow
```

### 影响

严格使用 ACL 或 `COMMAND INFO` 的客户端、代理、审计工具会观察到不同权限模型。

### 修复建议

补 `COMMAND INFO` / ACL golden tests。若决定保持 readonly，需要在兼容文档中明确偏离 Redis 行为；若目标是兼容，应对齐官方 command metadata。

## COMPAT-09：复用 RedisBloom data type name 但未证明 RDB/SCANDUMP/AOF 互通

**级别：P1**

### 证据

模块注册 data type name：

```cpp
RedisModule_CreateDataType(ctx, "MBbloom--", kCurrentEncVer, &tm)
```

位置：`modules/gemini-bloom/src/redis_bloom_module.cc:19-30`。

但源码注释仍写明 full interoperability has not been verified：

- `bloom_filter.h:11-15`
- `sb_chain.h:84-87`
- `bloom_rdb.cc:39-42`

### 影响

Redis/RDB/AOF 工具会把该数据当作 RedisBloom 类型处理。只要格式不是 100% 兼容，就可能出现 silent corruption 或不可恢复的数据迁移失败。

### 修复建议

在通过 official golden corpus 前，不要使用 `MBbloom--`；改用私有类型名，例如 `GMBloom--`。如果必须复用，先建立跨实现测试：

```text
RedisBloom RDB -> gemini load
gemini RDB -> RedisBloom load
RedisBloom SCANDUMP -> gemini LOADCHUNK
gemini SCANDUMP -> RedisBloom LOADCHUNK
RedisBloom AOF -> gemini replay
gemini AOF -> RedisBloom replay
```

