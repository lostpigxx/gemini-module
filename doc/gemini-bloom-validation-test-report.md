# GeminiBloom 验证测试报告

## 文档信息

| 项目 | 内容 |
|---|---|
| 测试对象 | GeminiBloom Redis Module |
| 被测实现基线 | `d56eab707ee55615e16bca3eccb652e96f14ae31` |
| 测试执行日期 | 2026-08-11 至 2026-08-12 |
| 报告编制日期 | 2026-08-13 |
| Redis 版本 | 5.0.14、6.2.20、7.2.11 |
| 对照实现 | RedisBloom 2.2.18、2.4.20、2.6.25 |
| 执行平台 | Docker、Ubuntu 22.04、Linux ARM64 |

## 1. 测试结论

GeminiBloom 在本报告规定的 Redis 5、Redis 6 和 Redis 7 测试矩阵中完成了功能、兼容性、持久化、升级、规模、内存限制和运行时安全验证。

测试结论如下：

1. 10 个 `BF.*` 命令的正常流程、参数校验、错误处理、边界值和批量命令语义符合测试预期。
2. RDB、AOF、DUMP/RESTORE、Redis 纯内核迁移、主从复制和 Redis 纯内核大版本升级场景未发现数据丢失或已写入数据的 false negative。
3. 部分加载状态在 command-AOF、RDB-preamble AOF、纯 RDB 和 `COPY` 路径中能够保持加载进度，并在恢复后继续完成加载。
4. GeminiBloom 与对应 RedisBloom 版本在命令对照、RDB 互操作、规模测试和大 key 测试中得到一致的逻辑结果。
5. ASan、UBSan、TSan 和 Valgrind 执行范围内未报告模块内存越界、未定义行为、数据竞争或 definite leak。
6. 20,000 key、50,000 key 内存曲线、百万项 Bloom、单 key 1,000 万和 1 亿 item 测试均通过。
7. `maxmemory`、8 种逐出策略以及 64 MiB/128 MiB 容器内存限制测试均满足数据完整性和失败原子性要求。

在本报告覆盖范围内，未发现阻止发布的正确性或数据安全缺陷。

## 2. 测试范围与判定标准

### 2.1 覆盖范围

本次测试包括：

- 模块构建和 C++ 单元测试；
- Redis 命令集成测试；
- Redis 5、6、7 运行兼容性；
- RESP2、RESP3 连接；
- GeminiBloom 与 RedisBloom 命令行为对照；
- RDB 格式双向读取；
- RDB、AOF、DUMP/RESTORE、Redis 纯内核迁移和复制；
- Redis 5 到 Redis 6/7、Redis 6 到 Redis 7 的纯内核大版本升级；
- 并发客户端、对象分配回收和 300 秒稳态负载；
- ASan、UBSan、TSan 和 Valgrind；
- 大量小 key、百万项 Bloom、千万/亿级单 key；
- `maxmemory`、逐出策略和受限容器 OOM；
- 部分 `BF.LOADCHUNK` 状态的持久化和恢复。

### 2.2 状态定义

| 状态 | 定义 |
|---|---|
| 通过 | 测试已执行，全部断言满足预期 |
| 失败 | 测试已执行，至少一个断言不满足预期 |
| 跳过 | 目标版本不提供被测能力，测试未执行 |
| 未覆盖 | 不在本次测试范围内 |

### 2.3 通用通过标准

除各测试项另有说明外，统一采用以下标准：

- 已写入 item 不允许出现 false negative；
- `BF.CARD`、`BF.INFO` 逻辑字段和命令返回值保持一致；
- 持久化、重启、Redis 纯内核迁移、复制或升级后，key、TTL、metadata 和 membership 保持一致；
- 非法输入必须返回错误，不得破坏已有 key；
- OOM 或逐出场景中的失败命令不得留下半创建对象或修改已有对象 metadata；
- sanitizer 和 Valgrind 不得报告模块代码错误；
- 对照测试中，除已声明的设计差异外，GeminiBloom 与 RedisBloom 的逻辑结果必须一致。

## 3. 测试环境

### 3.1 版本矩阵

| 测试环境 | Redis | RedisBloom | 协议 |
|---|---:|---:|---|
| Redis 5 验证环境 | 5.0.14 | 2.2.18 | RESP2 |
| Redis 6 验证环境 | 6.2.20 | 2.4.20 | RESP3、强制 RESP2 |
| Redis 7 验证环境 | 7.2.11 | 2.6.25 | RESP3、强制 RESP2 |
| 纯内核升级环境 | 5.0.14、6.2.20、7.2.11 | 不加载 | Redis 纯内核大版本升级 |

Redis 和 RedisBloom 均使用固定版本源码构建。测试过程中未动态切换依赖版本。

### 3.2 平台与资源隔离

| 项目 | 配置 |
|---|---|
| 宿主机架构 | ARM64 |
| 容器架构 | Linux ARM64 |
| 基础系统 | Ubuntu 22.04 |
| Redis 进程位置 | Docker 容器内 |
| 千万/亿级测试内存 | `--memory=2g --memory-swap=2g` |
| AOF OOM 测试内存 | 64 MiB、128 MiB，禁止额外 swap |
| 临时数据 | 测试结束后随容器或临时目录删除 |

OOM 测试在执行前读取 cgroup 内存限制；无限制或超过 256 MiB 的环境不执行该测试，以避免影响宿主机。

## 4. 测试结果汇总

### 4.1 Redis 版本矩阵

| 测试类别 | Redis 5 | Redis 6 | Redis 7 | 结果 |
|---|---:|---:|---:|---|
| Docker 构建 | 通过 | 通过 | 通过 | 通过 |
| BloomLayer 单元测试 | 30/30 | 30/30 | 30/30 | 通过 |
| ScalingBloomFilter 单元测试 | 24/24 | 24/24 | 24/24 | 通过 |
| 最终 RDB/wire/AOF 单元测试 | 76/76 | 76/76 | 76/76 | 通过 |
| Tcl 命令集成 | 185/185 | 185/185 | 188/188 | 通过 |
| 完整对象持久化、纯内核迁移与复制 | 6/6 | 6/6 | 6/6 | 通过 |
| 部分加载持久化 | 5/5 | 5/5 | 5/5 | 通过 |
| 并发与分配回收 | 2/2 | 2/2 | 2/2 | 通过 |
| RedisBloom 命令对照 | 346 通过、12 跳过 | 370/370 | 370/370 | 通过 |
| RDB 互操作 | 7/7 | 7/7 | 7/7 | 通过 |
| `maxmemory` 和逐出 | 16/16 | 16/16 | 16/16 | 通过 |
| 64/128 MiB AOF OOM | 4/4 | 4/4 | 4/4 | 通过 |
| 20,000 key 和百万项 Bloom | 通过 | 通过 | 通过 | 通过 |
| 50,000 key 内存曲线 | 通过 | 通过 | 通过 | 通过 |
| 单 key 1,000 万/1 亿 | 通过 | 通过 | 通过 | 通过 |

Redis 5 的 12 个跳过项是 RedisBloom 2.2.18 不支持 `BF.CARD` 和 `BF.INFO key FIELD`，不属于 GeminiBloom 测试失败。

### 4.2 Redis 纯内核大版本升级

| 升级路径 | RDB 后继续写入并重启 | DUMP/RESTORE | fullsync 和增量命令 | RDB-preamble AOF | command-AOF | 结果 |
|---|---:|---:|---:|---:|---:|---|
| Redis 5 -> Redis 6 | 通过 | 通过 | 通过 | 通过 | 通过 | 5/5 |
| Redis 5 -> Redis 7 | 通过 | 通过 | 通过 | 通过 | 通过 | 5/5 |
| Redis 6 -> Redis 7 | 通过 | 通过 | 通过 | 通过 | 通过 | 5/5 |

升级矩阵合计 15/15 通过。升级前后均加载 GeminiBloom，不包含 RedisBloom 与 GeminiBloom 的在线替换。

本项为纯内核验证：测试直接切换 `redis-server` 版本并加载已有 RDB/AOF 数据，不经过管控平台。管控侧的升级编排、节点调度、主备切换、状态机、超时重试、失败回滚和告警不属于本项测试范围。

## 5. 详细测试结果

### 5.1 构建与单元测试

#### 5.1.1 Docker 构建

| 测试内容 | 检查点 | 结果 |
|---|---|---|
| 分别编译 Redis 5.0.14、6.2.20、7.2.11 | `redis-server`、`redis-cli` 构建成功 | 通过 |
| 分别编译 RedisBloom 2.2.18、2.4.20、2.6.25 | 对照模块可被对应 Redis 加载 | 通过 |
| 编译 GeminiBloom 和单元测试 | 模块和测试程序生成成功 | 通过 |
| 加载 GeminiBloom | Redis 启动成功，`MODULE LIST` 可见模块 | 通过 |

#### 5.1.2 BloomLayer

| 测试内容 | 具体检查 | 结果 |
|---|---|---|
| MurmurHash2/MurmurHash64A | 固定输入 golden vector、空输入、二进制输入和 seed 行为 | 通过 |
| Hash policy | 32 位、64 位 hash pair 的确定性和精确值 | 通过 |
| Bloom 参数 | capacity、false-positive rate、bits per entry、hash count 和 bit 数 | 通过 |
| 位数组布局 | 64-bit 对齐、power-of-two mask、非 power-of-two modulo 和字节内 bit 顺序 | 通过 |
| 插入与查询 | 首次插入、重复插入、membership 和零 false negative | 通过 |
| 单层误判率 | capacity 10,000、目标误判率 0.01、100,000 个未写入查询 | 通过 |
| 资源边界 | 无效 capacity/rate、极小 rate、超大 bit 数和单层数据上限 | 通过 |
| 对象生命周期 | move、move assignment、clone 深拷贝和存储释放 | 通过 |

结果：30/30 通过。

#### 5.1.3 ScalingBloomFilter

| 测试内容 | 具体检查 | 结果 |
|---|---|---|
| 自动扩容 | 容量用尽后创建新层，已插入数据保持可查询 | 通过 |
| NONSCALING | 达到容量后拒绝新增，重复值不消耗容量 | 通过 |
| Expansion | expansion 1 使用同容量层；其他 expansion 按配置增长 | 通过 |
| 计数语义 | 每层 item count、总 CARD、总 capacity 和重复插入返回值 | 通过 |
| 动态层数组 | 层数组扩容、placement construction、move 和析构 | 通过 |
| 内存统计 | bit array、对象头和层数组存储计入 `BytesUsed()` | 通过 |
| 资源上限 | capacity、扩容乘法、总数据量和层数边界 | 通过 |
| Clone | metadata、bit array、层数和对象独立性 | 通过 |

结果：24/24 通过。

#### 5.1.4 RDB、wire 和模块回调

| 测试组 | 具体检查 | 结果 |
|---|---|---|
| RDB round-trip | 空对象、单层、多层、NONSCALING、不同 expansion 和不同误判率 | 通过 |
| metadata | capacity、rate、hash count、bits per entry、total bits、item count | 通过 |
| bit array | 序列化前后逐字节一致 | 通过 |
| SCANDUMP/LOADCHUNK wire | header、layer metadata、cursor 和完整往返 | 通过 |
| 输入校验 | 截断数据、非法 flag、NaN/Inf、长度不符、计数不符和越界字段 | 通过 |
| 模块回调 | AOF rewrite、digest、copy、free effort 和 defrag | 通过 |
| 部分加载 | AOF 输出已加载前缀、RDB 保存进度、clone 保存进度 | 通过 |
| 完整对象兼容保护 | 完整对象 RDB 编码保持原布局 | 通过 |

结果：最终测试集 76/76 通过。

### 5.2 `BF.*` 命令集成测试

#### 5.2.1 Tcl 集成测试说明

Tcl 测试是 Redis 生态常用的服务端集成测试方式。本报告中的 Tcl 测试不是对 C++ 函数的模拟调用，而是在真实 Redis 进程中加载 GeminiBloom，通过 Redis 客户端连接发送请求，并对服务端返回值和数据状态执行断言。

测试过程包括：

1. 启动独立 Redis 实例并加载 GeminiBloom；
2. 建立客户端连接，通过 Redis 协议调用模块接口；
3. 检查正常返回值、错误类型、错误文本和数组结构；
4. 查询 key、CARD、INFO、TTL 和 membership，确认服务端实际状态；
5. 对持久化场景执行保存、停止和重新启动，再次检查数据；
6. 每个测试场景结束后清理相关 key，避免场景间相互影响。

因此，Tcl 测试覆盖的是从 Redis 命令解析、模块回调、对象读写到 Redis keyspace 行为的完整链路。C++ 单元测试主要验证内部算法和序列化函数，Tcl 测试用于验证模块加载到 Redis 后的外部可见行为。

#### 5.2.2 被测接口与场景

真实 Redis 进程中执行以下 10 个模块接口：

`BF.RESERVE`、`BF.ADD`、`BF.MADD`、`BF.INSERT`、`BF.EXISTS`、`BF.MEXISTS`、`BF.INFO`、`BF.CARD`、`BF.SCANDUMP`、`BF.LOADCHUNK`。

| 测试组 | 具体测试内容 | 判定结果 |
|---|---|---|
| 创建与写入 | 显式创建、自动创建、MADD、INSERT、NOCREATE、NONSCALING | 通过 |
| 查询 | EXISTS、MEXISTS、CARD、INFO 完整字段和单字段 | 通过 |
| 参数组合 | ERROR、CAPACITY、EXPANSION 1/2/4、option 顺序 | 通过 |
| 参数错误 | 缺值、非数值、重复 option、互斥 option、未知 option、越界 | 通过 |
| 类型错误 | string key、已有 Bloom key、缺失 key 和 WRONGTYPE | 通过 |
| 容量行为 | NONSCALING 满容量、自动扩容、多层 membership | 通过 |
| 批量部分失败 | MADD/INSERT 在首个容量错误处停止，返回数组长度与 CARD 一致 | 通过 |
| 特殊 item | 空字符串、10 KiB 字符串、NUL、CRLF、0..255 二进制值 | 通过 |
| SCANDUMP/LOADCHUNK | 完整导出导入、非法 cursor、乱序、重复、错误 chunk 长度 | 通过 |
| Loading 状态 | 未完成加载时拒绝 ADD、EXISTS、INFO、CARD、SCANDUMP 等操作 | 通过 |
| 误判率 | 单层 0.01、单层 0.001 和多层 filter 的统计检查 | 通过 |

#### 5.2.3 结果

| Redis 版本 | 执行数 | 通过数 | 失败数 | 结果 |
|---|---:|---:|---:|---|
| Redis 5.0.14 | 185 | 185 | 0 | 通过 |
| Redis 6.2.20 | 185 | 185 | 0 | 通过 |
| Redis 7.2.11 | 188 | 188 | 0 | 通过 |

Redis 7 多出的 3 项为该版本支持的 module key `COPY` 行为验证。三个版本的共同测试项使用相同的业务语义和判定标准。

### 5.3 Redis 通用命令

| 类别 | 已测试命令或场景 | 检查结果 |
|---|---|---|
| 生命周期 | `DEL`、`UNLINK`、`EXISTS`、`TYPE`、`TOUCH` | key 状态和返回值正确 |
| 过期 | `EXPIRE`、`PEXPIRE`、`TTL`、`PTTL`、`PERSIST` | TTL 行为正确，过期前数据可读 |
| 重命名与复制 | `RENAME`、`RENAMENX`、`COPY` | metadata、CARD、membership 保持一致 |
| 原生序列化 | `DUMP`、`RESTORE`、`RESTORE REPLACE` | 数据和 metadata 保持一致 |
| 数据库操作 | `SELECT`、`MOVE`、`SWAPDB` | key 在目标数据库中保持可用 |
| 遍历 | `SCAN`、`KEYS`、`RANDOMKEY` | Bloom key 可被正常发现 |
| 对象信息 | `MEMORY USAGE`、`OBJECT REFCOUNT/IDLETIME/ENCODING` | 返回合法结果，不破坏对象 |
| 事务 | `MULTI/EXEC`、`DISCARD`、`WATCH` | 提交、取消和监视语义正确 |
| 脚本 | Lua `EVAL` 调用 ADD、EXISTS、MADD、INFO | 返回值和写入结果正确 |
| 命令元数据 | `COMMAND INFO`、`COMMAND GETKEYS`、ACL dry-run | key 位置和命令 flag 正确 |
| 通知 | keyspace notification | Bloom key 事件可被订阅 |

所有已执行场景通过。

### 5.4 RESP 与 RedisBloom 命令对照

#### 5.4.1 协议连接

- Redis 5 使用 RESP2。
- Redis 6、Redis 7 分别使用自动 RESP3 和强制 RESP2 执行命令对照与稳态测试。
- 两种协议下命令均可正常调用。
- GeminiBloom 保持 RESP2 形状的返回值；RedisBloom 原生 RESP3 boolean/map 形状不作为一致性要求。

#### 5.4.2 命令对照方法

GeminiBloom 和 RedisBloom 分别启动在独立 Redis 实例中，对相同命令和相同 item corpus 执行以下比较：

- 创建和自动创建返回值；
- ADD、MADD、INSERT 的逐项返回值；
- EXISTS、MEXISTS membership；
- CARD 和可比的 INFO 字段；
- NONSCALING、expansion、多层和重复 item；
- 缺失 key、WRONGTYPE 和参数错误。

结果：

| 环境 | 结果 |
|---|---:|
| Redis 5 / RedisBloom 2.2.18 | 346 通过、12 跳过 |
| Redis 6 / RedisBloom 2.4.20 | 370/370，RESP2 和 RESP3 均执行 |
| Redis 7 / RedisBloom 2.6.25 | 370/370，RESP2 和 RESP3 均执行 |

### 5.5 持久化、Redis 纯内核迁移与复制

本节中的“迁移”是 Redis 内核原生数据迁移能力验证。测试由 Redis 实例直接完成对象序列化、网络传输、目标实例恢复和源实例状态处理，不经过任何管控平台。

本项验证范围包括：

- GeminiBloom 对象能否被 Redis 内核正确识别和序列化；
- 对象能否通过 Redis 实例间的数据通道传输到目标实例；
- 迁移后 key、TTL、metadata、CARD 和 membership 是否保持一致；
- 源实例和目标实例的 key 状态是否符合 Redis 内核迁移语义。

管控侧的迁移任务创建、实例发现、拓扑变更、slot 调度、进度跟踪、超时重试、失败回滚和告警不属于本节测试范围。

#### 5.5.1 完整对象

每个 Redis 版本执行 6 个场景：

| 场景 | 操作 | 恢复后检查 | 结果 |
|---|---|---|---|
| RDB | `BGSAVE` 后停止并重启 Redis | key 数、metadata、CARD、membership | 通过 |
| DUMP/RESTORE | 导出对象并恢复到新 key，包含 TTL | TTL、metadata、CARD、membership | 通过 |
| Redis 纯内核迁移 | 由源 Redis 实例直接将 Bloom key 迁移到目标 Redis 实例 | 源/目标 key 状态、TTL、metadata、CARD、membership | 通过 |
| 主从复制 | fullsync 后继续执行 Bloom 写命令 | master/replica CARD 和 membership | 通过 |
| RDB-preamble AOF | `aof-use-rdb-preamble yes`，rewrite 后重启 | metadata、CARD、membership、日志 | 通过 |
| command-AOF | `aof-use-rdb-preamble no`，rewrite 后重启 | metadata、CARD、membership、日志 | 通过 |

测试 corpus 包含空 filter、单层、多层、NONSCALING、expansion 1/2/4、自动创建、二进制 item 和长 item。Redis 5、6、7 均为 6/6 通过。

#### 5.5.2 部分加载对象

测试先创建多层 source filter，通过 `BF.SCANDUMP` 获取 header 和 layer chunk，再构造只加载 header 或只加载部分 layer 的 target。

| 场景 | 关键时序 | 检查点 | 结果 |
|---|---|---|---|
| command-AOF header-only | rewrite fork 后写入全部剩余 chunk | base AOF 保留 header-only 状态，tail 可继续加载 | 通过 |
| command-AOF partial-prefix | rewrite fork 前已有部分 layer，fork 后写入其余 layer | restart 后 cursor 连续，无重复或乱序错误 | 通过 |
| RDB-preamble header-only | RDB preamble 生成后写入剩余 chunk | preamble 和 AOF tail 合并后数据完整 | 通过 |
| RDB-preamble partial-prefix | preamble 保存部分加载进度 | restart 后从正确 cursor 继续 | 通过 |
| 纯 RDB header-only | BGSAVE 保存 header-only 对象并重启 | 重启后仍处于 Loading，补齐 chunk 后完整 | 通过 |

每个场景检查：

- rewrite 期间 chunk 确实写入 incremental tail；
- restart 前 source/target membership；
- restart 后 Loading 状态和下一 cursor；
- 补齐 chunk 后 `BF.CARD`；
- 全部 100 个已写入 item，无 false negative；
- Redis 日志中无 AOF replay 数据错误。

结果：Redis 5、6、7 各 5/5，共 15/15 通过。实际 `COPY` 和 `DUMP/RESTORE` 补充测试也验证了部分对象能够保持 cursor 并继续加载。

### 5.6 RDB 互操作

每个 Redis 版本执行 7 条 RDB 读取路径：

1. RedisBloom -> GeminiBloom；
2. GeminiBloom -> RedisBloom；
3. GeminiBloom -> GeminiBloom；
4. RedisBloom -> RedisBloom；
5. GeminiBloom -> RedisBloom -> GeminiBloom；
6. RedisBloom -> GeminiBloom -> RedisBloom；
7. GeminiBloom/RedisBloom 四跳交替加载。

每条路径使用 16 个 filter corpus，包括 empty、single-layer、multi-layer、fixed、expansion 1/2/4、binary、long item 和 16 MiB 对象。单条路径比较 283,131 次 `EXISTS`，并检查可比的 metadata。

结果：Redis 5、6、7 均为 7/7 通过。该结果用于验证 RDB 数据可读性，不表示支持两个模块在同一 Redis 实例中同时加载。

### 5.7 Redis 纯内核大版本升级

测试在同一容器中准备 Redis 5、6、7 可执行文件，直接停止旧版本 `redis-server`，再使用新版本 `redis-server` 加载原有数据。升级前后始终加载同一 GeminiBloom 模块，对以下路径执行 Redis 5 -> 6、Redis 5 -> 7、Redis 6 -> 7：

- RDB 加载、升级后继续写入、再次保存和重启；
- DUMP/RESTORE；
- fullsync 和升级后的增量 Bloom 命令；
- RDB-preamble AOF；
- command-AOF。

每条路径比较 key 数、TTL、Capacity、filter 数、items inserted、Expansion、CARD 和 membership。结果为 15/15 通过。

该结果仅说明 Redis 内核版本切换后的模块加载和数据兼容性，不代表管控系统的大版本升级流程已经通过验证。

### 5.8 并发与稳态运行

#### 5.8.1 并发客户端

| 场景 | 负载 | 检查点 | 结果 |
|---|---|---|---|
| 共享 key 并发 | 8 个客户端并发 MADD，每客户端 1,000 个 item | 成功返回数、CARD、全量 membership | 通过 |
| 重复值并发 | 多客户端重复写入已有 item | 返回新增数为 0，CARD 不变化 | 通过 |
| 分配回收 | 反复创建、写入和删除 Bloom key | cleanup 后内存增长低于 16 MiB 门限 | 通过 |

Redis 5、6、7 均为 2/2 通过。

#### 5.8.2 300 秒稳态负载

负载持续执行创建、写入、查询、INFO、CARD、SCANDUMP、LOADCHUNK 和删除。

| Redis / 协议 | 时长 | 循环数 | 操作数 | 内存变化 | 结果 |
|---|---:|---:|---:|---:|---|
| Redis 5 / RESP2 | 300 s | 15,450 | 27,508,595 | +32 B | 通过 |
| Redis 6 / RESP3 | 300 s | 15,336 | 27,307,665 | +32 B | 通过 |
| Redis 6 / RESP2 | 300 s | 15,570 | 27,722,270 | +32 B | 通过 |
| Redis 7 / RESP3 | 300 s | 15,519 | 27,634,407 | +296,288 B | 通过 |
| Redis 7 / RESP2 | 300 s | 15,374 | 27,376,735 | +296,288 B | 通过 |

总执行时间 1,500 秒，总操作数 137,549,672。Redis 7 的内存增量发生在预热阶段，后续未持续增长；未观察到随循环累积的模块内存泄漏。

### 5.9 Sanitizer 与 Valgrind

| 工具 | 执行内容 | 验收标准 | 结果 |
|---|---|---|---|
| ASan | 124 个核心测试和真实 Redis 集成负载 | 无越界、use-after-free、double-free；单元测试启用 leak detection | 通过 |
| UBSan | 124 个核心测试和集成负载 | 无模块未定义行为报告 | 通过 |
| TSan | 独立插桩构建、核心测试、并发和持久化负载 | 无 data race 报告 | 通过 |
| Valgrind | GeminiBloom 核心测试、GeminiBloom/RedisBloom 集成负载 | `ERROR SUMMARY: 0`，无 definite leak | 通过 |

集成负载包括：

- 正常、重复和二进制 item；
- 并发写入和查询；
- 10 层扩容；
- 分配和删除循环；
- RDB save/restart；
- command-AOF rewrite/restart。

ASan 集成阶段关闭 leak detection，避免 Redis 进程退出时仍存活的全局分配影响模块判定。RedisBloom 预编译模块未进行 sanitizer 插桩，仅在 sanitizer 阶段作为功能对照；Valgrind 阶段两种实现均执行 Memcheck。

### 5.10 规模与内存

#### 5.10.1 20,000 个小 key

| 项目 | 配置或检查 |
|---|---|
| key 数 | 20,000 |
| 单 key capacity | 100 |
| 单 key 写入数 | 3 |
| membership 检查 | restart 前后各检查全部 60,000 个 item |
| metadata 检查 | key 数、CARD、INFO |

GeminiBloom 和 RedisBloom 均通过，restart 前后无 false negative。

#### 5.10.2 百万项 Bloom

| 项目 | 数值 |
|---|---:|
| capacity | 1,000,000 |
| error rate | 0.001 |
| 模式 | NONSCALING |
| 写入尝试 | 1,000,000 |
| 两种实现记录的新元素 | 999,893 |
| 已写入候选全量查询 | 1,000,000，false negative 为 0 |
| 未写入查询 | 100,000，false positive 为 97 |

restart 前后结果一致，GeminiBloom 与 RedisBloom 的逻辑结果一致。

#### 5.10.3 50,000 key 内存曲线

在 1,000、5,000、20,000 和 50,000 key 处记录进程内存、dataset memory 和线性斜率。每个 key capacity 为 100，写入 3 项。

| Redis | GeminiBloom 斜率 | RedisBloom 斜率 | 差值 | 50,000 key 总差 | 结果 |
|---|---:|---:|---:|---:|---|
| 5 | 362.425 B/key | 346.425 B/key | 16 B/key | 约 0.80 MB | 通过 |
| 6 | 362.425 B/key | 346.425 B/key | 16 B/key | 约 0.80 MB | 通过 |
| 7 | 362.704 B/key | 346.704 B/key | 16 B/key | 约 0.80 MB | 通过 |

验收门限为 GeminiBloom 相对 RedisBloom的额外成本不超过 64 B/key。三个版本均满足门限。

补充测试覆盖 capacity 1、100、1,000、10,000、100,000、1,000,000，以及 1 层、5 层、10 层对象，并检查 BGSAVE/restart 前后的 membership、metadata 和对象内存统计。

#### 5.10.4 单 key 1,000 万和 1 亿 item

每个 Redis 版本分别测试 GeminiBloom 和对应 RedisBloom。所有测试运行在 2 GiB、无额外 swap 的独立容器中。

| 规模 | Capacity | 写入尝试 | Error rate | 已写入候选全量读取 | 未写入查询 | restart 抽样 |
|---|---:|---:|---:|---:|---:|---:|
| 1,000 万 | 10,000,000 | 10,000,000 | 0.001 | 10,000,000 | 1,000,000 | 10,001 |
| 1 亿 | 100,000,000 | 100,000,000 | 0.001 | 100,000,000 | 1,000,000 | 100,001 |

三版本、两实现累计执行：

- 660,000,000 次写入尝试；
- 660,000,000 次已写入候选读取；
- 12,000,000 次未写入查询；
- 660,012 次重启后 membership 抽样。

正确性结果：

| 规模 | 记录为新元素 | False negative | False positive | Gemini/RedisBloom 逻辑比较 |
|---|---:|---:|---:|---|
| 1,000 万 | 9,998,819 | 0 | 1,030 / 1,000,000 | 一致 |
| 1 亿 | 99,987,772 | 0 | 1,026 / 1,000,000 | 一致 |

首次出现的候选值可能在插入前被 Bloom Filter 误判为已存在，因此“记录为新元素”少于写入尝试数属于 Bloom Filter 的预期近似语义。两种实现的命令返回计数和最终 CARD 完全一致。

内存结果：

| Redis | 规模 | GeminiBloom `MEMORY USAGE` | RedisBloom `MEMORY USAGE` | 差值 |
|---|---:|---:|---:|---:|
| 5 | 1,000 万 | 17,972,143 B | 17,972,127 B | +16 B |
| 5 | 1 亿 | 179,720,000 B | 179,719,984 B | +16 B |
| 6 | 1,000 万 | 17,972,144 B | 17,972,128 B | +16 B |
| 6 | 1 亿 | 179,720,000 B | 179,719,984 B | +16 B |
| 7 | 1,000 万 | 17,972,144 B | 17,972,128 B | +16 B |
| 7 | 1 亿 | 179,720,000 B | 179,719,984 B | +16 B |

亿级对象约 171.4 MiB。未观察到 32 位计数溢出、截断、异常扩容、进程崩溃、容器 OOM 或已写入数据 false negative。BGSAVE/restart 后 metadata 和抽样 membership 保持一致。

### 5.11 `maxmemory` 与逐出

每种实现创建 600 个 capacity=20,000 的 NONSCALING Bloom key，在数据集建立后设置较低 `maxmemory`，分别执行以下策略：

- `noeviction`
- `allkeys-lru`
- `allkeys-lfu`
- `allkeys-random`
- `volatile-lru`
- `volatile-lfu`
- `volatile-random`
- `volatile-ttl`

| 检查项 | 预期 | 结果 |
|---|---|---|
| noeviction 新建 key | OOM，key 不创建 | 通过 |
| noeviction 修改已有 key | OOM，metadata 不变化 | 通过 |
| allkeys 策略 | 实际逐出 Bloom key | 通过 |
| allkeys 幸存 key | seed membership 和 metadata 有效 | 通过 |
| volatile 策略 | 只逐出带 TTL 的压力 key | 通过 |
| 无 TTL sentinel | 保留且可读 | 通过 |

GeminiBloom 与 RedisBloom 比较相同的数据安全不变量，不要求逐出数量完全相等。Redis 5、6、7 均为 16/16 通过。

### 5.12 AOF 与受限内存

#### 5.12.1 负载配置

- 100 个小 Bloom key；
- 1 个 capacity=10,000,000 的 NONSCALING key；
- 大 key 写入 100,000 项；
- `aof-use-rdb-preamble no`；
- `appendfsync always`。

#### 5.12.2 cgroup 内存限制

| 容器内存 | swap | 预期 rewrite 状态 | GeminiBloom | RedisBloom | restart 检查 |
|---|---|---|---|---|---|
| 64 MiB | 禁用 | `err` | 通过 | 通过 | 旧 AOF 保留，数据完整 |
| 128 MiB | 禁用 | `ok` | 通过 | 通过 | 新 AOF 可回放，数据完整 |

64 MiB 场景中，rewrite child 被 cgroup OOM 终止，Redis 父进程保留旧 AOF。测试完成后容器被删除，宿主机未运行 Redis 进程。

#### 5.12.3 AOF callback 故障注入

测试构建通过受控分配器使 AOF callback 的指定分配点返回 `nullptr`，用于检查 callback 提前退出时 Redis 对 rewrite 文件的处理。该场景属于故障注入，不代表标准 Redis allocator 的生产行为。

验收内容包括：

- rewrite 状态；
- 生成文件大小；
- restart 后 key 数、CARD 和 membership；
- callback 不依赖临时 heap allocation 后的回归行为。

最终实现的 AOF header 使用固定上限的栈缓冲区，回归测试通过。

## 6. 兼容性边界

以下行为属于设计边界，不作为测试失败：

1. GeminiBloom 的 `SCANDUMP/LOADCHUNK` 使用 layer-index cursor 和私有 header，不与 RedisBloom 的 byte-offset 协议互通。
2. command-AOF 使用 GeminiBloom 私有的 `BF.LOADCHUNK` 表示，不能由 RedisBloom 回放。
3. RedisBloom 与 GeminiBloom 的在线模块替换不在支持范围内。
4. `BF.INFO Size` 的统计口径不同，不要求两种实现返回完全相同的数值。
5. 多层 filter 的运行时 layer capacity 不持久化，因此 `BF.INFO Size` 在 restart 前后可能不同；逻辑 metadata 和 membership 必须一致。
6. GeminiBloom 不提供 RedisBloom 原生 RESP3 boolean/map 返回形状。
7. 参数解析严格程度和部分错误文本不要求与 RedisBloom 完全一致。
8. GeminiBloom 与 RedisBloom 使用相同命令名和 RDB data type name，不能在同一 Redis 实例中同时加载。

## 7. 未覆盖范围

以下项目未纳入本次测试结论：

- Redis Cluster slot、MOVED/ASK 和 slot migration；
- Sentinel 或编排系统自动故障转移；
- 磁盘满、短写、fsync 错误和损坏 AOF/RDB；
- 任意分配点的全进程 allocator fail-N 遍历；
- Linux x86_64、其他 libc/编译器和大小端平台；
- Redis 8 内置 Bloom；
- RedisBloom 2.2/2.4/2.6 之外的版本；
- 超过 300 秒的单次稳态测试；
- 超过 50,000 个小 key；
- 超过单 key 100,000,000 item；
- `2^30` capacity 附近的完整资源曲线；
