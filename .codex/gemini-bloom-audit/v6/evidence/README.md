# Evidence outputs

每个 stage 创建独立目录：

```text
evidence/stageXX/
  commands.txt
  stdout.log
  stderr.log
  exit_codes.txt
  env_snapshot.txt
  evidence_index.md
```

运行测试、对比测试、fuzz、sanitizer、replica、cluster、perf 的原始输出都必须放在这里。
