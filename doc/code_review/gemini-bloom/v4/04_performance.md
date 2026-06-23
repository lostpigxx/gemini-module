# 04 - 性能与可扩展性问题

本文件关注 Redis 单线程阻塞、内存峰值、持久化成本和大数据迁移性能。

## PERF-01：`BF.SCANDUMP` 一次返回整层 bit array，缺少 16MB chunk 上限

**级别：P1**

### 证据

`CmdScandump()` 对每个 layer 直接返回整个 bit array：

位置：`modules/gemini-bloom/src/bloom_commands.cc:518-525`。

RedisBloom upstream 定义 `MAX_SCANDUMP_SIZE = 1024 * 1024 * 16`，并通过 `SBChain_GetEncodedChunk()` 分块。

### 影响

大 filter 的单个 layer 可能达到数十 MB、数百 MB 甚至更大：

- 单次 Redis 命令长时间阻塞 event loop。
- 客户端和网络一次承载巨大 bulk string。
- Redis output buffer 压力显著升高。

### 建议

改为 byte-offset cursor，并限制每次返回最大 chunk size。默认可对齐 RedisBloom 16MB，也可提供 module config。

## PERF-02：`BF.LOADCHUNK` 要求整层 chunk，导入大 filter 内存和网络峰值高

**级别：P1**

`CmdLoadchunk()` 要求 `dataLen == layer.bloom.GetDataSize()`：

位置：`modules/gemini-bloom/src/bloom_commands.cc:582-586`。

这导致导入端也必须一次接收完整层数据，无法流式恢复一个大层。

### 建议

支持任意 offset/length chunk。这样迁移工具可以稳定使用固定大小缓冲区，Redis 主线程每次命令处理时间也更可控。

## PERF-03：超大 capacity/error 参数缺少前置上限，可能先进入昂贵计算或巨大分配

**级别：P1/P2**

`BloomLayer::Create()` 会根据 `cap * bitsPerEntry` 推导 bit array 大小，并尝试分配：

位置：`modules/gemini-bloom/src/bloom_filter.cc:91-130`。

虽然有部分 overflow 检查，但没有业务上限。用户可以输入远大于实际可承载的 capacity，使模块反复走巨大分配失败路径。

### 建议

在命令 parser 层先按配置上限拒绝，不要把明显不合理的请求传入 allocator。

## PERF-04：EXPANSION=1 会制造较多 layer，查询成本线性增长

**级别：P2**

查找从最新 layer 向旧 layer 逐层检查：

位置：`modules/gemini-bloom/src/sb_chain.cc:94-100`。

`EXPANSION 1` 下每层容量不增长，达到相同 item 数需要更多 layer。虽然 FP rate 下限会最终阻止无限增长，但在此之前 `BF.EXISTS` miss 和旧层 hit 都会变慢。

### 建议

- 在文档中明确 `EXPANSION 1` 的查询成本。
- 增加 max runtime layers 或 warning。
- 建立 expansion=1/2/4 的 benchmark。

## PERF-05：RDB/LOADCHUNK header 可声明大量内存，缺少 total bytes cap

**级别：P1/P2**

wire header 当前只限制 layer 数量，不限制所有 layer `dataSize` 总和：

位置：`modules/gemini-bloom/src/bloom_rdb.cc:209-249`。

`DeserializeHeader()` 会按 header 为每层分配 zero-filled bit array，即使后续 data chunks 永远不会到达。

### 建议

在 header 阶段先累计 `sum(dataSize)`，超过限制直接拒绝。对 RDB 路径也做同样检查。

## PERF-06：AOF rewrite 对每层输出一个完整 bulk string

**级别：P2**

`AofRewriteBloom()` 输出 header 后，对每层输出完整 bit array：

位置：`modules/gemini-bloom/src/bloom_rdb.cc:286-291`。

这与当前私有 LOADCHUNK 协议绑定，会让 rewritten AOF 包含超大单条命令。

### 建议

在实现标准 byte-offset chunk 后，AOF rewrite 也使用同样 chunk size，避免单条 AOF command 过大。

## PERF-07：缺少性能基准，无法量化误判率、层数、AOF/RDB 成本

**级别：P2/P3**

现有测试验证正确性，但没有 benchmark。

建议至少覆盖：

- `BF.ADD` 单层/多层
- `BF.EXISTS` miss、hit latest layer、hit old layer
- `BF.MADD` batch size 1/10/1000
- `SCANDUMP` / `LOADCHUNK` 10MB/100MB/1GB 级别
- RDB save/load
- BGREWRITEAOF
- expansion=1/2/4/8

