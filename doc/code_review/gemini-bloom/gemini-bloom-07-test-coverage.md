# Codex Review: gemini-bloom 测试场景与代码覆盖问题

## 当前验证结果

所有运行均在 Docker container `974d83bcff5c` 内执行。

命令：

```bash
docker exec 974d83bcff5c bash -lc 'cd <repo> && cmake -B build'
docker exec 974d83bcff5c bash -lc 'cd <repo> && cmake --build build -j$(nproc)'
docker exec 974d83bcff5c bash -lc 'cd <repo> && tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl ./build/redis_bloom.so'
```

结果：

- 编译通过。
- Bloom Tcl 集成测试：`53 passed, 0 failed`。
- GoogleTest 单元测试目标未生成：容器内 CMake 没找到 GTest，`bloom_test` target 不存在。

gcov 构建和 Tcl 测试后，源码覆盖摘要：

| 文件 | 行覆盖 | 分支执行 | 分支至少 taken 一次 |
|---|---:|---:|---:|
| `bloom_commands.cc` | 86.60% | 98.40% | 70.00% |
| `bloom_filter.cc` | 84.38% | 92.86% | 60.71% |
| `sb_chain.cc` | 67.86% | 76.19% | 52.38% |
| `bloom_rdb.cc` | 80.13% | 78.57% | 46.43% |
| `bloom_config.cc` | 8.82% | 5.00% | 2.50% |
| `redis_bloom_module.cc` | 71.43% | 100.00% | 50.00% |
| `murmur2.cc` | 56.90% | 62.50% | 56.25% |

注意：这些覆盖率主要来自 Tcl 集成测试，不包含未运行的 GTest 单元测试。

## 高风险测试缺口

### 1. SCANDUMP/LOADCHUNK 测试没有使用官方 iterator 流程

位置：`modules/gemini-bloom/tests/tcl/bloom_test.tcl:460-468`

当前测试自己维护 `load_iter`，没有把 `BF.SCANDUMP` 返回的 iterator 原样传给 `BF.LOADCHUNK`。这掩盖了当前实现的核心协议错误。

必须新增：

- `iter,data = BF.SCANDUMP(key, iter)` 后，把同一个 `iter,data` 保存并传给 `BF.LOADCHUNK`。
- 覆盖单层、多层、大于 16MB layer 的分块场景。
- 覆盖官方 RedisBloom chunks -> gemini-bloom LOADCHUNK。
- 覆盖 gemini-bloom chunks -> 官方 RedisBloom LOADCHUNK。

### 2. 没有 malformed LOADCHUNK 测试

未覆盖路径：

- `modules/gemini-bloom/src/bloom_commands.cc:496`
- `modules/gemini-bloom/src/bloom_commands.cc:501`
- `modules/gemini-bloom/src/bloom_commands.cc:508`
- `modules/gemini-bloom/src/bloom_commands.cc:512`

必须新增：

- cursor 非数字、0、负数。
- header 太短、太长、numLayers=0、numLayers 超限。
- flags 非法。
- expansionFactor=0。
- dataSize 与 totalBits 不匹配。
- chunk 长度错误。
- chunk 乱序、重复、缺失。
- 在已有 Bloom key、已有 string key 上 LOADCHUNK。
- malformed header 不得删除原 key。

### 3. 没有 RDB 损坏输入测试

未覆盖路径：

- `modules/gemini-bloom/src/bloom_rdb.cc:53-61`
- `modules/gemini-bloom/src/bloom_rdb.cc:131-133`

必须新增：

- RDB blob 短读。
- bit array blob 长度小于/大于 metadata 推导值。
- numLayers 巨大。
- itemCount sum 与 totalItems 不一致。
- encver 0/1/2/3/4 的真实 RedisBloom 样本。
- encver > current 拒绝。

### 4. AOF rewrite 完全未覆盖

未覆盖路径：

- `modules/gemini-bloom/src/bloom_rdb.cc:220-237`

必须新增：

- 开启 AOF，写入 filter，执行 `BGREWRITEAOF`，重启加载。
- 检查 AOF 中 `BF.LOADCHUNK` cursor 是否符合 RedisBloom。
- 使用官方 RedisBloom 加载 gemini-bloom AOF。
- 使用 gemini-bloom 加载官方 RedisBloom AOF。

### 5. RESP3 未覆盖

当前 Tcl client 只处理 RESP2 简单类型，不处理 RESP3 bool/map/null。

必须新增：

- `HELLO 3` 后测试 `BF.ADD/BF.MADD/BF.EXISTS/BF.MEXISTS/BF.INFO`。
- 对比 RedisBloom 2.6+ 或 Redis 8 的 RESP3 reply type。

### 6. 配置加载几乎未覆盖

覆盖率显示 `bloom_config.cc` 行覆盖只有 8.82%。当前测试只覆盖默认参数。

必须新增：

- loadmodule `ERROR_RATE` 正常/非法。
- loadmodule `INITIAL_SIZE` 正常/非法。
- loadmodule `EXPANSION` 正常/非法，特别是 0。
- 未知参数。
- 参数缺值。
- 自动创建 filter 是否使用配置值。

### 7. wrong-type 覆盖不足

已有：

- `BF.ADD` on string。
- `BF.EXISTS` on string。
- `BF.INFO` on string。

缺少：

- `BF.MADD` on string。
- `BF.MEXISTS` on string。
- `BF.INSERT` on string。
- `BF.CARD` on string。
- `BF.SCANDUMP` on string。
- `BF.LOADCHUNK` on string，且必须验证不会删除 string。

### 8. 参数矩阵覆盖不足

缺少：

- `BF.RESERVE EXPANSION 0` 应报错。
- `BF.RESERVE NONSCALING EXPANSION 2` 应报错。
- 重复 option：`EXPANSION 2 EXPANSION 4`、`NONSCALING NONSCALING`。
- `BF.INSERT ITEMS` 后还有额外 option 的行为。
- `BF.INSERT NOCREATE CAPACITY/ERROR` 与官方文档/实现的最终兼容策略。
- capacity 接近 `LLONG_MAX`。
- error rate NaN/Inf 字符串。

### 9. 边界数据覆盖不足

已有：

- empty string。
- 10000 字节长字符串。

缺少：

- binary item，包含 `\0`、`\r\n`、高位字节。
- 多个不同 binary item 的 round-trip。
- 大于 2GB item 的拒绝或定义行为。
- 非 UTF-8 payload。

### 10. 多层行为覆盖不足

已有：

- 小 capacity 下扩到多层。
- 所有插入项无 false negative。

缺少：

- `EXPANSION 1` 大量 layer。
- `EXPANSION 4/8`。
- 扩容到 fp rate 下限附近。
- 多层 `BF.INFO Capacity/Size/Filters/Items/Expansion` 精确值。
- 新 layer hit、旧 layer hit、miss 的性能基准。

### 11. 单元测试没有在当前环境运行

源文件存在：

- `modules/gemini-bloom/tests/bloom_filter_test.cc`
- `modules/gemini-bloom/tests/sb_chain_test.cc`
- `modules/gemini-bloom/tests/bloom_rdb_test.cc`

但容器内 `GTest_FOUND` 为 false，CMake 不生成这些 targets。

建议：

- CI 中 `ENABLE_UNIT_TESTS=ON` 时必须找不到 GTest 就 fail。
- 或把 GTest 作为可用的容器依赖。

### 12. 缺少 sanitizer/fuzz/compat 测试

建议新增：

- ASAN/UBSAN：重点覆盖 LOADCHUNK/RDB 损坏输入和 C++ object lifetime。
- libFuzzer/AFL：针对 `DeserializeHeader`、`RdbLoadBloom` mock stream、command parser。
- Differential test：同一命令 trace 同时打到 RedisBloom 和 gemini-bloom，比对 reply、RDB、SCANDUMP、INFO。

## 覆盖率结论

当前测试能证明“常规 RESP2 BF 命令 happy path 基本可用”，但不能证明：

- RedisBloom wire compatibility。
- RDB/AOF corruption safety。
- LOADCHUNK destructive safety。
- RESP3 compatibility。
- 配置兼容。
- 官方 iterator 协议正确。
- 单元级算法边界正确。

在修复 `SCANDUMP/LOADCHUNK` 和反序列化校验前，不建议把当前测试结果解读为 RedisBloom 兼容。
