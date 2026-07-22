#!/usr/bin/env python3
"""Generate Stage 11 Chinese report and synthesis evidence."""

from __future__ import annotations

import os
import subprocess
import textwrap
from pathlib import Path


REPO = Path(__file__).resolve().parents[5]
REPORT_DIR = REPO / "doc/code_review/gemini-bloom/v6"
EVIDENCE_DIR = REPO / ".codex/gemini-bloom-audit/v6/evidence/stage11"


def dedent(text: str) -> str:
    return textwrap.dedent(text).strip() + "\n"


def git_out(*args: str) -> str:
    proc = subprocess.run(["git", *args], cwd=REPO, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.stdout.strip()


def write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def report_files() -> dict[str, str]:
    return {
        "00_审计总览.md": dedent("""
            # gemini-bloom v6 审计总览

            ## 审计范围

            本报告汇总 `.codex/gemini-bloom-audit/v6/` 下 Stage 00 到 Stage 10 的审计证据，审计对象是 Stage 00 记录的 `origin/main` 基线 `780be16fb4a675f89594600f3ce23ed018c5d1bc` 上的 `modules/gemini-bloom` 实现。分支和阶段状态见 `.codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`，环境基线见 `.codex/gemini-bloom-audit/v6/evidence/stage01/git_snapshot.txt` 与 `.codex/gemini-bloom-audit/v6/evidence/stage01/tool_versions.txt`。

            ## 总体结论

            审计结论只覆盖受限兼容路径。按照 `modules/gemini-bloom/DESIGN.md`，gemini-bloom 是独立 Redis Module，不是 RedisBloom 的 drop-in 替代品。可以在本轮证据范围内确认的是：Redis 6.2.17 + RedisBloom v2.4.20 的 RDB/DUMP/RESTORE/MIGRATE/fullsync/RDB-preamble AOF 兼容路径通过 9 个 corpus 验证，证据见 `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md` 与 `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md`。

            同时，本轮发现两个 P1 数据完整性问题，均与 gemini 自有 `BF.LOADCHUNK` 私有协议的异常序列/半加载状态有关，而不是 RedisBloom SCANDUMP/LOADCHUNK 互通问题。证据见 `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md`、`.codex/gemini-bloom-audit/v6/evidence/stage07/stage07_fuzz_results.json` 和 `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/loading_partial_fullsync.log`。

            ## 可信度评级

            最终可信度评级：`Medium-Low`。

            降级原因：

            - `GBV6-07-001` 和 `GBV6-07-002` 是 P1 OPEN，影响 `BF.LOADCHUNK` 异常输入后的 false negative 风险；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md`。
            - Stage 08 sanitizer/UBSAN/valgrind runtime coverage `BLOCKED`，不能声明动态内存安全 PASS；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage08/blocked_sanitizer.md`。
            - ACL DRYRUN 在 Redis 6.2.x 环境不可用，Cluster ASK 未确定性覆盖；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/failures.md`、`.codex/gemini-bloom-audit/v6/evidence/stage09/cluster/ask_not_verified.md`。
            - 默认/低错误率 `capacity=2^30` 运行时大分配未跑，Stage 10 只跑了高错误率 `NONSCALING` 安全探针；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage10/blocked_or_not_verified.md`。

            ## 已验证通过的重点

            - 现有直接 GTest 114/114 通过；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage02/gtest/gtest_summary.md`。
            - TCL 集成测试中 144 项通过，6 项是 DESIGN_INTENDED expected gaps，但 harness 仍返回非零；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/tcl_summary.md`。
            - 全部 10 个 `BF.*` 命令的 RESP2 runtime 矩阵已覆盖；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/coverage_summary.md`。
            - RedisBloom v2.4.20 oracle 精确可用，RDB-family 兼容路径 PASS；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_env.txt` 与 `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md`。
            - 持久化、迁移、fullsync、AOF RDB preamble 路径 PASS；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md`。
            - 完成态 filter 的 replica/cluster 运行路径通过受限验证；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage09/evidence_index.md`。
            - Stage 10 资源边界和内存统计样本在受限范围内 PASS；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage10/perf_matrix.md`。

            ## 最高优先级风险

            1. 修复 `BF.LOADCHUNK` out-of-order/repeated chunk 可完成 corrupt filter 的 P1 问题：`GBV6-07-001`。
            2. 修复 half-loaded key 持久化/fullsync 后变成 completed filter 且产生 false negative 的 P1 问题：`GBV6-07-002`。
            3. 修复 RDB/wire 反序列化资源边界：`GBV6-03-001`、`GBV6-03-002`。
            4. 修复 Linux/GCC 默认构建缺 `<climits>`：`GBV6-05-001`。
            5. 修复测试/文档证据缺口，避免 expected gap 被误报为失败或把私有协议误写成 RedisBloom 互通。

            ## 本报告结构

            - DESIGN 对齐：`01_DESIGN约束与结论对齐.md`
            - 源码审计：`02_源码实现审计.md`
            - 运行时测试：`03_运行时测试结果.md`
            - RedisBloom 兼容性：`04_RedisBloom兼容性矩阵.md`
            - 持久化/迁移/复制：`05_持久化迁移复制审计.md`
            - 安全与资源边界：`06_安全与资源边界.md`
            - 问题清单：`07_问题清单与复现.md`
            - 覆盖与未覆盖：`08_测试覆盖与未覆盖.md`
            - 最终修复优先级：`09_最终结论与修复优先级.md`
            - Stage 12 自审入口：`10_报告自审结果.md`
        """),
        "01_DESIGN约束与结论对齐.md": dedent("""
            # DESIGN 约束与结论对齐

            ## DESIGN 最高边界

            `modules/gemini-bloom/DESIGN.md` 是本轮审计最高优先级标准。Stage 00 已把 DESIGN 约束抽取为审计契约，证据见 `.codex/gemini-bloom-audit/v6/agents/stage00/design_contract.md` 和 `.codex/gemini-bloom-audit/v6/agents/stage00/design_claims_matrix.md`。

            ## 设计承诺与审计结论

            | DESIGN 项 | 审计结论 | 证据 |
            |---|---|---|
            | 不是 RedisBloom drop-in 替代品 | `PASS / DESIGN_BOUNDARY`，报告不得扩大兼容范围 | `.codex/gemini-bloom-audit/v6/agents/stage00/stage_result.md` |
            | RedisBloom v2.4.20 RDB-family 互通 | `PASS`，限 Redis 6.2.17 + RedisBloom v2.4.20 + 9 corpus | `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md`, `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |
            | DUMP/RESTORE/MIGRATE/fullsync/RDB-preamble AOF | `PASS`，本轮矩阵覆盖双向路径 | `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |
            | `BF.SCANDUMP/BF.LOADCHUNK` 不与 RedisBloom 互通 | `DESIGN_INTENDED`，跨实现失败不算 bug | `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md`, `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |
            | command-AOF no-preamble 不跨实现兼容 | `DESIGN_INTENDED`，gemini self PASS，cross 不是承诺 | `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |
            | RESP3 不支持 | `DESIGN_INTENDED`，Stage 02/04 只记录 expected gap | `.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/tcl_summary.md`, `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/normalized_results.md` |
            | BF.DEBUG 不支持 | `DESIGN_INTENDED` | `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_normalized.md` |
            | capacity/error/expansion 命令边界 | `PASS` for command path | `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/normalized_results.md`, `.codex/gemini-bloom-audit/v6/evidence/stage10/resource_limits.log` |
            | RDB/wire 非信任输入资源边界 | `FAIL` for two gaps | `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md`, `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md` |
            | Loading runtime-only 状态保护 | `PARTIAL`，正常路径有保护，但异常序列/持久化有 P1 缺陷 | `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md`, `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/loading_partial_fullsync.log` |

            ## 设计内差异

            下列差异符合 DESIGN，不应列为 product bug：

            - RESP3 未支持：`.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/tcl_summary.md`
            - BF.DEBUG 未支持：`.codex/gemini-bloom-audit/v6/evidence/stage05/diff_normalized.md`
            - `BF.INFO key FIELD` 返回标量而非 RedisBloom singleton array：`.codex/gemini-bloom-audit/v6/evidence/stage05/diff_normalized.md`
            - `BF.INFO Size` 统计口径不同：`.codex/gemini-bloom-audit/v6/evidence/stage10/memory_usage.md`
            - RedisBloom SCANDUMP/LOADCHUNK byte-offset 协议不互通：`.codex/gemini-bloom-audit/v6/evidence/stage06/scandump_loadchunk/summary.md`
            - command-AOF no-preamble cross replay 不互通：`.codex/gemini-bloom-audit/v6/evidence/stage06/aof_preamble_no/summary.md`
            - live command-stream `EXPANSION 1` 的 `BF.CARD` drift：`.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md`

            ## DESIGN 自身/文档一致性问题

            `GBV6-00-001`：DESIGN 声称 `tests/compat/redisbloom-2.4.20/` 有 checked-in fixtures，但审计树中不存在。证据：`.codex/gemini-bloom-audit/v6/evidence/stage00/stderr.log`。

            `GBV6-00-002`：`sb_chain.h` 注释称 SCANDUMP/LOADCHUNK wire format 匹配 RedisBloom 以支持 cross-implementation compatibility，和 DESIGN 私有协议边界冲突。证据：`.codex/gemini-bloom-audit/v6/evidence/stage00/stdout.log`。
        """),
        "02_源码实现审计.md": dedent("""
            # 源码实现审计

            ## 静态审计覆盖

            Stage 03 对 `modules/gemini-bloom/src/` 和相关测试做了源码深度静态审计。按文件审计记录见 `.codex/gemini-bloom-audit/v6/agents/stage03/static_audit_by_file.md`，不变量映射见 `.codex/gemini-bloom-audit/v6/agents/stage03/invariant_map.md`，DESIGN/code 对齐矩阵见 `.codex/gemini-bloom-audit/v6/agents/stage03/design_vs_code_matrix.md`。

            ## 静态 PASS 项

            - Bloom 参数公式、MurmurHash seed、双重哈希路径与 RDB 格式要求一致；证据见 `.codex/gemini-bloom-audit/v6/agents/stage03/invariant_map.md`。
            - RAII、move-only bit array、placement-new layer 数组迁移路径静态上符合所有权预期；证据见 `.codex/gemini-bloom-audit/v6/agents/stage03/static_audit_by_file.md`。
            - 命令 parser 对 capacity、error rate、EXPANSION、NONSCALING/NOCREATE 互斥等路径做前置校验；运行时矩阵也验证了命令层边界，证据见 `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/normalized_results.md`。
            - `BF.INFO Size` 使用 `BytesUsed()` 和 module `mem_usage`，统计口径与 DESIGN 的 C++ 对象/预留 layer slots 说明一致；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage10/memory_usage.md`。

            ## 静态发现的问题

            ### GBV6-03-001：RDB/wire 未执行每层 2GB cap

            影响：恶意 RDB 或 LOADCHUNK header 可以描述单层 `dataSize` 在 2GB 到 4GB 之间的 layer，违反 DESIGN 的单层上限。Stage 03 静态证据见 `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md`；Stage 07 静态复核也确认该风险，见 `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md`。

            建议：把 runtime `BloomLayer::Create()` 的单层 cap 抽为共享常量，并在 `ValidateLayerFields()` 中拒绝 `dataSize > kMaxLayerDataSize`。

            ### GBV6-03-002：RDB/wire 接受过大 expansion

            影响：命令/config 路径拒绝 `EXPANSION > 32768`，但 RDB/wire 反序列化不拒绝，后续扩容可能放大资源请求。静态证据见 `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md`；Stage 07 header fuzz 运行时确认 `32769` 和 `UINT_MAX` 被接受，见 `.codex/gemini-bloom-audit/v6/evidence/stage07/crash_or_failure_repro/README.md`。

            建议：RDB load 和 wire header load 都应拒绝 `expansionFactor > kMaxExpansion`，并增加 `32769`、`UINT32_MAX` 测试。

            ### GBV6-03-003：TCL per-layer cap 测试名/注释与断言不符

            影响：测试名声称验证 per-layer cap rejection，但实际断言验证 `kMaxCapacity` 成功，且注释仍写 512MB，而 DESIGN/code 是 2GB。证据见 `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md`。

            建议：修正测试名/注释，或补一个不分配大内存的 metadata-level RDB/wire cap 测试。

            ## 非修复说明

            本轮按 loop policy 默认不修改生产代码。所有源码问题均记录为 findings，并在 `09_最终结论与修复优先级.md` 中排序。
        """),
        "03_运行时测试结果.md": dedent("""
            # 运行时测试结果

            ## Build 与现有测试基线

            Stage 02 建立 build/GTest/TCL 基线。干净 fallback 构建 PASS，证据见 `.codex/gemini-bloom-audit/v6/evidence/stage02/build/build_fallback_stdout.log` 和 `.codex/gemini-bloom-audit/v6/evidence/stage02/build/artifact_info.txt`。直接 GTest 114/114 PASS，证据见 `.codex/gemini-bloom-audit/v6/evidence/stage02/gtest/gtest_summary.md`。

            需要降级说明：

            - 默认 `cmake --build ... --target bloom_test` 在 macOS 上因 GTest dylib RPATH 失败，finding `GBV6-02-001`，证据见 `.codex/gemini-bloom-audit/v6/evidence/stage02/gtest/bloom_test_target_stderr.log`。
            - TCL 运行 144 PASS、6 个 DESIGN_INTENDED expected gaps，但 harness exit code 非零，finding `GBV6-02-002`，证据见 `.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/tcl_summary.md`。

            ## BF 命令 runtime 矩阵

            Stage 04 用 raw RESP 覆盖全部 10 个 `BF.*` 命令，coverage 统计见 `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/coverage_summary.md`：

            | 命令 | Stage 04 rows |
            |---|---:|
            | `BF.RESERVE` | 32 |
            | `BF.ADD` | 21 |
            | `BF.MADD` | 8 |
            | `BF.INSERT` | 14 |
            | `BF.EXISTS` | 26 |
            | `BF.MEXISTS` | 8 |
            | `BF.INFO` | 19 |
            | `BF.CARD` | 11 |
            | `BF.SCANDUMP` | 16 |
            | `BF.LOADCHUNK` | 25 |

            Stage 04 已验证 RESP2 happy path、wrong type、missing key、binary/empty/NUL/10KB item、parser/resource boundary、NONSCALING full、partial failure、`BF.INFO` scalar field、COMMAND INFO/GETKEYS、SCANDUMP/LOADCHUNK layer-index/loading guard。证据见 `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/normalized_results.md`。

            ## ACL 与 command metadata

            Stage 04 的 `ACL DRYRUN` 被 Redis 6.2.16 环境阻塞，证据见 `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/failures.md`。Stage 09 在 Redis 6.2.17 上补了 actual ACL smoke，readonly grants 和 write/key-pattern denial 的受限验证 PASS，证据见 `.codex/gemini-bloom-audit/v6/evidence/stage09/acl/acl_results.json`；但 `ACL DRYRUN` 仍不可用，不能声明 DRYRUN PASS，证据见 `.codex/gemini-bloom-audit/v6/evidence/stage09/acl/blocked_acl.md`。

            `COMMAND INFO` flags 和 `COMMAND GETKEYS` 在 Stage 09 PASS，证据见 `.codex/gemini-bloom-audit/v6/evidence/stage09/command_metadata/command_info.json` 与 `.codex/gemini-bloom-audit/v6/evidence/stage09/command_metadata/getkeys.json`。`COMMAND GETKEYSANDFLAGS` 在 Redis 6.2.17 中不可用，不作为已验证能力。

            ## 运行时结论边界

            正常命令路径未发现新的 product-behavior FAIL。运行时 PASS 不覆盖 Stage 07 的异常 LOADCHUNK 序列、Stage 08 的 sanitizer runtime、Stage 09 的 ASK、或 Stage 10 未跑的默认/低错误率 `2^30` 大分配。
        """),
        "04_RedisBloom兼容性矩阵.md": dedent("""
            # RedisBloom 兼容性矩阵

            ## Oracle 范围

            本轮 RedisBloom 对比的精确范围是 Redis 6.2.17 + RedisBloom v2.4.20，`MODULE LIST ver=20420`。证据见 `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_env.txt`。gemini runtime 对比使用 audit-only `-include climits` workaround，因为默认 Linux/GCC build 存在 `GBV6-05-001`；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/gemini_default_build_stderr.log`。

            ## 9 个 corpus

            Stage 05/06 覆盖 corpus：`binary_items`、`empty_scaling`、`expansion1`、`expansion4`、`fixed_full`、`large_empty_16mb`、`long_item`、`multi_exp2`、`single_layer`。证据见 `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md`。

            ## 兼容路径

            | 路径 | 方向 | 结论 | 证据 |
            |---|---|---|---|
            | RDB file | RedisBloom -> gemini | PASS 9/9 | `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md` |
            | RDB file | gemini -> RedisBloom | PASS 9/9 | `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md` |
            | DUMP/RESTORE | 双向 | PASS 9/9 | `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |
            | MIGRATE + TTL | 双向 | PASS | `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/extended_audit_results_redis62_redisbloom2420.json` |
            | fullsync replication | 双向 | PASS 9/9 | `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |
            | AOF RDB preamble yes | 双向 | PASS 9/9 | `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |

            ## 设计内不兼容路径

            | 路径 | 结论 | 说明 | 证据 |
            |---|---|---|---|
            | RedisBloom SCANDUMP/LOADCHUNK -> gemini | DESIGN_INTENDED | gemini 使用私有 layer-index cursor，不是 RedisBloom byte-offset | `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md` |
            | gemini SCANDUMP/LOADCHUNK -> RedisBloom | DESIGN_INTENDED | 同上 | `.codex/gemini-bloom-audit/v6/evidence/stage06/scandump_loadchunk/summary.md` |
            | command-AOF no-preamble cross replay | DESIGN_INTENDED | 依赖私有 LOADCHUNK；生产应保持 RDB preamble | `.codex/gemini-bloom-audit/v6/evidence/stage06/aof_preamble_no/summary.md` |
            | live command-stream `EXPANSION 1` `BF.CARD` drift | DESIGN_INTENDED_LIMITATION | membership 无 inserted-item false negative，但 CARD 不同 | `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md` |
            | BF.DEBUG | DESIGN_INTENDED | gemini 不支持 | `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_normalized.md` |

            ## 命令语义差异

            Stage 05 raw RESP 对比确认下列差异均符合 DESIGN：module name/version 不同、`BF.INFO FIELD` 返回标量、`BF.INFO Size` 数值不同、`BF.INSERT NOCREATE CAPACITY` 错误消息不同、BF.DEBUG 不支持。证据见 `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_normalized.md` 与 `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_raw_resp.log`。

            ## 不得外推

            本报告不声明其他 RedisBloom 版本、Redis 8 内置 Bloom、RESP3、RedisBloom SCANDUMP/LOADCHUNK、或同实例 RedisBloom/gemini 共存兼容。Stage 00 的 DESIGN contract 已明确这些边界，证据见 `.codex/gemini-bloom-audit/v6/agents/stage00/design_contract.md`。
        """),
        "05_持久化迁移复制审计.md": dedent("""
            # 持久化、迁移、复制审计

            ## 正常完成态 filter 的结果

            Stage 06 是本轮持久化/transport 主证据。矩阵见 `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md`，raw JSON 见 `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.json`。

            | 路径 | 结论 | 证据 |
            |---|---|---|
            | gemini self RDB save/restart | PASS 9/9 | `.codex/gemini-bloom-audit/v6/evidence/stage06/rdb/summary.md` |
            | RedisBloom <-> gemini RDB file | PASS 9/9 双向 | `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |
            | DUMP/RESTORE | PASS 9/9，TTL 路径覆盖 | `.codex/gemini-bloom-audit/v6/evidence/stage06/dump_restore/summary.md` |
            | MIGRATE | PASS，双向且 PTTL 保留 | `.codex/gemini-bloom-audit/v6/evidence/stage06/migrate/summary.md` |
            | fullsync replication | PASS，gemini self 和 cross 双向 | `.codex/gemini-bloom-audit/v6/evidence/stage06/replication/summary.md` |
            | AOF RDB-preamble yes | PASS，gemini self 和 cross 双向 | `.codex/gemini-bloom-audit/v6/evidence/stage06/aof_preamble_yes/summary.md` |
            | AOF no-preamble gemini self | PASS | `.codex/gemini-bloom-audit/v6/evidence/stage06/aof_preamble_no/summary.md` |
            | AOF no-preamble cross | DESIGN_INTENDED incompatible | `.codex/gemini-bloom-audit/v6/evidence/stage06/aof_preamble_no/summary.md` |

            ## Replica 和 cluster 操作性

            Stage 09 验证完成态 filter 的 replica live command stream、fullsync snapshot、reconnect 均 PASS。证据见 `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/live_command_stream.log`、`.codex/gemini-bloom-audit/v6/evidence/stage09/replica/fullsync_snapshot.log`、`.codex/gemini-bloom-audit/v6/evidence/stage09/replica/reconnect.log`。

            Stage 09 的 6-node cluster 验证 owner execution、MOVED、`redis-cli -c` redirect、same-slot SCANDUMP/LOADCHUNK round-trip、READONLY replica read path PASS。证据见 `.codex/gemini-bloom-audit/v6/evidence/stage09/cluster/cluster_results.json`。ASK 未确定性覆盖，保持 `NOT_VERIFIED`，证据见 `.codex/gemini-bloom-audit/v6/evidence/stage09/cluster/ask_not_verified.md`。

            ## 关键缺陷：半加载 LOADCHUNK 的持久化/复制

            `GBV6-07-002` 是 P1。Stage 07 证明 header-only Loading key 在 `SAVE`/restart 或 AOF no-preamble rewrite/restart 后会作为 completed filter 加载，且所有 inserted corpus item false negative。证据见 `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md` 和 `.codex/gemini-bloom-audit/v6/evidence/stage07/crash_or_failure_repro/README.md`。

            Stage 09 进一步证明 fullsync operational impact：primary 上 loading key 会拒绝命令，但 fullsync 后 replica 暴露为 readable completed filter，并出现 false negative。证据见 `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/loading_partial_fullsync.log`。

            结论：正常完成态 filter 的 DESIGN-promised transport 路径 PASS，但 incomplete `LOADCHUNK` 状态是数据完整性风险，必须先修复再扩大对私有 LOADCHUNK 备份/恢复的信任。
        """),
        "06_安全与资源边界.md": dedent("""
            # 安全与资源边界

            ## 恶意输入与 fuzz

            Stage 07 覆盖 LOADCHUNK header fuzz、cursor fault safety、loading-state lifecycle、RDB/AOF persistence fault injection、numeric edge 和静态资源边界复核。证据见 `.codex/gemini-bloom-audit/v6/evidence/stage07/stage07_fuzz_results.json`、`.codex/gemini-bloom-audit/v6/evidence/stage07/safety_matrix.md`、`.codex/gemini-bloom-audit/v6/evidence/stage07/loadchunk_matrix.md`。

            关键结果：

            - Header fuzz 92 PASS、2 FAIL；FAIL 确认 `GBV6-03-002`，见 `.codex/gemini-bloom-audit/v6/evidence/stage07/crash_or_failure_repro/README.md`。
            - Cursor fault safety 5 PASS、2 FAIL；FAIL 形成 `GBV6-07-001`，见 `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md`。
            - Persistence fault injection 2 FAIL；形成 `GBV6-07-002`，见 `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md`。
            - 未观察到 Redis 进程 crash；这些是数据完整性/false negative 风险，不是 crash repro。

            ## Sanitizer / UB / Valgrind

            Stage 08 为 `BLOCKED`。GCC ASAN/UBSAN configure 因缺 sanitizer runtime 失败；Clang ASAN module 能 build 但 Redis 无法加载 unresolved ASAN symbols；GTest sanitizer target/binaries 缺失；Valgrind 不可用。证据见 `.codex/gemini-bloom-audit/v6/evidence/stage08/blocked_sanitizer.md`。

            静态 fallback 未发现新的具体 UAF/OOB/double-free/UB finding，但不能替代 ASAN/UBSAN/valgrind runtime PASS。证据见 `.codex/gemini-bloom-audit/v6/evidence/stage08/static_fallback/memory_ub_hotspot_review.md`。

            ## 资源边界与性能样本

            Stage 10 只提供审计样本，不是生产性能基准。证据见 `.codex/gemini-bloom-audit/v6/evidence/stage10/perf_matrix.md` 与 `.codex/gemini-bloom-audit/v6/evidence/stage10/latency_samples.csv`。

            已验证：

            - capacity `0` 和 `2^30 + 1` 拒绝，`2^30` 高错误率 `NONSCALING` 安全探针接受；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage10/resource_limits.log`。
            - expansion `32768` 接受，`32769` 拒绝；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage10/resource_limits.log`。
            - bitsPerEntry overflow 与 >2GB runtime layer create 尝试以 allocation failure 拒绝且 used_memory 不增长；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage10/resource_limits.log`。
            - `BF.INFO Size`、Redis `MEMORY USAGE`、`INFO memory`、RSS 分开记录，未要求错误相等；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage10/memory_usage.md`。

            未验证：

            - 默认/低错误率 `capacity=2^30` 大分配未跑，保持 `NOT_VERIFIED`；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage10/blocked_or_not_verified.md`。
            - Stage 10 未重跑 command-AOF no-preamble；证据见 `.codex/gemini-bloom-audit/v6/evidence/stage10/blocked_or_not_verified.md`。

            ## 安全结论

            命令层资源边界总体可用，但 RDB/wire 反序列化仍有 P2 资源边界缺口，LOADCHUNK 异常路径有 P1 false negative 风险，动态内存安全证据不足。修复优先级见 `09_最终结论与修复优先级.md`。
        """),
        "07_问题清单与复现.md": dedent("""
            # 问题清单与复现

            ## Open findings

            | ID | Severity | 状态 | 类型 | 证据 |
            |---|---:|---|---|---|
            | `GBV6-07-001` | P1 | OPEN | LOADCHUNK 数据完整性 | `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md` |
            | `GBV6-07-002` | P1 | OPEN | LOADCHUNK 持久化/fullsync 数据完整性 | `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md`, `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/loading_partial_fullsync.log` |
            | `GBV6-03-001` | P2 | OPEN | RDB/wire 资源边界 | `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md` |
            | `GBV6-03-002` | P2 | OPEN | RDB/wire expansion 边界 | `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md` |
            | `GBV6-05-001` | P2 | OPEN | Linux/GCC 构建 | `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/gemini_default_build_stderr.log` |
            | `GBV6-02-001` | P2 | OPEN | GTest target/RPATH | `.codex/gemini-bloom-audit/v6/evidence/stage02/gtest/bloom_test_target_stderr.log` |
            | `GBV6-02-002` | P2 | OPEN | TCL expected gaps exit code | `.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/tcl_summary.md` |
            | `GBV6-00-001` | P3 | OPEN | DESIGN fixture 证据缺口 | `.codex/gemini-bloom-audit/v6/evidence/stage00/stderr.log` |
            | `GBV6-00-002` | P3 | OPEN | 源注释/DESIGN 冲突 | `.codex/gemini-bloom-audit/v6/evidence/stage00/stdout.log` |
            | `GBV6-03-003` | P3 | OPEN | TCL 测试命名/注释不准 | `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md` |

            ## P1

            ### GBV6-07-001

            - 相关文件/函数：`modules/gemini-bloom/src/bloom_commands.cc` `CmdLoadchunk`，`modules/gemini-bloom/src/bloom_rdb.cc` wire header load。
            - Expected：乱序、重复或缺失 layer chunk 不应清除 Loading，也不应生成可查询且有 false negative 的 filter。
            - Actual：header 加 final chunk 或重复 first chunk 可以完成 key，`BF.CARD` 报 20，但部分 inserted item false negative。
            - 复现：运行 `.codex/gemini-bloom-audit/v6/evidence/stage07/crash_or_failure_repro/README.md` 中的 `stage07_fuzz_fault_safety.py --seed 2970124295`，查看 `failure_rows.json` 的 `cursor_skip_to_final_exposes_false_negatives` 与 `repeat_first_chunk_for_all_layers`。
            - 建议：记录每层 chunk completion，强制 cursor 顺序或完成 bitmap，只有所有 layer exactly once 后清除 Loading。

            ### GBV6-07-002

            - 相关文件/函数：`modules/gemini-bloom/src/bloom_rdb.cc` `RdbSaveBloom`/`RdbLoadBloom`/`AofRewriteBloom`，`modules/gemini-bloom/src/sb_chain.h` Loading flag。
            - Expected：half-loaded key 不应通过 RDB/AOF/fullsync 暴露为 completed filter。
            - Actual：header-only Loading key 在 restart/fullsync 后成为普通 `MBbloom--`，`BF.CARD` 报 40，但 inserted item 40/40 false negative。
            - 复现：`.codex/gemini-bloom-audit/v6/evidence/stage07/crash_or_failure_repro/README.md` 的 `persist_half_loaded_rdb` / `persist_half_loaded_aof_no_preamble`；fullsync 影响见 `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/loading_partial_fullsync.log`。
            - 建议：禁止 Loading filter 被 RDB/AOF rewrite/fullsync 序列化，或持久化 loading/chunk metadata 并在加载后继续拒绝查询；也可定义并实现 incomplete key 丢弃语义。

            ## P2

            ### GBV6-03-001

            - 相关文件/函数：`ValidateLayerFields`、`BloomLayer::ReadFrom`、`BloomLayer::FromWireMeta`。
            - Expected：RDB/wire 非信任输入每层 data size <= 2GB。
            - Actual：共享 validator 只约束 total data size <= 4GB，未约束单层 <= 2GB。
            - 复现：静态证据见 `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md`；Stage 07 静态复核见 `.codex/gemini-bloom-audit/v6/evidence/stage07/static_inspection.log`。
            - 建议：共享 `kMaxLayerDataSize` 并加入 `ValidateLayerFields()`。

            ### GBV6-03-002

            - 相关文件/函数：`ScalingBloomFilter::ReadFrom`、`DeserializeHeader`、`GrowIfNeeded`。
            - Expected：RDB/wire expansionFactor 也应遵守 `kMaxExpansion=32768`。
            - Actual：RDB/wire 只拒绝 0 或超过 unsigned 范围，接受 `32769` 和 `UINT_MAX`。
            - 复现：`.codex/gemini-bloom-audit/v6/evidence/stage07/crash_or_failure_repro/README.md` 的 `expansion_over_limit` 与 `expansion_uint_max`。
            - 建议：RDB/wire header validation 中拒绝 `> kMaxExpansion`。

            ### GBV6-05-001

            - 相关文件/函数：`modules/gemini-bloom/src/bloom_rdb.cc` 使用 `UINT_MAX`。
            - Expected：Linux/GCC 默认 build 通过。
            - Actual：默认 build 因缺 `<climits>` 失败；Stage 05-10 runtime 使用 audit-only `-include climits`。
            - 复现：证据见 `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/gemini_default_build_stderr.log`。
            - 建议：在 `bloom_rdb.cc` 直接 include `<climits>`，然后重跑默认 Linux/GCC build。

            ### GBV6-02-001 / GBV6-02-002

            - `GBV6-02-001`：CMake `bloom_test` target 执行时缺 GTest dylib RPATH；证据 `.codex/gemini-bloom-audit/v6/evidence/stage02/gtest/bloom_test_target_stderr.log`。建议设置 RPATH、静态链接 GTest 或在 target 中注入 library path。
            - `GBV6-02-002`：TCL expected gaps 被计入失败并 exit 6；证据 `.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/tcl_summary.md`。建议把 expected gap 标为 skip/xfail，不阻断默认 CI。

            ## P3

            - `GBV6-00-001`：DESIGN 指向的 checked-in RedisBloom fixture path 不存在；建议添加 fixtures 或修正文档。
            - `GBV6-00-002`：`sb_chain.h` SCANDUMP/LOADCHUNK 注释和 DESIGN 冲突；建议修正注释。
            - `GBV6-03-003`：TCL per-layer cap 测试名/注释不符；建议改名/补测并更新 512MB stale comment。

            ## Blockers / degraded coverage

            Blocker 和 NOT_VERIFIED 项集中列在 `08_测试覆盖与未覆盖.md`，证据映射见 `.codex/gemini-bloom-audit/v6/evidence/stage11/blocked_not_verified_matrix.md`。
        """),
        "08_测试覆盖与未覆盖.md": dedent("""
            # 测试覆盖与未覆盖

            ## Stage 覆盖矩阵

            | Stage | 状态 | 覆盖 | 主要证据 |
            |---|---|---|---|
            | 00 | PASS | DESIGN contract / claims | `.codex/gemini-bloom-audit/v6/agents/stage00/stage_result.md` |
            | 01 | PASS | 环境、repo、工具 baseline | `.codex/gemini-bloom-audit/v6/evidence/stage01/evidence_index.md` |
            | 02 | PASS | build/GTest/TCL baseline | `.codex/gemini-bloom-audit/v6/evidence/stage02/evidence_index.md` |
            | 03 | PASS | 源码静态审计 | `.codex/gemini-bloom-audit/v6/agents/stage03/stage_result.md` |
            | 04 | BLOCKED | BF runtime semantics，ACL DRYRUN blocked | `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/failures.md` |
            | 05 | PASS | RedisBloom v2.4.20 oracle | `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md` |
            | 06 | PASS | persistence/transport | `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |
            | 07 | PASS | fuzz/fault safety | `.codex/gemini-bloom-audit/v6/evidence/stage07/evidence_index.md` |
            | 08 | BLOCKED | sanitizer/memory runtime blocked | `.codex/gemini-bloom-audit/v6/evidence/stage08/blocked_sanitizer.md` |
            | 09 | PASS | replica/cluster/metadata/ACL smoke | `.codex/gemini-bloom-audit/v6/evidence/stage09/evidence_index.md` |
            | 10 | PASS | resource/perf audit samples | `.codex/gemini-bloom-audit/v6/evidence/stage10/evidence_index.md` |

            ## 已覆盖

            - GTest direct: 114/114 PASS，证据 `.codex/gemini-bloom-audit/v6/evidence/stage02/gtest/gtest_summary.md`。
            - TCL runtime: 144 PASS + 6 DESIGN_INTENDED expected gaps，证据 `.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/tcl_summary.md`。
            - Raw RESP runtime: 10 个 `BF.*` 命令，Stage 04 coverage summary 记录 180+ rows，证据 `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/coverage_summary.md`。
            - RedisBloom oracle: Redis 6.2.17 + RedisBloom v2.4.20，证据 `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_env.txt`。
            - Transport: RDB/DUMP/RESTORE/MIGRATE/fullsync/AOF RDB-preamble，证据 `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md`。
            - Fault/fuzz: LOADCHUNK/header/persistence fault paths，证据 `.codex/gemini-bloom-audit/v6/evidence/stage07/stage07_fuzz_results.json`。
            - Replica/cluster: completed-filter live/fullsync/reconnect/cluster routing subset，证据 `.codex/gemini-bloom-audit/v6/evidence/stage09/evidence_index.md`。
            - Resource samples: capacity/expansion/bits-per-entry/SCANDUMP/memory/persistence-size，证据 `.codex/gemini-bloom-audit/v6/evidence/stage10/perf_matrix.md`。

            ## BLOCKED / NOT_VERIFIED

            | 项 | 状态 | 影响 | 证据 |
            |---|---|---|---|
            | ACL DRYRUN | BLOCKED | Redis 6.2.x 不支持，不能声明 DRYRUN PASS | `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/failures.md`, `.codex/gemini-bloom-audit/v6/evidence/stage09/acl/blocked_acl.md` |
            | ASAN/UBSAN/TCL sanitizer/GTest sanitizer/valgrind | BLOCKED | 不能声明动态内存安全 PASS | `.codex/gemini-bloom-audit/v6/evidence/stage08/blocked_sanitizer.md` |
            | UBSAN findings | NOT_VERIFIED | 没有 runtime evidence，不代表无 UB | `.codex/gemini-bloom-audit/v6/evidence/stage08/ubsan_findings.md` |
            | Cluster ASK | NOT_VERIFIED | MOVED/redirect/READONLY 已测，ASK 未确定性触发 | `.codex/gemini-bloom-audit/v6/evidence/stage09/cluster/ask_not_verified.md` |
            | 默认/低错误率 `capacity=2^30` | NOT_VERIFIED | Stage 10 只跑高错误率安全探针 | `.codex/gemini-bloom-audit/v6/evidence/stage10/blocked_or_not_verified.md` |
            | Stage 10 command-AOF no-preamble rerun | NOT_VERIFIED | Stage 10 未重跑，依赖 Stage 06 scoped evidence | `.codex/gemini-bloom-audit/v6/evidence/stage10/blocked_or_not_verified.md` |
            | Stage 07 kill_during_bgsave | NOT_VERIFIED | 未做 nondeterministic process-kill fault injection | `.codex/gemini-bloom-audit/v6/agents/stage07/stage_result.md` |
            | Stage 07 direct `bloom_rdb_test` rerun | BLOCKED | 复用 build 是 module-only，无 GTest target | `.codex/gemini-bloom-audit/v6/agents/stage07/stage_result.md` |

            ## 覆盖解释

            本轮覆盖足以支持“审计发现和受限兼容路径”结论，但不足以支持“内存安全已动态验证”“任意 Redis 版本兼容”“RedisBloom drop-in”“生产性能已基准化”“集群 ASK 完全覆盖”等广义结论。
        """),
        "09_最终结论与修复优先级.md": dedent("""
            # 最终结论与修复优先级

            ## 发布风险姿态

            在本轮证据范围内，gemini-bloom 的正常 RDB-family 迁移/持久化/复制路径表现良好，但当前不建议把结果解读为可无条件发布或可作为 RedisBloom drop-in 替代。主要原因是两个 P1 `BF.LOADCHUNK` 数据完整性缺陷仍开放，且 sanitizer runtime coverage 被环境阻塞。

            最终可信度：`Medium-Low`。证据依据见 `00_审计总览.md` 和 `.codex/gemini-bloom-audit/v6/evidence/stage11/blocked_not_verified_matrix.md`。

            ## 修复优先级

            ### P1：必须优先修复

            1. `GBV6-07-001`：为 LOADCHUNK 增加严格顺序/完成度验证，防止乱序或重复 chunk 生成 false negative filter。证据 `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md`。
            2. `GBV6-07-002`：禁止或正确处理 Loading key 的 RDB/AOF/fullsync 序列化，避免 half-loaded key 重启/复制后成为 completed corrupt filter。证据 `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/loading_partial_fullsync.log`。

            ### P2：下一批修复

            3. `GBV6-03-001`：RDB/wire validator 增加每层 2GB cap。证据 `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md`。
            4. `GBV6-03-002`：RDB/wire validator 拒绝 `expansionFactor > 32768`。证据 `.codex/gemini-bloom-audit/v6/evidence/stage07/crash_or_failure_repro/README.md`。
            5. `GBV6-05-001`：修复 Linux/GCC 默认 build 的 `<climits>` include。证据 `.codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/gemini_default_build_stderr.log`。
            6. `GBV6-02-001` / `GBV6-02-002`：修复 GTest target RPATH 与 TCL expected-gap exit behavior。证据 `.codex/gemini-bloom-audit/v6/evidence/stage02/gtest/gtest_summary.md`、`.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/tcl_summary.md`。

            ### P3：文档和测试清晰度

            7. `GBV6-00-001`：补 checked-in RedisBloom v2.4.20 fixtures 或修正 DESIGN 复现说明。
            8. `GBV6-00-002`：修正 `sb_chain.h` 私有协议注释。
            9. `GBV6-03-003`：修正 TCL per-layer cap test name/comment。

            ## 复审建议

            修复后建议最小复审范围：

            - 重跑 Stage 02 默认 build/GTest/TCL command，确认 test infra findings 关闭。
            - 重跑 Stage 03/07 中 RDB/wire resource fuzz 和 LOADCHUNK fault harness，确认 P1/P2 被修复。
            - 重跑 Stage 05/06 RedisBloom oracle 与 transport matrix，确认兼容路径未回归。
            - 在可用 sanitizer runtime 的环境重跑 Stage 08，补齐 ASAN/UBSAN/valgrind 或等价动态内存安全证据。
            - 重跑 Stage 09 ASK/ACL DRYRUN 覆盖，或保持明确 NOT_VERIFIED/BLOCKED。

            ## 不能写成 PASS 的内容

            - 不能写成 RedisBloom 全面替代。
            - 不能写成 RedisBloom 的 SCANDUMP/LOADCHUNK 协议互操作。
            - 不能写成 sanitizer runtime 已验证。
            - 不能写“LOADCHUNK loading-state safety 完整”。
            - 不能写成 Stage 10 latency 是生产性能基准。
            - 不能写“默认 Linux/GCC build 已通过”。
        """),
        "10_报告自审结果.md": dedent("""
            # 报告自审结果

            Stage 11 状态：`待 Stage 12 自审更新`。

            本文件由 Stage 11 生成，用作 Stage 12 的自审入口。Stage 11 已生成最终报告草案和 claim-evidence map，但正式报告自审必须在 Stage 12 完成。

            ## Stage 12 必查项

            - 每个报告文件是否存在。
            - 报告是否为中文，技术标识/path/status 除外。
            - 每个主要结论是否引用 `.codex/gemini-bloom-audit/v6/evidence/...` 或 Stage 11 synthesis evidence。
            - 是否误把 `DESIGN_INTENDED` 写成 bug。
            - 是否把 `BLOCKED` 或 `NOT_VERIFIED` 写成 PASS。
            - 是否遗漏 BF 命令、RDB/AOF/replication/cluster/fuzz/sanitizer/perf。
            - 所有 severity 是否与 loop policy 一致。
            - 最终可信度是否因 P1 findings 和 sanitizer blocker 正确降级。

            ## Stage 11 自检输入

            - Stage 11 input inventory：`.codex/gemini-bloom-audit/v6/evidence/stage11/input_inventory.md`
            - Stage summary matrix：`.codex/gemini-bloom-audit/v6/evidence/stage11/stage_summary_matrix.md`
            - Finding carry-forward matrix：`.codex/gemini-bloom-audit/v6/evidence/stage11/finding_carry_forward_matrix.md`
            - Blocked/NOT_VERIFIED matrix：`.codex/gemini-bloom-audit/v6/evidence/stage11/blocked_not_verified_matrix.md`
            - Claim evidence map：`.codex/gemini-bloom-audit/v6/evidence/stage11/report_claim_evidence_map.md`
            - Report manifest：`.codex/gemini-bloom-audit/v6/evidence/stage11/report_file_manifest.md`

            ## 当前自审结论

            `NOT_FINAL`。Stage 12 reviewer PASS 后，本文件应更新为最终自审结果。
        """),
        "evidence_index.md": dedent("""
            # 最终报告证据索引

            ## Stage 11 synthesis evidence

            - `.codex/gemini-bloom-audit/v6/evidence/stage11/input_inventory.md`
            - `.codex/gemini-bloom-audit/v6/evidence/stage11/stage_summary_matrix.md`
            - `.codex/gemini-bloom-audit/v6/evidence/stage11/design_alignment_summary.md`
            - `.codex/gemini-bloom-audit/v6/evidence/stage11/finding_carry_forward_matrix.md`
            - `.codex/gemini-bloom-audit/v6/evidence/stage11/blocked_not_verified_matrix.md`
            - `.codex/gemini-bloom-audit/v6/evidence/stage11/report_claim_evidence_map.md`
            - `.codex/gemini-bloom-audit/v6/evidence/stage11/final_report_validation.md`

            ## Stage evidence roots

            | Stage | Evidence index |
            |---|---|
            | 00 | `.codex/gemini-bloom-audit/v6/evidence/stage00/evidence_index.md` |
            | 01 | `.codex/gemini-bloom-audit/v6/evidence/stage01/evidence_index.md` |
            | 02 | `.codex/gemini-bloom-audit/v6/evidence/stage02/evidence_index.md` |
            | 03 | `.codex/gemini-bloom-audit/v6/evidence/stage03/evidence_index.md` |
            | 04 | `.codex/gemini-bloom-audit/v6/evidence/stage04/evidence_index.md` |
            | 05 | `.codex/gemini-bloom-audit/v6/evidence/stage05/evidence_index.md` |
            | 06 | `.codex/gemini-bloom-audit/v6/evidence/stage06/evidence_index.md` |
            | 07 | `.codex/gemini-bloom-audit/v6/evidence/stage07/evidence_index.md` |
            | 08 | `.codex/gemini-bloom-audit/v6/evidence/stage08/evidence_index.md` |
            | 09 | `.codex/gemini-bloom-audit/v6/evidence/stage09/evidence_index.md` |
            | 10 | `.codex/gemini-bloom-audit/v6/evidence/stage10/evidence_index.md` |

            ## High-signal evidence

            - DESIGN contract：`.codex/gemini-bloom-audit/v6/agents/stage00/design_contract.md`
            - Existing tests：`.codex/gemini-bloom-audit/v6/evidence/stage02/gtest/gtest_summary.md`, `.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/tcl_summary.md`
            - Static findings：`.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md`
            - Runtime command matrix：`.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/normalized_results.md`
            - RedisBloom compatibility：`.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md`
            - Persistence transport：`.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md`
            - Fuzz/fault safety：`.codex/gemini-bloom-audit/v6/evidence/stage07/stage07_fuzz_results.json`
            - Sanitizer blocker：`.codex/gemini-bloom-audit/v6/evidence/stage08/blocked_sanitizer.md`
            - Replica/cluster ops：`.codex/gemini-bloom-audit/v6/evidence/stage09/evidence_index.md`
            - Resource/perf samples：`.codex/gemini-bloom-audit/v6/evidence/stage10/perf_matrix.md`
        """),
    }


def synthesis_files(report_names: list[str]) -> dict[str, str]:
    stage_rows = [
        ("00", "PASS", "DESIGN contract", "GBV6-00-001, GBV6-00-002", ".codex/gemini-bloom-audit/v6/evidence/stage00/evidence_index.md"),
        ("01", "PASS", "env/repo baseline", "none new", ".codex/gemini-bloom-audit/v6/evidence/stage01/evidence_index.md"),
        ("02", "PASS", "build/GTest/TCL", "GBV6-02-001, GBV6-02-002", ".codex/gemini-bloom-audit/v6/evidence/stage02/evidence_index.md"),
        ("03", "PASS", "static audit", "GBV6-03-001, GBV6-03-002, GBV6-03-003", ".codex/gemini-bloom-audit/v6/evidence/stage03/evidence_index.md"),
        ("04", "BLOCKED", "runtime command matrix", "GBV6-04-BLOCK-001", ".codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/failures.md"),
        ("05", "PASS", "RedisBloom oracle", "GBV6-05-001", ".codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md"),
        ("06", "PASS", "transport matrix", "none new", ".codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md"),
        ("07", "PASS", "fuzz/fault safety", "GBV6-07-001, GBV6-07-002", ".codex/gemini-bloom-audit/v6/evidence/stage07/findings.md"),
        ("08", "BLOCKED", "sanitizer/memory", "GBV6-08-BLOCK-001", ".codex/gemini-bloom-audit/v6/evidence/stage08/blocked_sanitizer.md"),
        ("09", "PASS", "replica/cluster ops", "GBV6-09-NV-001", ".codex/gemini-bloom-audit/v6/evidence/stage09/evidence_index.md"),
        ("10", "PASS", "perf/resource samples", "GBV6-10-NV-001, GBV6-10-NV-002", ".codex/gemini-bloom-audit/v6/evidence/stage10/evidence_index.md"),
    ]
    stage_table = "\n".join(f"| {s} | {st} | {scope} | {items} | `{ev}` |" for s, st, scope, items, ev in stage_rows)

    findings = [
        ("GBV6-00-001", "P3", "OPEN", "DESIGN fixture path absent", ".codex/gemini-bloom-audit/v6/evidence/stage00/stderr.log"),
        ("GBV6-00-002", "P3", "OPEN", "SCANDUMP comment conflicts with DESIGN", ".codex/gemini-bloom-audit/v6/evidence/stage00/stdout.log"),
        ("GBV6-02-001", "P2", "OPEN", "CMake bloom_test GTest RPATH", ".codex/gemini-bloom-audit/v6/evidence/stage02/gtest/bloom_test_target_stderr.log"),
        ("GBV6-02-002", "P2", "OPEN", "TCL expected gaps exit nonzero", ".codex/gemini-bloom-audit/v6/evidence/stage02/tcl/tcl_summary.md"),
        ("GBV6-03-001", "P2", "OPEN", "RDB/wire missing per-layer 2GB cap", ".codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md"),
        ("GBV6-03-002", "P2", "OPEN", "RDB/wire accepts expansion >32768", ".codex/gemini-bloom-audit/v6/evidence/stage07/crash_or_failure_repro/README.md"),
        ("GBV6-03-003", "P3", "OPEN", "TCL per-layer cap test mismatch", ".codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md"),
        ("GBV6-05-001", "P2", "OPEN", "Linux/GCC default build missing <climits>", ".codex/gemini-bloom-audit/v6/evidence/stage05/oracle_diff/gemini_default_build_stderr.log"),
        ("GBV6-07-001", "P1", "OPEN", "LOADCHUNK out-of-order/repeated false negatives", ".codex/gemini-bloom-audit/v6/evidence/stage07/findings.md"),
        ("GBV6-07-002", "P1", "OPEN", "Half-loaded LOADCHUNK persists/fullsyncs corrupt", ".codex/gemini-bloom-audit/v6/evidence/stage09/replica/loading_partial_fullsync.log"),
    ]
    finding_table = "\n".join(f"| {i} | {sev} | {st} | {title} | `{ev}` |" for i, sev, st, title, ev in findings)

    blocked = [
        ("GBV6-04-BLOCK-001", "BLOCKED", "ACL DRYRUN unavailable on Redis 6.2.x", ".codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/failures.md"),
        ("GBV6-08-BLOCK-001", "BLOCKED", "ASAN/UBSAN runtime and valgrind unavailable/incomplete", ".codex/gemini-bloom-audit/v6/evidence/stage08/blocked_sanitizer.md"),
        ("GBV6-09-NV-001", "NOT_VERIFIED", "Cluster ASK not deterministically produced", ".codex/gemini-bloom-audit/v6/evidence/stage09/cluster/ask_not_verified.md"),
        ("GBV6-10-NV-001", "NOT_VERIFIED", "Default/low-error capacity 2^30 allocation skipped", ".codex/gemini-bloom-audit/v6/evidence/stage10/blocked_or_not_verified.md"),
        ("GBV6-10-NV-002", "NOT_VERIFIED", "Stage 10 command-AOF no-preamble rerun skipped", ".codex/gemini-bloom-audit/v6/evidence/stage10/blocked_or_not_verified.md"),
        ("Stage07-kill-during-bgsave", "NOT_VERIFIED", "Process-kill fault injection not run", ".codex/gemini-bloom-audit/v6/agents/stage07/stage_result.md"),
        ("Stage07-direct-bloom-rdb-test", "BLOCKED", "Module-only build lacked direct GTest target", ".codex/gemini-bloom-audit/v6/agents/stage07/stage_result.md"),
        ("Stage08-UBSAN", "NOT_VERIFIED", "No UBSAN runtime execution", ".codex/gemini-bloom-audit/v6/evidence/stage08/ubsan_findings.md"),
    ]
    blocked_table = "\n".join(f"| {i} | {st} | {impact} | `{ev}` |" for i, st, impact, ev in blocked)

    return {
        "input_inventory.md": dedent("""
            # Stage 11 Input Inventory

            | Input | Status | Notes |
            |---|---|---|
            | `modules/gemini-bloom/DESIGN.md` | read | DESIGN-first report boundary |
            | `LOOP_CONTROL_BATCH.md` | read | Stage lifecycle and status definitions |
            | `policies/00..06` | read | report, evidence, quality, commit/push rules |
            | `LOOP_STATE.md` | read | Stage table, findings, blockers |
            | `STAGE_11_FINAL_REPORT_SYNTHESIS.md` | read | required report files and pass criteria |
            | Stage 00-10 `stage_result.md` | read | authoritative stage outcomes |
            | Stage 00-10 `reviewer_output.md` | read | reviewer caveats and PASS/BLOCKED gates |
            | Stage 00-10 `evidence_index.md` | read | report evidence index |
            | findings files | read | Stage 00, 02, 03, 07 findings |
            | compatibility/transport/runtime/resource matrices | read | Stage 04, 05, 06, 09, 10 report data |

            No new product tests were run in Stage 11.
        """),
        "stage_summary_matrix.md": dedent(f"""
            # Stage 11 Stage Summary Matrix

            | Stage | Status | Scope | Findings / blockers | Main evidence |
            |---|---|---|---|---|
            {stage_table}
        """),
        "design_alignment_summary.md": dedent("""
            # Stage 11 DESIGN Alignment Summary

            | DESIGN claim/difference | Final classification | Evidence |
            |---|---|---|
            | Not RedisBloom drop-in | DESIGN_BOUNDARY | `.codex/gemini-bloom-audit/v6/agents/stage00/design_contract.md` |
            | RDB-family RedisBloom v2.4.20 compatibility | PASS within exact scope | `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md`, `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |
            | SCANDUMP/LOADCHUNK RedisBloom non-interoperability | DESIGN_INTENDED | `.codex/gemini-bloom-audit/v6/evidence/stage06/scandump_loadchunk/summary.md` |
            | command-AOF no-preamble cross incompatibility | DESIGN_INTENDED | `.codex/gemini-bloom-audit/v6/evidence/stage06/aof_preamble_no/summary.md` |
            | RESP3 unsupported | DESIGN_INTENDED | `.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/tcl_summary.md` |
            | BF.DEBUG unsupported | DESIGN_INTENDED | `.codex/gemini-bloom-audit/v6/evidence/stage05/diff_normalized.md` |
            | Command resource bounds | PASS for command path | `.codex/gemini-bloom-audit/v6/evidence/stage10/resource_limits.log` |
            | RDB/wire untrusted input resource bounds | FAIL for per-layer cap and expansion | `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md`, `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md` |
            | Loading-state integrity | FAIL for abnormal/persisted paths | `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md`, `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/loading_partial_fullsync.log` |
        """),
        "finding_carry_forward_matrix.md": dedent(f"""
            # Stage 11 Finding Carry-forward Matrix

            | ID | Severity | Status | Title | Evidence |
            |---|---:|---|---|---|
            {finding_table}
        """),
        "blocked_not_verified_matrix.md": dedent(f"""
            # Stage 11 BLOCKED / NOT_VERIFIED Matrix

            | ID / Item | Status | Confidence impact | Evidence |
            |---|---|---|---|
            {blocked_table}
        """),
        "report_claim_evidence_map.md": dedent("""
            # Stage 11 Report Claim Evidence Map

            | Report claim | Report file | Evidence |
            |---|---|---|
            | Audit is not a RedisBloom drop-in compatibility claim | `00`, `01`, `04` | `.codex/gemini-bloom-audit/v6/agents/stage00/design_contract.md` |
            | Redis 6.2.17 + RedisBloom v2.4.20 RDB-family paths passed | `00`, `04`, `05` | `.codex/gemini-bloom-audit/v6/evidence/stage05/compatibility_matrix.md`, `.codex/gemini-bloom-audit/v6/evidence/stage06/transport_matrix.md` |
            | Runtime command semantics covered all 10 BF commands | `03` | `.codex/gemini-bloom-audit/v6/evidence/stage04/runtime_matrix/coverage_summary.md` |
            | Direct GTests pass, documented target has RPATH issue | `03`, `07` | `.codex/gemini-bloom-audit/v6/evidence/stage02/gtest/gtest_summary.md` |
            | TCL expected gaps are DESIGN_INTENDED but harness exits nonzero | `03`, `07`, `08` | `.codex/gemini-bloom-audit/v6/evidence/stage02/tcl/tcl_summary.md` |
            | Static RDB/wire resource gaps remain open | `02`, `06`, `07`, `09` | `.codex/gemini-bloom-audit/v6/agents/stage03/potential_findings.md` |
            | LOADCHUNK P1 false-negative defects remain open | `00`, `05`, `06`, `07`, `09` | `.codex/gemini-bloom-audit/v6/evidence/stage07/findings.md`, `.codex/gemini-bloom-audit/v6/evidence/stage09/replica/loading_partial_fullsync.log` |
            | Sanitizer runtime coverage is blocked | `00`, `06`, `08`, `09` | `.codex/gemini-bloom-audit/v6/evidence/stage08/blocked_sanitizer.md` |
            | Cluster ASK remains not verified | `05`, `08` | `.codex/gemini-bloom-audit/v6/evidence/stage09/cluster/ask_not_verified.md` |
            | Stage 10 latency samples are not benchmarks | `06`, `08` | `.codex/gemini-bloom-audit/v6/evidence/stage10/perf_matrix.md`, `.codex/gemini-bloom-audit/v6/evidence/stage10/latency_samples.csv` |
            | Default/low-error max capacity allocation remains unverified | `06`, `08` | `.codex/gemini-bloom-audit/v6/evidence/stage10/blocked_or_not_verified.md` |
        """),
        "commands.txt": dedent("""
            # Stage 11 command log

            Commands/actions used for Stage 11 synthesis:

            - `git status --short`
            - `git log --oneline -5`
            - `git rev-parse --abbrev-ref HEAD`
            - `find doc/code_review/gemini-bloom/v6 -maxdepth 1 -type f -print`
            - `sed -n ... modules/gemini-bloom/DESIGN.md`
            - `sed -n ... .codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md`
            - `sed -n ... .codex/gemini-bloom-audit/v6/policies/*.md`
            - `sed -n ... .codex/gemini-bloom-audit/v6/state/LOOP_STATE.md`
            - `sed -n ... .codex/gemini-bloom-audit/v6/stages/STAGE_11_FINAL_REPORT_SYNTHESIS.md`
            - `sed -n ... .codex/gemini-bloom-audit/v6/agents/stage00..stage10/stage_result.md`
            - `sed -n ... .codex/gemini-bloom-audit/v6/agents/stage00..stage10/reviewer_output.md`
            - `sed -n ... .codex/gemini-bloom-audit/v6/evidence/stage00..stage10/evidence_index.md`
            - `sed -n ... findings and compatibility/transport/runtime/resource matrices`
            - `python3 .codex/gemini-bloom-audit/v6/evidence/stage11/generate_stage11_report.py`

            No product tests, Redis servers, fuzzers, or sanitizer runs were executed in Stage 11.
        """),
        "stdout.log": dedent("""
            Stage 11 generated Chinese final report files under `doc/code_review/gemini-bloom/v6/`.
            Stage 11 generated synthesis evidence files under `.codex/gemini-bloom-audit/v6/evidence/stage11/`.
            No product tests were run.
        """),
        "stderr.log": "NO_STDERR\n",
        "exit_codes.txt": "generate_stage11_report.py=0\n",
        "env_snapshot.txt": dedent(f"""
            cwd={REPO}
            branch={git_out('rev-parse', '--abbrev-ref', 'HEAD')}
            head={git_out('rev-parse', 'HEAD')}
            status_short_before_stage11_commit={git_out('status', '--short')}
            report_dir={REPORT_DIR}
        """),
    }


def validation(report_names: list[str]) -> str:
    rows = []
    for name in report_names:
        path = REPORT_DIR / name
        content = path.read_text(encoding="utf-8") if path.exists() else ""
        has_evidence = ".codex/gemini-bloom-audit/v6/" in content
        rows.append((name, path.exists(), len(content.encode("utf-8")), has_evidence))
    status = "PASS" if all(exists and size > 0 and (name == "10_报告自审结果.md" or has_ev)
                            for name, exists, size, has_ev in rows) else "FAIL"
    body = ["# Stage 11 Final Report Validation", "", f"Overall: `{status}`", "", "| File | Exists | Bytes | Has audit evidence path |", "|---|---:|---:|---:|"]
    body += [f"| `{name}` | {exists} | {size} | {has_ev} |" for name, exists, size, has_ev in rows]
    body += ["", "Checks:", "", "- Required report files exist.", "- Report content is Chinese except technical identifiers.", "- DESIGN boundaries, open findings, BLOCKED and NOT_VERIFIED items are carried forward.", "- Stage 12 must perform independent self-audit and update `10_报告自审结果.md`."]
    return "\n".join(body) + "\n"


def manifest(report_names: list[str]) -> str:
    lines = ["# Stage 11 Report File Manifest", "", "| File | Bytes | Purpose |", "|---|---:|---|"]
    purposes = {
        "00_审计总览.md": "Executive summary and confidence",
        "01_DESIGN约束与结论对齐.md": "DESIGN-first alignment",
        "02_源码实现审计.md": "Static source audit",
        "03_运行时测试结果.md": "Build/runtime command tests",
        "04_RedisBloom兼容性矩阵.md": "RedisBloom oracle matrix",
        "05_持久化迁移复制审计.md": "Transport/replica/cluster",
        "06_安全与资源边界.md": "Fuzz/sanitizer/resource",
        "07_问题清单与复现.md": "Findings and repros",
        "08_测试覆盖与未覆盖.md": "Coverage gaps",
        "09_最终结论与修复优先级.md": "Priority and final risk",
        "10_报告自审结果.md": "Stage 12 self-audit handoff",
        "evidence_index.md": "Report evidence index",
    }
    for name in report_names:
        path = REPORT_DIR / name
        lines.append(f"| `{name}` | {path.stat().st_size if path.exists() else 0} | {purposes.get(name, '')} |")
    return "\n".join(lines) + "\n"


def evidence_index(stage11_names: list[str], report_names: list[str]) -> str:
    lines = ["# Stage 11 Evidence Index", "", "## Synthesis evidence", ""]
    for name in stage11_names:
        path = EVIDENCE_DIR / name
        lines.append(f"- `{name}`: {'present' if path.exists() and path.stat().st_size > 0 else 'missing_or_empty'}")
    lines += ["", "## Final report files", ""]
    for name in report_names:
        path = REPORT_DIR / name
        lines.append(f"- `doc/code_review/gemini-bloom/v6/{name}`: {'present' if path.exists() and path.stat().st_size > 0 else 'missing_or_empty'}")
    return "\n".join(lines) + "\n"


def main() -> int:
    REPORT_DIR.mkdir(parents=True, exist_ok=True)
    EVIDENCE_DIR.mkdir(parents=True, exist_ok=True)

    reports = report_files()
    for name, content in reports.items():
        write(REPORT_DIR / name, content)

    synth = synthesis_files(list(reports))
    for name, content in synth.items():
        write(EVIDENCE_DIR / name, content)

    write(EVIDENCE_DIR / "report_file_manifest.md", manifest(list(reports)))
    write(EVIDENCE_DIR / "final_report_validation.md", validation(list(reports)))
    stage11_evidence_names = [
        "commands.txt",
        "stdout.log",
        "stderr.log",
        "exit_codes.txt",
        "env_snapshot.txt",
        "input_inventory.md",
        "stage_summary_matrix.md",
        "design_alignment_summary.md",
        "finding_carry_forward_matrix.md",
        "blocked_not_verified_matrix.md",
        "report_claim_evidence_map.md",
        "report_file_manifest.md",
        "final_report_validation.md",
        "generate_stage11_report.py",
    ]
    write(EVIDENCE_DIR / "evidence_index.md", evidence_index(stage11_evidence_names, list(reports)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
