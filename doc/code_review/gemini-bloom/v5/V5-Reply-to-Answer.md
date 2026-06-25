# V5 Reply to Answer

本文件是对 `doc/code_review/gemini-bloom/v5/V5-Answer.md` 的复审回复。复审基线仍按 v5 审计假设：

- gemini-bloom 是一个需要与 RedisBloom 互通的 Bloom Filter module。
- 目标兼容基线是 Redis 6.2 + gemini-bloom 对比 Redis 6.2 + RedisBloom v2.4.20。
- 需要支持数据迁入迁出；RESP3 不作为本轮必需目标。

## 总结论

`V5-Answer.md` 不能作为 v5 审计闭环接受。

原因不是实现完全没改。相反，代码确实修了若干点：`BF.MADD`/`BF.INSERT` partial failure reply shape、`BF.SCANDUMP` command flags、`BF.LOADCHUNK cursor=1` 覆盖已有 Bloom key、部分 RDB/wire metadata 校验和部分 overflow guard 都有实质改动。

但 Answer 的闭环声明仍有硬伤：

1. 把 `BF.SCANDUMP` / `BF.LOADCHUNK` 和 command-AOF 这种目标内迁入迁出阻断项标成 Won't Fix，和本轮产品假设冲突。
2. 多个 `Fixed` 只修了窄路径，仍留下 RDB 恶意输入、资源上限和 post-fix 证据缺口。
3. `README.md`、`05_known_failing_cases.md`、`06_...compat_results.md`、`07_fuzz...md` 和对应 JSON 多处仍是修复前结果，和 `V5-Answer.md` 的 post-fix 声明互相矛盾。
4. `05_known_failing_cases` 的归类被 Answer 简化错了：38 个矩阵失败不是单纯 "SCANDUMP/LOADCHUNK 38 个"，而是 18 个 SCANDUMP/LOADCHUNK、18 个 command-AOF、2 个 live replication `BF.CARD` 分歧。

因此，当前状态只能说“部分修复已落地，且 gemini 自身基础测试通过”；不能说“v5 审计意见已完整处理”。

## 目标边界不能被 Answer 偷换

v5 的 `00_scope_and_method.md` 明确写了目标假设：需要支持 RDB、DUMP/RESTORE、AOF、SCANDUMP/LOADCHUNK、replication 的互通或明确声明不互通；RESP3 不作为 P0/P1 阻断项。

在这个前提下：

- RDB / DUMP / RESTORE / MIGRATE / RDB-preamble AOF / fullsync replication 通过，是强正向信号。
- 但 public `BF.SCANDUMP` / `BF.LOADCHUNK` 双向失败和 command-AOF rewrite 双向失败，仍是目标内迁入迁出的硬阻断。
- Answer 可以选择“产品只承诺 RDB-object 迁移，不承诺 public wire protocol 和 command-AOF”。但它必须把兼容声明改窄，不能继续把这些 P0/P1 作为普通 Won't Fix 后声称 v5 已闭环。

## 代码修改复审

### `modules/gemini-bloom/src/bloom_commands.cc`

`BF.MADD` 和 `BF.INSERT` 的 partial failure 修改方向正确：

- `CmdMadd()` 使用 `REDISMODULE_POSTPONED_ARRAY_LEN`，遇到第一个 full error 后 `break`，最后 `ReplySetArrayLength()`。
- `CmdInsert()` 使用同样模式。
- Tcl 测试覆盖了 full filter 和 partial success 两种 reply length。

对应位置：

- `modules/gemini-bloom/src/bloom_commands.cc:191-221`
- `modules/gemini-bloom/src/bloom_commands.cc:369-388`
- `modules/gemini-bloom/tests/tcl/bloom_test.tcl:1248-1292`

这个修复可以接受。但 post-fix RedisBloom oracle 没有重跑：`compat_matrix_results_redis62_redisbloom2420.json` 仍记录 gemini 返回 `[1, 1, ERR, ERR]`，RedisBloom 返回 `[1, 1, ERR]`。所以 Answer 中“与 RedisBloom oracle 一致”的结论目前只有本地 Tcl 回归支撑，没有 post-fix 矩阵结果支撑。

`BF.SCANDUMP` flags 修改方向正确：

- 当前注册为 `readonly fast`。
- 这和 RedisBloom v2.4.20 的 command metadata 一致。

对应位置：

- `modules/gemini-bloom/src/bloom_commands.cc:633-643`

但 `extended_audit_results_redis62_redisbloom2420.json` 仍记录 `readonly_scandump_ok.gemini=false`，说明补充审计 JSON 是修复前结果，不能继续作为 post-fix evidence。

`BF.LOADCHUNK cursor=1` 对已有 Bloom key 的修改方向正确：

- 当前已有 Bloom key 返回 `ERR received bad data`。
- 不再删除旧 key。
- Tcl 回归覆盖了错误和旧数据保留。

对应位置：

- `modules/gemini-bloom/src/bloom_commands.cc:586-602`
- `modules/gemini-bloom/tests/tcl/bloom_test.tcl:1547-1566`

但 `extended_audit_results_redis62_redisbloom2420.json` 仍记录 `loadchunk_header_over_existing_key.gemini = OK`，`malicious_wire_audit_results_redis62_redisbloom2420.json` 仍记录 `old_lost=4`。这些是修复前证据，必须重跑或标注为 pre-fix。

`BF.SCANDUMP` / `BF.LOADCHUNK` public wire protocol 没有修：

- `SCANDUMP cursor=0` 返回 header 和 next cursor `1`。
- 后续 chunk 的 cursor 仍是 layer index 风格：`cursor + 1`。
- `LOADCHUNK cursor>1` 仍用 `idx = cursor - 2`，并要求 `dataLen == whole layer data size`。

对应位置：

- `modules/gemini-bloom/src/bloom_commands.cc:543-561`
- `modules/gemini-bloom/src/bloom_commands.cc:610-618`

这正是 v5 P0 兼容失败的根因。Answer 把它归为 Won't Fix，和“支持 RedisBloom 迁入迁出”的目标冲突。

### `modules/gemini-bloom/src/bloom_rdb.cc`

`ValidateLayerFields()` 增加 `bitsPerEntry > 0` 和 `hashCount == ceil(ln2 * bitsPerEntry)` 的方向正确：

- `bitsPerEntry <= 0` 现在被拒绝。
- `hashCount` 不一致现在被拒绝。

对应位置：

- `modules/gemini-bloom/src/bloom_rdb.cc:53-68`

但是这个修复不是完整安全闭环。

第一，RDB path 先窄化再校验：

- `hashCount` 从 RDB `uint64` 读出后直接 cast 为 `uint32_t`。
- `log2Bits` 从 RDB `uint64` 读出后直接 cast 为 `uint8_t`。
- `rawFlags` 和 `expansionFactor` 也先 cast 为 `unsigned`。

对应位置：

- `modules/gemini-bloom/src/bloom_rdb.cc:89-92`
- `modules/gemini-bloom/src/bloom_rdb.cc:169-178`

这意味着恶意 RDB 可以写入 `2^32 + expectedHash`、`256 + validLog2`、`2^32 + validFlags` 等值，cast 后变成合法低位，再通过 validator。现有 `CustomLayerFields` 测试字段本身就是 `uint32_t hashCount` 和 `uint8_t log2Bits`，无法覆盖这个绕过。正确做法是先以 `uint64_t` 保存 raw 值，检查范围后再 cast。

第二，`bitsPerEntry` 没有合理上限：

- `ValidateLayerFields()` 只检查 finite 和 `> 0`。
- 然后对 `ceil(ln2 * bitsPerEntry)` 做 `uint32_t` cast。

对应位置：

- `modules/gemini-bloom/src/bloom_rdb.cc:62-65`

如果恶意 RDB/wire 提供极大但 finite 的 `bitsPerEntry`，这里存在浮点到整数越界风险。`Inf` 被拒绝不等于所有超大 finite 值安全。这个点需要上界校验，至少保证 hash count 计算结果落在 `uint32_t` 可表示范围，并符合 RedisBloom v2.4.20 corpus。

第三，SAFE-03 的 RDB total data size cap 没落实：

- Answer 说 RDB `itemSum` overflow guard 已加，这是真的。
- 但 v5 原建议还要求 RDB path 引入与 wire path 一致的总 data size 上限。
- 当前 RDB `ReadFrom()` 会按 metadata 计算 `dataSize_`，然后 `RMAlloc(layer.dataSize_)`。

对应位置：

- `modules/gemini-bloom/src/bloom_rdb.cc:96`
- `modules/gemini-bloom/src/bloom_rdb.cc:104-115`
- `modules/gemini-bloom/src/bloom_rdb.cc:187-212`

因此 SAFE-03 只能算 partial fixed，不能算完整 fixed。

第四，wire path 仍允许最高 4GB totalDataSize，并且每层 allocation 走 `FromWireMeta()`：

- `DeserializeHeader()` 只限制 total data size 到 4GB。
- `BloomLayer::FromWireMeta()` 直接 `RMCalloc(layer.dataSize_, 1)`。

对应位置：

- `modules/gemini-bloom/src/bloom_rdb.cc:132-143`
- `modules/gemini-bloom/src/bloom_rdb.cc:273-283`
- `modules/gemini-bloom/src/bloom_rdb.cc:294-302`

这和 Answer 对 SAFE-06 的“极端参数不再能触发不可控巨大分配”不匹配。

### `modules/gemini-bloom/src/bloom_filter.cc`

`BloomLayer::Create()` 新增 2GB per-layer cap，方向正确：

- 在 `RMCalloc` 之前拒绝 `dataSize_ > 2GB`。

对应位置：

- `modules/gemini-bloom/src/bloom_filter.cc:122-132`

但这个 cap 只覆盖通过 `BloomLayer::Create()` 创建 layer 的路径，不覆盖 RDB `ReadFrom()` 和 wire `FromWireMeta()`。所以 SAFE-06 不能算完整 fixed。

Tcl 测试也不证明这个 cap：

- 测试名叫 `BloomLayer::Create rejects extremely large capacity`。
- 实际执行的是 `BF.RESERVE` 在命令层最大 capacity 上成功。
- 注释写 “512MB data size cap”，实现是 2GB cap。
- 该测试没有构造会被 `BloomLayer::Create()` cap 拒绝的参数，也没有覆盖 RDB/wire 绕过命令层的路径。

对应位置：

- `modules/gemini-bloom/tests/tcl/bloom_test.tcl:1568-1579`

`sb_chain_test` 的 `ExtremeParamsRejected` 本地全量通过是正向信号，但它仍只覆盖 `BloomLayer::Create()` 的极端参数，不覆盖 RDB/wire payload。

## 对 `V5-Answer.md` 逐项回复

### 01_function_correctness_gaps.md

| Issue | Answer 结论 | 复审结论 |
| --- | --- | --- |
| FUNC-01 BF.INFO field RESP2 shape | Won't Fix | 可以作为产品决策保留，但 Answer 的证据不硬。RESP2 shape 差异不能用 `RESP3 BF.INFO single Capacity remains integer scalar` 证明。必须在兼容声明中写明 gemini 不是 RedisBloom drop-in client compatibility。 |
| FUNC-02 MADD/INSERT partial failure | Fixed | 代码修改方向正确，本地 Tcl 回归覆盖。缺 post-fix RedisBloom oracle 重跑；现有 compat JSON 仍是修复前结果。 |
| FUNC-03 option parser 兼容边界 | Won't Fix | 可作为产品决策，但“gemini 更严格不是 bug”不能抹掉 client compatibility 差异。`BF.INSERT EXPANSION 0`、`NOCREATE + CAPACITY`、missing-key error 文案仍应列入兼容差异。 |
| FUNC-04 RESP3 | Won't Fix | 接受。当前用户明确不需要 RESP3。 |
| FUNC-05 BF.INFO Size | Won't Fix | 可接受为文档化差异；但它会影响 RedisBloom 客户端监控和告警，不能说“无影响”，只能说“不影响 membership/RDB 迁移正确性”。 |
| FUNC-06 command surface / metadata | Partially Fixed | `BF.SCANDUMP readonly fast` 代码已修。`BF.INFO`/`BF.CARD` fast flag、`BF.DEBUG` 缺失仍是差异。extended JSON 未重跑。 |
| FUNC-07 module identity / load args | Won't Fix | 可接受为产品 identity 决策，但必须明确不是 RedisBloom drop-in module identity。 |

### 02_redisbloom_interop_gaps.md

| Issue | Answer 结论 | 复审结论 |
| --- | --- | --- |
| INTEROP-01 SCANDUMP cursor | Won't Fix | 不接受。v5 目标包括 RedisBloom 迁入迁出，public `BF.SCANDUMP` 是核心迁移入口。18/18 失败不能用“独立 project”关闭，除非产品明确收窄为只支持 RDB-object 迁移。 |
| INTEROP-02 LOADCHUNK data chunk | Won't Fix | 不接受。与 INTEROP-01 同一阻断。当前 `cursor - 2` layer-index 逻辑仍不能导入 RedisBloom byte-offset chunk。 |
| INTEROP-03 command-AOF rewrite | Won't Fix | 不接受。关闭 RDB preamble 后 command-AOF 18/18 失败，且使用公共 `BF.LOADCHUNK` 命令承载私有协议会误导迁移工具。若不修，必须声明 command-AOF 不兼容，并避免把 AOF 泛称为通过。 |
| INTEROP-04 MBbloom-- / golden corpus | Won't Fix | 部分接受。9 corpus 矩阵是强正向信号，但不是 official golden corpus，也不是 post-fix 证据。建议至少把当前 corpus 固化为 CI gate。 |
| INTEROP-05 版本边界 | Won't Fix | 接受。结论只能限定 Redis 6.2.17 + RedisBloom v2.4.20。 |
| INTEROP-06 live replication BF.CARD | Won't Fix | 部分接受。membership 无 false negative 是事实；但 `BF.CARD` 是 RedisBloom 可观察 API。不能说“不是 bug”，只能说“不影响 membership，但不满足完整 command-stream observable compatibility”。 |
| INTEROP-07 SCANDUMP flags | Fixed | 代码已修；但 extended audit JSON/README/05/06 仍是修复前结果。需要重跑或标注 pre-fix。 |
| INTEROP-08 MIGRATE / TTL restore | 正向 | 可以保留，但仍需 post-fix 重跑或把证据明确标注为修复前模块结果且说明本次代码不会影响 RDB object path。 |
| INTEROP-09 module identity | Won't Fix | 可接受为产品决策；必须进入兼容声明。 |

### 03_persistence_and_safety_gaps.md

| Issue | Answer 结论 | 复审结论 |
| --- | --- | --- |
| SAFE-01 bitsPerEntry == 0 | Fixed | wire/header path方向正确；RDB path仍有“先窄化再校验”和超大 finite `bitsPerEntry` 风险。需要补 raw range 和极大 finite 测试。 |
| SAFE-02 hashCount consistency | Fixed | 不完整。RDB `hashCount` 先 cast 到 `uint32_t`，恶意 RDB 可用 `2^32 + expectedHash` 绕过。`bitsPerEntry` 极大 finite 值还可能触发 cast 越界。 |
| SAFE-03 sum/data size overflow | Fixed | 只能 partial。`itemSum` 和 wire `totalDataSize` guard 已加；RDB 总 data size cap 未加，RDB raw field narrowing 未处理。 |
| SAFE-04 half-restored key visible | Won't Fix | 可作为设计取舍保留，但必须明文警告 `LOADCHUNK` 非事务性 restore，且不能混淆它和 SCANDUMP/LOADCHUNK 协议不互通问题。 |
| SAFE-05 failure injection | Won't Fix | 可作为测试基建项保留；但 ASAN/UBSAN fuzz 是修复前结果，不能证明 post-fix 状态。 |
| SAFE-06 huge allocation | Fixed | 不接受完整 fixed。只加了 `BloomLayer::Create()` 2GB cap，RDB/wire deserialization 仍可绕过；Tcl 测试没有验证拒绝路径。 |
| SAFE-07 existing-key LOADCHUNK overwrite | Fixed | 代码和 Tcl 回归可以接受；但 extended/malicious JSON 仍是修复前结果。 |
| SAFE-08 malicious LOADCHUNK fuzz | 正向 | 不能作为 post-fix 正向结论，因 JSON 仍显示修复前 old_lost 和 unsafe accept。必须重跑。 |

### 04_test_validation_gaps.md

| Issue | Answer 结论 | 复审结论 |
| --- | --- | --- |
| TEST-01 golden corpus | Won't Fix | 不接受为 release 闭环。当前没有 official corpus，可以接受为限制，但至少要把现有 9 corpus 作为 post-fix CI fixture。 |
| TEST-02 differential oracle | Won't Fix | 不接受为闭环。脚本存在不等于 release gate；post-fix JSON 也未重跑。 |
| TEST-03 byte-offset tests | Won't Fix | 不接受。如果继续声明 RedisBloom public wire migration，需要修协议并补测试；如果产品不支持，需要把兼容声明收窄。 |
| TEST-04 raw RESP harness CI | Won't Fix | 可以降级为基建项，但 fixed issue 的最小恶意样本必须进入自动回归。 |
| TEST-05 replication | Won't Fix | 部分接受。fullsync 通过是正向；live command stream 仍有 `BF.CARD` 分歧，不能写成完全覆盖。 |
| TEST-06 fuzz release gate | Won't Fix | 不接受为安全闭环。RDB/wire/LOADCHUNK 是非信任输入，至少要把已发现的最小失败样本固化。 |
| TEST-07 test environment | Won't Fix | 环境问题本身不是模块正确性问题。本地当前 GTest/Tcl 可跑通，文档应更新。 |
| TEST-08 AOF split validation | Won't Fix | 脚本已区分两种 AOF，但 post-fix 未重跑。更重要的是 command-AOF 仍失败，不能作为普通 CI 增强处理。 |
| TEST-09 extended audit | Won't Fix | 不接受当前闭环。extended JSON 明确是修复前状态，至少要重跑 `BF.SCANDUMP` readonly replica 和 existing-key LOADCHUNK 两项。 |

### 05_known_failing_cases.md

Answer 写“所有实际失败用例已在上述 issue 中处理”，这个结论不成立。

具体问题：

- 6 个 Tcl 失败中 5 个 RESP3 是目标外，1 个 SCANDUMP byte-offset 不是目标外；它是当前产品假设下 RedisBloom public wire migration 的核心失败。
- 38 个矩阵失败不能归为 “SCANDUMP/LOADCHUNK 38 个”。实际是 `SCANDUMP/LOADCHUNK 18`、`command-AOF 18`、`live replication BF.CARD 2`。
- `ExtremeParamsRejected` 本地已通过，但 `README.md` 和 `05_known_failing_cases.md` 仍保留 abort 叙述。
- 4 个 GTest 失败已修，但 fuzz JSON 和 README 仍保留 unsafe accept 结论。

## 证据一致性问题

当前 v5 目录里至少有以下修复前证据仍被当成当前结果：

- `compat_matrix_results_redis62_redisbloom2420.json` 仍记录 gemini MADD/INSERT overflow 返回额外 error 元素。
- `extended_audit_results_redis62_redisbloom2420.json` 仍记录 `readonly_scandump_ok.gemini=false`。
- `extended_audit_results_redis62_redisbloom2420.json` 仍记录 `loadchunk_header_over_existing_key.gemini=OK`。
- `malicious_wire_audit_results_redis62_redisbloom2420.json` 仍记录 gemini existing-key `old_lost=4`。
- `rdb_wire_fuzz_results_redis62_redisbloom2420.json` 和 ASAN 版仍记录 `bits_per_entry_zero`、`hash_count_inconsistent` unsafe accept。
- `README.md` 仍把 `BF.SCANDUMP write`、unsafe accept、ExtremeParams abort、old_lost 写成当前阻断项。

这不是小文档瑕疵。`V5-Answer.md` 的核心形式是“对审计意见的回复”，它必须能让读者判断每个 issue 是否闭环。当前同一目录内的证据互相打架，结论不可审计。

## 必须补的工作

如果目标仍是“RedisBloom 可迁入迁出”，最低要求如下：

1. 实现 RedisBloom v2.4.20 byte-offset `BF.SCANDUMP` / `BF.LOADCHUNK` 协议，覆盖 layer 内、layer 边界、跨 layer、16MiB split、`iter < dataLen`、越界和半恢复错误。
2. 修复 command-AOF rewrite，使关闭 RDB preamble 时的 AOF 能在 RedisBloom v2.4.20 和 gemini 之间双向回放；或者改用私有命令并明确声明 command-AOF 不兼容。
3. RDB loader 所有窄字段先 raw range check 再 cast，包括 flags、expansion、hashCount、log2Bits。
4. 给 `bitsPerEntry` 加上合理上限，避免 finite 极大值在 `ceil(...)->uint32_t` 中越界。
5. 将 RDB/wire deserialization 的资源上限和 `BloomLayer::Create()` 的上限统一，补 RDB totalDataSize cap。
6. 重跑 post-fix RedisBloom v2.4.20 compat matrix、extended audit、decoder fuzz、ASAN/UBSAN fuzz、malicious LOADCHUNK audit。
7. 更新 `README.md`、`05_known_failing_cases.md`、`06_...compat_results.md`、`07_fuzz...md` 和所有 JSON，使它们明确是 post-fix 结果；如果保留 pre-fix JSON，文件名和文档必须标注 pre-fix。
8. 把已修复项的最小样本固化为 CI 回归，不要只留审计 runner。

如果产品决定只支持 RDB-object 迁移，则必须把兼容声明改成更窄的、不可误读的版本：

```text
gemini-bloom currently interoperates with RedisBloom v2.4.20 only through Redis
RDB object paths validated by this matrix: RDB file load/save, DUMP/RESTORE,
MIGRATE, RDB-preamble AOF, and fullsync replication. It does not currently
support RedisBloom-compatible public BF.SCANDUMP/BF.LOADCHUNK byte-offset
wire migration or command-AOF rewrite replay.
```

在这种收窄声明之前，`V5-Answer.md` 的 Won't Fix 分类不成立。

## 当前可接受的部分

以下点可以认可为已经有实质进展：

- `BF.MADD` / `BF.INSERT` partial failure reply truncation 已在代码中实现，本地 Tcl 覆盖。
- `BF.SCANDUMP` command flag 已改为 `readonly fast`。
- `BF.LOADCHUNK cursor=1` 已拒绝已有 Bloom key，并保留旧数据。
- `bitsPerEntry <= 0` 和普通 `hashCount` 不一致的直接用例已被 validator 拒绝。
- `itemSum` overflow guard 和 wire `totalDataSize` overflow guard 已补。
- `BloomLayer::Create()` 对普通创建路径加了 2GB allocation cap。
- 本地 GTest 和 Tcl 基础测试可通过，剩余 Tcl 红灯符合已知 expected gap。

这些进展应该保留，但不能支持“v5 审计已完整闭环”的结论。

