# Stage 00 Findings

## GBV6-00-001

| Field | Value |
|---|---|
| Severity | P3 |
| Status | OPEN |
| Classification | DOC_RISK / TEST_EVIDENCE_GAP |
| Title | DESIGN.md claims RedisBloom v2.4.20 compat fixtures exist, but the path is absent |
| Affected area | `modules/gemini-bloom/DESIGN.md` test/compat evidence claim |
| Evidence | `.codex/gemini-bloom-audit/v6/evidence/stage00/stderr.log`, `.codex/gemini-bloom-audit/v6/evidence/stage00/stdout.log` |

DESIGN.md line 45 states that RDB fixture files and expected results are stored in `tests/compat/redisbloom-2.4.20/`. Static inventory on the audited `origin/main` tree found only `modules/gemini-bloom/tests`, `tests/pict`, `tests/pict/__pycache__`, and `tests/tcl`; `ls -ld modules/gemini-bloom/tests/compat/redisbloom-2.4.20` failed with `No such file or directory`.

Impact: RedisBloom corpus claims cannot be reproduced from the audited tree as documented. Later Stage 05 may still run an oracle another way, but final confidence must account for missing checked-in fixtures unless evidence is added.

Suggested follow-up: either add the referenced fixtures/expected results to the repository or revise DESIGN.md/final report to state where compat evidence lives and how it is reproduced.

## GBV6-00-002

| Field | Value |
|---|---|
| Severity | P3 |
| Status | OPEN |
| Classification | DOC_RISK / SOURCE_COMMENT_CONFLICT |
| Title | `sb_chain.h` SCANDUMP/LOADCHUNK comment contradicts DESIGN.md private protocol boundary |
| Affected area | `modules/gemini-bloom/src/sb_chain.h`, `modules/gemini-bloom/DESIGN.md` |
| Evidence | `.codex/gemini-bloom-audit/v6/evidence/stage00/stdout.log` |

DESIGN.md repeatedly states that BF.SCANDUMP/BF.LOADCHUNK use a gemini-private layer-index cursor/header protocol and are not RedisBloom-interoperable. However `modules/gemini-bloom/src/sb_chain.h:88-91` says the wire-format structures for BF.SCANDUMP/BF.LOADCHUNK match the RedisBloom binary protocol for cross-implementation compatibility and were verified against RedisBloom v2.4.20.

Impact: this is not a runtime bug by itself, but it can mislead maintainers and future auditors into treating SCANDUMP/LOADCHUNK as a RedisBloom compatibility surface, directly conflicting with the highest-priority DESIGN.md boundary.

Suggested follow-up: update the source comment to match DESIGN.md's private protocol boundary, or update DESIGN.md only if later evidence proves the source comment is correct. This audit does not modify production code by policy.
