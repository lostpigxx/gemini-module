# gemini-json 设计文档

## 1. 概述

### 1.1 产品定位
`gemini-json`（`redis_json.so`）是面向 **Redis 7**（基线 Redis 7.2.11）的 Clean-room C++20 JSON 数据类型模块，提供与 RedisJSON 命令集兼容的 JSONPath 读写、数组/对象/字符串/数值原地操作、RFC 7396 Merge Patch、RDB/AOF 持久化及与 `gemini-search`（`ON JSON`）联动的 keyspace 通知。

### 1.2 支持命令列表

| 类别 | 命令 |
|---|---|
| 读写与合并 | `JSON.SET`, `JSON.GET`, `JSON.MGET`, `JSON.MSET`, `JSON.MERGE`, `JSON.DEL`, `JSON.FORGET` |
| 类型与状态 | `JSON.TYPE`, `JSON.CLEAR`, `JSON.TOGGLE`, `JSON.RESP`, `JSON.DEBUG` |
| 数组操作 | `JSON.ARRAPPEND`, `JSON.ARRINDEX`, `JSON.ARRINSERT`, `JSON.ARRLEN`, `JSON.ARRPOP`, `JSON.ARRTRIM` |
| 字符串操作 | `JSON.STRAPPEND`, `JSON.STRLEN` |
| 数值操作 | `JSON.NUMINCRBY`, `JSON.NUMMULTBY` |
| 对象操作 | `JSON.OBJKEYS`, `JSON.OBJLEN` |

- 全命令已接入 Redis 7 动态 `KeySpecs`（含 `JSON.MGET`、`JSON.MSET`、`JSON.DEBUG`），支持 Cluster 路由校验与 ACL。

---

## 2. 架构与数据结构设计

### 2.1 源码结构

| 文件 | 职责 |
|---|---|
| `json_value.h / .cc` | 紧凑 DOM（`JsonValue`、`ObjEntry`、Object 哈希索引、深度限制 `kMaxJsonDepth = 128`） |
| `json_simd.h` | 跨平台字符扫描内核（ARM NEON / x86-64 SSE2 / 64-bit SWAR 回退） |
| `json_parse.h / .cc` | 高吞吐递归下降 JSON 解析器（含深度/长度/非法转义防护） |
| `json_serialize.h / .cc` | 向量化块拷贝 JSON 序列化器与缩进/格式选项 |
| `json_path.h / .cc` | JSONPath 解析器、LRU 路径缓存、预编译正则与 RFC 9535 Filter 求值引擎 |
| `json_type.h / .cc` | Redis 7 `RedisModuleType` 回调（RDB、AOF、`mem_usage2`、`copy2`、`free_effort2`、`defrag`、`digest`） |
| `json_commands.h / .cc` | 全部 `JSON.*` 命令入口、参数校验、KeySpecs 与 keyspace 通知 |
| `redis_json_module.cc` | `RedisModule_OnLoad` 与 `COMPAT` 模式开关 |
| `rm_alloc.h` | `RedisModule_Alloc` / 标准分配器测试桩统一封装 |

### 2.2 紧凑 DOM 内存布局（Phases 1–9）
- **`JsonValue`（24 字节）**：
  - 区分 `kNull`、`kBool`、`kInteger`（`int64_t`）、`kNumber`（`double`）、`kString`、`kArray`、`kObject`。
  - **短字符串内联（SSO）**：`<= 21` 字节的字符串直接内联在 24B 节点中，零额外堆分配。
- **`ObjEntry`（32 字节）与有序哈希对象**：
  - **短键内联（SKO）**：`<= 15` 字节的对象键直接内联在 `ObjEntry` 内部，消除数组套对象（如数千条同结构记录）中海量字段名的独立堆分配碎片。
  - 对象同时维护插入顺序数组 `entries` 与 2 的幂开放寻址哈希索引 `hash_idx`，保证按插入顺序遍历的同时实现均摊 $O(1)$ 查找/更新/删除（墓碑标记 + 阈值压缩）。

### 2.3 SWAR / SIMD Parser & Serializer
- `json_simd.h` 提供 `FindNextSpecialInString`（检测 `"`、`\`、`< 0x20`、`>= 0x80`）与 `FindNextEscapeInSerialize`（检测 `"`、`\`、`< 0x20`）。
- 编译期自动分派 ARM64 NEON（16B/step）、x86-64 SSE2（16B/step）或通用 64-bit SWAR（8B/step），尾部不足向量宽度安全落回标量循环，ASan/UBSan 零越界。

### 2.4 JSONPath 与 RFC 9535 过滤引擎
- **路径缓存**：所有 `JSON.*` 命令统一通过 `ParsePathCached` 复用解析后的 `JsonPath` AST，热路径解析开销接近零。
- **正则预编译**：Filter 中的正则模式在解析阶段一次性 `regcomp` 为 RAII `CompiledRegex`（`std::shared_ptr`），求值期直接 `regexec`。
- **RFC 9535 函数**：支持 `length()`、`count()`、`match()`、`search()`，可与比较运算及 `&&`/`||`/`!` 任意组合。
- **DoS 求值预算**：对递归下降（`..`）、通配符展开与 Filter 求值步数设置上限，防止畸形路径打挂主线程。

---

## 3. 持久化、运维回调与 `gemini-search` 联动

### 3.1 RDB 双模式持久化
- **原生二进制模式（默认，`encver = 2`）**：按紧凑类型标签直接序列化 DOM 树，跳过二次 JSON 文本解析。
- **兼容文本模式（`MODULE LOAD redis_json.so COMPAT`，`encver = 3`，type name `ReJSON-RL`）**：写出 UTF-8 JSON 文本，支持与 RedisJSON 之间的 RDB 迁移；同时兼容读取旧版 `encver 2` 二进制 RDB。
- 反序列化包含完整的字节级截断、非法类型/长度、超深嵌套（`> 128`）防护，经字节级模糊测试验证。

### 3.2 Redis 7 ModuleType 回调
- `AofRewriteJson`：写出 `JSON.SET <key> $ <json>`。
- `CopyJson2`：独立深拷贝 DOM 树（支持 Redis 7 `COPY`）。
- `FreeEffortJson2`：按子树规模返回释放代价，使大文档 `UNLINK` 走异步 lazy-free。
- `DefragJson`：递归迁移 `JsonValue`、数组、对象表、长字符串与堆键缓冲区，支持 `activedefrag`。
- `JsonMemUsage` / `DigestJson`：精确统计 DOM 内存占用与确定性摘要。

### 3.3 Keyspace 通知
- 所有写命令（`JSON.SET`/`DEL`/`MERGE`/`ARR*`/`STR*`/`NUM*`/`CLEAR`/`TOGGLE`）均调用 `RedisModule_NotifyKeyspaceEvent`，触发 `gemini-search` 的 `ON JSON` 实时增量建索引。

---

## 4. 性能基线与测试体系

### 4.1 性能基线（`json_bench`，Apple Silicon ARM64 NEON）

| 指标 | 实测结果 |
|---|---:|
| `sizeof(JsonValue)` / `sizeof(ObjEntry)` | `24 B` / `32 B`（`<=21B` SSO / `<=15B` SKO） |
| Parse small (`~110 B`) / large (`~1 MB`) | `347.5 MB/s` / `316.3 MB/s` |
| Parse / Serialize 256 KB clean string (SIMD) | `20.3 GB/s` / `24.4 GB/s` |
| Serialize escaped (`~200 KB`) | `1,417 MB/s` |
| Path cached + eval (`$.records[150].name`) | `0.03 us/op` |
| RDB Binary / Text Load (`~50 KB`) | `115.1 us/op` / `97.1 us/op` |

详细演进记录见 [audit-and-optimization-plan.md](../../doc/gemini-json/audit-and-optimization-plan.md) 与 [optimization-plan-v2.md](../../doc/gemini-json/optimization-plan-v2.md)。

### 4.2 测试矩阵
- **GTest（普通 + ASan/UBSan）**：6 套单测（`json_value_test`、`json_parse_test`、`json_path_test`、`json_rdb_test`、`json_asan_test`、`json_stress_test`），含 RDB 截断/篡改模糊测试。
- **Tcl 集成测试**：`modules/gemini-json/tests/tcl/json_test.tcl`（118+ 用例，含 `COPY`、`activedefrag`、KeySpecs、Compat RDB）。
- **SDK 兼容测试 & Soak**：`ci/sdk_compat_test.py`（`redis-py` RESP2/RESP3）与 `ci/json_soak_test.py`（RESP2/RESP3 长稳压测）。
