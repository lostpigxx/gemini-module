# Main Agent Stage Execution 模板

每个 stage 的主 agent 执行文件应写入：

- `.codex/gemini-bloom-audit/v6/agents/stageXX/main_execution.md`

结构：

```markdown
# Stage XX Main Execution

## 1. Rehydrate log
列出重新读取的文件。

## 2. Planner review
说明采纳了 planner 哪些建议，修正了哪些建议。

## 3. Commands executed
列出实际命令，或引用 evidence/stageXX/commands.txt。

## 4. Evidence produced
列出证据路径。

## 5. Results
按 PASS / FAIL / BLOCKED / NOT_VERIFIED / DESIGN_INTENDED 分类。

## 6. Findings
引用 FINDING_TEMPLATE。

## 7. Reviewer request
说明交给 reviewer 审计的材料。

## 8. Agent close status
Planner closed: yes/no
Reviewer closed: yes/no
```
