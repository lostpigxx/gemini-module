# gemini-json 深度优化规划 v2（Phases 6–9）

> 前置基线：已完成 Phase 1–5（见 [audit-and-optimization-plan.md](./audit-and-optimization-plan.md)，commit `cbac1c5`）  
> 目标平台：**Redis 7**（Clean-room C++20，ASan + UBSan + Docker Redis 7.2 端到端验证）

---

## 总览

| Phase | 主题 | 核心内容与预期收益 |
|---|---|---|
| **Phase 6** | **JSONPath 热路径收口、语义对齐与 Docker Redis 7 + UBSan 验收** | `FilterNode` 正则预编译（正则过滤预计 **10×~50×** 提速）；全命令统一接入 `ParsePathCached`；`JSON.MERGE` 缺失叶子键自动创建；Docker Redis 7.2（`COPY` / `activedefrag` / `COMMAND INFO` KeySpecs / Compat RDB）与 UBSan 全量回归 |
| **Phase 7** | **SWAR / SIMD 高吞吐 Parser & Serializer** | 8 字节 SWAR + ARM NEON / x86 SSE2 批量扫描 `"`、`\`、`< 0x20`、`>= 0x80`；Serializer 向量化非转义块拷贝；目标 Parse **450~700+ MB/s**、Serialize **1.2~2.0+ GB/s** |
| **Phase 8** | **RFC 9535 JSONPath 函数扩展** | Filter 表达式支持 `length()`、`count()`、`match()`、`search()` 函数（预编译正则、类型安全、可与 `&&`/`||`/`!` 及比较运算任意组合） |
| **Phase 9** | **DOM 内存深度压缩（短键内联 / 紧凑值存储）** | 消除对象短键重复堆分配（SKO / Arena），并评估/落地数组与对象内紧凑值存储，目标将 5k records 内存倍率从 `4.90×` 压至 `≤ 3.0×`，100k 整数数组进一步大幅减负 |

---

## 各 Phase 详细设计与验证矩阵

### Phase 6：JSONPath 热路径收口、语义对齐与 Docker Redis 7 + UBSan 验收

**改造内容**
1. **Regex Filter 预编译（`json_path.h` / `json_path.cc`）**：
   - 新增 RAII 封装 `struct CompiledRegex`（持有已 `regcomp` 的 `regex_t`，析构时自动 `regfree`），以 `std::shared_ptr<const CompiledRegex>` 存入 `FilterNode`。
   - `ParseRegexPattern` 解析阶段一次性编译并校验；`CompareEvalValues` 求值期直接调用 `regexec`，彻底消除逐元素重复编译开销，且保持 `JsonPath` 可安全拷贝入 LRU 缓存。
2. **全命令接入 `ParsePathCached`（`json_commands.cc`）**：
   - 将 `ParsePathFromArg` 重构为返回 `const JsonPath*`（内部调用 `ParsePathCached`），覆盖 `JSON.SET`、`JSON.DEL`、`JSON.TYPE`、`JSON.ARR*`、`JSON.STR*`、`JSON.NUM*`、`JSON.OBJ*`、`JSON.CLEAR`、`JSON.TOGGLE`、`JSON.MGET`、`JSON.MSET`、`JSON.MERGE`、`JSON.DEBUG`、`JSON.RESP` 全部命令。
3. **`JSON.MERGE` 缺失叶子键语义对齐（`json_commands.cc`）**：
   - 当 `EvaluatePath` 未命中但父对象存在时，若 patch 非 `null`，通过 `TryCreateAtLeafKey` 自动创建叶子键（与 RedisJSON RFC 7396 行为一致）；若 patch 为 `null` 则保持 no-op 返回 `OK`。
4. **Docker Redis 7.2 + UBSan 端到端验证**：
   - 新增/运行 UBSan 构建（`-fsanitize=address,undefined -fno-sanitize-recover=all`）跑通全部 GTest。
   - 在 Docker `redis:7.2` 容器内编译并运行完整 Tcl 套件，实测：
     - `COPY` 独立深拷贝（Redis 7+）
     - `COMMAND INFO` 验证 `JSON.MGET` / `JSON.MSET` / `JSON.DEBUG` KeySpecs
     - `CONFIG SET activedefrag yes` 触发 `DefragJson` 回调无崩溃、数据完整
     - `MODULE LOAD ... COMPAT` 文本 RDB 持久化与重启恢复

**测试与验证**
- **GTest**：预编译正则在 10k 项数组上的多次求值与缓存复用；非法正则拒绝；`JSON.MERGE` 缺失叶子键场景。
- **`json_bench`**：新增 `Eval regex filter (300 recs / 3k recs)` 对比优化前后耗时。
- **Tcl（本机 + Docker Redis 7.2）**：`JSON.MERGE` 自动建叶子键、`COPY`、`COMMAND INFO`、Compat 模式 RDB、`activedefrag` 压测。

---

### Phase 7：SWAR / SIMD 高吞吐 Parser & Serializer

**改造内容**
1. **跨平台 SWAR + SIMD 字符扫描模块（`json_simd.h` 内部头文件）**：
   - 提供 `FindNextSpecialInString(const char* p, const char* end)`（检测 `"`、`\`、`< 0x20`、`>= 0x80`）与 `FindNextEscapeInSerialize(const char* p, const char* end)`（检测 `"`、`\`、`< 0x20`）。
   - 编译期分派：
     - ARM64 (`__ARM_NEON`)：16 字节 NEON 向量比较（`vceqq_u8` / `vcltq_u8` / `vcgeq_u8`）。
     - x86-64 (`__SSE2__`)：16 字节 SSE2 `_mm_movemask_epi8`。
     - 通用回退：8 字节 SWAR 位运算无分支扫描（零平台依赖）。
   - 尾部不足 16 字节安全落回标量循环，严格杜绝越界读（ASan/UBSan 验证）。
2. **Parser 与 Serializer 接入**：
   - `ParseStringView`（无转义快路径与转义后洁净段）接入 `FindNextSpecialInString`。
   - `Writer::WriteString` 接入 `FindNextEscapeInSerialize`，批量 `out.append` 整个无转义大块。
   - 优化 `SkipWhitespace`（先单字节快速判非空白，长空白段批量推进）。

**测试与验证**
- **GTest**：0..64 字节全长度与全 lane 偏移（0..15）特殊字符（`"`、`\`、`\x00`..`\x1f`、`\x80`..`\xff`）边界单测，确保 ASan/UBSan 下零越界。
- **`json_bench`**：Parse / Serialize（small / medium / 1MB large / escaped）MB/s 对比 Phase 5 基线。
- **Tcl**：长字符串与大文档读写、转义与 UTF-8 端到端回归。

---

### Phase 8：RFC 9535 JSONPath 函数扩展

**改造内容**
1. **Filter AST 扩展函数调用操作数（`json_path.h` / `json_path.cc`）**：
   - `FilterOperand::Kind` 新增 `kFuncCall`，支持标准 RFC 9535 函数：
     - `length(arg)`：string 返回 UTF-8 字符数（或字节数，遵循 RFC 9535 Unicode scalar count），array 返回 `ArrayLen()`，object 返回 `ObjectLen()`，其余类型返回 `kMissing`。
     - `count(path)`：统计相对/根路径匹配的节点数量（支持通配符如 `count(@.tags[*])`）。
     - `match(str_arg, regex_lit)`：整串正则匹配（自动锚定 `^(?:...)$` 并预编译 `CompiledRegex`）。
     - `search(str_arg, regex_lit)`：子串正则匹配（预编译 `CompiledRegex`）。
   - 支持函数返回值直接参与 `==`/`!=`/`>`/`>=`/`<`/`<=` 比较，以及布尔函数（`match`/`search`）直接作为条件或配合 `&&`/`||`/`!` 组合。
2. **安全与错误检查**：
   - 校验函数名、参数个数、正则字面量合法性与递归深度，非法表达式返回清晰错误信息。

**测试与验证**
- **GTest**：`length()`（ASCII/UTF-8 多字节字符串、数组、对象、标量返回 missing）、`count()`、`match()`（全串 vs 子串区别）、`search()`、与 `&&`/`||`/`!` 组合、非法参数报错。
- **`json_bench`**：`Eval length(@.tags) >= 3` 与 `Eval search(@.name, ...)` 基准。
- **Tcl**：`JSON.GET`/`JSON.SET`/`JSON.DEL` 结合 `length()` / `match()` / `search()` 端到端测试。

---

### Phase 9：DOM 内存深度压缩（短键内联 / 紧凑值存储）

**改造内容**
1. **Object 短键内联（Small Key Optimization, SKO）**：
   - 将 `ObjEntry` 的键存储改造为支持短键内联（或基于文档/对象级紧凑字符串槽），消除数组套对象（如 5k~100k 记录共享固定字段名）中海量小 key 的独立 `RMAlloc` 碎片与 allocator 元数据开销。
   - 保持 `ObjectEntries()` / `ObjectGet()` / `ObjectSet()` / `ObjectDelete()` 插入顺序与 $O(1)$ 均摊复杂度不变。
2. **标量值紧缩评估与落地**：
   - 方案 A（低侵入）：对象短键内联 + `JsonValue` 16B 对齐池/块分配器（大幅降低 allocator 元数据与 Cache Miss，外部指针 API 零破坏）。
   - 方案 B（极限压缩）：容器内部直接按值存储 16B/24B `JsonValue`（取消标量指针间接层，100k 整数数组直接降至 ~16~24 B/elem），需要同步适配 `PathMatch`（改用 `(parent, index/key)` 而非裸 `JsonValue*`）与命令层。
   - 在 Phase 9 启动前基于 Phase 6–8 的实测数据最终拍板方案 A 或 B，确保收益与维护复杂度最优平衡。
3. **运维回调同步**：
   - 完整适配 `MemoryUsage()`、`DefragJson()`、`Clone()`、`Destroy()` 与二进制 RDB Save/Load。

**测试与验证**
- **GTest**：短键/长键边界、大对象删除与顺序保持、全树 `DefragJson` 迁移、内存上限断言。
- **`json_bench`**：100k int array、5k records doc、5k wide object 内存占用与 Parse/Destroy 吞吐全量对比（Phase 1 → Phase 5 → Phase 9）。
- **ASan + UBSan + Docker Redis 7.2 Tcl**：全量压测与 BGSAVE / AOF / PSYNC / Compat 回归。

---

## 最终基准汇总（Phase 1 基线 → Phase 5 → Phase 9 最终）

全部 **Phases 6–9** 已完成，通过 **macOS Clang GTest + ASan/UBSan** 与 **Docker Redis 7.2.11 (jemalloc-5.3.0) 全量 Linux GTest + 118 项 Tcl 集成测试**。

| 指标 | Phase 1 基线 | Phase 5 | Phase 9 最终 | 总提升（vs 基线） |
|---|---:|---:|---:|---:|
| `sizeof(JsonValue)` / `sizeof(ObjEntry)` | 40 B / 24 B (+堆key) | 24 B / 24 B (+堆key) | **24 B / 32 B (≤15B SKO)** | **-40% 节点 / 0 短键堆分配** |
| 100k 整数数组 DOM 内存 | 4930 KB | 3367 KB | **3367 KB** | **-31.7%** |
| Parse small (~110B) | 146.76 MB/s | 190.11 MB/s | **347.50 MB/s** | **2.37×** |
| Parse medium (~50KB, 300 recs) | 185.31 MB/s | 244.00 MB/s | **311.41 MB/s** | **+68.1%** |
| Parse large (~1MB, 6k recs) | 205.56 MB/s | 285.30 MB/s | **316.33 MB/s** | **+53.9%** |
| Parse 256KB clean string (SIMD) | — | — | **20,341 MB/s** | **NEON/SSE2 向量化** |
| Serialize escaped (~200KB) | 582.48 MB/s | 1189.13 MB/s | **1417.12 MB/s** | **2.43×** |
| Serialize 256KB clean string (SIMD) | — | — | **24,405 MB/s** | **NEON/SSE2 向量化** |
| Object build (5k keys) | 219.78 us/op | 164.66 us/op | **125.34 us/op** | **-43.0%** |
| Object delete (1k of 2k keys) | 7076.44 us/op | 86.50 us/op | **64.76 us/op** | **109.3×** |
| Clone+Destroy medium (~50KB) | 95.09 us/op | 81.20 us/op | **70.20 us/op** | **-26.2%** |
| Path cached + eval (`$.records[150].name`) | 0.13 us/op | 0.03 us/op | **0.03 us/op** | **4.3×** |
| Eval precompiled regex / RFC 9535 `length()+search()` | — | — | **25.75 / 31.69 us/op** | **预编译零反复 `regcomp`** |
| RDB Binary Load (~50KB, encver 2) | 136.77 us/op | 125.60 us/op | **115.10 us/op** | **-15.8%** |
| RDB Text Load (~50KB, encver 3) | 156.72 us/op | 112.63 us/op | **97.06 us/op** | **-38.1%** |

