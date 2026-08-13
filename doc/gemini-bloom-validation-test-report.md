# GeminiBloom 功能测试报告

## 文档信息

| 项目 | 内容 |
|---|---|
| 测试对象 | GeminiBloom Redis Module |
| Redis 版本 | 5.0.14、6.2.20、7.2.11 |
| 对照实现 | RedisBloom 2.2.18、2.4.20、2.6.25 |
| 执行平台 | Docker、Ubuntu 22.04、Linux ARM64 |

## 1. 测试结论

GeminiBloom 在 Redis 5.0.14、6.2.20 和 7.2.11 环境中的各项测试均通过。在本报告覆盖范围内，未发现正确性或数据安全缺陷。

1. **基本功能符合预期。** 10 个 `BF.*` 命令的创建、写入、查询、批量操作、参数校验、边界值和错误处理均符合通过标准。
2. **数据保存和恢复正常。** RDB、AOF、DUMP/RESTORE、主从复制、Redis 纯内核迁移和纯内核大版本升级后，已写入数据和对象信息保持一致，未出现数据丢失或假阴性。通过 `BF.LOADCHUNK` 分批导入但尚未完成的对象，在保存、重启或 `COPY` 后也能从正确位置继续导入。
3. **集群和故障转移正常。** Redis Cluster 的 slot 路由、`MOVED`、`ASK` 和 slot migration 测试通过；Redis Sentinel 能在主节点停止后完成新主提升，新主上的 Bloom 数据完整，并可继续写入和复制。
4. **与 RedisBloom 的可比结果一致。** 在对应 RedisBloom 版本的对照测试中，命令返回值、可比对象信息、membership、RDB 互操作结果以及规模测试结果一致。
5. **大规模和资源受限场景通过。** 测试覆盖 100,000 个小 key、单 key 2 亿 item、600 秒持续负载、8 种逐出策略、64/128 MiB 容器内存限制、磁盘满、AOF 短写和 fsync 错误。测试后数据状态均符合通过标准。
6. **运行时安全检查通过。** ASan、UBSan、TSan 和 Valgrind 未报告模块内存越界、未定义行为、数据竞争或 `definitely lost` 内存。

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
- 并发客户端、对象分配回收以及 300 秒和 600 秒稳态负载；
- ASan、UBSan、TSan 和 Valgrind；
- 100,000 个小 key、百万项 Bloom、千万/亿级单 key；
- `maxmemory`、逐出策略和受限容器 OOM；
- 部分 `BF.LOADCHUNK` 状态的持久化和恢复；
- Cluster slot、MOVED/ASK 和 slot migration；
- Sentinel 自动选主和故障转移后的增量复制；
- 磁盘满、AOF 短写、fsync 错误以及损坏 RDB/AOF；
- GeminiBloom 受控分配点 fail-N。

### 2.2 状态定义

| 状态 | 定义 |
|---|---|
| 通过 | 测试已执行，全部断言满足预期 |
| 失败 | 测试已执行，至少一个断言不满足预期 |
| 跳过 | 目标版本不提供被测能力，测试未执行 |

### 2.3 通用通过标准

除各测试项另有说明外，统一采用以下标准：

- 已写入 item 不允许出现假阴性；
- `BF.CARD`、`BF.INFO` 逻辑字段和命令返回值保持一致；
- 持久化、重启、Redis 纯内核迁移、复制或升级后，key、TTL、metadata 和 membership 保持一致；
- 非法输入必须返回错误，不得破坏已有 key；
- OOM 或逐出场景中的失败命令不得留下不完整对象或修改已有对象 metadata；
- sanitizer 和 Valgrind 不得报告模块代码错误；
- 对照测试中，除已声明的设计差异外，GeminiBloom 与 RedisBloom 的可比命令返回值和数据状态必须一致。

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
| 千万/亿级测试内存 | 1,000 万/1 亿使用 2 GiB；2 亿使用 4 GiB；swap 上限与内存上限相同 |
| AOF OOM 测试内存 | 64 MiB、128 MiB，禁止额外 swap |
| 临时数据 | 测试结束后随容器或临时目录删除 |
| 扩展故障测试 | 无宿主机端口映射，网络隔离，512 MiB 至 4 GiB cgroup 内存上限 |
| 磁盘满介质 | 容器内 12 MiB tmpfs，不使用宿主机持久卷 |

64/128 MiB AOF OOM 测试在执行前校验 cgroup 内存限制；未设置限制或限制超过 256 MiB 时不执行该场景。

## 4. 测试结果汇总

### 4.1 Redis 版本矩阵

| 测试类别 | Redis 5 | Redis 6 | Redis 7 | 结果 |
|---|---:|---:|---:|---|
| Docker 构建 | 通过 | 通过 | 通过 | 通过 |
| BloomLayer 单元测试 | 31/31 | 31/31 | 31/31 | 通过 |
| ScalingBloomFilter 单元测试 | 26/26 | 26/26 | 26/26 | 通过 |
| RDB/wire/AOF 单元测试 | 77/77 | 77/77 | 77/77 | 通过 |
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
| 单 key 1,000 万/1 亿/2 亿 | 通过 | 通过 | 通过 | 通过 |
| Cluster slot 与 slot migration | 通过 | 通过 | 通过 | 通过 |
| Sentinel 自动故障转移 | 通过 | 通过 | 通过 | 通过 |
| 磁盘与持久化故障 | 通过 | 通过 | 通过 | 通过 |
| GeminiBloom 受控分配点 fail-N | 通过 | 通过 | 通过 | 通过 |
| 600 秒稳态负载 | 通过 | 通过 | 通过 | 通过 |
| 100,000 个小 key | 通过 | 通过 | 通过 | 通过 |

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
| 插入与查询 | 首次插入、重复插入、membership 和假阴性检查 | 通过 |
| 单层误判率 | capacity 10,000、目标误判率 0.01、100,000 个未写入查询 | 通过 |
| 资源边界 | 无效 capacity/rate、极小 rate、超大 bit 数和单层数据上限 | 通过 |
| 对象生命周期 | move、move assignment、clone 深拷贝和存储释放 | 通过 |

结果：31/31 通过。

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

结果：26/26 通过。

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

结果：77/77 通过。

### 5.2 `BF.*` 命令集成测试

#### 5.2.1 Tcl 集成测试说明

Tcl 测试是 Redis 服务端集成测试方式。本报告中的 Tcl 测试不模拟 C++ 函数调用，而是在实际 Redis 服务进程中加载 GeminiBloom，通过客户端连接发送请求，并对服务端返回值和数据状态执行断言。

测试过程包括：

1. 启动独立 Redis 实例并加载 GeminiBloom；
2. 建立客户端连接，通过 Redis 协议调用模块接口；
3. 检查正常返回值、错误类型、错误文本和数组结构；
4. 查询 key、CARD、INFO、TTL 和 membership，确认服务端实际状态；
5. 对持久化场景执行保存、停止和重新启动，再次检查数据；
6. 每个测试场景结束后清理相关 key，避免场景间相互影响。

Tcl 测试覆盖 Redis 命令解析、模块回调、对象读写和 keyspace 行为。C++ 单元测试验证内部算法与序列化函数，Tcl 测试验证模块加载后的外部接口行为。

#### 5.2.2 被测接口与场景

测试在实际 Redis 服务进程中执行以下 10 个模块接口：

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

Redis 7 多出的 3 项用于验证该版本支持的 module key `COPY` 行为。三个版本的共同测试项采用相同的测试条件和判定标准。

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

以上场景均通过。

### 5.4 RESP 与 RedisBloom 命令对照

#### 5.4.1 协议连接

- Redis 5 使用 RESP2。
- Redis 6、Redis 7 分别使用自动 RESP3 和强制 RESP2 执行命令对照与稳态测试。
- 两种协议下命令均可正常调用。
- GeminiBloom 保持 RESP2 形状的返回值；RedisBloom 原生 RESP3 boolean/map 形状不作为一致性要求。

#### 5.4.2 命令对照方法

GeminiBloom 和 RedisBloom 分别加载到独立 Redis 实例，对相同命令和相同 item 数据集执行以下比较：

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

测试数据集包含空 filter、单层、多层、NONSCALING、expansion 1/2/4、自动创建、二进制 item 和长 item。Redis 5、6、7 均为 6/6 通过。

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
- 全部 100 个已写入 item 均无假阴性；
- Redis 日志中无 AOF replay 数据错误。

结果：Redis 5、6、7 各 5/5，共 15/15 通过。`COPY` 和 `DUMP/RESTORE` 测试同时验证了部分对象能够保持 cursor 并继续加载。

### 5.6 RDB 互操作

每个 Redis 版本执行 7 条 RDB 读取路径：

1. RedisBloom -> GeminiBloom；
2. GeminiBloom -> RedisBloom；
3. GeminiBloom -> GeminiBloom；
4. RedisBloom -> RedisBloom；
5. GeminiBloom -> RedisBloom -> GeminiBloom；
6. RedisBloom -> GeminiBloom -> RedisBloom；
7. GeminiBloom 与 RedisBloom 连续四次交替加载。

每条路径使用包含 16 个 filter 的数据集，覆盖 empty、single-layer、multi-layer、fixed、expansion 1/2/4、binary、long item 和 16 MiB 对象。单条路径比较 283,131 次 `EXISTS`，并检查可比的 metadata。

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
| 分配回收 | 反复创建、写入和删除 Bloom key | 清理完成后内存增长低于 16 MiB 门限 | 通过 |

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

#### 5.8.3 600 秒稳态负载

每个 Redis 版本在独立容器内连续运行 600 秒。负载循环覆盖 8 组 capacity、error rate、expansion 和 NONSCALING 组合，并持续执行创建、单条和批量写入、命中查询、CARD/INFO 一致性检查、SCANDUMP/LOADCHUNK 往返及删除。每轮均检查已写入 item 和导入副本不存在假阴性。

| 实现 | Redis / 协议 | 时长 | 循环数 | 操作数 | 初始/结束内存 | 结果 |
|---|---|---:|---:|---:|---:|---|
| GeminiBloom | Redis 5 / RESP2 | 600 s | 54,362 | 96,796,275 | 886,648 / 886,680 B | 通过 |
| GeminiBloom | Redis 6 / RESP3 | 600 s | 53,862 | 95,909,430 | 908,192 / 908,224 B | 通过 |
| GeminiBloom | Redis 7 / RESP3 | 600 s | 55,142 | 98,188,630 | 1,033,760 / 1,358,720 B | 通过 |
| RedisBloom | Redis 5 / RESP2 | 600 s | 32,310 | 57,404,160 | 892,336 / 892,368 B | 通过 |
| RedisBloom | Redis 6 / RESP3 | 600 s | 32,399 | 57,691,357 | 928,952 / 926,936 B | 通过 |
| RedisBloom | Redis 7 / RESP3 | 600 s | 31,809 | 56,638,911 | 1,066,520 / 1,361,064 B | 通过 |

GeminiBloom 三组累计运行 1,800 秒，执行 290,894,335 次操作。Redis 7 的内存在预热后处于约 1.33 MiB 至 1.38 MiB 区间内波动，未随循环次数线性增长。

RedisBloom 对照组使用相同的 600 秒负载和资源上限。RedisBloom 2.2.18 不支持 `BF.CARD` 和单字段 `BF.INFO`，该环境使用完整 `BF.INFO` 中的 item count 完成等价一致性校验。

### 5.9 Sanitizer 与 Valgrind

| 工具 | 执行内容 | 验收标准 | 结果 |
|---|---|---|---|
| ASan | 124 个核心测试和 Redis 服务集成负载 | 无越界、use-after-free、double-free；单元测试启用 leak detection | 通过 |
| UBSan | 124 个核心测试和集成负载 | 无模块未定义行为报告 | 通过 |
| TSan | 独立插桩构建、核心测试、并发和持久化负载 | 无 data race 报告 | 通过 |
| Valgrind | GeminiBloom 核心测试、GeminiBloom/RedisBloom 集成负载 | `ERROR SUMMARY: 0`，无 `definitely lost` 内存 | 通过 |

集成负载包括：

- 普通 item、重复 item 和二进制 item；
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

GeminiBloom 和 RedisBloom 均通过，restart 前后无假阴性。

#### 5.10.2 100,000 个小 key

Redis 5、6、7 分别对 GeminiBloom 和对应 RedisBloom 创建 100,000 个 capacity=100 的 Bloom key，每个 key 写入 3 个 item；同一实例还创建 1 个百万项 Bloom key。保存并重启前后遍历全部小 key，逐 key 检查 3 个已写入 item，并核对数据库 key 数为 100,001。

六组执行均通过。未出现创建失败、key 数量不符、重启后 key 丢失或已写入 item 假阴性。GeminiBloom 与 RedisBloom 的 key 数、百万项 Bloom 的可比 `BF.INFO` 字段和查询结果一致。

#### 5.10.3 百万项 Bloom

| 项目 | 数值 |
|---|---:|
| capacity | 1,000,000 |
| error rate | 0.001 |
| 模式 | NONSCALING |
| 写入尝试 | 1,000,000 |
| 两种实现记录的新元素 | 999,893 |
| 已写入候选全量查询 | 1,000,000，假阴性为 0 |
| 未写入查询 | 100,000，假阳性为 97 |

restart 前后的 `BF.INFO` 字段、假阳性计数和 membership 保持一致；GeminiBloom 与 RedisBloom 的对应结果一致。

#### 5.10.4 50,000 key 内存曲线

在 1,000、5,000、20,000 和 50,000 key 处记录进程内存、dataset memory 和线性斜率。每个 key capacity 为 100，写入 3 项。

| Redis | GeminiBloom 斜率 | RedisBloom 斜率 | 差值 | 50,000 key 总差 | 结果 |
|---|---:|---:|---:|---:|---|
| 5 | 362.425 B/key | 346.425 B/key | 16 B/key | 约 0.80 MB | 通过 |
| 6 | 362.425 B/key | 346.425 B/key | 16 B/key | 约 0.80 MB | 通过 |
| 7 | 362.704 B/key | 346.704 B/key | 16 B/key | 约 0.80 MB | 通过 |

验收门限为 GeminiBloom 相对 RedisBloom 的额外成本不超过 64 B/key。三个版本均满足门限。

补充测试覆盖 capacity 1、100、1,000、10,000、100,000、1,000,000，以及 1 层、5 层、10 层对象，并检查 BGSAVE/restart 前后的 membership、metadata 和对象内存统计。

#### 5.10.5 单 key 1,000 万、1 亿和 2 亿 item

每个 Redis 版本分别测试 GeminiBloom 和对应 RedisBloom。1,000 万和 1 亿 item 使用 2 GiB 容器；2 亿 item 使用 4 GiB 容器。容器内存和 swap 上限相同，不允许超出 cgroup 限制。

| 规模 | Capacity | 写入尝试 | Error rate | 已写入候选全量读取 | 未写入查询 | restart 抽样 |
|---|---:|---:|---:|---:|---:|---:|
| 1,000 万 | 10,000,000 | 10,000,000 | 0.001 | 10,000,000 | 1,000,000 | 10,001 |
| 1 亿 | 100,000,000 | 100,000,000 | 0.001 | 100,000,000 | 1,000,000 | 100,001 |
| 2 亿 | 200,000,000 | 200,000,000 | 0.001 | 200,000,000 | 1,000,000 | 200,001 |

三版本、两实现累计执行：

- 1,860,000,000 次写入尝试；
- 1,860,000,000 次已写入候选读取；
- 18,000,000 次未写入查询；
- 1,860,018 次重启后 membership 抽样。

正确性结果：

| 规模 | 记录为新元素 | 假阴性 | 假阳性 | Gemini/RedisBloom 逻辑比较 |
|---|---:|---:|---:|---|
| 1,000 万 | 9,998,819 | 0 | 1,030 / 1,000,000 | 一致 |
| 1 亿 | 99,987,772 | 0 | 1,026 / 1,000,000 | 一致 |
| 2 亿 | 199,975,865 | 0 | 1,015 / 1,000,000 | 一致 |

首次出现的候选值可能在插入前被 Bloom Filter 误判为已存在，因此“记录为新元素”少于写入尝试数属于 Bloom Filter 的预期近似语义。两种实现的命令返回计数和 `BF.INFO` item count 完全一致；支持 `BF.CARD` 的版本中，CARD 也与该计数一致。

内存结果：

| Redis | 规模 | GeminiBloom `MEMORY USAGE` | RedisBloom `MEMORY USAGE` | 差值 |
|---|---:|---:|---:|---:|
| 5 | 1,000 万 | 17,972,143 B | 17,972,127 B | +16 B |
| 5 | 1 亿 | 179,720,000 B | 179,719,984 B | +16 B |
| 5 | 2 亿 | 359,439,848 B | 359,439,832 B | +16 B |
| 6 | 1,000 万 | 17,972,144 B | 17,972,128 B | +16 B |
| 6 | 1 亿 | 179,720,000 B | 179,719,984 B | +16 B |
| 6 | 2 亿 | 359,439,848 B | 359,439,832 B | +16 B |
| 7 | 1,000 万 | 17,972,144 B | 17,972,128 B | +16 B |
| 7 | 1 亿 | 179,720,000 B | 179,719,984 B | +16 B |
| 7 | 2 亿 | 359,439,848 B | 359,439,832 B | +16 B |

2 亿 item 对象约 342.8 MiB。未观察到 32 位计数溢出、截断、异常扩容、进程崩溃、容器 OOM 或已写入数据假阴性。BGSAVE/restart 后 metadata 和抽样 membership 保持一致。

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
| allkeys 幸存 key | 种子 item 的 membership 和 metadata 有效 | 通过 |
| volatile 策略 | 只逐出带 TTL 的压力 key | 通过 |
| 无 TTL 基准 key | 保留且可读 | 通过 |

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
- 故障注入后的 AOF 输出完整性。

结果：故障注入后的 AOF 输出、rewrite 状态和 restart 数据检查均通过。

### 5.13 Redis Cluster

Redis 5、6、7 分别建立 3 节点 Cluster，完整分配 16,384 个 slot。GeminiBloom 和对应 RedisBloom 使用相同数据与判定标准分别执行。

| 测试阶段 | 具体检查 | 结果 |
|---|---|---|
| Slot 路由 | 在非归属节点访问 Bloom key，确认返回 `MOVED` 和正确 slot | 通过 |
| 迁移准备 | 目标节点进入 IMPORTING，源节点进入 MIGRATING | 通过 |
| Key 迁移 | 通过 Redis 内核迁移包含 1,000 个 item 的 Bloom key | 通过 |
| ASK 路由 | 源节点返回 `ASK`；目标节点在 ASKING 后可读取全部 item | 通过 |
| Slot 归属切换 | 三节点更新 slot owner，原节点返回 `MOVED` | 通过 |
| 迁移后读写 | 目标节点无假阴性，可继续写入并保持 CARD 一致 | 通过 |

三版本、两实现全部通过。RedisBloom 2.2.18 不提供 `BF.CARD`，该对照环境通过全量 membership 和迁移后新增 item 验证数据状态。该项验证 Redis Cluster 内核路由和 slot migration，不包含管控系统的 slot 规划、批量调度、限速、重试和回滚。

### 5.14 Sentinel 自动故障转移

每组环境包含 1 个主节点、2 个副本和 3 个 Sentinel，quorum 为 2。主节点创建包含 2,000 个 item 的 Bloom key，确认数据到达两个副本后终止主进程，由 Sentinel 自动选举并提升新主。

| 检查项 | 验收标准 | 结果 |
|---|---|---|
| 初始复制 | 两个副本均包含全部 2,000 个 item | 通过 |
| 自动判定与提升 | Sentinel 返回新的主节点地址，对应节点角色为 master | 通过 |
| 提升后数据 | 新主全部 membership 命中，CARD 保持一致 | 通过 |
| 提升后写入 | 新主可接受新增 item | 通过 |
| 增量复制 | 至少一个存活副本收到故障转移后的新增 item | 通过 |

Redis 5、6、7 的 GeminiBloom 和 RedisBloom 对照组均通过。RedisBloom 2.2.18 不提供 `BF.CARD`，该对照环境使用 `BF.INFO` item count 和全量 membership 验证提升后数据。本项执行的是 Redis Sentinel 自动故障转移；未使用外部编排系统。

### 5.15 磁盘与持久化故障

所有故障均限制在临时容器内。磁盘满使用 12 MiB tmpfs；短写和 fsync 错误仅注入 Redis 进程打开的 AOF 文件描述符，不修改宿主机文件系统。

| 故障 | 测试方法 | 验收标准 | 结果 |
|---|---|---|---|
| 磁盘满 | 在受限 tmpfs 中保存包含不可压缩数据和 Bloom key 的 RDB | BGSAVE 返回失败；Redis 仍可响应；内存中的 5,000 个 item 全部可查 | 通过 |
| AOF 短写 | 对一次 AOF `write` 返回部分写入长度 | Redis 处理错误后可恢复写入；重启不丢失已确认数据 | 通过 |
| fsync 错误 | 对一次 AOF `fsync`/`fdatasync` 返回 `EIO` | Redis 保持运行或按版本行为退出；恢复后不丢失已确认数据 | 通过 |
| 损坏 RDB | 保存后翻转 RDB 中部字节 | Redis 启动拒绝加载损坏文件 | 通过 |
| 损坏 AOF | 完成 AOF rewrite 后截断数据文件尾部 | 在禁止 truncated-AOF 宽松恢复时，Redis 启动拒绝加载 | 通过 |

Redis 5、6、7 的 GeminiBloom 和 RedisBloom 对照组均通过。fsync `EIO` 后，Redis 5 在本次环境中保持运行；Redis 6、7 主动退出并由测试重新启动，两条路径恢复后均保留全部已确认 Bloom 数据。

### 5.16 分配失败注入

测试构建中的 GeminiBloom 分配包装器支持令第 N 次 `RMAlloc`、`RMCalloc` 或 `RMRealloc` 返回空指针。测试逐点覆盖以下可恢复路径：

| 路径 | Fail-N 范围 | 一致性检查 | 结果 |
|---|---|---|---|
| BloomLayer 创建 | 位数组分配 | 创建失败且不返回初始化不完整的 layer | 通过 |
| ScalingBloomFilter 创建 | 层数组和首层位数组 | 对象保持可析构的无效状态 | 通过 |
| 自动扩容 | 新层数组和新层位数组 | 原有 item 无假阴性，清除故障后可重试扩容 | 通过 |
| 多层 Clone | 对象、层数组和每层位数组的全部 N 值 | clone 失败不修改 source；越过最后分配点后 clone 完整 | 通过 |
| AOF rewrite callback | callback 受控分配点 | AOF 输出保持完整 | 通过 |

同一测试集在 Redis 5、6、7 构建环境中分别执行。每个环境包含 31 项 BloomLayer、26 项 ScalingBloomFilter 和 77 项 RDB/wire/AOF 测试，共 134 项；三环境累计执行 402 项，均通过。

生产构建使用 RedisModule allocator。Redis 5、6、7 的底层分配器在内存耗尽时执行进程级 OOM 处理，不保证向模块返回可恢复的空指针。因此，本项结论限定为 GeminiBloom 受控分配路径；未将 LD_PRELOAD 随机拦截 Redis 全进程分配的结果计为有效的“任意分配点全进程 fail-N”。该做法会同时破坏 Redis 内核、模块框架、日志和客户端相关分配，不能可靠归因到 GeminiBloom。

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
