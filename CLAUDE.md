# CLAUDE.md

项目级指令，所有使用 Claude Code 的协作者自动遵守。

## 构建与测试

```bash
# 构建
cmake -B build
cmake --build build -j$(nproc)

# Tcl 集成测试（需要 redis-server 可用）
tclsh modules/gemini-bloom/tests/tcl/bloom_test.tcl ./build/redis_bloom.so
```

## 编码规范

- 代码列宽 **120 字符**，不是 80
- 缩进用 **2 空格**（tab = 2 spaces）
- 函数名**首字母大写**（PascalCase），例如 `AppendLayer`、`ComputeHash`
- 指针 `*` 和引用 `&` **跟着类型**，不跟变量名：`int* ptr`，不是 `int *ptr`

## 规则

### 1. Clean-room 实现原则

gemini-search 模块必须严格遵守 clean-room 实现原则。RediSearch 采用 RSALv2/SSPL 许可证，引用其源码存在许可证风险。

- 只依赖公开文档（redis.io 命令语法、行为说明）
- 禁止阅读、引用或抓取 RediSearch 的 GitHub 源码
- 数据结构和算法必须独立设计
- 兼容的命令接口没问题（参考 Oracle v. Google），但禁止复制实现

### 2. 跨平台链接：禁止在测试编译单元中传递 redismodule.h

macOS ld64 会静默丢弃未引用的 `extern` 符号，但 GNU ld (Linux) 要求所有 `extern` 符号可解析。详见 `doc/cross_platform_linking.md`。

- 不要在编译进测试二进制的文件中 include `redismodule.h`（直接或间接）
- 包装了 `redismodule.h` 的头文件（`bloom_rdb.h`、`bloom_commands.h`、`bloom_config.h`）不能被与测试共享的纯算法文件（`bloom_filter.cc`、`sb_chain.cc`、`murmur2.cc`）include
- 保持 include 最小化——只 include 文件实际使用的头文件

### 3. Docker 离线化

Dockerfile 必须支持纯离线构建（面向无外网的内网环境）。所有外部依赖的源码以 tarball 形式放在 `deps/` 目录中，Dockerfile 从本地文件解压编译，禁止在构建过程中使用 `git clone`、`wget`、`curl` 等网络操作获取源码。打包 tarball 时在 macOS 上必须设置 `COPYFILE_DISABLE=1` 以排除 `._` 资源分叉文件。

### 4. Commit 规范

- gemini-search 相关的 commit message 必须包含 phase 编号（例如 "Phase 1"、"Phase 2"）
- 每次 commit 后立即 `git push` 到远端
