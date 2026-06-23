# 00 - 审计范围与方法

## 审计对象

审计对象为当前工作区的 `modules/gemini-bloom`：

- 核心数据结构：`src/bloom_filter.*`、`src/sb_chain.*`
- Redis 命令层：`src/bloom_commands.cc`
- RDB / SCANDUMP / LOADCHUNK / AOF：`src/bloom_rdb.*`
- 配置和模块入口：`src/bloom_config.*`、`src/redis_bloom_module.cc`
- 测试：`tests/*.cc`、`tests/tcl/bloom_test.tcl`
- 用户文档：仓库根 `README.md`

## 参考资料

本轮参考了以下官方资料：

- Redis 命令文档：
  - `BF.INFO`: https://redis.io/docs/latest/commands/bf.info/
  - `BF.SCANDUMP`: https://redis.io/docs/latest/commands/bf.scandump/
  - `BF.LOADCHUNK`: https://redis.io/docs/latest/commands/bf.loadchunk/
  - `BF.ADD`: https://redis.io/docs/latest/commands/bf.add/
  - `BF.EXISTS`: https://redis.io/docs/latest/commands/bf.exists/
- RedisBloom upstream source：
  - `src/rebloom.c`: https://github.com/RedisBloom/RedisBloom/blob/master/src/rebloom.c
  - `src/sb.c`: https://github.com/RedisBloom/RedisBloom/blob/master/src/sb.c
  - `src/sb.h`: https://github.com/RedisBloom/RedisBloom/blob/master/src/sb.h
  - `src/config.c`: https://github.com/RedisBloom/RedisBloom/blob/master/src/config.c
  - `deps/bloom/bloom.c`: https://github.com/RedisBloom/RedisBloom/blob/master/deps/bloom/bloom.c

关键官方行为点：

- `BF.SCANDUMP` 文档要求从 iterator 0 开始，持续返回 `(iter, data)`，直到 `(0, NULL)`。
- RedisBloom `SBChain_GetEncodedChunk()` 使用 iterator 作为 byte-offset 递进，并限制 `MAX_SCANDUMP_SIZE = 16MB`。
- RedisBloom `SBChain_LoadEncodedChunk()` 使用 `iter - bufLen` 还原写入 offset，支持任意合法 chunk。
- Redis `BF.INFO` 文档要求 RESP2 单字段返回 singleton array，RESP3 完整返回 map。
- Redis `BF.ADD` / `BF.EXISTS` 文档要求 RESP3 返回 boolean。
- RedisBloom `bloom_validate_integrity()` 会校验 error、bits、bytes、hashes 等核心元数据一致性。

## 已执行测试

临时构建目录：`/tmp/gemini-module-bloom-review`

执行结果：

```text
DYLD_LIBRARY_PATH=/opt/anaconda3/lib bloom_filter_test --gtest_color=no
  11 tests passed

DYLD_LIBRARY_PATH=/opt/anaconda3/lib sb_chain_test --gtest_color=no
  12 tests passed

DYLD_LIBRARY_PATH=/opt/anaconda3/lib bloom_rdb_test --gtest_color=no
  24 tests passed

tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl /tmp/gemini-module-bloom-review/redis_bloom.so
  89 passed, 0 failed
```

初次使用仓库内 `build/` 失败，因为 `CMakeCache.txt` 记录的是另一路径。GTest 初次运行也遇到 macOS `@rpath` 未配置问题，使用 `DYLD_LIBRARY_PATH=/opt/anaconda3/lib` 后通过。

## 相对 v3 的变化

以下 v3 问题在当前代码中已被修复或明显缓解，本轮不再按同一问题列为当前 bug：

- NoRound bit/byte 对齐已经改为 8-byte 对齐，并将 `totalBits = bytes * 8`。
- `FromRdbShell()` 已从 `numLayers_ = 0` 开始构造 layer，避免析构未构造对象。
- RDB load 已拒绝非 fixed filter 的 `expansionFactor == 0`。
- RDB/wire 已拒绝 `log2Bits >= 64` 和 `log2Bits > 0 && totalBits != 1 << log2Bits`。
- `BF.INSERT NOCREATE` 已与 `CAPACITY` / `ERROR` 互斥。
- `BF.RESERVE` / `BF.INSERT EXPANSION 0` 已映射到 non-scaling。
- `BF.LOADCHUNK` 已避免 malformed header 先删除已有 key，且会保护 wrong-type key。

## 严重级别

- P0：可导致 Redis 进程崩溃、内存破坏、持久化恢复失败、严重数据损坏。
- P1：会造成 RedisBloom 不兼容、错误业务结果、持久化语义错误、明显 DoS 风险。
- P2：实现不合理、边界语义差、测试缺口大、维护风险高。
- P3：文档、组织、可观测性或长期演进问题。
