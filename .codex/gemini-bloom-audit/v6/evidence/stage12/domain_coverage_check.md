# Stage 12 Required Domain Coverage Check

| Domain | Status | Required terms | Report files |
| --- | --- | --- | --- |
| RDB | PASS | RDB | 00_审计总览.md, 01_DESIGN约束与结论对齐.md, 02_源码实现审计.md, 04_RedisBloom兼容性矩阵.md, 05_持久化迁移复制审计.md, 06_安全与资源边界.md, 07_问题清单与复现.md, 08_测试覆盖与未覆盖.md, 09_最终结论与修复优先级.md, 10_报告自审结果.md |
| DUMP/RESTORE | PASS | DUMP/RESTORE | 00_审计总览.md, 01_DESIGN约束与结论对齐.md, 04_RedisBloom兼容性矩阵.md, 05_持久化迁移复制审计.md, 08_测试覆盖与未覆盖.md |
| MIGRATE | PASS | MIGRATE | 00_审计总览.md, 01_DESIGN约束与结论对齐.md, 04_RedisBloom兼容性矩阵.md, 05_持久化迁移复制审计.md, 08_测试覆盖与未覆盖.md |
| fullsync | PASS | fullsync + psync | 05_持久化迁移复制审计.md |
| RDB-preamble AOF | PASS | RDB-preamble + aof-use-rdb-preamble yes | 05_持久化迁移复制审计.md |
| command-AOF no-preamble | PASS | command-AOF + aof-use-rdb-preamble no | 05_持久化迁移复制审计.md |
| SCANDUMP/LOADCHUNK private protocol | PASS | SCANDUMP/LOADCHUNK + 私有 | 00_审计总览.md, 01_DESIGN约束与结论对齐.md, 04_RedisBloom兼容性矩阵.md, 05_持久化迁移复制审计.md, 07_问题清单与复现.md, 09_最终结论与修复优先级.md |
| RESP3 unsupported | PASS | RESP3 + 不支持 | 01_DESIGN约束与结论对齐.md, 04_RedisBloom兼容性矩阵.md, 10_报告自审结果.md |
| fuzz | PASS | fuzz + 恶意输入 | 06_安全与资源边界.md |
| sanitizer | PASS | sanitizer + ASAN + UBSAN + valgrind | 06_安全与资源边界.md, 08_测试覆盖与未覆盖.md, 09_最终结论与修复优先级.md |
| replica | PASS | replica + 复制 | 00_审计总览.md, 05_持久化迁移复制审计.md, 07_问题清单与复现.md, 09_最终结论与修复优先级.md |
| cluster | PASS | cluster + ASK + MOVED | 05_持久化迁移复制审计.md, 08_测试覆盖与未覆盖.md |
| perf/resource | PASS | 性能 + 资源 + capacity=2^30 | 06_安全与资源边界.md |
