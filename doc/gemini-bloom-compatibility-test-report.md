# GeminiBloom 全量测试报告

| 项目 | 内容 |
|---|---|
| 测试日期 | 2026-08-11 |
| 测试对象 | `modules/gemini-bloom` |
| 测试期间代码范围 | `a238160..246016d`（合入前提交序列） |
| 最终复核内容 | 小 key 内存优化、AOF OOM 加固、部分加载持久化修复 |
| Redis 版本 | 5.0.14、6.2.20、7.2.11 |
| 对照实现 | RedisBloom 2.2.18、2.4.20、2.6.25 |
| 执行平台 | Docker、Ubuntu 22.04、Linux ARM64 |

## 1. 报告范围

本报告汇总本次测试期间实际执行并取得结果的 GeminiBloom 测试。内容包括：

- 构建、单元测试和命令集成测试；
- Redis 5、6、7 兼容性；
- RESP2 和 RESP3 连接；
- RDB、AOF、DUMP/RESTORE、MIGRATE 和复制；
- Redis 大版本升级；
- 并发、长时间运行和内存回收；
- ASan、UBSan、TSan 和 Valgrind；
- 大量 key、大 Bloom key 和单 key 千万/亿级数据；
- `maxmemory`、逐出策略和容器内存限制；
- 小 key 内存问题、AOF OOM 路径和部分加载持久化问题；
- 对中间修复 `f6e961d` 和最终部分加载修复的复核测试。

状态定义：

| 状态 | 含义 |
|---|---|
| 通过 | 实际执行，结果满足断言 |
| 失败 | 实际执行，稳定复现错误结果 |
| 诊断 | 用于比较格式或行为，不代表支持该兼容路径 |
| 未覆盖 | 本次没有执行，不能给出结论 |

## 2. 最终结论

普通完整对象的功能、持久化、升级、规模和内存安全测试均通过。Redis 5、6、7 的主要结果一致。

测试确认两个已修复问题：

- 单层小 key 固定预留 4 个 layer slot，造成约 208 B/key 的额外内存成本。修复后额外成本降至约 16 B/key。
- `BF.LOADCHUNK` 尚未完成时执行 AOF rewrite 或 RDB snapshot，旧实现可能在重启后保留 key 和元素计数，但丢失尚未到达的 Bloom 位图。中间修复 `f6e961d` 没有解决该问题；最终修复保存 Loading 状态和 chunk cursor，并在 Redis 5、6、7 上通过 15 个进程级回归场景。

测试还修正了一项早期判断：

- `RedisModule_Alloc()` 在标准 Redis 中不会通过返回 `nullptr` 表示 OOM。早期的 header allocation 失败依赖故障注入，只能证明 AOF callback 提前返回会造成数据遗漏，不能证明标准 Redis 的该分配点在生产中会自然返回空指针。

在本报告覆盖的 Redis 5、6、7 功能、持久化、规模、内存和运行时安全范围内，最终代码没有遗留发布阻断项。未覆盖范围列在第 16 节，不能据此外推到 Redis 8、x86_64、磁盘故障或无限时长运行。

## 3. 测试环境

### 3.1 版本矩阵

| Dockerfile | Redis | RedisBloom | 协议 |
|---|---:|---:|---|
| `Dockerfile.redis5` | 5.0.14 | 2.2.18 | RESP2 |
| `Dockerfile.redis6` | 6.2.20 | 2.4.20 | RESP3、强制 RESP2 |
| `Dockerfile.redis7` | 7.2.11 | 2.6.25 | RESP3、强制 RESP2 |
| `Dockerfile.upgrade` | 5.0.14 / 6.2.20 / 7.2.11 | 不加载 | GeminiBloom 跨版本升级 |

### 3.2 平台与隔离

- 宿主机架构：ARM64。
- 容器架构：Linux ARM64。
- 基础系统：Ubuntu 22.04。
- Redis 和 RedisBloom 源码包来自仓库 `deps/`。
- Redis 进程均运行在 Docker 容器中。
- 千万/亿级测试使用 2 GiB 内存上限，`--memory-swap` 与 `--memory` 相同。
- AOF OOM 测试使用 64 MiB 和 128 MiB 内存上限，禁止额外 swap。
- OOM 测试会读取 cgroup 限制，并拒绝无限制或超过 256 MiB 的环境。

本次没有执行 Linux x86_64 或大小端交叉测试。

## 4. 结果总表

### 4.1 主测试矩阵

| 测试项 | Redis 5 | Redis 6 | Redis 7 |
|---|---:|---:|---:|
| Docker 构建 | 通过 | 通过 | 通过 |
| GTest: BloomLayer | 30/30 | 30/30 | 30/30 |
| GTest: ScalingBloomFilter | 24/24 | 24/24 | 24/24 |
| GTest: RDB/wire/AOF | 70/70 | 70/70 | 70/70 |
| Tcl 命令集成 | 185/185 | 185/185 | 188/188 |
| 完整对象持久化 | 6/6 | 6/6 | 6/6 |
| 并发与分配回收 | 2/2 | 2/2 | 2/2 |
| RedisBloom 命令对照 | 346 通过，12 跳过 | 370/370，执行两种协议 | 370/370，执行两种协议 |
| RDB 格式互通诊断 | 7/7 | 7/7 | 7/7 |
| ASan + UBSan | 124/124 + 集成通过 | 124/124 + 集成通过 | 124/124 + 集成通过 |
| TSan | 124/124 + 集成通过 | 124/124 + 集成通过 | 124/124 + 集成通过 |
| Valgrind | 核心和两种实现集成通过 | 核心和两种实现集成通过 | 核心和两种实现集成通过 |
| 20,000 key + 百万项大 key | 通过 | 通过 | 通过 |
| 50,000 key 内存曲线 | 通过 | 通过 | 通过 |
| `maxmemory` + 8 种策略 | 16/16 | 16/16 | 16/16 |
| 64/128 MiB AOF OOM | 4/4 | 4/4 | 4/4 |
| 单 key 1,000 万/1 亿 | 通过 | 通过 | 通过 |
| 部分加载 AOF/RDB 持久化回归 | 5/5 | 5/5 | 5/5 |

Redis 5 的 12 个跳过项来自 RedisBloom 2.2.18 不支持 `BF.CARD` 和 `BF.INFO key FIELD`，不是 GeminiBloom 失败。

Redis 7 的 Tcl 测试多 3 项，原因是 Redis 7 支持 module key 的 `COPY` 测试。

最终部分加载修复另外执行 130 个核心单元测试：BloomLayer 30/30、ScalingBloomFilter 24/24、RDB/wire/AOF 76/76，全部通过。

### 4.2 跨 Redis 版本升级

| 升级路径 | RDB + 升级后写入/重启 | DUMP/RESTORE | fullsync + 增量命令 | AOF RDB preamble | command-AOF |
|---|---:|---:|---:|---:|---:|
| Redis 5 -> Redis 6 | 通过 | 通过 | 通过 | 通过 | 通过 |
| Redis 5 -> Redis 7 | 通过 | 通过 | 通过 | 通过 | 通过 |
| Redis 6 -> Redis 7 | 通过 | 通过 | 通过 | 通过 | 通过 |

合计 15/15 通过。测试两端始终加载 GeminiBloom，不包含 RedisBloom 与 GeminiBloom 的在线替换。

## 5. 功能与命令测试

### 5.1 Bloom 命令

实际测试以下 10 个命令：

- `BF.RESERVE`
- `BF.ADD`
- `BF.MADD`
- `BF.INSERT`
- `BF.EXISTS`
- `BF.MEXISTS`
- `BF.INFO`
- `BF.CARD`
- `BF.SCANDUMP`
- `BF.LOADCHUNK`

覆盖内容：

- 正常创建、自动创建、固定大小和自动扩容；
- `ERROR`、`CAPACITY`、`EXPANSION`、`NONSCALING`、`NOCREATE`；
- option 缺值、重复、互斥、未知 option 和数值越界；
- 错误参数个数、错误类型和 key 已存在；
- `MADD`/`INSERT` 部分成功和数组截断语义；
- NONSCALING 满容量错误；
- 空字符串、长字符串、NUL、CRLF 和 0..255 二进制值；
- expansion 1、2、4 和多层扩容；
- half-loaded 状态下拒绝读写命令；
- `LOADCHUNK` 非法 cursor、错误长度、乱序和重复加载；
- `SCANDUMP`/`LOADCHUNK` 正常完整往返；
- false-positive rate 基础检查。

### 5.2 Redis 通用命令

实际覆盖：

- `EXPIRE`、`PEXPIRE`、`TTL`、`PTTL`、`PERSIST`；
- `RENAME`、`RENAMENX`、`COPY`；
- `DUMP`、`RESTORE`、`RESTORE REPLACE`；
- `UNLINK`、`TYPE`、`EXISTS`、`TOUCH`；
- `SCAN`、`KEYS`、`RANDOMKEY`；
- `MEMORY USAGE`、`OBJECT REFCOUNT`、`OBJECT IDLETIME`、`OBJECT ENCODING`；
- `SELECT`、`MOVE`、`SWAPDB`；
- `MULTI/EXEC`、`DISCARD`、`WATCH`；
- Lua `EVAL`；
- keyspace notification；
- `COMMAND INFO`、`COMMAND GETKEYS` 和 ACL dry-run。

### 5.3 RESP2 和 RESP3

- Redis 5 使用 RESP2。
- Redis 6 和 Redis 7 分别使用自动 RESP3 和强制 RESP2 执行兼容性及稳态测试。
- 测试确认命令在 RESP3 连接下可用。
- GeminiBloom 返回值仍采用 RESP2 形状，不提供 RedisBloom 原生 RESP3 boolean/map 类型。

## 6. 持久化、复制与格式测试

### 6.1 完整对象持久化

每个 Redis 版本执行 6 个场景：

1. RDB `BGSAVE` + restart；
2. DUMP/RESTORE，包含 TTL；
3. MIGRATE，包含 TTL；
4. master/replica fullsync + 增量 Bloom 命令；
5. `aof-use-rdb-preamble yes`；
6. `aof-use-rdb-preamble no` 的 command-AOF rewrite。

完整对象的 key 数、TTL、Capacity、filter 数、inserted 数、Expansion、CARD 和 membership 均通过检查。

部分多层对象的 `BF.INFO Size` 在重启后变化，例如 944 B 变为 880 B。该字段包含运行时 layer capacity，不是稳定持久化字段。逻辑 metadata 和 membership 没有变化。

### 6.2 RDB/wire 防御性测试

覆盖：

- 截断 header 和 layer blob；
- 0 层和超过层数上限；
- `totalBits`、`dataSize`、`itemCount` 和总数据大小不一致；
- NaN、Inf、0、负数和越界 false-positive rate；
- `hashCount`、`bitsPerEntry`、`log2Bits` 不一致；
- 未知 flag 和内部 Loading flag；
- capacity、expansion 和累加溢出；
- RDB 旧编码版本兼容。

### 6.3 RedisBloom RDB 格式诊断

每个 Redis 版本执行 7 个诊断场景：

- RedisBloom -> GeminiBloom；
- GeminiBloom -> RedisBloom；
- GeminiBloom -> GeminiBloom；
- RedisBloom -> RedisBloom；
- 两种双向多跳；
- 四跳交替加载。

每个方向比较 16 个 filter corpus，单次执行 283,131 次 `EXISTS`，并比较可比的 metadata。7/7 通过。

该结果仅说明当前测试样本的 RDB 格式可以相互读取，不表示支持 RedisBloom 与 GeminiBloom 的生产替换升级。

## 7. 并发与长时间运行

### 7.1 并发测试

- 8 个客户端并发操作共享 Bloom key；
- 并发 `MADD`、重复值、CARD 和 membership 检查；
- 反复创建和删除 Bloom key；
- 分配回收后检查内存增长门限。

Redis 5、6、7 均为 2/2 通过。

### 7.2 300 秒稳态测试

| Redis / 协议 | 时长 | 循环 | 操作数 | 内存变化 |
|---|---:|---:|---:|---:|
| Redis 5 / RESP2 | 300 s | 15,450 | 27,508,595 | +32 B |
| Redis 6 / RESP3 | 300 s | 15,336 | 27,307,665 | +32 B |
| Redis 6 / RESP2 | 300 s | 15,570 | 27,722,270 | +32 B |
| Redis 7 / RESP3 | 300 s | 15,519 | 27,634,407 | +296,288 B |
| Redis 7 / RESP2 | 300 s | 15,374 | 27,376,735 | +296,288 B |

合计运行 1,500 秒，执行 137,549,672 次操作。负载包含创建、写入、查询、INFO、CARD、SCANDUMP、LOADCHUNK 和删除。

Redis 7 的约 289 KiB 增长出现在预热阶段，后续 300 秒没有继续增长。独立 allocation-churn 测试中，Redis 5/6 增长 32 B，Redis 7 增长 25,712 B，均低于 16 MiB 门限。没有观察到随循环持续增长的模块内存泄漏。

## 8. Sanitizer 与 Valgrind

### 8.1 ASan + UBSan

每个 Redis 版本执行：

- 30 个 BloomLayer 单元测试；
- 24 个 ScalingBloomFilter 单元测试；
- 70 个 RDB/wire/AOF 单元测试；
- 真实 redis-server 集成负载；
- 并发写入、二进制值、10 层扩容和分配回收；
- RDB、command-AOF rewrite 和 restart。

核心测试和集成负载均通过。

ASan 单元测试启用 leak detection。Redis 集成阶段关闭 ASan leak detection，避免 Redis 进程本身的退出分配影响模块结果。

### 8.2 UBSan 测试代码问题

首次执行时，UBSan 报告 GTest 直接引用 packed wire struct 字段造成未对齐访问。问题位于测试断言，不在模块命令或序列化代码。测试改为按值读取后，ARM64 全量通过。

### 8.3 TSan

TSan 独立构建执行相同的 124 个核心测试和 Redis 集成负载，Redis 5、6、7 均通过。

TSan 结果覆盖插桩后的模块代码和测试负载，不覆盖未插桩 Redis 内部、kernel 或文件系统竞态。

### 8.4 Valgrind

- GeminiBloom 的 124 个核心测试均为 `ERROR SUMMARY: 0 errors`；
- GeminiBloom 真实 Redis 集成负载通过；
- 对应 RedisBloom 对照模块运行相同集成负载并通过；
- definite leak 和 Memcheck error 使用非零退出码 gate。

RedisBloom 预编译模块没有 ASan/UBSan/TSan 插桩，因此 sanitizer 阶段只作为功能对照；Valgrind 阶段两种实现都实际运行 Memcheck。

## 9. 规模与内存测试

### 9.1 20,000 个小 key 和百万项大 key

每个 Redis 版本分别测试 GeminiBloom 和 RedisBloom：

- 创建 20,000 个 capacity=100 的 Bloom key；
- 每个 key 写入 3 项；
- restart 前后检查全部 60,000 个 membership；
- 创建 capacity=1,000,000、error rate=0.001 的 NONSCALING key；
- 写入 1,000,000 个候选值；
- 两种实现均记录 999,893 个新元素；
- restart 前后各全量读取 1,000,000 个候选值，false negative 为 0；
- 对 100,000 个未写入值查询，restart 前后均得到 97 个 false positive。

GeminiBloom 与 RedisBloom 的逻辑结果一致。

### 9.2 50,000 个小 key 内存曲线

测试分别记录 1,000、5,000、20,000 和 50,000 个小 key 的进程内存及 dataset memory，并计算线性斜率。每个 key 为 capacity=100，写入 3 项。

同时测试：

- capacity 1、100、1,000、10,000、100,000、1,000,000；
- 1 层、5 层、10 层对象；
- BGSAVE/restart 前后的 membership、metadata 和对象大小。

修复前 Redis 5 结果：

| 指标 | GeminiBloom | RedisBloom | 差值 |
|---|---:|---:|---:|
| 内存斜率 | 554.425 B/key | 346.425 B/key | +208 B/key |
| 50,000 key 总增量差 | - | - | +10,400,000 B |
| 单 key `BF.INFO Size` | 448 B | 296 B | +152 B |

原因是首层动态数组固定预留 4 个 `FilterLayer` slot。

修复后结果：

| Redis | GeminiBloom | RedisBloom | 额外成本 | 50,000 key 总差 |
|---|---:|---:|---:|---:|
| 5 | 362.425 B/key | 346.425 B/key | 16 B/key | 约 0.80 MB |
| 6 | 362.425 B/key | 346.425 B/key | 16 B/key | 约 0.80 MB |
| 7 | 362.704 B/key | 346.704 B/key | 16 B/key | 约 0.80 MB |

额外成本下降约 92.3%，通过 `<= 64 B/key` 回归门限。

多层对象仍保留动态数组几何扩容成本。Redis 6/7 的 5 层和 10 层 case 中，GeminiBloom 的 `BF.INFO Size` 分别比 RedisBloom 多 208 B 和 400 B。

## 10. `maxmemory` 与逐出

每种实现创建 600 个 capacity=20,000 的 NONSCALING Bloom key，使数据集超过随后设置的内存上限。

执行策略：

- `noeviction`；
- `allkeys-lru`；
- `allkeys-lfu`；
- `allkeys-random`；
- `volatile-lru`；
- `volatile-lfu`；
- `volatile-random`；
- `volatile-ttl`。

检查结果：

- `noeviction` 下，新建 key 和已有 key 的写入均被 OOM gate 拒绝；
- 失败命令不创建 key，不改变旧 key 的逻辑 metadata；
- allkeys 策略确实逐出 Bloom key；
- 所有幸存 key 保持 seed membership 和合法 metadata；
- volatile 策略只逐出设置 TTL 的压力 key；
- 无 TTL 的 sentinel key 保留且可读。

GeminiBloom 与 RedisBloom 比较相同安全不变量，不要求逐出数量完全相等。两种实现的对象内存统计不同，会影响达到同一内存目标时的逐出数量。

Redis 5、6、7 均为 16/16 通过。

## 11. AOF OOM 与故障注入

### 11.1 测试数据

- 100 个小 Bloom key；
- 1 个 capacity=10,000,000 的 NONSCALING key；
- 大 key 写入 100,000 项；
- `aof-use-rdb-preamble no`；
- `appendfsync always`。

### 11.2 callback 提前返回的后果

旧 AOF callback 为 header 分配临时 heap buffer，并在分配返回空指针时记录日志后直接返回。

故障注入使该分配点返回 `nullptr` 后，真实 redis-server 结果为：

- `BGREWRITEAOF` 状态为 `ok`；
- 新 AOF 只有 23 B；
- restart 后 101 个 Bloom key 全部丢失。

该测试证明 `RedisModule_LogIOError()` 不会设置 `RedisModuleIO::error`，callback 提前返回可能生成被 Redis 接受的不完整 AOF。

该测试不证明标准 Redis 的 `RedisModule_Alloc()` 会自然返回 `nullptr`。Redis 5/6 使用 `zmalloc()`，Redis 7 使用 `zmalloc_usable()`；标准实现分配失败时会 panic/abort，而不是返回空指针。该场景应归类为故障注入验证和防御性加固，不应归类为已确认的标准 Redis 生产 OOM 触发路径。

### 11.3 cgroup OOM

Redis 5、6、7 均对 GeminiBloom 和 RedisBloom 执行：

| 内存限制 | swap | 预期 rewrite | GeminiBloom | RedisBloom | restart |
|---|---|---|---|---|---|
| 64 MiB | 禁用 | `err` | 通过 | 通过 | 旧 AOF 保留，数据完整 |
| 128 MiB | 禁用 | `ok` | 通过 | 通过 | 新 AOF 可回放，数据完整 |

64 MiB 场景中，系统级 OOM 终止 rewrite child，父进程保留旧 AOF。未观察到宿主机失控 OOM。

## 12. 单 key 千万/亿级测试

### 12.1 结论

Redis 5.0.14、6.2.20、7.2.11 下的 GeminiBloom 均通过单 key 1,000 万和 1 亿 item 测试，并与对应 RedisBloom 2.2.18、2.4.20、2.6.25 得到完全一致的逻辑结果。

没有发现新的正确性问题：

- 所有已写入候选值全量读取均为 1，零 false negative；
- `BF.INFO` capacity、filter 数、inserted 数与命令返回值一致；
- 100 万未写入值的 false positive 数与 RedisBloom 完全一致；
- BGSAVE/restart 后 metadata 和抽样 membership 完整；
- 1 亿规模没有出现 32 位溢出、截断、崩溃、OOM 或异常扩容；
- GeminiBloom 与 RedisBloom 的单 key 内存相差约 16 B。

测试观察到 GeminiBloom 吞吐低于 RedisBloom，但本轮固定先跑 GeminiBloom、后跑 RedisBloom，且每组只有一次计时，因此只能作为性能线索，不能定性为稳定性能回归。

### 12.2 环境与隔离

| Docker 镜像 | Redis | RedisBloom | 平台 |
|---|---:|---:|---|
| `gemini-module:redis5-deep` | 5.0.14 | 2.2.18 | linux/arm64 |
| `gemini-module:redis6-deep` | 6.2.20 | 2.4.20 | linux/arm64 |
| `gemini-module:redis7-deep` | 7.2.11 | 2.6.25 | linux/arm64 |

每个测试都运行在独立 Docker 容器中：

- `--memory=2g`；
- `--memory-swap=2g`，不允许额外 swap；
- `--rm`，退出后删除临时 Redis、RDB 和日志；
- 不在宿主机启动 redis-server。

### 12.3 测试参数与步骤

每个 Redis 版本分别测试 GeminiBloom 和 RedisBloom：

| 规模 | Capacity | 写入尝试 | Error rate |
|---|---:|---:|---:|
| 千万级 | 10,000,000 | 10,000,000 | 0.001 |
| 亿级 | 100,000,000 | 100,000,000 | 0.001 |

item 为确定性的 8 字节二进制整数。写入批次为 10,000，读取批次为 20,000。

每个 key 执行：

1. 写入全部 1,000 万或 1 亿候选 item；
2. 全量读取全部候选 item，检查零 false negative；
3. 查询 100 万个未写入 item，记录 false positive；
4. 检查 `BF.INFO`、`MEMORY USAGE` 和命令返回的新增元素数；
5. BGSAVE 并重启 Redis；
6. 千万级抽样 10,001 项，亿级抽样 100,001 项，重新检查 membership 和 metadata。

三版本、两实现合计执行：

- 660,000,000 次写入尝试；
- 660,000,000 次已写入候选读取；
- 12,000,000 次未写入查询；
- 660,012 次 restart 后 membership 抽样。

### 12.4 正确性结果

三个 Redis 版本的逻辑结果相同：

| 规模 | 尝试写入 | 记录为新元素 | 全量读取 false negative | 未写入查询 | False positive | restart 抽样 |
|---|---:|---:|---:|---:|---:|---:|
| 1,000 万 | 10,000,000 | 9,998,819 | 0 | 1,000,000 | 1,030 | 10,001 |
| 1 亿 | 100,000,000 | 99,987,772 | 0 | 1,000,000 | 1,026 | 100,001 |

写入时少于尝试数是 Bloom Filter 的正常近似语义：部分首次出现的候选值在写入前已被误判为存在，因此 `BF.MADD` 返回 0。GeminiBloom 和 RedisBloom 的命令返回计数、最终 inserted 数、missing false positive 数和 restart 后结果完全一致。

没有观察到：

- 32 位计数溢出；
- 截断；
- 异常扩容；
- crash；
- 容器 OOM；
- 已写入元素 false negative。

### 12.5 内存结果

| Redis | 规模 | GeminiBloom `MEMORY USAGE` | RedisBloom `MEMORY USAGE` | 差值 |
|---|---:|---:|---:|---:|
| 5 | 1,000 万 | 17,972,143 B | 17,972,127 B | +16 B |
| 5 | 1 亿 | 179,720,000 B | 179,719,984 B | +16 B |
| 6 | 1,000 万 | 17,972,144 B | 17,972,128 B | +16 B |
| 6 | 1 亿 | 179,720,000 B | 179,719,984 B | +16 B |
| 7 | 1,000 万 | 17,972,144 B | 17,972,128 B | +16 B |
| 7 | 1 亿 | 179,720,000 B | 179,719,984 B | +16 B |

亿级单 key 实际对象约 171.4 MiB。restart 前后 `MEMORY USAGE`、metadata 和抽样 membership 保持一致，没有额外复制或层数增长。

### 12.6 吞吐观察

以下是单轮 wall-clock 结果，包含本地 Redis 协议和 Python 客户端批处理开销：

| Redis | 规模 | GeminiBloom 写入/s | RedisBloom 写入/s | GeminiBloom 读取/s | RedisBloom 读取/s |
|---|---:|---:|---:|---:|---:|
| 5 | 1,000 万 | 1,027,794 | 1,366,461 | 1,195,585 | 1,445,786 |
| 5 | 1 亿 | 931,562 | 1,271,054 | 1,058,288 | 1,271,521 |
| 6 | 1,000 万 | 1,067,886 | 1,233,121 | 1,226,782 | 1,290,128 |
| 6 | 1 亿 | 943,340 | 1,188,788 | 993,177 | 1,261,347 |
| 7 | 1,000 万 | 1,065,529 | 1,357,572 | 1,235,601 | 1,510,104 |
| 7 | 1 亿 | 948,504 | 1,327,168 | 1,059,556 | 1,372,511 |

亿级场景中，GeminiBloom 的单轮写入吞吐比 RedisBloom 低约 21% 到 29%，读取低约 17% 到 23%。这个趋势跨三个 Redis 版本出现，值得做独立 microbenchmark 和 profiler；但在没有交替顺序和多轮统计之前，不把它列为已确认产品缺陷。

### 12.7 复现

脚本默认运行正式的 1,000 万和 1 亿参数：

```bash
docker run --rm \
  --memory=2g \
  --memory-swap=2g \
  -v "$PWD/ci:/src/ci:ro" \
  -w /src \
  --entrypoint python3 \
  gemini-module:redis7-deep \
  ci/bloom_huge_key_test.py \
  ./build/redis_bloom.so \
  /opt/redisbloom.so
```

可用 `BLOOM_HUGE_KEY_SIZES`、`BLOOM_HUGE_MISSING_READS`、`BLOOM_HUGE_WRITE_BATCH`、`BLOOM_HUGE_READ_BATCH` 和 `BLOOM_HUGE_RESTART_SAMPLE_STEP` 调整参数。

## 13. 部分加载问题复现与最终修复

### 13.1 提交内容

`f6e961d` 删除 `AofRewriteBloom()` 对 `IsLoading()` 的提前返回，改为对部分加载对象输出 header 和全部 layer。

新增单元测试 `AofRewriteEmitsPartiallyLoadedFilter`。在 Redis 7 构建环境中执行结果为：

- BloomLayer 30/30；
- ScalingBloomFilter 24/24；
- RDB/wire/AOF 71/71；
- 合计 125/125。

单元测试通过，但没有覆盖真实失败路径。

### 13.2 真实 restart 复现

使用 `f6e961d` 生成的同一个 ARM64 module，在 Redis 5、6、7 容器中执行：

1. 创建包含 100 个 item 的 source Bloom filter；
2. 通过 `BF.SCANDUMP` 获取 header 和位图 chunk；
3. 只执行 `BF.LOADCHUNK target 1 <header>`；
4. 创建 30,000 个普通 padding key；
5. 启动 `BGREWRITEAOF`；
6. rewrite fork 后执行真实位图 chunk；
7. 确认 live target 可以命中已写入 item；
8. 等待 rewrite 成功并从新 AOF restart。

结果：

| Redis | restart 前 membership | restart 后 key | restart 后 membership | restart 后 `BF.CARD` |
|---|---:|---:|---:|---:|
| 5.0.14 | 1 | 1 | 0 | 100 |
| 6.2.20 | 1 | 1 | 0 | 100 |
| 7.2.11 | 1 | 1 | 0 | 100 |

Redis 6 和 Redis 7 在 AOF replay 时记录：

```text
ERR received bad data
after processing the command 'BF.LOADCHUNK'
```

### 13.3 原因

header 创建全部 layer，但尚未收到的 layer 位图为 0。`f6e961d` 将这些 0 位图也写入 base AOF。

replay 时：

1. header 使对象进入 Loading；
2. base AOF 中的全部 layer 使 `ChunksLoaded()` 达到 `NumLayers()`；
3. 模块清除 Loading，认为对象加载完成；
4. fork 后写入 incremental AOF 的真实 chunk 被判定为重复加载；
5. 真实 chunk 被拒绝，0 位图保留；
6. key 和 CARD 存在，但 membership 产生 false negative。

### 13.4 新增单元测试的缺口

新增测试先向完整 filter 写入数据，再手工调用 `SetLoading()`。该对象的 layer 已包含有效位图，不等同于只执行 header 的真实 `BF.LOADCHUNK` 状态。

测试只检查：

- callback 调用次数；
- cursor；
- payload 大小；
- header replay 后 Loading flag 被移除。

测试没有执行：

- header-only 或部分 layer 状态；
- base AOF 与 incremental AOF 连续 replay；
- fork 后继续 `BF.LOADCHUNK`；
- redis-server restart；
- restart 后 membership、CARD 和错误日志检查。

### 13.5 修复要求

command-AOF rewrite 应在 Loading 状态下只输出已经收到的 layer：

```text
layer_count = filter.IsLoading() ? filter.ChunksLoaded() : filter.NumLayers()
emit header
emit layers [0, layer_count)
```

这样 replay 后对象保持相同的 Loading 和 `ChunksLoaded()` 进度，incremental AOF 中的后续 chunk 可以继续加载。

RDB 路径还必须保存 Loading 和 `ChunksLoaded()`，否则 RDB preamble 和纯 RDB snapshot 会把未完成对象恢复成完整对象。

### 13.6 最终实现与复核结果

最终修复包含三部分：

1. command-AOF rewrite 只输出 header 和已经收到的 layer 前缀；
2. RDB 保存 Loading 标志和 `ChunksLoaded()`，完整对象的 RDB 字节布局保持不变；
3. `COPY`/Clone 保留相同的加载游标。

核心单元测试结果：

- BloomLayer：30/30；
- ScalingBloomFilter：24/24；
- RDB/wire/AOF：76/76；
- 合计：130/130。

`ci/bloom_partial_load_test.py` 在每个 Redis 版本上执行以下 5 个真实持久化场景：

1. command-AOF，header-only；
2. command-AOF，已加载部分 layer；
3. RDB-preamble AOF，header-only；
4. RDB-preamble AOF，已加载部分 layer；
5. 纯 RDB，header-only，重启后继续加载。

| Redis | 结果 |
|---|---:|
| 5.0.14 | 5/5 |
| 6.2.20 | 5/5 |
| 7.2.11 | 5/5 |

AOF 场景确保剩余 chunk 在 rewrite fork 之后写入 incremental tail。每个场景都检查 restart 前 membership、restart 后 Loading 状态、继续加载、CARD、全部已写入 item 和 replay 错误日志。

完整对象的 RedisBloom RDB 互操作诊断再次通过，说明部分加载状态的条件字段没有改变普通 Bloom 对象的编码。

## 14. 已确认问题与观察项

| 编号 | 状态 | 说明 | 影响 |
|---|---|---|---|
| GB-01 | 已修复 | 单层小 key 固定预留 4 个 layer slot | 大量小 key 时内存成本增加，提前触发 maxmemory/逐出 |
| GB-02 | 已修复 | 部分加载期间 AOF rewrite，fork 后继续加载，restart 后位图丢失 | 修复后 Redis 5/6/7 共 15/15 回归通过 |
| GB-03 | 已纠正结论 | 标准 Redis allocator OOM 不通过 `nullptr` 返回 | 原故障注入结果不能作为标准生产触发证据 |
| TEST-01 | 已修复 | GTest 引用 packed 字段导致 UBSan 未对齐报告 | 仅影响测试代码，不影响模块运行 |
| PERF-01 | 待确认 | 亿级单 key 吞吐低于 RedisBloom | 需要多轮、交替顺序和 profiler |
| RDB-01 | 已修复 | 部分加载进度未写入 RDB | Loading 和 chunk cursor 已持久化，RDB 场景通过 |

除表中项目外，本次没有发现千万/亿级计数溢出、已写入数据 false negative、持续性模块内存泄漏、sanitizer 错误或 Valgrind definite leak。

## 15. 明确差异

以下差异不作为本次测试失败：

1. GeminiBloom 的 `SCANDUMP/LOADCHUNK` 使用 layer-index cursor 和私有 header，不与 RedisBloom byte-offset 协议互通。
2. command-AOF 使用 Gemini 私有 `BF.LOADCHUNK`，RedisBloom 不能回放。
3. RedisBloom 与 GeminiBloom 的在线替换升级不在支持范围。
4. `BF.INFO Size` 统计口径不同，不能要求数值完全一致。
5. 多层 filter 的 `BF.INFO Size` 包含运行时预留空间，跨 restart 可能变化。
6. GeminiBloom 没有 RedisBloom 原生 RESP3 返回类型。
7. 参数解析和部分错误文本与 RedisBloom 不完全一致。
8. RedisBloom 与 GeminiBloom 复用命令名和 `MBbloom--` 类型名，不能在同一 Redis 实例中同时加载。

## 16. 未覆盖范围

本次没有覆盖：

- Redis Cluster slot、MOVED/ASK 和 slot migration；
- Sentinel 或编排系统自动故障转移；
- 磁盘满、短写、fsync 错误和损坏 AOF/RDB；
- callback 之外任意分配点的全进程 allocator fault injection；
- Linux x86_64、不同 libc/编译器和大小端交叉测试；
- Redis 8 内置 Bloom；
- RedisBloom 2.2/2.4/2.6 之外的版本；
- 超过 300 秒的 endurance；
- 超过 50,000 个小 key 或超过单 key 100,000,000 item；
- 2^30 capacity 上限附近的完整资源曲线；
- 严格性能基准和 CPU profiler；

## 17. 测试入口与结果文件

### 17.1 主测试入口

```bash
docker build -f Dockerfile.redis5 -t gemini-module:redis5 .
docker run --rm gemini-module:redis5

docker build -f Dockerfile.redis6 -t gemini-module:redis6 .
docker run --rm gemini-module:redis6

docker build -f Dockerfile.redis7 -t gemini-module:redis7 .
docker run --rm gemini-module:redis7

bash ci/run_bloom_oom_matrix.sh gemini-module:redis7

docker build -f Dockerfile.upgrade -t gemini-module:upgrade .
docker run --rm gemini-module:upgrade
```

### 17.2 主要脚本

- `ci/bloom_compat_test.py`
- `ci/bloom_persistence_test.py`
- `ci/bloom_concurrency_test.py`
- `ci/bloom_runtime_safety_test.py`
- `ci/run_bloom_safety_tests.sh`
- `ci/bloom_scale_compare_test.py`
- `ci/bloom_memory_profile_test.py`
- `ci/bloom_eviction_compare_test.py`
- `ci/bloom_aof_oom_test.py`
- `ci/run_bloom_oom_matrix.sh`
- `ci/bloom_huge_key_test.py`
- `ci/bloom_soak_test.py`
- `ci/bloom_migrate_test.py`
- `ci/bloom_upgrade_test.py`

### 17.3 本地执行日志

以下原始日志保留在本地 `test-results/`，不纳入 Git：

- `test-results/redis5-full.log`
- `test-results/redis6-full.log`
- `test-results/redis7-full.log`
- `test-results/upgrade.log`
- `test-results/redis5-build.log`

部分加载 AOF 复核说明见 `doc/gemini-bloom-partial-load-aof-review.md`。
