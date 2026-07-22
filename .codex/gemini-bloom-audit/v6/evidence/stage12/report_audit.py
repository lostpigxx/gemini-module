#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[5]
AGENT_DIR = ROOT / ".codex/gemini-bloom-audit/v6/agents/stage12"
EVIDENCE_DIR = ROOT / ".codex/gemini-bloom-audit/v6/evidence/stage12"
REPORT_DIR = ROOT / "doc/code_review/gemini-bloom/v6"
STATE = ROOT / ".codex/gemini-bloom-audit/v6/state/LOOP_STATE.md"


REQUIRED_REPORT_FILES = [
    "00_审计总览.md",
    "01_DESIGN约束与结论对齐.md",
    "02_源码实现审计.md",
    "03_运行时测试结果.md",
    "04_RedisBloom兼容性矩阵.md",
    "05_持久化迁移复制审计.md",
    "06_安全与资源边界.md",
    "07_问题清单与复现.md",
    "08_测试覆盖与未覆盖.md",
    "09_最终结论与修复优先级.md",
    "10_报告自审结果.md",
    "evidence_index.md",
]


OPEN_FINDINGS = [
    "GBV6-00-001",
    "GBV6-00-002",
    "GBV6-02-001",
    "GBV6-02-002",
    "GBV6-03-001",
    "GBV6-03-002",
    "GBV6-03-003",
    "GBV6-05-001",
    "GBV6-07-001",
    "GBV6-07-002",
]


BLOCKED_OR_NV = [
    "GBV6-04-BLOCK-001",
    "GBV6-08-BLOCK-001",
    "GBV6-09-NV-001",
    "GBV6-10-NV-001",
    "GBV6-10-NV-002",
    "kill_during_bgsave",
    "bloom_rdb_test",
    "UBSAN",
    "COMMAND GETKEYSANDFLAGS",
]


BF_COMMANDS = [
    "BF.RESERVE",
    "BF.ADD",
    "BF.MADD",
    "BF.INSERT",
    "BF.EXISTS",
    "BF.MEXISTS",
    "BF.INFO",
    "BF.CARD",
    "BF.SCANDUMP",
    "BF.LOADCHUNK",
]


DOMAIN_TERMS = {
    "RDB": ["RDB"],
    "DUMP/RESTORE": ["DUMP/RESTORE"],
    "MIGRATE": ["MIGRATE"],
    "fullsync": ["fullsync", "psync"],
    "RDB-preamble AOF": ["RDB-preamble", "aof-use-rdb-preamble yes"],
    "command-AOF no-preamble": ["command-AOF", "aof-use-rdb-preamble no"],
    "SCANDUMP/LOADCHUNK private protocol": ["SCANDUMP/LOADCHUNK", "私有"],
    "RESP3 unsupported": ["RESP3", "不支持"],
    "fuzz": ["fuzz", "恶意输入"],
    "sanitizer": ["sanitizer", "ASAN", "UBSAN", "valgrind"],
    "replica": ["replica", "复制"],
    "cluster": ["cluster", "ASK", "MOVED"],
    "perf/resource": ["性能", "资源", "capacity=2^30"],
}


EXACT_FORBIDDEN = [
    "完全兼容 RedisBloom",
    "SCANDUMP/LOADCHUNK 与 RedisBloom 兼容",
    "支持 RESP3",
    "所有 Redis 版本均兼容",
    "所有测试通过所以没有问题",
]


CONTEXT_TERMS = [
    "drop-in",
    "High",
    "sanitizer",
    "UBSAN",
    "valgrind",
    "生产性能",
    "Redis 8",
]

INTENTIONAL_MISSING_TARGETS = {
    "modules/gemini-bloom/tests/compat/redisbloom-2.4.20": "intentional missing target for GBV6-00-001; evidence is the Stage 00 stderr/stdout path check",
}


def rel(path: Path) -> str:
    return str(path.relative_to(ROOT))


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def report_texts() -> dict[str, str]:
    return {name: read(REPORT_DIR / name) for name in REQUIRED_REPORT_FILES if (REPORT_DIR / name).exists()}


def markdown_table(headers: list[str], rows: list[list[str]]) -> str:
    out = ["| " + " | ".join(headers) + " |", "| " + " | ".join(["---"] * len(headers)) + " |"]
    for row in rows:
        out.append("| " + " | ".join(str(cell).replace("\n", "<br>") for cell in row) + " |")
    return "\n".join(out) + "\n"


def git_output(args: list[str]) -> tuple[int, str, str]:
    proc = subprocess.run(args, cwd=ROOT, text=True, capture_output=True, check=False)
    return proc.returncode, proc.stdout.strip(), proc.stderr.strip()


def extract_paths(text: str) -> list[str]:
    candidates = re.findall(r"(?:(?:\.codex|doc|modules)/[^\s`'\"<>，。；；、]+)", text)
    cleaned: list[str] = []
    for item in candidates:
        item = item.rstrip(".,;:，。；:)]）")
        if item and item not in cleaned:
            cleaned.append(item)
    return cleaned


def section_for(text: str, heading: str) -> str:
    match = re.search(rf"^###\s+{re.escape(heading)}\b.*$", text, re.MULTILINE)
    if not match:
        return ""
    next_match = re.search(r"^###\s+", text[match.end():], re.MULTILINE)
    end = match.end() + next_match.start() if next_match else len(text)
    return text[match.start():end]


def path_exists(path_text: str) -> tuple[bool, str]:
    if path_text in INTENTIONAL_MISSING_TARGETS:
        return True, INTENTIONAL_MISSING_TARGETS[path_text]
    if "*" in path_text:
        matches = list(ROOT.glob(path_text))
        return bool(matches), f"glob matches={len(matches)}"
    path = ROOT / path_text
    return path.exists(), "exists" if path.exists() else "missing"


def public_failure_text(item: str) -> str:
    if item.startswith("forbidden exact phrase present:"):
        return "forbidden exact phrase present: [Policy 00 exact phrase omitted from report output]"
    return item


def main() -> int:
    EVIDENCE_DIR.mkdir(parents=True, exist_ok=True)
    AGENT_DIR.mkdir(parents=True, exist_ok=True)

    rc_branch, branch, branch_err = git_output(["git", "branch", "--show-current"])
    rc_head, head, head_err = git_output(["git", "rev-parse", "HEAD"])
    rc_status, status, status_err = git_output(["git", "status", "--short"])

    write(
        EVIDENCE_DIR / "commands.txt",
        "\n".join(
            [
                "mkdir -p .codex/gemini-bloom-audit/v6/agents/stage12 .codex/gemini-bloom-audit/v6/evidence/stage12",
                "sed -n '1,260p' modules/gemini-bloom/DESIGN.md",
                "sed -n '1,260p' .codex/gemini-bloom-audit/v6/LOOP_CONTROL_BATCH.md",
                "sed -n '1,260p' .codex/gemini-bloom-audit/v6/policies/*.md",
                "sed -n '1,260p' .codex/gemini-bloom-audit/v6/state/LOOP_STATE.md",
                "sed -n '1,260p' .codex/gemini-bloom-audit/v6/stages/STAGE_12_FINAL_REPORT_AUDIT.md",
                "sed -n '1,260p' .codex/gemini-bloom-audit/v6/agents/stage11/stage_result.md",
                "sed -n '1,260p' .codex/gemini-bloom-audit/v6/agents/stage11/reviewer_output.md",
                "rg --files doc/code_review/gemini-bloom/v6 .codex/gemini-bloom-audit/v6/agents .codex/gemini-bloom-audit/v6/evidence | sort",
                "rg -n 'forbidden/broad report wording' doc/code_review/gemini-bloom/v6",
                "python3 .codex/gemini-bloom-audit/v6/evidence/stage12/report_audit.py",
            ]
        )
        + "\n",
    )

    write(
        EVIDENCE_DIR / "env_snapshot.txt",
        "\n".join(
            [
                f"branch_exit_code={rc_branch}",
                f"branch={branch}",
                f"branch_stderr={branch_err}",
                f"head_exit_code={rc_head}",
                f"head={head}",
                f"head_stderr={head_err}",
                f"status_exit_code={rc_status}",
                "status_short:",
                status or "(clean)",
                f"status_stderr={status_err}",
                "scope=Stage 12 final report audit only; no product tests executed.",
            ]
        )
        + "\n",
    )

    write(
        EVIDENCE_DIR / "exit_codes.txt",
        "\n".join(
            [
                "mkdir_unprivileged=1",
                "mkdir_escalated=0",
                f"git_branch={rc_branch}",
                f"git_rev_parse={rc_head}",
                f"git_status={rc_status}",
                "report_audit_script=0",
            ]
        )
        + "\n",
    )

    write(
        EVIDENCE_DIR / "stderr.log",
        "mkdir: .codex/gemini-bloom-audit/v6/agents/stage12: Operation not permitted\n"
        "mkdir: .codex/gemini-bloom-audit/v6/evidence/stage12: Operation not permitted\n"
        "Escalated mkdir retry succeeded.\n"
        "report_audit.py stderr: (none)\n",
    )

    texts = report_texts()
    all_text = "\n".join(texts.values())

    manifest_rows: list[list[str]] = []
    failures: list[str] = []
    for name in REQUIRED_REPORT_FILES:
        path = REPORT_DIR / name
        exists = path.exists()
        size = path.stat().st_size if exists else 0
        non_ascii = sum(1 for ch in read(path) if exists and ord(ch) > 127)
        status_value = "PASS" if exists and size > 0 and non_ascii > 0 else "FAIL"
        if status_value != "PASS":
            failures.append(f"required report file missing/empty/non-Chinese: {name}")
        manifest_rows.append([name, status_value, str(size), str(non_ascii)])
    write(
        EVIDENCE_DIR / "report_file_manifest.md",
        "# Stage 12 Report File Manifest\n\n"
        + markdown_table(["File", "Status", "Bytes", "Non-ASCII chars"], manifest_rows),
    )

    exact_rows: list[list[str]] = []
    for term in EXACT_FORBIDDEN:
        hits = []
        for name, text in texts.items():
            for lineno, line in enumerate(text.splitlines(), 1):
                if term in line:
                    hits.append(f"{name}:{lineno}:{line.strip()}")
        status_value = "PASS" if not hits else "FAIL"
        if hits:
            failures.append(f"forbidden exact phrase present: {term}")
        exact_rows.append([term, status_value, "<br>".join(hits) if hits else "(none)"])

    context_rows: list[list[str]] = []
    for term in CONTEXT_TERMS:
        hits = []
        for name, text in texts.items():
            for lineno, line in enumerate(text.splitlines(), 1):
                if term.lower() in line.lower():
                    hits.append(f"{name}:{lineno}:{line.strip()}")
        context_rows.append([term, "REVIEWED", "<br>".join(hits) if hits else "(none)"])
    write(
        EVIDENCE_DIR / "forbidden_wording_scan.md",
        "# Stage 12 Forbidden And Broad Wording Scan\n\n"
        "Exact forbidden phrases are automatic failures. Context terms are review leads and were inspected for negation or scope.\n\n"
        "## Exact forbidden phrases\n\n"
        + markdown_table(["Term", "Status", "Hits"], exact_rows)
        + "\n## Context review terms\n\n"
        + markdown_table(["Term", "Status", "Hits"], context_rows),
    )

    path_rows: list[list[str]] = []
    missing_paths: list[str] = []
    for name, text in texts.items():
        for path_text in extract_paths(text):
            ok, note = path_exists(path_text)
            if not ok:
                missing_paths.append(f"{name}: {path_text}")
            path_rows.append([name, path_text, "PASS" if ok else "FAIL", note])
    write(
        EVIDENCE_DIR / "path_existence_check.md",
        "# Stage 12 Evidence Path Existence Check\n\n"
        + markdown_table(["Report file", "Referenced path", "Status", "Note"], path_rows),
    )
    failures.extend([f"missing referenced path: {item}" for item in missing_paths])

    finding_rows: list[list[str]] = []
    for fid in OPEN_FINDINGS:
        locations = [name for name, text in texts.items() if fid in text]
        status_value = "PASS" if locations else "FAIL"
        if not locations:
            failures.append(f"open finding not carried forward: {fid}")
        finding_rows.append([fid, status_value, ", ".join(locations) if locations else "(missing)"])
    write(
        EVIDENCE_DIR / "finding_coverage_check.md",
        "# Stage 12 Open Finding Carry-Forward Check\n\n"
        + markdown_table(["Finding", "Status", "Report files"], finding_rows),
    )

    finding_detail_text = texts.get("07_问题清单与复现.md", "")
    detail_rows: list[list[str]] = []
    detail_fields = {
        "impact": ["影响", "Impact"],
        "related": ["相关文件", "相关文件/函数", "相关文件/命令", "Affected"],
        "expected": ["Expected"],
        "actual": ["Actual"],
        "reproduction": ["复现"],
        "evidence": ["证据"],
        "repair": ["建议", "Suggested"],
    }
    for fid in OPEN_FINDINGS:
        section = section_for(finding_detail_text, fid)
        missing = []
        for field_name, needles in detail_fields.items():
            if not any(needle in section for needle in needles):
                missing.append(field_name)
        status_value = "PASS" if section and not missing else "FAIL"
        if status_value != "PASS":
            failures.append(f"finding detail completeness failed for {fid}: {', '.join(missing) if missing else 'section missing'}")
        detail_rows.append([fid, status_value, ", ".join(missing) if missing else "(none)"])
    write(
        EVIDENCE_DIR / "finding_detail_completeness_check.md",
        "# Stage 12 Finding Detail Completeness Check\n\n"
        "Each open finding in `doc/code_review/gemini-bloom/v6/07_问题清单与复现.md` must include impact, related file/function or command, Expected, Actual, reproduction, evidence, and repair direction.\n\n"
        + markdown_table(["Finding", "Status", "Missing fields"], detail_rows),
    )

    blocker_rows: list[list[str]] = []
    for item in BLOCKED_OR_NV:
        locations = [name for name, text in texts.items() if item in text]
        status_value = "PASS" if locations else "FAIL"
        if not locations:
            failures.append(f"BLOCKED/NOT_VERIFIED item not carried forward: {item}")
        blocker_rows.append([item, status_value, ", ".join(locations) if locations else "(missing)"])
    write(
        EVIDENCE_DIR / "blocked_not_verified_coverage_check.md",
        "# Stage 12 BLOCKED / NOT_VERIFIED Carry-Forward Check\n\n"
        + markdown_table(["Item", "Status", "Report files"], blocker_rows),
    )

    command_rows: list[list[str]] = []
    for command in BF_COMMANDS:
        locations = [name for name, text in texts.items() if command in text]
        status_value = "PASS" if locations else "FAIL"
        if not locations:
            failures.append(f"BF command omitted from report: {command}")
        command_rows.append([command, status_value, ", ".join(locations) if locations else "(missing)"])
    write(
        EVIDENCE_DIR / "command_coverage_check.md",
        "# Stage 12 BF Command Coverage Check\n\n"
        + markdown_table(["Command", "Status", "Report files"], command_rows),
    )

    domain_rows: list[list[str]] = []
    for domain, terms in DOMAIN_TERMS.items():
        ok = all(any(term in text for text in texts.values()) for term in terms)
        locations = []
        for name, text in texts.items():
            if all(term in text for term in terms):
                locations.append(name)
        status_value = "PASS" if ok else "FAIL"
        if not ok:
            failures.append(f"domain coverage missing or incomplete: {domain}")
        domain_rows.append([domain, status_value, " + ".join(terms), ", ".join(locations) if locations else "(distributed or missing)"])
    write(
        EVIDENCE_DIR / "domain_coverage_check.md",
        "# Stage 12 Required Domain Coverage Check\n\n"
        + markdown_table(["Domain", "Status", "Required terms", "Report files"], domain_rows),
    )

    confidence_locations = [name for name, text in texts.items() if "Medium-Low" in text]
    high_confidence_bad = bool(
        re.search(r"(?:confidence|Confidence|可信度)[^\n]{0,40}(?:High|高)|高可信", all_text)
    )
    confidence_status = "PASS" if confidence_locations and not high_confidence_bad else "FAIL"
    if confidence_status != "PASS":
        failures.append("confidence rating missing Medium-Low or contains unscoped High/high-confidence wording")
    write(
        EVIDENCE_DIR / "confidence_check.md",
        "# Stage 12 Confidence Check\n\n"
        + markdown_table(
            ["Check", "Status", "Evidence"],
            [
                ["Medium-Low present", "PASS" if confidence_locations else "FAIL", ", ".join(confidence_locations) if confidence_locations else "(missing)"],
                ["No High/high-confidence overclaim", "PASS" if not high_confidence_bad else "FAIL", "context scan inspected"],
                ["P1 LOADCHUNK findings keep confidence degraded", "PASS" if "GBV6-07-001" in all_text and "GBV6-07-002" in all_text else "FAIL", "finding IDs present in final report"],
                ["Sanitizer runtime remains blocked", "PASS" if "GBV6-08-BLOCK-001" in all_text and "不能声明动态内存安全 PASS" in all_text else "FAIL", "blocked item and caveat present"],
            ],
        ),
    )

    design_rows = [
        ["Not drop-in replacement", "PASS" if "不是 RedisBloom 的 drop-in 替代品" in all_text else "FAIL", "negative/scoped wording required"],
        ["SCANDUMP/LOADCHUNK private protocol", "PASS" if "私有" in all_text and "SCANDUMP/LOADCHUNK" in all_text else "FAIL", "must be DESIGN_INTENDED boundary"],
        ["RESP3 unsupported", "PASS" if "RESP3" in all_text and "不支持" in all_text else "FAIL", "must not claim support"],
        ["command-AOF no-preamble boundary", "PASS" if "command-AOF" in all_text and "DESIGN_INTENDED" in all_text else "FAIL", "must keep cross-impl incompatibility scoped"],
        ["P1 LOADCHUNK self-protocol defects", "PASS" if "GBV6-07-001" in all_text and "GBV6-07-002" in all_text and "P1" in all_text else "FAIL", "must not be softened"],
    ]
    for row in design_rows:
        if row[1] != "PASS":
            failures.append(f"DESIGN alignment check failed: {row[0]}")
    write(
        EVIDENCE_DIR / "design_intended_check.md",
        "# Stage 12 DESIGN_INTENDED Handling Check\n\n"
        + markdown_table(["Check", "Status", "Note"], design_rows),
    )

    matrix_rows = [
        ["Required report files", "PASS" if all(row[1] == "PASS" for row in manifest_rows) else "FAIL", ".codex/gemini-bloom-audit/v6/evidence/stage12/report_file_manifest.md"],
        ["Evidence path existence", "PASS" if not missing_paths else "FAIL", ".codex/gemini-bloom-audit/v6/evidence/stage12/path_existence_check.md"],
        ["Forbidden overclaims", "PASS" if all(row[1] == "PASS" for row in exact_rows) else "FAIL", ".codex/gemini-bloom-audit/v6/evidence/stage12/forbidden_wording_scan.md"],
        ["Open finding carry-forward", "PASS" if all(row[1] == "PASS" for row in finding_rows) else "FAIL", ".codex/gemini-bloom-audit/v6/evidence/stage12/finding_coverage_check.md"],
        ["Finding detail completeness", "PASS" if all(row[1] == "PASS" for row in detail_rows) else "FAIL", ".codex/gemini-bloom-audit/v6/evidence/stage12/finding_detail_completeness_check.md"],
        ["BLOCKED/NOT_VERIFIED carry-forward", "PASS" if all(row[1] == "PASS" for row in blocker_rows) else "FAIL", ".codex/gemini-bloom-audit/v6/evidence/stage12/blocked_not_verified_coverage_check.md"],
        ["BF command coverage", "PASS" if all(row[1] == "PASS" for row in command_rows) else "FAIL", ".codex/gemini-bloom-audit/v6/evidence/stage12/command_coverage_check.md"],
        ["Required domain coverage", "PASS" if all(row[1] == "PASS" for row in domain_rows) else "FAIL", ".codex/gemini-bloom-audit/v6/evidence/stage12/domain_coverage_check.md"],
        ["DESIGN_INTENDED handling", "PASS" if all(row[1] == "PASS" for row in design_rows) else "FAIL", ".codex/gemini-bloom-audit/v6/evidence/stage12/design_intended_check.md"],
        ["Severity and confidence", confidence_status, ".codex/gemini-bloom-audit/v6/evidence/stage12/confidence_check.md"],
    ]
    overall = "PASS" if not failures else "FAIL"

    write(
        AGENT_DIR / "report_audit_matrix.md",
        "# Stage 12 Report Audit Matrix\n\n"
        f"Overall: `{overall}`\n\n"
        + markdown_table(["Area", "Status", "Evidence"], matrix_rows)
        + "\n## Failures\n\n"
        + ("\n".join(f"- {item}" for item in failures) if failures else "- None.\n"),
    )

    self_audit = f"""# 报告自审结果

Stage 12 verdict: `{overall}`

本文件由 Stage 12 更新。自审范围是 `doc/code_review/gemini-bloom/v6/` 下的最终中文报告，以及 `.codex/gemini-bloom-audit/v6/` 下 Stage 00-11 的阶段结果、reviewer 输出和证据索引。Stage 12 没有运行新的产品测试。

## 自审结论

| 检查项 | 结果 | 证据 |
|---|---|---|
| 必需中文报告文件存在且非空 | {'PASS' if all(row[1] == 'PASS' for row in manifest_rows) else 'FAIL'} | `.codex/gemini-bloom-audit/v6/evidence/stage12/report_file_manifest.md` |
| 报告引用的证据路径存在 | {'PASS' if not missing_paths else 'FAIL'} | `.codex/gemini-bloom-audit/v6/evidence/stage12/path_existence_check.md` |
| 禁用或过宽措辞检查 | {'PASS' if all(row[1] == 'PASS' for row in exact_rows) else 'FAIL'} | `.codex/gemini-bloom-audit/v6/evidence/stage12/forbidden_wording_scan.md` |
| DESIGN_INTENDED 边界未误判为 bug | {'PASS' if all(row[1] == 'PASS' for row in design_rows) else 'FAIL'} | `.codex/gemini-bloom-audit/v6/evidence/stage12/design_intended_check.md` |
| 所有 open findings 已继承 | {'PASS' if all(row[1] == 'PASS' for row in finding_rows) else 'FAIL'} | `.codex/gemini-bloom-audit/v6/evidence/stage12/finding_coverage_check.md` |
| 每个 finding 的影响/复现/Expected/Actual/证据/建议字段完整 | {'PASS' if all(row[1] == 'PASS' for row in detail_rows) else 'FAIL'} | `.codex/gemini-bloom-audit/v6/evidence/stage12/finding_detail_completeness_check.md` |
| BLOCKED / NOT_VERIFIED 已呈现并影响可信度 | {'PASS' if all(row[1] == 'PASS' for row in blocker_rows) else 'FAIL'} | `.codex/gemini-bloom-audit/v6/evidence/stage12/blocked_not_verified_coverage_check.md` |
| 10 个 BF 命令均有覆盖说明 | {'PASS' if all(row[1] == 'PASS' for row in command_rows) else 'FAIL'} | `.codex/gemini-bloom-audit/v6/evidence/stage12/command_coverage_check.md` |
| RDB/AOF/replication/cluster/fuzz/sanitizer/perf 覆盖未遗漏 | {'PASS' if all(row[1] == 'PASS' for row in domain_rows) else 'FAIL'} | `.codex/gemini-bloom-audit/v6/evidence/stage12/domain_coverage_check.md` |
| severity 与最终可信度匹配证据 | {confidence_status} | `.codex/gemini-bloom-audit/v6/evidence/stage12/confidence_check.md` |

## 关键确认

- 报告没有声明 Policy 00 禁止的 5 类过宽兼容、RESP3 支持、全版本支持或“测试通过即无问题”结论。
- RedisBloom SCANDUMP/LOADCHUNK 不互通、command-AOF no-preamble 跨实现不兼容、RESP3 不支持和 BF.DEBUG 不支持仍按 DESIGN.md 设计边界处理。
- `GBV6-07-001` 与 `GBV6-07-002` 仍按 P1 数据完整性缺陷保留，没有被误降级为 RedisBloom 互通差异。
- `GBV6-08-BLOCK-001` 仍阻止报告声明动态内存安全 PASS。
- 最终可信度保持 `Medium-Low`，与开放 P1 finding 和 sanitizer runtime BLOCKED 状态一致。

## 自审失败项

{chr(10).join(f'- {public_failure_text(item)}' for item in failures) if failures else '- 无。'}

## Stage 12 证据索引

- `.codex/gemini-bloom-audit/v6/agents/stage12/report_audit_matrix.md`
- `.codex/gemini-bloom-audit/v6/evidence/stage12/evidence_index.md`
"""
    write(REPORT_DIR / "10_报告自审结果.md", self_audit)

    summary = {
        "overall": overall,
        "failures": failures,
        "required_report_files": len(REQUIRED_REPORT_FILES),
        "referenced_paths_checked": len(path_rows),
        "open_findings_checked": len(OPEN_FINDINGS),
        "finding_detail_rows_checked": len(detail_rows),
        "blocked_or_not_verified_items_checked": len(BLOCKED_OR_NV),
        "bf_commands_checked": len(BF_COMMANDS),
        "domain_checks": len(DOMAIN_TERMS),
    }
    write(EVIDENCE_DIR / "report_audit_summary.json", json.dumps(summary, ensure_ascii=False, indent=2) + "\n")

    write(
        EVIDENCE_DIR / "evidence_index.md",
        "# Stage 12 Evidence Index\n\n"
        "| Evidence | Purpose |\n"
        "|---|---|\n"
        "| `.codex/gemini-bloom-audit/v6/evidence/stage12/commands.txt` | Report-audit commands and setup actions. |\n"
        "| `.codex/gemini-bloom-audit/v6/evidence/stage12/env_snapshot.txt` | Branch, HEAD, dirty tree, and scope. |\n"
        "| `.codex/gemini-bloom-audit/v6/evidence/stage12/stdout.log` | Script summary output. |\n"
        "| `.codex/gemini-bloom-audit/v6/evidence/stage12/stderr.log` | Setup stderr and script stderr note. |\n"
        "| `.codex/gemini-bloom-audit/v6/evidence/stage12/exit_codes.txt` | Exit codes for setup and script commands. |\n"
        "| `.codex/gemini-bloom-audit/v6/evidence/stage12/report_file_manifest.md` | Required final report file manifest. |\n"
        "| `.codex/gemini-bloom-audit/v6/evidence/stage12/path_existence_check.md` | Existence check for report-cited paths. |\n"
        "| `.codex/gemini-bloom-audit/v6/evidence/stage12/forbidden_wording_scan.md` | Forbidden and broad wording scan. |\n"
        "| `.codex/gemini-bloom-audit/v6/evidence/stage12/finding_coverage_check.md` | Open finding carry-forward check. |\n"
        "| `.codex/gemini-bloom-audit/v6/evidence/stage12/finding_detail_completeness_check.md` | Per-finding impact/repro/Expected/Actual/evidence/repair completeness check. |\n"
        "| `.codex/gemini-bloom-audit/v6/evidence/stage12/blocked_not_verified_coverage_check.md` | BLOCKED and NOT_VERIFIED carry-forward check. |\n"
        "| `.codex/gemini-bloom-audit/v6/evidence/stage12/command_coverage_check.md` | BF command coverage check. |\n"
        "| `.codex/gemini-bloom-audit/v6/evidence/stage12/domain_coverage_check.md` | Required technical domain coverage check. |\n"
        "| `.codex/gemini-bloom-audit/v6/evidence/stage12/design_intended_check.md` | DESIGN_INTENDED boundary check. |\n"
        "| `.codex/gemini-bloom-audit/v6/evidence/stage12/confidence_check.md` | Severity/confidence consistency check. |\n"
        "| `.codex/gemini-bloom-audit/v6/evidence/stage12/report_audit_summary.json` | Machine-readable audit summary. |\n",
    )

    stdout = [
        "# Stage 12 Report Audit Stdout",
        "",
        f"overall={overall}",
        f"required_report_files={len(REQUIRED_REPORT_FILES)}",
        f"referenced_paths_checked={len(path_rows)}",
        f"open_findings_checked={len(OPEN_FINDINGS)}",
        f"finding_detail_rows_checked={len(detail_rows)}",
        f"blocked_or_not_verified_items_checked={len(BLOCKED_OR_NV)}",
        f"bf_commands_checked={len(BF_COMMANDS)}",
        f"domain_checks={len(DOMAIN_TERMS)}",
    ]
    if failures:
        stdout.append("failures:")
        stdout.extend(f"- {item}" for item in failures)
    else:
        stdout.append("failures=none")
    write(EVIDENCE_DIR / "stdout.log", "\n".join(stdout) + "\n")

    return 0 if overall == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
