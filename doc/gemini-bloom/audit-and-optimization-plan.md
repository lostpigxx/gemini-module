# gemini-bloom 深度审计报告与分 Phase 优化加固规划

> 目标平台：**Redis 7**（基线 Redis 7.2.11，不再兼容 Redis 5 / 6；Clean-room C++20）  
> 兼容性边界：`MBbloom--` 保持与 RedisBloom 2.6.25（Redis 7.2）的 **RDB/DUMP/RESTORE/PSYNC 字节级双向兼容**；`MBcuckoo-`、`MBcms----`、`MBtopk---`、`MBtdigest` 保持命令级兼容，使用私有 RDB 格式。  
> 审计范围：`modules/gemini-bloom/src/*`（21 个 `.cc` / 21 个 `.h`，共 7,643 行）、`modules/gemini-bloom/tests/*`、`modules/gemini-bloom/CMakeLists.txt`、`ci/*`

---

## 一、深度审计结论

### 1. 公共基础设施与 Redis 7 平台适配（`murmur2.cc` / `redis_bloom_module.cc` / `*_rdb.cc`）

| 问题类别 | 现状与位置 | 影响 |
|---|---|---|
| **残留 Redis 5/6 兼容判断，未启用 Redis 7 KeySpecs** | `redis_bloom_module.cc:32` 仍做 `if (RedisModule_SetModuleOptions)` 运行时探测；`bloom_rdb.cc:24,30,37` 每次读写均做 `if (RedisModule_IsIOError)`；5 个模块全部使用旧版 `mem_usage` 而非 `mem_usage2` | 未发挥 Redis 7 能力；尤其 `CMS.MERGE` 与 `TDIGEST.MERGE` 写死 `(1, 1, 1)`，在 Redis 7 Cluster 与 ACL 下源 key 完全不可见 |
| **MurmurHash 逐字节拼接读取** | `murmur2.cc:14-18, 51-59` 主循环逐字节移位 OR 拼接 32/64 位字，且 `len` 为 `int` | 吞吐低于小端整字 `memcpy` 读取；所有子模块热路径均受影响 |
| **反序列化 2× 峰值内存与多余拷贝** | `BloomLayer::ReadFrom`（`bloom_rdb.cc:109-120`）、`ReadCfChain`（`cf_rdb.cc:97-119`）、`ReadCmsSketch`（`cms_rdb.cc:40-56`）、`ReadTopkSketch`（`topk_rdb.cc:61-78`）均先由 `RedisModule_LoadStringBuffer` 分配 `buf`，再 `RMAlloc`/`RMCalloc` 分配一份同等大小缓冲区并 `memcpy` 后释放 `buf` | 加载大对象（数百 MB ~ 1 GiB）时瞬时内存翻倍，易触发 OOM |
| **非对齐指针强转与空指针 `memcpy` UB** | `TdigestSerializeChunk`（`tdigest_rdb.cc:172`）在空 T-Digest 上 `memcpy(cursor, nullptr, 0)`；`TopkDeserializeChunk`（`topk_rdb.cc:183`）在变长 `nameLen` 后直接 `reinterpret_cast<const TopkWireEntryHeader*>` 读取 `uint64_t`；各 `*DeserializeHeader/Chunk` 均对输入 `const void*` 直接强转 packed/非 packed 结构体 | UBSan 报 `null-pointer-passed-as-arg` / `misaligned-address` UB |
| **大对象 AOF 重写可能超过 `proto-max-bulk-len`** | `AofRewriteCms`/`Topk`/`Tdigest` 将整个数据结构打包为单个 `*.LOADCHUNK` bulk 参数发送 | 当 CMS/TopK 达到 >512 MiB（上限 1 GiB）时，单条 AOF 命令超出默认 `proto-max-bulk-len` 导致重启加载失败 |
| **命令层 `-fno-exceptions` 下使用无界 `std::vector`** | `cms_commands.cc`、`topk_commands.cc`、`tdigest_commands.cc` 在 `-fno-exceptions` 编译选项下直接构造 `std::vector` 承载命令参数 | 遇超大 `argc` 或参数溢出时 `std::bad_alloc` 直接触发 `std::terminate()` 导致 Redis 进程崩溃 |

---

### 2. Bloom Filter（`bloom_filter.*` / `sb_chain.*` / `bloom_rdb.*` / `bloom_commands.cc` / `bloom_config.cc`）

| 问题类别 | 现状与位置 | 影响 |
|---|---|---|
| **默认模式每探针一次 64 位硬件除法** | `AllocFilter`（`bloom_commands.cc:21`）默认开启 `BloomFlags::NoRound`，导致 `ProbePosition`（`bloom_filter.h:103`）对每次探测执行 `raw % totalBits` | 64 位整数除法耗时 ~20–40 cycles，成为 `BF.ADD`/`BF.EXISTS`/`BF.MADD` 的头号 CPU 热点 |
| **`ScalingBloomFilter::Put` 顶层重复探测** | `sb_chain.cc:124-128` 先 `IsDuplicate(hp)` 从顶层向下扫所有层，紧接着对顶层再次调用 `target.bloom.Insert(hp)` | 单层过滤器每次新插入都对同一层做 2× 位探测 |
| **批量命令缺少软件预取** | `BF.MADD` / `BF.MEXISTS` 串行逐元素哈希+随机访存 | 大过滤器（超出 L3 Cache）批量操作受限于 DRAM 访存延迟 |
| **`NaN` 绕过 `ERROR_RATE` 校验** | `bloom_config.cc:22`、`bloom_commands.cc:106,272` 仅判断 `val <= 0.0 || val >= 1.0`，未校验 `std::isfinite(val)` | `BF.RESERVE k nan 100` 返回错误码不准确；更严重的是模块加载参数 `ERROR_RATE nan` 可被接受，导致此后所有隐式创建永久失败 |
| **`BF.LOADCHUNK` 异常删 key 未复制（主从分歧）** | `bloom_commands.cc:645,649,654` 在校验失败时调用 `RedisModule_DeleteKey(key)` 但未复制 `DEL` | 主节点删除了半加载 key，从节点仍残留半加载 key |

---

### 3. Cuckoo Filter（`cuckoo_filter.*` / `cf_chain.*` / `cf_rdb.*` / `cf_commands.cc` / `cf_config.cc`）

| 问题类别 | 现状与位置 | 影响 |
|---|---|---|
| **无单层/总内存上限（高危 OOM DoS）** | `kCfMaxBuckets = 1ULL << 30`、`kCfMaxBucketSize = 255`，`CuckooFilter::Create`（`cuckoo_filter.cc:108`）与 `cf_rdb.cc` 均无字节上限检查 | 恶意命令 `CF.RESERVE k 1073741824 BUCKETSIZE 255` 或畸形 RDB/LOADCHUNK 尝试分配 **255 GiB** |
| **非 2 的幂 `EXPANSION` 导致扩容必然失败（Bug）** | `CF.RESERVE` 允许任意 `expansion ∈ [0, 32768]`（如 `3`），但 `CuckooChain::Add`（`cf_chain.cc:123`）直接算 `nextBuckets = prevBuckets * expansion_`，而 `CuckooFilter::Create`（`cuckoo_filter.cc:98`）要求 `numBuckets` 为 2 的幂 | 设置 `EXPANSION 3` 等非 2 的幂值时，首层满后扩容必然失败报 `ERR filter is full` |
| **踢出循环热路径每次 `RMAlloc`/`RMFree`** | `CuckooFilter::Insert`（`cuckoo_filter.cc:178`）每次进入 kick loop 均堆分配 `sizeof(KickEntry) * maxIterations_`（`maxIterations = 65535` 时单次分配 **1 MiB**！） | 高负载因子下插入延迟暴涨、内存碎片剧增 |
| **多层链重复哈希 + 每次踢出调 `MurmurHash2`** | `CuckooChain::Contains`/`Delete`/`Count`（`cf_chain.cc:133-156`）逐层调用子 filter，每层重算 2 次 `MurmurHash2`；`CfAltIndex`（`cuckoo_filter.cc:61`）对 1 字节 `fp` 反复调用 `MurmurHash2`；`Contains` 从最旧层开始扫描 | 8 层链单次查询执行 16+ 次 `MurmurHash2`；踢出循环内反复哈希单字节 |
| **桶内标量逐字节扫描** | `FindSlot` / `FindEmptySlot` / `Count`（`cuckoo_filter.cc:131-141, 248-255`）逐字节循环 | 无法利用 SWAR/SIMD 并行字节比较 |
| **`CF.LOADCHUNK` 异常删 key 未复制 & Header 校验缺口** | `cf_commands.cc:576,580,585` 删 key 未复制；`CfDeserializeHeader` 未校验 `totalDeleted <= totalItems` | 主从分歧；畸形 LOADCHUNK 直到 RDB 重载才暴露 |

---

### 4. Count-Min Sketch（`cms_sketch.*` / `cms_rdb.*` / `cms_commands.cc`）

| 问题类别 | 现状与位置 | 影响 |
|---|---|---|
| **`CMS.MERGE` `numKeys` 整型截断致 32 GiB OOM 崩溃（高危）** | `cms_commands.cc:181`：`int weightsStart = sourcesStart + static_cast<int>(numKeys);`，若传入 `numKeys = 4294967296`（$2^{32}$），截断为 `0` 绕过 `weightsStart > argc`，随后 `std::vector<double> weights(numKeys)` 尝试分配 32 GiB 触发 abort | 单条恶意命令可直接打挂 Redis 进程 |
| **`CmsSketch::Merge` 自别名数据清空（高危 Bug）** | `cms_sketch.cc:123` 先 `std::memset(counters_, 0, ...)` 再遍历 `sources` | `CMS.MERGE dst 2 dst src2` 会把 `dst` 自身计数先清零再累加，结果完全错误 |
| **`CMS.MERGE` 未校验 `NaN`/`Inf`/负权重与溢出 UB** | `cms_commands.cc:196`、`cms_sketch.cc:129-138` 接受 `nan`/`inf`/超大权重 | `static_cast<uint32_t>(NaN)` 与 `static_cast<uint64_t>(weightedTotal)` 触发未定义行为（UB） |
| **`CmsSketch::IncrBy` 有符号溢出 UB + `CMS.INCRBY` 非原子主从分歧** | `cms_sketch.cc:88` `counter + INT64_MAX` 与 `:102` `-INT64_MIN` 均为有符号溢出 UB；`cms_commands.cc:127-135` 逐项应用，后项溢出返回错误但前项已修改且未复制 | UBSan 崩溃；多 item `CMS.INCRBY` 失败时主从数据不一致 |
| **双倍哈希计算与每行硬件除法** | `IncrBy` Pass 1 与 Pass 2 对同一 item 重复哈希 `2 * depth_` 次；`ColumnOf` 每行执行 `% width_` 除法 | 写吞吐减半，每行受限于除法延迟 |
| **`CMS.MERGE` 缺少动态 KeySpecs** | `cms_commands.cc:302` 注册为 `(1, 1, 1)` | Cluster 路由校验与 ACL 权限检查遗漏所有 `src` key |

---

### 5. Top-K（`topk_sketch.*` / `topk_rdb.*` / `topk_commands.cc`）

| 问题类别 | 现状与位置 | 影响 |
|---|---|---|
| **`ApplyOneIncrement` 热路径每次冲突调 `std::pow`** | `topk_sketch.cc:171`：`std::pow(decay_, cell.count)` | 每次指纹冲突耗时 ~50–100 cycles，严重拖慢 HeavyKeeper 吞吐 |
| **`IncrBy(item, increment)` 暴力循环 `increment` 次（最高 100,000 次）** | `topk_sketch.cc:231-238` 循环调用 `Add(item)`，每次重算 `depth+1` 次 MurmurHash2 + $O(k)$ 线性扫描 `FindEntryIndex` | 单条 `TOPK.INCRBY k item 100000` 在大 `k` 下可卡顿主线程数十至数百毫秒 |
| **`FindEntryIndex` 为 $O(k)$ 线性 `memcmp` 扫描** | `topk_sketch.cc:123-131`，`k` 最大可达 $2^{20}$ | 大 `k` 下 `TOPK.ADD`/`QUERY`/`COUNT` 退化严重 |
| **OOM 空指针崩溃 & 超长 item 致 RDB 重载失败 & 计数器回绕** | `SetEntryName`（`topk_sketch.cc:143-144`）未检查 `RMAlloc` 返回值；`CmdAdd`/`CmdIncrby` 未拦截 `len > kTopkMaxNameLen`（1 MiB），写入后 RDB 重载直接拒收；`cell.count++`（`uint32_t`）在 $2^{32}-1$ 后静默回绕为 0 | OOM SIGSEGV；重启数据丢失；高频重流计数清零 |
| **`TOPK.LIST` 全量深拷贝 + `DefragTopk`/`FreeEffortTopk2` 缺陷** | `CmdList` 调用 `sketch->List()` 构造 `std::vector<pair<string, uint64_t>>`；`DefragTopk` 未 defrag 每个 `entries_[i].name`；`FreeEffortTopk2` 恒返回 `3`（忽略 `numActive_` 次独立字符串分配） | 大 `k` 时 `TOPK.LIST` 峰值内存翻倍；`UNLINK` 阻塞主线程；碎片整理不完整 |
| **`TOPK.RESERVE` `decay = nan` 校验漏洞 & RDB 重复项缺口** | `topk_commands.cc:86` 未检查 `std::isfinite(decay)`；`ReadTopkSketch`/`TopkDeserializeChunk` 未校验 `entries_` 无重复 name | 错误信息不一致；畸形 RDB 可注入重复 top-k 条目 |

---

### 6. T-Digest（`tdigest_sketch.*` / `tdigest_rdb.*` / `tdigest_commands.cc`）

| 问题类别 | 现状与位置 | 影响 |
|---|---|---|
| **使用平权分桶而非 Dunning-Ertl $k_1$ 缩放函数** | `tdigest_sketch.cc:172-186` 使用固定 `targetWeight = totalWeight / compression_` | p99 / p99.9 / p99.99 尾部精度显著差于标准 T-Digest |
| **每次查询堆分配 `std::vector<Anchor>` + $O(C)$ 线性扫描** | `BuildAnchors()` + `ValueAtRank()`（`tdigest_sketch.cc:260-291`）在每次 `Quantile`/`ByRank`/`ByRevRank` 调用时重建并线性扫描 | 多分位查询（如一次查 10 个分位数）重复分配和扫描 10 次 |
| **全部查询命令被注册为 `"write deny-oom"`** | `tdigest_commands.cc:520-526` 将 `QUANTILE`/`CDF`/`RANK`/`REVRANK`/`BYRANK`/`BYREVRANK`/`TRIMMED_MEAN` 标记为 `"write"` | **只读副本（Replica）无法执行任何 T-Digest 分位查询** |
| **`TDIGEST.MERGE` `numKeys` 截断 OOM 崩溃 & `OVERRIDE` 自别名清空（高危）** | `tdigest_commands.cc:372` 同 `CMS.MERGE` 的 `int32` 截断致 32 GiB OOM 崩溃；`:429` 在 `OVERRIDE` 时先 `dest->Reset()` 再 `dest->Merge(sources)`，若 `dest` 也在 `sources` 中则先清空自身 | 单条命令打挂 Redis；`TDIGEST.MERGE k 2 k k2 OVERRIDE` 丢失 `k` 原有数据 |
| **`TDIGEST.ADD` 非原子性（`NaN`/`Inf` 致主从分歧）** | `tdigest_commands.cc:94-105` 未在写入前校验 `std::isfinite`，遇 `nan`/`inf` 中途失败但前序值已写入且未复制 | 主从数据不一致 |
| **数值/RDB 边界缺陷与内存统计偏差** | `ValueAtRank`（`:286`）相邻 anchor rank 相等时 `0/0 = NaN`；`Rank`/`RevRank`（`:328,336`）对超大 `TotalWeight()` 强转 `int64_t` UB；`ReadTdigestSketch` 未校验 centroids 有序性及权重和一致性；`TdigestMemUsage` 用 `NumCentroids()` 而非 `CentroidCapacity()`；`TDIGEST.MERGE` 缺动态 KeySpecs | 极端分布下返回 NaN / UB；畸形 RDB 破坏排序不变量；`MEMORY USAGE` 低估 |

---

### 7. 测试与基准缺口

- 缺少统一的 `bloom_bench` 微基准套件，无法量化 5 个子模块的写入/查询吞吐、内存占用、尾部精度与 RDB 吞吐。
- 缺少针对上述安全/UB 边界（`numKeys` 截断、自别名 Merge、`NaN`/`Inf` 参数与权重、超长 Top-K 名称、`int64_t` 溢出、UBSan 对齐/空指针、畸形/截断 RDB 模糊测试、Redis 7 KeySpecs `COMMAND INFO` / Cluster 校验）的 GTest 与 Tcl 覆盖。

---

## 二、分 Phase 深度优化与加固计划

> 规划总览：共 **5 个 Phase**，遵循「**基准建立 + 安全止血 + Redis 7 收敛 → Bloom/Cuckoo 吞吐与内存优化 → CMS/Top-K 算法加速与结构重构 → T-Digest 精度/查询/只读副本升级 → RDB 零拷贝/大对象分片与全链路模糊压测闭环**」的渐进路线。  
> 每个 Phase 完成后均执行完整验证矩阵（**GTest + ASan/UBSan + `bloom_bench` + Docker `gemini-module:redis7`（Redis 7.2.11）5 套 Tcl + compat/migrate 回归**），验证通过后按 Phase 提交并推送到 `origin/main`。

| Phase | 核心主题 | 关键交付与预期收益 |
|---|---|---|
| **Phase 1** | **基准体系建立 + 高危安全/UB 止血 + Redis 7 收敛** | 新增 `bloom_bench` 基线；修复 `CMS/TDIGEST.MERGE` 截断 OOM 崩溃、CF 255 GiB OOM 缺口与非 2 次幂扩容 bug、自别名 Merge、`NaN`/`Inf` 绕过与非原子写入、有符号溢出/对齐/空指针 UB、`LOADCHUNK` 删 key 复制分歧；清理 Redis 5/6 兼容代码，为 `CMS.MERGE`/`TDIGEST.MERGE` 注册 Redis 7 动态 KeySpecs |
| **Phase 2** | **Bloom & Cuckoo Filter 极致吞吐与内存优化** | `MurmurHash2/64A` 整字读取提速（100% 位等价）；Bloom 128-bit 精确 Fastmod 消除 `% totalBits` 硬件除法（100% 保持 `MBbloom--` RDB 互通）+ 消除 `Put` 顶层双探测 + `MADD`/`MEXISTS` 软件预取；Cuckoo 链单次哈希 + `CfAltIndex` 256B `constexpr` LUT + 栈上踢出历史零堆分配 + SWAR/SIMD 桶匹配 |
| **Phase 3** | **Count-Min Sketch & Top-K 算法加速与结构重构** | CMS 单次哈希缓存 + 32-bit Fastmod + 整数权重快路径 Merge；Top-K 预计算 `decay^c` 查表（消除 `std::pow`）+ `IncrBy` 真正批处理（大 increment 提速 100×+）+ $O(1)$ 哈希索引替代 $O(k)$ 线性扫描 + `TOPK.LIST` 零拷贝回复 + `DefragTopk`/`FreeEffortTopk2` 补全 |
| **Phase 4** | **T-Digest $k_1$ 精度升级、$O(\log C)$ 查询与只读副本支持** | 升级标准 Dunning-Ertl $k_1$（arcsin）缩放函数 + 交替扫描（p99/p99.9 误差降 1~2 个数量级）；有序归并压缩；累计权重前缀和实现 $O(\log C)$ 零分配查询；`QUANTILE`/`CDF`/`RANK` 等查询命令恢复 `"readonly"` 支持只读副本；修正 `TdigestMemUsage` |
| **Phase 5** | **RDB 零拷贝加载、大对象分片与全链路模糊/压测闭环** | 4 个子模块 RDB 加载零拷贝接管 buffer（峰值内存 2× → 1×）；补全 T-Digest 有序性/权重和与 Top-K 唯一性 RDB 校验；大对象 AOF 重写防超 `proto-max-bulk-len` 分片；全模块升级 Redis 7 `mem_usage2`；新增差分/模糊测试套件与最终 `bloom_bench` 对比报告、Docker Redis 7.2 全量 soak/compat/migrate 闭环 |

---

## 三、各 Phase 详细实施方案与验证矩阵

### Phase 1：基准体系建立 + 高危安全/UB 止血 + Redis 7 平台收敛

**改造内容**
1. **微基准套件 `modules/gemini-bloom/tests/bloom_bench.cc`**（CMake target `bloom_bench`）：
   - **Bloom**：100k / 1M 元素 `Put`、`Contains`（命中/未命中）、批量 100-item `MADD`/`MEXISTS`、RDB Save/Load 吞吐与内存占用。
   - **Cuckoo**：不同负载因子（50% / 85% / 触发扩容）下 `Add`/`Contains`/`Delete`/`Count` 吞吐与内存占用。
   - **CMS**：`IncrBy`、`Query`、`Merge`（4 路合并）吞吐。
   - **Top-K**：小 `k`（10）与大 `k`（1000）下 `Add`、`IncrBy(inc=1000)`、`Query`、`Count`、`List` 吞吐。
   - **T-Digest**：100k 观测值 `Add`、`Compress`、`Quantile`（单次/10 分位）、`Cdf`/`Rank` 吞吐，以及正态/长尾分布下 p50/p90/p99/p99.9 误差报告。
2. **高危 DoS / OOM / 崩溃修复**：
   - `cms_commands.cc` / `tdigest_commands.cc`：`numKeys` 校验 `numKeys <= 0 || numKeys > argc - 3` 后再转 `int`，杜绝 `int32` 截断导致 32 GiB 分配崩溃；命令内临时数组优先栈缓冲（`<= 64` 项）+ `RMAlloc` 安全回退，消除 `-fno-exceptions` 下 `std::vector` OOM abort。
   - `cuckoo_filter.h/cc` / `cf_chain.cc` / `cf_rdb.cc`：引入 `kCfMaxLayerDataSize = 1 GiB`、`kCfMaxTotalDataSize = 2 GiB`，拦截 `255 GiB` OOM；修复 `CuckooChain::Add` 中 `expansion_ > 1` 非 2 的幂时 `nextBuckets` 非 2 的幂导致扩容必然失败的问题（对 `nextBuckets` 做 `std::bit_ceil` 并检查溢出）；`BucketsForCapacity` 防止 `capacity + bucketSize - 1` 溢出。
   - `topk_sketch.cc` / `topk_commands.cc`：`SetEntryName` 返回 `bool` 检查 `RMAlloc` 失败；`CmdAdd`/`CmdIncrby` 拒绝 `len > kTopkMaxNameLen`；`ApplyOneIncrement` 对 `cell.count` 饱和至 `UINT32_MAX`。
3. **正确性、UB 与主从一致性修复**：
   - `cms_sketch.cc` / `cms_commands.cc`：`IncrBy` 使用无符号/安全边界判断消除 `int64_t` 溢出与 `-INT64_MIN` UB；`CmdIncrby` 先全部校验再提交（或失败回滚），保证原子性与主从一致；`Merge` 写入临时缓冲再交换修复自别名清空；`CmdMerge` 校验 `std::isfinite(w) && w >= 0.0` 并对 `totalCount_` 做饱和保护。
   - `tdigest_sketch.cc` / `tdigest_commands.cc`：`CmdAdd` 先校验全部 `std::isfinite(v)` 再写入；`CmdMerge` 在 `OVERRIDE` 且 `dest` 也在 `sources` 中时先收集/合并再替换（不提前 `Reset()`）；`ValueAtRank` 处理 `b.rank <= a.rank` 防除零 `NaN`；`Rank`/`RevRank` 对 `TotalWeight()` 做 `INT64_MAX` 饱和。
   - `bloom_config.cc` / `bloom_commands.cc` / `topk_commands.cc`：所有浮点数范围判断补齐 `std::isfinite(...)`。
   - `bloom_commands.cc` / `cf_commands.cc`：`LOADCHUNK` 校验失败执行 `RedisModule_DeleteKey(key)` 时同步 `RedisModule_Replicate(ctx, "DEL", "s", argv[1])`。
   - 全模块 RDB/Wire 序列化消除 `memcpy(..., nullptr, 0)`（加 `if (bytes > 0)` 守卫）与非对齐指针强转（统一用 `std::memcpy` 读写 wire header/entry）。
4. **Redis 7 平台收敛**：
   - 清理 `redis_bloom_module.cc` / `bloom_rdb.cc` 中 Redis 5/6 空函数指针判断。
   - 为 `CMS.MERGE` 和 `TDIGEST.MERGE` 使用 `RedisModule_SetCommandInfo` 注册 `REDISMODULE_CMD_KEY_BEGIN_SEARCH_INDEX` + `REDISMODULE_CMD_KEY_FIND_KEYNUM` 动态 KeySpecs（`dest` 为 RW+UPDATE，`sources` 为 RO+ACCESS）。
   - 更新 `CLAUDE.md` 与 `modules/gemini-bloom/DESIGN.md` 目标版本为仅 Redis 7。

**测试与验证**
- **GTest + ASan + UBSan**：`CMS.MERGE`/`TDIGEST.MERGE` 自别名、`NaN`/`Inf` 权重与输入、`INT64_MAX`/`INT64_MIN` delta、CF 超大内存拒绝与非 2 次幂 `expansion=3` 扩容、Top-K 超长名拒收与计数饱和、空 T-Digest `LOADCHUNK` 序列化零 UBSan 告警。
- **`bloom_bench`**：采集并记录 Phase 1 优化前基线数据表。
- **Docker `gemini-module:redis7` Tcl + Compat + Migrate**：5 套 Tcl 新增上述边界/主从一致/KeySpecs（`COMMAND GETKEYS CMS.MERGE ...`、`COMMAND GETKEYS TDIGEST.MERGE ...`）用例；运行 `bloom_compat_test.py` 与 `bloom_migrate_test.py`。

---

### Phase 2：Bloom Filter & Cuckoo Filter 极致吞吐与内存优化

**改造内容**
1. **公共哈希优化（`murmur2.h/cc`）**：
   - `MurmurHash2` / `MurmurHash64A` 参数改 `size_t len`；在小端平台使用 `std::memcpy(&k, data, 4/8)` 整字加载（编译器生成单条 `mov`/`ldr` 指令），保持与逐字节版本 100% 逐位一致。
2. **Bloom Filter 提速（`bloom_filter.h/cc`、`sb_chain.h/cc`、`bloom_commands.cc`）**：
   - **128-bit 精确 Fastmod**：在 `BloomLayer` 中预计算 `totalBits_` 的快速取模结构（`__uint128_t` 乘法高 64 位近似商 + 至多 1 次修正减法，精确等价于 `raw % totalBits_`），`Test`/`Insert` 彻底消除 64 位除法指令，同时 **100% 保持 `MBbloom--` RDB 比特级兼容**。
   - **消除 `ScalingBloomFilter::Put` 顶层双探测**：当顶层未满时，先查旧层 `[0 .. numLayers_-2]`，若未命中则直接对顶层执行一次 `Insert(hp)`（返回值即指示是否新插入），减少 1 次完整顶层探测。
   - **批量软件预取**：新增 `ScalingBloomFilter::PutBatch` / `ContainsBatch`（供 `BF.MADD`/`BF.MEXISTS` 调用），对每批 8 个 item 先集中计算 `HashPair` 并 `__builtin_prefetch` 首探针字节地址再执行探测。
3. **Cuckoo Filter 提速（`cuckoo_filter.h/cc`、`cf_chain.h/cc`）**：
   - **链级单次哈希 + 倒序查找**：提取 `CfItemHash { uint8_t fp; uint32_t rawIndex; }`，`CuckooChain::Contains`/`Delete`/`Count`/`Add` 仅对 `item` 哈希一次后传入各层；`Contains` 从最新（最大）层向最旧层遍历。
   - **`CfAltIndex` 编译期 256 项 LUT**：`constexpr std::array<uint32_t, 256>` 预存 `MurmurHash2(&fp, 1, kCfAltSeed)` 的精确结果，运行时一次查表 + XOR 完成，与现有 RDB 100% 兼容。
   - **栈上踢出历史缓冲**：`KickEntry` 收紧为 16B（或 12B packed），优先使用栈上 `KickEntry stackHistory[64]`，仅当实际踢出次数 > 64 时才按需 `RMAlloc` 扩容，热路径零堆分配。
   - **SWAR / SIMD 桶内并行匹配**：对 `bucketSize == 2/4/8` 采用 64 位字级 SWAR 字节匹配（`haszero`/`hasbyte`），对 `bucketSize >= 16` 采用 SSE2 / ARM NEON 16 字节并行比较，同步加速 `FindSlot`、`FindEmptySlot`、`Count` 与 `RecountItems`。

**测试与验证**
- **GTest + 差分测试 + ASan/UBSan**：
  - `MurmurHash2/64A` 优化版 vs 参考逐字节版 10 万随机长度/对齐/二进制输入逐位一致。
  - `BloomLayer` Fastmod vs 原生 `%` 100 万随机 `(raw, totalBits)` 差分 100% 一致。
  - `CfAltIndex` LUT vs `MurmurHash2` 全部 256 个 `fp` 逐一验证 100% 一致；SWAR/SIMD `FindSlot`/`Count`/`RecountItems` 覆盖 `bucketSize = 1, 2, 3, 4, 7, 8, 15, 16, 32, 64, 255` 边界。
- **`bloom_bench`**：对比 Phase 1 基线，验证 Bloom `Put`/`Contains`/`MADD` 与 Cuckoo `Add`/`Contains`/`Delete` 吞吐显著提升。
- **Docker `gemini-module:redis7`**：全量 Tcl + `bloom_compat_test.py` + `bloom_migrate_test.py`（重点验证 `MBbloom--` 双向 RDB 兼容 0 回归）。

---

### Phase 3：Count-Min Sketch & Top-K 算法加速与结构重构

**改造内容**
1. **Count-Min Sketch 优化（`cms_sketch.h/cc`、`cms_commands.cc`）**：
   - `IncrBy` Pass 1 将 `depth_` 个列号缓存到栈数组 `uint32_t cols[kCmsMaxDepth]`，Pass 2 直接复用，哈希次数减半。
   - `CmsSketch` 预计算 `width_` 的 32-bit Fastmod 乘数（`uint64_t` 乘法），消除 `ColumnOf` 每行 `% width_` 除法。
   - `Merge` 增加非负整数权重快路径（纯 `uint64_t` 累加 + 饱和，避免逐元素 `uint32 -> double -> uint32` 转换），并编写利于编译器自动向量化的内层循环。
2. **Top-K 深度重构（`topk_sketch.h/cc`、`topk_rdb.cc`、`topk_commands.cc`）**：
   - **预计算 `decay^c` 查表**：在 `TopkSketch::Create` 时预计算 `decayTable_[256]`（及精确截断阈值 `maxDecayCount_`，当 `decay^c < 2^-53` 时直接跳过随机数与 `pow`），彻底消除热路径 `std::pow`。
   - **批量 `IncrBy` 真批处理**：一次计算 `fp` 和 `cols[0..depth_-1]`；对指纹已匹配行直接累加（带 `UINT32_MAX` 饱和）；对冲突行高效推进衰减；批量结束后仅调用一次 `UpdateTopK` 更新堆/索引，避免 100,000 次重复哈希与 `FindEntryIndex`。
   - **$O(1)$ 开放寻址哈希索引**：在 `TopkSketch` 中维护与 `entries_` 联动的紧凑哈希表（2 的幂容量、线性探测、缓存 64-bit `nameHash`），将 `FindEntryIndex` / `Query` / `Count` / `Add` 从 $O(k)$ 降为均摊 $O(1)$；同步更新 `TopkMemUsage` 与 `DefragTopk`。
   - **`TOPK.LIST` 零拷贝流式回复**：`CmdList` 直接读取 `sketch->GetEntries()[0..numActive_-1]` 回复，消除 `sketch->List()` 的 `std::vector<pair<string, uint64_t>>` 全量拷贝。
   - **完善 Redis 7 回调**：`DefragTopk` 补齐对 `entries_[i].name` 与哈希表的 defrag；`FreeEffortTopk2` 改为返回 `3 + sketch->NumActive()`，确保大 `k` 时 `UNLINK` 正确走异步 lazy-free。

**测试与验证**
- **GTest + 差分/压力测试 + ASan/UBSan**：
  - CMS Fastmod vs `%` 全范围差分；整数权重 vs 浮点权重 Merge 一致性测试。
  - Top-K `decayTable_` vs `std::pow` 误差测试；`IncrBy(100000)` 批量与单次行为一致性及重流识别准确率测试；$O(1)$ 哈希索引在频繁插入/升级/淘汰/扩容/`Clone`/RDB 重载下的不变量校验（`k = 1, 10, 1000, 10000`）。
- **`bloom_bench`**：对比 CMS `IncrBy`/`Query`/`Merge` 与 Top-K（尤其大 `k` 与大 `increment`）吞吐提升。
- **Docker `gemini-module:redis7`**：`cms_test.tcl`、`topk_test.tcl`（新增大 `k`、大 `increment`、`UNLINK`、`MEMORY USAGE`、`COPY`、RDB/AOF 测试）。

---

### Phase 4：T-Digest $k_1$ 精度升级、$O(\log C)$ 查询与只读副本支持

**改造内容**
1. **标准 Dunning-Ertl $k_1$（arcsin）缩放函数与增量归并（`tdigest_sketch.h/cc`）**：
   - 将 `RebuildFromPoints` 的平权分桶升级为标准 $k_1(q) = \frac{\delta}{2\pi}\arcsin(2q - 1)$ 缩放函数（允许每簇最大权重受 $\Delta k_1 \le 1$ 约束），并交替使用升序/降序合并方向消除长期流式写入的单边漂移。
   - `Compress()` 利用 `centroids_` 已有序的特性：仅对 `buffer_` 排序，随后与 `centroids_` 双指针归并并直接流式分簇，复用内部 scratch 缓冲区（`RMAlloc` 管理，无 `-fno-exceptions` 风险），替代每次 `Compress()` 分配两个 `std::vector` 并全量 `std::sort`。
2. **$O(\log C)$ 零分配查询**：
   - 维护与 `centroids_` 同步的累计权重前缀数组 `prefixWeights_`（长度 `numCentroids_`），在 `Compress()` / `Merge()` / RDB Load 后一次性构建。
   - `Quantile` / `ByRank` / `ByRevRank` / `Cdf` / `Rank` / `RevRank` 全部改为基于 `prefixWeights_` 和 `centroids_` 的 $O(\log C)$ 二分查找，**零堆分配**，彻底移除 `BuildAnchors()` 的每查询 `std::vector<Anchor>` 分配。
   - 多参数查询命令（如 `TDIGEST.QUANTILE k q1 q2 ... qN`）一次合并检查 + $N$ 次 $O(\log C)$ 二分。
3. **查询命令恢复 `"readonly"`（支持只读副本）**：
   - `TDIGEST.QUANTILE` / `CDF` / `RANK` / `REVRANK` / `BYRANK` / `BYREVRANK` / `TRIMMED_MEAN` 改以 `"readonly"` 注册并以 `REDISMODULE_READ` 打开 key。
   - 实现只读查询安全机制：当 `numBuffered_ == 0` 直接 $O(\log C)$ 查询；当 `numBuffered_ > 0` 时，在只读副本/只读命令中对缓冲做惰性只读视图或 const-safe 压缩（保证主从复制语义与 `TDIGEST.INFO` 一致，且不触发只读副本 `READONLY` 报错）。
4. **精确内存统计与 defrag**：
   - `TdigestMemUsage` 按 `CentroidCapacity()` + `BufferCapacity()` + `prefixWeights_` 精确计算；`DefragTdigest` 同步 defrag 新增的 `prefixWeights_` 缓冲区。

**测试与验证**
- **GTest + 精度基准 + ASan/UBSan**：
  - 均匀分布、正态分布、指数/帕累托长尾分布、双峰分布（100k 样本）下 p50/p90/p99/p99.9 误差对比（验证尾部精度大幅优于旧平权实现）。
  - $O(\log C)$ 二分查询 vs 朴素线性扫描差分测试（覆盖单 centroid、重复值、极值边界、rank 相等边界）。
  - 大规模连续 `Add` + `Merge` + `Reset` + `Clone` 内存安全与不变量测试。
- **`bloom_bench`**：对比 T-Digest `Add`/`Compress`/`Quantile`(单分位 & 10 分位)/`Cdf`/`Rank` 吞吐与精度指标。
- **Docker `gemini-module:redis7`**：`tdigest_test.tcl`（新增**只读副本（`REPLICAOF`）上执行 `TDIGEST.QUANTILE`/`CDF`/`RANK`/`BYRANK`/`TRIMMED_MEAN`** 集成测试）。

---

### Phase 5：RDB 零拷贝加载、大对象分片与全链路模糊/压测闭环

**改造内容**
1. **RDB 零拷贝接管与完整性校验加固（`*_rdb.cc`）**：
   - 封装 `AdoptLoadedBlob`：在 Redis 运行时直接接管 `RedisModule_LoadStringBuffer` 返回的缓冲区（消除 `Bloom`/`Cuckoo`/`CMS`/`TopK` 加载时的 2× 内存峰值与整块 `memcpy`）；测试桩下保持等价分配语义。
   - `tdigest_rdb.cc`：加载 RDB / `LOADCHUNK` 时严格校验 `centroids_` 按 `mean` 非递减有序、校验 `sum(centroid.weight)` 与 `mergedWeight` 及 `sum(buffer.weight)` 与 `unmergedWeight` 在浮点容差内一致、校验 `min_ <= centroids_[0].mean` 且 `centroids_[last].mean <= max_`。
   - `topk_rdb.cc`：加载 RDB / `LOADCHUNK` 时校验 `entries_` 无重复名称、严格非递增顺序，并重建 $O(1)$ 哈希索引。
   - 全模块 `RedisModuleTypeMethods` 升级为 Redis 7 `mem_usage2` 回调。
2. **大对象 AOF 重写防超限**：
   - `CMS` / `TopK` / `T-Digest` 的 `AofRewrite*` 与私有 `*.LOADCHUNK` 支持大对象分片（如单块 > 32 MiB 时自动分片写出并顺序加载，类似 `BF/CF.LOADCHUNK` 的分块机制，兼容小对象单块格式），确保接近 1 GiB 的大 Sketch 在 AOF 重写与重启加载时不触发 `proto-max-bulk-len` 限制。
3. **模糊测试与全链路压测闭环**：
   - 新增 `modules/gemini-bloom/tests/bloom_fuzz_test.cc`（纳入 `bloom_test`）：对 5 个子模块的 RDB 反序列化与 Wire `LOADCHUNK` 执行数千组随机截断、比特翻转、长度篡改、非法浮点（`NaN`/`±Inf`/subnormal）、超大计数攻击，在 ASan + UBSan 下要求 100% 优雅返回 `nullptr`、零泄漏、零崩溃。
   - 在 Docker `gemini-module:redis7`（Redis 7.2.11）中运行全量 CI：
     - `bloom_test`（普通 + ASan + UBSan）
     - 5 套 Tcl 集成测试（`bloom_test.tcl`、`cf_test.tcl`、`cms_test.tcl`、`topk_test.tcl`、`tdigest_test.tcl`）
     - `ci/bloom_compat_test.py`（RESP3 + RESP2）
     - `ci/bloom_migrate_test.py`（RedisBloom 2.6.25 ↔ GeminiBloom 双向，RESP3 + RESP2）
     - `ci/bloom_soak_test.py`
   - 输出最终 `bloom_bench` Phase 1 基线 vs Phase 5 最终性能/内存/精度对比报告并归档至 `doc/gemini-bloom/`。

---

## 四、优化前后实测对比（`bloom_bench`）

| 指标 | 优化前基线 | Phase 5 最终实测 | 提升幅度 |
|---|---:|---:|---:|
| `MurmurHash2` (1M keys) | 246.21 M ops/s | 276.01 M ops/s | **+12.1%** |
| `MurmurHash64A` (1M keys) | 365.78 M ops/s | 409.11 M ops/s | **+11.8%** |
| `Bloom Put 500k` (single layer, NoRound) | 27.68 M ops/s | 30.27 M ops/s | **+9.4%** |
| `Cuckoo Add 200k` | 56.54 M ops/s | 107.16 M ops/s | **1.90×** |
| `Cuckoo Contains 200k` (hit) | 52.77 M ops/s | 126.52 M ops/s | **2.40×** |
| `Cuckoo Delete 100k` | 66.78 M ops/s | 137.26 M ops/s | **2.06×** |
| `CMS IncrBy 300k` | 34.11 M ops/s | 55.35 M ops/s | **1.62×** |
| `CMS Merge 100x` (65536×7) | 3,277 merges/s | 6,723 merges/s | **2.05×** |
| `Top-K Add 100k` (k=20) | 15.24 M ops/s | 21.08 M ops/s | **1.38×** |
| `Top-K IncrBy 100×1000` (k=20) | 28.06 K calls/s | 10,435 K calls/s | **372×** |
| `Top-K Add 100k` (k=1000) | 2.87 M ops/s | 13.57 M ops/s | **4.73×** |
| `Top-K Query 100k` (k=1000) | 2.32 M ops/s | 144.85 M ops/s | **62.4×** |
| `T-Digest Quantile 100k` | 4.00 M ops/s | 166.62 M ops/s | **41.7×** |
| `T-Digest CDF 100k` | 19.28 M ops/s | 38.55 M ops/s | **2.00×** |
| `T-Digest p99.00` 相对误差 (Exp(1)) | 2.19% | 0.02% | **误差降 109×** |
| `T-Digest p99.90` 相对误差 (Exp(1)) | 79.87% | 0.85% | **误差降 94×** |
| `T-Digest p99.99` 相对误差 (Exp(1)) | 32.31% | 4.42% | **误差降 7.3×** |

