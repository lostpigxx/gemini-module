# 08 - 其他问题

本文件记录不完全属于代码 bug、兼容差异、性能或测试覆盖的问题，但这些问题会影响上线、维护和迁移。

## OTHER-01：RDB / AOF / LOADCHUNK 的安全边界没有写清楚

**级别：P1/P2**

RDB 文件、AOF 文件和 `BF.LOADCHUNK` payload 都可能来自备份、迁移工具、外部环境或不可信客户端。当前代码已经有部分校验，但文档没有把这些输入视为非信任边界。

### 建议

在模块文档中明确：

- 是否支持加载外部 RedisBloom 数据。
- 是否允许从不可信客户端调用 `BF.LOADCHUNK`。
- 损坏 payload 的错误保证：不会崩溃、不会越界、不会破坏已有 key。
- RDB/LOADCHUNK fuzz 和 sanitizer 是 release gate。

## OTHER-02：Redis 8 内置 Bloom 与本模块共存策略不明

**级别：P2**

Redis 8 已包含 Bloom 命令。gemini-bloom 也注册 `BF.*` 命令和 `MBbloom--` 类型名。用户在 Redis 8 或 Redis Stack 环境加载本模块时可能遇到：

- 命令名冲突
- data type name 冲突
- RDB 互读误判
- `COMMAND INFO` 不一致

### 建议

在 README / COMPATIBILITY 中写明支持的 Redis 版本矩阵：

| Redis 版本 | 支持状态 | 注意事项 |
|---|---|---|
| 6.x | 待验证 | RESP3/config 能力有限 |
| 7.x | 待验证 | module config/ACL 需要测试 |
| 8.x | 待验证 | 内置 BF 命令冲突风险 |

## OTHER-03：许可证和代码来源边界需要明确

**级别：P2/P3**

`murmur2.cc` 注释写 MurmurHash2 public domain。其他 RedisBloom 行为若从 upstream 移植，需要注意 RedisBloom 当前许可证组合。

### 建议

仓库层补充：

- 项目整体 license。
- MurmurHash2 来源和 license。
- RedisBloom 参考实现是否只是行为对齐，还是复制/改写代码。
- 如果复制 RedisBloom 代码，记录来源 commit 和 license 义务。

## OTHER-04：版本策略不完整

**级别：P2**

当前：

- `RedisModule_Init(ctx, "GeminiBloom", 1, ...)`
- RDB `kCurrentEncVer = 4`
- data type name `MBbloom--`

位置：

- `modules/gemini-bloom/src/redis_bloom_module.cc:9-30`
- `modules/gemini-bloom/src/bloom_rdb.h:23-29`

但没有定义：

- module semantic version
- native format version
- RedisBloom compatibility version
- RDB/wire migration policy
- downgrade/upgrade policy

### 建议

建立版本表并在 release note 中维护。

## OTHER-05：缺少运维可观测性

**级别：P3**

当前日志很少，`BF.INFO` 字段有限，缺少：

- 每层饱和度
- 当前 false-positive 目标
- 最近一次 expansion failure 原因
- loaded format version
- compatibility mode

### 建议

提供 `GEMINI.BF.DEBUG` 或 module info command。必要时把关键统计暴露到 Redis `INFO MODULES`。

## OTHER-06：README 的“full BF.* command set”容易被理解成完全兼容 RedisBloom

**级别：P2**

README 当前只列命令名，不说明限制：

位置：`README.md:7-10`。

### 建议

改成更精确的表述：

```text
gemini-bloom implements the main RedisBloom Bloom Filter command names.
RedisBloom RDB/AOF/SCANDUMP compatibility is not yet guaranteed.
RESP3 compatibility is incomplete.
```

并链接到 `modules/gemini-bloom/COMPATIBILITY.md`。

## OTHER-07：缺少 release checklist

**级别：P3**

建议每次 release 前检查：

```text
[ ] GTest pass
[ ] TCL integration pass
[ ] ASAN/UBSAN pass
[ ] RDB/LOADCHUNK fuzz pass
[ ] RedisBloom golden corpus pass or documented fail
[ ] RESP2/RESP3 raw reply shape pass
[ ] COMMAND INFO / ACL pass
[ ] README compatibility table updated
```

