# 02 — 与 RedisBloom 兼容性问题

本文件只讨论与 Redis 官方命令文档、RedisBloom 官方源码行为、wire/RDB/AOF 格式和客户端响应形状的差异。核心结论：当前实现不能宣称“RedisBloom 兼容”，尤其是 SCANDUMP/LOADCHUNK、NoRound bit/byte 对齐、RESP3/BF.INFO 返回形状存在硬不兼容。

## COMPAT-01：NoRound 模式 bit/byte 对齐与 RedisBloom 不一致，导致 bit array、hash probe、SCANDUMP header 都不兼容

**级别：P0/P1**

### 证据

当前实现：

- 命令创建 filter 时固定使用 `Use64Bit | NoRound`。
- `BloomLayer::Create()` 在 NoRound 下：`totalBits_ = max(rawBits, 1024)`，`dataSize_ = ceil(totalBits_/8)`，probe 使用 `raw % totalBits_`。
- 位置：`src/bloom_commands.cc:21-33`，`src/bloom_filter.cc:93-126`，`src/bloom_filter.h:82-89`。

RedisBloom official：

- `bfCreateChain()` 使用 `BLOOM_OPT_FORCE64 | BLOOM_OPT_NOROUND`。
- official `bloom_init()` 在 NOROUND 下先算 `bits = entries * bpe`，随后把 `bytes` 对齐到 8 字节边界，并设置 `bloom->bits = bloom->bytes * 8`。
- official probe 在 compat path 使用 `hash % bloom->bits`。
- 位置：RedisBloom `src/rebloom.c:102-105`，`deps/bloom/bloom.c:158-192, 117-119`。

### 差异

```text
gemini:
  totalBits = floor(entries * bpe)
  bytes     = ceil(totalBits / 8)
  modulo    = totalBits

RedisBloom:
  rawBits   = floor(entries * bpe)
  bytes     = align_up(rawBits, 64) / 8
  bits      = bytes * 8
  modulo    = bits
```

同一个 item 的 hash probe 位置不同。即便 header 字段布局相同，bit array 内容也不能互通。

### 影响

- RedisBloom 生成的 `BF.SCANDUMP` chunk 用 gemini `BF.LOADCHUNK` 加载后可能出现 false negative。
- gemini 生成的 dump 用 RedisBloom 加载也会不一致或被 integrity check 拒绝。
- RDB/AOF 也无法安全互通。
- README 中“supporting the full BF.* command set”的表述容易被理解成兼容 RedisBloom，但底层格式并不兼容。

### 修复建议

按 RedisBloom official `bloom_init()` 实现字节/bit 对齐：

```cpp
if (bits % 64) bytes = ((bits / 64) + 1) * 8;
else bytes = bits / 8;
bits = bytes * 8;
```

并将 `totalBits_` 设为对齐后的 `bits`，probe 使用对齐后的 `bits`。

---

## COMPAT-02：SCANDUMP/LOADCHUNK cursor 语义错误：gemini 用“层序号”，RedisBloom 用“字节偏移”

**级别：P0/P1**

### 证据

当前实现：

```text
BF.SCANDUMP key 0   -> [1, header]
BF.SCANDUMP key 1   -> [2, layer0_full_bits]
BF.SCANDUMP key 2   -> [3, layer1_full_bits]
...
BF.SCANDUMP key N+1 -> [0, ""]
```

位置：`src/bloom_commands.cc:124-177`。

RedisBloom official：

- `SB_CHUNKITER_INIT = 1`。
- `getLinkPos()` 把 iterator 映射为所有 sub-filter bit arrays 拼接后的 byte offset。
- `SBChain_GetEncodedChunk()` 返回最多 `MAX_SCANDUMP_SIZE` 字节，`curIter += len`。
- `SBChain_LoadEncodedChunk()` 用 `iter - bufLen` 还原 offset。
- 位置：RedisBloom `src/sb.h:85-100`，`src/sb.c:172-220, 321-345`，`src/rebloom.c:151-188, 520-561`。

### 差异

RedisBloom 的 iterator 是“下一段数据的全局字节位置 + 1”语义。gemini 的 iterator 是“下一层 index + 1”语义。Redis 官方文档示例里，`BF.SCANDUMP bf 0` 返回 iterator 1，`BF.SCANDUMP bf 1` 返回 iterator 9，已经说明第二次 iterator 不是 2，而是按 chunk 字节长度推进。

### 影响

- official RedisBloom 的 dump 不能用 gemini 正确加载。
- gemini 的 dump 不能用 official RedisBloom 正确加载。
- 大 filter 无法按 16MB 分块；gemini 会一次返回整层 bit array。
- 客户端只按文档“把上一次返回 iterator 传回”可能在小 filter 下自洽可用，但与 official cursor 数值不一致，任何 cross implementation restore 都会失败。

### 修复建议

复用 official 语义：cursor 0 返回 header；cursor > 0 表示拼接 bit array 的 byte offset；`next_cursor = cursor + chunk_len`；done 为 `(0, NULL/empty)`。LOADCHUNK 必须支持任意 offset/length chunk，而不是要求整层 exact length。

---

## COMPAT-03：LOADCHUNK 只接受整层 bit array，RedisBloom 接受任意合法 chunk

**级别：P1**

### 证据

当前实现：cursor > 1 时 `idx = cursor - 2`，要求 `dataLen == layer.bloom.GetDataSize()`，直接写到层首地址。位置：`src/bloom_commands.cc:216-232`。

RedisBloom official：`SBChain_LoadEncodedChunk(sb, iter, buf, bufLen)` 允许 chunk 是某一层中的任意连续片段，通过 `iter - bufLen` 计算写入 offset。位置：RedisBloom `src/sb.c:321-345`。

### 影响

- 任何官方 chunk size 小于层大小的 dump 都不能导入 gemini。
- 大层 dump 会完全失败，因为 official 会切成多段。
- gemini 的 LOADCHUNK 不是官方增量协议，只是“header + layer blobs”的私有协议。

### 修复建议

实现 byte-offset loader：`offset = iterator - dataLen - 1`，resolve 到 layer + inner offset，再 memcpy；按 official 规则拒绝 `iter <= 0`、`iter < bufLen`、跨层超界 chunk。

---

## COMPAT-04：BF.INFO 单字段返回形状不兼容

**级别：P1**

### 证据

Redis 文档：RESP2 请求单字段时返回 singleton array，例如 `BF.INFO bf1 CAPACITY` 返回 `1) (integer) 100`；RESP3 完整信息应返回 map。

当前实现：`BF.INFO key Capacity` 直接返回 integer，不包 singleton array；完整信息始终返回 array，不区分 RESP3。位置：`src/bloom_commands.cc:54-104`。

### 影响

- 客户端库按 RedisBloom 协议解析单字段 BF.INFO 时会失败。
- RESP3 客户端无法得到 map reply。
- 当前 TCL 测试把 scalar 当正确结果，反而固化了不兼容行为。

### 修复建议

- RESP2 单字段：`ReplyWithArray(ctx, 1)` 后回复整数/null。
- RESP3 完整：`RedisModule_ReplyWithMap()` 或按 Redis Module API 支持检测 RESP3。

---

## COMPAT-05：BF.INSERT `NOCREATE` 与 `CAPACITY`/`ERROR` 的互斥规则未实现

**级别：P1**

### 证据

Redis 文档明确说明：`NOCREATE` 与 `CAPACITY` 或 `ERROR` 同时指定是错误。当前 `ParseInsertOptions()` 接受 `ERROR`、`CAPACITY`、`NOCREATE` 任意组合，只有 `NONSCALING` 与 `EXPANSION` 会被互斥检查。位置：`src/bloom_commands.cc:221-275`。

### 影响

这些命令在 gemini 下可能成功或只在 key missing 时返回“key does not exist”：

```text
BF.INSERT k NOCREATE CAPACITY 100 ITEMS a
BF.INSERT k NOCREATE ERROR 0.01 ITEMS a
```

官方客户端/测试会认为这是协议错误。

### 修复建议

记录 `capacitySet`、`errorSet`，解析后：

```cpp
if (opts.noCreate && (capacitySet || errorSet)) {
  return RedisModule_ReplyWithError(ctx,
    "ERR NOCREATE cannot be used with CAPACITY or ERROR");
}
```

---

## COMPAT-06：BF.RESERVE 参数数量与重复 option 处理不匹配 RedisBloom

**级别：P2/P1**

### 证据

当前实现：`CmdReserve()` 只检查 `argc < 4`，任意数量 option 都会解析，不拒绝重复 `EXPANSION` 或重复 `NONSCALING`。位置：`src/bloom_commands.cc:96-139`。

RedisBloom official：`BFReserve_RedisCommand()` 检查 `argc < 4 || argc > 7`。位置：RedisBloom `src/rebloom.c:137-142`。

### 影响

gemini 接受 official 不接受的命令：

```text
BF.RESERVE k 0.01 100 EXPANSION 2 EXPANSION 3
BF.RESERVE k 0.01 100 NONSCALING NONSCALING
```

### 修复建议

对齐 official parser 或明确声明“非 RedisBloom 兼容扩展”。如果目标是兼容，应拒绝重复 option 和超长参数。

---

## COMPAT-07：EXPANSION=0 语义不同

**级别：P2/P1**

### 证据

当前实现：`BF.RESERVE` / `BF.INSERT` / module config 都要求 `EXPANSION >= 1`。位置：`src/bloom_commands.cc:120-129, 248-256`，`src/bloom_config.cc:39-50`。

RedisBloom official：config 中 `bf_expansion_factor.min = 0`；`BF.RESERVE` 遇到 `EXPANSION 0` 会设置 nonScaling；`BF.INSERT` 遇到 `options.expansion == 0` 会设置 nonScaling。位置：RedisBloom `src/config.c:30-35`，`src/rebloom.c:171-184, 99-101`。

### 影响

如果用户或工具依赖 RedisBloom 的 `EXPANSION 0` 表示 non-scaling，gemini 会拒绝。

### 修复建议

若目标是 RedisBloom 行为兼容：允许 0 并映射到 `FixedSize`。若目标只跟随文档“positive integer”：README 必须声明与 RedisBloom implementation behavior 不完全一致，并测试覆盖。

---

## COMPAT-08：RESP3 布尔返回不兼容

**级别：P2**

### 证据

当前实现所有 membership/add 结果都用 integer reply。位置：`src/bloom_commands.cc:83-92, 164-208, 317-378`。

RedisBloom official 在 RESP3 下使用 bool reply：`BFCheck_RedisCommand()` 和 insert common path 都区分 RESP3 bool。位置：RedisBloom `src/rebloom.c:247-260, 304-307`。

### 影响

RESP3 客户端看到的类型不同。严格类型解析客户端会不兼容。

### 修复建议

增加 RESP version 检测，并按 official 形状返回 bool。

---

## COMPAT-09：错误消息不兼容，且有些错误类型过于泛化

**级别：P2**

| 场景 | gemini | RedisBloom official/docs |
|---|---|---|
| `BF.INSERT NOCREATE` missing key | `ERR key does not exist` | official common path会走 `statusStrerror(SB_EMPTY)`，返回 `ERR not found` |
| non-scaling full | `ERR reached capacity limit (non-scaling mode)` | official source返回 `ERR non scaling filter is full` |
| bad expansion | `ERR EXPANSION value must be a positive integer` | official source有 `Bad expansion` 或 range error |
| corrupt LOADCHUNK header | `ERR corrupted header payload` | official `ERR received bad data` |

### 影响

Redis 客户端通常不应该依赖完整错误字符串，但现实中测试、运维脚本、兼容层经常做 pattern match。错误语义不一致会造成迁移问题。

### 修复建议

维护 RedisBloom error compatibility matrix；至少对常见错误保留官方 prefix 和关键短语。

---

## COMPAT-10：RDB/data type 名称伪装成 RedisBloom，但 module identity 不是 RedisBloom

**级别：P2**

### 证据

当前实现：`RedisModule_Init(ctx, "GeminiBloom", 1, ...)`，`RedisModule_CreateDataType(ctx, "MBbloom--", kCurrentEncVer, ...)`。位置：`src/redis_bloom_module.cc:11-31`。

### 问题

数据类型名使用 RedisBloom 的 `MBbloom--`，但 module 名为 `GeminiBloom`。这会带来两个方向的问题：

1. 运行时客户端通过 `MODULE LIST` 判断 RedisBloom 是否存在，可能看不到预期模块名。
2. 与官方 RedisBloom/Redis 8 内置 Bloom 同时加载时，命令名和 data type name 可能冲突。

### 修复建议

如果不完全兼容，不应复用 `MBbloom--`。如果必须复用，就需要通过 official golden corpus 证明 RDB/SCANDUMP/AOF 完全兼容。

---

## COMPAT-11：wire header 字段布局看似相同，但完整 integrity 规则不相同

**级别：P1**

`WireLayerMeta` 表面对应 RedisBloom `dumpedChainLink`：`dataSize/bytes`、`totalBits/bits`、`itemCount/size`、`fpRate/error`、`bitsPerEntry/bpe`、`hashCount/hashes`、`capacity/entries`、`log2Bits/n2`。但 official 加载后会调用 `bloom_validate_integrity()` 和 `SB_ValidateIntegrity()`；gemini 只做部分校验。

### 影响

- official 拒绝的数据，gemini 可能接受。
- gemini 接受后可能 crash，见 `01_code_bugs.md#BUG-01`、`BUG-03`、`BUG-04`。
- 表面字段相同不代表兼容。

### 修复建议

直接移植或逐条复刻 official 的 integrity checks。

---

## COMPAT-12：Rounded mode 的 capacity 语义与 RedisBloom 不一致

**级别：P2**

当前实现：未设置 `NoRound` 时会 bit_ceil `totalBits_`，但 `capacity_` 保持原始 `cap`。位置：`src/bloom_filter.cc:113-117`。

RedisBloom official：rounded mode 会计算额外 bits 能多存多少 item，并增加 `bloom->entries`。位置：RedisBloom `deps/bloom/bloom.c:169-185`。

### 影响

虽然命令层当前总是使用 NoRound，但 RDB/wire 可能加载 rounded filter。此时 `BF.INFO Capacity` 和扩容判断与 official 不一致。

### 修复建议

若支持 rounded mode，按 official 更新 entries/capacity。若不支持，应在 loader 中拒绝非 NoRound 数据。

---

## COMPAT-13：`BLOOM_OPT_ENTS_IS_BITS` / RawBits 语义不兼容

**级别：P1**

RedisBloom official：`BLOOM_OPT_ENTS_IS_BITS = 2`，该模式把 `entries` 解释为 bit exponent `n2`，仍会计算 `entries = bits / bpe` 和 hashes。位置：RedisBloom `deps/bloom/bloom.h:67-78`、`deps/bloom/bloom.c:148-157`。

gemini：`BloomFlags::RawBits = 2`，创建时 `totalBits = cap`、`hashCount = 0`。位置：`src/bloom_filter.h:18-24`、`src/bloom_filter.cc:100-103`。

### 影响

任何 official RawBits/ENTS_IS_BITS 数据都不能被 gemini 正确解释。

### 修复建议

要么实现 official 语义，要么拒绝 `RawBits` flags。

---

## COMPAT-14：官方 RedisBloom 源码支持 `BF.DEBUG`，gemini README 宣称“full BF.*”但未实现

**级别：P3/P2**

RedisBloom official 源码中有 `BFDebug_RedisCommand()`。当前 README 列出的命令只包括 10 个主要 BF 命令。

### 影响

`BF.DEBUG` 是否算公共兼容面需要确认。如果目标是“RedisBloom 用户可迁移”，debug/diagnostic command 缺失会影响排障工具。

### 修复建议

如果不支持，README 不要写“full BF.* command set”；改为“supports the following BF commands”。如果要支持，补 `BF.DEBUG` 或兼容性声明。
