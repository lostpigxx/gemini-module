## 0. 审计基准

这次按你指定的口径：

```text
DESIGN.md
  │
  ├─ §1：单独审计文档自身
  │       只判断设计是否自洽、边界是否清楚、约束是否闭合
  │
  └─ §2：代码与文档交叉分析
          以 DESIGN.md 作为预期行为 / 设计承诺
          以代码作为实际行为
          代码偏离文档处记为冲突、相悖或未闭合
```

结论先给出：

**单看 `DESIGN.md`，主设计是合理的，但安全边界、资源约束闭包、AOF 失败语义、兼容性证明方式还有明显缺口。**

**代码与文档对照后，最严重的冲突是：文档把 `BF.SCANDUMP/BF.LOADCHUNK` 定义成内部协议、不对客户开放；代码却把它们注册成普通 Redis 命令，并允许 `LOADCHUNK` 创建 shell、覆写 layer bit array。**

---

## 1. 单从 DESIGN.md 看：设计本身是否合理

### 1.1 合理的主干

整体定位是合理的。文档没有把 gemini-bloom 定义成 RedisBloom drop-in replacement，而是定义成“实现主流 `BF.*` 命令、复用 RedisBloom RDB type name 做 RDB 级互通”的内部组件；同时明确列出 RESP3、BF.DEBUG、SCANDUMP/LOADCHUNK、command-AOF rewrite 等兼容边界。这个边界比泛泛说“兼容 RedisBloom”更可审计。

RDB 兼容路径也有明确锚点：data type name 使用 `MBbloom--`，encver 支持 2/4，字段序列、hash seed、NoRound 64-bit 对齐都被写入设计。这是做持久化兼容必须有的内容。

算法设计也基本合理。文档采用 Scalable Bloom Filter，用 tightening ratio 让各层误判率几何收敛；单层 Bloom 参数使用经典公式；hash 使用 MurmurHash2/64A + Kirsch-Mitzenmacher 双重哈希，并指出 seed 是 RDB 格式的一部分。

架构分层方向正确。文档把 Redis Module API、序列化、core data structure、memory abstraction 分开，并要求 core 层不依赖 `redismodule.h`，这有利于单元测试和 ABI 风险收敛。

安全设计里，RDB / LOADCHUNK payload 被视为非信任输入，并统一走 `ValidateLayerFields()`，同时对窄化 cast、blob 长度、data size、item count 做校验。这是必要且合理的。

```text
RDB / wire payload
        │
        ▼
field-level validation
        │
        ├─ reject malformed metadata
        ├─ reject unsafe cast / overflow
        └─ reject invalid blob size
        ▼
construct Bloom object
```

### 1.2 文档自身的主要不合理点

#### D1. “内部协议”边界没有闭合

文档一方面说 `BF.SCANDUMP/BF.LOADCHUNK` 不兼容 RedisBloom，并且“不对客户开放”；另一方面又在命令接口章节把 `BF.SCANDUMP` 和 `BF.LOADCHUNK` 列入 `BF.*` 命令列表。

从文档自身看，这里缺少一个关键设计决策：**到底由谁保证“不对客户开放”？**

```text
DESIGN.md says:
  SCANDUMP / LOADCHUNK = internal protocol
        │
        ▼
But command list includes:
  BF.SCANDUMP
  BF.LOADCHUNK
        │
        ▼
Missing:
  ACL rule? proxy block? renamed private command? admin-only command?
```

这个问题不只是表述问题。`LOADCHUNK` 是反序列化写入口，天然比普通 `BF.ADD` 危险。如果它是内部协议，文档必须明确 enforcement mechanism，而不是只写“客户不应直接使用”。

建议文档改成二选一：

```text
方案 A：公开但危险
  明确 ACL / admin-only / 不保证误用安全

方案 B：真正内部
  明确部署层屏蔽方式，或改成不暴露给客户的私有命令名
```

#### D2. “可创建对象域”和“可反序列化对象域”没有证明一致

文档规定命令参数 `error_rate` 有效范围是 `(0.0, 1.0)`，同时在 RDB 反序列化校验中规定 `bitsPerEntry > 1000` 要拒绝。

但文档里的 Bloom 参数公式是：

```text
bitsPerEntry = -log(fpRate) / (ln2)^2
```

当 `error_rate` 足够小时，`bitsPerEntry` 会超过 1000。按文档公式计算，阈值约是：

```text
error_rate < exp(-1000 * (ln2)^2) ≈ 2.2e-209
```

也就是说，单看文档，下面这个状态空间没有被排除：

```text
BF.RESERVE accepts error_rate in (0, 1)
        │
        │  e.g. 1e-300
        ▼
created filter has bitsPerEntry > 1000
        │
        ▼
RDB save
        │
        ▼
RDB load rejects bitsPerEntry > 1000
```

这不是实现细节，而是设计约束没有闭合。设计文档应该明确：

```text
allowed_create_state ⊆ allowed_deserialize_state
```

否则会出现“模块自己创建、自己持久化、自己 reload 失败”的风险。

#### D3. runtime 总 data size 与 RDB/wire 总 data size 的关系没有闭合

文档写了：

| 限制                         | 文档位置                        |
| -------------------------- | --------------------------- |
| per-layer data size <= 2GB | `BloomLayer::Create()` 内部检查 |
| total data size <= 4GB     | RDB/wire 反序列化路径检查           |
| max layers <= 1024         | RDB/wire 反序列化检查             |

这个写法的问题是：**total data size 只在 RDB/wire 反序列化上限里出现，没有说明 runtime 创建和扩容是否也必须小于 4GB。**

如果 runtime 可以通过自动扩容生成总 bit array 超过 4GB 的 filter，那么它持久化后是否还能被当前模块重新加载？文档没有回答。

```text
runtime auto-scale
        │
        ├─ each layer <= 2GB
        ├─ many layers possible
        ▼
total data size may exceed 4GB
        │
        ▼
RDB/wire load path rejects >4GB
```

这和 D2 是同类问题：创建域、运行域、持久化域需要一致。

#### D4. command-AOF rewrite 依赖 Redis 默认配置，但没有设计级 enforcement

文档说 AOF rewrite 会输出 gemini 私有 `BF.LOADCHUNK` 序列；同时说明 Redis 6/7 默认 `aof-use-rdb-preamble yes`，所以实际 AOF 通常走 RDB preamble，只有关闭 preamble 才会触发 command-AOF rewrite。

这本身可以接受，但文档没有说明模块如何防止生产误配置：

```text
aof-use-rdb-preamble yes
        │
        ▼
RDB-compatible AOF path

aof-use-rdb-preamble no
        │
        ▼
private LOADCHUNK command-AOF path
        │
        └─ not cross-compatible with RedisBloom
```

文档在限制章节建议“生产环境应保持 Redis 默认的 RDB preamble 模式”，但这只是运维建议，不是设计约束。

建议补充：

1. 模块启动时是否检测 `aof-use-rdb-preamble`。
2. 检测到关闭时是 warn、fail load，还是允许但降级。
3. command-AOF rewrite 的失败语义是什么。
4. 该模式是否进入 CI 测试矩阵。

#### D5. RDB 兼容性验证声明还不够可复现

文档声明已经用 Redis 6.2.17 + RedisBloom v2.4.20 做过 9 个 corpus 的双向矩阵验证，并且明确不能外推到其他 RedisBloom 版本或 Redis 8 Bloom。这个边界是好的。

但作为设计文档，兼容性结论还缺少可复现要素：

```text
compatibility claim
        │
        ├─ Redis version: yes
        ├─ RedisBloom version: yes
        ├─ corpus categories: yes
        ├─ exact fixture files: not specified
        ├─ CI gate: not specified
        └─ expected binary hashes: not specified
```

建议文档明确：

```text
compat/
  redisbloom-2.4.20/
    rdb/
    dump/
    migrate/
    fullsync/
    expected.json
```

并说明这些 fixtures 是否是 CI 阻断项。

#### D6. RedisBloom / Redis 8 共存策略写得不完整

文档提到 Redis 8 内置 Bloom 使用相同 `BF.*` 命令名和 `MBbloom--` 类型名，因此在 Redis 8 环境中加载 gemini-bloom 可能冲突。

但文档没有同样明确说明：在 Redis 6/7 中，如果已经加载 RedisBloom module，也会存在命令名和 data type name 冲突风险。文档既然复用了 `BF.*` 命令和 `MBbloom--` 类型名，就应该明确：

```text
same Redis instance
        │
        ├─ RedisBloom module loaded
        └─ gemini-bloom loaded
              │
              ▼
        command/type registration conflict expected
```

这不是代码问题，单看文档也应该补上。

---

## 2. 代码与文档交叉分析：哪些代码与文档冲突 / 相悖

### 2.1 总表

| ID |         严重度 | 文档预期                             | 代码事实                                                 | 判断                   |
| -- | ----------: | -------------------------------- | ---------------------------------------------------- | -------------------- |
| C1 |    Critical | `SCANDUMP/LOADCHUNK` 内部使用，不对客户开放 | 注册为普通 Redis 命令                                       | 直接冲突                 |
| C2 |    Critical | `LOADCHUNK` 安全、不覆盖已有 Bloom key   | `cursor>1` 可直接覆写已有 Bloom key 的 layer bit array       | 文档安全语义不完整 / 相悖       |
| C3 |        High | 重复 `EXPANSION` option 拒绝         | `BF.INSERT EXPANSION 0 EXPANSION 0` 不会按 duplicate 拒绝 | 直接冲突                 |
| C4 |        High | `(0,1)` 创建参数 + RDB 反序列化约束应自洽     | 创建路径不限制 `bitsPerEntry <= 1000`，load 路径限制             | 设计域不闭合，代码确认风险        |
| C5 |        High | 资源限制应保护持久化安全                     | runtime 扩容无 total data size 4GB 累计限制                 | 设计/实现约束不闭合           |
| C6 | Medium-High | AOF rewrite 是持久化路径               | AOF rewrite header 分配失败时日志写 “key omitted” 后返回        | 失败语义未覆盖，疑似 fail-open |
| C7 |      Medium | encver 2/4 兼容                    | `RdbLoadBloom()` 接受所有 `encver <= 4`                  | 接受面大于文档承诺            |
| C8 |      Medium | 兼容性已验证                           | 源码注释仍说 full compatibility 未验证                        | 仓库内口径冲突              |

---

### 2.2 C1：`SCANDUMP/LOADCHUNK` 文档说内部，代码注册为公开命令

文档明确说 `BF.SCANDUMP / BF.LOADCHUNK` 不兼容 RedisBloom，gemini 使用私有 layer-index cursor 协议，不对客户开放。

但代码在 `RegisterBloomCommands()` 中直接注册：

```cpp
// modules/gemini-bloom/src/bloom_commands.cc::RegisterBloomCommands
{"BF.SCANDUMP",  CmdScandump,  "readonly fast"},
{"BF.LOADCHUNK", CmdLoadchunk, "write deny-oom"},
```

这就是直接冲突：

```text
DESIGN.md expected:
  internal protocol, not exposed to customers

code actual:
  Redis command table contains BF.SCANDUMP / BF.LOADCHUNK
        │
        ▼
  callable by any client with command permission
```

这不是简单的“兼容性差异”。`LOADCHUNK` 是任意构造 Bloom 内部 bit array 的入口，暴露后会变成数据完整性攻击面。

建议按文档为准修代码：

1. 不注册公开 `BF.SCANDUMP/BF.LOADCHUNK`；或者
2. 注册成私有命名，例如 `GEMINI.BLOOM.LOADCHUNK`，并通过 ACL / proxy 阻断普通客户；或者
3. 文档改口，承认它是 public dangerous API，并补安全协议。

---

### 2.3 C2：`LOADCHUNK` 可以覆写已有 Bloom key 的 layer bit array

文档安全行为写的是：

```text
cursor=1 在 key 已存在且是 Bloom 类型时返回 ERR received bad data，不覆盖旧 key
cursor=1 在 key 是非 Bloom 类型时返回 WRONGTYPE
数据 chunk 长度必须精确匹配该层 dataSize
```

代码对 `cursor=1` 的确符合文档：已有 Bloom key 返回 `ERR received bad data`。但 `cursor>1` 时，代码会打开已有 key，取出对应 layer，然后直接 `memcpy` 覆写 bit array。

```cpp
// modules/gemini-bloom/src/bloom_commands.cc::CmdLoadchunk
size_t idx = static_cast<size_t>(cursor - 2);
auto& layer = filter->Layers()[idx];

if (dataLen != static_cast<size_t>(layer.bloom.GetDataSize())) {
  return RedisModule_ReplyWithError(ctx, "ERR data length mismatch for layer");
}

std::memcpy(layer.bloom.GetBitArray(), data, dataLen);
```

实际风险：

```text
existing Bloom key
        │
        ▼
BF.LOADCHUNK key 2 <all-zero-bytes>
        │
        ▼
layer0 bit array overwritten
        │
        ▼
previously inserted items may become false negatives
```

这和 Bloom Filter 的核心语义冲突：已插入元素不应出现 false negative。

文档只描述了 `cursor=1` 不覆盖旧 key，但没有约束 `cursor>1` 对已有 key 的写入。这在“内部协议”前提下可以勉强成立；一旦代码公开注册命令，就变成严重设计-实现冲突。

按文档为准，代码需要引入 loading state：

```text
LOADCHUNK cursor=1
        │
        ▼
create hidden/incomplete loading object
        │
        ▼
LOADCHUNK cursor=2..N+1
        │
        ▼
all chunks loaded and validated
        │
        ▼
atomic publish as visible Bloom key
```

并且普通已完成 Bloom key 不应接受 `cursor>1` 覆写。

---

### 2.4 C3：`BF.INSERT EXPANSION 0` 的重复 option 检查与文档不一致

文档说 parser 行为包括：重复的 `EXPANSION`、`NONSCALING`、`ERROR`、`CAPACITY` 选项会被拒绝。

代码里 `BF.RESERVE` 使用 `expansionSeen`，所以 `EXPANSION 0 EXPANSION 0` 会被识别成重复。

但 `BF.INSERT` 的 parser 只在 `val > 0` 时设置 `expansionSet = true`；`val == 0` 时只设置 `opts.fixedSize = true`，没有记录“EXPANSION option 已出现”。

```cpp
// modules/gemini-bloom/src/bloom_commands.cc::ParseInsertOptions
if (val == 0) {
  opts.fixedSize = true;
} else {
  opts.expansion = static_cast<unsigned>(val);
  expansionSet = true;
}
```

因此：

```redis
BF.INSERT k EXPANSION 0 EXPANSION 0 ITEMS a
```

按文档预期：

```text
ERR duplicate EXPANSION option
```

按代码路径：

```text
first EXPANSION 0  -> fixedSize = true, expansionSet remains false
second EXPANSION 0 -> still not duplicate
ITEMS a            -> accepted
```

这是明确的代码-文档冲突。

修法：`BF.INSERT` 应像 `BF.RESERVE` 一样拆成两个状态：

```cpp
bool expansionSeen = false; // any EXPANSION option appeared
bool expansionPositive = false; // EXPANSION > 0
```

---

### 2.5 C4：创建路径不限制 `bitsPerEntry <= 1000`，但 load 路径会拒绝

文档规定 RDB/wire 反序列化时 `bitsPerEntry > 1000` 拒绝。

代码实现确实如此：

```cpp
// modules/gemini-bloom/src/bloom_rdb.cc::ValidateLayerFields
if (!std::isfinite(f.bitsPerEntry) || f.bitsPerEntry <= 0.0 ||
    f.bitsPerEntry > 1000.0) return false;
```

但创建路径 `BloomLayer::Create()` 只检查 `falsePositiveRate` 是否 finite、是否在 `(0,1)`，没有检查计算出来的 `bitsPerEntry <= 1000`。

```cpp
// modules/gemini-bloom/src/bloom_filter.cc::BloomLayer::Create
layer.bitsPerEntry_ = OptimalBitsPerEntry(falsePositiveRate);
auto rawBits = static_cast<double>(cap) * layer.bitsPerEntry_;
...
layer.hashCount_ = OptimalHashCount(layer.bitsPerEntry_);
```

风险链：

```text
BF.RESERVE k 1e-300 1
        │
        ▼
BloomLayer::Create accepts fpRate in (0,1)
        │
        ▼
bitsPerEntry ≈ 1438 > 1000
        │
        ▼
RDB save writes that metadata
        │
        ▼
RDB load rejects bitsPerEntry > 1000
```

这不是“文档与代码表面冲突”，而是文档约束和代码路径共同暴露出的持久化不变量缺失。

按文档为准，应该补一个统一 validator：

```text
ValidateCreateParams
        │
        ├─ command path
        ├─ config path
        ├─ auto-grow path
        └─ must imply ValidateLayerFields
```

---

### 2.6 C5：runtime 扩容没有 total data size 4GB 累计限制

文档写 `total data size (RDB/wire) <= 4GB`，并且 RDB 反序列化路径确实累计检查 `totalDataSize`。

但 runtime 扩容路径只检查：

1. 顶层是否满；
2. fixed-size 是否禁止扩容；
3. 下一层 capacity 是否 `uint64_t` 溢出；
4. 下一层 fp rate 是否低于 `1e-15`；
5. `AppendLayer()` 是否成功。

代码没有维护“整个 filter 的累计 data size 是否超过 4GB”。

```cpp
// modules/gemini-bloom/src/sb_chain.cc::GrowIfNeeded
if (prevCap > UINT64_MAX / expansionFactor_) return false;
uint64_t nextCap = prevCap * expansionFactor_;
double nextRate = top.bloom.GetFpRate() * kTighteningRatio;
return (nextRate >= kMinFpRate) && AppendLayer(nextCap, nextRate);
```

而 `AppendLayer()` 只创建单层，不检查全局总 data size。

```text
runtime Put()
   │
   ▼
GrowIfNeeded()
   │
   ├─ checks next layer only
   └─ does not check total filter bytes
        │
        ▼
possible runtime object outside RDB-load accepted domain
```

如果文档里的 4GB 是“仅对外部输入 payload 的防御限制”，那代码和文档不算硬冲突；但如果文档目标是保证持久化对象永远可 reload，那么当前实现不满足。

建议文档明确语义，同时代码增加 runtime 累计限制。

---

### 2.7 C6：AOF rewrite 分配失败时可能省略 key，文档没有定义失败语义

文档说 `AofRewriteBloom()` 输出 `BF.LOADCHUNK` 命令序列，使用 gemini 私有 cursor 协议。

代码中，如果 header buffer 分配失败，会记录：

```cpp
"GeminiBloom: AOF rewrite allocation failure, key omitted"
```

然后直接 `return`。

```cpp
// modules/gemini-bloom/src/bloom_rdb.cc::AofRewriteBloom
auto* hdrBuf = static_cast<char*>(RMAlloc(hdrBytes));
if (!hdrBuf) {
  RedisModule_LogIOError(aof, "warning",
    "GeminiBloom: AOF rewrite allocation failure, key omitted");
  return;
}
```

这里的风险取决于 Redis Module API 对 `RedisModule_LogIOError()` 的处理是否会可靠 abort rewrite。这个点需要运行验证。但从模块自身代码看，它的显式行为是“key omitted”。

按文档为准，AOF rewrite 是持久化路径，失败语义必须 fail-closed：

```text
AOF rewrite allocation failure
        │
        ├─ acceptable: abort whole rewrite, keep old AOF
        └─ unacceptable: generate new AOF missing this key
```

建议把这个作为测试门禁，而不是只写日志。

---

### 2.8 C7：encver 接受面大于文档承诺

文档只描述：

```text
encver 2: added BloomFlags
encver 4: added expansion factor
current encver: 4
```

代码的 `RdbLoadBloom()` 只拒绝 `encver > kCurrentEncVer`，也就是任何 `encver <= 4` 都会进入读取逻辑。

随后读取逻辑对低版本使用默认 flags / expansion：

```cpp
rawFlags = (encver >= 2) ? r.GetUint()
                         : Use64Bit | NoRound;

rawExpansion = (encver >= 4) ? r.GetUint()
                             : 2;
```

这意味着代码实际接受 encver 0/1。文档没有承诺，也没有给出 encver 0/1 corpus。

```text
DESIGN.md:
  supported/verified encver = 2, 4

code:
  accepts encver = 0, 1, 2, 3, 4
```

建议按文档收紧：

```cpp
if (encver != 2 && encver != 4) return nullptr;
```

除非确实要支持 0/1/3，并补文档和 fixtures。

---

### 2.9 C8：文档说兼容性已验证，源码注释仍说未验证

文档声称 Redis 6.2.17 + RedisBloom v2.4.20 的双向兼容矩阵验证通过，覆盖 RDB/DUMP/RESTORE/MIGRATE/RDB-preamble AOF/fullsync replication。

但源码注释仍写：

```cpp
// intended to match RedisBloom's layout
// full compatibility has not been verified against an official RedisBloom golden corpus
```

序列化代码注释也有类似表述：

```cpp
// Field order is intended to match the RedisBloom RDB wire format...
// Full interoperability has not been verified against an official RedisBloom golden corpus.
```

这是仓库内部口径冲突：

```text
DESIGN.md:
  verified with RedisBloom v2.4.20 matrix

source comments:
  full compatibility not verified against official corpus
```

可能的解释有两个：

1. 文档更新了，源码注释没更新；
2. 文档说的是项目自建 corpus，不是 official RedisBloom golden corpus。

无论哪种，都应该统一。建议写成：

```text
Verified against project-owned RedisBloom v2.4.20 corpus.
No upstream official golden corpus is known/used.
```

或者如果确实没把 corpus 固化进仓库，就把文档里的“验证通过”改成“本地验证通过，待固化为 CI corpus”。

---

## 3. 代码与文档一致的关键点

为了避免只看问题，下面是交叉检查中确认基本一致的部分。

| 设计点                                                                | 代码状态     | 依据                                                                  |                                          |                      |
| ------------------------------------------------------------------ | -------- | ------------------------------------------------------------------- | ---------------------------------------- | -------------------- |
| Redis module type name 使用 `MBbloom--`                              | 一致       | `RedisModule_CreateDataType(ctx, "MBbloom--", kCurrentEncVer, &tm)` |                                          |                      |
| 默认创建 flags 是 `Use64Bit                                             | NoRound` | 一致                                                                  | `AllocFilter()` 设置 `BloomFlags::Use64Bit | BloomFlags::NoRound` |
| fixed-size 首层 fp rate 使用原始 error rate，scaling 使用 `errorRate * 0.5` | 一致       | `ScalingBloomFilter` 构造函数里区分 FixedSize 与 scaling                    |                                          |                      |
| RDB load 拒绝未知 flags / itemCount 超 capacity / itemSum 不一致           | 一致       | `ReadFrom()` 做对应校验                                                  |                                          |                      |
| `BF.RESERVE` 参数解析整体比 RedisBloom 严格                                 | 一致       | 重复 `EXPANSION`、未知 option、NONSCALING+EXPANSION>0 均有检查                |                                          |                      |
| `BF.INFO FIELD` 返回标量                                               | 一致       | `CmdInfo()` 对单字段直接返回整数 / null                                       |                                          |                      |
| `BF.MADD/BF.INSERT` fixed-size 部分失败返回截断数组                          | 一致       | postponed array length + first error break                          |                                          |                      |

---

## 4. 建议修订顺序

### P0：必须先修

```text
1. LOADCHUNK 暴露面
   ├─ 要么不公开注册
   ├─ 要么通过 ACL/proxy 明确屏蔽
   └─ 要么文档承认 public dangerous API

2. LOADCHUNK 事务性
   ├─ cursor=1 不应创建可查询 Bloom key
   ├─ cursor>1 不应覆写 completed Bloom key
   └─ 完整 chunk 到齐后 atomic publish

3. 创建域 ⊆ 反序列化域
   ├─ bitsPerEntry <= 1000 在创建时也检查
   ├─ total data size 4GB 在 runtime 也检查
   └─ 增加“所有可创建 filter 必须 RDB round-trip 成功”测试
```

### P1：高优先级

```text
4. 修 BF.INSERT EXPANSION 0 重复检测
5. 明确 encver 只支持 2/4，或补 0/1/3 支持文档和 fixtures
6. AOF rewrite 分配失败必须 fail-closed，并加入测试
7. 统一 DESIGN 与源码注释里的兼容性验证口径
```

### P2：文档完善

```text
8. 写清 RedisBloom module 同实例冲突策略
9. 写清 Redis 8 / RedisBloom 共存加载预期
10. 写清 SCANDUMP/LOADCHUNK 的 ACL / 部署层封锁要求
11. 把 RedisBloom v2.4.20 corpus 固化成可复现目录和 CI gate
```

最终判断：

```text
DESIGN.md 自身：
  主体合理，但安全边界和持久化约束没有完全闭合。

代码对照 DESIGN.md：
  有多个实现偏离，其中最严重的是 LOADCHUNK 的公开注册和可覆写行为。
  按文档为准，这部分应视为阻断级问题。
```

