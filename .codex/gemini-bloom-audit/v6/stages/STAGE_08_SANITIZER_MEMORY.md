# Stage 08 — SANITIZER_MEMORY


## Universal stage protocol

本 stage 必须遵守：

1. 重新读取 `modules/gemini-bloom/DESIGN.md`、`LOOP_CONTROL_BATCH.md`、所有 policies、`LOOP_STATE.md`、本 stage 文件。
2. 创建 `.codex/gemini-bloom-audit/v6/agents/stage08/rehydrate_log.md`。
3. 创建 planner 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage08/planner_output.md`。
4. 主 agent 审核 planner，然后执行。
5. 所有证据写入 `.codex/gemini-bloom-audit/v6/evidence/stage08/`。
6. 写 `.codex/gemini-bloom-audit/v6/agents/stage08/stage_result.md`。
7. 创建 reviewer 子 agent，输出 `.codex/gemini-bloom-audit/v6/agents/stage08/reviewer_output.md`。
8. reviewer 未 PASS 时必须补跑/修正/降级，然后重新 reviewer。
9. 更新 `LOOP_STATE.md`。
10. commit + push。
11. 标记 planner/reviewer closed。



## Goal

用 sanitizer 或替代方式审计内存安全、UB、泄漏、越界、生命周期错误。

## Main tasks

优先执行：

```bash
cmake -B build-asan \
  -DENABLE_ASAN=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j$(nproc)
cmake --build build-asan -j$(nproc) --target bloom_test
```

尝试运行 TCL 集成测试加载 ASAN module。如果 redis-server 无法加载 sanitizer runtime，记录 BLOCKED，并尝试可行替代：

- `LD_PRELOAD` ASAN runtime。
- 只跑 GTest sanitizer。
- valgrind，如可用。
- 静态审计补充。

## Required evidence

- `evidence/stage08/asan_build/`
- `evidence/stage08/asan_gtest/`
- `evidence/stage08/asan_tcl/`
- `evidence/stage08/ubsan_findings.md`
- `evidence/stage08/blocked_sanitizer.md` 如有

## Pass criteria

- sanitizer build/test 的 stdout/stderr/exit code 落盘。
- sanitizer 失败必须分类：真实内存问题 vs 环境加载问题。
- reviewer 确认没有把无法加载 ASAN 的集成测试写成 PASS。

## Commit message

`audit(gemini-bloom): v6 stage 08 sanitizer memory`

