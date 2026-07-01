# Policy 03 — Evidence / 证据策略

## 1. 证据落盘规则

任何运行结果必须落盘到：

- `.codex/gemini-bloom-audit/v6/evidence/stageXX/`

每个 stage 至少包含：

- `commands.txt`：实际执行命令。
- `stdout.log`：标准输出。
- `stderr.log`：标准错误。
- `exit_codes.txt`：每个命令的 exit code。
- `env_snapshot.txt`：与本 stage 相关的环境信息。
- `evidence_index.md`：证据索引。

## 2. 结论引用规则

任何结论必须引用具体 evidence 路径。例如：

```text
结论：BF.LOADCHUNK malformed header 不破坏已有 key。状态：PASS。
证据：.codex/gemini-bloom-audit/v6/evidence/stage07/loadchunk_malformed/stdout.log
复现：.codex/gemini-bloom-audit/v6/evidence/stage07/loadchunk_malformed/commands.txt
```

禁止：

- “运行通过”但无日志。
- “看起来兼容”但无差分输出。
- “测试覆盖充分”但无 coverage matrix。

## 3. 随机测试规则

任何随机/fuzz/压力测试必须记录：

- seed。
- corpus 生成规则。
- 命令序列。
- 失败时最小化前后的输入。
- Redis/server/module 日志。

## 4. RedisBloom 对比证据

如果使用 RedisBloom oracle，必须记录：

- Redis server 版本。
- RedisBloom 版本。
- module 加载方式。
- gemini 实例端口和 RedisBloom 实例端口。
- raw RESP。
- normalized reply。
- 判定规则。

## 5. BLOCKED 证据

BLOCKED 也需要证据。例如：

- `docker: command not found`
- `redis-server not found`
- `GTest not found`
- `git push` 权限失败
- RedisBloom v2.4.20 无法下载或加载

不能只写“环境不支持”。
