# gemini-bloom 审计说明

审计对象：`lostpigxx/gemini-module/modules/gemini-bloom`，按 GitHub 默认分支 `main` 读取当前源码。  
审计日期：2026-06-17。  
范围：Bloom Filter 核心算法、Redis 命令层、RDB/SCANDUMP/LOADCHUNK/AOF 序列化、配置、构建、单测与 TCL 集成测试。  
对照对象：Redis 官方命令文档、RedisBloom 当前源码中的 `src/sb.c`、`src/sb.h`、`deps/bloom/bloom.c`、`deps/bloom/bloom.h`、`src/rebloom.c`。

## 重要限制

容器内直接 `git clone https://github.com/lostpigxx/gemini-module` 失败，报错为 DNS 无法解析 `github.com`。本次审计基于 GitHub connector/web 获取的当前源码文本和官方源码文本完成，没有在本地实际编译、运行 GTest、运行 TCL 集成测试或统计覆盖率。因此：

- “确定”项：来自源码直接推导、官方文档/源码直接对照，或能够从控制流静态推出。
- “待验证”项：依赖 Redis Module API 运行时行为、平台编译器行为、客户端兼容策略或需要运行 Redis/RedisBloom 做金样本比对。
- 下面 8 个文件按用户指定维度拆分，同一根因若影响多个维度，会在不同文件中以不同角度出现，并用交叉引用说明。

## 严重级别

| 级别 | 含义 |
|---|---|
| P0 | 可能导致崩溃、越界、未定义行为、数据损坏、错误持久化或恢复失败 |
| P1 | 与 RedisBloom/命令协议不兼容，或功能语义明显错误 |
| P2 | 设计、性能、维护性、测试覆盖的实质缺陷 |
| P3 | 清理项、可移植性、命名、组织、文档一致性问题 |

## 主要源码入口

| 文件 | 关注点 |
|---|---|
| `modules/gemini-bloom/src/bloom_filter.{h,cc}` | 单层 Bloom filter、hash、bit addressing、参数计算 |
| `modules/gemini-bloom/src/sb_chain.{h,cc}` | scalable bloom filter、多层扩容、item count、内存生命周期 |
| `modules/gemini-bloom/src/bloom_commands.cc` | `BF.*` Redis 命令解析与回复 |
| `modules/gemini-bloom/src/bloom_rdb.{h,cc}` | RDB、SCANDUMP/LOADCHUNK、AOF rewrite、mem_usage |
| `modules/gemini-bloom/src/bloom_config.{h,cc}` | module load 参数和默认配置 |
| `modules/gemini-bloom/tests/*` | GTest 单元测试和 TCL 集成测试 |

## 输出文件

1. `01_code_bugs.md`
2. `02_redis_bloom_compatibility.md`
3. `03_function_design.md`
4. `04_performance.md`
5. `05_directory_organization.md`
6. `06_implementation_details.md`
7. `07_tests_coverage.md`
8. `08_other_issues.md`
