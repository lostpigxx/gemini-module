# 08 — 其他问题

本文件收集安全、运维、文档、发布、可移植性、法律/许可证和项目治理层面的问题。

## OTHER-01：安全边界没有写清楚，RDB/LOADCHUNK 实际是非信任输入

**级别：P1**

RDB 文件、AOF、`BF.LOADCHUNK` data 都可以来自外部系统或备份文件。当前 loader 校验不足，存在越界、除 0、内存放大风险。

### 建议

在文档和代码中把所有持久化/wire 输入视为 untrusted，加载路径必须满足：

```text
parse -> validate full metadata -> allocate -> construct -> publish
```

不能边解析边发布。

---

## OTHER-02：复用 RedisBloom data type name 的风险很高

**级别：P1/P2**

如果格式没有 100% 兼容，使用 `MBbloom--` 会让 Redis/RDB/AOF/工具误以为它是 RedisBloom 数据。数据迁移时可能出现 silent corruption。

### 建议

在兼容性未被 official golden corpus 证明前：

```text
native data type name: GMBloom--
compat mode data type: MBbloom-- only after verified
```

---

## OTHER-03：README 的兼容性表述过强

**级别：P2**

README 写“supporting the full BF.* command set”。但源码注释又说 full compatibility 未验证。

实际存在：RESP3 缺失、BF.INFO 形状不兼容、SCANDUMP/LOADCHUNK wire 不兼容、NoRound bit alignment 不兼容、NOCREATE 参数规则不兼容。

### 建议

改成：

```text
gemini-bloom implements a subset of RedisBloom BF command names.
RedisBloom wire/RDB/AOF compatibility is not yet guaranteed.
See modules/gemini-bloom/COMPATIBILITY.md.
```

---

## OTHER-04：没有发布/版本策略

**级别：P3/P2**

`RedisModule_Init(ctx, "GeminiBloom", 1, ...)` 固定 module version 1。RDB encver 当前是 4，但模块版本、格式版本、兼容版本没有统一策略。

### 建议

定义：module version、native format version、redisbloom compatibility version、command compatibility level，并在 `BF.INFO`/`MODULE LIST`/日志中可见。

---

## OTHER-05：没有故障诊断命令

**级别：P3/P2**

当用户怀疑 filter 损坏、容量异常、误判率异常、层数过多时，当前只有 `BF.INFO` 的 5 个字段。

### 建议

实现兼容 `BF.DEBUG` 或私有 `GEMINI.BF.DEBUG`，输出每层：

```text
bytes
bits
hashes
hash-width
capacity
itemCount
fpRate
bpe
log2Bits
flags
```

同时可提供 `VALIDATE` 子命令做 integrity check。

---

## OTHER-06：缺少 fuzzing 策略

**级别：P2**

本模块最危险的输入是二进制 header/RDB/chunk。当前测试主要是手写案例，不足以覆盖组合状态。

### 建议

增加 fuzz targets：

```text
FuzzDeserializeHeader(data)
FuzzRdbRead(stream)
FuzzLoadChunkSequence(chunks)
FuzzCommandParser(tokens)
```

配合 ASAN/UBSAN/LSAN。

---

## OTHER-07：没有明确支持的 Redis 版本矩阵

**级别：P3**

Redis Module API、RESP3、config registration、ACL categories、Redis 8 内置 Bloom 都与 Redis 版本有关。当前 README 只写如何 load module。

### 建议

文档列出：

| Redis version | 支持状态 | 限制 |
|---|---|---|
| 6.x | 待验证 | RESP3/config 支持有限 |
| 7.x | 待验证 | 需要 integration |
| 8.x | 待验证 | 内置 Bloom 冲突风险 |

---

## OTHER-08：缺少二进制兼容/端序说明

**级别：P3**

SCANDUMP header 使用 packed struct 和 raw double，没有说明 endian、double 格式、packing 假设。RedisBloom official 也是如此，但 gemini 自研实现仍应把假设写清楚。

### 建议

在 `FORMAT.md` 写：

```text
little-endian assumed
IEEE-754 double assumed
#pragma pack(1)
field widths fixed
no cross-endian guarantee
```

---

## OTHER-09：没有用户迁移指南

**级别：P2/P3**

如果用户已经有 RedisBloom 数据，当前不知道：是否能直接加载 RDB、是否能通过 SCANDUMP/LOADCHUNK 迁移、AOF 是否可重放、MODULE LIST/command 名冲突怎么处理、Redis 8 内置 Bloom 环境如何使用。

### 建议

新增 `MIGRATION.md`，先明确“不支持直接迁移”或列出经测试路径。

---

## OTHER-10：错误日志和用户错误没有统一等级

**级别：P3**

AOF rewrite alloc failure 使用 `RedisModule_LogIOError` warning；命令 alloc failure 返回 `ERR allocation failure`；config 解析用 warning。没有统一的错误码/日志策略。

### 建议

定义错误分类：user input error、wrong type/missing key、allocation failure、corrupt persistence input、internal invariant violation、compatibility rejection，并统一消息前缀和日志等级。

---

## OTHER-11：没有内存限制/拒绝策略文档

**级别：P2**

Bloom filter 可能按用户参数分配大量内存。当前只依赖 Redis `deny-oom` 和 allocator 失败。LOADCHUNK/RDB 还可能绕过命令层参数范围。

### 建议

添加模块级 max memory / max capacity / max layer bytes 配置，所有路径共享。

---

## OTHER-12：许可证和第三方代码来源需要更清楚

**级别：P3**

`murmur2.cc` 注释说 MurmurHash2 public domain。项目整体许可证、RedisBloom 兼容参考、复制/改写边界需要在仓库层说明。

### 建议

- 根 README 标明项目 license。
- `NOTICE` 中列出 MurmurHash2 来源。
- 若移植 RedisBloom integrity/format 代码，注意 RedisBloom 当前源码许可证组合，并明确是否只是行为兼容还是代码复制。

---

## OTHER-13：没有安全回归分级

**级别：P2/P3**

像 RDB/LOADCHUNK 越界、除 0、OOM 放大这类问题应有安全 severity 和回归测试要求。当前 commit message 有“fix bugs”，但没有 security checklist。

### 建议

引入 PR checklist：

```text
[ ] 所有外部 binary 输入有完整长度检查
[ ] 所有乘法/加法有 overflow check
[ ] 所有 object lifetime 失败路径被 sanitizer 覆盖
[ ] 所有 Redis reply shape 有 RESP2/RESP3 测试
[ ] 所有 RedisBloom 兼容声明有 golden test
```

---

## OTHER-14：审计文档不应混在实现结论里当作通过依据

**级别：P3**

仓库历史中有 code review 文档和“fix bugs”提交。测试文件也包含“Bug regression”注释。这些是好事，但不能替代持续测试和兼容金样本。

### 建议

把 review 发现转化为 issue tracker、regression tests、coverage gates、compatibility matrix，而不是仅保留文档或注释。
