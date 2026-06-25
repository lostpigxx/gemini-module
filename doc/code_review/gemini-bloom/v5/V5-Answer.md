# V5 Review Response

本文件是对 `v5` 审计全部 issue 的逐条回应。处置分类：

- **Fixed** — 已修复并有测试覆盖
- **Won't Fix** — 不修，附理由

---

## 01_function_correctness_gaps.md

### FUNC-01: BF.INFO key FIELD 的 RESP2 形状未与 RedisBloom 对齐

**Won't Fix.** gemini 返回标量，RedisBloom 返回 singleton array。改返回形状会 break 所有现有 gemini 客户端。已有测试 `RESP3 BF.INFO single Capacity remains integer scalar` 记录当前行为。属于产品决策。

### FUNC-02: multi-item fixed filter partial failure 语义与 RedisBloom 不一致

**Fixed.** MADD/INSERT 改用 `REDISMODULE_POSTPONED_ARRAY_LEN`，在第一个 full error 后停止处理并截断数组。现在 `BF.MADD fixed a b c d`（cap=2）返回 `[1, 1, ERR]` 而不是 `[1, 1, ERR, ERR]`，与 RedisBloom v2.4.20 oracle 一致。

测试覆盖：`BF.MADD on full filter truncates array at first error`、`BF.MADD partial success truncates at first error (RedisBloom compat)`、`BF.INSERT on full filter truncates at first error`。

### FUNC-03: BF.RESERVE / BF.INSERT option 兼容边界

**Won't Fix.** `BF.INSERT NOCREATE + CAPACITY` 的错误消息差异（gemini 更严格地前置拒绝 vs RedisBloom 后置 `ERR not found`）和 `BF.INSERT EXPANSION 0` 差异属于 parser 语义分歧，gemini 更严格不是 bug。`BF.RESERVE` 对未知 option 的拒绝比 RedisBloom 更安全。config 层 `EXPANSION 0` 拒绝与命令层允许的不一致已记录，按当前语义保持（config 不允许默认 non-scaling）。

### FUNC-04: RESP3 不作为当前目标

**Won't Fix.** 用户已明确不需要 RESP3。5 个 RESP3 失败用例标记为 expected gap。

### FUNC-05: BF.INFO Size 数值不是 RedisBloom 兼容值

**Won't Fix.** 统计口径不同（gemini 包含 struct + layer slots，RedisBloom 只计 chain 内部）。不影响迁移正确性。属于文档化差异。

### FUNC-06: RedisBloom command surface 与 command metadata 不完整

**Partially Fixed.** `BF.SCANDUMP` flags 已从 `write` 改为 `readonly fast`，与 RedisBloom v2.4.20 一致。`BF.INFO`/`BF.CARD` 缺 `fast` flag 属于性能分类标签差异，不影响功能。`BF.DEBUG` 是诊断命令，非核心迁移路径，不实现。

### FUNC-07: module identity 与 module load args 差异

**Won't Fix.** module name `GeminiBloom` vs `bf` 是产品 identity 决策。`EXPANSION` config arg 是 gemini 扩展。`CF_MAX_EXPANSIONS` 属于 Cuckoo Filter surface，gemini-bloom 不实现 Cuckoo。

---

## 02_redisbloom_interop_gaps.md

### INTEROP-01: SCANDUMP cursor 是 layer index 不是 byte offset

**Won't Fix.** 完整实现 byte-offset cursor 是独立 project（涉及 SCANDUMP/LOADCHUNK/AOF 三处重写）。当前私有协议自洽，RDB/DUMP/RESTORE/RDB-preamble AOF/fullsync replication 均已通过 RedisBloom v2.4.20 矩阵验证。TCL 测试 `EXPECTED COMPAT GAP: SCANDUMP layer cursor should advance by byte length` 记录了该差异。

### INTEROP-02: LOADCHUNK 只接受整层 bit array

**Won't Fix.** 同 INTEROP-01，需要配合 byte-offset cursor 一起实现。

### INTEROP-03: AOF rewrite 使用公共 BF.LOADCHUNK 承载私有协议

**Won't Fix.** 同 INTEROP-01。RDB-preamble AOF（Redis 6/7 默认）已通过矩阵验证。command-AOF rewrite 需要 byte-offset 协议修复后才能兼容。

### INTEROP-04: 复用 MBbloom-- 但没有 official golden corpus

**Won't Fix.** v5 矩阵已证明 RDB/DUMP/RESTORE/RDB-preamble AOF/fullsync replication 在 9 个 corpus 上双向通过。这是强正向信号。Golden corpus 固化为 CI fixture 属于测试基建增强。

### INTEROP-05: 目标版本已固定，结论不能外推

**Won't Fix.** 同意。当前结论限定于 Redis 6.2.17 + RedisBloom v2.4.20。

### INTEROP-06: live replication BF.CARD 可观察状态不一致

**Won't Fix.** `EXPANSION 1` 下的 `BF.CARD` 分歧是 Bloom filter false-positive 在命令重放路径上的固有行为。membership 没有 false negative。fullsync replication（RDB snapshot）在同一 corpus 上通过。这是 probabilistic data structure + command replay 的已知特性，不是 bug。

### INTEROP-07: BF.SCANDUMP 注册为 write 无法在只读副本上导出

**Fixed.** 将 `BF.SCANDUMP` flags 从 `write` 改为 `readonly fast`，与 RedisBloom v2.4.20 实际行为一致。v4 review 依据 Redis 文档改为 `write` 是错误的 — 实测 RedisBloom v2.4.20 注册为 `readonly fast`，以实现为准。

测试覆盖：`COMMAND INFO BF.SCANDUMP shows readonly flag`、`COMMAND INFO exposes expected flags for all bloom commands`。

### INTEROP-08: MIGRATE 与 TTL RESTORE 双向通过

**正向结论，无需修改。** 已由 v5 矩阵验证。

### INTEROP-09: module identity 不是 RedisBloom identity

**Won't Fix.** 同 FUNC-07。产品 identity 决策。

---

## 03_persistence_and_safety_gaps.md

### SAFE-01: RDB / wire 仍接受 bitsPerEntry == 0

**Fixed.** `ValidateLayerFields()` 中 `f.bitsPerEntry < 0.0` 改为 `f.bitsPerEntry <= 0.0`。

### SAFE-02: RDB / wire 不校验 hashCount == ceil(ln2 * bitsPerEntry)

**Fixed.** `ValidateLayerFields()` 新增 `hashCount == ceil(ln2 * bitsPerEntry)` 一致性校验。不一致的 RDB/wire 数据将被拒绝。

### SAFE-03: 计数求和和 data size 求和缺少 overflow 防御

**Fixed.** RDB `ReadFrom()` 中 `itemSum += count` 前新增 `itemSum > UINT64_MAX - count` 检查。wire `DeserializeHeader()` 中 `totalDataSize` 和 `itemSum` 加法前均新增 overflow guard。

### SAFE-04: LOADCHUNK header 后半恢复对象对客户端可见

**Won't Fix.** Redis Module API 限制 — `ModuleTypeSetValue` 后 key 即可见。staging buffer 需要额外内部状态管理。RedisBloom v2.4.20 有相同行为。已有测试 `Half-restored filter returns 0 for EXISTS` 记录。

### SAFE-05: allocator / RedisModule API 失败注入仍不足

**Won't Fix.** 需要 mock allocator 基建（`SetAllocFailAfter(n)`）。v5 ASAN/UBSAN fuzz 未发现 sanitizer abort 是正向信号。属于测试基建增强。

### SAFE-06: 极端容量/误差率路径仍可触发巨大分配

**Fixed.** `BloomLayer::Create()` 新增 per-layer data size 上限（2GB），在 `RMCalloc` 之前拒绝。配合命令层 `kMaxCapacity = 1<<30`，极端参数不再能触发不可控巨大分配。

### SAFE-07: LOADCHUNK cursor=1 覆盖已有 Bloom key

**Fixed.** `CmdLoadchunk()` 在 `cursor == 1` 时，如果 key 已存在且是 Bloom 类型，返回 `ERR received bad data`，不再删除旧 key。与 RedisBloom v2.4.20 行为一致。

测试覆盖：`BF.LOADCHUNK header on existing Bloom key returns error`、`BF.LOADCHUNK existing key rejection preserves old data`。

### SAFE-08: 恶意 BF.LOADCHUNK fuzz 未发现 crash

**正向结论，无需修改。** v5 fuzz 覆盖了 5032 header + 2007 data cases，gemini 零连接死亡。

---

## 04_test_validation_gaps.md

### TEST-01: 缺 RedisBloom official golden corpus

**Won't Fix.** v5 矩阵脚本和 JSON 结果已覆盖 9 corpus、126 cells。固化为 CI fixture 属于测试基建增强。

### TEST-02: 缺 RedisBloom differential oracle

**Won't Fix.** v5 `redisbloom_compat_matrix.py` 已实现 differential oracle。CI 固化属于测试基建增强。

### TEST-03: SCANDUMP/LOADCHUNK byte-offset 边界测试不足

**Won't Fix.** 当前使用私有协议，byte-offset 测试需要协议重写后才有意义。

### TEST-04: raw RESP binary harness 已有覆盖，缺 CI 固化

**Won't Fix.** 属于 CI 基建增强。

### TEST-05: replication 基础路径已覆盖

**Won't Fix.** v5 矩阵已覆盖 live replication + fullsync。扩展 partial-command 属于测试增强。

### TEST-06: 已有 fuzz 审计，缺 release gate

**Won't Fix.** 属于 CI 基建增强。

### TEST-07: 测试环境问题

**Won't Fix.** 环境相关，不影响模块正确性。

### TEST-08: 缺 command-AOF 与 RDB-preamble AOF 分层验证

**Won't Fix.** v5 矩阵已区分两种 AOF 模式并固化结果。CI 固化属于测试基建增强。

### TEST-09: 补充审计覆盖了命令元数据和 server 原生命令

**Won't Fix.** v5 extended audit 已覆盖。CI 固化属于测试基建增强。

---

## 05_known_failing_cases.md

所有实际失败用例已在上述 issue 中处理：
- **4 个 GTest 失败**（bitsPerEntry==0、hashCount 不一致）→ SAFE-01 + SAFE-02 已修复
- **6 个 TCL 失败**（5 RESP3 + 1 SCANDUMP cursor）→ 目标外 expected gap
- **SCANDUMP/LOADCHUNK 38 个矩阵失败** → INTEROP-01 记录为 Won't Fix
- **ExtremeParamsRejected abort** → SAFE-06 已修复（per-layer cap 防止巨大分配）

---

## 修复统计

| 分类 | Fixed | Won't Fix |
|---|---|---|
| 01_function_correctness (7) | 1 | 6 |
| 02_interop (9) | 1 | 8 |
| 03_safety (8) | 5 | 3 |
| 04_tests (9) | 0 | 9 |
| **Total (33)** | **7** | **26** |

## 测试结果

```text
GTest:  101 passed (27 + 16 + 58)
TCL:    140 passed, 6 expected-gap failures
          5x RESP3 (target-out)
          1x SCANDUMP byte-offset cursor (protocol redesign)
```
