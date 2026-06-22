# 性能优化与正确性任务清单

> 来源：2026-06-22 全代码库性能分析 + API/错误处理/可观测性/工程基础设施审计。
> 背景：代码库已高度优化（运行时 SIMD 派发、LTO 已开、内存序精确、256-shard、零拷贝）。

## 已完成归档（2026-06-22）

| 梯队 | 范围 | 关键收益 |
|---|---|---|
| 第一梯队 ①-④ | HNSW rerank / WAND 排序 / qcodes 条件分配 / FStats 对齐 | HNSW 查询延迟 −7~9%，WAND 4096 词 −5.1% |
| 第二梯队 ⑤-⑥ | KeyDir 换 ankerl::unordered_dense / HNSW 邻接 bump-slab arena | 大 keydir Get_Single −31%，HNSW 累计 −11~15% |
| 第三梯队 ⑦⑩⑭⑮⑯⑰ | thread_local scratch/encoded、serialize 复用、hint pread_into、prefetch dim、-march=native 开关 | put -4~6%，分配热路径复用 |
| 第四梯队 C1-C5 | merger 全 9 错误路径 cleanup + 无条件 fsync + **keydir 延后 apply** + synonym 错误传播 + IndexPool 异常计数 + IndexPool 析构 guard 注释 + close() try/catch 兜底 | 失败后 keydir 完全未动→立即可见，无需重启恢复；close 路径 noexcept 安全 |
| 测试 T1 | 崩溃恢复（fork+SIGKILL）+ MergeFailurePreservesKeyDirVisibility | 432/432 ctest 通过 |
| CI1 | GitHub Actions workflow（Release + ASAN/UBSAN/TSan matrix） | 后续改动的回归护栏 |

**跳过**（保留作为审计记录）：⑧（⑦实测前例中性）/⑨（此机不可测）/⑫（FP 风险）/⑬（无法缓存）；**⑪ 按设计否决**（WAL 语义下缓冲 pwrite = 丢数据）。

## ⚠️ 基准测试警告

当前 `build/` 目录是 Debug 配置（`-g`，无 `-O`）。跑 benchmark 前务必用独立的 Release build 目录（`-O3 -DNDEBUG` + LTO），否则数字无意义。

---

## 待办：生产正确性（必做，<1 天/项，第四梯队剩余）

> 用户/数据可见的失败模式，建议在任何下一波性能优化前先清掉。

**全部完成**——C1-C5 已全部落地（详见「已完成归档」）。下一波可专注性能。

---

## 待办：高 ROI 性能（第五梯队，启动延迟 + 索引吞吐）

> 原审计聚焦查询热路径；启动延迟 + 写入/索引吞吐几乎未触。预期总体：大库冷启动 **−30~50%**，写入/索引吞吐显著提升。

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

---

## 待办：结构性优化（第六梯队，高收益但需设计）

> 建议第五梯队落地后再做（部分有依赖关系）。

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

---

## 待办：小修小补（第七梯队，低成本、收益较小）

> 可穿插在任何阶段做。

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

---

## 待办：工程基础设施（第八梯队）

### 测试缺失（必加项标 **\***）

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

### CI 剩余

- [ ] **CI2 Sanitizer matrix（ASAN/UBSAN/TSan）自动化验证**
  - 当前 ci.yml 已配置 matrix，但尚未在 GitHub 实际跑过；TSan 是 Barrier v2 / IndexPool 并发协议的唯一回归保护。首次 push 后核实 workflow 运行情况。
- [ ] **CI3 Benchmark-on-PR 追踪**
  - 周期跑 bench → JSON artifact；可选 PR 回归检测（>10% 报警）。

---

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
- **⑪ data 文件缓冲 pwrite**：按设计否决（WAL 语义下 = 丢数据；详见归档）。
- **⑧ HNSW search_layer 堆复用**：跳过（⑦实测前例性能中性）。
- **⑨ AVX2 intersection branchless compress**：跳过（此机不可测 + 风险）。
- **⑫ WAND total_ub 增量维护**：跳过（FP 风险，epsilon 敏感）。
- **⑬ select_neighbors O(M²) 距离缓存**：跳过（candidate↔picked 两两唯一，无法缓存）。

---

## 建议执行顺序

**下一波建议（按依赖关系）**：

1. ~~**C4 + C5** ← 收尾正确性问题~~ ✅ 完成（2026-06-22）
2. **R1 + R2 + W1** ← 高 ROI 性能（启动 + 索引热路径，**最高优先级**）
3. **T2 + T3** ← C1 / checkpoint 的回归保护（必加测试）
4. **R3 + W2 + W3** ← 需 R1/W1 基础
5. **S1 - S5** ← 结构性优化，依赖第五梯队的 batch/zero-copy 基础
6. **P1 - P7** 按需穿插
7. **T4 - T6、B1 - B4、H1 - H5、CI3** 长期推进

> **关键决策点**：所有「生产正确性」问题已清空（C1-C5 全部落地）。
> 下一波最高 ROI：R1/R2/W1——大库冷启动 −30~50%，索引吞吐显著提升。
