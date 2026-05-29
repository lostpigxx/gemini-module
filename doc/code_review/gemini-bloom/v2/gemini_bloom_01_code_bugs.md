> 分析基准：GitHub 仓库 `lostpigxx/gemini-module` 的 `main` 分支当前可读取源码；重点目录为 `modules/gemini-bloom`。  
> 方法：静态源码审查 + 与当前 RedisBloom 官方源码/命令文档做行为对照。未在本地编译运行；容器内无法直接 `git clone`，因此结论基于 GitHub API 拉取到的文件内容。  
> 严重性：P0=可能崩溃/数据损坏/安全风险；P1=兼容性或核心语义错误；P2=明显设计/性能/可维护性缺陷；P3=低风险但应修正。

# 01. 代码 Bug 挑刺

## 总览

本文件只列“会导致错误行为、未定义行为、崩溃、数据损坏、错误回复或危险边界行为”的问题。设计取舍、兼容性偏差、性能问题分别见其他文件。

| ID | 严重性 | 位置 | 问题摘要 |
|---|---:|---|---|
| BUG-01 | P0 | `src/sb_chain.cc:66-82` | 对含非平凡对象的数组使用 `RMRealloc`，对象生命周期未定义 |
| BUG-02 | P0 | `src/sb_chain.cc:147-170` | RDB/LOADCHUNK 反序列化后向未构造的 `FilterLayer` 赋值 |
| BUG-03 | P0 | `src/bloom_commands.cc`, `src/bloom_config.cc`, `src/sb_chain.cc` | `EXPANSION` 从 `long long` 转 `unsigned` 会截断，可能在扩容时除零 |
| BUG-04 | P0 | `src/bloom_filter.cc:105-117` | 极端参数下 `double` 到 `uint64_t` 转换越界，属于未定义行为 |
| BUG-05 | P0 | `src/bloom_rdb.cc:54-80` | RDB 层元数据缺少完整校验，`totalBits + 7` 可溢出并接受损坏对象 |
| BUG-06 | P0 | `src/bloom_filter.cc:128-152`, `src/bloom_filter.h:141` | 反序列化的 `log2Bits` 可触发移位 UB，`IsPowerOfTwo()` 判定错误 |
| BUG-07 | P1 | `src/bloom_rdb.cc:184-235` | LOADCHUNK header 校验不足，可接受畸形/超大/带尾随垃圾的 header |
| BUG-08 | P1 | `src/bloom_commands.cc:82-91, 191-202, 307-317` | 多元素命令内部写入 error reply，回复结构和部分写入语义错误 |
| BUG-09 | P1 | `src/bloom_commands.cc:33-44` | `BF.SCANDUMP` header 分配失败时伪装成正常结束 |
| BUG-10 | P1 | `src/bloom_rdb.cc:256-265` | AOF rewrite header 分配失败时直接返回，重写后的 AOF 可能丢 key |
| BUG-11 | P1 | `src/redis_bloom_module.cc`, `src/bloom_rdb.cc`, `include/mock_redismodule_io.h` | 使用 IO error API 但未设置 Redis module IO error 选项；mock 也未覆盖 |
| BUG-12 | P1 | `src/bloom_commands.cc:139-143` | `BF.RESERVE` 对 wrong type key 返回“key already exists”而不是 WRONGTYPE |
| BUG-13 | P3 | `src/sb_chain.cc:113-123` | 固定容量 filter 已满时 false positive 会返回 duplicate；这是 Bloom/RedisBloom 语义边界，不应列为实现 bug |
| BUG-14 | P3 | `src/bloom_filter.cc:100-104`, RDB flags | `RawBits` 创建路径未完整实现；命令路径不暴露，LOADCHUNK 会拒绝 `hashCount=0`，主要是潜在 invariant/兼容性问题 |
| BUG-15 | P2 | `src/bloom_filter.cc:25-36` | hash 输入长度被静默截断到 `INT_MAX` |
| BUG-16 | P2 | 多处 `ReplyWithLongLong(static_cast<long long>(uint64_t))` | 大容量/大计数可能转成负数或错误值 |
| BUG-17 | P2 | `src/sb_chain.cc:137-143` | `BytesUsed()` 计算可能 `size_t` 溢出 |
| BUG-18 | P2 | `src/bloom_rdb.cc:126-156` | 读取每层 `itemCount` 后不检查 IO 状态 |
| BUG-19 | P2 | `src/bloom_rdb.cc:126-156`, `src/sb_chain.cc:147-163` | RDB `numLayers` 无上限，畸形 RDB 可触发巨量分配 |
| BUG-20 | P2 | `src/sb_chain.cc:152` | `shell.numLayers * sizeof(FilterLayer)` 无乘法溢出检查 |

## 详细问题

### BUG-01：`RMRealloc` 不能安全搬运 `FilterLayer`

`ScalingBloomFilter::AppendLayer()` 在扩容 `layers_` 时直接调用：

```cpp
auto* expanded = static_cast<FilterLayer*>(
  RMRealloc(layers_, newCap * sizeof(FilterLayer)));
```

`FilterLayer` 内含 `BloomLayer`，而 `BloomLayer` 有自定义析构、移动构造、移动赋值，并删除了拷贝构造/赋值。这样的类型不是 trivially relocatable。`realloc` 只做字节复制和释放旧块，不会调用移动构造，也不会调整源对象生命周期。

影响：

1. 当前 `BloomLayer` 主要持有裸指针，字节搬运在多数平台上“看起来能跑”，但 C++ 对象生命周期仍然不成立。
2. 未来只要 `BloomLayer` 增加非平凡成员，扩容可能立刻变成双重释放、泄漏或悬垂指针。
3. ASAN 未必能稳定抓到；UBSAN/object-lifetime sanitizer 更可能暴露。

修复建议：

- 用 `std::vector<FilterLayer, RedisAllocator<FilterLayer>>`。
- 或手写 `reserve`：`RMAlloc` 新内存 → placement-new move 每个元素 → 显式析构旧元素 → `RMFree` 旧内存。
- 不要对 C++ 非平凡对象使用 `realloc`。

### BUG-02：反序列化路径向未构造对象赋值

`ScalingBloomFilter::FromRdbShell()` 用 `RMCalloc` 分配 `FilterLayer` 数组：

```cpp
filter->layers_ = static_cast<FilterLayer*>(
  RMCalloc(shell.numLayers, sizeof(FilterLayer)));
filter->numLayers_ = shell.numLayers;
```

随后 `SetLayer()` 执行：

```cpp
layers_[index] = {std::move(layer.bloom), layer.itemCount};
```

问题是 `RMCalloc` 只分配并清零原始内存，并没有 placement-new 构造 `FilterLayer`。向一个未开始生命周期的 C++ 对象做赋值是未定义行为。此路径被 RDB load 和 `BF.LOADCHUNK` header load 共同使用，因此不是死代码问题。

修复建议：

```cpp
filter->layerCapacity_ = shell.numLayers;
filter->numLayers_ = 0;
filter->layers_ = static_cast<FilterLayer*>(RMAlloc(shell.numLayers * sizeof(FilterLayer)));

void SetLayer(size_t index, FilterLayer&& layer) {
  // index 应该等于当前已构造数量，或先显式析构旧对象
  new (&layers_[index]) FilterLayer{std::move(layer.bloom), layer.itemCount};
  numLayers_ = std::max(numLayers_, index + 1);
}
```

更好的修复仍然是 Redis allocator + `std::vector`。

### BUG-03：`EXPANSION` 截断后可导致除零

命令层把 `long long` 直接转成 `unsigned`：

```cpp
expansion = static_cast<unsigned>(val);
```

配置层也一样：

```cpp
g_bloomConfig.defaultExpansion = static_cast<unsigned>(val);
```

代码只检查 `val >= 1`，没有检查 `val <= UINT_MAX`。在常见 32-bit `unsigned` 上，输入 `4294967296` 会转换成 `0`。之后扩容路径执行：

```cpp
if (prevCap > UINT64_MAX / expansionFactor_) return false;
```

当 `expansionFactor_ == 0` 时是整数除零，直接崩溃或触发未定义行为。

触发面：

- `BF.RESERVE k 0.01 10 EXPANSION 4294967296`
- `BF.INSERT k EXPANSION 4294967296 ITEMS a`
- 模块加载参数 `EXPANSION 4294967296`

修复建议：

```cpp
if (val < 1 || val > std::numeric_limits<unsigned>::max()) {
  return RedisModule_ReplyWithError(ctx, "ERR EXPANSION value out of range");
}
```

并在 `ScalingBloomFilter` 构造入口再次断言 `expansion >= 1`。

### BUG-04：极端容量和误判率下存在浮点到整数越界 UB

`BloomLayer::Create()` 计算：

```cpp
auto rawBits = static_cast<double>(cap) * layer.bitsPerEntry_;
layer.totalBits_ = static_cast<uint64_t>(std::max(rawBits, 1024.0));
```

当 `cap` 很大且 `falsePositiveRate` 很小时，`rawBits` 可能超过 `uint64_t` 可表示范围，甚至变成 `inf`。C++ 中浮点数转换到整数，如果值不可表示，行为未定义。

命令层允许：

- `capacity` 到 `LLONG_MAX`
- `error_rate` 只要 `0 < rate < 1`

因此 `BF.RESERVE k 1e-300 9223372036854775807` 这类输入可能触发未定义行为或错误容量计算。

修复建议：

- 在命令层和构造层设置硬上限。
- 先检查 `std::isfinite(rawBits)`。
- 检查 `rawBits <= static_cast<double>(UINT64_MAX - 7)`。
- 拒绝会导致 `dataSize` 超过 Redis/模块允许内存上限的请求。

### BUG-05：RDB `BloomLayer::ReadFrom()` 接受危险元数据

RDB 读取层只读字段、计算 `dataSize`、校验 blob 长度：

```cpp
layer.totalBits_ = r.GetUint();
layer.log2Bits_ = static_cast<uint8_t>(r.GetUint());
layer.dataSize_ = (layer.totalBits_ > 0) ? ((layer.totalBits_ + 7) / 8) : 0;
...
if (!r.Ok() || !buf || bufLen != static_cast<size_t>(layer.dataSize_)) ...
```

缺失校验：

- `totalBits + 7` 会 `uint64_t` 溢出。例如 `UINT64_MAX + 7` 回绕，`dataSize_` 变成 0。
- `hashCount_ == 0` 但不是 RawBits 时会让 `Test()` 空循环返回 true。
- `fpRate_`、`bitsPerEntry_` 可为 NaN/Inf/负数。
- `log2Bits_` 可为 64 以上，引发移位 UB。
- `capacity_ == 0`、`itemCount > capacity`、`totalItems != sum(itemCount)` 都不检查。
- `totalBits` 与 blob 长度只按当前公式做等长校验，但公式本身可溢出。

修复建议：

新增统一的 `ValidateLayerInvariants()`，并在 `Create`、RDB load、LOADCHUNK header load 后都调用。

### BUG-06：`ComputeModuloMask()` 可被损坏数据触发移位 UB

```cpp
uint64_t BloomLayer::ComputeModuloMask() const {
  return (1ULL << log2Bits_) - 1;
}
```

`log2Bits_` 是从 RDB/LOADCHUNK 读入的 `uint8_t`。如果是 64、65 等值，`1ULL << log2Bits_` 是未定义行为。

同时：

```cpp
bool IsPowerOfTwo() const { return log2Bits_ > 0; }
```

这不是“是否 power-of-two”的判断，而是“是否记录了非零 log2”。反序列化数据只要把 `log2Bits` 填成 1，即使 `totalBits` 不是 2，也会走 mask 路径，导致查询/插入映射到错误位置。

修复建议：

```cpp
bool IsPowerOfTwo() const {
  return totalBits_ != 0 && (totalBits_ & (totalBits_ - 1)) == 0;
}

bool ValidateLog2() const {
  return !IsPowerOfTwo() || (log2Bits_ < 64 && (1ULL << log2Bits_) == totalBits_);
}
```

### BUG-07：LOADCHUNK header 校验不足

`DeserializeHeader()` 做了基础校验，但仍有明显洞：

```cpp
if (length < required) return nullptr;
```

它接受 `length > required` 的 payload。官方 RedisBloom 的 header load 要求长度精确匹配。接受尾随垃圾会降低数据格式的确定性，也让 fuzz/攻击面变大。

`ValidateLayerMeta()` 还存在：

```cpp
uint64_t expectedSize = (meta.totalBits + 7) / 8;
if (meta.dataSize < expectedSize) return false;
```

问题：

- `meta.totalBits + 7` 可溢出。
- 只拒绝小于 expected 的 dataSize，允许大于 expected 的 dataSize。
- 不校验 `capacity > 0`。
- 不校验 `itemCount <= capacity`。
- 不校验 `log2Bits` 与 `totalBits` 一致。
- 不校验 flags 是否只包含已知位。
- 不校验 `totalItems == sum(layer.itemCount)`。

修复建议：

- `length == required`。
- `totalBits <= UINT64_MAX - 7`。
- `dataSize == (totalBits + 7) / 8`。
- `knownFlagMask` 校验。
- header 级别校验总数与各层计数一致。

### BUG-08：多元素命令错误回复和部分写入语义错误

`PutAndReply()` 在 filter 满时直接：

```cpp
RedisModule_ReplyWithError(ctx, "ERR reached capacity limit (non-scaling mode)");
return false;
```

但 `CmdMadd()` / `CmdInsert()` 已经先调用了：

```cpp
RedisModule_ReplyWithArray(ctx, count);
```

结果是错误被写进数组元素位置，或导致客户端观察到非预期结构。更严重的是，循环没有立即中止，之前已经成功插入的元素也不会回滚。

修复建议：

- 明确语义：多元素命令是“部分成功”还是“全有或全无”。
- 如果要兼容 RedisBloom，满容量时应遵循 RedisBloom 当前行为：遇到 full 后停止并设置数组实际长度，或按官方版本返回对应 error element。
- 如果要强一致，先 dry-run 容量/状态，再提交；或记录 undo 信息。

### BUG-09：`BF.SCANDUMP` OOM 时返回正常结束

```cpp
auto* hdrBuf = static_cast<char*>(RMAlloc(hdrBytes));
if (!hdrBuf) {
  RedisModule_ReplyWithLongLong(ctx, 0);
  RedisModule_ReplyWithStringBuffer(ctx, "", 0);
  return REDISMODULE_OK;
}
```

这会让客户端误以为 dump 已完成，并得到一个空目标。OOM 应该返回 error，而不是协议里的完成标记。

修复建议：

```cpp
return RedisModule_ReplyWithError(ctx, "ERR allocation failure");
```

### BUG-10：AOF rewrite 分配失败会静默丢数据

`AofRewriteBloom()`：

```cpp
auto* hdrBuf = static_cast<char*>(RMAlloc(hdrBytes));
if (!hdrBuf) return;
```

这在 AOF rewrite 回调中会让该 key 没有任何重写命令输出。后续用新 AOF 恢复时 key 丢失。

修复建议：

- 至少 `RedisModule_LogIOError` / module log。
- 避免需要一次性分配 header，或预计算并保证小 header 不会失败。
- 如果 Redis Module API 允许，触发 rewrite 失败，而不是产出损坏 AOF。

### BUG-11：IO error 处理链条不完整

RDB reader 调用：

```cpp
if (RedisModule_IsIOError(io_)) ok_ = false;
```

但模块加载时没有调用类似 RedisBloom 官方实现里的：

```c
RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_HANDLE_IO_ERRORS);
```

测试 mock `InstallMockRedisModuleIO()` 只安装 Save/Load/Free 函数指针，也没有安装 `RedisModule_IsIOError`。这意味着：

- 真实 Redis 中 IO error 语义可能不完整。
- 单测没有覆盖 IO error，甚至可能因为函数指针未安装而崩溃；若当前 header 有默认 stub，也只是掩盖问题。

修复建议：

- OnLoad 设置 `REDISMODULE_OPTIONS_HANDLE_IO_ERRORS`。
- mock 中实现 `RedisModule_IsIOError` 和可控错误标志。
- 增加 truncated stream / IO error 单测。

### BUG-12：`BF.RESERVE` wrong type 错误码错误

当前：

```cpp
if (RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_EMPTY) {
  return RedisModule_ReplyWithError(ctx, "ERR key already exists");
}
```

如果 key 是 string/list/hash，应该返回 WRONGTYPE，而不是“key already exists”。否则客户端无法区分“已有 Bloom filter”和“类型错误”。

修复建议：

```cpp
int type = RedisModule_KeyType(key);
if (type == REDISMODULE_KEYTYPE_EMPTY) ...
if (type == REDISMODULE_KEYTYPE_MODULE && GetFilter(key)) return ERR item exists;
return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
```

### BUG-13：固定容量满时 false positive 返回 duplicate 是语义边界

`Put()` 顺序是：

```cpp
if (IsDuplicate(hp)) return false;
if (!GrowIfNeeded()) return std::nullopt;
```

复核结论：原报告把这一项列为 P1 代码 bug 过重，且“应先报满”的建议不适合作为默认修复。Bloom filter 无法区分真实重复和 false positive；新元素即使在未满时也可能被误判为存在。RedisBloom 的插入路径同样先按 Bloom 语义判断是否存在，再处理实际写入/扩容，因此 fixed-size filter 已满时被 false positive 命中的新元素返回 duplicate `0` 属于兼容语义边界，而不是 Gemini 独有实现错误。

后续处理应取决于产品目标：

- 如果严格按 RedisBloom 行为，应保留当前顺序，并在文档中说明 Bloom false positive 会影响返回值和 `BF.CARD` 计数。
- 如果产品语义改成“达到 capacity 后任何输入都拒绝”，才应先检查 fixed-size top layer 是否满；但这会让真实重复项也可能返回 capacity error，破坏 RedisBloom 兼容性。

### BUG-14：`RawBits` 模式未完整实现，但不是当前命令路径的 P1 bug

`BloomLayer::Create(... RawBits)` 设置：

```cpp
layer.totalBits_ = cap;
layer.hashCount_ = 0;
```

`Test()` 对 `hashCount_ == 0` 的层会执行 0 次循环并返回 true；`Insert()` 会返回 false 且不设置任何 bit。这等价于“所有元素都存在”。

复核结论：原报告把这一项列为 P1 运行时 bug 过重。当前公开命令创建路径只使用 `Use64Bit | NoRound`，可选 `FixedSize`，不会创建 `RawBits` filter；`BF.LOADCHUNK` header 路径还会通过 `ValidateLayerMeta()` 拒绝 `hashCount == 0 && totalBits > 0` 的 bit-array 元数据。因此它主要是内部 API / 损坏 RDB / 未来 RedisBloom 兼容路径上的 latent invariant 问题，而不是普通 BF 命令可直接触发的核心 bug。

修复建议：

- 如果不支持 RawBits，删除 flag 并在反序列化拒绝。
- 如果支持 RawBits，应定义 hashCount、bitsPerEntry 和查询语义。

### BUG-15：超长输入 hash 被静默截断

```cpp
auto len = static_cast<int>(std::min(data.size(), static_cast<size_t>(INT_MAX)));
```

长度超过 `INT_MAX` 的输入只 hash 前 `INT_MAX` 字节。两个只在后半部分不同的大对象会被视为同一个元素。

Redis 单个字符串通常小于该上限，因此普通命令路径风险较低；但内部 API 接收 `std::span`，不应埋静默截断。

修复建议：

- 把 Murmur API 改成 `size_t len`。
- 或显式拒绝 `data.size() > INT_MAX`，不要悄悄截断。

### BUG-16：`uint64_t` 到 `long long` 回复可能溢出

例如：

```cpp
RedisModule_ReplyWithLongLong(ctx, static_cast<long long>(filter->TotalCapacity()));
```

当容量、size、items 超过 `LLONG_MAX` 时，回复会变成负数或实现相关值。虽然实际内存可能先耗尽，但命令层并没有硬限制，所以这是边界 bug。

修复建议：对可回复为 Redis integer 的指标设置 `<= LLONG_MAX` 的创建/载入上限。

### BUG-17：`BytesUsed()` 可溢出

```cpp
size_t base = sizeof(ScalingBloomFilter) + layerCapacity_ * sizeof(FilterLayer);
...
base + sum(dataSize)
```

没有乘法/加法溢出检查。该值会用于 `mem_usage` callback 和 `BF.INFO Size`，畸形 RDB/LOADCHUNK 数据可让其回绕成小值。

修复建议：使用 checked arithmetic；溢出时返回 `SIZE_MAX` 或拒绝对象载入。

### BUG-18：读取 `itemCount` 后没有检查 IO 状态

`ScalingBloomFilter::ReadFrom()`：

```cpp
size_t count = static_cast<size_t>(r.GetUint());
filter->SetLayer(i, {std::move(*maybeLayer), count});
```

如果 stream 在 layer blob 后截断，`GetUint()` 会返回 0 并把 `ok_` 置 false，但这里没有立即检查。函数可能返回一个计数错误的对象。

修复建议：

```cpp
uint64_t count = r.GetUint();
if (!r.Ok()) { free filter; return nullptr; }
```

### BUG-19：RDB `numLayers` 无上限

`DeserializeHeader()` 对 LOADCHUNK header 有 `kMaxLayers = 1024`，但 RDB `ReadFrom()` 没有对应上限。损坏 RDB 可指定巨大 `numLayers` 并触发巨量分配或长循环。

修复建议：RDB load 与 LOADCHUNK 共用同一套 header/filter invariant 校验。

### BUG-20：层数组分配乘法未检查溢出

```cpp
RMCalloc(shell.numLayers, sizeof(FilterLayer))
```

C allocator 对乘法溢出的行为依赖具体实现。Redis 的 allocator 未必会替调用方做语义级保护。应该在调用前检查：

```cpp
if (shell.numLayers > SIZE_MAX / sizeof(FilterLayer)) return nullptr;
```
