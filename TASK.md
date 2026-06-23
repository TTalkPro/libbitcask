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

- [x] **R1 `DataFile::read()` 在 fold 路径每记录 2 次堆分配** — `src/fileops/data_file.cpp:218-219`
  - **已完成（2026-06-23 核实）**：fold 路径已返回 `DataRecordView`（zero-copy span 进复用 buf），`cask.cpp:820` 回调以 `const DataRecordView& view` 接收。`read()` 的 `out.key.assign`/`out.value.assign`（218-219 行）属于**单记录 get() 路径**，非 fold 路径——fold 已零分配。

- [x] **R2 `HintFile::fold()` 每记录 2 次 pread syscall** — `src/fileops/hint_file.cpp:86-182`
  - **已完成（2026-06-23）**：改为 256 KiB `thread_local` chunked pread + 流式解析。refill lambda 把残留 memmove 到 buf 头部后一次 pread 读满 256 KiB；多 record 从单 chunk 解析后才 refill。
  - 实测：100 万 hint 文件 **200 万 syscall → 46 次**（4348× ↓）。
  - 防膨胀：buf > 1 MiB 时 fold 结尾 `clear()+shrink_to_fit()`。
  - 全量 439/439 ctest 通过。

- [x] **R3 多数据文件 fold 串行** — `src/cask/cask.cpp:752-855`
  - **已完成（2026-06-23）**：per-file fold 抽成 `fold_one(e)`，纯 KV 恢复
    （`search_layer==null`）且文件数 > 1 时用 worker pool（`hardware_concurrency`
    上限、原子计数器分发）并行 fold；结果数组收集错误统一传播。
  - **并发正确性论证**：keydir 冲突解析按 `(file_id, tstamp, offset)` LWW，与到达
    序无关（`put_overwrite`）；`update_fstats` 全程无锁原子累加；cold-start 期
    `keyfolders_==0` → 新 key 直入分片 `entries`（不触 `meta_mu_`），256 分片提供
    真并发；`increment_file_id_at_least`/`advance_ord`/`biggest_file_id_` 均原子。
  - **search_layer 存在时仍串行**（HNSW 单写者 + BM25 ord 序约束属 S3 域）。
  - 验证：新增 `MultiFileParallelFoldRecovers`（多文件 + 跨文件覆盖校验 LWW），
    Release 全量 440/440 ctest 通过；**TSan 插桩跑该测试零 data race**。
  - 收益：纯 KV 大库多文件冷启动 ~min(N, 核数)× 加速。

### 索引/写入吞吐

- [x] **W1 NgramAnalyzer 每 n-gram 一次堆分配** — `src/text/analyzer.cpp:167-265`
  - **已完成（2026-06-23）**：内部改用 `unordered_map<string_view, ...>` 去重，key 是 `normalized` 本地 string 的切片；末尾一次性转成 owning `TermPositionsMap`。`emit_ngrams` 和 `emit_word` 均 zero-alloc。
  - 分配数：O(N)（N = n-gram 总数，含重复）→ **O(U)**（U = 唯一 term 数）。
  - 无 API 变更；`WhitespaceAnalyzer`/`JiebaAnalyzer` 不受影响。
  - 全量 439/439 ctest 通过（含 analyzer/search_layer/docvalue/jieba/stemming）。

- [x] **W2 `IndexTask::make` fields 参数双重拷贝 + vec/meta assign 拷贝** — `include/bitcask/thread_pool.hpp:86-107`、`src/cask/cask.cpp:1505-1513`
  - **已完成（2026-06-23）**：cosine 路径 `vec_out` 是本地 `vec_norm` 的 span，
    encode（`parts.vector`）用完后直接 `task.vec = std::move(vec_norm)`，省一次
    512B（128-dim）拷贝 + 分配；passthrough/L2 仍按需 `assign`。
  - **(a) make() fields 经核实非双重拷贝**：`task_fields()` 是 prvalue，传入
    by-value 参数 C++17 强制 elision → 仅一次构造，随后 `std::move` 入 `t.fields`；
    原审计「copy-initialize」描述不确，无可省拷贝，未改签名（避免无收益 churn）。
  - **meta 仍 assign**：`doc` 是 `const DocInput&`，不可移动；无 API 变更下属固有拷贝。
  - 验证：Release 440/440 ctest 通过。
  - 风险：低（move 语义标准）。

- [x] **W3 `IndexPool::flush()` 自旋 `yield()` 浪费 CPU** — `include/bitcask/thread_pool.hpp:199-201`
  - **已完成（2026-06-23）**：加 `std::mutex flush_mu_` + `std::condition_variable
    flush_cv_`；worker 的两处 `pending_` 减 1 统一走 `dec_pending()`，`fetch_sub`
    返回 1（即归 0）时持锁 `notify_all`；`flush()` 在锁下 `wait` 谓词 `pending_==0`
    （锁下复查 + worker notify 同持锁 → 无丢失唤醒）。仅归零时取锁，重负载罕见。
  - 验证：Release 440/440 ctest 通过；TSan 插桩 thread_pool/crash 路径无新 race。
  - 风险：低（cv 语义严格强于 spin）。

---

## 待办：结构性优化（第六梯队，高收益但需设计）

> 建议第五梯队落地后再做（部分有依赖关系）。

- [ ] **S1 `put_doc_batch` API + `submit_index_task_batch`** — `src/cask/cask.cpp`、`include/bitcask/thread_pool.hpp`
  - 现状：每 `put_doc` 单任务入队，1M 文档批量索引 = 1M 次锁。
  - 改法：批量入队 API（vector of IndexTask 一次锁）；可选配套 `Cask::put_doc_batch`。
  - 收益：批量索引吞吐 **2-5×**（多核机器）。
  - 风险：中（新公共 API + 测试 + 文档）。

- [x] **S2 Merger 批量 `write`（每条 pwrite → 累积 N 条一次 pwrite）** — `src/merge/merger.cpp`、`src/fileops/data_file.cpp`
  - **已完成（2026-06-23）**：`DataFile` 新增 `write_buffered()` + `flush_batch()`：
    record 编码进 `batch_buf_`（encode 是 append 语义，累积），累计 ≥ 1 MiB 才一次
    `pwrite`；返回的 `offset` 取 `current_offset_`（逻辑位置，含未落盘缓冲），
    确定性、与落盘时机解耦——hint/keydir 引用照旧正确。`flush_batch()` 把残尾
    pwrite 到 `current_offset_-batch_buf_.size()` 起点。`sync()` 内部先兜底
    `flush_batch()`，防漏 flush 致缓冲未落盘却被采信。
  - merger fold callback 改调 `write_buffered`，input 循环结束后显式 `flush_batch()`
    （错误走 cleanup 而非掩在 sync）；末尾 fsync 序不变。**仅 merge 输出用本 API**
    （末尾统一 fsync 后才被 caller 采信）；put 的 WAL 每条 durable 语义不变（⑪ 否决）。
  - syscall：data pwrite 数 **N → ⌈总字节/1MiB⌉**（如 1M 记录 ~数百万 → 数十次）。
  - 验证：新增 `S2BatchedMergeManyRecordsRoundTrip`（写 ~3.3 MiB live + 跨文件覆盖 →
    merge 输出跨阈值多次 flush → 逐 key 读回 + 重开再验，端到端校验 flush 边界两侧
    offset 连续）。Release 442/442 ctest 通过；merge 并发护栏测试 TSan 零 race。
  - 风险：中（已含 partial-write：`PosixFile::pwrite` 循环写满；offset 原子性靠
    逻辑 offset 确定性 + 末尾统一 fsync）。

- [ ] **S3 Recovery 期 IndexPool 批量化 / 流水线** — `src/search/search_layer.cpp` `recover_doc`
  - 现状：recovery 时每 doc 同步走 `bm25::InvertedIndex::add_doc`，未走 IndexPool，无批量化、无流水线。
  - 改法：批量 1024 一组；analysis 与 index insert 流水线（batch N 分析 vs batch N-1 插入重叠）。
  - 收益：冷启动索引重建 **~2× 加速**（CPU bound 部分）。
  - 风险：中（需保证 ord 顺序一致）。

- [x] **S4 Checkpoint / keydir 序列化：reserve + memcpy 替代 N 次 vector::insert** — `src/search/search_layer.cpp`、`src/keydir/keydir.cpp`
  - **已完成（2026-06-23）**：核心问题是 **reallocation churn**（从零/低估容量起几何
    增长，GB 级 buffer 累计搬运 ~2× 终态字节），非 `insert` 本身——trivial 元素
    的 end-`insert(end,p,p+n)` 在容量足够时即编译为 memcpy。故修复 = 精确 reserve：
    - `keydir.cpp save_snapshot`：旧 `64 + entries_total*56`（按 ~18B 均长 key，长 key
      反复 realloc）→ 一次算出 `头+标量+fstats+watermarks+entries(38/条)+key 字节+crc`。
      变长 key 段用增量维护的 `key_bytes_` 原子（put +/remove −；快照点 keyfolders_==0
      全 SingleEntry 时即 live key 字节总和），**免去对 entries 的第二趟随机遍历**
      （大 keydir 下二次遍历可能比省下的 realloc 还贵）。
    - `search_layer.cpp serialize_docmap`：原 **完全无 reserve** → 按 `index_.info()
      .live_docs`（O(1) 计数器）预留 `28 + live*(34+48)`（34B 固定行 + ext 估值）。
  - reserve 偏差只影响容量（偏小→个别 realloc，偏大→略浪费），**绝不溢出 / 不影响
    正确性**；故未改用裸指针 cursor（溢出风险换边际收益不划算）。
  - 验证：Release 441/441 ctest 通过（含 keydir snapshot 与 search.ckpt 全量 round-trip：
    MidPut/TornWrite/MultiFileParallelFold 写读快照 + CheckpointRecovery 全 4 例）。
  - 风险：低（纯容量预留，行为不变）。

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

- [x] **T2\* Merge 并发 writer 测试** — `tests/merge_concurrent_writer_test.cpp`
  - **已完成**：3 例（ConcurrentMergeWithActiveReader / ConcurrentMergePreservesActiveFile
    / MergeFailureLeavesKeydirConsistent）保护 C1 修复 + write.lock/merge.lock 独立性；
    Release 全绿 + TSan 零 race（本轮多次复跑）。
- [x] **T3\* Checkpoint 腐败回退测试** — `tests/checkpoint_recovery_test.cpp`
  - **已完成**：4 例（CorruptKeydir / MissingCheckpoint / CorruptSearch /
    CorruptSearchPrevGenerationFallback → 全量 fold）保护 P14e 设计契约；全绿。
- [ ] **T4 IndexPool 背压 / 关闭排空测试** — `tests/index_pool_backpressure_test.cpp`（新建）
  - 队列满 → submit 阻塞；stop() 排空 pending。
- [ ] **T5 `key_length_histogram` 测试** — `tests/keydir_histogram_test.cpp`（新建）
  - 验证 tier-2 ⑤ 诊断探针（bucket 边界、sso/heap 计数）。
- [ ] **T6 `thread_local encoded` 并发测试** — `tests/thread_local_encoded_buffer_test.cpp`（新建）
  - 保护 tier-3 ⑩ 多线程并发 put 无干扰。
- [x] **T7 X1 显式 release 路径回归** — `tests/crash_recovery_test.cpp`
  - **已完成（2026-06-23）**：`IteratorExplicitReleaseAfterCloseNoUaf`——close() 后
    显式 `it->release()`（验证 `iter_->release → iter_.reset → keydir_pin_.reset`
    序）+ 二次 release 幂等 + `it.reset()`。Release 通过 + TSan 零 race。
- [x] **T8 X1 多 iterator 交错 release → MultiEntry 折叠正确性** — `tests/crash_recovery_test.cpp`
  - **已完成（2026-06-23）**：`MultiIteratorInterleavedReleaseAfterClose`——3 iterator，
    fold 态 overwrite k0 造 MultiEntry，close() 后交错 release（it1/it3 非末位不折叠），
    其间用活的 it2 drain 全部 entry，断言见 k0..k4 且 k0=新 revision（链头）；末位
    it2 release（keyfolders_→0）触发折叠。**附带实证 close 后 next() 可用**（T9 点 1）。
    Release 通过 + **TSan 零 race**（折叠机制在 pinned KeyDir 上无 race）。
- [x] **T9 iterator ↔ Cask 对象生命周期契约文档化** — `include/bitcask/cask.hpp`
  - **已完成（2026-06-23）**：CaskIter 类头补生命周期契约：①可跨 close() 存活
    （keydir_pin_ + pin_files 保活，next/release 仍可用，多 iterator 折叠安全）；
    ②**必须先于 Cask 对象析构**（`parent_` 裸 `Cask*`，Cask 销毁则 next() 访问
    `parent_->opts_` UAF——与 close() 正交，X1 未恶化）。
  - **修正原计划 (a)**：经核实「在 next() 加 keydir_ 空检查」是**有害的**——X1 已让
    close 后 next() 合法（T8 实证），空检查会误杀该合法用法；且无法防真正的
    parent_ 悬空（裸指针解引用本身即 UAF）。故只做文档，结构性修复（weak_ptr/
    owning 句柄，原 (b)）留 zero-copy 重构。测试由 T7/T8 覆盖可测部分（点 2 违反
    即 UB，不可测）。

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
2. ~~**R1 + R2 + W1** ← 高 ROI 性能（启动 + 索引热路径）~~ ✅ 完成（2026-06-23）
3. ~~**T2 + T3** ← C1 / checkpoint 的回归保护~~ ✅ 完成
4. ~~**R3 + W2 + W3** ← 需 R1/W1 基础~~ ✅ 完成（2026-06-23）
5. **X1** ← 正确性收尾（本轮 TSan 发现）✅ 完成（2026-06-23）
6. **S1 - S5** ← 结构性优化，依赖第五梯队的 batch/zero-copy 基础
7. **P1 - P7** 按需穿插
8. **T4 - T6、B1 - B4、H1 - H5、CI3** 长期推进

> **关键决策点**：R1-R3 + W1-W3 + X1 + S4 + S2 全部落地。
> 剩余结构性优化：**S5**（checkpoint zstd，低风险但引第三方依赖）、
> **S1**（put_doc_batch 公共 API，中风险需测试+文档）、
> **S3**（recovery IndexPool 流水线，中风险需保 ord 序）。
> 建议下一步 **S1**（批量索引吞吐 2-5×，纯写侧、不碰磁盘格式），或按需穿插
> P1-P7 小修。S5 需评估是否愿引入 zstd 依赖再定。

## 待办：本轮发现（2026-06-23 TSan 跑出）

- [x] **X1 `Cask::close()` 释放 keydir 后存活的 iterator → UAF** — 既有问题，非本轮引入
  - **已完成（2026-06-23）**：`CaskIter` 新增 `std::shared_ptr<keydir::KeyDir>
    keydir_pin_` 成员（声明在 `iter_` 之前）；`start()` 建 `IterHandle` 前先
    `keydir_pin_ = parent_->keydir_` 复制一份引用，`release()` 在 `iter_.reset()`
    之后才 `keydir_pin_.reset()`——保证 `IterHandle::release()→BarrierGuard` 锁
    KeyDir mutex 期间该 KeyDir 始终存活。
  - 现象（修复前）：TSan 插桩 `MidPutRestartFoldsCorrectly` 报 heap-use-after-free：
    `it=make_iter()` 后调 `(*c)->close()`（reset keydir shared_ptr + registry
    release），随后 `it` 析构 → `IterHandle` 裸 `KeyDir*` 悬空。
  - 已核实**在 clean tree（无本轮改动）同样复现**——纯生命周期序问题，与 R3
    并行 fold 无关。Release/ASAN 未暴露（释放内存恰未被复用）。
  - 回归保护：新增 `IteratorAliveAcrossCloseNoUaf`（迭代器跨 close 存活）；
    已验证**移除修复后该测试在 TSan 下必 UAF**，加回后通过。Release 441/441
    + 全 3 crash-recovery 测试 TSan 零 race。
  - **副作用契约**：iterator pin 会让 KeyDir 存活到迭代器析构；若同名库在此期间
    被 `open()`，registry 建新 KeyDir，老 iterator 在已释放出 registry 的旧
    KeyDir 快照上完成 fold——隔离、无正确性影响（fd 已 S13 pin）。
