# 性能优化任务清单

> 来源：2026-06-22 全代码库性能分析（四个子系统并行审计：HNSW/向量、存储核心、全文检索、构建/并发）。
> 背景：代码库已高度优化（运行时 SIMD 派发、LTO 已开、内存序精确、256-shard、零拷贝）。以下是经代码核实、仍剩余的高 ROI 机会。
> 相关文档：`doc/hnsw-memory-footprint-zh.md`、`doc/hnsw-graph-construction-zh.md`。

> ⚠️ **基准测试警告**：当前 `build/` 目录是 Debug 配置（`-g`，无 `-O`）。跑 benchmark 前务必用独立的 Release build 目录（`-O3 -DNDEBUG` + LTO），否则数字无意义。

## 第一梯队：低成本 / 高收益（优先）

> ✅ 2026-06-22 全部完成并验证：Release 构建（`build-rel/`）编译通过，`ctest` 429/429 全绿（含 HNSW 召回 Dim384/Ef64/Ef256、L2MetricBasic、Int8Only 往返、inverted 全套、keydir/cask 全套）。
>
> 📊 **性能 A/B 实测**（Release，`taskset -c 0-3`，7×median，CV 1~2.6%）：
> - **①** `BM_Hnsw_Search` 端到端查询延迟 **−7~9%**（10k/100k × ef64/256 一致；修正了"少算 40 倍"的注释）。注：低于先前 15~30% 估计——图遍历是大头，rerank 节省被摊薄。
> - **②** `BM_Inverted_SearchHotTerm`：4096 **−5.1%**、100k −2.2%、512 噪声内。符合预期（libstdc++ 小数组本就自适应插入排序）。
> - **③④** 不在单线程延迟基准内：③ 是确定性内存节省（dim 字节/节点，仅 kL2/非 VNNI）；④ 需 merge+active 并发写不同文件才显现。

- [x] **① HNSW rerank 阶段重复计算 f32 距离** — `src/vector/hnsw.cpp:885-892`
  - 现状：`partial_sort` 比较器每次算 2 次全维 SIMD 距离，之后再算第 3 次；ef=128/k=10 时实际 ~1250 次，只需 ~128 次。注释「固定开销 ~30 次」少算约 40 倍。
  - 改法：int8 粗筛得候选后，一次性把 f32 距离写进 `.first`，再对缓存浮点值排序。
  - 收益：默认 int8 查询路径延迟 **−15~30%**；改动小、风险低。**建议第一个做。**

- [x] **② BlockMax-WAND 每轮迭代全量重排 term 数组** — `src/bm25/inverted.cpp:552-564`
  - 现状：`while` 主循环内每轮 `std::sort(order...)`，O(iters·t log t)，每次比较两次间接 `ords[cursor]` 加载。
  - 改法：照搬同文件 `bool_search` must-only BMW 路径（~1072 行）的「预排一次 + leapfrog」写法，或只 `partial_sort` 到 pivot。
  - 收益：disjunctive top-k 热路径最大冗余，延迟显著下降；改动小。

- [x] **③ `qcodes` 量化副本永远分配（即使用不上）** — `src/vector/hnsw.cpp:301`
  - 现状：非 VNNI 机器或 `kL2` 度量下，`qcodes`/`qscales`/`qsums` 从不读写却无条件分配（dim=384 时每 chunk 浪费 24MB）。
  - 改法：仿 `needs_vecs`，加 `needs_qcodes = inmem_int8 || (int8_dot_ && metric==kDot)`，false 时零容量。注意 deserialize 路径须按 config 而非 needs_vecs 判断。
  - 收益：上述配置内存 **−20~25%**。详见 `doc/hnsw-memory-footprint-zh.md` §6。

- [x] **④ `AtomicFStats` 缺 cache-line 对齐（假共享）** — `include/bitcask/keydir.hpp:433`
  - 现状：8 个 atomic（~52B）放在 `std::deque<AtomicFStats>`，两个不同 file_id 可能共享一条 64B line；merge + active 并发写时假共享。
  - 改法：`struct alignas(64) AtomicFStats`，一行。
  - 收益：消除跨文件假共享；KeyDir 全局 atomics 已 padding，唯此处漏。

## 第二梯队：中等成本 / 高收益（结构性）

- [x] **⑤ KeyDir 用 node-based `std::unordered_map<std::string, Entry>`** — `include/bitcask/keydir.hpp:359`
  - 现状：每新 key 一次堆分配（`keydir.cpp:496` 的 `std::string(key)`）；每次 get/put 指针追逐随机节点 → 必然 cache miss。
  - 改法：换开放寻址扁平表（`absl::flat_hash_map` / `boost::unordered_flat_map`）+ 内联 key。`IterHandle` 已按拷贝快照 key（`keydir.hpp:208`），rehash-immune，扁平表安全。
  - 收益：存储侧最高杠杆，触及每次 get/put；同时消掉 #⑩ 的恢复期 key 分配。
  - 🔶 **2026-06-22 SSO 测量 → 决策待定**：libstdc++ SSO 阈值=15B。代码库全部基准/测试用 ≤8B 短 key（`"k0"`/`"key123"`），即**围绕短 key 设计**。结论：
    - 真实 key ≤15B → 已 SSO 内联无堆分配 → ⑤ 的「消除每键分配」收益=0，只剩指针 cache-miss（较小一半）。
    - 真实 key >15B（UUID/路径/复合）→ 每键一次 malloc → ⑤ 收益显著。
    - **缺失输入**：生产真实 key 典型长度（用户答「混合/不确定」）。
    - ✅ **已加诊断探针** `KeyDir::key_length_histogram()`（`keydir.hpp` / `keydir.cpp`，零热路径开销，只读已存 key）：在真实负载调用读出 `sso`/`heap` 占比 + 8 桶分布。
    - ✅ **已实现（用 `ankerl::unordered_dense`，非 boost/abseil）**：单头 submodule `third_party/unordered_dense`（v4.8.1，MIT，INTERFACE 库）→ CMake `add_subdirectory` + `bitcask_keydir` PUBLIC 链接传播 include。`Shard::entries` 改为 `ankerl::unordered_dense::map<std::string, Entry, StringHash, std::equal_to<>>`（透明 hash 异构查找不变）。`entries_entry` 裸指针经审计仅锁内、获取与使用间无 insert/erase → 安全（plain `map`，非 `segmented_map`）。
    - ✅ **验证**：429/429 测试通过；**TSan**（`-DBITCASK_SANITIZE=thread`，含插桩 oneTBB）keydir 并发测试 7/7 无 race；零 API 改动（unordered_dense 兼容 std API）。
    - 📊 **keydir_bench A/B（关键：收益随工作集放大）**：
      - 小数据集（1024 key，全在 cache）：纯查找 ~持平（甚至 +1~4%，dense 多一跳 index→value），Mixed −7%。
      - **大数据集（1M key，出 cache）**：**Get_Single −31%**、Get_MT/4 −17%、Mixed_MT/4 −23%。出 cache 后 std 指针追逐=DRAM miss，dense 连续布局完胜。
      - 结论：**读重 + 大 keydir 场景收益显著**（−31% 纯查找）；小数据集常驻 cache 时收益主要在插入半边。

- [x] **⑥ HNSW 邻接表是 per-node `vector<vector<uint32_t>>`** — `include/bitcask/hnsw.hpp:163`、`src/vector/hnsw.cpp:713`
  - 现状：每节点一次独立 malloc（L0-only 节点 132B 数据背 ~40B 开销 ≈30%）；内层块散落堆上，拖累 `copy_neighbors` 预取局部性。
  - 改法（已实现）：per-chunk **bump-slab arena**（`kAdjSlabWords=256K u32/slab`）。`adj` 从 `vector<vector>` 改为 `vector<uint32*>`（8B/节点指针）+ 永不搬迁的固定 slab；`alloc_adj()` 顺序 bump 分配（分配序=插入序 → 邻接块连续）。保持拼接布局（`layer_off` 不变）+ 地址稳定不变量。serialize/deserialize 同步改 `.data()`→指针、`.resize()`→`alloc_adj()`。
  - ✅ **实测验证（tier1 → tier1+⑥，`BM_Hnsw_Search` median）**：10k/ef64 **−7.3%**、10k/ef256 **−8.5%**、100k/ef64 −2.2%、100k/ef256 **−5.5%**。局部性收益证实。叠加 ①后 HNSW 查询累计 **−11~15%** vs 原始 baseline。消除 ~93.75% 每节点 malloc（插入/碎片有利，未单独量化）。429/429 测试通过（含并发读者 + checkpoint 往返）。

## 第三梯队：小修小补（低成本、收益较小）

> 2026-06-22 本轮做了 ⑦⑭⑮⑯（安全的分配除去 / dim 修正），跳过 ⑫（FP 风险）。其余 ⑧⑨⑩⑪⑬⑰ 为中风险/策略项，待逐项确认后再做。全程 429/429 通过。

- [x] **⑦ HNSW `scratch`/`found`/`pool` 每次调用都 new → 改 `thread_local`** — `src/vector/hnsw.cpp`（仿 `t_visited`）。
  - 已实现：search/insert 的 `scratch`/`found` + insert 收缩的 `pool` 全部 `thread_local` 复用（resize/clear 保容量）。serialize 的冷路径 scratch 保留。
  - 📊 同会话 back-to-back A/B（`BM_Hnsw_Search`，dim=384）：**性能中性**（±2~3% 噪声内）。dim=384 下 malloc 仅占查询时间极小一段 → 测不出；价值在高并发/多索引时降低 allocator 压力（hygiene）。无回归。
- [~] **⑧ `search_layer` 两个 `std::priority_queue` → 复用扁平有界堆** — `src/vector/hnsw.cpp`。**暂跳过（割当 hygiene，⑦ 实测前例）**：⑦ 已证 dim=384 下割当除去性能中性（距离计算主导）。⑧ 同类但是热路径双函数（f32 + int8 twin）重写、正确性风险更大；查询路径走 search_layer_int8。除非实测确证 −5~10% 否则不值。可作「做并实测、不赚就回退」的实验项。
- [~] **⑨ AVX2 intersection 分支提取 → branchless compress** — `src/bm25/intersect.cpp:95`。**跳过（此机不可测 + 风险）**：本机走 AVX-512 路径（已用最优 `compressstoreu`），要改的 AVX2 路径仅 AVX2-only CPU 命中、**此机无法 benchmark**；且 permute+full-store 需输出缓冲 ≥3 qword slack 否则尾部 overflow。盲改不做。
- [x] **⑩ 公共 `put` 每次 new `encoded`** — `src/cask/cask.cpp:1361`。已实现 (a)：`encoded` 改 `thread_local` 复用（clear 保容量，并发 put 各线程独占）。(b) scatter-write 双拷贝消除未做（较大重构）。📊 A/B：`BM_Put_WalBatch` **−4~6%**，`Cask_Put_Overwrite` 中性。无回归。
- [~] **⑪ data 文件每条记录一次无缓冲 `pwrite`** — `src/cask/cask.cpp`、`src/fileops/data_file.cpp:148`。**❌ 按设计否决（WAL 语义下 = 丢数据）**。
  - **data 文件即 WAL**：现状每条记录立即 pwrite → 字节立刻进内核 page cache，**进程崩溃一条不丢**（重启 fold 读 data 文件即得）。⑪ 把字节攒在用户态内存 → 进程崩在 flush 前那批**从没进内核 → 直接丢失**，正是 WAL 要防的事。
  - 与持久化模型冲突（`cask.hpp:58-62`）：`o_sync`（逐条 O_SYNC durable）下 ⑪ **根本不兼容**；`sync_every_n` 组提交下 ⑪ 把丢数据窗口从「仅断电」扩大到「连进程崩溃也丢」。
  - 前提也不成立：对持久 WAL，贵的是 **fsync**（等磁盘），不是 pwrite（拷进 page cache，便宜）；而 fsync 批处理**已由 `maybe_group_commit`（`cask.cpp:946`）的组提交实现**。⑪ 是「不安全地优化便宜的一半，而贵的一半早已优化」。
  - 唯一不牺牲持久性的 syscall 批处理 = **批量 put API**（一次 encode N 条、一次 pwrite，N 条整体进内核），但前提是写入本来就成批；非 ⑪ 的「跨已确认写缓冲」。
- [~] **⑫ WAND `total_ub` 每轮重新累加所有 term 上界** — `src/bm25/inverted.cpp:601-604`。**跳过（FP 风险）**：total_ub 用于 epsilon 敏感的 admissible block-skip（注释 629-632 明确警告不能加 epsilon）。增量 FP 和与每轮新求和最终位会漂移，total_ub 偏小 → 过剩跳过 → 结果欠落。小 t 利得僅少，不值此风险。
- [~] **⑬ `select_neighbors` O(M²) 距离重算无缓存** — `src/vector/hnsw.cpp:584`（int8 版 607）。**跳过（无法缓存）**：内层是 candidate↔picked 两两距离，每对唯一、跨调用无复用，缓存不掉；只能微优化 `vec_of(pid)` 提升，收益僅微。构建期成本本就由 search_layer 主导（agent 评 secondary）。
- [x] **⑭ `serialize` 每 FOR block / positions 列表 new 一个 vector** — `src/bm25/inverted.cpp:1761`。已实现：`packed`/`ords_view` 提到块循环外复用（`for_encode_block` 内部自 clear/assign，安全）。save 路径，无回归。
- [x] **⑮ `HintFile::validate_trailer` 每 64KiB chunk 一次堆分配** — `src/fileops/hint_file.cpp:152`。已实现：`pread_into(off, span)` + 复用缓冲（容量只增）替代 `file_.read(n)`。open 路径 O(filesize/64K)→1 次分配，无回归。
- [x] **⑯ `f32 prefetch_vec` 不感知 dim** — `src/vector/hnsw.cpp:222`。已实现：传 dim，按字节守卫每条 prefetch（仿 int8 路径）。dim=384 性能中性（分支可预测），小 dim 不再越尾预取。
- [x] **⑰ 可选 `-march=native` 构建开关** — `CMakeLists.txt`。已实现 opt-in `BITCASK_NATIVE`（默认 OFF；ON 时加 `-march=native` 榨 scalar glue ~3-8%，破坏二进制可移植性）。**jemalloc 不做**（用户明确不需要）。未加 `-ffast-math`（HNSW/int8 依赖精确可复现浮点，self-test 校验 ULP）。

## 第四梯队：生产正确性问题（必做，<1 天/项）

> 来源：2026-06-22 API / 错误处理 / 可观测性 / 生命周期审计（原 perf 审计未覆盖维度，4 个并行 explore agent）。
> 这些都是**用户/数据可见**的失败模式，逻辑上与 ⑪「WAL 不能丢数据」同级。建议在任何下一波性能优化前先清掉——否则性能建立在脆弱基础上。
> 风险/收益与第一梯队反向：这里**风险=不做就生产事故**，收益=「消除静默损坏」。

- [ ] **C1 `run_merge` 失败泄露部分输出文件** — `src/merge/merger.cpp:156-168`
  - 现状：`out_hint->finalize()` 或 `out_data->sync()` 失败时直接返回错误，**不清理 `output_data_path` / `output_hint_path`**。残文件留在盘上。
  - 后果：下次 `needs_merge` 把这些**残文件当输入** → 数据损坏或 merge 失败循环。Ops 无错误提示。
  - 改法：失败分支 `std::filesystem::remove()` 两个输出路径（容许 ignore ENOENT）。
  - 风险：无（纯 cleanup）；改动 ~5 行。

- [ ] **C2 `bitcask_set_synonym_map` 静默吞加载错误** — `c_api/bitcask_c.cpp:497-506`、`include/bitcask/synonym_map.hpp:18-37`
  - 现状：**API 签名是对的**（C API 已返回 `bitcask_error_t` + 有 `fault` 出参；C++ `set_synonym_map(unique_ptr)` 接管所有权，`void` 合理）。问题在实现链路：
    - `SynonymMap::load_from_file` 返回 `void`，文件打不开 `if (!file) return;` 静默返回。
    - C API `bitcask_set_synonym_map` 不检查加载结果，**永远返回 `BITCASK_OK`**。
  - 后果：文件不存在/解析错 → 用户以为同义词生效，实际 `set_synonym_map` 装了个空 map；Ops 无法察觉。
  - 改法（**零 API 破坏**）：(1) `load_from_file` 返回 `bool`（成功/失败）；(2) C API 检查返回值，失败填 `fault` + 返 `BITCASK_ERR_IO`。共 ~10 行。
  - 风险：无（签名不变，只开始正确填充早就设计好的错误出参）。

- [ ] **C3 IndexPool worker `catch(...)` 吞所有异常 → 索引静默漂移** — `src/cask/cask.cpp:620-647`
  - 现状：`search.on_write()` / `on_delete()` 抛异常被吞，`pending_--`，worker 继续。注释「best-effort」。
  - 后果：keydir 与 search index **悄悄不一致**；get/put 正常但搜索返回陈旧/缺失结果。**Ops 无任何信号**——没日志、没计数、没告警。
  - 改法（最小）：加 `std::atomic<std::uint64_t> index_errors_` 计数；暴露到 `bitcask_status()`；可选挂一个 `on_index_error` 回调 hook。
  - 风险：低（计数器是无锁原子，热路径开销 relaxed RMW）。

- [ ] **C4 IndexPool 析构 UB（`start()` 未调用即销毁）** — `include/bitcask/thread_pool.hpp:169,230`
  - 现状：`~IndexPool()` → `stop()` → `worker_.join()`；若 `start()` 从未调用，`worker_` 默认构造，`joinable()` 返回 false 当前已 guard——需核实。**核实点**：若 `start()` 失败回滚，worker 可能在半启动状态。
  - 后果：`Cask::open()` 失败回滚路径 → UB 崩溃。
  - 改法：`stop()` 加 `started_` 标志门；或 `worker_.joinable()` guard 写得更严格。
  - 风险：低；需先核实当前 `joinable()` 是否已正确 guard（若是则降级为文档项）。

- [ ] **C5 `close()` 标 `noexcept` 但调用非 noexcept 操作** — `src/cask/cask.cpp:662`
  - 现状：`close() noexcept` 调 `maybe_group_commit()` / `active_hint_->finalize()` / `save_search_ckpt()` / `save_snapshot()`——这些都不是 noexcept。任何抛出 → `std::terminate`。
  - 后果：关闭路径任何异常 = **进程硬死**，无日志、无 cleanup 机会。
  - 改法：要么去 noexcept、要么内部 `try/catch(...)` 兜底 + 错误记到成员/日志。
  - 风险：低（去 noexcept 是 API 变更；内部 try/catch 是内部修改）。

## 第五梯队：高 ROI 性能（启动延迟 + 索引吞吐，原审计未覆盖）

> 来源：2026-06-22 recovery / analyzer / checkpoint / IndexPool 审计。
> 原审计（①-⑰）聚焦**查询热路径**；启动延迟 + 写入/索引吞吐几乎未触。下列是确证高 ROI、风险低、改动小的项。
> 预期总体：大库冷启动 **−30~50%**，写入/索引吞吐显著提升（具体数字待 A/B）。

### 启动恢复（冷启动延迟）

- [ ] **R1 `DataFile::read()` 在 fold 路径每记录 2 次堆分配** — `src/fileops/data_file.cpp:218-219`
  - 现状：`out.key.assign(...)` / `out.value.assign(...)` 每记录两次 `vector::assign` 堆分配；fold 调用方（`cask.cpp:799`）只用 `bytes_to_view()`——**堆拷贝完全浪费**。
  - 改法：加 fold-friendly 读路径返回 `DataRecordView`（span 进 thread_local 解码缓冲，仿 `get()` 的 mmap 零拷贝路径）。
  - 收益：**100 万记录启动省 200 万次 malloc**。
  - 风险：低（fold 专用，不影响 query 路径）。

- [ ] **R2 `HintFile::fold()` 每记录 2 次 pread syscall** — `src/fileops/hint_file.cpp:79-130`
  - 现状：每条 hint 记录先 `pread(18)` 读头、再 `pread(full_size)` 读完整记录——**2 syscall/记录**。
  - 改法：64-256KB thread_local chunked read + 流式解析（仿 ⑮ 的 `pread_into` 模式扩展到整段 fold）。
  - 收益：**100 万 hint 文件：200 万 syscall → hundreds**。
  - 风险：低（流式解析结构已支持，buf.resize 已有）。

- [ ] **R3 多数据文件 fold 串行** — `src/cask/cask.cpp:752-855`
  - 现状：`for (file : entries) df->fold(...)` 完全串行；keydir 分片锁本就支持并发写。
  - 改法：`std::async` 或 worker pool 并行 fold N 文件、按 tstamp 序合并入 keydir。
  - 收益：**大库多文件冷启动 ~N× 加速**（N = 数据文件数；典型 8-32）。
  - 风险：中（需保证 tstamp 顺序、keydir 并发正确、错误传播）。

### 索引/写入吞吐

- [ ] **W1 NgramAnalyzer 每 n-gram 一次堆分配** — `src/text/analyzer.cpp:187-193`
  - 现状：每个 n-gram 构造一个 `std::string`，`tpm[std::move(term)]` 还是拷贝进 map 节点。Latin 文本 1KB 文档 ~ 数千 bigram → 数千 malloc。
  - 改法：`std::string_view` 作 map key；归一化后整段 `std::string` 单独持有，key 切片进它。
  - 收益：**每文档 N 次堆分配 → 0**（N = n-gram 数；CJK trigram 更甚）。
  - 风险：低（map 必须 owning 整段 string；lifecycle 已就地）。

- [ ] **W2 `IndexTask::make` fields 参数双重拷贝 + vec/meta assign 拷贝** — `include/bitcask/thread_pool.hpp:86-107`、`src/cask/cask.cpp:1505-1513`
  - 现状：(a) `make()` 取 `fields_` by value；`task_fields()` 是 prvalue 但 copy-initialize 参数（非 move）。(b) `task.vec.assign(begin, end)` / `task.meta.assign(...)` 是拷贝。
  - 改法：(a) `fields_&&` + caller `std::move(task_fields())`；(b) vec/meta 改 `std::move` 或换 `std::span`（caller 保活到 flush）。
  - 收益：**每个 put_doc 省 1~3 次堆分配 + 1~2 次大拷贝**（128-dim vec = 512B）。
  - 风险：低（move 语义标准；span 方案需审计 caller lifetime）。

- [ ] **W3 `IndexPool::flush()` 自旋 `yield()` 浪费 CPU** — `include/bitcask/thread_pool.hpp:199-201`
  - 现状：`while (pending_.load(acquire) > 0) std::this_thread::yield();`——`pending_` cache line 在 flusher 与 worker 间来回弹。
  - 改法：加 `std::condition_variable cv_` + `std::mutex`；worker `fetch_sub` 后若归 0 则 `notify`；`flush()` wait on cv。
  - 收益：flush 路径 CPU 占用 **−5~15%**（merge / close 场景显现）。
  - 风险：低（cv 语义严格强于 spin）。

## 第六梯队：结构性优化（高收益但需设计）

> 这些需要新 API 或较大重构，建议第五梯队落地后再做（部分有依赖关系）。

- [ ] **S1 `put_doc_batch` API + `submit_index_task_batch`** — `src/cask/cask.cpp`、`include/bitcask/thread_pool.hpp`
  - 现状：每 `put_doc` 单任务入队，1M 文档批量索引 = 1M 次锁。
  - 改法：批量入队 API（vector of IndexTask 一次锁）；可选配套 `Cask::put_doc_batch`。
  - 收益：批量索引吞吐 **2-5×**（多核机器）。
  - 风险：中（新公共 API + 测试 + 文档）。

- [ ] **S2 Merger 批量 `write`（每条 pwrite → 累积 N 条一次 pwrite）** — `src/merge/merger.cpp:102-118`、`src/fileops/data_file.cpp:148`
  - 现状：fold callback 每条 live 记录调一次 `out_data->write()` → 一次 `pwrite`。
  - 改法：`DataFile::write_batch(span<Record>)` 累积到 1MB 缓冲再一次 pwrite；hint 已 batched（kFlushBytes=64KB）。
  - 收益：**100 万记录 merge 节省 ~10s**（高延迟 I/O 更显著）；syscall 数 N → N/batch_size。
  - 风险：中（需保 hint/data 位置原子性；新 API；partial write 处理）。

- [ ] **S3 Recovery 期 IndexPool 批量化 / 流水线** — `src/search/search_layer.cpp` `recover_doc`
  - 现状：recovery 时每 doc 同步走 `bm25::InvertedIndex::add_doc`，未走 IndexPool，无批量化、无流水线。
  - 改法：批量 1024 一组；analysis 与 index insert 流水线（batch N 分析 vs batch N-1 插入重叠）。
  - 收益：冷启动索引重建 **~2× 加速**（CPU bound 部分）。
  - 风险：中（需保证 ord 顺序一致）。

- [ ] **S4 Checkpoint / keydir 序列化：reserve + memcpy 替代 N 次 vector::insert** — `src/search/search_layer.cpp:803-812`、`src/keydir/keydir.cpp:1082-1089`
  - 现状：每个字段一次 `buf.insert(end, p, p+n)`；vector 多次 realloc。
  - 改法：预估总长 `reserve()` 一次，后续 `memcpy` 到 `data() + offset`。
  - 收益：docmap/keydir save 路径 **5M insert → 1 reserve + 5M memcpy**。
  - 风险：低（size 可计算）。

- [ ] **S5 Checkpoint 可选 zstd 压缩** — `src/search/search_layer.cpp:998-1109`
  - 现状：docmap/bm25/hnsw payload 全部原始序列化，大库可达 GB 级。
  - 改法：section header 加 `compression` 标志（0=raw, 1=zstd）；zstd level 1（2-4× 压缩比，~1GB/s）。
  - 收益：checkpoint 文件体积 **−50~70%**；冷启动 I/O 减少。
  - 风险：低（向后兼容：旧文件 compression=0）。

## 第七梯队：小修小补（低成本、收益较小）

> 来源：merger / analyzer / jieba 审计的边角发现。可穿插在任何阶段做。

- [ ] **P1 `merge_policy::cap_size` 无条件分配 vector** — `src/merge/merge_policy.cpp:146-164`
  - `max_merge_size==0` 时 `return files;`（const ref → copy）；改 `return {}` 让 caller 自查。
- [ ] **P2 HintFile `kFlushBytes` 64KB → 1MB** — `include/bitcask/hint_file.hpp:95`
  - merge 时 hint flush 次数 16×↓。
- [ ] **P3 `nfkc_fold` ASCII fast path 仍 std::string 拷贝** — `include/bitcask/text_utils.hpp:68-74`
  - 大多数索引文本以 ASCII 为主；in-place 或返回 `string_view`。
- [ ] **P4 `to_codepoints` 必堆分配 vector** — `include/bitcask/text_utils.hpp:99-118`
  - 改出参 `vector<CpInfo>& out` 或 `thread_local` 复用。
- [ ] **P5 Jieba `jieba_cut` 多余 `std::string(sentence)` 拷贝** — `src/text/jieba_analyzer.cpp:97-99`
  - 检查 cppjieba 是否接受 string_view。
- [ ] **P6 Jieba 输出词再走一次 NFKC + codepoint** — `src/text/jieba_analyzer.cpp:144-146`
  - 源已归一化；冗余工作；考虑缓存或短路。
- [ ] **P7 Jieba 词位置搜索 O(n²)** — `src/text/jieba_analyzer.cpp:171-185`
  - 每个 jieba 词线性扫全 codepoint 数组；考虑后缀数组或多模式匹配。

## 第八梯队：工程基础设施（不阻塞优化但阻塞可维护性）

> 来源：测试 / bench / 构建 / CI 审计。
> **重要**：第一~七梯队所有改动的「回归保护」依赖这里。没 CI 意味着 429/429 这个数字没有持续验证。

### 测试缺失（必加项标 **\***）

- [ ] **T1\* 崩溃恢复测试（fork+SIGKILL+restart → fold）** — `tests/crash_recovery_test.cpp`（新建）
  - 保护 R1/R2 + torn-write 截断路径（`cask.cpp:847-853`）。
- [ ] **T2\* Merge 并发 writer 测试** — `tests/merge_concurrent_writer_test.cpp`（新建）
  - 保护 C1 修复 + tier-1 ④ 假共享消除 + write.lock/merge.lock 独立性。
- [ ] **T3\* Checkpoint 腐败回退测试** — `tests/checkpoint_recovery_test.cpp`（新建）
  - 保护 P14e 设计契约：`search.ckpt` / `kv.keydir.ckpt` CRC 失败 → 全量 fold。
- [ ] **T4 IndexPool 背压 / 关闭排空测试** — `tests/index_pool_backpressure_test.cpp`（新建）
  - 队列满 → submit 阻塞；stop() 排空 pending。
- [ ] **T5 `key_length_histogram` 测试** — `tests/keydir_histogram_test.cpp`（新建）
  - 验证 tier-2 ⑤ 诊断探针（bucket 边界、sso/heap 计数）。
- [ ] **T6 `thread_local encoded` 并发测试** — `tests/thread_local_encoded_buffer_test.cpp`（新建）
  - 保护 tier-3 ⑩ 多线程并发 put 无干扰。

### Bench 缺失

- [ ] **B1 Merge 吞吐 bench** — `bench/merge_bench.cpp`（新建）
  - 度量 S2 + tier-1 ④；记录数/秒、MB/秒。
- [ ] **B2 IndexPool 异步路径 bench** — `bench/index_pool_bench.cpp`（新建）
  - 度量 W2/W3；submit 延迟、worker 处理延迟。
- [ ] **B3 大 keydir bench（>cache，1M key）** — 扩展 `bench/keydir_bench.cpp`
  - 永久回归保护 tier-2 ⑤ 的 −31%；当前 1024 key 永远 cache-hot 测不出。
- [ ] **B4 Checkpoint 保存/加载 bench** — `bench/checkpoint_bench.cpp`（新建）
  - 度量 S4/S5；ms/save、ms/load。

### 构建加固（独立于优化，可任意时刻加）

- [ ] **H1 栈保护 `-fstack-protector-strong`** — `CMakeLists.txt`
- [ ] **H2 `_FORTIFY_SOURCE=2`** — `CMakeLists.txt`（需配合 `-O2` 以上）
- [ ] **H3 Full RELRO `-Wl,-z,relro,-z,now`** — `CMakeLists.txt`
- [ ] **H4 可执行文件 PIE `-pie`** — `CMAKE_EXE_LINKER_FLAGS`（migrate_le / gen_inert_table）
- [ ] **H5 PCH（precompiled header）** — `CMakeLists.txt`
  - `target_precompile_headers(bitcask_cask PRIVATE bitcask/cask.hpp)` 等；编译时间 −20~30%。

> 当前只有 `-fvisibility=hidden` + `-fPIC`（SO 用）；RELRO/FORTIFY/栈保护全无。

### CI（完全空白）

- [ ] **CI1 `.github/workflows/ci.yml`：Release + ctest on PR**
- [ ] **CI2 Sanitizer matrix（ASAN/UBSAN/TSan）**
  - TSan 是 Barrier v2 / IndexPool 并发协议的唯一回归保护。
- [ ] **CI3 Benchmark-on-PR 追踪**
  - 周期跑 bench → JSON artifact；可选 PR 回归检测（>10% 报警）。

> 现 429/429 全绿仅本地验证；无 CI 意味着无回归检测。

## 已核实「无需改动」（避免重复审计）

- CRC32：PCLMULQDQ 硬件路径 + zlib fallback，已最优。
- 内存序：无过强 `seq_cst`，全部 relaxed/acquire/release 带 happens-before 注释。
- 读路径：sealed 文件 mmap 零拷贝 + thread_local 复用 read_buf。
- SIMD：运行时 `__builtin_cpu_supports` 派发，`-march=native` 故意缺省（保通用构建）。
- LTO/IPO：已开（`BITCASK_LTO=ON`）。
- `live` 谓词 `std::function`：仅在结果收集 O(k) 处调用，不在图遍历热路径。
- BM25 评分：branchless + SIMD 派发，live/dl 批量化消除 per-posting 虚调用。
- Merger fold buffer 复用：`data_file.cpp` write_buf_ / hint pending_ 已正确复用（输出侧无堆分配问题；S2 是 syscall 批量化的独立维度）。
- `std::string_view` key in fold callback：`merger.cpp:89-91` 已零拷贝指向 fold 复用缓冲。
- IndexPool 队列底座：`tbb::concurrent_bounded_queue` 已 MPSC lock-free；问题在 flush 等待策略（W3）而非队列本身。

## 建议执行顺序

**已完成（2026-06-22）**：第一梯队 ①②③④ + 第二梯队 ⑤⑥ + 第三梯队 ⑦⑩⑭⑮⑯⑰。⑧⑨⑫⑬ 跳过（风险/不可测/无收益），⑪ 按设计否决（WAL 语义）。

**下一波建议（按依赖关系）**：

1. **第四梯队（C1-C5）+ T1-T3 配套测试** ← 阻塞生产部署，必须先做
2. **CI1-CI2（Release + TSan）** ← 后续一切改动的护栏
3. **第五梯队 R1/R2/W1**（启动 + 索引热路径，最高 ROI 性能）
4. **第五梯队 R3/W2/W3**（需 R1/W1 基础）
5. **第六梯队 S1-S5**（结构性优化，依赖第五梯队的 batch/zero-copy 基础）
6. **第七梯队 P1-P7** 按需穿插
7. **第八梯队剩余（T4-T6、B1-B4、H1-H5、CI3）** 长期推进

> **关键决策点**：第一~三梯队是「查询热路径性能」；第四梯队是「生产正确性」；第五梯队是「启动 + 索引吞吐」。**前者的优化基础不能跳过后者**——否则性能建立在脆弱基础上。
