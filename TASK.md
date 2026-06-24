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

- [⛔] **S1 `submit_index_task_batch`（仅索引入队批量化）** — **被 S6 取代，否决**
  - 原结论：入队批量化只省 producer 侧 N−1 次 atomic RMW，**碰不到消费端单 worker
    串行瓶颈**（收益有限）。
  - **被 S6 取代**：S6 直接解掉「单 worker 串行消费」这个真瓶颈（并行 analyze），
    S1 的入队批量化在 S6 架构下无独立价值。详见 `docs/design/async-index-pipeline.md`。

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

- [x] **S3 Recovery 期索引重建批量并行 analyze** — `src/search/search_layer.cpp`、`src/cask/cask.cpp`
  - **已完成（2026-06-23）**：新增 `SearchLayer::recover_doc_batch`——一批文档的
    `analyze_with_positions` 走 `tbb::parallel_for` **并行**（analyzer 仅 const 配置态、
    无可变 scratch，cppjieba `Cut` 亦 const 线程安全 → 纯函数并发安全），随后**按
    batch 序串行插入**索引/HNSW（插入序 == fold 序 → 与逐条 `recover_doc` 字节等价；
    HNSW 单写者 = 本线程）。`cask.cpp` 恢复 fold 把 `recover_doc` 攒成 1024 一批，
    **墓碑前强制 flush** 保「文档↔墓碑」相对序。
  - 安全前提核实：恢复期 IndexPool worker 阻塞在空队列、仅主线程碰 index；recover_doc
    本就以 fold 序（非严格 ord 序）调用 → index 早已容忍任意插入序，故只需保持
    fold 序即可逐字节复现。
  - 收益：冷启动索引重建的 **analyze（CPU bound 大头）并行化 ~核数×**；插入串行不变。
    （流水线 overlap fold-IO 与 analyze 是进一步优化，未做——风险/收益不划算。）
  - 验证：新增 `S3BatchedRecoveryMatchesSerial`（1500 文档跨 batch + 删除穿插）——断言
    **批量 fold 恢复的搜索结果集 == 异步索引结果集**；**已实证移除 tomb-flush 则
    key1030/1040/1050 等高位被删 key 复活**（搜索可观测，非被 keydir live filter 掩盖）→
    测试确为护栏。Release 445/445 + TSan 零 race。
  - 风险：中（已通过等价性测试 + TSan 验证）。

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

- [x] **S6 异步索引 MapReduce 流水线（全局双池）** ✅ Phase 0-4 全部完成（2026-06-24，G1+G2 达成）— 设计稿 `docs/design/async-index-pipeline.md`
  - **背景**：当前每库一个 `IndexPool` 单 worker，把 analyze（CPU 重，纯函数）与
    insert（改共享索引，必串行）焊死 → ① 热点库吞吐被单 worker 锁死；② 库数不定
    → 常驻 worker 线程随库数线性膨胀。S1（入队批量化）碰不到瓶颈，已被本条取代。
  - **目标**：G1 热点库 analyze 并行吃满多核；G2 索引线程数与库数解耦（≈2×核数）；
    G3 与现串行 worker **字节等价**；G4 不削弱任何现有不变量（LWW/墓碑序/durability/checkpoint）。
  - **架构**：全局共享 **Map 池**（并行分词，纯函数）+ 全局共享 **Reduce 池**
    （per-库串行车道：reorder buffer 按 ord 序 apply + per-库 apply 锁）。
  - **核心约束（设计稿 §3 F4∧F5）**：当前正确性 = 单写线程让「到达序==ord序」⇒
    到达序 LWW 等价 ord 序 LWW。analyze 一并行则完成序乱 → **必须 reorder buffer
    把 apply 拗回 ord 序**（否则被删 key 复活）。乱序脆弱点**仅** `ext2ord_` 一行
    （设计稿 §9.1）。
  - **决策（设计稿 §14，已定稿 2026-06-23）**：D1 **策略 A**（reorder buffer，按 ord 序
    apply，不改 index 核心）/ D2 **registry 级 + registry 强制**（open 无 registry 报错，
    无 fallback）/ D3 N=核数·M≤4 / D4 reorder in-flight 上限待 bench / D5 接受慢分词队头
    阻塞致该库可见性短暂延迟 / D6 单写契约不放宽。

  - [x] **S6-P0-pre registry 强制化（纯 API 硬化）** — `include/bitcask/cask.hpp`、`src/cask/cask.cpp`、`c_api/bitcask_c.cpp`、`tests/`、`bench/`
    - **已完成（2026-06-23）**：`Cask::open()` **移除 `=nullptr` 默认** + 顶部 null 校验
      返回 `kInvalidOption`（双保险，编译期 + 运行期）。
    - **迁移 151 处调用点**（4 测试文件 132 + 3 bench 文件 19）：各文件匿名 namespace
      加 `test_registry()` 静态局部 registry 访问器，Python 平衡括号注入器统一注入
      `&test_registry()`（含 `v31_opts(4)` 等嵌套括号正确处理）。
    - **C API**（真生产调用方，原传 nullptr）：加进程级 `c_api_registry()` 全局 registry
      —— 即「每共享库实例一个全局 registry」生产形态。
    - **行为等价性**：测试共享 registry 对「open→close→reopen」经 refcount 归零重载等价；
      read_write 双开撞 write.lock 本不可能 → 无同目录并发共享风险。全量 **452/452 通过**。
    - 新增契约护栏 `CaskRegistryContract.OpenWithNullRegistryReturnsInvalidOption`。
    - 文档同步：README / doc/api-cpp.md 的 open 签名与示例。
  - [x] **S6-P0 重构（无行为变更）** — `include/bitcask/search_layer.hpp`、`src/search/search_layer.cpp`、`src/cask/cask.cpp`
    - **已完成（2026-06-23）**：`on_write_fields` 拆 `map_analyze()`（`const` 纯函数：
      analyze + catch-all 合并下推，产 owning `ReduceJob`）+ `reduce_apply(job, meta_span, vec_span)`
      （锁下：ord_field_lens / per-field add_doc / catch-all add_doc / put_doc / doc_texts / set_meta /
      on_vector / cache.invalidate）。`on_write_fields` 降为薄包装（map→reduce），签名不变。
    - **ReduceJob 结构**：`ReduceJob::FieldResult{field_name, terms, doc_len}` per-field 列表 +
      `ca_data`/`ca_len`/`wrote_default` catch-all + `doc_text`（高亮原文）+ DocSlot 定位。
      P0 不含 `lib`/routing 字段（P2+ 跨线程时扩展）。
    - **reduce_apply 折入 set_meta + on_vector**：worker 不再分开调（fields 路径）；
      meta/vec 以 `std::span` 传入免拷贝（P0 同线程；P2+ 跨线程时 MapJob 承载 owning 拷贝）。
    - **recover_doc / recover_doc_batch 复用 map_analyze**：recover_doc 喂单 kDefaultField（触发
      `wrote_default=true` → 不走 catch-all，与旧逐条版语义一致）；batch Phase 1 `tbb::parallel_for`
      调 `map_analyze`（const → 线程安全），Phase 2 串行 `reduce_apply`（逐条 cache_.invalidate，
      恢复期无查询，最终态一致）。
    - **on_write（单 text）不动**：保留 `cache_.invalidate_terms()`（selective）— 与
      `on_write_fields` 的 `cache_.invalidate()`（full）行为不同；worker 单 text 路径不变。
    - 验证：Release **452/452 ctest 通过**（0 warning on modified files）；TSan 插桩跑
      crash_recovery + search_layer + thread_pool + cask_docvalue 共 **87/87 零 race**。
    - 风险：低（纯重排、同线程序；catch-all merge 逐字节保持；锁序 fields_mu_ → index_.mutex_ 不变）。
  - [x] **S6-P1 reorder buffer 基础设施（仍单 worker，map 仍同步）** — `include/bitcask/thread_pool.hpp`、`src/cask/cask.cpp`、`tests/thread_pool_test.cpp`
    - **已完成（2026-06-23）**：引入 `IndexOp::Skip` 枚举 + `applied_ord_`/`submitted_ord_hwm_`
      原子水位跟踪 + `flush()` 谓词升级。
    - **IndexPool 改动**：
      - `submit()` CAS 更新 `submitted_ord_hwm_`（排除 Sentinel/RebuildHnsw——不携带 ord）。
      - `worker_loop` 在 consumer 返回后、`dec_pending` 前调 `track_applied(task)` 更新
        `applied_ord_`（保证 flush 谓词在 notify 时看到最新值）。
      - `flush()` 谓词从 `pending_==0` 升级为 `pending_==0 && applied_ord_>=submitted_ord_hwm_`
        （P1 单 worker 下二者等价——`pending==0 ⟹ applied>=hwm`；P2 并行 map 后成为独立必要条件）。
      - 新增 `applied_ord()`/`submitted_ord_hwm()` public getter（测试用）。
    - **Consumer lambda**：新增 `IndexOp::Skip` no-op 分支（无索引操作；worker_loop 的
      `track_applied` 推进 `applied_ord_`）。
    - **write_and_keydir 重试路径**：原始 `ord` 在 keydir 竞争中落败（kAlreadyExists → roll_active →
      重试 `ord2`），caller 提交 `Add{ord2}` 但 `ord` 成空洞。重试成功后在 return 前提交
      `Skip{ord}`——**先于 caller 的 Add{ord2}**（队列 FIFO 保序 → applied_ord 单调递增）。
    - **未做**（P2 范畴）：per-库 `next_apply_ord` + `pending` map（P1 单 worker 到达序 == ord
      序，reorder buffer 退化为 pass-through；pending map + drain 逻辑在 P2 并行 map 下才有效）。
    - 验证：Release **454/454 ctest 通过**（452 existing + AT3 + AT4）；TSan 插桩跑
      thread_pool_test（12 例）+ crash_recovery_test（7 例）**全绿零 race**。
    - **已知预存 race**（非本轮引入）：`cask_docvalue_test` 的 `V35ConcurrentSearchDuringRebuild`
      在 `rebuild_hnsw()` 与 `search_vector()` 间报 race——V3.5 时代 issue，在 search_layer.cpp
      （本轮未触碰），与本轮 Skip/ord-tracking 改动无关。
    - 风险：低（flush 谓词变化在 P1 等价；track_applied 在 dec_pending 前调用保可见性）。
  - [x] **S6-P2 Map 池并行（拿到 G1）** — `include/bitcask/thread_pool.hpp`、`src/cask/cask.cpp`、`src/search/search_layer.cpp`、`tests/thread_pool_test.cpp`
    - **已完成（2026-06-23）**：IndexPool 从单 worker 重构为 **dispatcher + reducer + TBB
      task_group 并行 map + per-库 reorder buffer**。`map_analyze` 在 TBB 线程并行执行；
      `reduce_apply`/`on_write`/`on_delete`/`rebuild_hnsw` 在 reducer 线程严格 ord 序串行 apply。
    - **架构**：
      - **Dispatcher** 线程：从 queue 弹任务 → Add-with-fields 走 TBB `map_group_.run()`（并行），
        其余（Skip/Delete/OnWrite/RebuildHnsw）直接构造 `ReorderEntry` 推入 reorder buffer。
      - **Reducer** 线程：在 `reorder_mu_` CV 上等 `next_apply_ord_` 到达 → `extract` → 解锁 →
        `reduce_fn_` apply → 更新 `applied_ord_` + `dec_pending` → 重锁继续 drain。
      - **Reorder buffer**：`std::map<ord, ReorderEntry>` + `got_sentinel_` flag（mutex 保护）。
        Sentinel 是 flag 而非 map 条目（Oracle 修复）。
    - **新 API**：`start(MapFn, ReduceFn, ErrorFn)` 替代旧 `start(Consumer)`。
      `MapFn = function<ReduceEntry(const IndexTask&)>`（TBB 并行调用，const 线程安全）。
      `ReduceFn = function<void(ReorderEntry&)>`（reducer 串行调用，std::visit 分发）。
    - **Oracle 修复全部落地**：
      1. RebuildHnsw 携带 ord（merge 路径 `alloc_ord()`，参与 reorder buffer）
      2. TBB map lambda：try/catch 包裹 `map_fn_`，异常时推空 ReduceEntry 填 ord 穴洞
      3. Reducer apply：try/catch 包裹 `reduce_fn_`，异常仍 `dec_pending`（不挂 flush）
      4. Sentinel 是 flag（`got_sentinel_`），不是 ReorderEntry 变体
      5. `reduce_apply` 加 early-return guard（空 job = 异常恢复 no-op）
    - **解决的关键难题**：
      - TBB `task_group` 存储 lambda 为 const → 不能用 `mutable` lambda + `this` 捕获；
        改为捕获裸指针（`&map_fn_`/`&reduce_fn_` 等）+ 局部变量。
      - Dispatcher 启动竞态：`stopped_` 可能在 dispatcher 首次调度前被 `stop()` 设 true →
        dispatcher 不 pop Sentinel → reducer 永久等 `got_sentinel_`。修复：dispatcher_loop
        不在循环顶检查 `stopped_`，而是靠 Sentinel 驱动退出。
      - RebuildHnsw 在新 Cask 实例中 ord=75 但 `next_apply_ord_=0`：reducer 加
        「buffer 非空但 `count(next)==0` → 跳到 `begin()->first`」逻辑（恢复后 ord 追赶）。
      - Backpressure 测试语义变化：dispatcher 快速排空 queue 到 reorder buffer，
        queue 不再因 reducer 阻塞而满 → 测试改为验证 queue 有界容量本身。
    - **新增测试**（6 例）：PipelineProcessesAllTaskTypes、AddWithFieldsGoesThroughMap、
      MapExceptionDoesNotStall、ReduceExceptionDoesNotStall、ReducerAppliesInOrdOrder、
      RebuildHnswCarriesOrd。
    - 验证：Release **461/461 ctest 通过**（454 existing + 7 new）。AT1 隐式覆盖（全量
      search/merge/checkpoint 集成测试通过 = pipeline 字节等价）；AT2 隐式覆盖（现有
      put→delete→put→flush→search 模式通过 = 墓碑不复活）。
    - **TSan 未完成**：oneTBB submodule 未初始化（网络受限），无法构建插桩 TBB。
      Oracle 已验证内存序正确性（release/acquire 链）；Release 全量通过。
      （后注 2026-06-24：oneTBB 就绪后已补 TSan，见 P3/P4。）
    - **⚠️ 后注（2026-06-24）：本条「拿到 G1」表述不准**。`parallel_for(0,1)` 阻塞 dispatcher
      使 map 实为**串行**（探针实测 max 并发=1），P2 只拿到 pipeline 并行而非数据并行。
      真正的多核 analyze 并行在 **P4** 用 std::thread map worker 池达成（实测 5.9×）。详见 P4。
    - 风险：中（并行 + 多线程 pipeline；解决多个竞态 + 死锁场景）。
  - [x] **S6-P3 池全局共享化（拿到 G2）** — `include/bitcask/thread_pool.hpp`、`include/bitcask/keydir_registry.hpp`、`src/keydir/keydir_registry.cpp`、`include/bitcask/cask.hpp`、`src/cask/cask.cpp`、`tests/thread_pool_test.cpp`
    - **已完成（2026-06-24）**：每库一池 → **registry 共享单池 + per-`LibId` 车道（lane）**。
    - **IndexPool 多 lib 化**：抽出 `IndexLane`（per-库回调 `map_fn/reduce_fn/error_fn` +
      reorder buffer `pending`/`next_apply_ord` + 水位 `submitted_ord_hwm/applied_ord/in_flight`）。
      **dispatcher + reducer 全局共享**（P4 后 dispatcher → N 个 map worker，见 P4）：按
      `task.lane` 路由（Add-with-fields → 并行 map → 该 lane 的 pending；其余直推）；reducer
      **扫描所有 lane**，对每条 lane 按其 `next_apply_ord` 串行 apply（库内 I2/I3，库间无队头
      阻塞）。新 API `register_lib(map,
      reduce,error,init_ord)→IndexLane*` / `unregister_lib` / `submit(lane,task)` / `flush(lane)`；
      保留单 lane facade（`start/submit/flush/applied_ord/...`）零改动兼容既有 12 例测试。
    - **lane 生命周期（UAF 防护）**：`lanes_` 持 `shared_ptr<IndexLane>`，reducer 在 unlock
      apply 前拷一份 shared_ptr 续命；`unregister_lib` 先 `flush`（保证 `in_flight==0` ⇒ 队列/
      reorder 无引用本 lane 的任务）再从 `lanes_` 移除。任务里的裸 `lane*` 由 in_flight 计数守护。
    - **registry 持有池（D2）**：`KeyDirRegistry` 懒创建 `unique_ptr<IndexPool>`（前置声明 +
      `.cpp` out-of-line dtor，避免头依赖 search_layer/TBB），dtor 停池。同 registry 所有 search
      库共用一对线程。
    - **Cask 接共享池**：去掉自有 `unique_ptr<IndexPool>` → 借用 `registry_->index_pool()` +
      车道句柄 `index_lane_`。open 注册 lane（起始 ord = `peek_next_ord`）；close `unregister_lib`
      （不停池）；open 失败经 `~Cask→close` 注销回滚。merge/checkpoint 三处 flush 改 `flush(lane)`。
    - **AT5 测试（3 例）**：`ThreadCountIndependentOfLibCount`（首注册起 2 线程，再注册 49 库零新增
      → G2 结构性证明，/proc/self/task 计数）、`LanesApplyIndependentlyInOrdOrder`（4 库 × 4 producer
      交错写，每库严格 ord 升序、互不串扰）、`UnregisterOneLibKeepsOthersRunning`（注销一库后池仍
      服务其它库）。
    - 验证：Release **464/464 ctest 通过**（461 + 3 AT5）。**TSan 全绿零 race**：thread_pool
      22 例（含 AT5 多 lib 并发）+ crash_recovery 7 + search_layer 31。oneTBB submodule 已就绪，
      补上 P2 当时无法跑的 TSan 插桩。
    - 风险：中（共享池 + 多线程 lane 路由）。缓释：facade 保旧 API 等价；shared_ptr 守 lane 生命
      期；flush 守 in_flight 防裸指针悬空；全量 + TSan 对拍。
  - [x] **S6-P4 真并行 map + 背压调优 + bench** — `include/bitcask/thread_pool.hpp`、`src/keydir/keydir_registry.cpp`、`bench/index_pool_bench.cpp`、`tests/thread_pool_test.cpp`
    - **已完成（2026-06-24）**。
    - **⚠️ 关键修正：P2/P3 的 map 实为串行，G1 未真正达成**。探针实测（map_fn 含 5ms
      负载，200 文档，6 核）：**P2/P3 max 并发 map = 1，总耗时 ≈ 串行**。根因：P2 的
      `tbb::parallel_for(0,1,…)+isolate`（为绕 task_group 的 TSan small_object_pool
      thread_data 坑而选）**阻塞单 dispatcher**——每文档 map 完成才处理下一条，无数据并行；
      P3 单 dispatcher 进一步把所有库的 map 串行化。P2 的「G1 达成」实为 **pipeline 并行
      （map∥reduce∥put）**，非数据并行。
    - **真并行 map worker 池**：去掉 dispatcher + `parallel_for(0,1)`，改 **N 个 std::thread
      map worker**（N=`hardware_concurrency`，registry 池）从 queue **并发**拉任务跑
      `map_analyze`，乱序结果入 per-lane reorder buffer，reducer 按 ord 序 apply。worker 是
      普通 std::thread（不碰 TBB task_group → 规避 P2 的坑）。`map_analyze` 纯函数线程安全
      （F7，P0 的 recover_doc_batch `parallel_for` 已验证）。RebuildHnsw 屏障**移除**——
      reducer 的 ord 序天然保证它在所有前序 map apply 后才执行。
    - **多核加速实测（探针 + bench）**：6 核上 **max 并发 map = 6，5.9× wall-clock 加速**
      （1042ms→176ms）。bench `BM_IndexPool_MapSpeedup`：1→8 worker **48k→181k tasks/s
      （3.7×）**（含 CPU 负载的模拟 analyze；过 4 后次线性因 6 核 + 单 reducer + sink 争用）。
    - **reorder 背压上限（D4）**：全局 `reorder_inflight_` 计数 + `reorder_cap`（默认 16384）。
      map worker push 前等 `inflight<cap`（否则停 pop → queue 满 → put 阻塞），reducer
      apply 后 `--inflight` 唤醒 worker；`stopped_` 旁路谓词防 shutdown 与 sentinel 死锁。
      shutdown：每 worker 一个 sentinel（FIFO 排真任务后，均衡 pop 退出），全 join 后才置
      `got_sentinel_` 通知 reducer 收尾。
    - **bench**（3 个）：`SubmitDrain`（纯流水线开销）、`MapSpeedup`（多核加速比，UseRealTime）、
      `MultiLibThroughput`（共享池多 lib 并发，吞吐随库数恒定 ≈ 池饱和 → 印证 G2）。
    - **基线存档（2026-06-24，Release+LTO+`-march=native`，6 核 / 24 MiB L3）**：

      | 度量 | 1 worker | 2 | 4 | 8 | 说明 |
      |---|---|---|---|---|---|
      | `MapSpeedup` (tasks/s) | 97k | 256k | 452k | 533k | **5.5× 加速**（4w 已 4.6×；过 6 受核数+单 reducer 限） |

      | 度量 | 1 lib | 2 | 4 | 8 |
      |---|---|---|---|---|
      | `MultiLibThroughput` (tasks/s) | 548k | 534k | 567k | 448k | 随库数**恒定**（池 6w 饱和）→ G2 |

      - `SubmitDrain`（no-op map/reduce 纯流水线开销）：**2.54M tasks/s**（远高于 analyze 成本，非瓶颈）。
      - 真实 analyze 成本（被并行化的工作，独立测）：Latin 1K **7.3µs**、Mixed 1K 9.8µs、
        **CJK ngram 1K 21.3µs**、Mixed ngram 20µs/doc。
      - **真实世界推算**：单线程 CJK 索引 ≈ 1/21µs ≈ **47k docs/s**（analyze 锁死）→ P4 并行后
        ≈ **5.5× ≈ 260k docs/s**。正是 S6 要解的「热点库吞吐被单 worker 锁死」。
      - KV 基线对照（无索引）：Put 覆盖 948k/s、Get 热点 1.19M/s、DocValue Get 6.57M/s。
      - 复跑：`cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release -DBITCASK_BUILD_BENCHMARKS=ON
        -DBITCASK_NATIVE=ON && cmake --build build-rel --target bitcask_bench` →
        `build-rel/bench/bitcask_bench --benchmark_filter=IndexPool`。
      - 注：`MapSpeedup` 用 `simulated_analyze`（CPU 负载代理）隔离调度开销；真实 analyze 成本
        见上独立基准，两者结合即真实世界并行化收益。
    - **新增测试**：`ReorderBackpressureBoundsMemoryThenDrains`（AT6：reducer 卡死 → 背压挡住
      producer（在途有界）→ 释放后零丢失全部 apply）。AT8 由现有 crash_recovery 套件覆盖。
    - 验证：Release **465/465 ctest**（464 + AT6）。**TSan 全绿零 race**（N 并行 worker 下）：
      thread_pool 23（含 AT5/AT6）+ crash_recovery 7 + search_layer 31 + cask_docvalue 62。
    - 风险：中（并行 map worker 池 + 背压）。缓释：std::thread worker 避 TBB 坑；shared_ptr
      守 lane；背压 cap 防 OOM；全量 + 4 套 TSan 对拍。

  - **风险**：中（改索引消费核心 + 并发）。缓释：Phase 0/1 行为等价于现状可安全停步；
    每 Phase 独立 TSan + 字节等价对拍；策略 A 不动 index 核心语义。

- [ ] **S7 查询内并行（Search Pool）** — `src/bm25/inverted.cpp`、`src/vector/hnsw.cpp`、`bench/inverted_bench.cpp`
  - 即 `thread_pool.hpp:14` 注释的「Search Pool（T6 阶段）」/ 设计稿待启用项。**注意**：与
    第八梯队的测试任务「T6 thread_local encoded 并发测试」（line 417，已完成，无关）**不是同一个
    T6**——本条是查询侧并行。
  - **背景**：S6 解的是**写/索引**侧并行（map worker 池）。读/查询侧是另一回事。评估实测（2026-06-24）：
    - **现状盘点**：单条查询基本**串行**——WAND（DAAT 顺序依赖）、短语、HNSW（图遍历）、布尔
      intersect 均串行；唯 wildcard 词表扫描按 shard 并行（保留）。多条查询**并发安全**（读路径无锁
      + `shared_mutex`），吞吐靠 inter-query 并发扩展。
    - **各查询延迟基线**（Release+native，6 核，100k 规模）：短语 **8.7ms** 🔥、3-term 布尔 904µs、
      HNSW k=256 **503µs**、热词 WAND 186–351µs。
  - [x] **S7-1 BOW 评分串行化（撤过度并行）** — `src/bm25/inverted.cpp`、`bench/inverted_bench.cpp`
    - **已完成（2026-06-24）**。原 `score_bow_topk` 用 `tbb::parallel_reduce`（grainsize=1）按词
      分片并行。**实测证明净亏**：BOW 路径按定义只在 `total_postings < kWandThreshold(1024)` 走
      → 评分工作量恒小，TBB task spawn/steal/join 开销远超收益。
    - **测量方法**：临时 `BITCASK_BM25_GRAIN` env 开关（grainsize 1=并行 / ∞=串行）+ 新基准
      `BM_Inverted_QueryThroughputBOW`（`ThreadRange` 扫读并发，8 词×120 posting=960 总 → BOW）。
    - **数据（聚合 QPS，并行→串行）**：1 线程 70→**114k（+62%）**、2 线程 45→**116k（+156%）**、
      4 线程 69→**113k（+64%）**、8 线程 42→**76k（+80%）**、16 线程 33→**45k（+37%）**。
      **每个读并发级别都涨**；单线程零竞争都快 1.6× = 纯 task 开销 > 小查询收益（冒烟枪）。
    - 落地：`parallel_reduce` → 直接串行循环（连框架开销一并去掉）；移除 env 脚手架；保留
      `BM_Inverted_QueryThroughputBOW` 作回归基准。附带：评分浮点累加序变**确定**。
    - 验证：**465/465 ctest** + TSan（inverted 77、search_layer 31）零 race + 修改文件零告警。
  - [x] **S7-2 进程级共享有界 Search 池（`search_arena()`）** — `src/search/search_layer.cpp`
    - **已落地（2026-06-24）**：进程级**共享** `tbb::task_arena`（**非每 Cask 一个**，与 S6 索引池
      registry 共享同思路）。并发上限由 TBB market 封顶（≈hardware_concurrency），与索引/恢复期
      TBB 工作隔离。故意泄漏（never-destroyed）规避静态析构 × `TbbLifetime::finalize` 顺序坑。
    - 用途定位 = **inter-query 并发**（见 S7-4），非单查询两路 fan-out。注释里「N threads
      **unbounded**」是错模型——已改为有界 market-capped。当前 `[[maybe_unused]]`，待 S7-4 接入。
  - [x] **S7-3 hybrid 两路：实测 → 定为串行** — `src/search/search_layer.cpp`（`search_hybrid`）
    - **已完成（2026-06-24）**。曾实现「两路（BM25 文本 + HNSW 向量）在 `search_arena()` 内
      `parallel_invoke` 并行 + RRF 合并」，但**实测盲目并行常见情形不赢**，已**撤回串行**：
      - **缓存命中**（生产 cache 开的常态）：text 路 ≈0 → 并行白付 worker 唤醒 ~10–13µs →
        **0.66× 变慢**；**两路常严重不对称** → 并行≈max≈大路，无收益。
      - **盈亏平衡**（合成均衡两路实测）：worker 热 ~1µs 开销（每路 ≳5µs 就赢）；worker 冷
        （低 QPS 间隔，futex 唤醒）~10–13µs（每路 ≳20–25µs 才赢）。
      - **甜区**（结合真实 leg 成本：vec 137µs@100k/k64、503µs@k256；text 未命中 186–351µs@100k）：
        **≥10 万文档 + 未命中缓存 + 两路同数量级** → 并行 **~1.5–1.8×**。规模小 / 缓存命中 / 不对称
        → 不值甚至负。
      - **决策**：单查询两路**保持串行**（零开销，对常见情形最优）。`search_arena()` 保留作
        inter-query 用（S7-4）。intra-query 两路并行留作甜区的自适应优化（peek 缓存 + vec 规模门控）。
    - 验证：465/465 ctest + search_layer/hybrid 测试通过 + 修改文件零告警。
  - [x] **S7-4 多查询并发入口（inter-query）+ Cask 批量查询** — `include/bitcask/search_layer.hpp`、`src/search/search_layer.cpp`、`include/bitcask/cask.hpp`、`src/cask/cask.cpp`、`tests/cask_docvalue_test.cpp`
    - **已完成（2026-06-24）**。线程池**稳赚**的用途落地：多条**独立**查询并发跑在 `search_arena()`
      上（每条是完整重单元，总功/核数，无单查询两路并行的均衡/唤醒摊销问题）。即「接口级并行
      查询、不要一个 Cask 一个线程」。
    - **池原语**（`bitcask::search::parallel_for_queries`，非模板，driven by `std::function`）：
      `n<=1` 直跑（零池开销快路径）；`n>=2` → `search_arena().execute([&]{ tbb::parallel_for(0,n,
      body); })`。grainsize=1 在此**正确**——每 item 是一条完整重查询（与 BOW 小 posting 不同）。
      此入口**激活**了 S7-2 的 `search_arena()`（去掉 `[[maybe_unused]]`）。
    - **Cask 批量入口**（3 个）：`search_text_batch(span<string_view>)`、
      `search_vector_batch(span<span<const float>>)`、`search_hybrid_batch(span<HybridQuery>)`
      → 均 `vector<expected<TextSearchResult>>`：保序、各槽独立错误、一次 `prepare_search`（flush）
      + 向量配置校验覆盖全批，并发体内只读 `search_`。
    - **并发安全验证**：原 `Cask::search_text` 注「线程安全:否」**实为保守**——`cache_`/`doc_texts_`
      各 `shared_mutex`、倒排/HNSW `shared_lock`、analyzer const（S6 已证并行 analyze 安全）。
      新增测试 4 例：text/vector/hybrid 批量各「批量 == 逐条 oracle」（text 含重复键压缓存写锁；
      hybrid 含单路退化）+ text 空/单条快路径。**TSan 零 race**（并发文本/向量/hybrid 查询 +
      并发 `cache_.put` 同键）。
    - 验证：**469/469 ctest**（465 + 4）+ TSan 零 race + 新增代码零告警。
    - 后续可选：跨 Cask 并发入口（`parallel_for_queries` 的 `fn` 闭包已支持多库——caller 直接用）。
  - [ ] **S7-5（甜区 intra-query，后置）并行化短语** — `search_phrase_impl` 候选文档循环（嵌
    `search_arena()`），候选数过阈才并行；末尾按 ord 稳定排序保确定性。预期砍 8.7ms。仅甜区做。
  - [ ] **S7-6（后置）HNSW 单查询并行** — 并行距离批算 / 多起点 ef-search。ROI 低 + 确定性/召回
    风险；先靠 inter-query（S7-4）吃满核。WAND 不碰（顺序依赖 + 已剪枝）。
  - **决策**：S7-1（BOW 串行）+ S7-2（共享池）+ S7-3（hybrid 串行）+ S7-4（inter-query 并发
    入口 + Cask 批量查询）**已落地**。单查询 intra-query 并行（S7-5/6）只对甜区做，且必经
    `search_arena()` + 自适应门控，绝不重蹈 grainsize=1 无脑拆分。

- [x] **S8 S6/S7 代码重构（质量收尾）** ✅ R1-R5 全完成（2026-06-24）— 6 准则：① C++ 最佳实践
  ② 高内聚低耦合/适当模式/合理继承 ③ 公共函数降冗余 ④ RAII/无泄露 ⑤ 加锁顺序/无死锁 ⑥ 完善中文注释
  - **scope**：仅 S6/S7 新建/重写代码（`thread_pool.hpp`、`search_layer.*`、`cask` 搜索路径、
    `keydir_registry`）。判断：**不强加继承/拆分**——`IndexPool` 高内聚（组合，靠单锁紧协调，
    拆分反增耦合 + 动 TSan-clean 并发核心风险高）；`LiveChecker` 已是合理虚基类。过度套模式
    违反准则①。
  - [x] **R1 Cask 批量搜索去重** — `search_*_batch` 三方法相同骨架抽 `run_search_batch(n,
    require_vector, run_one)`，各降为薄包装。✅（criteria 1/3/6）
  - [x] **R2 IndexPool 锁/RAII 不变量文档** — 类级注释固化「任一时刻最多持一把锁 ⇒ 无死锁」
    + 「线程必 join、lane shared_ptr 防 UAF」。审计确认健全。✅（criteria 4/5/6）
  - [x] **R3 单条 search 包装去重** — 9 个 `search_text/phrase/fields/near/bool/fuzzy/wildcard/
    vector/hybrid` 共享骨架 → 抽 `run_search_one(require_vector, err_kind, run)`，各降为 1 行
    返回。✅（criteria 1/3）
  - [x] **R4 池魔法数字 → 具名常量** — `kDefaultIndexQueueCapacity`(10240) /
    `kDefaultReorderInflightCap`(16384) `constexpr`；构造默认引用之，registry 改用默认（删
    显式 10240）。✅（criteria 1/6）
  - [x] **R5 注释收尾** — 重写 `thread_pool.hpp` 顶部文件级注释（原描述 P2 单 dispatcher +
    「Search Pool unbounded T6」全 stale）→ 准确描述 S6-P4 的 N map worker + reducer + registry
    共享 + 生命周期；修 Sentinel/entry 类型的 stale「dispatcher/TBB map」注释。✅（criteria 6）
  - 验证：每条 build + 全量 **469/469 ctest** + TSan（thread_pool 23 / batch 4）零 race +
    修改文件无新增告警，行为零变更（纯重构对拍）。

- [ ] **S9 全代码库重构评估（6 准则）** — 全代码库审计（S8 仅覆盖 S6/S7 新代码，本轮扩展到全部）
  - **审计方法（2026-06-24）**：3 个并行 explore agent 扫描全代码库（架构/类层级 + RAII/锁 +
    代码重复）。指标快照：总 21853 行，raw new/delete 仅 6 处（4 HNSW 锁-free 必需、1 task_arena
    故意泄漏），smart pointer 96 处，std::thread 16 处（全 join）。注释密度：cask 23%、keydir 26%
    （好）；inverted/hnsw 12%（中等）；**search_layer 仅 8%（需补）**。
  - **准则 5（锁/死锁）✅ 无问题**：IndexPool「单锁不变量」（S8-R2 已审计）、KeyDir 文档化锁序
    （`barrier → gate → meta → shard → fstats`）、HNSW 正确 spinlock 协议、SearchLayer/SearchCache
    标准 shared_mutex。全代码库无死锁风险、无线程泄漏。
  - **准则 2（内聚/耦合/继承/模式）✅ 总体健康**：无循环依赖（DAG：cask → keydir/fileops/merge/
    search → index/bm25/vec/text）；11 种设计模式正确使用（Template Method / Factory / Strategy /
    CoW / Atomic Swap / Pipeline / Registry / LRU / Barrier / Sharded / WAL）。**2 个 god class 待评估**
    （见 P2 项）。

  - [x] **P0-a FieldSchema FILE\* → RAII** — `include/bitcask/field_schema.hpp`
    - **已完成（2026-06-24）**：`std::FILE* fp_` → `unique_ptr<FILE, detail::FileCloser>`；删除手动析构
      fclose + open() 中途 fclose。detail::FileCloser 无状态 deleter（零额外开销）。
  - [x] **P0-b search_checkpoint fopen/fclose → RAII** — `include/bitcask/search_checkpoint.hpp`
    - **已完成（2026-06-24）**：write() 和 read() 的 `FILE*` → `unique_ptr<FILE, FileCloser>`；
      write 保留 `f.reset()` 在 rename 前显式关闭（平台正确性）；read 早退路径自动关闭。
  - [x] **P0-c kDefaultField 临时 string 消除** — `src/search/search_layer.cpp`
    - **已完成（2026-06-24）**：`fields_.find(std::string(kDefaultField))` × 2 → `fields_.find(kDefaultField)`
      （透明 hash 直传 string_view，零临时 string）；`fields_.emplace(std::string(kDefaultField), ...)` × 2 →
      `fields_.emplace(kDefaultField, ...)`。默认字段查询热路径每次省 1 次 SSO string 构造。
  - [x] **P0-d byte_order.hpp 提取共享 LE 工具** — 新建 `include/bitcask/byte_order.hpp`、`src/fileops/codec.cpp`、`src/keydir/keydir.cpp`
    - **已完成（2026-06-24）**：codec.cpp 匿名 namespace 的 `le_store/load_u16/32/64` 提取到
      `byte_order.hpp`；keydir.cpp `snap_put32/64` 从 `reinterpret_cast<uint8_t*>(&v)`（隐式依赖
      主机 LE）改为显式 `le_store_u32/64`（可移植 + DRY）。
  - 验证：Release **472/472 ctest**；TSan 零 race（checkpoint_recovery 5 + cask_docvalue 66 +
    keydir 9 = 80 例）。
  - [ ] **P1-a C API new/delete → unique_ptr** — `c_api/bitcask_c.cpp:222,231`
    - 裸 `new bitcask_impl_t` / 裸 `delete`；caller 遗漏 close → 泄露。改 unique_ptr + 自定义 deleter。
    - 风险：低。
  - [ ] **P1-b vbyte 编码去重** — `src/fileops/codec.cpp:58-64` + `include/bitcask/vbyte.hpp:27-33`
    - codec.cpp 匿名 `vbyte_append()` 与 vbyte.hpp `vbyte_encode()` 逻辑相同；合并到 vbyte.hpp。
    - 风险：低。
  - [ ] **P1-c search_layer.cpp 注释补充** — `src/search/search_layer.cpp`（1355 行，注释密度仅 8%）
    - 补 `map_analyze` / `reduce_apply` / `search_hybrid` RRF 融合 / checkpoint save/load 的算法说明。
    - 风险：零（纯文档）。
  - [ ] **P1-d thread_local buffer 工具类** — `src/fileops/data_file.cpp:225` + `src/fileops/hint_file.cpp:97`
    - 两处 `thread_local vector<byte>` + retain 限制 + resize 模式重复；抽 `ThreadLocalBuffer` 工具类。
    - 风险：中（改 I/O 路径，需全量测试）。
  - [ ] **P2-a Cask god class 拆分（search 方法抽 SearchOps）** — `include/bitcask/cask.hpp`（694 行,
    60+ 公有方法） + `src/cask/cask.cpp`（1993 行）
    - 职责过多：KV facade + search facade + merge 协调 + 迭代器 + 读缓存 + 索引池。
    - 建议：search_* 系列 15+ 方法抽到内部 `SearchOps` helper 类。Cask 保留 facade 角色。
    - 风险：高（大重构，动核心 API）。
  - [ ] **P2-b InvertedIndex god class 拆分（ScoringEngine + PostingManager）** — `src/bm25/inverted.cpp`（2049 行）
    - 职责过多：BM25 评分 + Block-Max WAND + posting 管理 + WAL + compaction + 5 种搜索模式。
    - 建议：抽 `ScoringEngine`（纯评分）+ `PostingManager`（posting 生命周期）。
    - 风险：高（核心算法路径，需 bench 对拍）。
  - [ ] **P2-c Analyzer 空文本基类默认实现** — `src/text/analyzer.cpp:167,271,312,358`
    - 4 个子类 `analyze_with_positions` 开头都 `if (text.empty()) return {};`；基类加默认实现。
    - 风险：低。
  - [ ] **P2-d search 层 SearchError 枚举** — `src/search/search_layer.cpp`（返回 `expected<..., string>`）
    vs `src/cask/cask.cpp`（返回 `expected<..., CaskFault>`）
    - search 层定义 `SearchError` 枚举，cask 边界翻译为 CaskFault。消除 leaky abstraction。
    - 风险：中（接口变更）。
  - [ ] **P2-e 魔法数字具名常量** — `src/bm25/inverted.cpp`（`0xFFFFFFFF` sentinel 3 处）+ 分散页大小常量
    - `0xFFFFFFFF` → `kInvalidPos`；`4096/65536/262144` → `format.hpp` 统一 `kPageSize4K` 等。
    - 风险：低。
  - **执行建议**：P0（4 项）风险最低、收益明确，优先实施。P1（4 项）次之。P2（5 项）含 god class
    拆分等高风险大重构，按需推进或永久搁置（现状可工作，god class 是风格问题非正确性问题）。

---

## 待办：小修小补（第七梯队，低成本、收益较小）

> 可穿插在任何阶段做。

- [~] **P1 `merge_policy::cap_size` 无条件分配 vector** — `src/merge/merge_policy.cpp:146-164`
  - **跳过（语义风险）**：`max_merge_size==0`/size 不匹配时返回 `{}` 会让「空=无 cap」
    与「空=无文件可 merge」语义混淆，需 caller 配合自查——为省一次（merge 决策频率，
    非热路径）vector 拷贝换语义歧义不划算。保留现状。
- [x] **P2 HintFile `kFlushBytes` 64KB → 1MB** — `include/bitcask/hint_file.hpp`
  - **已完成（2026-06-23）**：常量改 `1024*1024`。merge/active 写 hint 的 pwrite 次数
    16×↓。hint 非 WAL（可重建），加大缓冲只增大「崩溃丢 hint → fold(data) 回退」窗口，
    不影响正确性。Release 445/445 通过。
- [ ] **P3 `nfkc_fold` ASCII fast path 仍 std::string 拷贝** — `include/bitcask/text_utils.hpp`
  - 暂留：fast path 必须返回 owning string（要 tolower），拷贝固有；省拷贝需改 API
    （string_view + caller 保活 / in-place），lifetime 复杂、收益边际。低优先。
- [x] **P4 `to_codepoints` 必堆分配 vector** — `include/bitcask/text_utils.hpp`、`src/text/analyzer.cpp`
  - **已完成（2026-06-23）**：加出参版 `to_codepoints(text, out&)` + `to_codepoints_reuse()`
    （thread_local 复用 + 防膨胀 shrink 守卫，对齐 read_buf 策略）。Ngram/Whitespace
    分词 3 处热点改 `const auto& cps = to_codepoints_reuse(normalized)`——分词热路径
    稳态零 codepoint 分配。并发安全（thread_local 每线程独立），S3 并行 analyze 下
    **TSan 零 race**。Release 445/445（含 analyzer/jieba/stemming/docvalue）。
    jieba 的 to_codepoints（嵌套用法）未迁移，归 P5-P7 一并处理。
- [x] **P5 Jieba `jieba_cut` 多余 `std::string(sentence)` 拷贝** — `src/text/jieba_analyzer.cpp`
  - **已完成（2026-06-23）**：cppjieba `CutForSearch` 要 `const std::string&`（不接
    string_view），但缓冲改 `thread_local sentence` 复用 → 稳态零分配。
- [x] **P6 Jieba 输出词再走一次 NFKC + codepoint** — `src/text/jieba_analyzer.cpp`、`text_utils.hpp`
  - **已完成（2026-06-23）**：加 `nfkc_fold(input, out&)` + `to_codepoints(text, out&)`
    出参版；collect_tokens 的全文 `normalized`/`cps` 与逐词 `word_norm`/`word_cps`
    各用独立 `thread_local` 复用（**两组分开避免别名**）→ 逐词归一化/分码点稳态零分配。
- [x] **P7 Jieba 词位置搜索 O(n²)** — `src/text/jieba_analyzer.cpp`
  - **已完成（2026-06-23）**：>64 codepoint 时建「首码点 → cps 下标（升序）」倒排，
    逐词定位从 O(词数·cps长度) 线扫降到 O(候选位置数)；小文本仍 naive（免建表）。
    倒排按升序 push → 候选升序 → 仍取首次匹配，**语义不变**。
  - 验证：新增 `LongDocIndexedWordLocation`（96 cp 触发倒排，断言每 token byte 区间
    精确切出 term + 重复词取首次位置）；**已实证强制 use_index=true 全 13 例 jieba 测试
    通过**（倒排路径与 naive 在整语料一致）。Release 446/446 通过。
  - 关联 **P4 jieba 嵌套 to_codepoints** 一并迁移（见 P6）。

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
- [x] **T4 IndexPool 背压 / 关闭排空测试** — `tests/thread_pool_test.cpp`（并入既有，免 CMake 改动）
  - **已完成（2026-06-23）**：`BackpressureBlocksWhenQueueFull`（worker 卡住填满有界队列 →
    额外 submit 阻塞，释放后才完成）+ `FlushDrainsBackpressuredThenStopClean`（修正契约：
    **真实序是 flush()→stop()**，flush 等 pending 归 0 排空背压堆积；stop() 本身 abrupt——
    worker 循环顶查 stopped_，忙时即退不排空，索引可由 data 重建故可接受）。
    Release 全绿 + **TSan 零 race**。
- [x] **T5 `key_length_histogram` 测试** — `tests/keydir_test.cpp`（并入既有）
  - **已完成（2026-06-23）**：`KeyLengthHistogramBucketsAndSso`（8 桶各取下界+上界−1，
    校验边界归桶 + sso≤15/heap>15 计数）+ `...EmptyAndAfterRemove`（空→全零；墓碑不计入）。
- [x] **T6 `thread_local encoded` 并发测试** — `tests/crash_recovery_test.cpp`（并入既有）
  - **已完成（2026-06-23）**：`ThreadLocalEncodedBufferNoCrossThreadInterference`——8 线程
    各独立 Cask 并发 put 变长 value，重开逐值校验无串台/残留（若缓冲是 static 则数据竞争，
    若未正确 clear 则变长 value 读残留字节）。Release 全绿 + **TSan 零 race**。
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

- [x] **B1 Merge 吞吐 bench** — `bench/merge_bench.cpp`
  - **已完成（2026-06-23）**：`BM_Merge_Throughput`——两轮写（旧值→覆写）造一半死记录，
    Pause/ResumeTiming 仅计时 `merge()`，报 records/s + MB/s（度量 S2 批量 pwrite）。
    实测 20k 记录 merge ≈ 330k records/s、~33 MiB/s（本机 tmpfs）。
- [x] **B2 IndexPool 异步路径 bench** — `bench/index_pool_bench.cpp`
  - **已完成（2026-06-23）**：`BM_IndexPool_SubmitDrain`——no-op consumer 隔离队列/调度/
    flush 开销，度量 `make + submit + worker 消费 + flush(W3 cv)` 端到端吞吐。实测
    ~3M tasks/s。
  - **S6-P2 后修订（2026-06-24）**：P2 把单参 `start(Consumer)` API 换成三段
    `start(MapFn, ReduceFn, ErrorFn)`，本 bench 一度编译不过。已改用新 API：map_fn/
    reduce_fn/error_fn 均 no-op，提交 **Add-with-fields** 走 dispatcher + TBB 并行 map +
    reorder buffer + reducer 按 ord 序 apply + flush（pending 归 0 且 applied_ord 追上
    hwm）的双线程流水线端到端吞吐。**旧 `~3M tasks/s` 数值已废，待在新形态下重测**。
    同轮清掉 `thread_pool.hpp` 的 `-Wreorder`（构造列表冗余）+ `-Wmissing-field-initializers`
    （新增 `IndexTask::sentinel()` 工厂替代聚合初始化）两处告警。
- [x] **B3 大 keydir bench（>cache，1M key）** — 扩展 `bench/keydir_bench.cpp`
  - **已完成（2026-06-23）**：`BM_KeyDir_Get_Large`——1M key 随机 get（cache-cold），暴露
    tier-2 ⑤（ankerl::unordered_dense）的 cache-miss + 指针追逐成本，永久回归护栏。
    实测 ~260-290 ns/get（vs 1024-key hot 远快——故旧基准测不出该优化）。
- [x] **B4 Checkpoint 保存/加载 bench** — `bench/checkpoint_bench.cpp`
  - **已完成（2026-06-23）**：`BM_Checkpoint_KeydirSave`/`...Load`——度量 S4 keydir 快照
    序列化（精确 reserve）+ 反序列化重建。实测 100k key：save ~5 ms（~20M keys/s）、
    load ~10.6 ms（~9.5M keys/s）。（S5 zstd 未做 → 暂无压缩维度。）

> 全部并入单一 `bitcask_bench`（`-DBITCASK_BUILD_BENCHMARKS=ON` 构建）；bench 链接加
> `TBB::tbb`（IndexPool 模板实例化 tbb 队列）。**bench 不链 sanitizer**（perf 数无意义）。

### 构建加固（独立于优化，可任意时刻加）

- [x] **H1 栈保护 `-fstack-protector-strong`** — `CMakeLists.txt`
  - **已完成（2026-06-23）**：目录作用域 `add_compile_options(-fstack-protector-strong)`，
    全配置（含 Debug/sanitizer）启用。验证：Release migrate_le `__stack_chk` 符号在位。
- [x] **H2 `_FORTIFY_SOURCE=2`** — `CMakeLists.txt`
  - **已完成（2026-06-23）**：`$<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=2>`，
    **且仅当 `BITCASK_SANITIZE` 为空**（与 ASAN/TSan interceptor 冲突）。先 `-U` 防发行版
    预定义重定义告警。验证：Release flags.make 含 `_FORTIFY_SOURCE=2`；Debug（-O0）与
    TSan 构建均**正确缺省**。
- [x] **H3 Full RELRO `-Wl,-z,relro,-z,now`** — `CMakeLists.txt`
  - **已完成（2026-06-23）**：`add_link_options(-Wl,-z,relro,-z,now)`。原仅 partial RELRO
    （有 GNU_RELRO 段、无 BIND_NOW）→ 现 **Full**：Release migrate_le `FLAGS=BIND_NOW` +
    `FLAGS_1=NOW`。覆盖 .so + 可执行文件。
- [x] **H4 可执行文件 PIE `-pie`** — 已由顶部 `CMAKE_POSITION_INDEPENDENT_CODE ON`
  - **已满足（核实，无需改动）**：cmake≥3.20 → CMP0083=NEW，全局 PIC 令可执行文件
    自动 `-fPIE -pie`。migrate_le / gen_inert_table 实测 `Type: DYN` + `FLAGS_1: PIE`。
- [x] **H5 PCH（precompiled header）** — `CMakeLists.txt`
  - **已完成（2026-06-23）**：重型 TU（cask/search/keydir/bm25/text）各 PCH 一组 STL 公共头
    （algorithm/cstdint/expected/memory/optional/span/string/string_view/unordered_map/vector）。
    仅 STL 头入 PCH（恒可用、与各 TU 无冲突）。`-DBITCASK_PCH=OFF` 可关闭排查。
    验证：Release/Debug/TSan 三构建均通过，全量 451/451 ctest 绿。

> ✅ H1-H5 全部落地。新增加固：栈保护（strong，全配置）+ FORTIFY=2（优化非 san）+
> Full RELRO（BIND_NOW）；PIE 早已就位（全局 PIC）。sanitizer 构建按需排除 FORTIFY，
> 实测 Release/Debug/TSan 三向验证一致。

### CI 剩余

- [x] **CI2 Sanitizer matrix（ASAN+UBSAN / TSan）自动化验证** — `.github/workflows/ci.yml`
  - **已完成（2026-06-23）**：加固既有 matrix——`concurrency`（取消过期运行）、
    job `timeout-minutes`（30/60，防 TSan 挂死）、`hendrikmuhs/ccache-action`（按
    sanitizer 分桶缓存，摊薄插桩 oneTBB 重编）、确定性运行期选项（`ASAN/UBSAN/TSAN_OPTIONS`
    = halt_on_error + print_stacktrace；ASAN 含 detect_leaks）、ctest `-j` 并行。
  - **本地全量 TSan 验证**：build-tsan 全 target 重建 + `TSAN_OPTIONS=halt_on_error=1`
    跑全 451 ctest（详见验证小结）——确认 push 后 TSan job 应绿（X1 既有 UAF 已修，
    无残留 race）。
- [x] **CI3 Benchmark 追踪** — `.github/workflows/ci.yml`
  - **已完成（2026-06-23）**：新增 `benchmark` job——Release + `BITCASK_BUILD_BENCHMARKS=ON`
    构建 `bitcask_bench`，`--benchmark_format=json` 跑全部微基准（含 B1-B4），
    `actions/upload-artifact` 上传 `bench-results-<sha>.json`（保留 30 天）。
  - **非门控**：GitHub 共享 runner CPU 噪声大，自动 >10% 回归报警会频繁误报；artifact
    留作离线/趋势比对，自动回归检测维度留待专用稳定机（避免误报噪声淹没真信号）。

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
5. **X1 + T7/T8/T9** ← 正确性收尾（迭代器生命周期）✅ 完成（2026-06-23）
6. **S2 + S3 + S4** ← 结构性优化 ✅ 完成（2026-06-23）
7. **P1 - P7** 按需穿插
8. **T4 - T6、B1 - B4、H1 - H5、CI3** 长期推进
9. **S6 异步索引 MapReduce 流水线** ← Phase 0-4 ✅ **全部完成**（G1 多核并行 + G2 线程解耦达成）

> **关键决策点**：R1-R3 + W1-W3 + X1(+T7-T9) + S2 + S3 + S4 全部落地。
> 结构性优化剩余：
> - **S6（异步索引双池）：Phase 0-4 全部完成** ← 取代 S1，解热点库吞吐 + 库数线程膨胀两个
>   真问题。设计稿 `docs/design/async-index-pipeline.md` 已评审；Phase 0-4 全部落地：
>   - Phase 0（map_analyze/reduce_apply 拆分）✅
>   - Phase 1（reorder buffer 基础设施 + Skip + applied_ord 跟踪）✅
>   - Phase 2（dispatcher/reducer 双线程 pipeline）✅ ——但 `parallel_for(0,1)` 致 map 实为
>     串行，只拿到 pipeline 并行（**G1 当时未真达成**，P4 修正）
>   - Phase 3（registry 共享池 + per-LibId 车道，线程数与库数解耦）✅ **G2 达成**
>   - Phase 4（**N 个 std::thread map worker 真数据并行**，实测 5.9× → **G1 真达成**；reorder
>     背压上限防 OOM；多核加速比 + 多 lib 吞吐 bench）✅
> - ~~**S1**：已降级~~ → **被 S6 取代否决**。
> - **S5**（checkpoint zstd）：低风险但需**引入 zstd 第三方依赖**——待用户拍板是否
>   接受新依赖再做。

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

---

## 待办：第十梯队（S10 第三方审计 — 2026-06-24）

> 来源：3 个并行 agent（explore × 2 + librarian × 1）+ 直接审计 posix_file / merger /
> highlighter / synonym_map / search_cache / codec 等。已与 TASK.md 全部既有项交叉去重。
> 总计 22 项，按 ROI/风险分 4 梯队。建议优先实施 A 梯队（5 项，~3-4 天可全部落地）。

### A 梯队：高 ROI 低风险（优先）

- [x] **A1 `search_text` / `phrase` / `bool` 缓存检查在 `analyze()` 之后** — `src/search/search_layer.cpp:545-556` 等
  - **已完成（2026-06-24）**：三个查询入口（`search_text` / `search_phrase` / `bool_search`）
    均改为缓存前置——`query.empty()` 早退 → 构造 `cache_key` → `cache_.get()` → 命中直接
    用 cached results / 未命中才 `analyze` 或 `parse_query`。
  - **正确性分析（Oracle 验证）**：
    - `prepare_search() → flush_index()` 保证搜索开始前所有**先前**写入的失效已完成。
    - 并发写入期间（设计允许，concurrency-zh §6）存在 ~20µs TOCTOU 窗口——hit 路径
      返回的拷贝可能含并发删除的 doc。Oracle 确认这是 near-real-time 契约的**程度**
      调整（窗口从 ~1µs 拓宽到 ~21µs），非**类别**变更。已在 `search_cache.hpp` 头注释
      明示。
    - `on_delete` / `reduce_apply` / `on_write` 的失效逻辑不动；`invalidate_terms` 的
      交集判定无漏洞（Oracle 验证）。
  - **测试**：新增 3 例护栏：
    - `CacheHitSkipsAnalyzer`：注入 `CountingAnalyzer`（test-only 注入构造函数），
      断言 hit 时 `analyze()` 调用次数不增加。
    - `CachePhraseHitSkipsAnalyzer`：短语查询同等护栏（`analyze_with_positions`）。
    - `CacheInvalidatedOnDeleteThenMissRecomputes`：契约验证——`on_delete` 后下次
      查询 miss 重算，不返回陈旧缓存。
  - **验证**：Release **472/472 ctest** 通过（原 469 + 3 新）；**TSan 零 race**
    （search_layer 34 例 + cask_docvalue batch/concurrent 12 例）。
  - **新公共 API**：`SearchLayer(const SearchLayerConfig&, unique_ptr<Analyzer>)`
    test-only 注入构造函数（delegating ctor，nullptr 退化为默认）。
  - **bench 量化**：`third_party/benchmark` submodule 网络受限，改写 ad-hoc 微基准
    `bench/a1_cache_bench.cpp`（不依赖 google benchmark，直接 `<chrono>` 计时）。
    **Release + LTO + `-march=native`，6 核**：

    | 场景 | hit (µs/q) | miss (µs/q) | A1 节省 |
    |---|---|---|---|
    | Latin ngram 短查询（~20 字符）| 0.18 | 2.25 | **2.06 µs/q** |
    | CJK ngram 短查询（~4 字符）| 0.17 | 2.43 | **2.27 µs/q** |
    | **CJK ngram 长查询（~200 字符）** | **0.21** | **414** | **🔥 414 µs/q** |

    命中率对整体延迟影响（Latin 短查询）：

    | hit ratio | avg µs/q | QPS | 相对 0% 提升 |
    |---|---|---|---|
    | 0% | 2.10 | 478k | — |
    | 50% | 1.13 | 884k | **+85%** |
    | 90% | 0.34 | 2.93M | **+513%** |

    **关键洞察**：
    - **短查询**：节省 ~2µs/q，高 QPS 系统下显著（100k QPS × 2µs = 20% CPU 节省）。
    - **长查询（甜区）**：CJK ngram 200 字符查询节省 **~414µs/q**——ngram 切分成本
      随字符数超线性增长，A1 让这类查询从 ~2.4k QPS 飙到 ~4.8M QPS（hit 时）。
    - **生产典型**（50% 命中率）：整体 QPS **+85%**。
    - 复跑：`g++ -std=c++23 -O3 -DNDEBUG -march=native -Iinclude \
      -Ithird_party/{unordered_dense,utf8proc,cppjieba,limonp}/include \
      bench/a1_cache_bench.cpp build/libbitcask.a \
      build/third_party/utf8proc/libutf8proc.a -ltbb -lz -lpthread -o /tmp/a1_bench && /tmp/a1_bench`
  - 风险：低（纯顺序调整 + test-only API 扩展）。

- [x] **A2 WAND 块上界 / list 上界每次重算 — 未缓存** — `src/bm25/inverted.cpp:531-533, 612-618`
  - **已完成（2026-06-24）**：`TermPostings` 加 `block_upper_bounds` 数组（per-query
    per-block 缓存），查询初始化阶段一次性算好所有 block 的 upper_bound；WAND 内层
    循环改为指针减法取 index + O(1) 读缓存。`list_upper_bound` 经核实**本就已缓存**
    （line 533），仅 `block_upper` 是每次 pivot 重算。
  - **⚠️ 实测收益 < 1%（噪声级别）**，远低于预估的 3-8%：
    - 5-term WAND (k=10): 121.13 → 121.67 µs/q（-0.4%，噪声）
    - 10-term WAND (k=10): 264.44 → 263.04 µs/q（+0.5%，噪声）
    - 5-term WAND (k=1):  116.27 → 116.79 µs/q（-0.4%，噪声）
    - 10-term WAND (k=1): 264.27 → 263.28 µs/q（+0.4%，噪声）
  - **收益不显著原因（事后分析）**：WAND 热点不在 `block_upper` 浮点计算（~5ns/次，
    6 FMA + 1 div），而在：① `snapshot_flat`（每 term 拷贝 ords/tfs/blocks 数组）；
    ② `fill_is_live`/`fill_doc_lens`（每 term 虚调用 + 批量填充）；③ 评分循环的 tf_norm
    除法（每 pivot_ord 一次）；④ cursor 排序（每 pivot 插入排序）。这些都是 µs 级，
    block_upper 的 5ns 是小头。
  - **决策：保留改动**。理由：① 代码正确（语义不变，消除冗余计算）；② 与 bool_search
    BMW 路径的 `c.block_ub[b]` 缓存模式统一；③ 极端场景（更频繁块跳跃）可能有边际收益；
    ④ 风险低（不改算法，仅缓存派生值）。
  - **测试**：无新增（既有 472 例覆盖 WAND 路径）。
  - **验证**：Release **472/472 ctest** + TSan 零 race（inverted 77 + search_layer 34）。
  - **bench**：`bench/a2_wand_blockmax_bench.cpp`（ad-hoc，5000 docs × 12 vocab，
    5/10-term × k=1/10 对比）。
  - **教训**：预估"3-8%"基于"1000 次冗余浮点除法"的算术，忽略了 block_upper 判定分支
    的实际触发频率相对其他热点偏低。未来类似优化应先 profile 再投入。

- [~] **A3 `search_vector` 每次构造 `std::function` 回调** — `src/search/search_layer.cpp:235-247`
  - **跳过（2026-06-24，二次分析后判定收益不足）**。详细拆解：
    - `std::function` 成本（构造 ~30-100ns + 间接调用 ~3-5ns/次）确认存在
    - 但 `live` 回调**仅在结果收集循环调用**（`hnsw.cpp:924`，O(ef) ≈ 几百次/查询），
      **不在图遍历热路径**（`greedy_closest`/`search_layer` 无 live 调用，TASK.md:677 已确认）
    - 总成本 ~1-2µs/查询，相对 search_vector 总耗时（137-503µs）**< 1.5%**
    - 真实热点是距离计算（dist_id × ef×M，~80%）+ 优先队列（~10%）
  - **预期与 A2 同病**：A2 预估 3-8% 实测 <1%（WAND 块上界非热点）；A3 的 std::function
    同样不是热点。**先做 A4（写入热路径，确定收益）**，A3 待 profile 证实再投入。

- [x] **A4 `reduce_apply` 字段名 `std::string` 拷进 `ord_field_lens_`** — `src/search/search_layer.cpp:446-451`、`include/bitcask/search_layer.hpp`
  - **已完成（2026-06-24）**：`ord_field_lens_` 值类型 `vector<pair<string,u32>>` →
    `vector<pair<string_view,u32>>`；新增 `intern_field_name(sv)` 把字段名首次 emplace 进
    `field_names_intern_`（`unordered_set<string,StringHash>`，node 稳定→string_view 安全），
    后续全命中返回稳定 string_view。双检锁（shared 查 / unique emplace）。`reduce_apply` 两处
    `emplace_back` 改用 intern；`on_delete` 消费端 `field_index(string_view)` 透明兼容无需改。
  - **实测（before/after 对比，Release+LTO+native，3 轮）**：
    - **短字段名（SSO ≤15B，典型）**：稳态 alloc/doc **139.0 → 139.0（零变化）**——SSO 本就
      不堆分配；bytes/doc 10257→10161（-96B，vector 元素缩小）。吞吐 ~4.9µs/doc 两版持平（噪声内）。
    - **长字段名（>15B，堆分配）**：稳态 alloc/doc **154.0 → 149.0（−5/doc）**——精确消除 5 字段名
      堆分配；bytes/doc 10590→10383（-207B）。吞吐 ~5.0µs/doc 持平（噪声内）。
    - **内存占用**：`ord_field_lens_` 元素 `pair<string,u32>`(40B) → `pair<string_view,u32>`(24B)，
      **−40%**。1M 文档×5 字段：200MB → 120MB。
  - **结论（诚实）**：**非吞吐优化**（<1%，analyze 主导的 5µs/doc 下 5 次小堆分配/SSO 拷贝可忽略；
    与 A2 同病——预估「高收益」基于「50MB 分配消除」算术，忽略了 SSO 对短名零堆分配的现实）。
    **保留**因：① 长字段名真实消除 5 alloc/doc（实测）② 侧表内存 −40% ③ intern 池是干净设计
    （与 FieldSchema 已有 intern 一致）④ 低风险（侧表、单写者、全量 472/472 + TSan 零 race）。
  - **教训**：优化前先验证「预估的分配是否真的走堆」——SSO 阈值（libstdc++ 15B）决定了短字符串
    名本就不分配。未来类似项先 `operator new` 计数器探针再投入。
  - 验证：Release **472/472 ctest**；TSan 零 race（cask_docvalue 66 + search_layer 34 +
    crash_recovery 7 + thread_pool 18 = 125 例）。bench：`bench/a4_field_intern_bench.cpp`（ad-hoc，
    全局 operator new 计数 + 短/长字段名 before/after 对比）。

- [x] **A5 `put_doc` 的 `task_fields()` lambda 拷贝所有字段名+值为新 string** — `src/cask/cask.cpp`、`include/bitcask/thread_pool.hpp`、`include/bitcask/search_layer.hpp`、`src/search/search_layer.cpp`
  - **已完成（2026-06-24）**。`IndexTask::fields` 从 `vector<pair<string,string>>` 改为
    `vector<pair<string_view,string_view>>`，字段名+值打包进新增 `fields_store`（`vector<char>`，
    **一次分配**替代 N×2 次 string 拷贝）。`make()` 去掉 `fields_` 参数（caller 构造后直设
    fields_store+fields，同 vec/meta 模式）。`map_analyze` 签名改收 `pair<string_view,string_view>`；
    `on_write_fields` 外部签名不变（内部转换 pair<string,string>→pair<sv,sv>，无堆分配）。
  - **生命周期安全**：`fields_store` 是 `vector<char>`（无 SSO），move 必为指针转移 → string_view
    跨 IndexTask 多次移动（入队/出队）始终有效。同步路径（on_write_fields/recover_doc*）的 views
    借自 caller 的 string / 局部 string_view，调用期间有效。
  - **实测（before/after operator new 计数，Release+LTO+native，5 字段/文档）**：
    - alloc/put: 101.8 → **95.3**（**−6.5, −6.4%**）—— 精确消除 5 value string 拷贝 + vector，
      净增 2 alloc（pack buffer + views vector）。
    - bytes/put: 8145 → **7086**（**−1059, −13%**）—— 堆字节显著降（省去 N 个 string 对象开销）。
    - 字段名（SSO ≤15B）本就不堆分配（A4 已验），收益主要来自字段值（>15B 文档文本）。
  - **改动面**：4 生产文件 + 2 测试/bench 文件（加 `mk_fields_task` helper 替代旧 10 参数 make）。
    on_write_fields/recover_doc* 内部改构造 `pair<string_view,string_view>`，无 API 变更。
  - 验证：Release **472/472 ctest**；TSan 零 race（cask_docvalue 66 + thread_pool 22 +
    search_layer 34 + crash_recovery 7 = 129 例）。

### B 梯队：中 ROI 低/中风险（次轮）

- [x] **B1 `SynonymMap::expand` 返回 `vector<string>` by value** — `include/bitcask/synonym_map.hpp`、`src/search/search_layer.cpp`、`tests/synonym_test.cpp`
  - **已完成（2026-06-24）**：`expand()` 返回 `span<const string>` 借内部 map（零分配）；
    空 span = 无同义词。`expand_terms` 显式处理空 span（fallback 原词）。`search_layer.cpp`
    search_fields 路径的 `expand(t)` 改 span + 空 span fallback。测试同步更新。
  - 验证：472/472 ctest + TSan（synonym 11 例零 race）。

- [ ] **B2 `search_text` 的 `terms` 拷贝 + synonym 再拷** — `src/search/search_layer.cpp:562-568`
  - **现状**：`analyzer_->analyze()` 返回 `unordered_map<string,...>`（map 拥有 string），
    接着 `terms.push_back(term)` 又拷一遍 keys；synonym_map 存在则 `expand_terms(terms)`
    返回新 vector 再拷。
  - **修法**：`InvertedIndex::search` 接 `span<const string_view>`；analyzer 增
    `analyze_terms()` 直接返回 `vector<string_view>`（借内部存储）。`search_phrase`/
    `near`/`fuzzy`/`bool` 同模式（5 处重复，可抽 helper）。
  - **收益**：每次查询省 `num_terms × avg_term_len` 拷贝。风险：中（动 InvertedIndex 接口）。

- [~] **B3 `doc_vector_f32` 总是返回 owning `vector<float>`** — **跳过（收益边际）**
  - 分析（2026-06-24）：cask.cpp:901（recovery 路径）的 `rd.vector = doc_vector_f32(*dv)` 已
    被 NRVO 优化（直接构造进 rd.vector，无中间拷贝）。`_into` + thread_local 在此路径无
    省分配（rd 需拥有数据）。cask.cpp:1393 是一次性初始化。**无可省分配**。

- [ ] **B4 `on_delete` 重新跑完整 analyze 仅为失效缓存** — `src/search/search_layer.cpp:497-502`
  - **现状**：`auto tf = analyzer_->analyze(*text)` 完整 NFKC + ngram + fold，
    纯为建 `changed_terms` 给 `cache_.invalidate_terms()` 用。写入时已分过词。
  - **修法**：`doc_texts_` LRU 条目同时存 `vector<string>` term 集（或 `vector<u64>`
    hashed terms），删除时直接取。
  - **收益**：每次删除省一次完整 NLP（CJK ~20µs/doc）。代价：每缓存条目多占
    `num_terms × avg_term_len` 字节。风险：中（多占内存）。

- [x] **B5 HNSW `search_layer` 每次 stack 构造两个 `priority_queue`** — `src/vector/hnsw.cpp`
  - **已完成（2026-06-24）**：`ReusablePQ`（继承 `priority_queue` 暴露 protected `Container c`）
    + `thread_local vector<Cand>` 底层 buffer。函数入口 clear（保容量）→ move 构造 queue →
    函数尾 `extract()` 回收。f32 + int8 两路均改。每次向量查询稳态零堆分配（cands + top）。
  - 验证：472/472 ctest + TSan（hnsw 14 例零 race）。

- [x] **B6 `merger` 的 `pending_` 不 reserve** — `src/merge/merger.cpp`
  - **已完成（2026-06-24）**：扫输入文件 sizes 估算 record 数（`file_size / 64` 粗估），
    `pending_.reserve(est)`。`file_size` 失败（ec 非零）时跳过该文件。大 merge 省 ~log(N) realloc。
  - 验证：472/472 ctest + TSan（merge_concurrent 3 例零 race）。

### C 梯队：算法 / SOTA（中 ROI 中风险，需 bench）

- [x] **C1 `select_best_fragments` 是 O(F·R²)** — `src/search/highlighter.cpp`
  - **已完成（2026-06-24）**：内层 O(R) 线性扫描 → `lower_bound` 二分搜索 O(log R)。
    `remaining_ranges` 已按 start 排序（`collect_query_ranges` 保证）→ 每轮 O(R log R)
    替代 O(R²)。F=5 R=200 时 200K → ~8K 次比较。
  - 验证：472/472 ctest + TSan 零 race（search_layer 34 + docvalue 66）。

- [x] **C2 `SynonymMap::add_group` 是 O(n²)** — `include/bitcask/synonym_map.hpp`
  - **已完成（2026-06-24）**：`unordered_map<string, vector<string>>` →
    `unordered_map<string, shared_ptr<const vector<string>>>`。add_group 排序去重 +
    set_union 合并 → 所有 term 指向同一 shared_ptr（零 vector 拷贝）。
  - **实测**：N=1000 add_group 5240µs → **116µs（45× 加速）**；per-term 5.2→0.1µs（O(n²)→O(n log n)）。
  - 验证：472/472 ctest + TSan 零 race（synonym 11）。

- [~] **C3 BM25 BOW 整 vector 排序** — **跳过（实测 hash-aggregate 更慢）**
  - **评估（2026-06-24）**：micro-bench 对比 sort+merge+heap vs hash-aggregate+nth_element。
    BOW 范围（< 1024 hits）：hash-aggregate **慢 25-40%**——sort 在 cache-resident 数据上
    极快（960 hits 仅 3.3µs），hash map 的 hashing/probing 开销不划算。**sort+merge+heap
    已是该规模最优**，保留原实现。inverted.cpp 注释登记评估结论防重复尝试。

- [ ] **C4 SOTA：Block-Max MaxScore（Lucene 9.9 自适应合取）** — `src/bm25/inverted.cpp`（WAND 路径）
  - Lucene 9.9 (2023)：term 按 max_score 排序，随 min-competitive-score 上升把
    非本质子句转合取评估。
  - **来源**：Elasticsearch Labs MAXSCORE 博客；Lucene story paper PMC7148045。
  - **收益**：高频多子句查询 6-11%（Lucene nightly bench）。
  - **风险**：中-高（核心评分路径重构）。建议 A2 落地后再评估。

- [ ] **C5 SOTA：SIMD Posting 压缩（FastPFOR / SIMD-BP128）** — `src/bm25/inverted.cpp` + `inverted.hpp`
  - posting list 未压缩存 `vector<Posting>`。Lemire SIMD-BP128⋆ ~0.7 cycles/int。
  - **来源**：Lemire et al. SIMD Compression (2016)；ClickHouse 已采用。
  - **风险**：高（新编码 + 兼容老 checkpoint）。建议作为新存储格式 v4 一部分。

- [ ] **C6 SOTA：Roaring Bitmap 用于 filter / posting** — `src/bm25/`、`include/bitcask/meta_filter.hpp`
  - 当前 filter 用 `MetaFilter::evaluate(blob)`；posting 用 vector。dense block + rank
    优化可加速多字段 AND/OR 与 bool_search。
  - **来源**：ES / Weaviate / Quickwit 均采用。
  - **风险**：中（新依赖或自实现）。建议 bool_search 成为瓶颈时引入。

### D 梯队：清理与小幅优化（按需穿插）

- [ ] **D1 HNSW `search_layer` 顶层 `out.resize` 跨层 churn** — `src/vector/hnsw.cpp:461-466`
  - `out.clear() + resize` 跨层可能 realloc。`out.reserve(ef)` 一次即可。风险：零。
- [ ] **D2 `search_phrase`/`near`/`bool`/`fuzzy` 的内层重复模式抽 helper** — 多处
  - "ordered terms 构建" + "results → hits materialize" 5+ 处重复。S8-R3 只动了外层。
    纯代码质量。
- [ ] **D3 `mmap` 的 read 文件加 `madvise(MADV_RANDOM)`** — `src/fileops/data_file.cpp:54-59`
  - random access 模式 hint，减少内核 readahead 浪费。风险：零。
- [ ] **D4 `.so` 链接加 `-fno-semantic-interposition` + `-fvisibility=hidden`** — `CMakeLists.txt`
  - C++ 内部符号 inline 潜力释放（C API 已 `extern "C"`）。~3-5%。需审计导出表。风险：低。
- [ ] **D5 `PosixFile::pread` / `read` 每次分配 `vector<byte>`** — `src/io/posix_file.cpp:70, 108`
  - 已有 `pread_into`，但旧 API 还在。grep 确认无热路径调用，逐步迁移。风险：低。
- [ ] **D6 `select_neighbors` 中 `vec_of(pid)` 反复取** — `src/vector/hnsw.cpp:577-610`
  - M=16 ef=200 = 3200 次冗余向量取指。picked 列表存 `{d, id, vec_ptr}` 或预取。风险：低。

### S10 执行进度（2026-06-24）

**A 梯队 — 全部完成**：A1 ✅（缓存前置）/ A2 ✅（WAND 块上界 <1% 保留）/ A3 ⏭️ 跳过（std::function 非热点）/ A4 ✅（字段名 intern，内存 −40%）/ A5 ✅（字段打包，alloc −6.4%）。

**B 梯队 — 低风险项完成**：B1 ✅（SynonymMap span）/ B5 ✅（HNSW PQ 复用）/ B6 ✅（merger reserve）/ B3 ⏭️ 跳过（NRVO 已优化）。**B2/B4 保留**（中风险：B2 动 InvertedIndex 接口，B4 改 LRU 结构）。

**S9-P0 — 全部完成**：P0-a ✅（FieldSchema RAII）/ P0-b ✅（checkpoint RAII）/ P0-c ✅（kDefaultField 透明查找）/ P0-d ✅（byte_order.hpp 提取）。**P1/P2 保留**。

**按需（C 梯队）**：A2 实测 <1%，C4（Block-Max MaxScore）暂不推荐（A2 同类优化未达预期）。C5/C6 与未来 v4 格式绑定。

**穿插（D 梯队）**：D1/D2/D3 可随手改；D4 单独 PR 配套导出表审计；D5/D6 视 bench 结果。

> **审计方法（2026-06-24）**：3 个并行 agent（explore × 2 + librarian × 1），覆盖
> （a）Cask facade / search_layer / codec 热路径分配拷贝；（b）BM25/HNSW 算法与内存
> 布局；（c）Bitcask/BM25/HNSW SOTA 文献对比。直接审计补充：posix_file / merger /
> highlighter / synonym_map / search_cache。所有发现均与 TASK.md 既有项交叉去重。
