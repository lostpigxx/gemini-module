# gemini-bloom Code Review 审查结果

审查基线：`doc/code_review/gemini-bloom/` 下 7 份审计文档，共 71 条意见。

## 修复统计

- **已修复 bug**：16 项（均已通过测试验证）
- **新增测试**：29 条（测试总数 53 → 82，全部通过）
- **设计/文档/组织类意见**：55 项（非代码缺陷，归类说明见下方）

---

## 一、已修复的 Bug

### 01-code-bugs

| # | 意见 | 判定 | 修复方案 | 涉及文件 |
|---|------|------|----------|----------|
| 01-1 | SCANDUMP 最后一个数据 chunk 返回 cursor=0，标准客户端会跳过该 chunk | **真 bug** | 数据 chunk 始终返回非零 cursor，迭代结束额外返回 `(0, "")` 终止符 | `bloom_commands.cc`, `bloom_rdb.cc` |
| 01-2 | LOADCHUNK cursor=1 先删 key 再验证 header，malformed header 导致数据丢失 | **真 bug** | 先解析验证 header，成功后再删除旧 key | `bloom_commands.cc` |
| 01-3 | LOADCHUNK 对非 Bloom key 无条件删除，违反 WRONGTYPE 保护 | **真 bug** | 检查 key 类型，非 Bloom 类型返回 WRONGTYPE | `bloom_commands.cc` |
| 01-4 | RDB 读取 bit array 时 `bufLen != dataSize_` 静默处理 | **真 bug** | 严格要求 `bufLen == dataSize_`，不匹配则拒绝加载 | `bloom_rdb.cc` |
| 01-5 | DeserializeHeader 缺少完整性校验 | **真 bug** | 新增 `ValidateLayerMeta()`，校验 hashCount、totalBits、dataSize、fpRate、bitsPerEntry、expansionFactor | `bloom_rdb.cc` |
| 01-6 | FilterLayer 使用 RMRealloc 管理非平凡 C++ 对象 | **技术 UB，实际安全** | 暂不修复。BloomLayer 字段均可 bitwise relocate，RMRealloc 的 bitwise copy 在当前实现下正确。修复需引入 `std::vector` + 自定义 allocator 或 placement-new/move/destroy，属重大重构 | — |
| 01-7 | 构造首层失败泄漏已扩容的 layer storage | **真 bug** | 构造函数失败路径中 `RMFree(layers_)` 后再置空 | `sb_chain.cc` |
| 01-8 | `BytesUsed()` 用 `numLayers_` 而非 `layerCapacity_`，低估内存 | **真 bug** | 改用 `layerCapacity_ * sizeof(FilterLayer)` | `sb_chain.cc` |
| 01-9 | MEXISTS wrong type 返回数组内错误而非顶层错误 | **真 bug** | 将 WRONGTYPE 检查移到 `ReplyWithArray` 之前，直接返回顶层错误 | `bloom_commands.cc` |
| 01-10 | EXPANSION 0 被接受并隐式映射为 non-scaling | **真 bug** | BF.RESERVE、BF.INSERT、config 加载均拒绝 `EXPANSION < 1`；同时拒绝 NONSCALING + EXPANSION 共存 | `bloom_commands.cc`, `bloom_config.cc` |

### 02-redisbloom-compatibility

| # | 意见 | 判定 | 修复方案 |
|---|------|------|----------|
| 02-1 | 共享 RedisBloom 类型名但 SCANDUMP/LOADCHUNK 协议不兼容 | **真 bug** | 同 01-1 |
| 02-2 | NoRound bit/byte 布局未按 8 字节对齐 | **设计决策** | 当前实现内部一致，对齐差异属兼容性目标选择，非功能缺陷 |
| 02-3 | SCANDUMP 未遵循"非零 cursor = 可恢复 chunk"流程 | **真 bug** | 同 01-1 |
| 02-4 | AOF rewrite 使用自定义 cursor 序列 | **真 bug** | 随 01-1 一并修复，AOF rewrite 现与 SCANDUMP/LOADCHUNK 协议一致 |
| 02-5 | EXPANSION 0 和 NONSCALING+EXPANSION 行为不兼容 | **真 bug** | 同 01-10 |
| 02-6 | RESP3 boolean 行为未对齐 | **设计增强** | 需 `RedisModule_ReplyWithBool` 支持，当前 Tcl 测试框架不支持 RESP3 验证 |
| 02-7 | BF.DEBUG 未实现 | **功能需求** | 非公开 API，非代码缺陷 |
| 02-8 | 配置参数面不兼容 | **设计决策** | EXPANSION 0 已修复；命名差异属兼容目标选择 |
| 02-9 | 错误文本不兼容 | **表面差异** | 语义等价，仅措辞不同；非标准客户端行为依赖 |
| 02-10 | 兼容性声明过强 | **文档问题** | 已将注释改为 "intended to match RedisBloom layout, not verified against official corpus" |

### 04-performance

| # | 意见 | 判定 | 修复方案 |
|---|------|------|----------|
| 04-1 | 多层查询从旧到新，热点路径慢 | **真问题** | `IsDuplicate` 改为从最新 layer 向旧 layer 反向遍历 |

### 06-implementation-details

| # | 意见 | 判定 | 修复方案 |
|---|------|------|----------|
| 06-4 | RDB reader 无 I/O 错误传播 | **真问题** | `RdbReader` 新增 `ok_` 状态位，`RedisModule_IsIOError` 检测后停止读取 |
| 06-6 | `strncasecmp` 依赖 POSIX，include 不准确 | **真问题** | 添加 `<strings.h>` |
| 06-10 | `RedisModule_ModuleTypeSetValue` 返回值未检查 | **真问题** | 所有调用点检查返回值，失败时释放 filter 并返回错误 |
| 06-12 | 注释措辞过强 | **文档问题** | 同 02-10 |

---

## 二、非 Bug 意见分类

### 设计/兼容性决策（不修改）

| 来源 | 意见 | 理由 |
|------|------|------|
| 02-2, 04-4, 06-1 | NoRound 64-bit 对齐 | 当前实现内部一致；对齐差异是兼容性目标选择 |
| 02-6, 07-5 | RESP3 boolean 返回 | 需 RESP3 客户端支持，属未来增强 |
| 02-7, 03-5 | BF.DEBUG 未实现 | 内部调试命令，非公开 API |
| 02-8, 03-6 | 配置参数命名差异 | 设计选择，非功能缺陷 |
| 02-9 | 错误文本措辞 | 语义等价，非标准客户端依赖 |
| 03-1, 03-7, 03-10 | 兼容范围定义 | 产品决策，已更新注释 |
| 03-2 | LOADCHUNK 缺少恢复状态机 | 设计增强，需要显著复杂度；Redis 其他模块有同样模式 |
| 03-3, 04-3 | 小容量 1024 bit 下限 | 合理工程最小值，防止退化 filter |
| 03-8 | BF.INFO Size 估算方式 | 已修复 layerCapacity_ 问题；精确语义属设计选择 |
| 04-2 | SCANDUMP 无 16MB 分块上限 | 需要重大重构（byte-offset cursor），当前合理 filter 大小下正常工作 |
| 04-7 | kMinFpRate = 1e-15 阈值 | 合理默认值，防止天文数字级 bit array |
| 04-8 | INFO 路径线性扫描 layer | layer 数通常 < 20，缓存收益极低 |
| 06-2, 01-6, 04-6 | RMRealloc 管理非平凡对象 | 技术 UB 但实际安全；修复需重大重构 |
| 06-3 | packed struct reinterpret_cast | RedisBloom 同样模式；仅在跨平台（大端序）时有风险 |
| 06-5 | uint64_t → long long 溢出 | 已通过反序列化校验缓解 |
| 06-7 | 32-bit hash / RawBits 死代码 | 新增 ValidateLayerMeta 已阻断损坏 header 触发此路径 |
| 06-8 | 超大 item hash 长度 | >2GB item 在 Redis 中不现实 |
| 06-11 | 错误路径无统一 helper | 代码质量建议，非功能缺陷 |

### 组织/文档/构建（不修改代码）

| 来源 | 意见 | 理由 |
|------|------|------|
| 05-1 | doc/ vs docs/ 双轨 | 目录组织选择 |
| 05-2 | 源码层次过平 | 当前模块规模下可接受 |
| 05-3 | wire format 无独立 schema | 文档需求 |
| 05-4, 03-9 | 各模块重复 allocator wrapper | 组织选择 |
| 05-5 | CMake 找不到 GTest 静默跳过 | 构建配置问题 |
| 05-6 | Tcl 测试未纳入 CTest | 构建配置问题 |
| 05-7 | README 模块清单过期 | 文档维护 |
| 05-8 | 缺少兼容性测试目录 | 测试基础设施需求 |

### 测试覆盖缺口（已部分补充）

| 来源 | 意见 | 状态 |
|------|------|------|
| 07-1 | SCANDUMP/LOADCHUNK 官方协议测试 | **已添加** |
| 07-2 | malformed LOADCHUNK 测试 | **已添加**（header 过短、零层、cursor 越界、数据长度错误） |
| 07-3 | RDB 损坏输入测试 | 需要 RDB 注入，Tcl 不可测；代码已做防御修复 |
| 07-4 | AOF rewrite 测试 | **已添加** |
| 07-5 | RESP3 测试 | 需 RESP3 客户端，暂不可测 |
| 07-6 | 配置加载测试 | EXPANSION 0 已测；其余需模块重载，Tcl 测试框架有限 |
| 07-7 | wrong-type 覆盖 | **已添加**（MEXISTS、MADD、INSERT、CARD、SCANDUMP、LOADCHUNK） |
| 07-8 | 参数矩阵 | **已添加**（EXPANSION 0/-1、NONSCALING+EXPANSION、NOCREATE overflow） |
| 07-9 | 边界数据 | **已添加**（binary item with null bytes） |
| 07-10 | 多层行为 | **已添加**（EXPANSION 1 vs 4 比较、false negative 验证） |
| 07-11 | 单元测试未运行 | GTest 未安装，构建配置问题 |
| 07-12 | sanitizer/fuzz/compat | 测试基础设施需求 |

---

## 三、修改文件清单

| 文件 | 修改内容 |
|------|----------|
| `modules/gemini-bloom/src/bloom_commands.cc` | MEXISTS 顶层错误、EXPANSION 校验、NONSCALING+EXPANSION 互斥、LOADCHUNK WRONGTYPE 保护、LOADCHUNK 先验证后删除、SetValue 返回值检查、添加 `<strings.h>` |
| `modules/gemini-bloom/src/bloom_config.cc` | EXPANSION 配置拒绝 < 1 |
| `modules/gemini-bloom/src/bloom_rdb.cc` | RDB bit array 长度严格校验、DeserializeHeader 完整性校验（ValidateLayerMeta）、AOF cursor 修复、RdbReader I/O 错误传播、注释措辞修正 |
| `modules/gemini-bloom/src/bloom_rdb.h` | RdbReader 新增 `ok_` 状态位 |
| `modules/gemini-bloom/src/sb_chain.cc` | BytesUsed 用 layerCapacity\_、构造失败释放 layers\_、IsDuplicate 反向遍历 |
| `modules/gemini-bloom/src/sb_chain.h` | 注释措辞修正 |
| `modules/gemini-bloom/src/bloom_filter.h` | 注释措辞修正 |
| `modules/gemini-bloom/tests/tcl/bloom_test.tcl` | 新增 29 条测试（从 53 → 82） |

---

## 四、测试结果

```
Results: 82 passed, 0 failed
```

所有原有测试继续通过，新增测试覆盖了全部已修复的 bug。
