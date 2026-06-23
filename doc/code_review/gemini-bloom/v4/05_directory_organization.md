# 05 - 目录与组织问题

本文件关注文件职责、测试夹具、文档归属和模块边界。

## ORG-01：`bloom_rdb.cc` 同时承载 RDB、wire、AOF、validation，职责过宽

**级别：P2**

当前 `bloom_rdb.cc` 包含：

- `RdbWriter` / `RdbReader`
- `BloomLayer::WriteTo()` / `ReadFrom()`
- `ScalingBloomFilter::WriteTo()` / `ReadFrom()`
- SCANDUMP header 序列化 / 反序列化
- wire metadata validation
- module type callbacks
- AOF rewrite
- memory usage callback

位置：`modules/gemini-bloom/src/bloom_rdb.cc`。

### 建议

拆分：

```text
src/core/
  bloom_filter.*
  sb_chain.*
  murmur2.*

src/redis/
  bloom_commands.*
  bloom_module.*
  bloom_config.*

src/codec/
  bloom_rdb_codec.*
  bloom_scandump_codec.*
  bloom_aof_rewrite.*
  bloom_validation.*
```

## ORG-02：命令 parser 嵌在 Redis command handler 中，难以单元测试

**级别：P2**

`ParseInsertOptions()` 依赖 `RedisModuleString**` 和 `RedisModuleCtx*`，错误时直接 reply。

位置：`modules/gemini-bloom/src/bloom_commands.cc:218-303`。

`CmdReserve()` parser 更是完全嵌在 handler 中。

### 建议

抽象为纯 parser：

```cpp
Expected<ReserveOptions, ParseError> ParseReserve(TokenSpan);
Expected<InsertOptions, ParseError> ParseInsert(TokenSpan);
```

Redis command handler 只负责把 `RedisModuleString` 转 token 和把 `ParseError` 转 reply。

## ORG-03：缺少 official compatibility fixtures 目录

**级别：P1/P2**

当前没有：

```text
modules/gemini-bloom/tests/fixtures/redisbloom/
modules/gemini-bloom/tests/compat/
```

兼容性只能靠源码阅读推断，无法在 CI 中证明。

### 建议

新增：

```text
tests/fixtures/redisbloom-2.8/
  scandump-small.resp
  scandump-large.resp
  rdb-single-layer.rdb
  rdb-multi-layer.rdb
  aof-loadchunk.aof
  info-resp2.trace
  info-resp3.trace

tests/compat/
  redisbloom_cross_load_test.py
```

## ORG-04：关键兼容/格式说明只存在 code_review 文档，不属于模块正式文档

**级别：P2/P3**

源码注释提到 intended to match RedisBloom，但没有模块正式文档说明格式。

### 建议

新增：

```text
modules/gemini-bloom/README.md
modules/gemini-bloom/COMPATIBILITY.md
modules/gemini-bloom/FORMAT.md
modules/gemini-bloom/TESTING.md
```

其中 `FORMAT.md` 固化 RDB、SCANDUMP header、layer metadata、endianness、double、encver 与 RedisBloom 对应关系。

## ORG-05：构建产物名、模块名、data type name 三者语义不一致

**级别：P2**

当前：

```text
shared library: redis_bloom.so
module name:    GeminiBloom
data type name: MBbloom--
```

位置：

- `modules/gemini-bloom/CMakeLists.txt`
- `modules/gemini-bloom/src/redis_bloom_module.cc:9-30`

### 建议

根据产品定位统一命名：

- 如果是 RedisBloom 兼容实现，模块名/文档/测试都应明确兼容版本。
- 如果是 Gemini 私有实现，shared library 和 data type name 应避免伪装成 RedisBloom。

## ORG-06：代码审计文档版本多，但缺少“当前推荐修复顺序”

**级别：P3**

`doc/code_review/gemini-bloom` 下有 v1/v2/v3/v4，旧结论中有些已修复，有些仍成立。没有一个面向执行的 backlog。

### 建议

在 `doc/code_review/gemini-bloom/README.md` 增加：

```text
Current authoritative review: v4
Top 5 fixes:
1. SCANDUMP/LOADCHUNK byte-offset protocol
2. BF.INFO/RESP3 reply shape
3. Unified RDB/wire validator
4. Official golden corpus
5. Compatibility/native mode decision
```

