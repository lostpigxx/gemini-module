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
- 不承诺与 RedisBloom 的 SCANDUMP/LOADCHUNK 协议兼容（与 RedisBloom 的兼容通过 RDB 层实现）

### 1.2 兼容性边界

| 层级 | 状态 | 说明 |
|---|---|---|
| RDB 序列化 | 兼容 | data type name `MBbloom--`，encver 2/4，与 RedisBloom v2.4.20 双向验证通过 |
| DUMP / RESTORE | 兼容 | Redis 原生序列化，基于 RDB object，双向通过 |
| MIGRATE | 兼容 | Redis 原生迁移命令，双向通过，保留 TTL |
| psync / fullsync replication | 兼容 | RDB snapshot 传输，双向通过 |
| RDB-preamble AOF | 兼容 | Redis 6/7 默认模式，AOF 文件包含 RDB 数据，双向通过 |
| BF.SCANDUMP / BF.LOADCHUNK | 不兼容 | gemini 使用私有 layer-index cursor 协议，与 RedisBloom 不互通（详见 §5.4） |
| command-AOF rewrite | 不兼容 | 依赖私有 LOADCHUNK 协议（详见 §5.3） |
| RESP3 | 不支持 | 所有命令使用 RESP2 返回格式 |
| BF.DEBUG | 不支持 | RedisBloom 诊断命令，不在实现范围 |

**支持的迁移方式（对客户承诺）：**
- 基于 psync 的主从复制（fullsync RDB snapshot）
- 基于 SCAN + DUMP/RESTORE 的逐 key 迁移
- RDB 文件直接加载
- MIGRATE 命令

### 1.3 验证基线

兼容性已通过 Redis 6.2.17 + RedisBloom v2.4.20 的对照矩阵验证，覆盖 9 个 corpus（empty、single-layer、multi-layer、fixed、expansion 1/2/4、binary items、long item、large 16MB）在 RDB/DUMP/RESTORE/MIGRATE/RDB-preamble AOF/fullsync replication 路径上的双向数据完整性。该结论不能外推到其他 RedisBloom 版本或 Redis 8 内置 Bloom。

**验证 corpus 与复现**：验证使用的 RDB fixture 文件和期望结果存储在 `tests/compat/redisbloom-2.4.20/` 目录下。目前为本地验证通过状态，尚未作为 CI 阻断项。后续计划将 corpus round-trip 纳入 CI gate，确保任何序列化变更都被自动检测。

---

## 2. 算法设计

gemini-bloom 和 RedisBloom 都实现了 Scalable Bloom Filter，算法源自同一篇论文，但在工程实现上有诸多差异。本章先介绍共同的算法原理，再详述 gemini-bloom 的具体实现选择及其与 RedisBloom 的区别。

### 2.1 Scalable Bloom Filter 原理

基于 Almeida, Baquero, Preguica & Hutchison (2007) 的论文 "Scalable Bloom Filters"。

**核心思想**：经典 Bloom Filter 需要预先知道元素数量上限。Scalable Bloom Filter 通过层叠多个子 filter 解决这个问题 — 当一个子 filter 达到容量上限时，在其上方创建新的子 filter。新层的容量按 expansion factor 倍增，误判率按 tightening ratio 递减，保证整体误判率收敛到用户指定的目标。

```
用户指定: capacity=100, errorRate=0.01, expansion=2

Layer 0: capacity=100, fpRate=0.005  (0.01 * 0.5)
Layer 1: capacity=200, fpRate=0.0025 (0.005 * 0.5)
Layer 2: capacity=400, fpRate=0.00125
...

整体误判率 = sum(各层 fpRate) 收敛于用户指定的 0.01
```

**查找**：从最新层向旧层逐层检查。任何一层返回 true 即判定"可能存在"，所有层都返回 false 才判定"确定不存在"。

**插入**：先在所有层上检查是否已存在（避免重复计数）。如果是新元素，插入到最顶层。如果顶层已满，先创建新层。

**扩容终止条件**：
- `FixedSize` 模式下不扩容，满了直接报错
- 下一层的误判率低于 `1e-15` 时停止扩容
- 下一层的容量溢出 `uint64_t` 时停止扩容

### 2.2 单层 Bloom Filter 参数计算

每层 Bloom Filter 的参数由经典公式推导（Mitzenmacher & Upfal, 2005）：

```
bitsPerEntry = -log(fpRate) / (ln2)^2     // 每元素所需位数
hashCount    = ceil(ln2 * bitsPerEntry)    // 最优哈希函数数量
totalBits    = capacity * bitsPerEntry     // 位数组大小

示例 (fpRate=0.01):
  bitsPerEntry ≈ 9.585
  hashCount    = ceil(0.693 * 9.585) = 7
  capacity=100 → totalBits ≈ 959 → 对齐后 1024 或 960
```

gemini-bloom 和 RedisBloom 使用相同的公式。参数计算后的差异在于位数组对齐方式（见 2.4 节）。

### 2.3 哈希函数

gemini-bloom 和 RedisBloom 都使用 **MurmurHash2**（Austin Appleby，public domain），采用 Kirsch-Mitzenmacher 增强双重哈希方案（"Less Hashing, Same Performance", ESA 2006）：

```
双重哈希: 计算两个独立 hash h1, h2，通过 h1 + i*h2 派生 k 个探测位置

32-bit mode:
  h1 = MurmurHash2(data, len, seed=0x9747b28c)
  h2 = MurmurHash2(data, len, seed=h1)

64-bit mode:
  h1 = MurmurHash64A(data, len, seed=0xc6a4a7935bd1e995)
  h2 = MurmurHash64A(data, len, seed=h1)

probe[i] = h1 + i * h2   (i = 0, 1, ..., hashCount-1)
```

**seed 值是 RDB 格式的一部分** — 它们决定了哪些 bit 被设置在持久化的位数组中。任何读写同一 RDB 格式的实现必须使用相同的 seed 和双重哈希模式，否则反序列化后的 filter 会出现 false negative。

gemini-bloom 的 seed 值和 `h2 = hash(data, seed=h1)` 模式与 RedisBloom 一致，这是 RDB 双向互通的基础。hash golden vector 测试固化了这些精确值。

### 2.4 gemini-bloom 与 RedisBloom 的设计差异

#### 2.4.1 实现语言与数据结构

| 维度 | gemini-bloom | RedisBloom |
|---|---|---|
| 语言 | C++20 | C |
| 多层容器 | `FilterLayer*` 动态数组 + placement new | `SBChain` 链表 + `SBLink` 节点 |
| 单层抽象 | `BloomLayer` 类 (RAII) | `struct bloom` (手动 malloc/free) |
| 内存管理 | RAII + move 语义，析构自动释放 | 手动 `RedisModule_Alloc`/`Free` |
| 扩容策略 | 动态数组倍增（初始 4 slots），move 迁移 | 链表 append，无搬迁 |
| 哈希接口 | `HashPair` + 编译期 policy 分发 | 宏 `BLOOM_CALLHASHES` 直接调用 |

**数据结构选择的影响**：RedisBloom 使用链表存储子 filter，每次扩容是 O(1) 的链表 append。gemini-bloom 使用动态数组，扩容时需要 move 所有已有层（O(n)），但查找时内存连续，cache 更友好。由于层数通常 < 20（EXPANSION 2 下），move 成本可忽略。

#### 2.4.2 位数组对齐

| 维度 | gemini-bloom | RedisBloom |
|---|---|---|
| 默认创建模式 | `Use64Bit \| NoRound` | 依赖 `BLOOM_OPT_NOROUND` 和 `BLOOM_OPT_FORCE64` flags |
| NoRound 对齐 | 按 8 字节（64 bit）边界对齐 | 按 8 字节（64 bit）边界对齐 |
| 默认取整 | NoRound 时不取整到 2 的幂 | 同上 |
| bit 寻址 | `UseBitMasking()` 判断使用 mask 还是 modulo | 类似逻辑 |

两者在 NoRound 模式下的位数组大小计算一致：先按 `capacity * bitsPerEntry` 计算原始位数，再按 64 bit 对齐。这是 RDB 兼容的关键 — 同一参数产生相同大小的位数组。

#### 2.4.3 Tightening Ratio

| 维度 | gemini-bloom | RedisBloom |
|---|---|---|
| 值 | `kTighteningRatio = 0.5` | `SB_TIGHTENING_RATIO = 0.5` |
| 首层 fpRate | `errorRate * 0.5` (scaling) 或 `errorRate` (fixed) | 同上 |
| 后续层 | 前一层 fpRate * 0.5 | 同上 |

两者完全一致。

#### 2.4.4 内存统计 (BF.INFO Size)

| 维度 | gemini-bloom | RedisBloom |
|---|---|---|
| 统计口径 | `sizeof(ScalingBloomFilter)` + `layerCapacity_ * sizeof(FilterLayer)` + 所有层 bit array | `sizeof(SBChain)` + 所有 `SBLink` + 所有 `struct bloom` + 所有层 bit array |
| 预分配 | 包含预留但未使用的 layer slots | 链表无预留 |
| 结果 | 数值通常大于 RedisBloom | 更紧凑 |

示例：capacity=100, fpRate=0.01 的单层空 filter：gemini 报 440 bytes，RedisBloom 报 240 bytes。差异来自 gemini 的 `layerCapacity_=4`（预分配 4 个 FilterLayer slots）和 C++ 对象头的大小。

#### 2.4.5 SCANDUMP / LOADCHUNK 协议

gemini 使用私有 layer-index cursor 协议，与 RedisBloom 的 byte-offset chunk 协议不互通。与 RedisBloom 的数据兼容通过 RDB 层实现，不依赖此协议。完整设计详见 §5.4。

```
RedisBloom v2.4.20 chunk 序列示例（EXPANSION 2, 40 items）:
  [(1, 179), (17, 16), (49, 32), (121, 72), (0, 0)]
  cursor 按 byte offset 递进: 1 → 1+16=17 → 17+32=49 → 49+72=121 → 0(结束)

gemini 同一 filter 的 chunk 序列:
  [(1, 179), (2, 128), (3, 128), (4, 128), (0, 0)]
  cursor 按 layer index 递进: 1 → 2 → 3 → 4 → 0(结束)
  每个 data chunk 是完整的一层 bit array
```

#### 2.4.6 命令 parser 严格性

| 维度 | gemini-bloom | RedisBloom v2.4.20 |
|---|---|---|
| 未知 option | 拒绝（ERR unrecognized option） | 静默接受 |
| 重复 option | 拒绝（ERR duplicate） | 最后一次赋值生效 |
| NOCREATE + CAPACITY | 前置拒绝（ERR NOCREATE cannot be used with CAPACITY or ERROR） | 后置拒绝（ERR not found） |
| BF.INSERT EXPANSION 0 | 映射为 NONSCALING | 拒绝（ERR Bad argument received） |
| BF.RESERVE EXPANSION 0 | 映射为 NONSCALING | 映射为 NONSCALING |

gemini 总体上比 RedisBloom 更严格。这是有意为之的设计选择 — 宁可在参数阶段就拒绝，也不让不明确的意图静默通过。

#### 2.4.7 LOADCHUNK 已有 key 行为

| 维度 | gemini-bloom | RedisBloom v2.4.20 |
|---|---|---|
| cursor=1 对空 key | 创建 shell，标记 Loading 状态 | 创建 shell |
| cursor=1 对已有 Bloom key | ERR received bad data，保留旧 key | ERR received bad data，保留旧 key |
| cursor=1 对 string key | WRONGTYPE | WRONGTYPE |
| cursor>1 对已完成的 Bloom key | ERR received bad data | 允许覆写 bit array |
| loading 中的 key 读写 | 返回 ERR filter is being loaded | 无此概念，可正常查询 |

gemini 在 cursor=1 行为上与 RedisBloom 对齐，但通过 loading 状态对 cursor>1 增加了完整性保护（详见 §5.4.3）。

#### 2.4.8 批量命令部分失败

| 维度 | gemini-bloom | RedisBloom v2.4.20 |
|---|---|---|
| MADD 部分失败数组长度 | 截断到第一个 error（postponed array length） | 截断到第一个 error |
| INSERT 部分失败数组长度 | 同上 | 同上 |
| 示例：cap=2, MADD a b c d | [1, 1, ERR]（长度 3） | [1, 1, ERR]（长度 3） |

gemini 在该行为上已与 RedisBloom v2.4.20 对齐。

#### 2.4.9 RDB 序列化格式

| 维度 | gemini-bloom | RedisBloom |
|---|---|---|
| data type name | `MBbloom--` | `MBbloom--` |
| 当前 encver | 4 | 4 |
| 字段序列 | totalItems, numLayers, flags, expansion, layers[] | 同上 |
| 每层字段 | capacity, fpRate, hashCount, bitsPerEntry, totalBits, log2Bits, bitArray(blob), itemCount | 同上 |
| encver 2 兼容 | 支持（无 expansion 字段时默认 2） | 支持 |
| hash seed | 32-bit: 0x9747b28c, 64-bit: 0xc6a4a7935bd1e995 | 同上 |

RDB 格式是两者兼容的核心。字段序列、编码版本和 hash seed 完全一致。这通过 Redis 6.2.17 + RedisBloom v2.4.20 的双向矩阵验证，覆盖了 RDB 文件、DUMP/RESTORE、MIGRATE、RDB-preamble AOF 和 fullsync replication。

#### 2.4.10 反序列化校验差异

gemini-bloom 在反序列化路径上比 RedisBloom 做了更多校验：

| 校验项 | gemini-bloom | RedisBloom v2.4.20 |
|---|---|---|
| hashCount 与 bitsPerEntry 一致性 | 校验 `hashCount == ceil(ln2 * bitsPerEntry)` | 不校验 |
| bitsPerEntry 上界 | 限制 <= 1000 | 不限制 |
| 未知 flags | 拒绝 | 不拒绝 |
| RawBits flag | 拒绝 | 接受 |
| itemCount > capacity | 拒绝 | 不校验 |
| sum(itemCount) == totalItems | 校验 | 不校验 |
| 总 data size | <= 4GB | 无限制 |
| 窄化 cast 范围检查 | uint64 → uint32/uint8 前检查 | 直接 cast |

这意味着 gemini 可以加载 RedisBloom 产生的合法 RDB，但某些 RedisBloom 接受的边缘 case（如 hashCount 不一致、极大 bitsPerEntry）gemini 会拒绝。在正常使用场景中，RedisBloom 产生的 RDB 数据都能通过 gemini 的校验。

### 2.5 参考文献

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
| `BF.SCANDUMP key cursor` | readonly fast | 增量导出（详见 §5.4） |
| `BF.LOADCHUNK key cursor data` | write deny-oom | 增量导入（详见 §5.4） |

### 3.2 参数校验与资源限制

| 参数 | 有效范围 | 说明 |
|---|---|---|
| capacity | 1 .. 2^30 | `kMaxCapacity`，防止单次请求分配过大内存 |
| error_rate | (0.0, 1.0) | 必须是有限正数 |
| expansion | 0 .. 32768 | `kMaxExpansion`，0 表示 NONSCALING |
| per-layer data size | <= 2 GB | `BloomLayer::Create` 内部检查 |
| total data size | <= 4 GB | runtime `AppendLayer()` + RDB/wire 反序列化 |
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
| `LOADCHUNK` cursor>1 对已有 key | 拒绝（loading 状态保护） | 允许覆写 bit array | gemini 更安全 |
| loading 中的 key 读写 | 返回 `ERR filter is being loaded` | 无此概念 | gemini 更严格 |

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
- `Loading(16)`: LOADCHUNK 正在加载中（runtime-only，不持久化，序列化时通过 `kPersistentFlagsMask` 剥离）

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

**`aof-use-rdb-preamble` 配置依赖**：

```
aof-use-rdb-preamble yes (Redis 6/7 默认)
  → AOF 中 bloom 数据以 RDB 格式存储，与 RedisBloom 双向兼容

aof-use-rdb-preamble no
  → AOF 中 bloom 数据以 BF.LOADCHUNK 命令序列存储
  → 使用 gemini 私有协议，与 RedisBloom 不兼容
```

模块当前不检测 `aof-use-rdb-preamble` 配置值。在非 RDB preamble 模式下，AOF 文件只能由 gemini 自身回放。生产环境**必须**保持 Redis 默认的 `aof-use-rdb-preamble yes`。后续可考虑在模块加载时检测该配置并在关闭时输出 warning。

**AOF rewrite 失败语义**：

当 `AofRewriteBloom()` 中 header buffer 分配失败时，模块调用 `RedisModule_LogIOError()` 记录 warning 并跳过该 key。这意味着在极端 OOM 场景下，生成的 AOF 文件可能缺少被跳过的 key。该行为依赖 Redis 对 `RedisModule_LogIOError()` 的处理策略 — Redis 可能 abort 整个 rewrite（保留旧 AOF），但模块层不做该假设。

**CI 覆盖**：TCL 集成测试覆盖了 `aof-use-rdb-preamble yes`（默认）模式下的 AOF 持久化 round-trip。非 RDB preamble 模式的 AOF rewrite 尚未纳入 CI 测试矩阵。

### 5.4 SCANDUMP / LOADCHUNK

#### 5.4.1 定位与职责分工

gemini-bloom 与 RedisBloom 的数据兼容通过 **RDB 层**实现，不依赖 SCANDUMP/LOADCHUNK。所有基于 RDB 的 Redis 原生机制（DUMP/RESTORE、MIGRATE、psync/fullsync、RDB 文件加载、RDB-preamble AOF）都走 `RdbSaveBloom`/`RdbLoadBloom`，与 RedisBloom 双向兼容。

SCANDUMP/LOADCHUNK 是 gemini 自有的用户侧导出/导入接口，用于：
- **AOF command rewrite**：Redis 的 AOF rewrite callback 产出的命令必须是已注册命令
- **应用层迁移**：在应用层做跨集群的 Bloom Filter 导出/导入、备份/恢复

这两个命令使用 gemini 私有协议，不需要也不打算与 RedisBloom 的 SCANDUMP/LOADCHUNK 互通。

#### 5.4.2 Cursor 协议

```
SCANDUMP key 0     → [1, header_blob]        # header（WireFilterHeader + WireLayerMeta[]）
SCANDUMP key 1     → [2, layer0_full_bits]    # 第 0 层完整 bit array
SCANDUMP key 2     → [3, layer1_full_bits]    # 第 1 层完整 bit array
...
SCANDUMP key N     → [N+1, layerN-1_bits]     # 最后一层
SCANDUMP key N+1   → [0, ""]                  # 结束标记
```

cursor 语义是 layer index，每个 data chunk 是一整层的 bit array。客户端使用返回的 iter 作为下一次 SCANDUMP/LOADCHUNK 的 cursor，直到 iter=0。

#### 5.4.3 Loading 状态保护

LOADCHUNK 通过 loading 状态保证数据完整性：

```
LOADCHUNK key 1 <header>
  → 反序列化 header，创建 filter shell（bit arrays 全零）
  → 标记 filter 为 Loading 状态
  → OK

LOADCHUNK key 2 <layer0_bits>
  → 填充第 0 层 bit array（仅 Loading 状态的 key 接受）
  → OK

LOADCHUNK key N+1 <layerN_bits>
  → 填充最后一层 bit array
  → 清除 Loading 标记，filter 进入正常状态
  → OK
```

**Loading 状态约束：**
- Loading 状态的 key：所有读写命令（BF.EXISTS/ADD/MADD/INSERT/INFO/CARD/SCANDUMP）返回 `ERR filter is being loaded`
- 已完成的 key：LOADCHUNK cursor>1 返回 `ERR received bad data`，不可被覆写
- Loading 标记是 runtime-only flag（`BloomFlags::Loading`），不会持久化到 RDB/wire 格式

这保证了已插入数据的 Bloom Filter 不会被 LOADCHUNK 覆写破坏（避免 false negative）。

#### 5.4.4 与 RedisBloom SCANDUMP/LOADCHUNK 的差异

| 维度 | gemini | RedisBloom v2.4.20 |
|---|---|---|
| cursor 语义 | layer index | byte offset 在拼接数据流中的位置 |
| 每个 chunk 大小 | 一整层 bit array | 最大 16MB，大层会被拆分 |
| chunk 可以跨层 | 不可以 | 可以 |
| header 格式 | gemini 私有二进制结构 | RedisBloom 私有二进制结构 |
| cursor>1 覆写已有 key | 拒绝（loading 状态保护） | 允许（直接 memcpy） |
| loading 中的 key 读操作 | 拒绝 | 无此概念，部分加载的 key 可查询 |
| 交叉兼容 | 不兼容 | 不兼容 |

功能上两者等价：都能完成"导出 → 导入"的完整流程。gemini 更严格 — 通过 loading 状态防止对已完成 key 的意外覆写。

#### 5.4.5 其他安全行为

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
| 总 data size | 4 GB | `AppendLayer()` runtime 检查 + RDB/wire 反序列化 |
| bitsPerEntry 上界 | 1000 | `BloomLayer::Create()` + `ValidateLayerFields()` |

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
- LOADCHUNK 通过 loading 状态保护已完成的 Bloom key 不被覆写（详见 §5.4.3）

---

## 7. 测试设计

### 7.1 测试体系总览

| 测试层 | 框架 | 用例数 | 定位 |
|---|---|---|---|
| BloomLayer 单元测试 | GTest (`bloom_filter_test`) | 28 | 单层 Bloom Filter 核心逻辑、hash 函数、flags |
| ScalingBloomFilter 单元测试 | GTest (`sb_chain_test`) | 21 | 多层管理、扩容、固定大小、loading 状态、极端参数 |
| RDB/wire 序列化测试 | GTest (`bloom_rdb_test`) | 65 | 序列化 round-trip、恶意输入拒绝、encver 兼容、loading flag 剥离 |
| 集成测试 | TCL (`bloom_test.tcl`) | 150 | 完整命令语义、Redis 持久化、loading 状态保护、模块配置 |

### 7.2 测试隔离策略

**GTest 与 Redis 解耦**：通过 `REDIS_BLOOM_TESTING` 编译宏，`rm_alloc.h` 将 `RMAlloc`/`RMFree` 映射为标准 `malloc`/`free`，使 core 层代码可以脱离 Redis server 独立编译运行。这同时避免了 GNU ld 要求所有 `extern` 符号可解析的跨平台链接问题（详见 `doc/cross_platform_linking.md`）。

**RDB Mock**：`bloom_rdb_test` 使用 `MockRdbStream`（`include/mock_redismodule_io.h`）模拟 Redis Module IO API。mock 将 `RedisModule_SaveUnsigned` / `RedisModule_LoadUnsigned` 等函数指针指向内存 buffer 实现，支持写入后 rewind 读回，完整覆盖序列化 round-trip 而无需启动 Redis。

**TCL 进程隔离**：每次运行 `bloom_test.tcl` 会在随机端口启动独立 redis-server 实例，加载编译好的 `redis_bloom.so`，测试完成后自动 SHUTDOWN。测试之间不共享 Redis 实例状态（AOF 测试会重启 server）。TCL 客户端实现了完整的 RESP2 协议解析（含 `redis_read_reply_nothrow` 变体用于读取包含 per-element error 的数组）。

### 7.3 GTest 测试场景

#### bloom_filter_test — 单层 Bloom Filter 核心

- **Hash 函数**：MurmurHash2/64A 确定性、空输入、Hash32Policy/Hash64Policy 一致性。关键：hash golden vector 测试对 `""`、`"a"`、`"hello"`、`"hello\0world"` 验证精确的 h1/h2 值，这些值是 RDB 格式的一部分，任何改动都意味着与已持久化数据不兼容。
- **BloomLayer 生命周期**：Create + RAII 资源释放、move 构造/赋值的所有权转移、析构后无泄漏。
- **功能正确性**：Insert 返回 true（首次）/false（重复）、Test 零 false negative、10 万次随机查询的 FP rate < 3%。
- **参数模式**：默认 power-of-two bit ceil 与 NoRound 64-bit 对齐两种模式的 totalBits/log2Bits 计算。
- **参数拒绝**：capacity=0、fpRate=0/1/负/NaN/Inf 全部返回 nullopt。
- **Flags 校验**：ValidateFlags 接受合法组合、拒绝未知 bit 和 RawBits；RawBits 行为证明（hashCount=0 导致 Test 恒真）。
- **辅助函数**：ResolveBit 的 byte/bit 映射、ProbePosition 的 mask vs modulo 分支。

#### sb_chain_test — ScalingBloomFilter 多层管理

- **基本功能**：构造/析构、Put/Contains、TotalCapacity/BytesUsed、move 赋值所有权转移。
- **扩容机制**：自动扩容后 NumLayers > 1 且所有元素仍可查；EXPANSION 1 vs 2 的层大小差异。
- **固定大小**：FixedSize 溢出返回 nullopt、重复不消耗 capacity、层数保持 1。
- **反序列化路径**：FromRdbShell + SetLayer 重建多层 filter 的正确性（回归历史 placement-new bug）。
- **安全边界**：AppendLayer 内部数组扩容使用 move 而非 realloc（回归历史 UB）；极端 capacity + fpRate 被拒绝而非触发巨大分配。

#### bloom_rdb_test — 序列化与反序列化

- **RDB round-trip**：空/有数据/多层/FixedSize filter 的 serialize→deserialize 后 metadata 完全一致、bit array 二进制精确匹配、零 false negative。覆盖 encver 2 向后兼容、多种 expansion/fpRate 组合、50 次连续 round-trip 压力测试。
- **恶意 RDB metadata 拒绝**（~25 个 case）：对 `ValidateLayerFields()` 覆盖的每个字段（totalBits=0、capacity=0、hashCount=0/不一致、fpRate NaN/Inf/0/1/负、bitsPerEntry NaN/Inf/0/负/>1000、log2Bits>=64、blob 长度不匹配、numLayers>1024、truncated stream、item count 不一致/超 capacity、未知 flags、RawBits flag）各构造一个非法 RDB 流，验证 `RdbLoadBloom()` 返回 nullptr。
- **窄化 cast 绕过防御**：构造 `hashCount = 2^32 + validValue`、`log2Bits = 2^8 + validValue`、`flags = 2^32 + validValue` 等高位攻击向量，验证 range check 在 cast 之前拦截。
- **Wire header**：SerializeHeader/DeserializeHeader round-trip、LayerMeta round-trip、完整 SCANDUMP/LOADCHUNK 模拟、各类非法 header 拒绝（截断、zero layers、过多 layers、字段篡改）、totalDataSize > 4GB 拒绝。

### 7.4 TCL 集成测试场景

集成测试在真实 redis-server 上运行，覆盖从命令解析到持久化再到 server restart 的完整路径。按场景分类：

- **命令基本功能**（~35 tests）：10 个 BF.* 命令的 happy path 和 arity 校验。BF.ADD 自动创建、BF.EXISTS 对不存在 key 返回 0、BF.MADD/MEXISTS 批量操作、BF.INSERT 各种 option 组合、BF.INFO 完整/单字段、BF.CARD 计数、BF.SCANDUMP/LOADCHUNK round-trip。
- **参数校验与错误路径**（~30 tests）：error rate / capacity / expansion 的边界值和非法值；NOCREATE + CAPACITY/ERROR 互斥；NONSCALING + EXPANSION 互斥；重复 option 拒绝（EXPANSION/NONSCALING/ERROR/CAPACITY）；未知 option 拒绝；arity 不足；缺值/非数值/超范围。
- **类型安全**（~10 tests）：所有 BF.* 命令对 string 类型 key 返回 WRONGTYPE；MADD/MEXISTS 返回 top-level error 而非 per-element error；BF.RESERVE 对已存在 bloom key 返回 `ERR item exists`、对 string key 返回 WRONGTYPE。
- **资源限制**（6 tests）：capacity > 2^30 和 expansion > 32768 被拒绝；边界值 2^30 和 32768 通过。
- **部分失败语义**（3 tests）：NONSCALING filter 的 MADD/INSERT 在第一个 full error 处截断数组（postponed array length），验证数组长度和 CARD 一致性，匹配 RedisBloom v2.4.20 行为。
- **LOADCHUNK 安全**（~10 tests）：WRONGTYPE 不删除原 key；malformed header 不破坏已有 bloom key；cursor=0/负数拒绝；data chunk key 不存在拒绝；data 长度不匹配拒绝；**对已有 Bloom key 的 cursor=1 返回 `ERR received bad data` 且旧数据保留**。
- **扩容与多层**（~5 tests）：自动扩容后零 false negative；EXPANSION 4 vs 1 的层数差异；expansion rate 报告正确。
- **边界值 item**（6 tests）：空字符串、10KB 长字符串、含 NUL 字节的二进制数据均可正确 ADD/EXISTS。
- **持久化**（2 tests）：BGSAVE + SHUTDOWN SAVE + restart 后数据完整；启用 AOF + BGREWRITEAOF + SHUTDOWN NOSAVE + restart 后数据完整。
- **模块配置**（9 tests）：默认值验证；`ERROR_RATE`/`INITIAL_SIZE`/`EXPANSION` 覆盖验证；缺值/超范围/零值/未知参数均导致模块加载失败。配置测试通过启动独立 redis-server 实例来验证模块加载行为。
- **Command metadata**（8 tests）：COMMAND INFO 验证所有命令的 write/readonly flag；COMMAND GETKEYS 验证 key 位置；ACL DRYRUN 验证鉴权。
- **已知兼容差异**（6 expected-fail tests）：标记为 `EXPECTED GAP`，记录 RESP3（5 个）和 SCANDUMP byte-offset cursor（1 个）差异，不作为 CI 阻断项。

### 7.5 构建与运行

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

1. **SCANDUMP/LOADCHUNK 不与 RedisBloom 互通**：使用 gemini 私有 layer-index cursor 协议和 header 格式，不兼容 RedisBloom 的 byte-offset chunk 协议。与 RedisBloom 的数据兼容通过 RDB 层实现（DUMP/RESTORE/MIGRATE/psync/RDB 文件），不依赖 SCANDUMP/LOADCHUNK（详见 §5.4）。

2. **command-AOF 不跨实现兼容**：关闭 `aof-use-rdb-preamble` 后的 AOF 文件无法在 RedisBloom 和 gemini 之间互相回放。生产环境**必须**保持 Redis 默认的 `aof-use-rdb-preamble yes`（详见 §5.3）。

3. **live replication command stream 的 BF.CARD 差异**：在 `EXPANSION 1` 等高 false-positive 场景下，command replay 路径上 RedisBloom 和 gemini 的 `BF.CARD` 可能不同（但 membership 不受影响）。fullsync replication（RDB snapshot）无此问题。

4. **BF.INFO Size 统计口径**：gemini 的 `Size` 包含 ScalingBloomFilter struct 和预留 layer slots，数值大于 RedisBloom 的统计。不影响 membership 正确性。

5. **不支持删除**：Bloom Filter 的固有特性。需要删除功能应使用 Cuckoo Filter（gemini 目前未实现）。

6. **子 filter 数量影响查询性能**：`EXPANSION 1` 会产生较多 layer，查询需要逐层检查。建议使用 `EXPANSION 2` 或更大值。

7. **command-AOF rewrite 分配失败时 key 会被跳过**：`AofRewriteBloom()` 中 header buffer 分配失败时，模块记录 warning 日志并跳过该 key（`return`）。Redis Module API 没有提供显式 abort AOF rewrite 的接口，`RedisModule_LogIOError` 是模块层能发出的最强信号。实际风险极低：默认 `aof-use-rdb-preamble yes` 时此代码路径不执行；即使执行，header 只有几百字节，能触发 OOM 意味着 Redis 整体已在极端内存压力下。

8. **RedisBloom / Redis 8 同实例共存冲突**：gemini-bloom 复用了 `BF.*` 命令名和 `MBbloom--` data type name。在同一 Redis 实例中，如果已加载 RedisBloom module，加载 gemini-bloom 将产生命令名和 data type name 的注册冲突（`RedisModule_CreateCommand` / `RedisModule_CreateDataType` 返回错误），模块加载会失败。同样，Redis 8 内置 Bloom 使用相同的命令名和类型名，在 Redis 8 环境中加载 gemini-bloom 也会产生冲突。gemini-bloom 与 RedisBloom / Redis 8 Bloom 是**互斥部署**关系，不能同时加载在同一 Redis 实例上。当前目标环境为 Redis 6.x / 7.x 且不加载 RedisBloom module。
