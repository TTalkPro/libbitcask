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

- [ ] **⑦ HNSW `scratch`/`found`/`pool` 每次调用都 new → 改 `thread_local`** — `src/vector/hnsw.cpp:727,841,749,792`（仿已有 `t_visited`）。查询 −2~5%。
- [ ] **⑧ `search_layer` 每次构造两个 `std::priority_queue`（各自 malloc）→ 复用扁平有界堆** — `src/vector/hnsw.cpp:422-423`（int8 双胞胎 522-523）。查询 −5~10%。
- [ ] **⑨ AVX2 intersection 用 4 个数据相关分支提取匹配 → branchless compress** — `src/bm25/intersect.cpp:95-98`（AVX-512 路径已用 `compressstoreu`；文件注释已标 PairLut 待测）。
- [ ] **⑩ 公共 `put` 把 value memcpy 两次 + 每次 new `encoded`** — `src/cask/cask.cpp:1361-1365`。(a) `encoded` 改复用缓冲（仿 `write_buf_`）；(b) scatter-write 消除 DocValue→record 双拷贝。
- [ ] **⑪ data 文件每条记录一次无缓冲 `pwrite`（hint 已缓冲）** — `src/cask/cask.cpp:1113-1120`、`src/fileops/data_file.cpp:148`。缓冲 data 追加，group-commit/roll 时合并 flush，系统调用 N→N/sync_n。
- [ ] **⑫ WAND `total_ub` 每轮重新累加所有 term 上界** — `src/bm25/inverted.cpp:587-590`。改维护增量 `live_ub_sum`。
- [ ] **⑬ `select_neighbors` O(M²) 距离重算无缓存** — `src/vector/hnsw.cpp:584`（int8 版 607）。构建期成本，build 吞吐 +5~10%。
- [ ] **⑭ `serialize` 每 FOR block / positions 列表 new 一个 vector** — `src/bm25/inverted.cpp:1747-1751,1695-1701`。save 路径，hoist + reuse 缓冲。
- [ ] **⑮ `HintFile::validate_trailer` 每 64KiB chunk 一次堆分配** — `src/fileops/hint_file.cpp:157-166`。改 `pread_into` + 复用缓冲（fold 已用此模式）。
- [ ] **⑯ `f32 prefetch_vec` 不感知 dim** — `src/vector/hnsw.cpp:222-237`。仿 int8 路径按 dim 守卫每条 prefetch。
- [ ] **⑰ 可选 `-march=native` / jemalloc 构建开关** — `CMakeLists.txt`。opt-in `BITCASK_NATIVE`（scalar glue ~3-8%）、`BITCASK_MALLOC`（写密集分配压力）。注意：**勿全局加 `-ffast-math`**，HNSW/int8 依赖精确可复现浮点（self-test 校验 ULP）。

## 已核实「无需改动」（避免重复审计）

- CRC32：PCLMULQDQ 硬件路径 + zlib fallback，已最优。
- 内存序：无过强 `seq_cst`，全部 relaxed/acquire/release 带 happens-before 注释。
- 读路径：sealed 文件 mmap 零拷贝 + thread_local 复用 read_buf。
- SIMD：运行时 `__builtin_cpu_supports` 派发，`-march=native` 故意缺省（保通用构建）。
- LTO/IPO：已开（`BITCASK_LTO=ON`）。
- `live` 谓词 `std::function`：仅在结果收集 O(k) 处调用，不在图遍历热路径。
- BM25 评分：branchless + SIMD 派发，live/dl 批量化消除 per-posting 虚调用。

## 建议执行顺序

第一梯队 **①②③④** 可一批做完（各处改动小、互不耦合），跑测试验证后再上结构性的 **⑤⑥**。第三梯队按需穿插。
