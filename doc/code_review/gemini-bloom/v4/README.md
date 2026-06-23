# gemini-bloom v4 代码审计

本目录是对当前 `modules/gemini-bloom` 的第四版审计结果。结论基于当前工作区源码、现有 v1/v2/v3 审计材料、官方 Redis 命令文档、RedisBloom upstream 源码片段，以及本轮实际执行的测试。

## 文件索引

- `00_scope_and_method.md`：审计范围、测试基线、严重级别定义、相对 v3 的变化。
- `01_code_bugs.md`：当前仍可能导致错误行为、恢复损坏、崩溃或 DoS 的代码问题。
- `02_redis_bloom_compatibility.md`：与 Redis/RedisBloom 协议、返回形状、配置、持久化格式的差异。
- `03_function_design.md`：功能边界、用户语义、恢复模型和长期演进问题。
- `04_performance.md`：大 filter、分块、扩容、持久化和内存估算方面的问题。
- `05_directory_organization.md`：目录、模块职责、测试夹具和文档归属问题。
- `06_implementation_details.md`：实现细节、校验、生命周期、flag、parser、AOF 等工程风险。
- `07_tests_coverage.md`：现有测试覆盖与缺失测试矩阵。
- `08_other_issues.md`：安全边界、版本、兼容声明、许可证和运维可观测性等其他问题。

## 顶层结论

当前实现的基本 BF 命令自洽可用，本轮 GTest 和 TCL 集成测试均通过。但它仍不应声明为 RedisBloom drop-in compatible，主要阻断点是：

1. `BF.SCANDUMP` / `BF.LOADCHUNK` 使用 layer-index 私有协议，而 RedisBloom 使用 byte-offset 增量 chunk 协议。
2. `BF.INFO` 单字段 RESP2 返回形状与 RESP3 map 返回形状不兼容。
3. RESP3 下 `BF.ADD` / `BF.EXISTS` / multi 命令仍返回 integer，而 Redis 文档要求 boolean。
4. RDB 与 wire header 的完整性校验不统一，RDB 路径仍可接受非法 layer metadata。
5. 使用 RedisBloom data type name `MBbloom--`，但没有官方 golden corpus 证明 RDB/AOF/SCANDUMP 可互通。

## 本轮测试基线

通过：

```text
GTest:
  bloom_filter_test: 11 passed
  sb_chain_test:     12 passed
  bloom_rdb_test:    24 passed

TCL integration:
  bloom_test.tcl:    89 passed, 0 failed
```

注意：这些测试主要证明 gemini-bloom 自身路径可用，不证明 RedisBloom 兼容。

