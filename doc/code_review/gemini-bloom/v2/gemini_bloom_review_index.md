# gemini-bloom 挑刺报告索引

分析对象：`https://github.com/lostpigxx/gemini-module` 中 `modules/gemini-bloom` 当前 `main` 分支源码。  
分析方式：静态源码审查 + 与 RedisBloom 当前官方源码/命令文档对照。未在本地编译运行。

## 文件列表

1. `gemini_bloom_01_code_bugs.md`  
   代码 bug、未定义行为、崩溃/数据损坏风险、错误回复和危险边界。

2. `gemini_bloom_02_redis_bloom_compatibility.md`  
   与 RedisBloom 的命令、RDB、SCANDUMP/LOADCHUNK、RESP3、ACL、错误语义兼容性差异。

3. `gemini_bloom_03_functional_design_issues.md`  
   产品定位、导入事务性、资源上限、配置、可观测性和用户语义问题。

4. `gemini_bloom_04_performance_issues.md`  
   hot path、层数退化、SCANDUMP/AOF 大 chunk、BF.INFO/mem_usage 复杂度等性能问题。

5. `gemini_bloom_05_directory_organization_issues.md`  
   README/CMake 不一致、测试组织、公共 include 污染、模块边界和文档结构问题。

6. `gemini_bloom_06_implementation_details.md`  
   C++/Redis Module API 实现细节、类型/invariant、wire ABI、parser、RESP 封装等问题。

7. `gemini_bloom_07_test_coverage_gaps.md`  
   当前测试覆盖总结，以及 RedisBloom golden、fuzz、sanitizer、RESP3、边界、OOM、replication、coverage、benchmark 缺口。

## 本地复核修正记录

复核 `modules/gemini-bloom` 当前代码后，以下第二版 review 结论被判定为结论或严重性过度，已在对应文件中修正：

- `BUG-13`：固定容量 filter 已满时 false positive 返回 duplicate 是 Bloom/RedisBloom 语义边界，不应列为 P1 实现 bug；降为 P3 文档/语义说明。
- `BUG-14` / `IMPL-04`：`RawBits` 当前公开命令路径不暴露，LOADCHUNK header 也拒绝 `hashCount=0` 的普通 bit-array 元数据；保留为 latent invariant/兼容性问题，但严重性下调。
- `COMPAT-12`：缺少 `BF.DEBUG` 是源码级/诊断命令差异，不是公开 BF 命令核心语义差异；由 P1 下调为 P3。
- `PERF-04`：`BF.INFO`/`mem_usage` O(numLayers) 在极端层数下有风险，但不是常规 hot path P1 blocker；下调为 P2。
- `ORG-02` / `IMPL-12`：测试 mock 位置和 POSIX `strncasecmp` 属于工程整洁度/可移植性问题，原严重性偏高，已下调。

## 最优先修复建议

P0/P1 优先顺序建议：

1. 修复 `FilterLayer` 数组对象生命周期：禁止对非平凡对象使用 `realloc`，禁止向未构造对象赋值。
2. 修复 `EXPANSION` 数值截断和除零风险。
3. 为 RDB/LOADCHUNK 元数据引入统一 invariant 校验。
4. 按 RedisBloom 官方 byte-offset iterator 重写 `SCANDUMP/LOADCHUNK`。
5. 补齐 RedisBloom golden corpus，至少覆盖 RDB、SCANDUMP/LOADCHUNK、BF.INFO RESP2/RESP3。
6. 强制 ASAN+UBSAN 测试，并把 TCL 集成测试纳入 CTest。
