# gemini-bloom 设计文档

## 1. 概述

gemini-bloom 是一个 C++20 实现的 Redis Module，提供 Scalable Bloom Filter 数据结构。它实现了 RedisBloom 的 `BF.*` 命令接口，并通过复用 RedisBloom 的 RDB data type name `MBbloom--` 实现 RDB 级别的数据互通。

### 1.1 产品定位

gemini-bloom 是 Gemini 产品线的 Bloom Filter 组件，不是 RedisBloom 的 drop-in 替代品。

**是什么：**
- 一个独立设计的 Scalable Bloom Filter Redis Module
- 使用与 RedisBloom 兼容的 RDB 序列化格式，支持通过标准 Redis 机制进行数据迁移
- 实现主流 `BF.*` 命令，供内部产品使用

**不是什么：**
- 不是 RedisBloom 的源码移植（clean-room 实现，不引用 RSALv2/SSPL 许可的 RedisBloom 源码）
- 不是 RedisBloom 的完整协议兼容层（RESP3、BF.DEBUG 等不在范围内）
- 不承诺与 RedisBloom 的 SCANDUMP/LOADCHUNK 公共协议兼容

### 1.2 兼容性边界

| 层级 | 状态 | 说明 |
|---|---|---|
| RDB 序列化 | 兼容 | data type name `MBbloom--`，encver 2/4，与 RedisBloom v2.4.20 双向验证通过 |
| DUMP / RESTORE | 兼容 | Redis 原生序列化，基于 RDB object，双向通过 |
| MIGRATE | 兼容 | Redis 原生迁移命令，双向通过，保留 TTL |
| psync / fullsync replication | 兼容 | RDB snapshot 传输，双向通过 |
| RDB-preamble AOF | 兼容 | Redis 6/7 默认模式，AOF 文件包含 RDB 数据，双向通过 |
| BF.SCANDUMP / BF.LOADCHUNK | 不兼容 | gemini 使用私有 layer-index cursor 协议，不对客户开放 |
| command-AOF rewrite | 不兼容 | 依赖私有 LOADCHUNK 协议，不对客户暴露 |
| RESP3 | 不支持 | 所有命令使用 RESP2 返回格式 |
| BF.DEBUG | 不支持 | RedisBloom 诊断命令，不在实现范围 |

**支持的迁移方式（对客户承诺）：**
- 基于 psync 的主从复制（fullsync RDB snapshot）
- 基于 SCAN + DUMP/RESTORE 的逐 key 迁移
- RDB 文件直接加载
- MIGRATE 命令

### 1.3 验证基线

兼容性已通过 Redis 6.2.17 + RedisBloom v2.4.20 的对照矩阵验证，覆盖 9 个 corpus（empty、single-layer、multi-layer、fixed、expansion 1/2/4、binary items、long item、large 16MB）在 RDB/DUMP/RESTORE/MIGRATE/RDB-preamble AOF/fullsync replication 路径上的双向数据完整性。该结论不能外推到其他 RedisBloom 版本或 Redis 8 内置 Bloom。

---

## 2. 算法设计

### 2.1 Scalable Bloom Filter

基于 Almeida, Baquero, Preguica & Hutchison (2007) 的论文 "Scalable Bloom Filters"。

核心思想：当一个 Bloom Filter 层达到容量上限时，自动创建新层。新层的容量按 expansion factor 倍增，误判率按 tightening ratio 递减，保证整体误判率收敛到用户指定的目标。

```
用户指定: capacity=100, errorRate=0.01, expansion=2

Layer 0: capacity=100, fpRate=0.005  (0.01 * 0.5)
Layer 1: capacity=200, fpRate=0.0025 (0.005 * 0.5)
Layer 2: capacity=400, fpRate=0.00125
...
```

**查找**：从最新层向旧层逐层检查。任何一层返回 true 即判定"可能存在"，所有层都返回 false 才判定"确定不存在"。

**插入**：先在所有层上检查是否已存在。如果是新元素，插入到最顶层。如果顶层已满，先创建新层。

**扩容终止条件**：
- `FixedSize` 模式下不扩容，满了直接报错
- 下一层的误判率低于 `1e-15` 时停止扩容
- 下一层的容量溢出 `uint64_t` 时停止扩容

### 2.2 单层 Bloom Filter 参数

每层 Bloom Filter 的参数由学术公式推导：

```
bitsPerEntry = -log(fpRate) / (ln2)^2    // Mitzenmacher & Upfal (2005)
hashCount    = ceil(ln2 * bitsPerEntry)   // 最优哈希函数数量
totalBits    = capacity * bitsPerEntry    // 位数组大小
```

**位数组对齐**：
- 默认模式（无 NoRound flag）：totalBits 向上取整到 2 的幂次，使用 bit masking 代替取模
- NoRound 模式：totalBits 按 8 字节（64 bit）边界对齐，使用取模运算
- gemini 命令层创建的 filter 始终使用 `Use64Bit | NoRound` flags

### 2.3 哈希函数

使用 **MurmurHash2**（Austin Appleby，public domain），采用 Kirsch-Mitzenmacher 增强双重哈希方案：

```
32-bit mode:
  h1 = MurmurHash2(data, len, seed=0x9747b28c)
  h2 = MurmurHash2(data, len, seed=h1)

64-bit mode:
  h1 = MurmurHash64A(data, len, seed=0xc6a4a7935bd1e995)
  h2 = MurmurHash64A(data, len, seed=h1)

probe[i] = h1 + i * h2   (i = 0, 1, ..., hashCount-1)
```

gemini 命令层创建的 filter 始终使用 64-bit 模式。seed 值和双重哈希模式是 RDB 持久化格式的一部分 — 任何读写同一 RDB 格式的实现必须使用相同的 seed。

### 2.4 参考文献

- Bloom, B.H. (1970). "Space/Time Trade-offs in Hash Coding with Allowable Errors"
- Mitzenmacher, M. & Upfal, E. (2005). "Probability and Computing"
- Kirsch, A. & Mitzenmacher, M. (2006). "Less Hashing, Same Performance: Building a Better Bloom Filter" (ESA 2006)
- Almeida, P. et al. (2007). "Scalable Bloom Filters"

---

## 3. 命令接口

### 3.1 命令列表

| 命令 | flags | 说明 |
|---|---|---|
| `BF.RESERVE key error_rate capacity [EXPANSION n \| NONSCALING]` | write deny-oom | 创建 Bloom Filter |
| `BF.ADD key item` | write deny-oom | 添加元素，key 不存在则自动创建 |
| `BF.MADD key item [item ...]` | write deny-oom | 批量添加 |
| `BF.INSERT key [CAPACITY n] [ERROR rate] [EXPANSION n] [NOCREATE] [NONSCALING] ITEMS item [item ...]` | write deny-oom | 创建（可选）并批量添加 |
| `BF.EXISTS key item` | readonly | 检查元素是否可能存在 |
| `BF.MEXISTS key item [item ...]` | readonly | 批量检查 |
| `BF.INFO key [field]` | readonly | 返回 filter 元数据 |
| `BF.CARD key` | readonly | 返回已插入元素数 |
| `BF.SCANDUMP key cursor` | readonly fast | 增量序列化（内部使用） |
| `BF.LOADCHUNK key cursor data` | write deny-oom | 增量反序列化（内部使用） |

### 3.2 参数校验与资源限制

| 参数 | 有效范围 | 说明 |
|---|---|---|
| capacity | 1 .. 2^30 | `kMaxCapacity`，防止单次请求分配过大内存 |
| error_rate | (0.0, 1.0) | 必须是有限正数 |
| expansion | 0 .. 32768 | `kMaxExpansion`，0 表示 NONSCALING |
| per-layer data size | <= 2 GB | `BloomLayer::Create` 内部检查 |
| total data size (RDB/wire) | <= 4 GB | 反序列化路径检查 |
| max layers | <= 1024 | RDB/wire 反序列化检查 |

**Parser 行为：**
- 重复的 EXPANSION、NONSCALING、ERROR、CAPACITY 选项会被拒绝
- NONSCALING 和 EXPANSION > 0 互斥
- NOCREATE 和 CAPACITY/ERROR 互斥

### 3.3 MADD/INSERT 部分失败语义

当 fixed-size filter 在批量操作中间满了：
- 在第一个 full error 处停止处理后续元素
- 使用 postponed array length，返回数组只包含已处理的元素（成功的 + 第一个错误）
- 已成功插入的元素保留，触发 replication
- 与 RedisBloom v2.4.20 行为一致

### 3.4 与 RedisBloom v2.4.20 的命令语义差异

| 差异点 | gemini | RedisBloom v2.4.20 | 影响 |
|---|---|---|---|
| `BF.INFO key FIELD` 返回形状 | 标量 | singleton array `[value]` | 客户端解析差异 |
| `BF.INFO Size` 数值 | 含 struct + layer slots | 只含 chain 内部统计 | 监控数值差异 |
| `BF.INSERT EXPANSION 0` | 映射为 NONSCALING | 返回 ERR | gemini 更宽松 |
| `BF.INSERT NOCREATE + CAPACITY` | ERR NOCREATE cannot be used with CAPACITY or ERROR | ERR not found | 错误消息不同 |
| `BF.RESERVE` 未知 option | ERR unrecognized option | 静默接受 | gemini 更严格 |
| `BF.INFO` missing key | ERR key does not exist | ERR not found | 错误消息不同 |
| `BF.DEBUG` | 不支持 | 返回每层诊断信息 | 缺少诊断命令 |
| Module name | GeminiBloom (ver=1) | bf (ver=20420) | MODULE LIST 差异 |
| BF.INFO / BF.CARD flags | readonly | readonly fast | 性能分类标签差异 |

---

## 4. 架构设计

### 4.1 源码结构

```
modules/gemini-bloom/
  src/
    redis_bloom_module.cc   # 模块入口：RedisModule_OnLoad
    bloom_commands.cc/h     # BF.* 命令处理函数，Redis Module API 交互
    bloom_rdb.cc/h          # RDB/wire 序列化反序列化，Module type callbacks
    bloom_config.cc/h       # module load args 解析
    bloom_filter.cc/h       # 单层 BloomLayer：创建、insert、test、参数计算
    sb_chain.cc/h           # ScalingBloomFilter：多层管理、自动扩容
    murmur2.cc/h            # MurmurHash2 / MurmurHash64A
    rm_alloc.h              # 内存分配抽象（RedisModule_Alloc 或 malloc）
  tests/
    bloom_filter_test.cc    # BloomLayer 单元测试 + hash golden vectors
    sb_chain_test.cc        # ScalingBloomFilter 单元测试
    bloom_rdb_test.cc       # RDB/wire 序列化 round-trip + 恶意 metadata 测试
    tcl/
      bloom_test.tcl        # 集成测试（启动 redis-server，完整命令覆盖）
```

### 4.2 层次划分

```
┌─────────────────────────────────────────────────────┐
│  Redis Module API Layer                             │
│  redis_bloom_module.cc  bloom_commands.cc            │
│  bloom_config.cc                                     │
├─────────────────────────────────────────────────────┤
│  Serialization Layer                                │
│  bloom_rdb.cc                                        │
│  RdbWriter / RdbReader / ValidateLayerFields         │
│  SerializeHeader / DeserializeHeader                 │
├─────────────────────────────────────────────────────┤
│  Core Data Structure Layer                          │
│  sb_chain.cc  (ScalingBloomFilter)                   │
│  bloom_filter.cc  (BloomLayer)                       │
│  murmur2.cc  (Hash functions)                        │
├─────────────────────────────────────────────────────┤
│  Memory Abstraction                                 │
│  rm_alloc.h  (RMAlloc/RMFree)                        │
└─────────────────────────────────────────────────────┘
```

**关键设计原则**：
- Core 层（bloom_filter、sb_chain、murmur2）不依赖 `redismodule.h`，可独立编译和测试
- 通过 `rm_alloc.h` 的 `REDIS_BLOOM_TESTING` 宏在测试环境中切换为标准 malloc，避免 GNU ld 链接问题
- 序列化逻辑集中在 `bloom_rdb.cc`，使 Redis Module API 依赖不扩散到 core 层

### 4.3 核心类型

**`BloomLayer`** — 单层 Bloom Filter，RAII 生命周期管理：
- 持有 bit array（`uint8_t*`），析构时自动释放
- 支持 move 语义，禁止拷贝
- `Create()` 根据 capacity + fpRate 计算最优参数并分配 bit array
- `Insert()` / `Test()` 使用 Kirsch-Mitzenmacher 双重哈希
- `WriteTo()` / `ReadFrom()` 处理 RDB 序列化

**`ScalingBloomFilter`** — 多层管理器：
- 持有 `FilterLayer*` 数组（placement new 管理生命周期）
- `Put()` 返回 `optional<bool>`：true=新插入，false=重复，nullopt=满
- `GrowIfNeeded()` 在顶层满时自动创建新层
- `AppendLayer()` 使用安全的 move + placement new 进行数组扩容（不用 realloc）

**`BloomFlags`** — 类型安全的 enum class bit field：
- `NoRound(1)`: 不将 totalBits 取整到 2 的幂
- `RawBits(2)`: 原始位模式（不通过 RDB/wire 校验接受）
- `Use64Bit(4)`: 使用 64-bit hash
- `FixedSize(8)`: 禁止自动扩容

### 4.4 内存管理

- 所有内存分配通过 `rm_alloc.h` 的 `RMAlloc` / `RMCalloc` / `RMFree`
- 生产环境映射到 `RedisModule_Alloc` / `RedisModule_Calloc` / `RedisModule_Free`
- 测试环境（`REDIS_BLOOM_TESTING` 宏）映射到标准 `malloc` / `calloc` / `free`
- `ScalingBloomFilter` 本身通过 `RMAlloc(sizeof(...))` + placement new 创建，析构 + `RMFree` 释放
- `BloomLayer` 的 bit array 通过 `RMCalloc` 分配（零初始化）

---

## 5. 持久化设计

### 5.1 RDB 格式

Data type name: `MBbloom--`（与 RedisBloom 相同，用于 RDB 互通）

Encoding version history:
- `encver 2`: 新增 BloomFlags 字段
- `encver 4`: 新增 expansion factor 字段（当前版本）

**RDB 字段序列：**

```
Filter level:
  uint64  totalItems
  uint64  numLayers
  uint64  flags          (encver >= 2, otherwise default Use64Bit|NoRound)
  uint64  expansionFactor (encver >= 4, otherwise default 2)

Per layer (repeated numLayers times):
  uint64  capacity
  double  fpRate
  uint64  hashCount
  double  bitsPerEntry
  uint64  totalBits
  uint64  log2Bits
  blob    bitArray       (length-prefixed)
  uint64  itemCount
```

### 5.2 RDB 反序列化校验

`ValidateLayerFields()` 是 RDB 和 wire 路径共用的统一校验函数：

| 检查项 | 拒绝条件 |
|---|---|
| capacity | == 0 |
| totalBits | == 0 |
| hashCount | == 0，或与 `ceil(ln2 * bitsPerEntry)` 不一致 |
| fpRate | 非有限、<= 0、>= 1.0 |
| bitsPerEntry | 非有限、<= 0、> 1000 |
| dataSize | != `(totalBits + 7) / 8` |
| log2Bits | >= 64，或 > 0 时 `totalBits != 2^log2Bits` |
| totalBits | > `UINT64_MAX - 7` |

Filter-level 校验：
- `numLayers == 0` 或 `> 1024`：拒绝
- 未知 flags（非 NoRound/Use64Bit/FixedSize）：拒绝
- 非 FixedSize 且 `expansionFactor == 0`：拒绝
- `sum(itemCount) != totalItems`：拒绝
- 任何 `itemCount > capacity`：拒绝
- `sum(dataSize) > 4GB`：拒绝

**Narrowing cast 安全**：RDB 中所有字段以 `uint64` 存储。在 cast 为窄类型（`uint32_t hashCount`、`uint8_t log2Bits`、`unsigned flags/expansion`）之前，先检查原始值是否在目标类型范围内。

### 5.3 AOF Rewrite

`AofRewriteBloom()` 输出 `BF.LOADCHUNK` 命令序列，使用 gemini 私有 cursor 协议（layer-index）。这些命令只能被 gemini 自身回放，不能被 RedisBloom 回放。

由于 Redis 6/7 默认启用 `aof-use-rdb-preamble yes`，实际 AOF 文件中的 bloom 数据以 RDB 格式存储，不包含 `BF.LOADCHUNK`。只有关闭 RDB preamble 时才会触发 command-AOF rewrite。

### 5.4 SCANDUMP / LOADCHUNK（内部协议）

gemini 使用 layer-index cursor 协议，不对客户开放：

```
SCANDUMP key 0     → [1, header_blob]
SCANDUMP key 1     → [2, layer0_full_bits]
SCANDUMP key 2     → [3, layer1_full_bits]
...
SCANDUMP key N+1   → [0, ""]    (end)
```

`LOADCHUNK cursor=1` 反序列化 header 并创建 filter shell（bit arrays 为零）。后续 cursor 逐层填充 bit array。

**安全行为**：
- `cursor=1` 在 key 已存在且是 Bloom 类型时返回 `ERR received bad data`，不覆盖旧 key
- `cursor=1` 在 key 是非 Bloom 类型时返回 `WRONGTYPE`
- 数据 chunk 的长度必须精确匹配该层的 `dataSize`

---

## 6. 安全设计

### 6.1 资源限制

| 限制 | 值 | 检查位置 |
|---|---|---|
| 最大 capacity | 2^30 (~10 亿) | 命令层、config 层 |
| 最大 expansion | 32768 | 命令层、config 层 |
| 最大 layers | 1024 | RDB/wire 反序列化 |
| 单层最大 data size | 2 GB | `BloomLayer::Create()` |
| 总 data size | 4 GB | RDB/wire 反序列化 |
| bitsPerEntry 上界 | 1000 | `ValidateLayerFields()` |

### 6.2 非信任输入防御

RDB 文件、LOADCHUNK payload 均视为非信任输入：
- 所有字段在使用前校验（`ValidateLayerFields()`）
- 窄化 cast 前做 range check
- 加法前做 overflow guard
- 分配前做 data size 上限检查
- blob 长度与计算出的 dataSize 严格匹配

### 6.3 命令层防御

- 所有 write 命令标记 `deny-oom`，Redis OOM 状态下自动拒绝
- 参数校验在任何 key 操作之前完成
- 类型检查使用 `WRONGTYPE` 标准错误
- LOADCHUNK 不覆盖已有 Bloom key

---

## 7. 测试设计

### 7.1 测试体系总览

| 测试层 | 框架 | 用例数 | 定位 |
|---|---|---|---|
| BloomLayer 单元测试 | GTest (`bloom_filter_test`) | 27 | 单层 Bloom Filter 核心逻辑、hash 函数、flags |
| ScalingBloomFilter 单元测试 | GTest (`sb_chain_test`) | 16 | 多层管理、扩容、固定大小、极端参数 |
| RDB/wire 序列化测试 | GTest (`bloom_rdb_test`) | 62 | 序列化 round-trip、恶意输入拒绝、encver 兼容 |
| 集成测试 | TCL (`bloom_test.tcl`) | 146 | 完整命令语义、Redis 持久化、模块配置 |

### 7.2 测试隔离策略

**GTest 与 Redis 解耦**：通过 `REDIS_BLOOM_TESTING` 编译宏，`rm_alloc.h` 将 `RMAlloc`/`RMFree` 映射为标准 `malloc`/`free`，使 core 层代码可以脱离 Redis server 独立编译运行。这同时避免了 GNU ld 要求所有 `extern` 符号可解析的跨平台链接问题。

**RDB Mock**：`bloom_rdb_test` 使用 `MockRdbStream`（`include/mock_redismodule_io.h`）模拟 Redis Module IO API。mock 将 `RedisModule_SaveUnsigned` / `RedisModule_LoadUnsigned` 等函数指针指向内存 buffer 实现，支持写入后 rewind 读回，完整覆盖序列化 round-trip 而无需启动 Redis。

**TCL 进程隔离**：每次运行 `bloom_test.tcl` 会在随机端口启动独立 redis-server 实例，加载编译好的 `redis_bloom.so`，测试完成后自动 SHUTDOWN。测试之间不共享 Redis 实例状态（AOF 测试会重启 server）。

### 7.3 GTest: BloomLayer 单元测试 (`bloom_filter_test.cc`)

#### 7.3.1 Hash 函数正确性

| 测试场景 | 验证逻辑 |
|---|---|
| MurmurHash2 确定性 | 同一输入 + 同一 seed 返回相同值；不同 seed 返回不同值 |
| MurmurHash64A 确定性 | 同上，64-bit 版本 |
| 空输入 hash | 空字符串 hash 结果确定且可重复 |
| Hash32Policy 一致性 | 同一输入的 primary/secondary 可重复；不同输入产生不同 primary |
| Hash64Policy 一致性 | 同上，64-bit 版本 |
| **Hash golden vectors** | MurmurHash2 和 MurmurHash64A 对 `""`、`"a"`、`"hello"`、`"hello\0world"` 四组输入验证精确的 h1/h2 值。Hash32Policy 和 Hash64Policy 同样验证精确的 primary/secondary。这些值是 RDB 格式的一部分 — 任何改动都意味着与已持久化数据不兼容 |

#### 7.3.2 BloomLayer 生命周期与功能

| 测试场景 | 验证逻辑 |
|---|---|
| Create + RAII | 创建后 hashCount > 0、totalBits > 0、dataSize > 0、bitArray 非空、bitsPerEntry > 0 |
| Move 构造 | move 后源对象 bitArray 为 null，目标对象 bitArray 非空 |
| Move 赋值 | 赋值后目标持有源的 capacity/totalBits/bitArray，源 bitArray 为 null，旧存储被释放 |
| Insert + Test | 插入返回 true（首次）、false（重复）；Test 对已插入元素返回 true |
| False positive rate | 插入 10000 个元素后，零 false negative；对 100000 个未插入元素测试 FP rate < 3% |
| Power-of-two bit ceil | 默认模式下 totalBits 是 2 的幂，log2Bits 正确 |
| NoRound 对齐 | NoRound 模式下 log2Bits == 0，totalBits 按 64 bit 对齐但不是 2 的幂 |
| 参数拒绝 | capacity=0、fpRate=0/1.0/负数/NaN/Inf 全部返回 nullopt |

#### 7.3.3 BloomFlags

| 测试场景 | 验证逻辑 |
|---|---|
| 位运算 | `\|` 和 `&` 运算符、HasFlag、ToUnderlying 正确 |
| ValidateFlags 接受 | None、NoRound、Use64Bit、FixedSize 及其组合全部接受 |
| ValidateFlags 拒绝 | 未知 bit（0x80、0x10、0xFF）全部拒绝 |
| RawBits 拒绝 | RawBits 单独或与 Use64Bit 组合均被 ValidateFlags 拒绝 |
| RawBits 行为证明 | RawBits 创建的 layer hashCount=0，Test() 对任何输入返回 true（证明不应接受） |
| 资源限制常量 | kMaxCapacity == 2^30，kMaxExpansion == 32768 |

#### 7.3.4 辅助函数

| 测试场景 | 验证逻辑 |
|---|---|
| ResolveBit | 线性 bit index 正确映射到 (byteOffset, mask) little-endian 布局 |
| ProbePosition | power-of-two 使用 mask，非 power-of-two 使用 modulo |

### 7.4 GTest: ScalingBloomFilter 单元测试 (`sb_chain_test.cc`)

| 测试场景 | 验证逻辑 |
|---|---|
| 构造与析构 | 创建后 IsValid==true、NumLayers==1、TotalItems==0、ExpansionFactor 正确 |
| Put + Contains | 首次插入返回 true，重复返回 false，Contains 确认存在，TotalItems==1 |
| 32-bit hash | 不设 Use64Bit flag 时使用 Hash32Policy |
| 零 false negative | 插入 5000 元素后全部可查 |
| 自动扩容 | 插入超过初始 capacity 后 NumLayers > 1，所有元素仍可查 |
| Fixed-size 溢出拒绝 | FixedSize 模式下超过 capacity 返回 nullopt，NumLayers 保持 1 |
| Optional 返回语义 | true=新插入、false=重复、nullopt=满，三种状态逻辑正确 |
| 重复不消耗 capacity | FixedSize 模式下重复插入不增加 itemCount |
| TotalCapacity | 返回所有层 capacity 之和 |
| BytesUsed | 返回值大于 sizeof(ScalingBloomFilter) |
| Move 赋值 | 所有权正确转移，源对象清空 |
| Expansion=1 层大小 | EXPANSION 1 下每层 capacity 相同（不倍增） |
| FromRdbShell + SetLayer | 通过 shell 重建多层 filter 后所有元素可查（RDB 反序列化路径回归） |
| AppendLayer 安全搬迁 | 插入 500 元素触发多次内部数组扩容，所有元素仍可查（不使用 realloc） |
| 极端参数拒绝 | capacity=UINT64_MAX + fpRate=1e-300 被拒绝而非触发巨大分配 |
| Span 接口 | std::span\<const std::byte\> 接口正确传入和使用 |

### 7.5 GTest: RDB/wire 序列化测试 (`bloom_rdb_test.cc`)

#### 7.5.1 RDB round-trip

| 测试场景 | 验证逻辑 |
|---|---|
| 空 filter | 序列化 + 反序列化后 TotalItems/NumLayers/ExpansionFactor/Flags 完全一致 |
| 有数据 filter | 500 元素写入后 round-trip，零 false negative |
| 多层 filter | 自动扩容后 round-trip，层数和元素数一致，零 false negative |
| 元数据保留 | capacity、fpRate、hashCount、totalBits、dataSize、log2Bits、bitsPerEntry 逐字段比对 |
| bit array 精确匹配 | 序列化前后的 bit array 二进制完全一致（memcmp） |
| FixedSize 保留 | FixedSize flag 在 round-trip 后保留，层数保持 1 |
| encver 2 兼容 | 手工构造 encver 2 格式（无 expansion 字段）的 RDB 流，反序列化默认 expansion=2，数据可查 |
| 未知 encver | encver=99 返回 nullptr |
| 多种 expansion | expansion 1/2/4/8 各自 round-trip 通过 |
| 多种 FP rate | fpRate 0.1/0.01/0.001/0.0001 各自 round-trip 通过，fpRate 精确保留 |
| 重复 round-trip | 同一 filter 连续 50 次 round-trip，每次验证 TotalItems/NumLayers 和抽样查询 |

#### 7.5.2 恶意 RDB metadata 拒绝

每个测试构造一个包含特定非法字段的 RDB 流，验证 `RdbLoadBloom()` 返回 nullptr：

| 非法字段 | 测试名 |
|---|---|
| totalBits == 0 | `RejectsTotalBitsZero` |
| fpRate = NaN | `RejectsFpRateNaN` |
| fpRate = Inf | `RejectsFpRateInf` |
| fpRate = 0 | `RejectsFpRateZero` |
| fpRate = 1.0 | `RejectsFpRateOne` |
| fpRate < 0 | `RejectsFpRateNegative` |
| bitsPerEntry = NaN | `RejectsBitsPerEntryNaN` |
| bitsPerEntry = Inf | `RejectsBitsPerEntryInf` |
| bitsPerEntry = 0 | `RejectsBitsPerEntryZero` |
| bitsPerEntry < 0 | `RejectsBitsPerEntryNegative` |
| bitsPerEntry > 1000 | `RejectsBitsPerEntryTooLarge` |
| capacity == 0 | `RejectsCapacityZero` |
| hashCount == 0 | `RejectsHashCountZero` |
| hashCount != ceil(ln2 * bpe) | `RejectsHashCountInconsistentWithBitsPerEntry` |
| log2Bits >= 64 | `RejectsInvalidLog2Bits` |
| numLayers > 1024 | `RejectsExcessiveNumLayers` |
| truncated itemCount | `TruncatedItemCountReturnsNull` |
| blob 过短 | `RejectsShortLayerBlob` |
| blob 过长 | `RejectsLongLayerBlob` |
| scaling + expansion=0 | `RejectsScalingExpansionZero` |
| fixed + expansion=0 | `AcceptsFixedExpansionZero`（正向验证） |
| totalItems != sum(itemCount) | `RejectsTotalItemsMismatch` |
| itemCount > capacity | `RejectsItemCountExceedsCapacity` |
| 一致的 item counts | `AcceptsConsistentItemCounts`（正向验证） |
| 未知 flags bits | `RejectsUnknownFlags` |
| RawBits flag | `RejectsRawBitsFlag` |

#### 7.5.3 窄化 cast 绕过防御

RDB 中所有字段存储为 uint64。攻击者可以设置高位使 cast 后低位"合法"。以下测试验证高位值被拒绝：

| 测试名 | 攻击向量 |
|---|---|
| `RejectsHashCountHighBitBypass` | hashCount = 2^32 + expectedHash |
| `RejectsLog2BitsHighBitBypass` | log2Bits = 2^8 + validLog2 |
| `RejectsFlagsHighBitBypass` | flags = 2^32 + validFlags |

#### 7.5.4 Wire header (SCANDUMP/LOADCHUNK)

| 测试场景 | 验证逻辑 |
|---|---|
| 空 filter header round-trip | SerializeHeader + DeserializeHeader 后 metadata 一致 |
| 有数据 header round-trip | 多层 filter 的 header round-trip 一致 |
| 完整 SCANDUMP/LOADCHUNK 模拟 | serialize header + 保存每层 bit array → deserialize header + 恢复 bit array → 零 false negative |
| LayerMeta round-trip | ToWireMeta + FromWireMeta 逐字段比对 |
| 截断 header | 过短的 header 返回 nullptr |
| zero layers | numLayers=0 返回 nullptr |
| 过多 layers | numLayers=9999 返回 nullptr |
| 未初始化内存 SetLayer | calloc 的 slot 上 placement new 正确工作 |
| bad log2Bits header | log2Bits=65 返回 nullptr |
| totalItems 不一致 | header totalItems 篡改后返回 nullptr |
| itemCount > capacity | 篡改后返回 nullptr |
| 未知 flags | flags 高位 set 返回 nullptr |
| fpRate=0 | 返回 nullptr |
| bitsPerEntry=Inf/0 | 返回 nullptr |
| dataSize 不匹配 | 返回 nullptr |
| hashCount 不一致 | 返回 nullptr |
| scaling+expansion=0 | 返回 nullptr |
| fixed+expansion=0 | 接受（正向验证） |
| totalBits=0 | 返回 nullptr |
| 超大 totalDataSize | 5GB dataSize 返回 nullptr（4GB cap） |
| header size 线性增长 | ComputeHeaderSize == sizeof(header) + N * sizeof(meta) |

### 7.6 TCL 集成测试 (`bloom_test.tcl`)

集成测试在真实 redis-server 上运行，覆盖从命令解析到持久化的完整路径。

#### 7.6.1 BF.RESERVE (10 tests)

| 场景 | 验证逻辑 |
|---|---|
| 正常创建 | 返回 OK |
| key 已存在 | 返回 ERR item exists |
| error rate = 0 / 1.0 / 负数 | 返回参数错误 |
| capacity = 0 / 负数 | 返回参数错误 |
| 带 EXPANSION | 创建成功 |
| 带 NONSCALING | 创建成功 |
| arity 不足 | 返回 wrong number of arguments |

#### 7.6.2 BF.ADD / BF.EXISTS (8 tests)

| 场景 | 验证逻辑 |
|---|---|
| 新元素 | ADD 返回 1 |
| 重复元素 | ADD 返回 0 |
| 第二个不同元素 | ADD 返回 1 |
| key 不存在时 ADD | 自动创建 filter 并返回 1 |
| ADD arity 错误 | 返回 wrong number of arguments |
| EXISTS 已存在 | 返回 1 |
| EXISTS 不存在 | 返回 0 |
| EXISTS key 不存在 | 返回 0 |

#### 7.6.3 BF.MADD / BF.MEXISTS (4 tests)

| 场景 | 验证逻辑 |
|---|---|
| MADD 多个新元素 | 返回全 1 数组 |
| MADD 部分重复 | 返回混合 0/1 数组 |
| MEXISTS 混合查询 | 已插入返回 1，未插入返回 0 |
| MEXISTS key 不存在 | 返回全 0 数组 |

#### 7.6.4 BF.INSERT (7 tests)

| 场景 | 验证逻辑 |
|---|---|
| 基本 ITEMS | 返回全 1 数组 |
| 自定义 ERROR + CAPACITY | 创建并返回成功 |
| 带 EXPANSION | 创建并返回成功 |
| 重复 item | 返回 0 |
| NOCREATE key 不存在 | 返回 ERR |
| NOCREATE key 存在 | 正常插入 |
| 缺少 ITEMS 关键字 | 返回 ERR |

#### 7.6.5 BF.INFO (9 tests)

| 场景 | 验证逻辑 |
|---|---|
| 完整 info | 返回 10 元素数组（5 对 key-value） |
| Capacity 字段正确 | 与 RESERVE 指定值一致 |
| Number of filters >= 1 | 至少 1 层 |
| Expansion rate 正确 | 与 RESERVE 指定值一致 |
| 单字段 Capacity | 返回标量（gemini 行为，区别于 RedisBloom 的 singleton array） |
| 单字段 Items | 返回 0（空 filter） |
| key 不存在 | 返回 ERR |
| 未知字段 | 返回 ERR |
| NONSCALING Expansion | 返回 nil |

#### 7.6.6 BF.CARD (2 tests)

| 场景 | 验证逻辑 |
|---|---|
| 正常计数 | 插入 3 个不同 + 1 个重复，返回 3 |
| key 不存在 | 返回 0 |

#### 7.6.7 NONSCALING 行为 (2 tests)

| 场景 | 验证逻辑 |
|---|---|
| 超过 capacity 被拒绝 | 循环插入直到报错 |
| 保持单层 | Number of filters == 1 |

#### 7.6.8 自动扩容 (2 tests)

| 场景 | 验证逻辑 |
|---|---|
| 超过 capacity 自动扩层 | 插入 50 元素到 capacity=10 的 filter，层数 > 1 |
| 扩容后零 false negative | 所有 50 个元素 EXISTS == 1 |

#### 7.6.9 BF.SCANDUMP / BF.LOADCHUNK round-trip (4 tests)

| 场景 | 验证逻辑 |
|---|---|
| 基本 round-trip | dump src → load dst，CARD 一致，所有 200 个 item EXISTS == 1 |
| 返回 iterator 直接传入 | 使用 SCANDUMP 返回的 cursor 值传给 LOADCHUNK（不自行计算） |
| 二进制 chunk 保留 | 含 NUL、CRLF、特殊字符的 item 在 dump/load 后仍可查 |
| cursor 协议语义 | 验证 layer-index cursor 的递增逻辑和终止条件 |

#### 7.6.10 WRONGTYPE 错误 (8 tests)

对 string 类型的 key 执行每个 BF 命令，全部返回 WRONGTYPE。特别验证 MEXISTS 和 MADD 返回 top-level error 而非 per-element error 数组。

#### 7.6.11 EXPANSION 边界 (4 tests)

| 场景 | 验证逻辑 |
|---|---|
| EXPANSION 0 → NONSCALING | BF.INFO Expansion 返回 nil |
| NONSCALING + EXPANSION 互斥 | 返回 ERR |
| BF.INSERT EXPANSION 0 → NONSCALING | BF.INFO Expansion 返回 nil |
| BF.INSERT NONSCALING + EXPANSION 互斥 | 返回 ERR |

#### 7.6.12 LOADCHUNK 安全 (7 tests)

| 场景 | 验证逻辑 |
|---|---|
| 对 string key LOADCHUNK | 返回 WRONGTYPE，不删除原 key |
| 保留原 key 数据 | GET 仍返回原值 |
| 对已有 bloom key 送 malformed header | 返回 ERR，不破坏已有数据 |
| header 过短 | 返回 ERR corrupted |
| header zero layers | 返回 ERR corrupted |
| cursor=0 | 返回 ERR |
| 负数 cursor | 返回 ERR |
| key 不存在时 data chunk | 返回 ERR |
| data chunk 长度不匹配 | 返回 ERR |
| **对已有 Bloom key LOADCHUNK cursor=1** | 返回 ERR received bad data，不覆盖旧 key，旧数据保留 |

#### 7.6.13 参数校验 (16 tests)

| 场景 | 验证逻辑 |
|---|---|
| RESERVE EXPANSION 负数 | 返回 ERR |
| RESERVE 未知 option | 返回 ERR unrecognized |
| INSERT NONSCALING 满后拒绝 | 返回 ERR non scaling filter is full |
| NOCREATE + CAPACITY | 返回 ERR 互斥 |
| NOCREATE + ERROR | 返回 ERR 互斥 |
| ITEMS 后无元素 | 返回 wrong arity |
| EXPANSION 超过 UINT32 | 返回 ERR |
| RESERVE on string key | 返回 WRONGTYPE |
| RESERVE on existing bloom | 返回 ERR item exists |
| INSERT ERROR/CAPACITY/EXPANSION 缺值 | 返回 wrong arity |
| INSERT ERROR/CAPACITY non-numeric | 返回 ERR |
| INSERT ERROR 超范围 | 返回 ERR |
| INSERT 未知 option | 返回 ERR |
| RESERVE/INSERT duplicate EXPANSION | 返回 ERR duplicate |
| RESERVE/INSERT duplicate NONSCALING | 返回 ERR duplicate |
| INSERT duplicate ERROR/CAPACITY | 返回 ERR duplicate |

#### 7.6.14 资源限制 (6 tests)

| 场景 | 验证逻辑 |
|---|---|
| RESERVE capacity > 2^30 | 返回 ERR |
| RESERVE expansion > 32768 | 返回 ERR |
| INSERT capacity > 2^30 | 返回 ERR |
| INSERT expansion > 32768 | 返回 ERR |
| RESERVE capacity = 2^30 | 返回 OK（边界值通过） |
| RESERVE expansion = 32768 | 返回 OK（边界值通过） |

#### 7.6.15 MADD/INSERT 部分失败 (3 tests)

| 场景 | 验证逻辑 |
|---|---|
| MADD 全部失败 | 返回数组长度 1（单个 error，立即停止），匹配 RedisBloom 行为 |
| MADD 部分成功 | cap=3 已插 2 个，batch 3 个：返回 [1, ERR]（长度 2），CARD=3 |
| INSERT 全部失败 | 返回数组长度 1（单个 error），CARD 不变 |

#### 7.6.16 多层行为 (3 tests)

| 场景 | 验证逻辑 |
|---|---|
| EXPANSION 4 vs 1 | EXPANSION 4 产生的层数少于 EXPANSION 1 |
| expansion rate 报告 | BF.INFO 返回正确的 expansion rate |
| 多层零 false negative | 两种 expansion 下所有 100 个元素 EXISTS == 1 |

#### 7.6.17 边界值 item (6 tests)

| 场景 | 验证逻辑 |
|---|---|
| 空字符串 | ADD 返回 1，EXISTS 返回 1 |
| 10000 字节长字符串 | ADD 返回 1，EXISTS 返回 1 |
| 含 NUL 的二进制 | ADD 返回 1，EXISTS 返回 1 |

#### 7.6.18 持久化 (2 tests)

| 场景 | 验证逻辑 |
|---|---|
| RDB BGSAVE + restart | 插入 3 元素 → BGSAVE → SHUTDOWN SAVE → 重启 → 3 元素 EXISTS == 1，CARD == 3 |
| AOF rewrite + restart | 启用 AOF → 插入 50 元素 → BGREWRITEAOF → SHUTDOWN NOSAVE → 重启 → 50 元素全部可查 |

#### 7.6.19 模块配置 (9 tests)

| 场景 | 验证逻辑 |
|---|---|
| 默认配置 | 自动创建 filter 的 Capacity=100、Expansion=2 |
| 覆盖配置 | `ERROR_RATE 0.02 INITIAL_SIZE 123 EXPANSION 3` 生效 |
| ERROR_RATE 缺值 | 加载失败 |
| ERROR_RATE 超范围 | 加载失败 |
| INITIAL_SIZE 缺值 | 加载失败 |
| INITIAL_SIZE = 0 | 加载失败 |
| EXPANSION 缺值 | 加载失败 |
| EXPANSION = 0 | 加载失败 |
| 未知参数 | 加载失败 |

#### 7.6.20 Command metadata (8 tests)

| 场景 | 验证逻辑 |
|---|---|
| COMMAND INFO flags | 每个命令的 write/readonly flag 与注册一致 |
| COMMAND GETKEYS | BF.ADD/SCANDUMP/RESERVE/LOADCHUNK/INFO 都正确报告 key 位置 |
| ACL DRYRUN | 在支持 ACL 的版本上 bloom 命令可正常鉴权 |

#### 7.6.21 已知兼容差异记录 (6 expected-fail tests)

这些测试标记为 `EXPECTED GAP`，验证当前已知的兼容差异仍然存在，不作为 CI 阻断项：

- 5 个 RESP3 差异（BF.ADD/EXISTS boolean、MADD/MEXISTS array boolean、BF.INFO map）
- 1 个 SCANDUMP byte-offset cursor 差异

### 7.7 构建与运行

```bash
# 构建
cmake -B build
cmake --build build -j$(nproc)

# GTest（编译 + 运行）
cmake --build build -j$(nproc) --target bloom_test

# TCL 集成测试
tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl ./build/redis_bloom.so
```

---

## 8. 配置

### 8.1 Module Load Args

通过 `--loadmodule redis_bloom.so [args]` 传递：

| 参数 | 默认值 | 有效范围 | 说明 |
|---|---|---|---|
| `ERROR_RATE` | 0.01 | (0.0, 1.0) | 默认误判率 |
| `INITIAL_SIZE` | 100 | 1 .. 2^30 | 默认初始容量 |
| `EXPANSION` | 2 | 1 .. 32768 | 默认扩展因子 |

参数名大小写不敏感。未知参数会导致模块加载失败。

### 8.2 与 RedisBloom 配置差异

| 差异 | gemini | RedisBloom v2.4.20 |
|---|---|---|
| `EXPANSION` 配置 | 支持（gemini 扩展） | 不支持（module load 失败） |
| `CF_MAX_EXPANSIONS` | 不支持（非 Bloom Filter） | 支持（Cuckoo Filter 参数） |
| 配置接口 | 仅 loadmodule args | loadmodule args（v2.4.x），后续版本有 RedisModule_RegisterConfig |
| `EXPANSION 0` 在配置中 | 拒绝 | N/A |
| `EXPANSION 0` 在命令中 | 映射为 NONSCALING | `BF.RESERVE` 接受，`BF.INSERT` 拒绝 |

---

## 9. 已知限制

1. **SCANDUMP/LOADCHUNK 不对外兼容**：使用私有 layer-index cursor 协议，不兼容 RedisBloom 的 byte-offset chunk 协议。客户不应直接使用这两个命令。

2. **command-AOF 不跨实现兼容**：关闭 `aof-use-rdb-preamble` 后的 AOF 文件无法在 RedisBloom 和 gemini 之间互相回放。生产环境应保持 Redis 默认的 RDB preamble 模式。

3. **live replication command stream 的 BF.CARD 差异**：在 `EXPANSION 1` 等高 false-positive 场景下，command replay 路径上 RedisBloom 和 gemini 的 `BF.CARD` 可能不同（但 membership 不受影响）。fullsync replication（RDB snapshot）无此问题。

4. **BF.INFO Size 统计口径**：gemini 的 `Size` 包含 ScalingBloomFilter struct 和预留 layer slots，数值大于 RedisBloom 的统计。不影响 membership 正确性。

5. **不支持删除**：Bloom Filter 的固有特性。需要删除功能应使用 Cuckoo Filter（gemini 目前未实现）。

6. **子 filter 数量影响查询性能**：`EXPANSION 1` 会产生较多 layer，查询需要逐层检查。建议使用 `EXPANSION 2` 或更大值。

7. **Redis 8 Bloom 共存未验证**：Redis 8 内置 Bloom 使用相同的 `BF.*` 命令名和 `MBbloom--` 类型名，在 Redis 8 环境中加载 gemini-bloom 可能产生命令或类型冲突。当前目标环境为 Redis 6.x / 7.x。
