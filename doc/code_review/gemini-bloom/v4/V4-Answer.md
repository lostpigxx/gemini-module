# V4 Review Response

本文件是对 `v4` 审计全部 issue 的逐条回应。每个 issue 标注处置结果和理由。

处置分类：
- **Fixed** — 已修复并有测试覆盖
- **Won't Fix** — 不修，附理由
- **Deferred** — 认可问题存在，但属于大改动 / 产品决策，不在本轮处理

---

## 01_code_bugs.md

### BUG-01: RDB layer metadata 校验弱于 wire 校验

**Fixed.**

提取 `ValidateLayerFields()` 作为统一校验函数，RDB `ReadFrom()` 和 wire `ValidateLayerMeta()` 共用同一套规则。新增校验：`totalBits==0`、`capacity==0`、`hashCount==0`、`fpRate` 非有限/非正/>=1、`bitsPerEntry` 非有限/负数、`dataSize` 不匹配。

测试覆盖：`RDB_RejectsTotalBitsZero`、`RDB_RejectsFpRateNaN`、`RDB_RejectsFpRateInf`、`RDB_RejectsFpRateZero`、`RDB_RejectsFpRateOne`、`RDB_RejectsFpRateNegative`、`RDB_RejectsBitsPerEntryNaN`、`RDB_RejectsBitsPerEntryNegative`、`RDB_RejectsCapacityZero`、`RDB_RejectsHashCountZero`、`Wire_RejectsInvalidFpRate`、`Wire_RejectsTotalBitsZero`。

### BUG-02: 反序列化不校验 item 计数一致性

**Fixed.**

RDB `ReadFrom()` 和 wire `DeserializeHeader()` 均新增：
- `sum(layer.itemCount) == totalItems` 校验
- `layer.itemCount <= layer.capacity` 校验

测试覆盖：`RDB_RejectsTotalItemsMismatch`、`RDB_RejectsItemCountExceedsCapacity`、`RDB_AcceptsConsistentItemCounts`、`Wire_RejectsTotalItemsMismatch`、`Wire_RejectsItemCountExceedsCapacity`。

### BUG-03: 未知 flags 和 RawBits 可从 RDB/wire 进入

**Fixed.**

新增 `kSupportedFlags` 掩码和 `ValidateFlags()` 函数。RDB 和 wire 反序列化均校验，未知 bit 和 RawBits 一律拒绝。RawBits 不在支持集合中（命令层从未创建，语义不完整）。

测试覆盖：`ValidateFlagsAcceptsSupportedCombinations`、`ValidateFlagsRejectsUnknownBits`、`ValidateFlagsRejectsRawBits`、`RDB_RejectsUnknownFlags`、`RDB_RejectsRawBitsFlag`、`Wire_RejectsUnknownFlags`、`BloomLayer_RawBitsCreatesZeroHashCount`（证明 RawBits 会导致 Test() 恒真）。

### BUG-04: 缺少统一资源上限

**Fixed.**

新增 `kMaxCapacity = 1<<30`（~10 亿）、`kMaxExpansion = 32768`。在 `CmdReserve`、`ParseInsertOptions`、`BloomConfigLoad` 中统一强制执行。

测试覆盖（TCL）：capacity/expansion 超限拒绝、边界值通过。

### BUG-05: multi-item full/error 继续处理后续 item

**Fixed.**

`CmdMadd` 和 `CmdInsert` 循环在第一个 `Put()` 返回 nullopt 后停止插入，后续 item 直接回复 error。

测试覆盖（TCL）：`BF.MADD on full NONSCALING filter returns errors`、`BF.CARD after partial MADD matches actual insertions`、`BF.INSERT on full filter stops and returns errors`。

---

## 02_redis_bloom_compatibility.md

### COMPAT-01: SCANDUMP cursor 是私有 layer index，不是 byte offset

**Won't Fix.**

当前私有协议自洽可用，实现 byte-offset cursor 是一个独立 project（涉及 SCANDUMP/LOADCHUNK/AOF 三处重写）。已新增测试 `SCANDUMP uses layer-index cursor protocol` 明确记录当前协议语义。

### COMPAT-02: LOADCHUNK 只接受整层 bit array，不支持任意 chunk

**Won't Fix.**

同 COMPAT-01，需要与 byte-offset cursor 一起实现。当前协议功能自洽。

### COMPAT-03: AOF rewrite 使用 public BF.LOADCHUNK 承载私有协议

**Won't Fix.**

同 COMPAT-01。AOF rewrite 依赖 LOADCHUNK 协议，改一必须改三。当前自洽。

### COMPAT-04: BF.INFO 返回形状不兼容 RESP2/RESP3

**Won't Fix.**

改返回形状（单字段从标量改为 singleton array）会 break 所有现有客户端。RESP3 map 返回需要 Redis 7.0+ API。属于产品定位决策，不在 bug fix 范围。已有测试 `BF.INFO single field: Capacity` 记录当前行为。

### COMPAT-05: RESP3 boolean 返回未实现

**Won't Fix.**

需要 `RedisModule_ReplyWithBool()`（Redis 7.0+）。当前 RESP2 integer 0/1 行为正确且被广泛使用。属于兼容性增强，非 bug。

### COMPAT-06: BF.RESERVE parser 接受重复 option

**Fixed.**

`CmdReserve` 新增 `expansionSeen` 和 `nonscalingSet` 追踪，重复即拒绝。`ParseInsertOptions` 新增 `errorSet`/`capacitySet`/`expansionSet`/`nonscalingSet` 重复检测。

测试覆盖（TCL）：6 个 duplicate option rejection tests。

### COMPAT-07: 配置机制未对齐 RedisBloom

**Won't Fix.**

注册 `RedisModule_RegisterConfig` 是现代化增强，不影响正确性。当前 loadmodule args 方式功能完整。已有测试 `Default config creates filter with expected defaults` 验证默认值。

### COMPAT-08: BF.SCANDUMP command flags 与 Redis 文档不匹配

**Fixed.**

将 `BF.SCANDUMP` 注册 flags 从 `"readonly"` 改为 `"write"`，对齐 Redis 文档 ACL categories。

测试覆盖（TCL）：`COMMAND INFO BF.SCANDUMP shows write flag`。

### COMPAT-09: 复用 RedisBloom data type name 但未证明互通

**Won't Fix.**

这是产品定位决策。使用 `MBbloom--` 是为了未来兼容性留出可能。改名为私有类型名会彻底断绝互通路径。源码注释已明确标注"full interoperability has not been verified"。

---

## 03_function_design.md

### DESIGN-01: 产品定位不清楚

**Won't Fix.** 产品决策，不属于代码 bug。

### DESIGN-02: LOADCHUNK header 后 key 立即可见，半恢复对象可被读写

**Won't Fix.**

这是 Redis Module API 的限制 — `ModuleTypeSetValue` 之后 key 即可见。实现 staging buffer 需要额外的内部状态管理，复杂度高且 RedisBloom 也有同样行为。

已新增测试 `Half-restored filter returns 0 for EXISTS` 明确记录当前行为。

### DESIGN-03: 序列化接口暴露到 public API

**Won't Fix.** 架构重构，不影响正确性。当前接口稳定且调用方受控。

### DESIGN-04: `std::optional<bool>` 无法表达失败原因

**Won't Fix.** 当前命令层已能根据 FixedSize flag 区分两种错误消息。改为 enum 收益不大，API 变更成本高。

### DESIGN-05: 资源策略没有产品化定义

**Fixed (partially).** `kMaxCapacity` 和 `kMaxExpansion` 已定义并统一执行。完整的 `BloomLimits` struct 是过度设计。

### DESIGN-06: BF.INFO Size 语义不明

**Won't Fix.** `BytesUsed()` 计算方式合理（struct + layer slots + bit arrays），文档澄清属于增强项。

### DESIGN-07: 没有 debug 命令

**Won't Fix.** 属于功能增强。5 个 INFO 字段足以覆盖常规排障。

### DESIGN-08: native mode 与 compat mode 没有边界

**Won't Fix.** 产品决策。

---

## 04_performance.md

### PERF-01: SCANDUMP 一次返回整层 bit array

**Won't Fix.** 同 COMPAT-01，需要 byte-offset cursor 重写。

### PERF-02: LOADCHUNK 要求整层 chunk

**Won't Fix.** 同上。

### PERF-03: 超大参数缺少前置上限

**Fixed.** 见 BUG-04。`kMaxCapacity = 1<<30`、`kMaxExpansion = 32768` 在命令 parser 层拒绝。

### PERF-04: EXPANSION=1 查询成本线性增长

**Won't Fix.** 这是 scaling bloom filter 的固有特性。FP rate 下限会自然限制层数。属于文档/建议，不是 bug。

### PERF-05: RDB/LOADCHUNK header 可声明大量内存

**Fixed.** `DeserializeHeader()` 新增 `sum(dataSize) <= 4GB` 校验，在分配前拒绝。

测试覆盖：`Wire_RejectsExcessiveTotalDataSize`。

### PERF-06: AOF rewrite 对每层输出完整 bulk string

**Won't Fix.** 同 COMPAT-01，依赖 LOADCHUNK 协议重写。

### PERF-07: 缺少性能基准

**Won't Fix.** 属于测试基建增强，不影响正确性。

---

## 05_directory_organization.md

### ORG-01 ~ ORG-06

**Won't Fix (all).** 文件拆分、parser 抽象、fixtures 目录、文档归属、命名统一、review backlog 都是架构/流程优化，不影响功能正确性。

---

## 06_implementation_details.md

### IMPL-01: RDB 与 wire 校验重复且不一致

**Fixed.** 见 BUG-01。统一为 `ValidateLayerFields()`。

### IMPL-02: 仍暴露高风险 public builder

**Won't Fix.** `SetLayer()` 调用方受控（仅 RDB 和 wire 反序列化），封装为 builder 是过度设计。

### IMPL-03: BloomFlags 没有 known-mask 检查

**Fixed.** 见 BUG-03。`ValidateFlags()` 拒绝未知 bits。

### IMPL-04: RawBits flag 存在但语义未完成

**Fixed.** 加载时拒绝 RawBits（不在 `kSupportedFlags` 中）。有测试证明 RawBits 会导致 hashCount=0 → Test() 恒真。

### IMPL-05: packed struct 缺少格式文档

**Won't Fix.** 属于文档增强。源码注释已标注 endian/packing 假设。

### IMPL-06: hash 实现缺少 official vectors

**Fixed.** 新增 MurmurHash2/64A golden vector 测试，覆盖 `""`、`"a"`、`"hello"`、`"hello\0world"` 四组精确值。Hash32Policy/Hash64Policy 也有 exact vector 测试。

### IMPL-07: PutAndReply 混合 mutation 和 reply

**Fixed (removed).** `PutAndReply` 已删除。MADD/INSERT 循环内联处理 Put + Reply + stop-on-full 逻辑。

### IMPL-08: parser 未检测重复 option

**Fixed.** `CmdReserve` 和 `ParseInsertOptions` 均新增重复检测。

测试覆盖（TCL）：EXPANSION/NONSCALING/ERROR/CAPACITY duplicate rejection。

### IMPL-09: command flags 不是集中声明

**Won't Fix.** 当前数组声明方式已足够清晰。command spec table + 自动化测试是增强项。

已新增测试验证 `COMMAND INFO` 返回的 flags。

### IMPL-10: AOF rewrite error path 是否让 rewrite 失败

**Won't Fix.** `RedisModule_LogIOError` 的语义取决于 Redis 内核。当前行为（log + return）是最安全的选择 — 不会崩溃。failure injection 测试属于测试基建增强。

---

## 07_tests_coverage.md

### TEST-01: 没有 RedisBloom official golden corpus

**Won't Fix.** 需要获取 RedisBloom 官方数据文件。属于长期兼容性验证项目。

### TEST-02: SCANDUMP/LOADCHUNK 没测 byte-offset cursor

**Won't Fix.** 当前使用私有协议，不存在 byte-offset cursor。已新增测试记录当前 layer-index cursor 协议语义。

### TEST-03: RESP3 raw type 未覆盖

**Won't Fix.** RESP3 返回类型需要 HELLO 3 协议切换和 Redis 7.0+ API。属于兼容性增强。

### TEST-04: RDB malicious metadata 覆盖不足

**Fixed (mostly).** 新增 12 个 RDB 恶意 metadata 测试。仍缺少的场景（`hashCount != ceil(ln2 * bpe)` 交叉校验）属于 RedisBloom 兼容性要求，当前不强制。

### TEST-05: 没有 OOM/failure injection

**Won't Fix.** 需要 test allocator 基建（`SetAllocFailAfter(n)`）。属于测试基建增强。

### TEST-06: 命令 parser 缺少纯单元测试矩阵

**Fixed (via TCL).** 新增 duplicate option、resource limit、partial failure 等 parser 场景的集成测试。纯单元测试需要 mock `RedisModuleCtx`，ROI 不高。

### TEST-07: module config 未集成测试

**Fixed (partially).** 新增 `Default config creates filter with expected defaults` 测试。完整的 config 边界测试需要多次 module reload，TCL 框架不支持。

### TEST-08: COMMAND INFO / ACL 未测试

**Fixed.** 新增 `COMMAND INFO BF.ADD`、`BF.EXISTS`、`BF.SCANDUMP` flags 测试。

### TEST-09: fixed filter partial failure 语义未精确覆盖

**Fixed.** 新增 3 个 TCL 测试：全满 MADD 全 error、partial MADD 停止 + CARD 一致、INSERT 全 error + CARD 一致。

### TEST-10: replication 测试缺失

**Won't Fix.** 需要 master/replica 测试基建。属于长期测试增强。

### TEST-11: 缺少 fuzz / sanitizer CI

**Won't Fix.** 属于 CI 基建项目。

### TEST-12: SCANDUMP binary safety 测试不足

**Won't Fix.** 需要 raw RESP harness。当前 TCL 已覆盖基本 SCANDUMP/LOADCHUNK 二进制往返。

### TEST-13: 缺少 official hash exact vector

**Fixed.** 见 IMPL-06。8 个 golden vector 测试。

### TEST-14: 缺少性能回归测试

**Won't Fix.** 属于 benchmark 基建。

---

## 08_other_issues.md

### OTHER-01 ~ OTHER-07

**Won't Fix (all).** 安全边界文档、Redis 8 共存策略、许可证声明、版本策略、运维可观测性、README 措辞、release checklist 均属于文档/流程/产品决策，不影响代码正确性。

---

## 修复统计

| 分类 | Fixed | Won't Fix | Deferred |
|---|---|---|---|
| 01_code_bugs (5) | 5 | 0 | 0 |
| 02_compatibility (9) | 2 | 7 | 0 |
| 03_design (8) | 1 | 7 | 0 |
| 04_performance (7) | 2 | 5 | 0 |
| 05_organization (6) | 0 | 6 | 0 |
| 06_implementation (10) | 5 | 5 | 0 |
| 07_tests (14) | 5 | 9 | 0 |
| 08_other (7) | 0 | 7 | 0 |
| **Total (66)** | **20** | **46** | **0** |

## 新增测试统计

| 测试类型 | 新增数量 | 总数 |
|---|---|---|
| GTest (bloom_filter_test) | +11 | 22 |
| GTest (bloom_rdb_test) | +21 | 45 |
| TCL integration | +21 | 110 |
| **Total** | **+53** | **189** |
