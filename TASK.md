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

- [⛔] **S5 Checkpoint 可选 zstd 压缩** — **否决（用户决定，2026-06-25：不做）**
  - 原计划：section header 加 `compression` 标志（0=raw, 1=zstd），zstd level 1（2-4× 压缩比）。
  - **否决理由**：需引入 zstd 第三方依赖；用户拍板不接受新依赖换 checkpoint 体积收益，保留现状原始序列化。

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

- [x] **S7 查询内并行（Search Pool）** ✅ S7-1~S7-6 全部完成（2026-06-25）— `src/bm25/inverted.cpp`、`src/vector/hnsw.cpp`、`bench/inverted_bench.cpp`
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
  - [x] **S7-5（甜区 intra-query）并行化短语** — `src/bm25/inverted.cpp`（`search_phrase_impl`）、
    `include/bitcask/inverted.hpp`、`tests/inverted_test.cpp`、`bench/inverted_bench.cpp`
    - **已完成（2026-06-25）**：`search_phrase_impl` 候选文档循环（遍历 first term 的 posting
      list）抽成纯函数 `score_one(i)`（仅读 tps/first_*/params，写自己返回值，无共享可变态），
      候选数 `first_pl.items.size() ≥ kPhraseParallelThreshold(2048)` 才 `tbb::parallel_for`
      并行评分写 `cand_scores[i]`（互异下标 → 无锁 data-race-free），否则串行。末尾按
      `(score, ord)` 串行选 top-k。
    - **确定性**：候选 ord 互异（posting 每 doc 一条）→ top-k 选择与评分顺序无关，并行/串行
      **逐字节同果**。附带把 `doc_len` 批量取一次（`fill_doc_lens`，原在评分点逐个抢 shared_lock
      → 并行下会成锁争用热点）。哨兵约定：`score_one` 返回 0 = 非短语（idf>0∧tf_norm>0∧δ≥0 ⇒
      真匹配恒 >0，0 无歧义）。
    - **实测（Release+LTO+native，6 核，`BM_Inverted_PhraseHotTerm`，A/B 切阈值）**：

      | 候选数 | 串行 | 并行 | 加速 |
      |---|---|---|---|
      | 4096 | 196 µs | 56 µs | **3.5×** |
      | 100000 | 5986 µs（~6 ms）| 1286 µs（~1.3 ms）| **4.65×** |

      正是任务预估的「砍 8.7ms 热词短语」。
    - **测试**：`PhraseSearchParallelPathDeterministic`（3000 doc 跨 2048 阈值 → 仅偶数 doc 成短语 +
      得分随 reps 降序 + 连跑两次 (ord,score) 逐字节一致）。
    - **架构说明**：未嵌 `search_arena()`（那在 search 层，bm25 在其下游，依赖倒置）；直接用
      `tbb::parallel_for`（bm25 本就依赖 TBB），并发上限由 TBB market 封顶（与 search_arena 同效），
      嵌在 search_arena.execute 内（batch 查询）时 TBB 可组合嵌套。
    - 验证：Release/Debug **474/474 ctest**（472 + 2 新）；**TSan 零 race**（inverted + hnsw 88 例）。
    - 风险：低（纯函数 + 互异下标写 + 末尾串行选 top-k；确定性测试 + TSan 双护栏）。
  - [x] **S7-6（甜区 intra-query）HNSW int8 精排距离批算并行** — `src/vector/hnsw.cpp`、
    `CMakeLists.txt`、`tests/hnsw_test.cpp`、`bench/hnsw_bench.cpp`
    - **已完成（2026-06-25）**。**先评估两条候选路再定甜区**：
      - **① 图遍历距离批算（per-step M≈16-32 邻居）**：太细粒度 → task spawn 开销 >> 16-32 次
        SIMD 距离，**保证净亏**（同 S7-1 BOW / A2 / A3 教训）。**否决**。
      - **② 多起点 ef-search**：改召回 + 确定性，且 R× 全量功换边际召回，**对吞吐严格劣于
        inter-query（S7-4）**。**否决**。
      - **③ int8 路径 f32 精排距离批算**（line 932 `for found: d = dist_id(q,id)`）：**唯一安全
        甜区**——嵌入式并行（各写互异 `found[i].first`），**确定性**（随后 partial_sort 仍串行
        → 与串行同果，不改召回/排序），读 mmap 只读。**采纳**。
    - **落地**：`found.size() ≥ kRerankParallelThreshold(512)` 才 `tbb::parallel_for` 批算 f32
      距离，否则串行。触发条件：kDot + dim≥64 + 有 VNNI（`int8_dot_` 非空）+ ef≥512。f32-only 路径
      （无 rerank 批，距离在串行遍历中算）+ inmem_int8（跳过 f32 精排）+ 无 VNNI 机器均不触发，
      走原串行——纯增量、无行为变更。`bitcask_vector` CMake 加 `TBB::tbb` 链接。
    - **实测（Release+native，6 核，`BM_Hnsw_Search/100000/1024`，dim=384）**：串行 1887 µs →
      并行 1689 µs ≈ **1.12×（省 ~198µs）**。**ROI 低，符合任务预判**——rerank 批仅占 HNSW 查询
      少数，图遍历（串行、不可并行）才是大头。**保留**因：确定性 + 安全 + 窄门控（仅大 ef int8
      查询付出，小 ef/无 VNNI 零成本），且 HNSW 并发主力仍是 inter-query（S7-4）。
    - **测试**：`Int8RerankParallelPathDeterministic`（2000 vec × dim128 × ef600 → 自匹配 top-1 +
      分数降序 + 连跑两次逐字节一致）；本机有 `avx_vnni` → 实测走并行精排路径，TSan 验证。
    - **WAND 不碰**（顺序依赖 + 已 Block-Max 剪枝）。
    - 验证：Release/Debug **474/474 ctest**；**TSan 零 race**（hnsw 含本测试 + inverted 88 例）。
    - 风险：低（确定性批算 + 互异下标 + 窄门控；确定性测试 + TSan）。
  - **决策**：S7-1（BOW 串行）+ S7-2（共享池）+ S7-3（hybrid 串行）+ S7-4（inter-query 并发
    入口 + Cask 批量查询）+ **S7-5（短语并行，3.5–4.65×）+ S7-6（HNSW int8 精排并行，1.12×）
    全部落地**。intra-query 并行严守「候选/规模过阈才并行 + 末尾串行选 top-k 保确定性」，
    绝不重蹈 grainsize=1 无脑拆分。S7 全部完成。

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

- [x] **S9 全代码库重构评估（6 准则）** ✅ 收尾（2026-06-25：P0/P1 全做 + P2 = 2 实现/1 跳过/2 搁置）— 全代码库审计（S8 仅覆盖 S6/S7 新代码，本轮扩展到全部）
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
  - [x] **P1-a C API new/delete → unique_ptr** — `c_api/bitcask_c.cpp`
    - **已完成（2026-06-25）**：`bitcask_open`/`bitcask_iter_create` 的裸 `new` → `make_unique` 构造期
      持有 + 跨 C 边界前 `release()` 转交裸句柄给调用方；`bitcask_close`/`bitcask_iter_release` 把裸句柄
      `adopt` 回 `unique_ptr`（作用域结束自动 delete，与 open 的 release 对称、异常安全）。cask + iterator
      两处统一。
  - [x] **P1-b vbyte 编码去重** — `include/bitcask/vbyte.hpp`、`src/fileops/codec.cpp`
    - **已完成（2026-06-25）**：`vbyte_encode` **模板化**单字节元素类型（`std::uint8_t` for bm25 WAL/落盘 +
      `std::byte` for codec DocValue 段），codec.cpp 删除匿名 `vbyte_append`、5 处调用改 `vbyte_encode`。
      **读端 `vbyte_read` 保留**——它对 std::byte 缓冲做 bounds-checked + 溢出防御（返回 bool），契约严于
      vbyte.hpp 的 `vbyte_decode`（裸指针无越界检查），非重复。
  - [x] **P1-c search_layer.cpp 注释补充** — `src/search/search_layer.cpp`
    - **已完成（2026-06-25）**：补 `map_analyze`（Map 阶段：纯 const、可并行、产 owning ReduceJob）/
      `reduce_apply`（Reduce 阶段：串行按 ord 序 apply、锁序、LWW 正确性关键）/ `serialize_docmap`
      （docmap sidecar 用途 + covers_next_ord 衔接点）的函数级算法说明。`search_hybrid` RRF 融合本就
      有引文注释（Cormack 2009，k=60）。
  - [x] **P1-d thread_local buffer 工具类** — 新建 `include/bitcask/detail/thread_local_buffer.hpp`、
    `src/fileops/data_file.cpp`、`src/fileops/hint_file.cpp`
    - **已完成（2026-06-25）**：新建 `detail::ThreadLocalBuffer`（`ensure(n)` 按需扩 + `data()`/`size()`
      直访 + `maybe_shrink()` 防膨胀，默认 retain 1 MiB）。`DataFile::read`（get 热路径）与
      `HintFile::fold`（chunked 流式）两处 `static thread_local vector<byte>` + retain/resize 重复模式
      收敛进本类，语义逐字等价。**仅这两处是 thread_local**（data_file fold 的 buf 与 hint validate_trailer
      的 buf 是 per-call local，RAII 自管，不迁移）。
  - **S9-P1 验证**：Release/Debug **474/474 ctest**；**TSan 零 race**（DataFile/HintFile/CrashRecovery/
    Checkpoint/Codec/DocValue/Inverted 共 177 例）。修改文件零新增告警。
    （注：`CApi.SmokeTest` 在 TSan 下有**预存** SEGV——已核实 stash 掉本轮全部改动后基线同样复现，
    与 P1 无关，属 C 测试 × TSan/.so 交互的历史问题。）
  - [⛔] **P2-a Cask god class 拆分（search 方法抽 SearchOps）** — **搁置（用户决定，2026-06-25）**
    - `include/bitcask/cask.hpp`（694 行,60+ 公有方法）+ `src/cask/cask.cpp`（1993 行）：KV facade +
      search facade + merge 协调 + 迭代器 + 读缓存 + 索引池。
    - **搁置理由**：高风险大重构、动核心 API、纯风格收益；现状可工作、TSan-clean。与本仓 S8 既定
      准则「不强加继承/拆分，拆分反增耦合」一致。god class 是风格问题非正确性问题。
  - [⛔] **P2-b InvertedIndex god class 拆分（ScoringEngine + PostingManager）** — **搁置（用户决定，2026-06-25）**
    - `src/bm25/inverted.cpp`（2049 行）：BM25 评分 + Block-Max WAND + posting 管理 + WAL +
      compaction + 5 种搜索模式。
    - **搁置理由**：高风险（核心算法路径，需 bench 对拍防回归）、纯风格收益；现状高内聚、可工作、
      TSan-clean。同 P2-a。
  - [~] **P2-c Analyzer 空文本基类默认实现** — **跳过（评估后，2026-06-25）**
    - 核实：`if (text.empty()) return {};` 仅在各子类 `analyze_with_positions` 开头各一行，且与紧随
      其后的 `if (normalized.empty()) return {}`（`nfkc_fold("")` → 空）**语义冗余**——是省一次空
      nfkc 调用的微守卫，非正确性必需。
    - 唯一干净的去重是 NVI（公有非虚 wrapper 做空检查 + 受保护纯虚 `_impl`），但要改 4 个子类
      （Ngram/Whitespace/Jieba/Stemming）的头+实现 + 公有虚接口，为消一行冗余守卫引入接口间接层，
      **净负值**——与本仓「不强加继承/过度套模式」准则（S8 注）相悖。保留现状。
  - [x] **P2-d search 层 SearchError 枚举** — `include/bitcask/search_layer.hpp`、`src/search/search_layer.cpp`、`include/bitcask/cask.hpp`、`src/cask/cask.cpp`、`tests/cask_docvalue_test.cpp`
    - **已完成（2026-06-25）**：搜索层全部 `expected<vector<SearchHit/Ex>, std::string>`（10 个方法）→
      `expected<…, SearchError>`（强类型枚举，仅 3 值：`kNoVectorIndex`/`kVectorDimMismatch`/
      `kEmptyHybridQuery`，对应 search_layer.cpp 仅有的 3 处 `unexpected`）。Cask 边界新增
      `search_fault(SearchError)→CaskFault` 翻译器，`run_search_one` **删掉 `err_kind` 参数**（caller 不再
      静态猜 kind，9 个单查询方法 + 3 个 batch lambda 统一走 search_fault）。
    - **行为等价**：三种 SearchError 当前都映射 `kInvalidOption`（与原 search_vector/hybrid 的
      `err_kind=kInvalidOption` 一致；text 族原传 `kIo` 但从不报错——dead path 一并清除）。detail 文案
      由枚举确定性派生，保持原字符串。测试断言（kInvalidOption × search_vector 无向量 / hybrid 维度不符 /
      双空）全部仍成立。
    - 验证：Release/Debug **474/474 ctest**；TSan 零 race（Inverted/SearchLayer/DocValue/Checkpoint 196 例）。
    - 风险：中（接口变更）→ 实测零行为变更（纯类型强化 + 边界翻译集中化）。
  - [x] **P2-e 魔法数字具名常量** — `src/bm25/inverted.cpp`（deserialize 读越界哨兵）
    - **已完成（2026-06-25）**：`deserialize` 的 `read_u32/u64/u8` 越界哨兵 `0xFFFFFFFF` /
      `0xFFFFFFFFFFFFFFFF` / `0xFF` → 具名 `kReadFail32/64/8`（函数局部 constexpr），下游 11 处
      `== 0xFFFFFFFF` 短读判定改用具名常量，语义自解释。
    - **原计划修正**：审计称「sentinel → kInvalidPos」+「4096/65536/262144 → kPageSize4K」**与实际不符**——
      这些 0xFFFFFFFF 是**读越界/短读哨兵**（非「无效位置」，故名 kReadFail 而非 kInvalidPos）；且
      inverted.cpp 内**根本不存在** 4096/65536/262144 页大小魔法数（65536 仅在 inverted_wal 的 term_len
      校验上界 + hint_file 的 kChunk 局部，已具名/有上下文）。故只落实准确的子集。
    - 验证：Release/Debug **474/474 ctest**（含 inverted serialize↔deserialize 全量 round-trip）。
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
  - **关联 S29-8**（2026-07-10）：同文件更大的问题是 CJK 快路径整趟 decode 只为校验
    inert 随即丢弃、`to_codepoints` 再解一遍（双重解码）——若做 S29-8 融合入口，本项
    的 API 改造应一并设计，勿分两次动 `nfkc_fold` 签名。
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

> **⚠️ 本节为历史记录（截至 2026-06-25，所列各波均已完成）。当前执行顺序见文末
> 「S29 批次 → 建议执行顺序」（2026-07-10）。**

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
> - ~~**S5**（checkpoint zstd）~~：**否决**（2026-06-25 用户决定不引入 zstd 依赖）。

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

- [~] **B2 `search_text` 的 `terms` 拷贝 + synonym 再拷** — **跳过（评估后判定净负，2026-06-25）**
  - **核实**：提议「`InvertedIndex::search` 接 `span<const string_view>`」**与底层数据结构冲突**——
    `PostingMap = tbb::concurrent_hash_map<std::string, …>`，**tbb 不支持异构查找**（无 transparent
    comparator），`find(acc, term)` 必须 `const std::string&`。代码已自证：`inverted.cpp:1542/1549`
    在只有 string_view 时被迫写 `find(acc, std::string(term))`。
  - **若改 string_view 反而更慢**：`search()` 对每 term 迭代 **2–3 次 find**（387 计数 + 403 快照 +
    447 wand），现 `vector<string>` 一次物化、全程复用（find 零临时）；改 string_view 后每次 find 都得
    `std::string(term)` → **每 term 2–3 个临时串**，比现状多。
  - **现状的「拷贝」近乎免费**：被消的是 analyze map keys → `vector<string>` 一次拷贝，而该 vector
    **本就是 find 所需**；且查询词通常 ≤15B → **SSO 零堆分配**（同 A4/A5 教训）。
  - **结论**：现 `vector<std::string>` 正是 tbb key 要求下的最优表示，无可省。DRY 维度（5 处构建
    terms 重复）归 [D2]（抽 helper，纯代码质量）处理，非本条性能项。

- [~] **B3 `doc_vector_f32` 总是返回 owning `vector<float>`** — **跳过（收益边际）**
  - 分析（2026-06-24）：cask.cpp:901（recovery 路径）的 `rd.vector = doc_vector_f32(*dv)` 已
    被 NRVO 优化（直接构造进 rd.vector，无中间拷贝）。`_into` + thread_local 在此路径无
    省分配（rd 需拥有数据）。cask.cpp:1393 是一次性初始化。**无可省分配**。

- [~] **B4 `on_delete` 重新跑完整 analyze 仅为失效缓存** — **跳过（评估后判定默认不划算，2026-06-25）**
  - **现状**：`on_delete` 取被删文档原文（`doc_texts_` LRU，本就为高亮存）后 `analyzer_->analyze(*text)`
    重分词，纯为建 `changed_terms` 给 `cache_.invalidate_terms()` 做选择性失效。
  - **提议**：写入时把 term 集存进 LRU 条目，删除时直接取，省那次 analyze。
  - **判定净负（默认场景）**——收益有条件且有界 vs 成本无条件且可观：
    - **收益**：仅省 1 次 analyze（Latin ~7µs / CJK-ngram ~21µs），且仅当 `on_delete` 命中 LRU
      （冷文档已降级 `invalidate()`）。`on_delete` 是显式墓碑路径，通常远低频于 写/查（bitcask 覆写是
      LWW put 走 `on_write`，非 `on_delete`）。
    - **成本（被主力 ngram 放大）**：默认 ngram(2–3)，T 码点文档 → ~2T term；即便 SSO `std::string`
      （~32B/个）也 ≈ **64·T 字节/文档**——200 字 CJK 文档约 **12 KB（原文的 4–5×）**；默认 1024 条 LRU
      → **常驻 +~12 MB**。hashed `vector<u64>` 变体约减半（~3 MB）但仍增内存，且要改 `SearchCache::
      invalidate_terms` + 每条目改存 hash（blast radius 更大）。
  - **结论**：为低频路径省 ~21µs 换无条件多 MB 常驻内存，对通用嵌入式存储是错的默认。
    **唯一划算的反例**：delete-heavy + whitespace/jieba 分词（term 集 ≈ 词数，不被 ngram 膨胀）——
    若将来确证此类工作负载再做（hashed 变体）。保留现状。

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

- [x] **D1 HNSW `search_layer` 顶层 `out.resize` 跨层 churn** — `src/vector/hnsw.cpp`
  - **已完成（2026-06-24）**：函数入口 `out.reserve(ef)`，保证后续 `clear+resize` 不 realloc。f32 + int8 两路。
- [x] **D2 `search_phrase`/`near`/`bool`/`fuzzy` 的内层重复模式抽 helper** — `include/bitcask/search_layer.hpp`、`src/search/search_layer.cpp`
  - **已完成（2026-06-25）**：抽两个私有 helper（S8-R3 只动外层骨架，本条收内层）：
    - `materialize_hits(results, filter=null, k=0)`——「bm25 结果集 → SearchHit」物化（ord→ext 翻译跳过
      失败 + 可选 MetaFilter 后过滤 + 可选截断 k）。**6 处调用**：search_text（filter+k 截断）/ phrase /
      near / fuzzy / bool / wildcard，各从 ~10 行循环降为 1 行。
    - `ordered_query_terms(query)`——phrase/near 共用的「analyze_with_positions 按 position 还原词序」，
      **2 处**各从 ~9 行降为 1 行。
  - **保留不动**（元素类型/逻辑确不同，强抽反增耦合）：search_vector（迭代 HNSW `Hit`、score float→double）/
    search_hybrid（RRF 融合 + 稳定平局排序）/ search_fields（`pair<ord,score>` 累加器、已 partial_sort 截断）。
  - 验证：Release/Debug **474/474 ctest**；TSan 零 race（SearchLayer/DocValue/Highlight 123 例）；
    修改文件零新增告警（2 处既有告警 `\x01default` hex-escape + search_fields:827 sign-conv 非本条引入）。
    纯重构、行为零变更（物化语义逐字保持）。
- [x] **D3 `mmap` 的 read 文件加 `madvise(MADV_RANDOM)`** — `src/fileops/data_file.cpp`
  - **已完成（2026-06-24）**：mmap 成功后加 `madvise(MADV_RANDOM)`。get() 热路径按 offset
    随机读，禁 readahead 避免内核预读浪费。
- [~] **D4 `.so` 链接加 `-fno-semantic-interposition` + `-fvisibility=hidden`** — **部分完成**
  - `-fvisibility=hidden -fvisibility-inlines-hidden` 已在 CMakeLists.txt:27 应用。
    `-fno-semantic-interposition` 增量收益边际（visibility hidden 已防大多数 interposition），
    暂缓。
- [~] **D5 `PosixFile::pread` / `read` 每次分配 `vector<byte>`** — **评估后保留**
  - 旧 API 仅 1 处 caller（hint_file.cpp:202，冷启动恢复路径），非热路径。不迁移。
- [x] **D6 `select_neighbors` 中 `vec_of(pid)` 反复取** — `src/vector/hnsw.cpp`
  - **已完成（2026-06-24）**：picked 旁挂 `picked_vecs` 缓存 vec_of 结果，内层循环用缓存指针
    替代重复 `vec_of(pid)` 调用。M=16 ef=200 下省 ~3000 次冗余取指/insert。

### S10 执行进度（2026-06-24）

**A 梯队 — 全部完成**：A1 ✅（缓存前置）/ A2 ✅（WAND 块上界 <1% 保留）/ A3 ⏭️ 跳过（std::function 非热点）/ A4 ✅（字段名 intern，内存 −40%）/ A5 ✅（字段打包，alloc −6.4%）。

**B 梯队 — 全部收尾**：B1 ✅（SynonymMap span）/ B5 ✅（HNSW PQ 复用）/ B6 ✅（merger reserve）/ B3 ⏭️ 跳过（NRVO 已优化）/ B2 ⏭️ 跳过（2026-06-25：tbb 无异构查找 → string_view 反增临时串，且现 vector<string> 是 find 所需、短词 SSO 零堆）/ B4 ⏭️ 跳过（2026-06-25：为低频 delete 路径省 ~21µs 换 ngram 下常驻 +~12MB，默认不划算）。

**S9-P0 — 全部完成**：P0-a ✅（FieldSchema RAII）/ P0-b ✅（checkpoint RAII）/ P0-c ✅（kDefaultField 透明查找）/ P0-d ✅（byte_order.hpp 提取）。

**S9-P1 — 全部完成（2026-06-25）**：P1-a ✅（C API make_unique + release/adopt）/ P1-b ✅（vbyte_encode 模板化去重）/ P1-c ✅（map_analyze/reduce_apply/serialize_docmap 注释）/ P1-d ✅（ThreadLocalBuffer 工具类）。

**S9-P2 — 收尾（2026-06-25）**：P2-c ⏭️ 跳过（空文本守卫冗余 + NVI 间接层净负值）/ P2-d ✅（SearchError 强类型枚举 + 边界翻译，去 leaky abstraction）/ P2-e ✅（deserialize 读越界哨兵具名 kReadFail*；原计划页大小常量经核实不存在）/ P2-a ⛔ 搁置 + P2-b ⛔ 搁置（god class 拆分，用户决定：高风险大重构、纯风格收益、动 TSan-clean 核心代码，永久搁置）。**S9 全部收尾**（P0/P1 完成，P2 = 2 实现 + 1 跳过 + 2 搁置）。

**按需（C 梯队）**：A2 实测 <1%，C4（Block-Max MaxScore）暂不推荐（A2 同类优化未达预期）。C5/C6 与未来 v4 格式绑定。

**穿插（D 梯队）**：D1 ✅ / D2 ✅（2026-06-25：materialize_hits + ordered_query_terms 两 helper，8 处去重）/ D3 ✅ / D6 ✅；D4 ⏭️ 部分（visibility 已加，-fno-semantic-interposition 暂缓）/ D5 ⏭️ 保留（非热路径）。

> **审计方法（2026-06-24）**：3 个并行 agent（explore × 2 + librarian × 1），覆盖
> （a）Cask facade / search_layer / codec 热路径分配拷贝；（b）BM25/HNSW 算法与内存
> 布局；（c）Bitcask/BM25/HNSW SOTA 文献对比。直接审计补充：posix_file / merger /
> highlighter / synonym_map / search_cache。所有发现均与 TASK.md 既有项交叉去重。

---

## 待办：第十一梯队（S11 线程安全化 — 通用 C++ 库定位，2026-06-25）

> **定向**：libbitcask 作为**通用 C++ 库**，而非仅服务 Erlang/NIF「一进程一 Cask」
> 模型。该定位下「同一 handle 多线程安全」从可选变为契约——通用用户默认期望它，
> 会在并发写时静默损坏数据的存储库不合格。设计稿 `docs/design/thread-safety.md`。
>
> **当前唯一真实安全缺口 = 多线程写同一 handle**（writer-vs-writer：`DataFile`
> 的 `current_offset_`/`write_buf_`/`batch_buf_`、`writes_since_sync_` 无保护）。
> 读路径（get/搜索）已结构级并发安全（S6/S7 TSan 已证）；keydir + 索引池
> （S6 reorder buffer 支持任意到达序按 ord apply）早为并发写就绪——W1 只补
> 「active 文件写序列」这最后一块。
>
> **否决**：细粒度写并发（预分配 offset + 并发 pwrite）——data 是单 append WAL，
> 写在文件层本就串行、IO-bound，串行化非瓶颈；高风险换不到吞吐。更高写并发 →
> 按目录分片多 Cask 实例（横向扩展）。

- [x] **W1 写路径内部串行化（核心）** — `include/bitcask/cask.hpp`、`src/cask/cask.cpp`、`tests/crash_recovery_test.cpp`
  - **已完成（2026-06-25）**：加 `std::mutex write_mu_`，在 `put`/`remove`/`put_doc`/`sync`/
    `close_write_file` 入口 `lock_guard`（覆盖内部 `ensure_active_writer`/`roll_active`/
    `maybe_group_commit`/`write_and_keydir` 全序列）。把「外部串行契约」内化为「内部互斥」→
    同一 handle 多线程写安全。
  - **关键修正（实施时核实）**：`flush_index` **不纳入** write_mu_——经核实它被 `prepare_search()`
    （**读/搜索路径**）调用，上锁会让搜索串行化；且 IndexPool flush 自带 cv 同步本就线程安全。
    锁集最终 = 5 个真写方法（put/remove/put_doc/sync/close_write_file）。
  - **merge 交互审计**：核实 `merge()` 不触 `active_*`/DataFile 成员（写自有输出文件，经 keydir
    `shared_mutex` 协调）→ **不纳入 write_mu_**，与写并发不变。`FieldSchema::intern` 已自带
    `shared_mutex`（读写安全）→ 无新增 reader-vs-writer 缺口。
  - **锁序确认**：`write_mu_` 最外层 → 内部再取 `read_cache_mu_`（roll/close_write_file）；读路径
    （get/search）**不**取 write_mu_（保持无锁/共享锁）→ 无反向依赖、无死锁。无递归锁（写方法
    互不内部调用）。
  - 吞吐不变（写本就串行，锁 ~20ns ≪ pwrite/fsync µs–ms）。C API 自动受益（包装 Cask）。
  - **验证**：新增 `ConcurrentWritersSharedCaskNoCorruption`（8 线程共享 handle 并发 put + remove，
    互不相交 key 段，重开逐键校验）。Release/Debug **475/475 ctest**；**TSan 零 race**（95 例并发
    套件含本测试）；**已实证移除 write_mu_ 后该测试 TSan 必报 data race**（active_data_/
    current_offset_/DataFile::size）→ 确为真护栏。
  - 风险：低（串行化把契约内化，读路径不动，全量 + TSan 对拍）。
- [x] **W2 读/搜索并发确认 + 注释订正 + 配置类审计** — `include/bitcask/cask.hpp`、`doc/api-cpp.md`、`README.md`、`tests/cask_docvalue_test.cpp`
  - **已完成（2026-06-25）**：
    - **TSan 测试** `W2ConcurrentSearchAndWriteNoRace`：4 读线程轮转 6 种搜索模式
      （text/phrase/bool/fields/vector/hybrid）+ 2 写线程并发 put_doc/remove 同一 handle →
      **TSan 零 race**（读路径并发安全 + W1 后读写并发安全实证）。
    - **注释订正（含 W1 连带）**：`cask.hpp` 顶部线程模型重写为「通用 C++ 库 handle 多线程安全」；
      写方法（put/remove/put_doc/sync/close_write_file）「线程安全:否」→「**是**（W1 write_mu_）」；
      搜索方法（text/phrase/bool/fields/near/fuzzy/wildcard）「否（保守）」→「**是**（并发读安全）」；
      search_text_batch 去掉「不得并发写」过时 caveat。
    - **`set_synonym_map` 审计**：判定为**配置类**——加锁会给查询热路径（无 synonym 的常态）添
      atomic 开销，不划算；定为「**须先于并发查询配置**或外部串行化」契约（注释 + 文档明示）。
    - **契约显眼化**：`doc/api-cpp.md` §9 线程模型汇总表全面订正（写=是、搜索=是、merge 与读写并发、
      set_synonym_map=配置类）+ §5.3 写节头 + 各方法注释；README 线程模型一行订正 + docs 表加
      `design/thread-safety.md` 指针；写吞吐指引「更高写并发 → 按目录分片」写入文档。
  - 验证：Release/Debug **476/476 ctest**（475 + 1）；TSan 零 race（CaskDocValue/CrashRecovery/
    SearchLayer 110 例）。纯文档 + 1 测试，零行为变更。
  - 风险：零。
- [x] **W3 生命周期硬化（close fail-fast）** — `include/bitcask/cask.hpp`、`src/cask/cask.cpp`、`doc/api-cpp.md`、`tests/crash_recovery_test.cpp`
  - **已完成（2026-06-25）**：加 `std::atomic<bool> closed_` + `is_closed()` helper。`close()` 顶
    `closed_.exchange(true)`（兼作**幂等门**——二次 close 直接返回）。公共方法入口 fail-fast：
    - 数据面（get/put/remove/put_doc/sync/close_write_file/merge）→ `unexpected(kInvalidOption,
      "cask is closed")`；
    - 搜索集中在 `run_search_one`/`run_search_batch` 两处守（覆盖 9 单 + 3 batch）；
    - 内省（status/needs_merge/is_empty_estimate/is_frozen）→ 安全默认值（不解引用空 keydir_）；
    - `CaskIter::start` 守 `parent_->is_closed()`（close 后建/启迭代器 fail-fast）。
  - **范围（诚实）**：best-effort 防误用——拒绝 close **后新发起**的调用;与 close **并发在途**
    的调用仍是 caller 责任（契约：close 时刻无在途操作）。**不做完整 rundown**（成本高、价值低）。
    `get_owned` 经 `get()` 透明覆盖;`read_handle_count` 天然返回 0（read_files_ 已 clear）。
    用 `kInvalidOption`+detail 而非新增 `kClosed`（避免 C API 枚举 churn）。
  - 验证：新增 `OperationsAfterCloseReturnErrorNotUb`（close 后 get/put/remove/sync/merge 返
    kInvalidOption + 内省安全默认 + iter start fail-fast + 二次 close 幂等）。Release/Debug
    **477/477 ctest**；TSan crash_recovery 11 例零 race。无 W3 守时该测试在 Debug 下空指针解引用崩溃。
  - 风险：低。
- [x] **W4 迭代器并行扫描** — `include/bitcask/cask.hpp`、`src/cask/cask.cpp`、`doc/api-cpp.md`、`tests/crash_recovery_test.cpp`
  - **已完成（2026-06-25）**：加 `Cask::parallel_scan(n_threads, fn)` 高层 API。把 W1-W3 建立的
    「多线程读安全」用于全表扫描（analytics/export/reindex）。
  - **设计**：原计划「N 独立 iterator 分区」不可行——keydir 迭代器是**单快照游标**
    （`keys_snapshot_` + cursor），无法切分。改为：① 单次快照所有 live key（调用线程串行，新增
    `CaskIter::drain_live_keys`——走 keydir proxy，**仅 key 拷贝、不读 value**，比逐条 next 的
    pread+decode 廉价）② 按 n_threads 分段 ③ N 个 std::thread 并发 `get()` 读值 + 调 fn。
    **被并行化的是读值的 pread+decode**（真正的成本）；单 append WAL 写串行不受影响。
  - **语义**：`n_threads==0` → hardware_concurrency；并发删除致 get kNotFound → 跳过
    （near-real-time，与搜索一致）；其它错误（IO/CRC）→ 停止返回该错误；返回遍历到的 key 数。
    `fn` 必须线程安全（不同线程并发调用，各处理不相交 key 段）；value 是零拷贝 view（仅回调内有效）。
    Cask 已 close → kInvalidOption（W3）。KV 模式也可用（不依赖 search）。C++-only（C API 未绑定——
    C host 可自行多线程 get）。
  - **决策**：单 iterator **不**支持并发（cursor 语义模糊，价值低）——已在 W2/W3 文档化「每线程一个
    CaskIter」（同 std 容器迭代器约定）。parallel_scan 是「并行遍历」的正解。
  - 验证：新增 `ParallelScanVisitsAllKeysOnce`（2000 key + 删 1/10 → 4 线程扫描每 key 恰一次 +
    value 正确 + 删的不出现；n_threads=0 路径；close 后 fail-fast）。Release/Debug **478/478 ctest**；
    **TSan 零 race**（parallel_scan 多线程 get + W1/W3 测试）。
  - 风险：低（建于 W1-W3 安全基座；快照串行 + get 并发安全；全量 + TSan 对拍）。

> **结论**：通用库定位下 **W1+W2+W3 为必做组**，共同建立「多读 + 多写（内部串行）
> + 读写并发 + fail-fast 生命周期」的常规契约（对标 RocksDB/LMDB）。合计约一天多，
> 全部低风险。建议顺序 W1 → W2 → W3，W4 按需。

**S11 — W1–W4 全部完成（2026-06-25）**：W1 ✅（write_mu_ 写路径串行化）/ W2 ✅（读写并发
确认 + 全文档注释订正 + set_synonym_map 契约）/ W3 ✅（closed_ fail-fast + 幂等）/
W4 ✅（parallel_scan 并行全表扫描）。
- **单元测试**（4 例，全 Debug 478/478 + TSan 零 race）：ConcurrentWritersSharedCaskNoCorruption /
  W2ConcurrentSearchAndWriteNoRace / OperationsAfterCloseReturnErrorNotUb / ParallelScanVisitsAllKeysOnce。
- **性能测试**（`bench/cask_bench.cpp` 新增 2 个）：
  - `BM_Cask_Put_Concurrent`（多写争用）：单写 ~980k/s 不受锁影响；多写**不升反降**
    （1→8 线程 980k→46k/s）——短临界区高争用 mutex 退化，**印证「写扩展靠分片不靠堆线程」**
    （LMDB-like 写串行；非 bug，数据安全 TSan 验证）。
  - `BM_Cask_ParallelScan`（读扩展）：5 万 key 全表扫描 1→4 线程 **3.17× 加速**（6 核饱和）。
  - 实测体现契约本质：**读真并行（scan 3.2×）+ 写内部串行（堆线程不提速）**。详见
    `docs/design/thread-safety.md` §9。
- **全套文档同步**：README / api-cpp §5+§9 / api-c §14 / cpp-arch / concurrency-zh /
  async-index-pipeline / design/thread-safety（§7 各接口实现机制 + §9 实测基线）+ 头注释。

## 待办：第十二梯队（S12 全库审计 — 2026-07-01）

> 来源：2026-07-01 全代码库深审（4 路并行核实：异步索引管线 / 向量·HNSW /
> WAL·mmap·存储层 / 并发·格式·构建工具链）。
> **总体结论**：代码质量高、功能债少——**多数设计文档标注的「未做」其实已落地**
> （文档滞后于实现，非缺口）。真实工程价值集中在 **P0 三项**；P1 文档同步本轮已清；
> P2 为能力/工程质量债；P3 为已知权衡（对齐「已核实无需改动」节，记录备查）。

### P0 真实缺口（建议优先，成本均不高）

- [x] **S12-1 sealed-mmap read 句柄默认上限（防 fd/mmap 无界）** — 已完成（2026-07-01）
    · `include/bitcask/cask.hpp`、`src/cask/cask.cpp`、`include/bitcask/data_file.hpp`、`tests/cask_docvalue_test.cpp`
  - **订正原判断**：审计报告称「无 LRU 驱逐」**有误**——P9 的 read-handle 近似 LRU
    （`max_read_handles` cap + atime + `evict_read_handles_locked`，只淘空闲句柄）**早已实现**，
    每句柄 = 1 fd + 1 sealed mmap（mmap 后 **fd 保留不关**，`data_file.cpp:50`），故该 cap
    **同时界定 fd 数与 mmap 映射数**。真实缺口只有一个：**默认 `max_read_handles = 0` = 不限**，
    即开箱无界。
  - **改动**：细化语义——`0`（默认）→ **自动**：由 `RLIMIT_NOFILE` 软上限推导安全上限
    （`getrlimit`，约一半、下限 64）；新增哨兵 `CaskOptions::kUnlimitedReadHandles` → 显式不限；
    其它 N → 显式上限。解析逻辑抽成纯静态 `Cask::resolve_read_handle_cap(opt, nofile_soft)`
    便于单测。open() 时 `getrlimit(RLIMIT_NOFILE)` 解析并写回 `opts_.max_read_handles`
    （evict 逻辑不变）。
  - **附带修 bug**：`data_file.hpp:78` 头注释「映射成功后 close(fd)」与实际（fd 保留）矛盾，已订正。
  - **未做（有意）**：字节/地址空间上限——count-based cap 已界定两个稀缺资源（`ulimit -n`、
    `vm.max_map_count`），64 位地址空间充裕，byte-based 收益低不做。
  - **验证**：新增 `ReadHandleCap.ResolveSemantics`（哨兵/显式/自动三态 + 下限），既有
    read-handle cap 测试改用显式哨兵；**Debug 全量 484/484**（483+1），Release 干净，零回归。
  - 风险：低。小/中库（< 自动上限）行为不变、零 churn；大库由 crash（fd 耗尽）改为 graceful
    句柄淘汰（miss 时重开 sealed 文件 ~μs）。**默认行为变更**（0 从「不限」变「自动上限」）——
    需显式不限者用 `kUnlimitedReadHandles`。

- [x] **S12-2 reducer 线程内自动 compaction（opt-in）** — 已完成（2026-07-01）
    · `include/bitcask/search_layer.hpp`、`src/search/search_layer.cpp`、`include/bitcask/index.hpp`、
      `src/keydir/index.cpp`、`include/bitcask/inverted.hpp`、`src/bm25/inverted.cpp`、
      `tests/search_layer_test.cpp`、`tests/cask_docvalue_test.cpp`
  - **并发核实（决定设计）**：后台线程 compact 与 live reducer 并发**不安全**——`reduce_apply`
    里 `add_doc`(先) 在 `put_doc`(后) 之前，间隙内 posting list 已含新 ord 但 Index 未 size 到它，
    并发 compact 的 `fill_is_live(新ord)` 因越界**误判 dead** → 压掉 live posting（数据损坏）。
    安全前提只有「先 flush 到静止」或「compact 在 reducer 线程内」。结合代码库**无后台维护线程**
    的哲学（merge 亦 caller 驱动），选**后者**。
  - **实现**：新增 `SearchLayerConfig::auto_compact_dead_ratio`（0=关，默认；(0,1]=开+per-list 阈值）。
    Index 加 `retired_since_compact_` 计数器（put_doc 覆盖 + remove 两个死亡点各 +1，均在
    `index_.mutex_` 下、仅 reducer 线程）。`SearchLayer::maybe_auto_compact()` 在 reduce_apply /
    on_write / on_delete 末尾调用：关时仅一次 double 比较（零开销）；开时累计退休达
    `max(1024, live/2)` 才在**本 reducer 线程内** compact()，与 add_doc/put_doc 同线程 → 无并发窗口。
    节流随 live 规模缩放，摊薄全量扫描成本。附带加 `total_postings()` 内省（InvertedIndex + SearchLayer）。
  - **未做（有意）**：不新增后台线程（违哲学 + 需 flush stall）；不改写路径 O(1) 触发（remove_doc
    仍不碰 posting list）；compact 频率靠节流而非精确 per-list 计数（够用，避免给 remove 加桶锁开销）。
  - **验证**：新增 3 例——`SearchLayer.AutoCompactBoundsPostingGrowth`（开：6020 写→postings<2000）/
    `NoAutoCompactWhenDisabledButSearchCorrect`（关：>6000 保留但搜索正确）/
    `CaskDocValueTest.AutoCompactConcurrentReadersNoRace`（3 读者并发 + churn 写者经异步管线，
    reducer 内触发 compact）。**Debug 全量 487/487**（484+3），**TSan 三例零 race**（并发护栏），
    Release 干净。
  - 风险：低。默认关=行为不变、零开销；开启后 compact 在 reducer 线程串行（安全已 TSan 实证），
    仅延迟索引可见性（非 durability）。**默认关，需显式 opt-in**。

- [x] **S12-3 field.schema 加 magic/version/CRC** — 已完成（2026-07-01）
    · `include/bitcask/field_schema.hpp`、`src/cask/cask.cpp`、`src/fileops/migrate.cpp`、`tests/data_file_test.cpp`
  - **格式**：文件头 8 字节 `[magic="FSCH":u32][version=1:u32]`（小端）；每条 entry
    `[NameLen:u16][name][CRC32:u32]`，CRC 覆盖 `[NameLen|name]`（`hw::crc32`，与 data/hint/WAL 同多项式）。
  - **健壮性**：magic/version 未知 → `open()` 返回 false（fail-fast）；完整 entry 但 CRC 不符
    → fail-fast；**torn tail**（尾部半条，append 崩溃常态）容忍跳过（与 WAL 语义一致）。两处
    `open()` 调用点（`cask.cpp` 创建/打开路径）已检查返回值 → `unexpected(kIo, "corrupt or
    incompatible")`。
  - **兼容（用户定：自动探测 + 兼容读旧）**：`open()` peek 前 4 字节——有 magic 走新格式；
    无头（v3.0.0 现存库）按 legacy `[len][name]` 照读，并在可写目录下**原子升级**为新格式
    （temp + `fsync` + `rename`，权威数据零丢失窗口）；升级失败（只读目录）退回 legacy 追加，
    保持文件自洽。`migrate_le` BE→LE 输出改为直接写新格式。
  - **验证**：新增 5 例（`FieldSchema.NewFormatRoundTripAndMagicHeader` / `DetectsCrcCorruption` /
    `RejectsUnknownVersion` / `LegacyHeaderlessAutoUpgrades` / `ToleratesTornTailNewFormat`），
    含既有 2 例全过；**Debug 全量 483/483 ctest**（478+5），零回归。
  - 风险：低（新写入 + 兼容读旧 + 原子升级；权威数据无丢失窗口）。

- [x] **S12-3b bitcask.meta 加 CRC（version 3）** — 已完成（2026-07-01；S12-3 的姊妹项）
    · `src/cask/meta_file.cpp`、`src/fileops/migrate.cpp`、`tests/cask_docvalue_test.cpp`、`tests/data_file_test.cpp`、`doc/format-zh.md`、`doc/migrate-le.md`
  - 审计指出 `bitcask.meta` 有 magic+version 但**无 CRC**（18B 里 metric/dim/quant/inmem 单 bit
    翻转检测不出）。修：**version bump 2→3**，保留区偏移 14 放 CRC32（u32 LE，覆盖 `[0,14)`）。
  - **读端向后兼容**：v1 拒绝（大端）；**v2 兼容读**（无 CRC 字段，旧库不破坏）；v3 校验 CRC
    失配 → fail-fast。**写端恒写 v3**。`migrate_le` 输出改 v1→v3（含 CRC）。
  - **验证**：新增 `MetaV3CrcRoundTripAndCorruption`（往返 + 篡改覆盖区 → 拒绝）/
    `MetaV2BackwardCompatRead`（v2 兼容读）；migrate RoundTrip 断言更新为 v3 + 校验 CRC。
    **全量 488/488**，Release + `-Werror` 库构建干净。
  - 附带回答：field.schema legacy 读后**确实原子重写为新升级格式**（`upgrade_legacy_to_new_`），
    仅只读目录升级失败时才回退 legacy 追加。

### P1 文档同步（✅ 本轮已完成 2026-07-01）

- [x] **S12-4 4 处过时设计文档状态行订正** — 已完成（2026-07-01）
  - `docs/design/async-index-pipeline.md:3`「评审中(未实现)」→ **已落地(S6/P0–P4)**，
    列各子项代码落地点，并标注唯一偏差（单 reducer 线程替代 M 线程池 → **库间 apply 未并发**，
    仅 Map 并行；`thread_pool.hpp:597`）。
  - `hnsw-design-zh.md` §4 + §7「过滤检索 V3 不做」→ **V5 已落地图内过滤**
    （`search_layer.cpp:279-290`，`cask.hpp:449-451`）。
  - `hnsw-int8-only-design-zh.md:99`「盘上直存 int8 仍未做」→ **V7 已落地**
    （BVH2 v2 段直存 qcodes+scale+sum，`hnsw.cpp:1243-1257`；另 DocValue int8 落盘
    `codec.cpp:148-173`）。
  - 原则：保留历史评审推理，加「已落地/更新」标注 + 代码落地点（沿用文档既有「落地记录」风格）。

### P2 能力覆盖 / 工程质量

- [x] **S12-5 C API 契约债 + 能力缺口** — 已完成（2026-07-01；[高] 注释订正 + [中] batch/parallel_scan 全落地）
  - [x] **[高] 头部线程安全注释订正** — 已完成。`bitcask_c.h:9-14` 旧注释「put/delete/search
    非线程安全，caller 串行化」与 C++ W1/W2 内化线程安全**矛盾**（C API 是 Cask 的透明包装、
    无 C 层共享可变态，完全继承其契约）。已重写为「同一 handle 多线程安全」，对齐
    cask.hpp:6-24 / api-c.md §14，含读/写/读写并发/merge/iter 各条。**纯注释、零行为变更**，
    C API smoke 测试通过。注：api-c.md §14 早在 S11 已正确，仅头文件被漏。
  - [x] **[中] batch / parallel_scan** — 已完成（2026-07-01）
    - [x] **三种批量搜索全部暴露**（`c_api/bitcask_c.{h,cpp}`）：`bitcask_search_text_batch` /
      `bitcask_search_vector_batch` / `bitcask_search_hybrid_batch`（新增 `bitcask_hybrid_query_t`）
      + 共用 `bitcask_search_result_batch_free`。共用 `fill_batch_results` helper（DRY）；`out_results[i]`
      失败=NULL、无命中=count 0 的非空结果，`fault` 回填首个失败详情；`n==0` → NULL+OK；参数校验。
    - [x] **`bitcask_parallel_scan` + `bitcask_scan_fn`**（callback 式；`ctx` 带用户状态，key/value 零
      拷贝 view 仅回调内有效；回调可能多线程并发调用）。透传 C++ `parallel_scan`（W4）。
    - **验证**：`test_search_text_batch` / `test_search_vector_hybrid_batch`（L2 精确 top-1）/
      `test_parallel_scan`（500 key×4 线程，atomic 校验访问一次 + value checksum；n_threads=0；空参）。
      **C API 7/7**；符号在 .so 导出；api-c.md §10/§11 已补文档；**plain + ASan(含 leak) + TSan 全过**。
    - **顺带修构建缺口**：`bitcask_c_api_test` 未 link `bitcask_sanitizers`（与其它测试目标不一致）→
      sanitize 构建下未插桩的 C 主程序链接已插桩 `.so`，`.so` 内起线程（search 模式 IndexPool）即
      SEGV。KV-only 时不触发，search 测试首次暴露。修：`tests/CMakeLists.txt` 补 link
      `bitcask_sanitizers`，TSan/ASan 无需 preload 即通过。
  - [x] **[中] `BITCASK_ERR_CLOSED`**（路线 A，2026-07-01）。新增 C++ `CaskError::kClosed`（枚举末尾，
    ABI 增量安全），11 处 `is_closed()` fail-fast 从 `kInvalidOption` 改为 `kClosed`
    （`cask.cpp`）；C 枚举加 `BITCASK_ERR_CLOSED = 13` + `to_c_error_kind` 映射。反转 W3 的
    "复用 kInvalidOption"取舍——现 C++ 消费方可区分「用了已关闭 handle」与「参数非法」。
    - **关键发现**：**纯 C API 下 `BITCASK_ERR_CLOSED` 不可达**——`bitcask_close` 直接 adopt+delete
      销毁句柄（`bitcask_c.cpp:284`），无「已关闭但存活」的 C 句柄；close 后再用是 UAF（caller bug）。
      故 kClosed 的实际受益方是 **C++ 消费方**（`Cask::close` 保留对象 + fail-fast），C 映射为
      完整性/未来路径保留。
    - 测试：C++ `OperationsAfterCloseReturnErrorNotUb` / `ParallelScanVisitsAllKeysOnce` 断言改
      kClosed；C 测试注明 UAF 语义不测。api-c.md §4.4 错误码表补 `13`。全量 486/486 + Release 干净。
    - **未做（有意）**：`bitcask_close` 返 `void` 不改——C++ `close()` 本就 void+noexcept best-effort，
      无可回报；改签名是破坏性变更却无实益。若要让 C 侧也能 graceful「close 后 fail-fast 不 UAF」，
      需把 close 拆成 `close`（不销毁）+ `free`（销毁）——ownership 破坏性变更，留待独立评估。

- [~] **S12-6 CI 单一编译器/平台** — 部分完成（2026-07-01）
  - [x] **加 clang 构建 job**（`.github/workflows/ci.yml` `clang-build-test`）。**过程中修了 2 个
    真实可移植性 bug**（代码库从未用 clang 构建过）：① `hnsw.cpp:150` AVX-512 归并的
    `#if` 只判 `__GNUC__>=10`，漏了 clang（其 `__GNUC__` 恒为 4）→ 落入 `#else` 用了错误
    intrinsic `_mm512_extractf64x4_ps`（clang 不认）；补 `defined(__clang__)` + 修正死分支
    intrinsic。② `kDefaultField = "\x01default"` 的 `\x` 转义贪婪吞 "defa" → 实际是 0xFA+"ult"
    （GCC 静默、clang 报错）；改 `"\xfa" "ult"` **保留完全相同字节**（已入 checkpoint，零 on-disk 变化）。
    **本地验证**：clang Debug 全量 486/486 通过（GCC 亦 486/486）。CI 用 Debug（clang+Release LTO
    需 gold 插件/lld，Debug 无 LTO 规避）。
  - [ ] **macOS / ARM64 job** — 需对应 runner + 本地无法验证的交叉构建，留待。ARM64 尤有价值
    （验证 NEON/非 VNNI 标量路径 `detail/int8_kernels.hpp`）。
  - 注：端序已安全（整数可移植位移；float 向量有 `static_assert(endian==little)`，`codec.cpp:139`）。

- [x] **S12-7 构建加固小项** — 已完成（2026-07-01；版本单一真源 + -Werror）
  - [x] **版本号单一真源**。`project()` 原无 VERSION；库 SOVERSION（`CMakeLists.txt` 硬编码
    `VERSION 3.0.0`）与 C API（`bitcask_c.cpp:143-146` 硬编码 `return 3/0/0`）各写一份，易漂移。
    改：`project(libbitcask VERSION 3.0.0)` 为唯一手写处；`configure_file` 从 `PROJECT_VERSION*`
    生成 `bitcask_version.h`（新增 `c_api/bitcask_version.h.in`），c_api 用宏（带非 CMake 回退）；
    库 `VERSION/SOVERSION` 用 `${PROJECT_VERSION}/${PROJECT_VERSION_MAJOR}`。生成头验证为 3/0/0，
    c_api 测试通过。**顺带修** `cask.cpp:2121` 忽略 `save_search_ckpt()` 返回值（-Wunused-result）→
    显式 `(void)` + best-effort 注释（checkpoint 失败非致命，下次 fold 重建）。
  - [x] **`-Werror`（first-party）** — 已完成。原估「~15 处噪音告警」核实后，**真正 first-party
    库告警只有 13 处**（之前把 vendored `include/cppjieba`/`include/limonp` 误算进来）：
    -Wshadow×7、-Wsign-conversion×4、-Wunused-function×1、-Wunused-parameter×1，全为**零行为风险**
    的机械修复（rename `max_tf`/`pos`/`k`、删冗余 `using Cand`、删未用 `str_to_bytes`、
    `[[maybe_unused]]` key、3 处 `static_cast<ptrdiff_t>`）。跨 6 文件（inverted.hpp/cpp、
    hnsw.cpp、thread_pool.hpp、search_layer.cpp、cask.cpp、keydir.cpp）。
    - **third_party 隔离**：`cppjieba` INTERFACE 改 `SYSTEM` include（`CMakeLists.txt`），编译器视为
      系统头、不对其大量告警报错，使 -Werror 只作用于 first-party。
    - **机制**：新增 `option(BITCASK_WERROR OFF)` → 开时给 `bitcask_warnings` 加 `-Werror`。**默认关**
      （避免新编译器新告警破坏下游/本地构建）；新增 CI job `werror-lib`（Release + BUILD_TESTING=OFF +
      只建 `bitcask_static`/`bitcask_shared`，不含 bench/tests）开启作护栏。
    - **验证**：本地 `-DBITCASK_WERROR=ON` 建库 0 错误 0 告警；常规构建全量 486/486 零回归
      （13 处编辑行为中性）；ci.yml YAML 合法（5 jobs）。

### P3 已知权衡 / 远期（记录备查，当前无需动）

> 均为设计文档**有意判定延后**或架构性约束，现有定位下不构成正确性问题；列此避免重复审计。

- **close 与并发在途操作 UB**：`closed_` 只是 best-effort fail-fast 门（`cask.cpp:710`），
  与 close 并发在途的调用仍 caller 责任；完整 rundown 判「成本高价值低」放弃
  （`docs/design/thread-safety.md:82`）。未来做通用嵌入库需重估。
- **异步管线 Reduce 端单 reducer → 库间 apply 不并发**（`thread_pool.hpp:597`）：多库高写入
  吞吐升级点，方案见 `async-index-pipeline` §5 的 M 线程池 + per-库 apply 锁。
- **WAL 异步 flush / 跨线程 group-commit**：单写者前提成立（`inverted_wal.hpp:22`），归 V7+。
- **posting zero-copy 完整 Phase 2**（published_count + deque）：Phase 1 + 2-min CoW
  已覆盖主要收益（`inverted.hpp:378-381`），有条件延后。
- **向量远期**：affine 量化（<1pt 召回）/ PQ（10M+ 才划算）/ 多段+DiskANN 外存（>1M 才需要）——
  当前 ≤1M 规模单图 μs 级已达红线，全用不上。

> **建议执行顺序**：~~S12-3（field.schema 加头）✅~~ → ~~S12-1（read 句柄默认上限）✅~~ →
> ~~S12-2（reducer 内自动 compaction）✅~~ → ~~S12-5 [高] C API 头注释订正 ✅~~ 2026-07-01
> （**P0 三项 + P1 文档债全部完成**）→ ~~S12-5[高] C API 注释~~ ✅ →
> ~~S12-6 clang job（+2 可移植性 bug 修复）~~ ✅ → ~~S12-7 版本单一真源~~ ✅ 2026-07-01。
> → ~~S12-5[中] C API batch×3 + parallel_scan~~ ✅ → ~~S12-5 BITCASK_ERR_CLOSED~~ ✅ →
> ~~S12-7 -Werror（first-party 库）~~ ✅ 2026-07-01。**S12-5 / S12-7 全部完成**。
> 剩余（均需 runner，本地无法验证）：**S12-6 macOS · ARM64 job**（唯一 S12 未决项）。P3 按需。

---

## 待办：第十三梯队（S13 四维审查 — 2026-07-02）

> 来源：2026-07-02 四路并行深审（内存泄漏 / 线程死锁·竞态 / 性能 / 功能缺口）。
> **总体结论**：RAII 全覆盖（内存侧仅 1 处真实泄漏）、锁纪律大部分论证成立，但发现
> **1 个并发数据丢失级 bug（S13-F1）** 与 **1 个永久挂起级 bug（S13-F2)**——均为纯并发
> 触发、串行测试不可见。性能侧最高性价比是搜索缓存全量失效（S13-P1）。功能侧最大断层
> 是 C API 缺 meta filter + README/门面漂移。

### A. 并发正确性（最优先）

- [x] **S13-F1【Critical·数据丢失】merge 重定位误用 `newest_put=true` + 收尾无条件 unlink** — 已完成（2026-07-02）
    · `src/merge/merger.cpp:229`、`src/keydir/keydir.cpp:539-556`、`src/cask/cask.cpp:2123-2148`
  - 交错：merge `output_id=N` → 并发 put 见 `active < N` roll → `biggest=N+1` → apply 时
    accept 条件 `N >= N+1` 恒假 → 全部冷 key 重定位被拒（kAlreadyExists）→ `Cask::merge`
    基于「CAS 失败=已被新写覆盖」的假设无条件 unlink 输入 → keydir 指向已删文件，
    **重启后 key 永久丢失**。`keydir.hpp:237-244` 注释本就写明 merge 应传 `newest_put=false`。
  - 修：① merger 改传 `false`（output_id > 全部输入 id，语义正确）；② 纵深防御：统计
    「CAS 门过但 accept 拒」条目，非零则跳过 unlink 留下轮 merge。
  - 验证：新增 `MergeConcurrentWriterTest.ConcurrentWriterRollDuringMergeNoDataLoss`
    （并发热写者持续 roll + merge 冷数据 → 进程内全可读 + close/reopen 后全可读 +
    `relocations_stuck==0`）。**反向验证**：临时改回 `newest_put=true` 该测试立即失败
    （抓到数据丢失），恢复后 4/4 过。落地：`merger.cpp` 改 false + `MergeStats` 加
    `relocations_stuck/stuck_file_ids`（复查 keydir 仍指旧位置才计）+ `Cask::merge`
    对 stuck 文件跳过 unlink/erase/trim。

- [x] **S13-F2【High·永久挂起】写路径失败泄漏 ord → reorder buffer 永久 stall** — 已完成（2026-07-02）
    · `src/cask/cask.cpp:1286-1316,1601,1660,1744`、`include/bitcask/thread_pool.hpp:354-362,521-565`
  - `write_and_keydir` 各错误路径（pwrite/hint/roll 失败、`pr2==kAlreadyExists` 双泄漏）
    return 前未对已 alloc 的 ord 提交 Skip → reducer `next_apply_ord` 永久空洞 →
    此后 flush/merge/close 全部在 `flush_cv_.wait` 永久阻塞（谓词永假）。一次 ENOSPC 即卡死句柄。
  - 修：ord RAII 守卫——析构时若未提交则自动 submit `IndexOp::Skip`（复用 S6-P1 机制）。
  - 落地：`cask.hpp` 新增私有 `OrdSkipGuard`（RAII，析构未 disarm 即 submit Skip），
    接入 put/remove/put_doc 三个 alloc 点 + `write_and_keydir` 内 ord2；`pr2==kAlreadyExists`
    双泄漏路径由双守卫覆盖。异常路径（bad_alloc 等）同样被 RAII 兜住。
    注：fault-injection 挂起测试留待（需 mock 文件层），守卫逻辑经全量回归验证无副作用。

- [x] **S13-F3【High·UB】`CaskIter::pin_files` 无锁读 `active_data_`（shared_ptr）** — 已完成（2026-07-02，shared_lock 内拍快照）
    · `src/cask/cask.cpp:217` vs 写方 `1083-1085,1140-1142,1168-1172`
  - 与并发 roll 对同一 shared_ptr 对象读写竞争（非控制块），TSan 必报，最坏解引用悬垂。
  - 修：`shared_lock(read_cache_mu_)` 内拍 `(active_data_ 副本, active_file_id_)` 快照。

- [x] **S13-F4【Medium·UB】`active_file_id_` 普通 u32，写者持 write_mu_/读者持 read_cache_mu_ 无 HB** — 已完成（2026-07-02，改 atomic + needs_merge 显式 relaxed load）
    · `src/cask/cask.cpp:1068,1172` vs `1207,1218,2008,2013-2015,217`
  - 修：改 `std::atomic<std::uint32_t>`（relaxed 足够，值仅作提示）。

- [x] **S13-F5【Medium】get 与 merge unlink 窗口 → 假 kIo** — 已完成（2026-07-02，get() 对 read_file 失败重查 keydir 重试一次）
    · `src/cask/cask.cpp:1375-1385` vs `2137-2148`
  - 读者先查 keydir 拿旧定位、后 open；merge 恰在其间 CAS+unlink → ENOENT 假失败。
    O10 的同临界区只保护已持句柄读者。修：`read_file` ENOENT 时重查 keydir 重试一次。

- [x] **S13-F6【Medium】tbb::concurrent_hash_map 并发插入时被整表遍历（TBB 不支持）** — 已完成（2026-07-02）
    · `src/bm25/inverted.cpp:196-205,299-305`、`src/search/search_layer.cpp:1192-1226,1259-1262`
  - `ensure_vocab`（查询线程）/ `compact`·`serialize`·`truncate_wal`（merge/close 线程）遍历
    与 reducer `add_doc` 插入并发；merge 路径破坏 S12-2「compact 仅在 reducer 内」前提。
  - 落地：① 新增 `IndexOp::RunFn` + `RunFnEntry`（thread_pool.hpp）——任意回调经
    reorder buffer 在 reducer 线程按 ord 序执行；`Cask::merge` 的 compact/
    compact_index_chunks 与 save_search_ckpt（含 truncate_wal）改经 RunFn 提交+flush，
    恢复 S12-2「遍历只在 reducer」不变量（close 路径本就在 unregister flush 之后，安全）。
    ② `ensure_vocab` 改增量：Shard 加 `vocab_delta_`（vocab_mtx_ 保护），add_doc 新词
    时记账（仅新词付锁，稳态零成本），重建 = vocab_ ∪ delta、不再遍历 map；deserialize
    直填路径同步记 delta；compact/finalize 的保守标脏移除（key 永不删除的不变量已注明）。
  - 验证：新增 `CaskDocValueTest.VocabConcurrentNewTermsAndMergeNoRace`（2 wildcard
    读者 + 400 新词写者 + 中途 merge；plain 验证 vocab 正确性，TSan 作竞态护栏）。

- [x] **S13-F7【文档】`cask.hpp:566-570`（merge 与 put 不兼容）与 `cask.hpp:16-18` / thread-safety.md §7.6（安全）自相矛盾** — 已完成（2026-07-02）
  - 统一为：KV 路径安全（F1/F5 已修）；索引模式注明 F6 未修前建议避免 merge 与高频 put_doc 并发。

- [x] **S13-F8【High·UAF·F1 修复揭出的存量竞态】fstats 无锁读与 deque 扩容的 UAF** — 已完成（2026-07-02）
    · `src/keydir/keydir.cpp`、`include/bitcask/keydir.hpp`
  - **发现**：新增的 F1 并发回归测试在 TSan 下抓出 heap-use-after-free——
    merge 线程 `update_fstats`（apply 成功路径，F1 修复前从未与写者并发执行过）
    无锁 `fstats_[idx]` 读，与写者 `emplace_back` 触发的 deque 内部块指针表
    重分配竞争。「deque 元素地址稳定」≠「operator[] 并发安全」：operator[]
    要遍历内部 map，而 map 会被扩容释放。
  - **修**：deque 保留为元素所有者（地址稳定），旁挂 RCU 指针表
    `fstats_ptrs_`——扩容在 `fstats_grow_mu_` 下建新表、release 发布、旧表
    退休不释放（在途读者可能持有；总内存 < 2×终表 ≈ 16B/file_id 有界）。
    全部无锁读点（update_fstats/info/save_snapshot）改经 `fstats_slot(idx)`；
    发布序「先指针表后 size」保证 acquire 读 size 的读者必见覆盖表。
  - **验证**：TSan 下 F1 测试 10 连跑 + 全并发批 199 项零告警（修复前 ~40% 复现率）。
  - **附带**：`cmake/tsan.supp` 增补 `mutable_pl` use_count+fence CoW 协议的
    fence 假阳性抑制（TSan 不建模 fence；协议论证正确，报告形状仅在读者已
    释放引用后出现——真并发时写者走克隆不触共享对象）。

### B. 内存 / 资源

- [x] **S13-M1【中】`bitcask_iter_next_batch` 错误中途 return -1 泄漏已填充条目** — 已完成（2026-07-02，错误分支逐条 free + 头文件契约注明）
    · `c_api/bitcask_c.cpp:725-769`、契约 `bitcask_c.h:486-494`
  - 修：错误分支 return 前对 `entries[0..count-1]` 逐条 `bitcask_iter_entry_free` 等价清理。
- [x] **S13-M2【低】C API malloc/strdup 返回值未检查（OOM → nullptr memcpy）+ extern "C" 未隔离异常** — 已完成（2026-07-02）
    · `bitcask_c.cpp:98-106,139-161,306,702-715,746-757,840-843`；全部导出函数缺 try/catch
  - 落地：31 个导出函数统一 `guarded`/try-catch 包裹（bad_alloc→ENOMEM、std::exception
    →detail 带 what()、其余→unexpected exception，均 BITCASK_ERR_IO）；`to_search_result`/
    `fill_get_result`/`fill_iter_entry`（新抽 helper，消 iter_next/next_batch 重复）/
    `needs_merge` 全部 malloc/strdup 检查 + 半成品清理；C API 测试通过。
- [x] **S13-M3【低·异常路径】5 处 fopen 与 fclose 间 vector 分配可抛 → FILE* 泄漏** — 已完成（2026-07-02，5 处全改 `unique_ptr<FILE, FileCloser>`；migrate.cpp 复用 field_schema 的 detail::FileCloser，其余局部定义）
    · `keydir.cpp:1242`、`hnsw.cpp:1318`、`inverted.cpp:1866`、`inverted_wal.cpp:409`、`migrate.cpp:46`
  - 修：照搬库内既有 `unique_ptr<FILE, FileCloser>` 模式（`field_schema.hpp:47` 先例）。
- [ ] **S13-M4【信息】`fstats_` 按 file_id 下标永不收缩**（长寿进程缓慢增长）· `keydir.cpp:196-200,254`
  - 低优先：考虑稀疏化或周期压缩。registry 目录记录只增为有意设计，不动。

### C. 性能（按性价比排序）

- [x] **S13-P1【高·一行级】搜索管线每写入清空整个查询缓存** · `src/search/search_layer.cpp:551` — 已完成（2026-07-02，reduce_apply 改 invalidate_terms，词集取自 job.fields[].terms + ca_data；缓存仅存 text 类查询，向量/meta 文档不影响正确性）
  - `reduce_apply` 末尾无条件 `cache_.invalidate()`，而 `on_write`(:436) 已有按词失效。
    所有 put/put_doc 走管线 → 混合负载缓存命中率归零。修：改调 `invalidate_terms`
    （`ReduceJob.fields[].terms` 已物化词集）。
- [x] **S13-P2【高·一行级】fsync→fdatasync、O_SYNC→O_DSYNC** · `src/io/posix_file.cpp:61,30-33` — 已完成（2026-07-02）
  - 追加写语义等价，每次持久化省一次 journal 元数据提交。**不改变 WAL 持久性契约**。
- [x] **S13-P3【高】`bool_search` posting 快照二次深拷贝** · `src/bm25/inverted.cpp:1378-1387` — 已完成（2026-07-02，make_move_iterator + reserve；all_tps 构建后 must/should_tps 不再使用）
  - 热词 ~1.7MB/词/查询。修：move 迭代器或 `vector<TermPostings*>`。
- [x] **S13-P4【高】`search_wand` 前置全量快照+live 填充抵消 BMW 跳块** · `inverted.cpp:505-528` — 已完成（2026-07-02）
  - 落地：每 term 加 `dls_filled` 位图（每 `kBlockSize=128` 一位），初始化
    只 resize 不填充；新增 `ensure_dls(tp, idx)` lambda 以
    `idx / kBlockSize` 查位按需 `fill_doc_lens` 整块；pivot 评分点前调。
    `live` 仍全量（IDF 用 live_df 是 BM25 分数位级不变约定）。走的是
    `bool_search` 既有 `ensure_block` 模式（:1099）。位级行为等价，
    无新回归。
- [x] **S13-P5【中】HNSW int8 每查询/插入堆分配 + 精排逐候选 madvise** — 已完成（2026-07-02）
  - `int8::quantize_into(v, dim, out)`：thread_local QVector 复用，查询/插入
    三个调用点稳态零分配（insert 两处 memcpy 即取即用，无别名）。
  - madvise：rerank 候选按地址排序 + 相邻页区间合并后批量 madvise——k=256
    时 ~768 次 syscall/查询降到典型个位数。
  - **有意不做**：SIMD round——`_mm256_cvtps_epi32` 是 round-half-to-even，与
    `std::round`（half-away-from-zero）在 .5 边界结果不同，codes 入 checkpoint，
    违反位级不变约定。
- [x] **S13-P6【中】`DataFile::fold` 每记录 2 次 pread** — 已完成（2026-07-02）
  - 照搬 hint_file 的 256KiB chunked refill（thread_local ThreadLocalBuffer + memmove
    残留 + 巨型 record 扩容 + maybe_shrink）。百万条 2M 次 pread → 数百次。
    短读改为 EOF-break（同 hint fold 语义；out_last_valid_end 仍正确供 caller 截断）。
- [x] **S13-P7【中】HNSW 读者 copy_neighbors 对节点自旋锁 atomic exchange → hub 缓存行乒乓** — 已完成（2026-07-02）
  - per-node 自旋锁改 **seqlock**（写者是单线程 reducer，无写-写互斥需求，只需
    发布协议）：写者 seq→奇 … 更新 adj … seq→偶（release）；读者双读 seq 一致
    才采信，torn count 有 cap 钳制防越界。数据字全走 `std::atomic_ref` relaxed
    ——UB-free 且 TSan 干净（无非原子冲突访问）。锁字 u8→u32 防 ABA 回绕。
  - 读侧 copy_neighbors **零共享行写**——hub 节点并发查询不再乒乓。
  - 补 `BM_Hnsw_SearchConcurrent`（100k/ef64 × Threads 1/4/8，UseRealTime）：
    实测 1→4 线程延迟持平（147→135µs）、8 线程 CPU 时间不涨（无锁争用）。
  - 验证：HNSW 15/15（含 recall）、TSan 并发批零告警。
- [~] **S13-P8【中批】** — 部分完成（2026-07-02，6/14 项落地）：
  - [x] 短语打分每候选堆分配 → `other_pos` thread_local（parallel_for 内免分配器争用）
  - [x] 短语驱动词选最稀有（"the quantum" 不再遍历 "the" 大表；候选集/分数/平分
    决策逐字节同果——两列表均 ord 升序，idf 仍取 first term 语义）
  - [x] `search_fuzzy` 并行化（镜像 wildcard 的 parallel_reduce；MyersMatcher::within
    const 纯计算无 mutable，跨线程安全）
  - [x] `on_delete` 空缓存跳过重分词（LRU 拷原文 + NFKC + 分词全免）
  - [x] C `bitcask_get` 双拷贝 → `fill_get_result_view` 直接从零拷贝 view malloc
  - [x] 新词 vocab 全量重建 → **已被 S13-F6 的 delta 增量设计覆盖**（不再遍历 map）
  - [x] FOR 解码 64-bit 窗口（2026-07-02）：MSB-first 流按大端 8 字节窗口
    一次移位+掩码取值，位级等价；bits>56 与尾部不足 8 字节回退逐 bit（越界
    安全兜底）。编码保持原样（输出位级不变）。inverted 78/78 含 ckpt 往返。
  - [x] `save_vec_payload` 流式（2026-07-02）：fseek 预留头区 → 逐页流式写
    + 累计页 CRC → 回补 header+CRC 表；文件字节与旧版一致（padding 走文件
    洞）。峰值内存从整 payload（1M×384d ≈ 1.5GB）降到头区+一页。
  - [x] hint 启动读两遍 → 单遍 `fold_validated`（2026-07-02）：整文件一次
    读入 → trailer CRC 判定（与 validate_trailer 逐字节一致）→ 内存解析
    回调；CRC 不过回调零次（keydir 零污染）。内存代价 = hint 文件瞬时缓冲
    （hint ≪ data）。
  - [x] `invalidate_terms` 反向索引（2026-07-02）：SearchCache 加
    `term_index_`（term→条目 hash 集，put/erase/invalidate/evict 同步维护），
    失效从 O(全部条目×词) 降为 O(变更词+受害条目)——每次写入持独占锁的热路径。
  - [x] meta filter 锁内求值（2026-07-02）：`Index::eval_meta(ord, filter)`
    shared_lock 内直接 evaluate（纯读无 IO），替代 `meta_blob` 的锁内堆拷贝
    ——materialize_hits（overfetch 4k 候选 = 4k 次拷贝/查询）与 HNSW 图内
    过滤 live 回调（每展开节点一次）两处受益。
  - [x] `search_fields` 死代码 + boost 分组搜索（2026-07-02）：删除从未使用
    的整体同义词扩展块；每 (词×同义词) 独立 top-k 改按 boost 分组一次多词
    search（内核调用 O(组)，组级 top-k 截断）。**行为改进**：跨词组合分高、
    单词排名 >k 被截丢的文档现在能进结果（synonym 11/11 回归通过）。
  - [x] merge `pending_` 分批 apply（2026-07-02）：每输入文件一批 + 批内
    256K 条阈值兜底巨型单文件，每批 flush+fsync 后才 apply（fsync-before-
    apply 契约逐批成立）。内存峰值从 O(全部活 key 字节) 降到 O(单文件/单批)。
    **失败语义修订**（替代 C1「keydir 完全未动」）：已 apply 批指向已 fsync
    的输出（合法可读，任何批 apply 过后输出保留不删——删即 S13-F1 同型丢失）；
    未 apply 的 key 完全不动，无中间不可读态；崩溃重启输出 file_id 最大按序胜出。
  - [x] `rebuild_hnsw` 结构化拷贝（2026-07-02）：新增 `HnswIndex::clone_live`
    ——保留原图层数/邻接，仅 id 重映射 + 死邻过滤，O(节点+边) memcpy 级、
    **零距离计算**（原从零重插每点 ~efC 次全维距离，100k 节点分钟级阻塞
    merge）。召回补偿：某层邻居全死时一跳路径收缩借道补边（去重限 cap）；
    极端删除下个别节点可能孤立，下轮 merge 自愈（已注明）。int8-only 直拷
    量化副本——顺带消掉旧路径的反量化→再量化往返（audit 同批项）。
    验证：V4 rebuild 端到端（删半→merge→图大小/ckpt 往返/搜索排除已删）
    11/11 + checkpoint recovery 5/5。
  - **S13-P8 至此 14/14 全部闭环。**

### D. 功能缺口（对照已规划 C4/C5/C6 与已否决项排除后）

- [x] **S13-D1【P0·M】`put_batch` 原子批量写** — 已完成（2026-07-02）
  - `Cask::put_batch(span<BatchItem>)`：全批前置校验（零副作用）→ write_buffered
    聚合（1MiB 块 pwrite）→ 单次 flush（+按 sync 策略组提交：o_sync 即时 /
    sync_every_n>0 整批一次 fdatasync / 否则同单条 put 由 caller sync() 控制）→
    keydir apply + 索引提交（**flush 之后** ⟹ 本进程内 all-or-nothing 可见）。
  - merge race（merge 恰在批写入期启动）：被拒条目走 write_and_keydir 单条重写
    （内部 roll+重试），原始 ord 由 BatchOrdGuard（S13-F2 批量版）补 Skip。
  - 不提供跨崩溃原子性（磁盘可能残留批前缀，重启 fold 后可见——与连续单条 put
    的崩溃语义一致），契约在头文件注明。write_buffered 使用契约注释同步放宽
    （「flush 后才采信」的第二个合法使用方）。
  - C API：`bitcask_kv_pair_t` + `bitcask_put_batch`（additive）。
  - 测试：`PutBatchBasicAndPersistence`（200 条 + 校验零副作用 + LWW 覆盖 +
    reopen 持久性）/ `PutBatchIndexedSearchable`（异步管线入索引）/ C 端
    `test_put_batch`。
- [x] **S13-D2【P0·M】C API 暴露 meta filter** — 已完成（2026-07-02）
  - `bitcask_meta_filter_t`（条件数组 + logic_or + 嵌套 children 数组）+
    `bitcask_meta_condition_t`/`bitcask_meta_value_t`（全部 8 算子 + 5 值类型），
    指针借调用方存储、调用返回即可释放。新增 **additive** 三函数（ABI 兼容）：
    `bitcask_search_text_filtered` / `bitcask_search_vector_filtered` /
    `bitcask_search_hybrid_filtered`；非法 filter（NULL key/缺 str/深度>32/枚举越界）
    → INVALID_OPTION。C 测试 `test_search_filtered`（8/8 过）。
  - 备忘：头文件注明引擎「无 meta 文档 filter 下一律不通过」语义；批量 `_filtered`
    变体与 C API 前缀扫描留待后续（薄包装）。
- [x] **S13-D3【P0·S】`search_text_highlight` 提上 Cask 门面 + 修 README 漂移** — 已完成（2026-07-02）
  - `Cask::search_text_highlight`（closed fail-fast + prepare_search flush + search_fault
    翻译，返回 `HighlightSearchResult{vector<SearchHitEx>}`）；README 第 29 行宣称从此为真。
  - 测试：`SearchTextHighlightViaCaskFacade`（端到端高亮片段 + close 后 kClosed）。
- [x] **S13-D4【P0·S】前缀扫描** — 已完成（2026-07-02）
  - `CaskIter::start` / `Cask::parallel_scan` 加尾置默认参 `key_prefix`（源兼容），
    过滤在 keydir proxy 层（非匹配 key 零 pread 零拷贝）；next/drain_live_keys 同步。
    测试 `PrefixScanIterAndParallelScan`（前缀/边界/空前缀回归）。C API 暴露留待后续。
- [x] **S13-D5【P1·M】per-key TTL** — 已完成（2026-07-02）
  - 格式：`kFlagHasExpiry=0x20` + value 末尾 `[ExpiryAt:u32 LE]`（绝对 unix 秒）。
    段在既有全部段之后 ⟹ **旧读端按位忽略、静默降级为永不过期**（非拒绝）。
  - API：`put(..., expiry_at=0)` 尾置默认参 / `DocInput::expiry_at`；C 端
    `bitcask_put_ex` + `bitcask_doc_input_t.expiry_at`（追加字段）。
  - 读路径：get（mmap/pread 双路径）过期 → kNotFound；CaskIter::next 跳过。
    与整库 expiry_secs 叠加（任一判过期即过期）。
  - merge：`run_merge` 加 `now_sec` 参——过期记录不搬运 + `conditional_remove`
    CAS 清 keydir（位置匹配才删，与并发 put 无冲突）+ `records_expired` 统计。
  - **未做（有意）**：per-key TTL 不参与 needs_merge 的过期触发（需全量扫描
    记录才能统计，fstats 无此信息）；put_batch 暂不带 TTL（后续薄扩展）。
  - 测试：`PerKeyTtlExpiryAndMergeReclaim`（三类 key + iter 过滤 + merge 回收
    计数 + 重启不复活）。
  - 原案：DocValue v3 已版本化，加可选 expiry；merge policy 同步扩展。
- [x] **S13-D6【P1·M】备份/热拷贝 API** — 已完成（2026-07-02）
  - `Cask::backup(dst_dir)`：持 write_mu_ 封存 active（finalize hint，write_lock
    不释放、下一次 put 惰性重建）→ hardlink（跨设备回退 copy）data/hint +
    bitcask.meta + field.schema + keydir/search ckpt（可选）。sealed 不可变 ⟹
    hardlink 即一致快照。契约：caller 保证与 merge 不并发（同 merge 单实例约束）。
  - 测试：`BackupHotCopyAndReopen`（多文件备份 → 独立只读 open 全量校验 +
    原库备份后继续可写）。
- [x] **S13-D7【P1·S/M】日志回调 hook** — 已完成（2026-07-02）
  - `CaskOptions::log_fn`（`function<void(LogLevel, string_view)>`，open-time 不可变，
    沿 synonym_map 模式）+ `LogLevel{kWarn,kError}`；`Cask::log/log_warn/log_error`
    noexcept（回调抛出被吞）。插桩 6 点：keydir 快照保存失败、write.lock active-path
    更新失败、merge 后 search ckpt 保存失败（RunFn/fallback 双路径）、stuck 重定位
    （S13-F1 防御路径触发）、索引 worker 吞异常、close 兜底 catch。
  - C API：options 追加 `log_fn(int level, const char* msg, void* ctx)` + `log_ctx`。
  - 测试：`LogHookFiresOnSnapshotFailure`（只读目录触发 close 快照失败 → warn）。
  - 原案文字：`CaskOptions::log_fn`（open-time 不可变）+ C 函数指针；
  ~10 处静默失败点插桩。
- [x] **S13-D8【P2·S】统计 API 扩展** — 已完成（2026-07-02）
  - StatusInfo 加 `hnsw_nodes`/`search_cache_entries`/`read_handles`（全部经线程安全
    访问器）。**有意不含** total_postings：统计需遍历 concurrent_hash_map，与 reducer
    插入并发不安全（S13-F6 同类）——待 InvertedIndex 原子计数器后再暴露（已注明）。
  - C API：additive `bitcask_status_ex_t` + `bitcask_status_ex()`（旧 struct 布局不动）。
- [x] **S13-D9【P2·M】查询语言括号嵌套+引号短语子句** — 已完成（2026-07-02）
  - 语法：`+(rust go) +web`、`+"rust systems"`、`-"exact phrase"`，任意嵌套；
    容错（未闭合引号取到尾、多余括号忽略）。
  - 实现：`parse_query_tree`（递归下降，query_parser.cpp）+ QueryNode 加
    `is_phrase/phrase_terms`；执行侧新增 `InvertedIndex::bool_search_tree`
    （集合式：组内 MUST 交/SHOULD 并/MUST_NOT 差，短语叶复用 search_phrase
    内核；候选按全部正向词 BM25 求和 top-k，term 叶乘 boost）。
  - **零风险路由**：仅查询含 '(' 或 '"' 走树路径——既有扁平查询走原路径，
    行为位级不变。缓存词集含短语成分词（collect_terms 扩展），按词失效正确。
  - 测试：`BoolSearchTreeSyntax`（组合/嵌套/短语排除/容错/扁平回归 6 例）。
- [x] **S13-D11【P2·S】HNSW M/ef_construction 透传** — 已完成（2026-07-02）：
  `SearchLayerConfig::hnsw_m/hnsw_ef_construction`（0=默认），构造与 merge 期
  rebuild（复用 old->config()）均生效；C options 同步追加；不入 meta 校验
  （调优参数非格式参数，已注明）。测试 `StatusExFieldsAndHnswParamPassthrough`。
- [x] **S13-D10【P2·S/M】搜索分页 offset** — 已完成（2026-07-02，offset 部分）
  - `search_text/search_phrase/bool_search` 加尾置 `offset=0`：facade 层
    overfetch k+offset 后截断（深分页线性成本，已注明）。测试
    `SearchTextPaginationOffset`（切片一致性 + 越界空页）。
  - **total 估计有意不做**：WAND/BMW 剪枝下只能给下界，误导大于价值；确需
    精确 total 的场景等 C6 Roaring（已规划）位图求交后自然获得。C API 分页
    变体留待后续薄包装。
- ~~S13-D11~~（见上）HNSW M/ef_construction 透传**。

> **建议执行顺序**：F1（数据丢失）→ F2（永久挂起）→ F3/F4（TSan 干净化）→ P1/P2（两处
> 一行级高收益）→ M1 → F5/F6 → F7+D3（文档漂移一并修）→ 其余按价值推进。
> F1/F3 均配 build-tsan 并发压测验证。

---

## 待办：第十四梯队（S14 checkpoint 增量化 — 2026-07-03）

> 来源：P1/H1 修复后的崩溃恢复讨论（s13-review §P1 后续）。现状 search.ckpt 只在
> 干净 close / merge 收尾保存，长跑库崩溃后重放全部历史（10M 库重分词 + HNSW 串行
> 重建可达小时级）。一次全量 ckpt 成本 ∝ 索引总量（10M/128d 估算 5–15GB：postings
> GB 级 + docmap ~1GB + hnsw 邻接 ~1.3GB + int8 码字 ~1.3GB + .vec f32 ~5GB）——
> 周期化必须配增量化，否则 roll 点 cadence 下写放大 5–10×。
>
> **方向判定（已论证，见 s13-review §P1 后续）**：hash 分片（term/key）对增量化
> 收益恒为零（一个文档的 terms 散到所有分片 → 每写全脏）；**ord 时间轴分段**才是
> append-only 负载的正确切法（写只脏 active 段）。**回收 = merge 点 rebase**（复用
> `needs_merge` 的 dead_doc_rate 触发 + compact/rebuild_hnsw 的 reducer 静止点），
> append 文件不打洞、整体重写——与 data 文件同生命周期哲学。**不引入逐条索引落盘**
> （那是路线 A 砍掉的 bm25 WAL 双重日志回魂）；索引侧磁盘写只在 ckpt 点整批发生。
> 依据：`doc/recovery-unified-checkpoint-design-zh.md`（路线 A）§5。
>
> **前置已完成（2026-07-03，本批前落地）**：
> - [x] ① open 恢复后回存：fold 重分析 ≥ `kPostRecoveryCkptMinDocs`(1000) 时 open
>   内直接回存 keydir 快照 + search.ckpt（重建成果落盘，再崩不全价重付）。
> - [x] ② `Cask::checkpoint()` 公开 API：RunFn 在 reducer 线程序列化（S13-F6 机制），
>   tri-state 原子只等自己的 RunFn（不等 flush(lane)，持续写入下等待有界）；
>   与 close 竞态由 H1 WriteOpGate 收敛。

- [x] **S14-1【P0·S】roll 封口点异步自动 checkpoint（cadence 机制）** — 已完成（2026-07-03）
  - `CaskOptions::auto_checkpoint_min_docs = 0`（0=关闭，默认零行为变化）。
    roll_active 置 pending 标记（原子）；写路径释放 write_mu_ 后的锁外提交点
    （maybe_submit_auto_checkpoint，WriteOpGate 持有中 → close 竞态安全）检测：
    增量（peek_next_ord − last_ckpt_ord_）≥ 阈值且无在途 → fire-and-forget 提交
    RunFn，**不阻塞任何写者**。仅索引模式生效（KV 恢复本就走 hint 快路径）。
    防重入：`auto_ckpt_inflight_` 原子标志，RunFn 完成时清；last_ckpt_ord_ 于
    open 末尾以当前水位初始化（老库首个 roll 不触发无谓全量 ckpt）。
  - **落地中抓出并修复一个成对性反转 bug**：write_keydir_snapshot 若在 reducer
    的 RunFn 执行时刻取字节水位，会被并发写者推进到超过 search.ckpt 的 ord 覆盖
    （破坏路线 A §4「keydir_covered ≤ search_covered」保存序不变量）→ 下次 open
    fold 从超前水位起跳，search 永久丢失 [ckpt_wm, 快照时刻) 区间。修法：
    `collect_snapshot_watermarks()` 拆分——字节水位在**提交时刻**（writer 侧，
    先于 wm 捕获）取，快照本体仍在 reducer 写（entries 较新无害，fold 幂等）。
    manual checkpoint() 的 RunFn 同步修正（其快照本批被移进 RunFn，统一 ckpt
    文件写到 reducer 单线程、消除 .tmp 并发写窗口；merge 收尾快照仍在 caller
    线程——flush 前捕获，本就满足不变量）。
  - 测试：`AutoCheckpointOnRoll`（roll 自动落成对文件 + 崩溃镜像重开 400/400
    可检索——该测试正是抓出水位反转的现场）、`AutoCheckpointDisabledByDefault`
    （默认零行为变化）、`CheckpointConcurrentWithWrites` 增开小文件 roll + 自动
    ckpt（手动/自动 RunFn 与并发写在 reducer 交错，TSan 护栏）。
    clang 507/507、TSan 505/505（排除项为既知预存问题）。
- [x] **S14-2【P0·M】search.vec 按 id 追加化（最大单点收益）** — 已完成（2026-07-03）
  - 实现与原设计的偏差（帧链 → **前缀不变契约**，更简且白得 .prev 修复）：
    数据区必须保持连续定长寻址（`vecs_mmap_base_ + id*dim` 查询路径不动），
    帧头无法内嵌 → 改为「文件 = header + 连续数据区」，有效向量 = ckpt
    （BVH2 段）声称的 [0, n) 前缀，header.count 允许 ≥ n。追加只写
    offset ≥ 旧数据尾的字节 → torn append 恒不伤前缀；顺序 数据 pwrite →
    fdatasync → header 原地重写（version=2、不再维护从未被 load 校验的页
    CRC 表）→ fdatasync → ckpt 原子发布。
  - 追加目标身份用 dev/ino 追踪（`VecFileState`，与 mmap 解耦）：load 与
    全量重写成功后收养；身份不符/前缀缺损/IO 失败退全量重写兜底（新建/
    rebuild 后的索引自动走全量 = merge 点 rebase，回收死向量，无需新机制）。
  - **副产品修复**：load 的 count 等值校验放宽为 ≥（前缀契约）——旧版下
    `.prev` 回退必然拒载 .vec（新代已重写文件）→ HNSW 全量重建；现在旧代
    ckpt + 更长的 .vec 正常装载。
  - **int8 码字不在本批**：qcodes 与可变邻接表逐节点交错在 BVH2 段内，
    拆出独立追加文件只有在邻接段低频化（S14-5）后才有净收益——并入 S14-5。
  - 测试：`HnswVecAppend.AppendRoundTripPrefixContract`（追加轮回 + inode
    稳定 + v2 header + .prev 等价装载 + 截短拒载）、
    `CaskDocValueTest.CheckpointVecPayloadAppends`（两次 checkpoint 间
    inode 不变/尺寸精确增长 + 崩溃镜像重开追加区向量可检索）。
    clang 509/509、TSan 507/507。
- [x] **S14-3【P1·M】段级 dirty-bit + 旧段字节前移（路线 A §5）** — 已完成（2026-07-03）
  - SearchLayer 四个 relaxed 原子脏位（docmap / bm25.default / bm25.fields /
    hnsw，初值 true=未知即重序列化）。**标记点**（全部变异入口）：reduce_apply
    （docmap 恒触；bm25 按实际写到的域**精确**标——recover 走单默认域不误脏
    fields 段）、on_write（docmap 恒触；bm25 仅真有词项时——向量-only 文档
    不碰 bm25）、on_vector/rebuild_hnsw（hnsw）、on_delete（docmap + 两个
    bm25：remove_doc 调整各域 N/sdl 全局统计）、on_relocate/recover_tomb/
    compact_index_chunks（docmap）、compact（两个 bm25）。
  - **清位点**：save 成功后清全部（save 在 reducer/静止点内，无并发置位
    窗口）；load 成功载入的段亦清（此刻内存 == 文件字节，新增 per-type
    载入标记 default_sec_ok/fields_sec_ok）——重启后首个 ckpt 即可享受
    carry，fold 尾部重放只重新弄脏真正变过的段。
  - **前移机制**：`SearchCheckpoint::read_selected(path, want)`——按页脚目录
    fseek 只读选中段（不为搬运干净段读整文件），坏段/缺段自动回退重序列化。
    hnsw 干净时同时跳过 save_vec_payload（配合 S14-2：无向量写周期图序列化
    + .vec 全部零成本）。carried docmap 的旧 covers_next_ord 较小——自门
    方向安全（fold 多放重叠区幂等丢弃）。
  - 测试：`CaskDocValueTest.CheckpointSectionCarry`——三阶段双向验证：纯文本
    增量期 hnsw 段与上代**逐字节相同**（carried）、纯向量增量期 bm25 段逐
    字节相同，且崩溃镜像重开后 carry 段与 fresh 段合成完整状态（文本 100/100
    + 向量抽查 self-top1 全中）。clang 510/510、TSan 508/508。
- [x] **S14-4【P1·L】ord-delta 链（bm25/docmap/hnsw）+ rebase** — 已完成（2026-07-03）
  - **前置修复（成对写序崩溃窗口，pre-existing）**：所有保存点原为「先 keydir
    快照后 search.ckpt」——两写之间崩溃留下「新快照+旧 ckpt」，fold 从超前
    字节水位起跳、search 永久丢窗口。对调为「先 ckpt/delta、成功后才写快照」
    （checkpoint()/自动/①/merge 四处；merge 早段快照改为捕获水位、延后落盘），
    崩溃任何前缀只留「旧快照+新 ckpt」（fold 多放、自门幂等）。
  - **delta 链**：`search.ckpt.d<seq>` 独立小文件（BCSC 容器复用，段型 7-11），
    base 不重写——写 I/O 从 ∝ 索引总量降到 ∝ 窗口增量。
    · bm25 delta（`serialize_delta/apply_delta`，"BIVD" 逐条编码）= 每 term
      的 ord 后缀 + 绝对 N/sdl（删除只改统计不碰 posting）；apply 尾部追加 +
      per-item ord 守卫幂等 + note_appended 增量封块 + vocab_delta_ 记账。
    · docmap delta = 窗口 live 行（for_each_live_in 范围提取）+ 删除日志
      (key, tomb_ord)，**按 ord 交错重放**（删后重写不误杀）；覆盖写靠
      put_doc 同 key 杀旧；relocate 只在 merge=rebase 点，永不进 delta。
    · hnsw delta = 插入日志 (ord, f32)（图无不可变旧段，重放 insert，
      自带 ord 水位门）；delta 路径不碰 .vec（向量内联，.vec 追加留给 base）。
  - **rebase**：compact/rebuild_index/rebuild_hnsw 置标志 → 下次 save 全量
    base + 链清扫（merge 恒 compact ⇒ merge 即 rebase 点，零特殊分支）；
    **close 强制 rebase**（干净关闭收敛单一 base，.prev 代际刷新，链不跨干净
    重启累积——链的存续范围 = 两次 base 间的运行期窗口）。
  - **防陈旧四层**：base_gen(=base wm) + prev_wm 链校验 + rebase unlink
    （8 空洞 orphan 扫尾）+ apply 幂等守卫；坏 delta（存在但无效）→ 整链判
    不健康退全量 fold（字节水位可能超前于链覆盖，必须放弃快路径）。
  - **`.prev` 回退加固（pre-existing 洞）**：CkptLoadResult 加 `from_prev`，
    cask 快路径门禁 `!from_prev`——回退旧代时磁盘 keydir 快照可能与坏掉的
    新代成对（水位超前），旧行为直接吃字节水位会漏喂 [prev, 快照) 区间。
  - 测试：`CheckpointDeltaChainDeletesAndOverwrites`（跨窗删/覆盖写/写后删/
    删后重写交错重放 + 多 delta 链 + 崩溃镜像重开 + 重开续链 d3 + close
    坍缩清扫）、`CheckpointDeltaChainSelectiveSections`（脏标记驱动段选择：
    纯文本窗口无 hnsw delta、纯向量窗口无 bm25 delta、base 逐字节不动）、
    `CheckpointVecPayloadAppends` 更新（delta 不碰 .vec，close 的 base 走
    S14-2 追加）。clang 512/512、TSan 510/510。
  - **已知后续**：每次 delta 保存仍全量写 kv.keydir.ckpt（O(live keys)，
    10M 库 ~0.5–1GB）——现在它成了 per-save I/O 的大头；keydir 快照增量化
    另立任务（S14-7 候选）。
- [x] **S14-5【P2·M】HNSW 邻接低频保存 + 追平（经 S14-4 重估后收敛）** — 已完成（2026-07-03）
  - **重估结论：原方案两个目标已被 S14-4 链架构天然实现**——
    「邻接段低频保存」= delta 保存完全不序列化 BVH2，图只在 base 点
    （merge/close/rebase）落盘；「载入尾部追平」= hnsw delta 插入日志重放
    （带向量内联 + insert ord 水位自门），比原设想的「从 .vec 追平」更完备
    （.vec 无 ord 映射，本就追不了）。原「每 K 次 ckpt 存图」的混频水位
    机制不再需要。
  - **本轮落地：链长上限自动 rebase**（S14-4 的遗留缺口）——不 merge 的纯
    追加负载（无删除 ⇒ needs_merge 不触发）链随写入量线性堆积、永不回收，
    向量库尤甚（每 delta 内联 f32）。`SearchLayerConfig::max_delta_chain`
    （默认 64，0=不设限）：链达上限 → 下次 save 强制全量 base、链坍缩回收。
    上限权衡已注释（小 → base 重序列化频繁；大 → 恢复重放长 + 磁盘冗余）。
  - 测试：`CheckpointDeltaChainLengthBound`（上限 2：d1/d2 → 第 4 次 ckpt
    强制 base + 链清扫 + .prev 刷新 + 重新起链 + 崩溃镜像重开 50/50）。
    clang 513/513、TSan 511/511。
  - **int8 码字拆分（S14-2 递延）→ 另立 S14-8 候选**：qcodes 逐节点嵌在
    BVH2 内，拆出 append-only 文件（qc8，S14-2 的 .vec 前缀契约同构）可
    减半非 rebuild 的 base 保存（close / 链满 rebase）的 hnsw 序列化；但
    merge 的 rebuild 重映射 node id 后必须全量重写 → 收益面 = 纯追加负载的
    链满 rebase 周期。BVH2 v3 格式手术 + 双模式（含 inmem_int8）兼容，
    等链满 rebase 的真实频率数据出来再定 ROI。

- [x] **S14-6【P0·M·BUG】fold 增量重放丢弃命名字段索引（bm25.fields）—— 崩溃恢复正确性** — 已完成（2026-07-03）
  - **现象**：最后一次 search.ckpt 之后、crash 之前写入的多字段文档，重启 fold 重放后其
    命名字段索引（bm25.fields）在任何地方都不存在 →「`title:foo`」类字段限定查询漏掉这些
    文档；连默认域 catch-all 也一并丢（仅存在于字段值、不在 `doc.text` 里的词，全文检索
    也命不中）。**不是数据丢失**：字段值仍在磁盘 DocValue 的 fields 段（可 decode），纯属
    索引重建缺口——但会**固化**（下次 `save_search_ckpt` 把不完整索引序列化下去）。
  - **根因**：恢复路径完全不消费 `dv->fields`。
    - `RecoverDoc` 结构体无 fields 成员 —— `include/bitcask/search_layer.hpp:331-340`
    - `recover_doc_batch` 硬编码 `fields = {{kDefaultField, d.text}}` —— `src/search/search_layer.cpp:932-933`
    - fold 解出 `dv->fields` 后只拷 `dv->text`，字段段直接丢弃 —— `src/cask/cask.cpp:1012-1033`
    - 对照活写路径 `map_analyze(task.fields)` 正常建 per-field 词表 + catch-all 合并进默认域
      —— `src/cask/cask.cpp:568`、`src/search/search_layer.cpp:496,536`
  - **窗口边界**：`fold_start = snap_loaded ? wm_of(file) : 0`（`cask.cpp:956`）。
    `ord ≤ ckpt watermark` 从 search.ckpt 反序列化（含 bm25.fields 段，完好）；
    `ord > watermark`（增量窗口）只能靠 fold → 丢。**checkpoint 不健康时更糟**：
    `all_segments_ok=false → snap_loaded=false → fold_start=0`（`cask.cpp:1152-1155`）→
    **全量 fold → 全库命名字段索引丢失**，不止窗口。
  - **修法**：`RecoverDoc` 加 `fields` 成员；fold 时把 `dv->fields` 各 `FieldId` 经
    `field_schema_.name_of(id)` 还原成名字塞入；`recover_doc_batch` 把命名字段一并喂
    `map_analyze`（与活写路径对齐，per-field + catch-all 都自然复原）。
  - **⚠️ 会触发「field.schema 悬空 id」**：恢复一旦调 `name_of(id)`，掉电场景下丢失的
    field.schema 尾条映射（intern 只 fflush 未 fsync，与数据文件落盘无序）会让
    `name_of` 返回 `nullopt`（`field_schema.hpp:155`，越界安全但需处理）→ 定策略：
    **跳过该字段 + 计数告警**（降级，与当前「丢」同级但更收敛可观测），或 fail-fast。
  - **落地**（2026-07-03）：`RecoverDoc` 加 owning `fields` 成员；fold 解出
    `dv->fields` 后经 `field_schema_.name_of(id)` 还原名字（悬空 id 跳过该字段 +
    fold 后聚合 log_warn 一次，可观测不刷屏）；`recover_doc_batch` 非空 fields 时
    镜像活写路径（map 只喂 fields、text 不参与索引——与 put_doc 的 task.fields
    装配一致），per-field 词表 + catch-all 在 map_analyze 内自然复原。
    **同族追加修复**：recover 门 `(!text.empty() || has_vec)` 会把纯命名字段文档
    （text 空、无向量）整个拒之门外——连 docmap 都不恢复、live 过滤当死文档；
    门补 `|| dv->has_fields`。
  - 测试：`RecoverPreservesNamedFields`——两分支崩溃镜像（无 ckpt 全量 fold /
    ckpt 健康 + 增量窗口），`search_fields` 与 catch-all `search_text` 全命中；
    **反向验证**：临时禁用修复该测试立即失败。clang 511/511、TSan 509/509。

- [x] **S14-7【P1·L】keydir 快照增量化（delta 内联元数据 + 链重放推进）** — 已完成（2026-07-03）
  - **核心洞察：搜索 delta 里已含 keydir delta 的主体**——docmap 行
    (ord/key/file_id/offset/total_sz/tstamp) 恰是 keydir entry 全字段，
    删除日志亦然。keydir 独缺的只有小件：字节水位 + 单调标量
    （next_ord/epoch/biggest）+ fstats——全 KB 级。
  - **保存**：delta 路径不再全量写 kv.keydir.ckpt（曾是 delta 时代 per-save
    I/O 大头，~0.5–1GB @10M）；delta 文件新增 `kKeydirDelta` 段（"BKMD"，
    提交时刻捕获）——搜索与 keydir 推进**同文件原子成对**，增量路径的写序
    窗口彻底消失。base 路径照旧全量快照。全部保存点统一收口到
    `Cask::save_search_ckpt_paired`（写序不变量集中一处）。
  - **载入**：重排为 keydir base 快照**先**载 → 链重放经 `DeltaReplayHook`
    同步推进 keydir（行 → LWW put；删除 → 新增 `KeyDir::remove_if_older`
    ——ord 守卫使「删后重写」顺序无关）→ 字节水位取链尾
    （`apply_meta_delta` 返回）。SearchLayer 不依赖 KeyDir：只透传解析
    结果，分层不破。
  - **计数策略（推敲后）**：key_count/key_bytes **不入 delta**——行/删除
    重放让 keydir 原生 ++/-- ，精确且免提交时刻的并发漂移；fstats 绝对
    覆盖（advisory 精度，merge/close 全量快照定期校准）；标量取 max 单调。
  - 测试：`CheckpointDeltaChainDeletesAndOverwrites` 扩展——delta 保存
    kv.keydir.ckpt mtime 不变 + 重开后 KV get() 全语义断言（跨窗删除无
    僵尸/删后重写不误杀/窗口新增可读）；**反向验证**：断开链推进钩子，
    僵尸断言立即抓到。clang 513/513、TSan 511/511。

- [x] **S14-8【P2·M】int8 码字外置（search.qc8 + BVH2 v3 + payload 代号）** — 已完成（2026-07-03）
  - **ROI 决定性场景**：高维 int8-only 部署（P5c 按 qwen3-embedding dim=2560
    校准）——码字 10M×2560B ≈ 25.6GB，占 BVH2 的 ~95%，此前每次 close/链满
    rebase 全量重写；外置后变窗口追加。
  - **格式**：`search.qc8`（BCQ8 v1，64B 头 + 定长 stride=dim+8 记录区，按
    node id 索引）——S14-2 .vec 的同款前缀契约/dev-ino 身份收养/追加机制；
    BVH2 升 v3：段内仅 ord/level/邻接（邻接可变无法外置）+ payload 代号，
    flags bit1 = 有 qc8。v2 旧文件照常载入（内嵌码字路径保留）；
    needs_qcodes_ 为假（kL2/无 int8 内核）不产 qc8。rebuild → 新对象无追加
    状态 → 自动全量重写（= rebase 收缩，与 .vec 同构）。
  - **payload 代号（gen nonce）——顺手闭合 S14-2 同型隐患**：rebuild 全量
    重写 payload 后走 .prev 回退时，旧图配新 payload（node id 已重映射）而
    「前缀 count ≥ n」检查会**错误接受**（v1 时代靠等值检查偶然安全，S14-2
    放宽为 ≥ 后暴露）。v3 段头与 .vec/.qc8 头共同携带 gen，双方非零即配对
    校验；legacy gen==0 跳过（首次 rebuild 后自动进入保护）。
  - **顺手修复**：backup 清单此前漏 `search.vec`（缺失仅降级 fold 重建，但
    备份目录首次 open 付全量重建）——.vec/.qc8 一并纳入；delta 链文件刻意
    不带（备份点 base+快照自洽）。
  - 测试：`HnswQc8Append.AppendGenGuardRoundTrip`（inmem_int8 纯 qc8 路径：
    追加 inode 稳定 + 尺寸精确增长 + v3 轮回检索 + **gen 守卫**——旧代
    ckpt 配重建后 count≥n 的 qc8 拒载）；既有全部向量测试自动覆盖 v3+qc8
    端到端。clang 514/514、TSan 512/512。

> **建议执行顺序（全批完成）**：S14-1 ✅ → S14-2 ✅ → S14-3 ✅ → S14-6 ✅ →
> S14-4 ✅（含成对写序修复 + .prev 加固）→ S14-5 ✅（重估收敛为链长上限）→
> S14-7 ✅（keydir 增量化收口）→ S14-8 ✅（qc8 外置 + gen 配对）。
> **S14 批次全部收官。**每步独立可交付；S14-1 落地后 ②③ 即形成完整的「手动 + 自动」
> ckpt 节奏。

## 待办：第十五梯队（S15 插件化架构 P1 — 接口化 + thread_pool 去搜索化，2026-07-03）

> 来源：`doc/plugin-arch-split-design-zh.md`（插件化架构拆分设计）§9 迁移阶段 P1。
> 总目标（全五阶段）：依赖方向反转——Cask 只认识 `IndexPlugin` 回调接口，
> BM25/HNSW 拆为互不感知的独立插件，解锁 KV-only / KV+BM25 / KV+HNSW 三种
> 发布形态。本批 = P1，**零行为变化**：把现有 IndexPool 流水线的 lambda 接缝
> （cask.cpp:562-608）固化成正式接口，SearchLayer 原样套 adapter 当「唯一插件」。
>
> **方向判定**：不发明新并发模型——map 并行纯函数 / reducer 单写者按 ord 序的
> 现有 TSan-clean 契约原样接口化（设计 §3.1），reorder buffer / 保序机制一行不动。
> 改动只发生在 payload 类型层（variant 塌缩 + 类型擦除）。
> **与 2026-06-25「god class 拆分搁置」决策（本文件 :544-549）不冲突**：那次否决
> 的是依赖方向不变的类内美学拆分；本批是依赖反转的第一步（能力变化，见设计 §2.3）。
> **完成判据**：`thread_pool.hpp` 不再 include `search_layer.hpp`；IndexPool 通路
> 全部经 `CaskPlugin` 接口分发；clang/TSan 全量回归零差异；put_doc bench 回退 ≤3%。
> **接口词汇（v2，2026-07-03 评审修订）**：动词全部来自 KV 固有事件
> （open/close/on_put/on_delete/on_relocate/maintain/flush），并行预处理降级为
> 可选能力（wants_prepare/prepare），恢复重放复用 on_put + 水位自门、无 recover_*
> 专用动词，查询不进通用接口。签名以设计 §3.1–§3.5 为准。
> P1 不动 Cask 门面/恢复编排/checkpoint（那是 P3/P4/P5 的事），Cask 仍持
> `unique_ptr<SearchLayer>`。

- [x] **S15-1【P0·S】plugin_api 接口层（`bitcask_plugin_api` INTERFACE 目标）** — 已完成（2026-07-03）
  - 新增 `include/bitcask/plugin_api.hpp`（KV 事件词汇，设计 §3.1/§3.2 签名为准）：
    数据类型 `RecordLoc` / `FieldKV` / `DocView{text, fields, vec, meta}` /
    `PutEvent{ord, key, value, doc*, loc, tstamp}`（原始 value 为主、DocView
    为结构化附件，纯 KV 写时 doc=nullptr；全 view/span 语义，生命周期 =
    回调期间）/ `DeleteEvent` / `RelocateEvent`（含 value 视图——merge fold
    正持有记录缓冲，零成本附带，供插件借 merge 的 I/O 做影子重建）/
    `MergeBeginEvent` / `MergeCommitEvent` / `MaintainEvent` /
    `FlushRequest/FlushResult` / `Prepared`（虚基，prepare 相产物）；
    接口 `CaskPlugin`（name / open / watermark / close / on_put / on_delete /
    wants_prepare / prepare / on_relocate / on_merge_begin / on_merge_commit /
    on_merge_abort / maintain / flush——merge 三事件默认空实现，参与协议见
    设计 §3.9：插件在 merge 线程同步收事件、经影子构建+原子发布或
    run_serialized 安全变异，收尾 GC 先于宿主成对保存点靠 RunFn FIFO 保序）、
    `PluginHost`（read_at / **run_serialized**（reducer 静止点串行执行，
    现 IndexOp::RunFn 的正式化）/ log；**不含** replay_rows——keydir delta
    成对推进是宿主 docmap 内部协议，P1 过渡期作为 adapter 构造参数私有传递）。
  - **契约文字化（写进头文件注释）**：prepare = 纯函数、任意线程、不得触碰
    插件可变状态（现 map_analyze 契约，cask.cpp:566-567 注释）；on_put/
    on_delete = reducer 单写者、ord 严格升序可有洞（现 reduce_apply 契约）；
    恢复重放**复用 on_put/on_delete**，插件按 watermark() 自门幂等（现 HNSW
    `max_inserted_ord_`/倒排 WAL 水位的隐式约定升格为接口义务，无 recover_*
    专用动词）；on_relocate 与 reducer 并发（现状 merger.cpp:131 直调语义），
    实现者自保线程安全；回调异常宿主吞并计数保活（S13-D7 语义）。
  - ord 分配**不进接口**：宿主（keydir `alloc_ord`）分配、插件只消费（现状即
    如此，固化为契约）。查询**不进接口**（插件私有能力，经类型化门面）。
    flush/open 本批只定义契约，保存点/恢复编排接线延后（P3 落地），
    adapter 先行委托实现。
  - CMake：`bitcask_plugin_api` INTERFACE 目标，仅依赖 `bitcask_format`。
  - 测试：头自包含编译单元（单独 TU include 即过编译）；CI 检查该头不引入
    任何 search/bm25/vector 依赖。
- [x] **S15-2【P0·M】thread_pool 去搜索化（variant 塌缩 + 类型擦除）** — 已完成（2026-07-03）
  - 现状（源码已核实）：`thread_pool.hpp:51` include search_layer.hpp（ReduceJob
    用于 ReduceEntry）；`ReorderEntry` variant 烧死六个搜索领域分支
    （ReduceEntry/OnWriteEntry/DeleteEntry/SkipEntry/RebuildEntry/RunFnEntry），
    cask.cpp:571-593 的 reduce lambda 逐一 `std::visit` 分发。
  - 目标：variant 塌缩四类通用条目——
    `PutEntry{owning 载体（现 IndexTask 字段即是）, small_vector<PreparedPtr>}`
    （吸收 ReduceEntry/OnWriteEntry：单文本与多字段路径统一为 prepared 扇出组）、
    `DeleteEntry{key, ord}`（广播全插件）、`SkipEntry`（ord 空洞填充，保留）、
    `RunFnEntry`（吸收 RebuildEntry——rebuild_hnsw 本就是塞进 reducer 静止点
    的闭包，无需专用分支；cask.cpp:2880-2887 的 `IndexOp::RebuildHnsw` 提交点
    改为封 RunFn）。`MapFn/ReduceFn/ErrorFn` 签名形状不变，entry 类型泛化；
    map 相只对 `wants_prepare()` 的插件调 `prepare`。
  - `thread_pool.hpp` 只 include `plugin_api.hpp`，删 search include；
    `IndexTask` 携带字段不变（它已是 PutEvent 的 owning 载体）。
  - **热路径护栏**：每（任务×声明 prepare 的插件）新增一次堆分配 + 虚调用。
    bench 基线先行（put_doc
    吞吐 + 索引落后水位），回退 >3% 才上 arena/freelist（设计 §10-2，先测后
    优化，不预优化）。
  - 测试：thread_pool_test 全绿；TSan 全量（reorder/保序核心零改动，变的只是
    payload 类型，但 unique_ptr 跨线程移交需 TSan 确认无新告警）。
  - **落地结果**：variant 四分支（PutEntry{task, preps}/Delete/Skip/RunFn）；
    `IndexOp::RebuildHnsw` 枚举删除，merge 提交点改 RunFn 闭包；所有 Add 统一
    走 map_fn（PipelineProcessesAllTaskTypes 契约同步改写：map_count 0→2）；
    `RebuildHnswCarriesOrd` 重写为 `RunFnCarriesOrdAndExecutesInReducer`；
    另附 make_doc_view/make_put_event 宿主视图 helper（分发闭包与契约测试共用）。
    **bench 护栏通过**：池级 SubmitDrain 3.59M/s vs HEAD 3.64M/s（−1.5%，噪声
    内）；端到端 put_doc（C API 临时压测，200k docs ×3 轮 ABBA 排除时序漂移）
    36.3k vs 36.4k docs/s（−0.3%）——未触发 arena 优化。
- [x] **S15-3【P0·M】SearchLayerAdapter + Cask 装配点改造（唯一插件，零行为变化）** — 已完成（2026-07-03）
  - `SearchLayerAdapter : CaskPlugin`（放 bitcask_search 目标内，内持
    `SearchLayer&`；DeltaReplayHook 经构造参数私有传入，不进通用接口）：
    `wants_prepare()=true`、prepare → `map_analyze`（单文本路径同样产
    Prepared，吸收原 OnWriteEntry 语义）；on_put → `reduce_apply` /
    `on_write`+`set_meta`+`on_vector`（`doc==nullptr` 时 `text:=value`，
    在 adapter 层保持「纯 put 也入全文索引」现行为）；on_delete /
    on_relocate 直委托；maintain → `compact`/`compact_index_chunks`/
    `rebuild_hnsw` 按 hint 分发；flush/open 暂委托现有
    `save_search_ckpt`/`load_search_ckpt`（编排仍在 Cask，P3 收）。
  - Cask 改造：新增 `std::vector<CaskPlugin*> plugins_`（P1 恒 = {adapter}）；
    `register_lib` 的 map/reduce lambda（cask.cpp:562-608）改为遍历 plugins_
    构造/分发扇出组，不再直呼 SearchLayer 方法。恢复（recover_doc_batch/
    recover_tomb）、merge（on_relocate）、门面查询本批**照旧直调 search_**——
    P1 只反转 IndexPool 这一条通路，控制爆炸半径。
  - **顺手收敛 on_delete 双路径**：cask.cpp:2108-2110 的非池同步直调分支——
    先核实不可达（search_ 强制 registry 非空 → index_pool 恒在，cask.hpp:370），
    坐实后删分支改断言；若存在可达路径则收敛进池路径再删。
  - 测试：全量回归 clang/TSan **零差异**（零行为变化是本批验收标准）；
    smoke / cask_docvalue（含 S14 全部 ckpt 系列）/ crash_recovery /
    checkpoint_recovery / merge_concurrent_writer 全绿；bench 对比 S15-2 基线。
  - **落地结果**：`search_plugin_adapter.hpp`（SearchPrepared 携带 ReduceJob；
    prepare 异常→空 job 降级镜像旧「空 ReduceEntry」语义；`doc==nullptr` 时
    `text:=value` 语义下沉 adapter）；cask.cpp 装配点改 plugins_ 注册序广播；
    on_delete 非池直调分支坐实不可达（open 强制 registry 非空）后删除；
    `map_analyze` fields 参数放宽为 span（调用点隐式转换，零行为变化）。
    clang 522/522；TSan 521/522——唯一失败 `ThreadCountIndependentOfLibCount`
    为 TSan 预存问题（git stash 后未改动 HEAD 同样失败，坐实与本批无关）。
- [x] **S15-4【P1·S】插件契约 Mock 测试（为 P2/P4 铺回归床）** — 已完成（2026-07-03）
  - `tests/plugin_contract_test.cpp` + `MockPlugin`（只链 bitcask_plugin_api +
    thread_pool 相关目标，**不链 search**）：断言 on_put 按 ord 严格递增到达；
    多插件按注册序固定分发（P2 的 DocMap 插队 reducer 首位、P4 的双插件扇出
    都靠此回归）；`wants_prepare()=false` 的插件收到 prep=nullptr 且 prepare
    不被调用；Skip 空洞不触发 on_put；Delete 广播全插件；RunFn 在静止点
    执行（与在途 on_put 不交错）；prepare 抛异常走 ErrorFn 计数且 lane 存活
    （cask.cpp:598-604 语义）。
  - 附带一条 TSan 场景：双 Mock 插件并发写入下扇出组移交无 race。
  - 测试：本条即测试；纳入 ctest 常规集。
  - **落地结果**：`plugin_contract_test.cpp` 4 测试（升序+注册序+配对移交+
    prepare 过滤+Skip/Delete 广播 / RunFn 精确顺位 / prepare 异常降级 ord 不
    stall / 双 lane 并发 TSan）——**配对契约**升级为强断言：prep 必须是本插件
    同 ord 的产物（MockPrepared 携带 ord+plugin_idx 验证）。目标只链
    plugin_api+TBB，不链 search。clang/TSan 双绿。

> **建议执行顺序（全批完成，2026-07-03）**：S15-1 ✅ → S15-2 ✅ → S15-3 ✅ →
> S15-4 ✅。**P1 验收判据全部达成**：thread_pool.hpp 不再 include
> search_layer.hpp；IndexPool 通路全部经 CaskPlugin 分发；clang 522/522、
> TSan 521/522（唯一失败为预存问题）；put_doc bench −0.3%（≤3% 护栏）。
> 后续批次预告：
> P2（DocMap 抽离为宿主服务）→ P3（checkpoint 拆分，高风险独立成批，设计 §5，
> 附退化方案 B）→ P4（SearchLayer 拆 Text/Vector 插件 + hybrid 上移）→
> P5（门面/C API/配置拆分收尾）。P1/P2/P4/P5 不依赖 P3。

## 待办：第十六梯队（S16 插件化架构 P2 — DocMap 宿主服务化，2026-07-03）

> 来源：`doc/plugin-arch-split-design-zh.md` §4/§9 P2。目标：`index::Index`
> （docmap：ord↔ext / live / meta）从 SearchLayer 私有成员上提为 **Cask 宿主
> 服务**，reducer 里先于所有插件 apply；SearchLayer 退化为借用消费者。这是
> BM25/HNSW 拆分（P4）的前置——双插件不能各自私有身份表。
>
> **实现期侦查发现（对设计 §4 的三点修正）**：
> - ① `index::Index` 已实现 `bm25::LiveChecker`（is_live/doc_len + SIMD
>   fill_is_live/fill_doc_lens，index.hpp:73,110-135）——「DocTable 只读窄
>   接口」有现成雏形，S16-3 在其上扩展而非新造。
> - ② **doc_len 迁移缓行**（偏离设计 §4「doc_len 迁 BM25 侧」）：doc_len 是
>   `DocSlot` 字段（index.hpp:49）**持久化在 kDocmap 段行内**，且以平坦 SoA
>   （`doc_lens_`，index.hpp:203）支撑 BM25 打分的 SIMD gather 热路径——迁移
>   同时牵连盘上格式与打分热路径。P2 保持「存储在 DocMap、语义归属 BM25」，
>   P4 拆插件时与 ckpt 格式变更（P3）合并评估。
> - ③ **delete 反转的顺序依赖**：`SearchLayer::on_delete` 先 `index_.get(key)`
>   查旧 ord 再调整 BM25 统计（ord_field_lens_）——若宿主先 remove docmap，
>   插件查不到旧 ord。修正：宿主在 docmap remove **前**捕获 `prior_ord`，
>   `DeleteEvent` 增带（无则哨兵值）。这对 P4 的独立 BM25 插件同样必要
>   （插件不该为拿旧 ord 反查 docmap）。

- [x] **S16-1【P0·S】Index 所有权上提（零行为变化）** — 已完成（2026-07-03）
  - SearchLayer 成员 `index::Index index_` 改为
    `std::shared_ptr<index::Index> index_holder_` + `index::Index& index_`
    引用别名（声明序 holder 先于 ref）——**两个 141K/88K 实现体零改动**，
    全部既有 `index_.` 用法照旧。
  - 构造函数增尾置参 `std::shared_ptr<index::Index> docmap = nullptr`：
    空 = 自持（standalone/测试路径零改动，59+ 测试构造点不动）；非空 =
    借用宿主实例。Cask 增成员 `std::shared_ptr<index::Index> docmap_`，
    create_search_infra / upgrade 两处先建 docmap_ 再注入 SearchLayer。
  - 测试：全量回归零差异（纯所有权反转）；新增一条断言
    `cask 侧 docmap_ 与 search_->index() 同一实例`（地址相等）。
  - **落地结果**：holder+引用别名如设计零改动实现体；`Cask::docmap()` 访问
    器补上（宿主服务句柄，S16-2 的写入口）；close 清句柄。新增
    `DocmapIsHostOwnedSharedInstance`（同址断言 + 双句柄可见性 + close 清
    句柄）。clang 523/523、TSan 522/523（唯一失败为既知预存项
    ThreadCountIndependentOfLibCount）。
- [x] **S16-2【P0·L】写路径反转：宿主先 apply DocMap，插件退纯索引写** — 已完成（2026-07-03）
  - reduce 闭包 PutEntry 分支：广播插件**之前**宿主直调
    `docmap_->put_doc`（DocSlot 从 task 构造）+ `set_meta`；DeleteEntry
    分支：先捕获 `prior_ord = docmap_->get(key)`，再 `docmap_->remove`，
    再广播（DeleteEvent 增 `prior_ord` 字段，哨兵 UINT64_MAX=原不存在）。
  - SearchLayer 侧：reduce_apply 去掉 ④put_doc/⑤set_meta（on_vector 留，
    属 HNSW）；on_write 同理；on_delete 改以 prior_ord 直达（不再
    index_.get 反查）。**顺序变化**（docmap 先亮 live 后加 postings，与现
    「postings 先、live 后」互换）的并发安全性论证写进注释：两序下查询
    都不可能命中「半个文档」（postings 无 → 不命中；live 无 → 过滤）。
  - **ckpt 记账迁移**：`dirty_docmap_` 脏位与 `delta_removals_` 删除日志
    从 SearchLayer 迁进 `index::Index`（写它的人负责记账）；SearchLayer
    的 save/delta 路径改读 Index 记账。恢复路径（recover_doc_batch /
    recover_tomb / DeltaReplayHook 链重放）的 docmap 写同步上提到 Cask。
  - 测试：全量回归（尤其 crash_recovery / checkpoint_recovery / S14 全系
    delta 链测试）+ TSan；plugin_contract_test 增「宿主 docmap 先于插件
    可见」断言（插件 on_put 内查 docmap 必已有本 ord 的 slot）。
  - **落地结果**：clang 524/524、TSan 523/523（唯一失败为既知预存项
    ThreadCountIndependentOfLibCount）；plugin_contract 新增
    DocmapVisibleBeforePluginOnPut（契约⑨，3 写 + 3 删全覆盖）；顺带修
    CheckpointDeltaChainLengthBound flaky（mtime→字节内容比较）。
- [x] **S16-3【P1·M】查询面 DocTable 化（P4 铺路）** — 已完成（2026-07-03）
  - 新设 `include/bitcask/doc_table.hpp`——`DocTable : public LiveChecker`，
    扩展 `ord_to_ext`/`eval_meta`/`ord_of`。`Index` 基类 LiveChecker→DocTable
    （已有方法补 override + ord_of 薄包装）。HNSW live-callback、
    materialize_hits、search_vector 过滤链改经 `const DocTable&` 形参——
    SearchLayer 查询代码不再直摸 `index_` 的具体类型。
  - 测试：查询全家（text/phrase/bool/fields/fuzzy/wildcard/vector/hybrid）
    回归零差异。**落地结果**：clang 524/524、TSan 523/523；查询全家 99/99。
- [x] **S16-4【P2·S】文档与契约测试收口** — 已完成（2026-07-03）
  - 设计文档 §4 更新（doc_len 缓行决定 + DeleteEvent.prior_ord 修正 +
    DocTable 最终形态 + 进度标记）；cpp-arch.md 分层图补 DocMap 宿主服务框。
  - plugin_contract_test 补 DeleteEvent.prior_ord 契约用例（契约⑩：
    覆盖写后 prior_ord = 最新 ord，非原始 ord）。

> **建议执行顺序**：S16-1 → S16-2（重头，含记账迁移）→ S16-3 → S16-4。
> S16-2 是 P2 的实质；若其 ckpt 记账迁移在评审中被判过重，可退化为
> 「记账经 SearchLayer 暴露的 docmap 写门面代持」（宿主仍是唯一写发起方，
> 记账物理位置暂留 SearchLayer，P3 一并迁）。

---

## 第十七梯队 S17：P3 — Checkpoint 拆分（manifest + 每组件独立文件族）

> 来源：`doc/plugin-arch-split-design-zh.md` §5。目标：单一 `search.ckpt`
> + 单 delta 链 → 3 个 per-component 文件族（docmap/bm25/vec 各自 base+delta
> 链 + .prev）+ `index.manifest`（唯一 commit 点）。解耦痛点：① 任一子系统
> rebase 强制全体 rebase；② BM/HNSW 无法独立装配。P3 是唯一高风险阶段——
> 动 S14 全部持久化布局。
>
> **Manifest 格式**（Oracle 确认，~80 字节）：
> ```
> magic "BCMF"(4) | version u32 | component_count u32
> per component [0=docmap, 1=bm25, 2=vec]:
>   base_watermark u64 | chain_seq u32 | chain_watermark u64
> footer_crc32 u32 | trailer "BCMF"(4)
> ```
> 无全局 watermark（recovery 取 `min(chain_watermark)`）。无 manifest .prev
> （80 字节 + CRC + 原子 rename 足够；损坏退全量 fold）。
>
> **Commit 协议**：
> - base 路径：组件文件各自 tmp+rename+fdatasync → manifest tmp+rename
>   （commit 点）→ keydir 快照
> - delta 路径：docmap.d\<seq\>（内联 kKeydirDelta，S14-7 原子性保留）
>   → manifest（无 keydir 快照）
> - 崩溃安全：组件文件 header watermark ≠ manifest → 用 per-component
>   `.prev`；manifest 是唯一 commit 点
>
> **不变量证明**：
> - delta 路径：所有组件 chain_wm 推进到同一 save-time watermark →
>   `keydir_covered = watermark = min(chain_wms)` ✓
> - base 路径：manifest 先于 keydir 快照 → crash 两者之间 keydir 旧 →
>   `keydir_covered < min(chain_wms)` ✓
>
> **⚠️ fsync 纪律**：当前 checkpoint 代码完全跳过 fsync。manifest 必须
> `fdatasync` + 目录 `fsync`，否则 rename 可能不持久化。
>
> **kKeydirDelta**：留在 docmap delta 文件内联（不进 manifest，不独立文件）。

- [x] **S17-1【P0·S】`index.manifest` 格式 + read/write** — 已完成（2026-07-03）
  - 新建 `include/bitcask/index_manifest.hpp`：`Manifest` 结构体（per-component
    entries: `{base_watermark, chain_seq, chain_watermark}`）+ `write()`
    （tmp+fdatasync+rename+dirfsync）+ `read()`（magic + CRC 校验）。
  - ComponentId 枚举：`kDocmap=0, kBm25=1, kVec=2`。
  - 测试：roundtrip + corruption injection + truncation + wrong magic + missing file（8/8 通过）。

- [x] **S17-2【P0·L】per-component save 路径拆分** — 已完成（2026-07-03）
  - `save_components_base` / `save_components_delta` 在 SearchLayer 实现，
    按 ComponentId 分配段，复用 SearchCheckpoint::write 不变。每组件独立
    rename→`.prev` on base。
  - 段分配：docmap→{kDocmap}，bm25→{kBm25Default, kBm25Fields}，
    vec→{kHnsw} + `.vec`/`.qc8` 侧车。delta 段同理。

- [x] **S17-3【P0·M】manifest-driven commit 协议接线** — 已完成（2026-07-03）
  - `save_checkpoint_paired` 实现 Oracle §2 pseudocode：组件文件先写 →
    manifest（commit 点）→ keydir 快照（仅 base 路径）。
  - close / checkpoint() / auto_checkpoint / merge 收尾路径全部改经新协议。

- [x] **S17-4【P0·L】recovery 协议重写** — 已完成（2026-07-03）
  - `load_recovery_snapshots` 重写：读 manifest → per-component
    `load_component`（header wm 匹配 manifest → 用；不匹配 → `.prev`；
    都不行 → 水位归零 fold 重建）→ per-component delta replay。
  - `fold_start = min(all component chain_watermarks)`。

- [x] **S17-5【P1·S】旧 search.ckpt → 新格式迁移** — 已完成（2026-07-03）
  - `migrate_legacy_search_ckpt`：首次 open 若无 manifest 但有 search.ckpt →
    读旧文件 → 按 section type 拆分为 per-component 文件 → 写 manifest。
  - 失败 → 全量 fold（安全兜底）。

- [x] **S17-7【P1·S】`.vec`/`.qc8` 侧车生命周期集成** — 已完成（2026-07-03）
  - vec 组件 base/delta 保存调 `save_vec_payload`/`save_qc_payload`（现有）。
  - manifest 不跟踪侧车（load 时 `load_vec_payload` 失败 → vec 段标 corrupt
    → fold 重建）。既有 CheckpointVecPayloadAppends 等测试覆盖。

- [x] **S17-6【P1·M】崩溃安全测试套** — 已完成（2026-07-03）
  - 既有 8+ 个 checkpoint/crash 测试已适配新文件名、全绿。
  - 新增 S17ManifestCorruptionFallsBackToFullFold：删 manifest + keydir →
    全量 fold 兜底 → 20/20 docs 全恢复。
  - 调查发现根因为测试代码 use-after-free（sv_bytes(temporary) 悬垂引用），
    非生产 bug——修正后测试通过。

> **建议执行顺序**：S17-1（格式基础）→ S17-2（拆 save）→ S17-3（commit
> 接线）→ S17-4（recovery）→ S17-5（迁移）→ S17-7（侧车）→ S17-6（崩溃测试）。
> S17-2 + S17-3 + S17-4 是实质重头（动 save/load 全路径）；S17-6 依赖前 5 项。

---

## 第十八梯队 S18：P4 — SearchLayer 拆 TextPlugin / VectorPlugin（2026-07-04）

> 来源：`doc/plugin-arch-split-design-zh.md` §6/§7/§9 P4。P1-P3 已备好全部落点
> （plugin_api 接口 / DocMap 宿主服务 + DocTable / per-component ckpt + manifest）。
> 目标：SearchLayer 一分为三——TextPlugin（BM25）/ VectorPlugin（HNSW）/
> HybridSearcher（RRF 门面）；merge/恢复/checkpoint 改插件广播；CMake 目标拆分。
> P5（后续批）做 Cask 门面迁出、C API 分文件、配置公开面换代、删 shim。
>
> **开工侦查修正（2026-07-04，对设计稿的两点偏离）**：
> - ① **归一化不可异步下沉**：cask.cpp `prepare_vector` 的归一化结果编码进
>   data file（V3.1「存储即归一化」），校验错误同步返回 put 调用方——设计 §6
>   「归一化作 VectorPlugin::prepare()（异步 map 相）」不可行。修正：函数搬
>   VectorPlugin（领域逻辑回家），Cask put 路径**同步**调用（Cask 是装配方，
>   认识具体插件对象；错误契约与磁盘语义不变）。
> - ② **wal_batch_size 是 dead config**：SearchLayer 从不调 InvertedIndex::
>   enable_wal（全仓调用点只在测试）。决定（用户拍板）：迁 TextPluginConfig
>   标 deprecated，真接线 or 删留 P5。
>
> **既定决策**：SearchLayer 保留为兼容 shim（85 处测试引用 + Cask 13 门面
> 零修改，P5 删）；doc_len 写通道 = 新 `bm25::DocLenWriter` 窄接口构造注入
> （不进 PluginHost、不给 Index&，保 S16-3 只读纪律）；`delta_removals_` 归
> Index 自记账（三条异构 remove 路径免同步改造）；恢复广播保 S3 并行
> （宿主攒批 + parallel_for prepare + fold 序串行 on_put + `PutEvent::replay`
> 标志）；merge 收尾 P4 保持无条件提交（dead_ratio 决策迁插件留 P5）；
> `bitcask_index`→`bitcask_docmap` 改名配 ALIAS（用户拍板）。
>
> **硬约束（每步验收）**：reducer 单写者/ord 序/LWW/S14 delta 链/S17 manifest
> commit 协议不破坏；**磁盘格式逐字节不变**（P4 只移交 save/load 发起权，
> 脚本化场景新旧 ckpt 对拍）；C API 签名不变；put_doc bench 回退 ≤3%
> （S18-5 检查点）；每子任务全量 ctest + TSan 全绿才进下一项。

- [x] **S18-1【P0·S】链水位一分为三 + DocLenWriter 窄接口** — 已完成（2026-07-04）
  - 窗口日志入账门从共享 `ckpt_chain_wm_` 改 per-component：`delta_vecs_`
    用 `comp_chain_wm_[kVec]`，`delta_removals_`（on_delete/recover_tomb）用
    `comp_chain_wm_[kDocmap]`；`save_components_delta` 日志清理按组件独立
    （写成功才清各自日志）；base 落成同样按组件清日志（链坍缩，静止点
    所有入账 ord < watermark）；legacy save/load 三处四水位联动置同值。
  - 新 `bm25::DocLenWriter` 窄接口（doc_table.hpp），`index::Index` 多继承
    实现（set_doc_len 补 override）；SearchLayer 增 `doc_len_writer_` 引用
    成员（现绑 index_，S18-4 起 TextPlugin 构造注入），apply_text/
    reduce_apply 改经窄接口回填。
  - **后注（2026-07-04 收官核对）**：本条的门限载体与回填成员均已随后续
    迁移易主——删除日志门限 → Index::delta_window_wm_（S18-2 自记账）、
    向量日志门限 → VectorPlugin::delta_window_wm_（S18-3）、comp_* 数组
    删除（S18-4）、SearchLayer 的 doc_len_writer_ 死成员移除（收官核对）。
    本条保留为 S18-1 时点的落地记录。
  - **顺手修复 S17 真实回归**：per-component 路径从不更新 legacy
    `ckpt_chain_wm_`（恒 0）→ 三个入账门失效——fold 重叠区旧墓碑全部误入
    delta_removals_，下一条 docmap delta 重放时 plain remove（无 ord 守卫，
    交错保护只覆盖同文件）**误杀上一代 base 里的复活文档**。新增回归测试
    `S18StaleTombstoneNotReloggedAfterBase`（已验证：门回退旧行为则转红）
    + `S18VecGateSkipsChainCoveredInserts`（重叠区向量不入日志 → 空链跳写）。
  - 验收：Debug 全量 **536/536**；TSan（search_layer/checkpoint_recovery/
    crash_recovery 67 例）零 race。

- [x] **S18-2【P0·M】docmap 持久化发起权上移宿主** — 已完成（2026-07-04）
  - `serialize_docmap`/`deserialize_docmap` 下沉 `index::Index`（SearchLayer
    留转发壳）；删除日志 + 脏位改 **Index 自记账**：`remove` 在 tomb_ord ≥
    delta 窗口水位时内部入账（found-check 前，保 keydir 半边重放语义）、
    `put_doc/set_doc_len/set_meta/compact_chunks` 自置脏；新 API
    `begin_delta_window`/`removals_snapshot`/`clear_removals`/`dirty`/
    `clear_dirty`。三条异构 remove 路径（流水线宿主 remove/legacy 2 参
    on_delete/recover_tomb）免同步改造。
  - 新 `include/bitcask/docmap_ckpt.hpp` + `src/keydir/docmap_ckpt.cpp`
    （bitcask_index 目标）：`save_docmap_base/save_docmap_delta/load_docmap`
    ——docmap 组件文件族（base+.prev+.d 链+内联 kKeydirDelta）宿主直驱，
    格式逐字节不变；`DocmapReplayHook`（index:: 类型）收归宿主。
  - Cask：`save_checkpoint_paired` base/delta 双路径 + `load_recovery_
    snapshots` + `migrate_legacy_search_ckpt` 改宿主直驱 docmap；新增
    `docmap_chain_` 链镜像成员（组件写成功即推进，独立于 manifest commit
    成败——防 manifest 写失败重试时丢已清空的删除日志）。SearchLayer 的
    `save_components_*`/`load_component` 收缩为 bm25+vec（ci=0 跳过）。
  - 验收：Debug 全量 **536/536**；TSan（SearchLayer/Checkpoint/Crash/
    CaskDocValue/PluginContract 154 例）零 race；S18 门限测试改写为宿主侧
    `docmap_ckpt` API 后仍绿（误杀护栏语义不变）。

- [x] **S18-3【P0·M】VectorPlugin 抽出（小而内聚先行）** — 已完成（2026-07-04）
  - 新 `include/bitcask/vector_plugin.hpp` + `src/search/vector_plugin.cpp`
    （暂挂 bitcask_search，目标拆分留 S18-10）：搬入 hnsw_/delta_vecs_/
    dirty_hnsw_/kVec 链槽/on_vector→insert/search_vector→search/
    rebuild_hnsw→rebuild/vec 组件 save-load（含 .vec/.qc8 侧车 + 链走读）/
    SIMD 归一化内核。注入 `VectorPluginConfig{dim,metric,m,ef,int8}` +
    `const DocTable&`；自持链状态与入账窗口（S18-1 门限语义随迁）。
  - `Cask::prepare_vector` 领域核心下沉 `VectorPlugin::normalize_for_write`
    （**同步调用**——V3.1「存储即归一化」+ put 同步错误契约不变，错误消息
    逐字保留经 Cask 翻译 CaskFault）。
  - 新 `include/bitcask/search_types.hpp`：SearchHit/SearchError 自
    search_layer.hpp 抽出共享（插件头不反向依赖）。
  - SearchLayer 变持有成员 + 全量委托（legacy 统一 ckpt 路径经
    graph()/serialize_delta_log/apply_delta_log/load_graph_section 原语）；
    set/get_component_state(kVec) 转发插件。
  - **实现期修正**：脏位语义必须镜像旧「save 时无条件清」——dim=0 base
    与空日志 delta 的跳写路径也要清 dirty，否则 vec 恒脏使 any_dirty 永真，
    静止后收尾 ckpt 永走 delta、bm25.ckpt base 永不落盘
    （CheckpointConcurrentWithWrites 抓出）。
  - 验收：Debug 全量 **536/536**（既有测试零修改）；TSan 166 例零 race。

- [x] **S18-4【P0·L】TextPlugin 抽出，SearchLayer 变纯 shim（本批最大搬迁）** — 已完成（2026-07-04）
  - 新 `text_plugin.hpp/cpp`（~1100 行，暂挂 bitcask_search）：搬入 fields_/
    analyzer_/ord_field_lens_/field_names_intern_/doc_texts_(DocTextLru)/
    synonym_map_/cache_/map_analyze/apply_job（reduce BM25 相）/apply_text/
    on_delete BM25 扣减半边/compact/rebuild_index（emit 回调形态，docmap
    遍历+磁盘读回留宿主）/maybe_auto_compact/bm25 组件 save-load + legacy
    容器原语（serialize/deserialize/apply_delta × default/fields +
    truncate_wal）/全部文本查询面。注入 `TextPluginConfig` +
    `const DocTable&` + `DocLenWriter&` + **`CompactionStats&`**（新窄接口，
    doc_table.hpp——S12-2 节流统计不给 Index&）。
  - reduce_apply 三域融合解体：shim 的 reduce_apply := text_.apply_job(job) +
    vec_.insert(ord,vec)。`SearchLayerConfig::text_config()/vector_config()`
    （= 计划的 split()）产两插件配置；wal_batch_size 迁 TextPluginConfig
    标 deprecated。kDefaultField/ReduceJob/SearchHitEx 迁 search_types.hpp。
    legacy 统一 ckpt 与 per-component 调度壳全部改经插件原语；comp_* 链
    状态数组删除（各归持有方）。
  - **计划偏差（核实后）**：「cache 失效修复」无需行为变化——S13-P1 的
    选择性失效已实质解决 §2.2-5 伪共享（向量-only 文档词集空 →
    invalidate_terms 空集 no-op）；S18-4 使之结构化（向量写不进 TextPlugin）。
  - 验收：Debug 全量 **536/536**（search_layer_test 等既有测试零修改）；
    TSan（含 text 域 Synonym/Highlighter/Wildcard/Fuzzy 共 190 例）零 race。

- [x] **S18-5【P0·S】写路径直连双插件、adapter 退役、PluginHost 落地** — 已完成（2026-07-04）
  - TextPlugin（name="bm25"）/VectorPlugin（name="hnsw"）直接实现
    CaskPlugin：adapter 路由语义逐条移入（doc==nullptr 时 text:=value、
    多字段走 prepare（TextPrepared 携 ReduceJob）、单文本 reducer 内分析、
    kNoPriorOrd 守卫；vec on_put 直插 doc->vec，on_delete no-op——软删经
    is_live）；open/close/watermark/flush 暂 stub（S18-6 实装）。
  - `plugins_ = {&text_plugin(), &vector_plugin()}`（注册序 = 原 reduce_apply
    内 add_doc→on_vector 顺序）；**删 search_plugin_adapter.hpp**；Cask 新增
    嵌套 `CaskPluginHost`（read_at→read_file+DataFile::read、
    run_serialized→RunFn 提交、log→log_warn/error），S18-6 经 OpenContext
    注入。
  - 验收：Debug 全量 **536/536**；TSan 158 例零 race；**put_doc bench 护栏
    通过**：C API 端到端 200k docs ABBA×3（vs S17 基线 cecafde worktree
    重建 so）——当前 ≈38.3k vs 基线 ≈38.5k docs/s（**−0.5%**，≤3%）；
    池级 SubmitDrain 3.82M/s（基线 3.59M/s）。

- [x] **S18-6【P0·M】checkpoint flush/open 接线** — 已完成（2026-07-04）
  - 两插件实装 flush/open/watermark：`FlushRequest` 增 `watermark` 字段
    （宿主提交时刻捕获的全局 ord 上界）；`OpenContext` 增 committed 三元组
    提示（base_wm/chain_wm/chain_seq，通用字段）。**base/delta 决策下沉**：
    force_rebase ‖ 插件自身 rebase 标志（compact/rebuild_index/rebuild 置位）
    ‖ 自身链长上限（max_delta_chain 每插件化）→ base；否则脏才 delta——
    旧「全体 base or 全体 delta」放宽为 per-component（rebase 解耦红利：
    rebuild_hnsw 只 rebase vec 链）。
  - `save_checkpoint_paired`：宿主 docmap（自决策 + keydir 快照与 docmap
    base 成对）→ 逐插件 flush（covered_ord==wm 才推进 manifest 项，链状态
    经 get_component_state 回读）→ manifest commit。`load_recovery_snapshots`：
    open_plugins 注入器（**全部路径**——含 manifest 缺失/迁移失败早退，
    零提示 = 插件降级自建；修「新库不 open → dir_ 空 → flush 写错目录」）；
    健康判据 = watermark() == manifest chain_watermark；全健康才清 legacy
    全局 rebase。base 轮落成清全局 rebase（修「新流程不再经
    save_components_base → 标志永不清 → 恒 base 不走 delta」）。
  - 验收：格式不变；Debug 全量 **536/536**（含 S17 注错/legacy 迁移/
    VecPayloadAppends 链行为）；TSan 154 例零 race。

- [x] **S18-7【P0·M】merge 广播接线** — 已完成（2026-07-04）
  - `run_merge` 签名 SearchLayer* → `std::span<plugin::CaskPlugin* const>`；
    wrapper 派发 begin → apply_pending 内逐条 on_relocate（value 传空——
    分批 apply 时缓冲已不在手，本就是可选便利）→ commit/abort。Cask 内部
    `DocmapRelocator`（不入注册表，置 span 首位，= 原 SearchLayer::
    on_relocate 语义）。`bitcask_merge` PUBLIC 依赖降 plugin_api。
  - merge 收尾硬编码解体：TextPlugin::on_merge_commit →
    run_serialized([compact(0.2)])、VectorPlugin → run_serialized([rebuild])
    ——FIFO 先于宿主成对保存点（顺序涌现 = 旧硬编码序）。**修正**：
    compact_index_chunks 属 docmap（宿主服务，S16 后），归宿主保存点 RunFn
    而非 TextPlugin（设计 §3.9-4 的映射按 P2 现实修订）；merge 恒 rebase
    改在宿主保存点 RunFn 显式 force（原经 compact shim 顺带置位）。
  - plugin_contract 的 merge 广播专项契约延至 S18-11 收口（顺序/abort/FIFO
    当前由 merge 全系集成测试隐式覆盖）。
  - 验收：Debug 全量 **536/536**；TSan（含 merge_concurrent_writer——merge
    线程广播 vs reducer 本批头号场景）163 例零 race。

- [x] **S18-8【P0·M】恢复广播接线** — 已完成（2026-07-04）
  - fold 重放改插件广播：活记录 → 宿主 put_doc（doc_len=0 占位，S16-2
    同构）+ 按注册序 on_put 广播；幂等靠既有水位自门（InvertedIndex::
    add_doc / HnswIndex::insert / docmap LWW——S15-1 接口义务的落地形态）。
    **保 S3 并行**：ReplayDoc 攒批（1024）→ 批内视图物化（FieldKV/DocView/
    PutEvent，容器预 size 保地址稳定）→ tbb::parallel_for 并行 prepare →
    fold 序串行 on_put。`PutEvent` 增 `replay` 字段：TextPlugin::prepare
    在 replay 且单文本时走 kDefaultField 包装分析（镜像原 recover_doc），
    活路径路由不动；on_put 改「有 prep 即 apply_job」三段式。
  - **计划偏差（保历史语义）**：墓碑重放**不广播 on_delete**——原
    recover_tomb 从不扣减倒排统计（统计基线随 ckpt 恢复，重放墓碑只翻
    live），改为宿主 `docmap_->remove`（Index 自记账门限日志）。恢复起点
    仍走 S17-4 字节水位 + 自门（min-wm 分段 fold = §10-1 followup 不变）。
  - recover_doc/recover_doc_batch/recover_tomb 在 Cask 路径下架（SearchLayer
    shim 保留供 standalone 测试）；**修 upgrade 路径**：手工装配不经
    create_search_infra → plugins_ 空 → 重放喂空表（UpgradeKVToIndex 抓出），
    补插件注册。
  - 验收：Debug 全量 **536/536**（crash_recovery 全系对拍）；TSan（并行
    prepare vs 串行 apply）163 例零 race。

- [x] **S18-9【P1·S】HybridSearcher 上移** — 已完成（2026-07-04）
  - 新 `hybrid_searcher.hpp/cpp`：`HybridSearcher{const TextPlugin&,
    const VectorPlugin&}`，RRF（K'=max(4k,64)/RRF(60)/ord 决胜）逐行平移；
    SearchLayer 增 `hybrid_` 成员并委托（Cask 门面经 shim 路由不变，P5
    迁出）。零行为变化：Debug 全量 536/536。

- [x] **S18-10【P1·M】CMake 目标拆分 + bitcask_docmap 改名** — 已完成（2026-07-04）
  - 新目标（设计 §8 终态）：`bitcask_text_plugin`（text_plugin+highlighter+
    search_cache → docmap/bm25/text/plugin_api）、`bitcask_vector_plugin`
    （→ vector/format/plugin_api，**不链 bitcask_text**——DocTable 头只读
    消费）、`bitcask_hybrid`（→ 两插件）；`bitcask_search` 退化 shim（仅
    search_layer.cpp，P5 删）；`bitcask_merge` 已去 search（S18-7）；
    `bitcask_cask` P4 期仍链 search（门面在，P5 摘）。
  - `bitcask_index` → `bitcask_docmap` 改名 + 旧名 ALIAS（tests/bench 的
    既有引用经别名零改动）；静态聚合脚本目标清单同步。
  - 验收：全量 **536/536** + TSan 149 例零 race；**依赖隔离实证**（nm -u）：
    libbitcask_vector_plugin.a 零 jieba/analyzer/nfkc 符号、
    libbitcask_text_plugin.a 零 Hnsw 符号——KV+BM25 / KV+HNSW 独立装配
    的链接基础就位（Cask 门面级 KV-only 摘除属 P5）。

- [x] **S18-11【P1·M】插件级测试套与契约收官** — 已完成（2026-07-04）
  - 新 `text_plugin_test`（3 例：FlushOpenRoundTrip / ReplayPrepareRouting /
    FlushDeltaThenForcedBase）+ `vector_plugin_test`（3 例：归一化错误契约
    逐字断言 / InsertFlushOpenRoundTrip 含侧车 / 重放幂等）——只链各自插件
    目标（依赖隔离在测试链接层再次验证）。
  - plugin_contract 补 **merge 广播契约**（S18-7 欠账）：契约⑪
    BeginRelocateCommitOrder（手工构造 data 文件 + KeyDir + run_merge 直调
    mock 插件——begin→relocate×3→commit 序 + 搬迁定位/输出 id 断言）、
    契约⑫ AbortOnFailure（输出目录注错 → begin→abort；缺失输入被宽容为
    无事可合，不构成失败——实现期侦查坐实）。
  - search_layer_test 不迁（shim 兜住，P5 处理）；CHANGELOG/TASK.md 收口。
  - 验收：Debug 全量 **544/544**（536 + 8 新）；TSan 145 例零 race。

> **S18 批次收官（2026-07-04）**：P4 全部 11 项落地。SearchLayer 一分为三
> （TextPlugin ~1170 行 / VectorPlugin ~600 行 / HybridSearcher + shim
> ~1130 行——shim 体量大半是 legacy 统一 search.ckpt 容器，随 P5 收编后
> 才真正缩壳），写/恢复/merge/
> checkpoint 四条通路全部经 CaskPlugin 广播；docmap 持久化归宿主；CMake
> 五目标终态（text_plugin/vector_plugin/hybrid/search-shim/docmap+ALIAS）。
> 顺手修复：S17 门限失效回归（stale 墓碑误杀复活文档，S18-1）。
> put_doc 端到端 bench −0.5%（≤3% 护栏）。后续：P5（Cask 门面迁出、
> C API 分文件、配置公开面换代、删 shim、legacy 统一 ckpt 收编）。

> **执行顺序**：S18-1 → S18-2 → S18-3 → S18-4 → S18-5 → S18-6 →
> {S18-7, S18-8}（S18-9 可与 6-8 并行）→ S18-10 → S18-11。
> 排序理由：1/2 先解「共享水位门」「delta_removals_ 错位」两耦合点（否则
> 3/4 把耦合搬进插件再拆一次）；VectorPlugin 先行验证组件自治形状；
> flush/open（6）先于 merge/恢复广播（7/8）——后者消费插件自治持久化状态。
> **TSan 重点**：① merge 线程回调 vs reducer；② run_serialized/保存点 FIFO；
> ③ 查询 vs reducer 写；④ 恢复期并行 prepare；⑤ close vs 在途查询。


---

## 第十九梯队 S19：P5 — 门面收编、删 shim、C API 分文件、文档换代（2026-07-04）

> 来源：`doc/plugin-arch-split-design-zh.md` §7/§9 P5（收尾批，低风险）。
> S18 后 SearchLayer 为纯委托 shim（hpp 581 + cpp 1137 行，~600 行是
> legacy 统一 search.ckpt 容器）；cask.cpp 对 shim 28 处 `search_->`。
>
> **批次决策（2026-07-04，规划时用户未响应按推荐默认）**：
> - **门面迁出取温和版**：新增 `text::Searcher`/`vec::Searcher` 门面 +
>   `drain_plugins()`；Cask 13 个 search_* 降为薄委托直调插件（不经 shim）
>   标 deprecated——源兼容保留（~200 处 facade 调用不动），彻底迁出留下个
>   major。
> - **配置面不换代**：`CaskOptions::search_config` 保留（`CaskOptions::
>   plugins` 需两阶段插件构造——构造只收 config、宿主服务经 OpenContext
>   注入——留待真有第三方插件需求）。
> - **wal_batch_size 删字段**（dead config 从未生效；InvertedWal 机制保留）。
> - `bitcask_index` ALIAS 保留（S18-10 承诺兼容一个版本期）。
>
> **核心策略：shim 整体降级为测试夹具**——search_layer.{hpp,cpp} 移入
> tests/support/（类名/方法不变），5 个测试文件 65 个构造点零 diff；legacy
> 统一 ckpt 的 save 半边随之变迁移测试夹具（生成 pre-S17 格式文件），load
> 半边收编为生产 `legacy_ckpt.cpp`（migrate 依赖，插件原语组合重写）。

- [x] **S19-1【P0·M】Searcher 门面 + drain_plugins + Cask 查询薄委托直连** — 已完成（2026-07-04）
  - 新 `include/bitcask/searcher.hpp`（header-only）：`text::Searcher`
    （文本查询全家 + 批量）、`vec::Searcher`（向量 + 批量）、
    `search::CaskHybridSearcher`（RRF），查询前 `cask.drain_plugins()` 读
    屏障，错误经 `Cask::search_error_fault` 共享翻译。
  - Cask 公开 `drain_plugins()` + `text_plugin()/vector_plugin()/
    hybrid_searcher()` 访问器（未启用搜索 = nullptr）；13 个 search_* 内核
    改直调插件/融合器（跳过 shim 查询委托层），`search()` 访问器标
    deprecated。**顺手**：`parallel_for_queries`/`search_arena` 抽出为
    `search_arena.{hpp,cpp}`（挂 bitcask_text_plugin——shim 降级后仍是
    生产件，S19-3 前置）。
  - 验收：Debug 全量 **545/545**（+1 门面等价测试 SearcherFacade.
    MatchesCaskFacade：text/vector/hybrid/批量四路逐 hit 对比）。

- [x] **S19-2【P0·L】Cask 去 shim：直持插件 + legacy load 收编** — 已完成（2026-07-04）
  - Cask 成员 `unique_ptr<SearchLayer>` → 直持 `unique_ptr<TextPlugin>` +
    `unique_ptr<VectorPlugin>` + `optional<HybridSearcher>`；
    `SearchLayerConfig` 移入新 `include/bitcask/search_config.hpp`
    （公开配置面不变）；cask.hpp 删 search_layer include。
  - 28 处 `search_->` 改直调（查询 13 → 插件/hybrid；ckpt 7 → dirty_mask
    组装/双插件 rebase + Cask 自持 legacy 全局 rebase 标志/chain_state；
    状态 3；setup 4；归一化 1）。
  - `migrate_legacy_search_ckpt` 改用新 `src/cask/legacy_ckpt.cpp`
    （load-only：容器段分发 → Index::deserialize_docmap /
    TextPlugin::deserialize_* / VectorPlugin::load_graph_section + .d 链
    重放喂 apply 原语 + DocmapReplayHook——自 shim 平移改造）。
  - 验收：全量 **545/545** + TSan 166 例零 race；S17-5 迁移测试全绿
    （legacy_ckpt 收编正确）；cask.cpp 零处 `search_->` 残留。**落地差异**：
    `search()` 访问器直接删除（非 deprecate——shim 类型将随 S19-3 离开产品
    库，返回类型无从保留）；cask_docvalue_test 11 处访问器用法与 gate_bench
    2 处改写为插件句柄（docmap 同址断言随所有权唯一化改为可用性断言，
    SearchTextAfterPut 的同步 on_write 直插改为插件查询内核直读）。

- [x] **S19-3【P0·M】shim 移入 tests/support + 测试/bench 迁移** — 已完成（2026-07-04）
  - `search_layer.hpp` → `tests/support/bitcask/`（**include 名不变** →
    65 个构造点与 5 个测试文件零 diff）、`search_layer.cpp` →
    `tests/support/`；新测试夹具目标 `bitcask_search_shim`（tests/
    CMakeLists，PUBLIC include support/ + 链 bitcask_hybrid），5 测试换链
    夹具、thread_pool_test 换链 bitcask_hybrid；产品 `bitcask_search` 目标
    删除（PCH/聚合脚本清单同步）。
  - c_api/gate_bench 的 include 改 search_config.hpp；a1/a4 bench 坐实为
    **孤儿源**（不在任何目标，早已不编译）——原样保留不迁。
  - 验收：全量 **545/545**（构造点零 diff）+ TSan 184 例零 race；
    `nm` 证实产品 so 零 SearchLayer 符号。

- [x] **S19-4【P1·S】wal_batch_size 删除** — 已完成（2026-07-04）
  - 从 SearchLayerConfig（search_config.hpp）与 TextPluginConfig 删字段及
    text_config() 传参（原位留删除说明注释）；**顺带删 BM_Put_WalBatch
    bench**——它测的就是 dead config，两档跑同一路径，基准前提不成立。
    inverted_wal 测试（直用 enable_wal）不受影响。全量 545/545。

- [x] **S19-5【P1·M】C API 分文件** — 已完成（2026-07-04）
  - 头拆 `bitcask_kv.h`（基础/配置/生命周期/KV/迭代/Meta 过滤/管理）/
    `bitcask_text.h`（BM25 文本 + text_filtered）/ `bitcask_vec.h`
    （向量/hybrid + filtered），`bitcask_c.h` 变聚合 include（符号/签名/
    ABI 不变，既有用户零改动）；实现拆 `bitcask_kv.cpp`（32 函数）/
    `bitcask_text.cpp`（9）/ `bitcask_vec.cpp`（6）+ 共享 `internal.h`
    （助手 inline 进 bitcask::capi + **visibility hidden**——否则原匿名 ns
    的内链接符号泄进动态表；具名 ns 保 c_api_registry 静态局部单实例）。
  - 验收：`nm -D` 符号面拆分前后**逐符号一致**；全量 545/545（含 CApi
    SmokeTest）。

- [x] **S19-6【P1·M】文档换代 + 收官** — 已完成（2026-07-04）
  - cpp-arch.md：分层图换代（plugins_ 分发表 + 三插件 + CaskPluginHost）+
    文首「S18/S19 架构换代」总述（正文 116 处 SearchLayer 历史行文按归属
    对号，不逐句重写）；README：框图换代 + Searcher 门面示例小节；
    api-c.md：§2.1 分域头文件表；设计稿 §9 迁移表 P3/P4/P5 全标 ✅（含
    落地偏离指引）；CHANGELOG 收官。

> **S19 批次收官（2026-07-04）**：P5 全部 6 项落地，**插件化架构五阶段
> （P1-P5）全线完成**。终态：产品库零 SearchLayer 符号（shim 降级测试
> 夹具）；查询三入口——Cask 门面（兼容薄委托）/ Searcher 类型化门面
> （推荐）/ 插件句柄直查；C API 三分域头 + 三实现 TU（ABI 逐符号一致）；
> pre-S17 迁移路径经 legacy_ckpt 收编保活。遗留（P6+）：Cask search_*
> 门面与 bitcask_index ALIAS 的正式退役（下个 major）、CaskOptions::plugins
> 换代（待第三方插件需求）、legacy 统一 ckpt 支持退役。

> **执行序**：S19-1 → S19-2 → S19-3 → S19-4 → S19-5 → S19-6（1-3 强依赖
> 链）。硬约束：C API ABI 不变、磁盘格式不变、pre-S17 迁移路径保活
> （S17-5 护栏）、热路径零回退。每步全量 + TSan 全绿才进下一步。

## 第二十梯队 S20：全库重构 — 冗余收敛、最佳实践、注释与文档一致性（2026-07-04）

> 来源：用户「对项目进行重构」六准则复审（最佳 C++ 实践 / 高内聚低耦合 /
> 公共代码降冗余 / 完善且正确的中文注释 / 文档与代码一致）。三路并行审计
> （冗余 R1-R9、最佳实践 B-*、注释与文档 C-*）汇总成八子任务。
>
> **审计正面结论**：S15-S19 新面无正确性隐患（RAII/extern "C" 边界/flush
> 失败原子性/const 正确性全绿）；问题集中在结构性冗余（组件 ckpt 骨架 ×N、
> delta 解析 ×2、结果结构三胞胎）与旧注释/文档未随迁移换代。
>
> **先例约束（S9 裁定续用）**：不做依赖方向不变的类内美学拆分；不动
> TSan-clean 并发核心语义；冗余收敛类改动 = 行为零变化（ckpt 字节对拍）；
> 每步全量 545 + TSan 全绿 + C ABI `nm` 逐符号对比。

- [x] **S20-1【冗余·M】低风险公共代码收敛 R1/R4/R6/R7** — 已完成（2026-07-04）
  - **R1** C API 单结果尾块 ×12 → `internal.h::finish_single()`（expected→C
    物化 + 错误/OOM 翻译）；filtered 变体的过滤树三态转换 ×3 →
    `parse_meta_filter()` + `ParsedFilter`（无过滤/成功/非法，storage 随对象
    存活不悬垂）。bitcask_text.cpp/bitcask_vec.cpp 各入口收敛为「校验→委托→
    finish_single」三行。
  - **R4** ckpt 分段写「owned payload 缓冲 + 并行 CkptSection」骨架 ×5 →
    `search_checkpoint.hpp::SectionWriter`（依赖 vector 移动语义保 span 不
    悬垂）。落地：text 组件 base/delta、vector delta、docmap delta、测试 shim
    delta。shim base-save（byte/uint8/carried 三缓冲混合）非此形态，保留。
  - **R6** ChainState/DeltaSaveResult/LoadResult 三胞胎（text/vector/docmap）
    → `component_ckpt.hpp::ckpt::{ChainState,DeltaSaveResult,LoadResult}`
    单一真源；各类以嵌套 `using` 别名暴露（`TextPlugin::ChainState` 等名字
    不变），差异化 setter（VectorPlugin 联动 delta_window_wm_）留各类。
    DocmapLoadResult → `using = ckpt::LoadResult`。legacy_ckpt::LoadResult
    字段序不同且属 P6 待删，不动。
  - **R7** byte helper 收敛：删 text_plugin.cpp 的 put_u*_b 转发层 → 直用
    `sc::detail::put_u*`；测试 shim 的 put/get_u*_byte 重实现删除 → 直用
    `detail::*`（文件本在 namespace bitcask::search 内）。规范收敛为
    sc::detail + byte_order 两套。
  - 验收：全量 **545/545**；TSan 相关套件 **124/124**；`nm -D` C ABI 与基线
    **逐符号一致**。

- [x] **S20-2【冗余·L】ckpt 链走读公共化 R2/R3/R8低** — 已完成（2026-07-04）
  - **R2** `.d` 链走读 ×5（text/vector/docmap load_component + legacy + 测试
    shim load_search_ckpt）→ `ckpt_chain.hpp::sc::walk_chain(base_path,
    base_gen, base_coverage, chain_seq, apply)` 模板。`chain_seq>0` 有界
    （缺文件=链断）、`=0` 无界（缺文件=正常链尾）；kDeltaInfo 三元组校验
    （gen/prev_wm/seq）内建，仅「如何应用一个 delta 文件」由 apply 回调定制。
    返回 {coverage, next_seq, ok}。仅 .cpp 包含（不进公共头，隔离 <filesystem>）。
  - **R3** kDocmapDelta 段解析 + 交错重放 ×2 → 公开 `index::
    apply_docmap_delta_section(docmap, payload, rows, rems)`（自 docmap_ckpt
    匿名 ns 提升），legacy_ckpt 的内联解析改调之（逐字节同构，已核对）。
  - **R8 低风险半** `remove_chain_files` 8-miss 链坍缩 ×4（docmap helper +
    text/vector 内联 + shim 内联）→ `ckpt_chain.hpp::sc::remove_chain_files`。
  - 说明：shim 的 docmapDelta 解析用 shim 本地行类型（keydir hook 素材），
    不在 R3（两处）范围；R8 的 save 骨架回调化（中风险）归 S20-5 评估。
  - 验收：全量 **545/545**；TSan checkpoint/recovery/legacy/migrate/plugin
    套件 **128/128**；C ABI `nm -D` 与基线**逐符号一致**。恢复链重放语义
    零变化（既有 S17/S18 崩溃恢复与链测试全绿）。

- [x] **S20-3【接口质量·S】B-B2 / B-B1 / R5** — 已完成（2026-07-04）
  - **B-B2** `plugin::FlushResult` 加宽 `chain_seq`/`chain_wm`——manifest entry
    三元组 {generation, chain_seq, chain_wm} 由 flush 直接回传。Text/Vector
    flush 统一尾部从 chain_（save 后态）回填；`save_checkpoint_paired` 的
    flush 轮询循环删去「按 `*comp` 分支下探 text_/vec_plugin_ 读 chain_state()」
    ——保持多态，第三组件零 else。vec dim==0/空日志/失败分支行为逐一保真
    （covered_ord≠wm ⇒ entry 不更新，chain 回执被忽略）。
  - **B-B1** `index_manifest.hpp::write_manifest/read_manifest` 裸 fopen/fclose
    → `manifest_io::FilePtr`（unique_ptr<FILE,FileCloser>，独立命名空间避与
    field_schema.hpp 的 bitcask::detail::FileCloser 撞名）。fdatasync 在
    显式 reset 前、rename 后 dirfsync 序不变。
  - **R5** searcher.hpp 三门面单查/批量骨架 → `search::detail::run_query` /
    `run_batch` 模板（读屏障 + 内核调用 + 错误翻译 + TextSearchResult 包装 /
    并发保序批量）。text::Searcher 删私有 run()；vec::Searcher / hybrid 各
    收敛。search_text_highlight 返回 SearchHitEx 向量（非 TextSearchResult），
    形态不同，保留独立物化。
  - 验收：全量 **545/545**；TSan 相关 **124/124**；C ABI `nm -D` 逐符号一致。

- [x] **S20-4【内聚耦合·S】config 头拆分 B-C1** — 已完成（2026-07-04）
  - 抽 `bm25_params.hpp`（Bm25Params，自 inverted.hpp）、`text_plugin_config.hpp`
    （TextPluginConfig，仅依赖 analyzer/bm25_params/synonym_map）、
    `vector_plugin_config.hpp`（VectorPluginConfig，仅依赖 meta_file）。
    text_plugin.hpp/vector_plugin.hpp/inverted.hpp 改包含轻量头（既有使用点
    零改动）；`search_config.hpp` 依赖从两插件完整定义换为两 config POD 头。
  - 效果：配置聚合层与插件实现层**解耦**——`search_config.hpp` 独立包含时
    的预处理树**不再含 inverted.hpp/hnsw.hpp**（`-H` 追踪证实）。
  - 实事求是：`c_api/internal.h` 经 `cask.hpp`（持 unique_ptr<插件> 必含完整
    定义）仍转译 inverted/hnsw，故 C-API 编译时间**未减**；本项价值在层次
    解耦（为将来 CaskOptions::plugins 换代铺路），非审计初估的构建时收益。
  - 验收：全量 **545/545**；TSan 编译通过 + plugin/ckpt 冒烟 41/41；
    C ABI `nm -D` 逐符号一致。

- [x] **S20-5【中风险评估·裁定】R8 save 骨架 / R9 flush 决策骨架 — 弃做** — 已完成（2026-07-04）
  - 用户指令「纳入但保守评估：保真优先，收益不抵风险即弃」。逐项评估结论：
    **两项均弃**，仅文档记裁定（无代码改动）。
  - **R9 flush 决策骨架**：S20-3 后 Text/Vector flush 结构已相近，但三处差异
    正落在 S18-3 vec dirty 回归的断层上——① vec `dim==0` 分支（save_base
    返 false 却按「成功但不推进」处理，covered_ord 钉链水位）② no-op 条件
    （text=`!dirty()`；vec=`!dirty()||delta_vecs_.empty()`）③ no-op 时 vec 无
    条件清 dirty_（镜像旧「save 时无条件清」），text 为纯 no-op。共用骨架
    须把这三处编码成回调/标志，抽象比现有线性可读的显式代码更漏；~25 行
    收益不抵重触该区的回归风险。
  - **R8 save_base/delta 骨架回调化**：save_component_base 的公共壳仅
    「rename→.prev + write + remove_chain_files + 记账」，而 vec 有 dim==0
    前置分支 + `.vec/.qc8` 侧车（uint8 载荷，非 SectionWriter 的 byte 形态，
    S20-1 R4 已因此排除 vec base）；save_component_delta 的载荷构造 text/vec/
    docmap >80% 不同（bm25 default+fields / hnsw 日志 / kDocmapDelta+keydir）。
    公共骨架小、发散回调大，且记账语义三者各异——回调化得不偿失。
  - 低风险部分（remove_chain_files ×4、SectionWriter ×5）已在 S20-1/S20-2
    收割。裁定与 S9「不做依赖方向不变的美学拆分」及 S18-3 回归教训一致。

- [x] **S20-6【注释·M】现状式误导注释换代（~17 处）** — 已完成（2026-07-04）
  - 判据：「自/原 SearchLayer 平移/一致」= 历史归属**保留**；「由 X 调用 /
    现状式描述 / 引已删符号」= 误导**改**。
  - cask.cpp：open 阶段 banner（创建 SearchLayer → Text/Vector 插件）×4
    （upgrade/meta/阶段三/config 透传）、save_checkpoint 读水位来源、
    search_vector（→ VectorPlugin + DocTable）、search_hybrid（→ HybridSearcher）、
    base/delta 决策注释旧成员名（comp_chain_wm_ → chain_watermark/docmap_chain_/
    chain_state()）。
  - cask.hpp：docmap 借用方（search()->index()/SearchLayer → 插件借用）×2、
    open 阶段三 banner、成员注释「SearchLayer 实例」→「搜索插件」。
  - thread_pool.hpp：search_layer.cpp → search_arena.cpp、on_vector →
    VectorPlugin::on_put。query.hpp ×2 / inverted.hpp / hnsw.hpp：SearchLayer
    路由/rebuild → TextPlugin/VectorPlugin。index.hpp：dirty_docmap_ 类比标
    「原」。plugin_api.hpp：`name()` 加注——持久化/manifest 身份 vs API 命名
    空间双词汇（C-B5）。text_plugin.hpp:52 的「见 search_layer.hpp」已在
    S20-4 config 迁出时消解。
  - 保留（历史归属）：search_types.hpp/search_config.hpp/search_arena.cpp 的
    「自 search_layer 抽出/平移」provenance 注释；index.hpp「自 SearchLayer
    平移」段。验收：编译通过 + plugin/smoke 全绿（注释改动不涉 ABI/行为）。

- [x] **S20-7【文档·L】文档一致性换代** — 已完成（2026-07-04）
  - **api-cpp.md**：include 换代（search_layer.hpp → searcher/search_config/
    search_types.hpp）；§5.5 重写（删已删 `search()`，补 drain_plugins /
    text_plugin / vector_plugin / hybrid_searcher）；§7 开篇插件化重写；§7.1
    删 wal_batch_size 行、补 max_delta_chain；**新增 §7.5 Searcher 门面节**；
    示例 include 换代。
  - **concurrency-zh.md + thread-safety.md**：§6 重写（SearchLayer → Text/
    Vector 双插件；「单 worker」失实 → **N map worker + 1 reducer**，单写者 =
    reducer，据 thread_pool.hpp 核对）；锁层级表 + 全序图 + 跨模块规则换代
    （SearchLayer::fields_mu_ → TextPlugin::fields_mu_@text_plugin.hpp:360、
    hnsw_ → VectorPlugin@vector_plugin.hpp:173，失效 file:line 修正）；
    thread-safety.md synonym_map_ 归属 TextPlugin。
  - **cpp-arch.md**：模块清单（search_layer.hpp/cpp 已删 → plugin_api/
    text_plugin/vector_plugin/hybrid_searcher/searcher/search_config +
    text_plugin.cpp/vector_plugin.cpp/hybrid_searcher.cpp/search_arena.cpp）；
    c_api 分域（bitcask_kv/text/vec + internal.h）；put_doc 流程 on_write →
    TextPlugin::on_put；索引层叙述换代（N map + 1 reducer）；测试二进制数
    22 → **32**（核对 tests/CMakeLists.txt）。
  - **api-c.md**：正文补 6 个在册函数——put_ex / put_batch / status_ex /
    search_{text,vector,hybrid}_filtered（签名核对 c_api 头）。
  - **format-zh.md**：文件树 + §10 intro 补 per-component（index.manifest +
    docmap/bm25/vec 文件族）；§10.2 标 BCSC 容器为 legacy/per-component 共用
    外壳、失效路径 search_layer.cpp → legacy_ckpt.cpp；**新增 §10.4
    index.manifest（BCMF 80B 格式）+ per-component ckpt + delta 段型
    （kDeltaInfo/kBm25*Delta/kDocmapDelta/kHnswDelta/kKeydirDelta）**。
  - **README.md**：测试二进制数 26 → 32。
  - **recovery-unified-…-design-zh.md**：顶部换代注记——单文件模型已被 S17
    per-component 取代（指向 format-zh §10.4 / index_manifest.hpp）。
  - **已知残留（S20-7 范围外，据审计 Section C）**：深层设计/分析文档
    （hnsw-design-zh / hnsw-lifecycle-zh / merge-policy-zh / ord-recycling-
    design-zh / posting-zero-copy-design-zh / put-flow-zh / async-index-
    pipeline / s13-review）仍含 `search_layer.cpp:NNN` 历史 file:line 指针。
    这些为特定时点的设计决策档案（性质同 recovery-unified 历史行文），审计
    未纳入换代清单；留待后续文档考古式清理批次。
  - 验收：7 个换代目标文档 stale 模式扫描**全 0**；文档-only，不涉编译。

- [x] **S20-8【收官】全量验证 + 批次收口** — 已完成（2026-07-04）
  - **全量 ctest（Debug）**：**545/545** 全绿。
  - **TSan**：**544/545**——唯一失败 `IndexPoolMultiLib.ThreadCountIndependentOfLibCount`
    经确认为**先例 TSan 不兼容**（非 S20 回归）：该测数 `/proc/self/task` 线程，
    gtest_discover 下每 case 独立进程 ⇒ 本 case 是其进程内首个 `pthread_create`，
    TSan 运行时在此惰性起 background 线程被计入 delta（3 vs 期望 2）。S15-S19
    的「TSan 全绿」跑的是 ~184 例相关子集，从未含本例；S20 对 thread_pool 仅
    2 行注释改动（IndexPool 线程创建逻辑逐字节不变）。排除本例 **544/544 全绿**。
  - **C ABI**：`nm -D` 与 S20 基线（scratchpad/abi-baseline-s20.txt，1281 符号）
    **逐符号一致**——磁盘格式与 C 接口零变化贯穿全批。
  - **审计「明确不动」项记录在案**：search_arena 有意泄漏进程单例、TextPlugin
    三窄接口引用（ISP 正确形态）、searcher.hpp header-only 门面、B3 字符串
    name→ComponentId 分发（仅两插件缓行）、`text_`/`vec_plugin_` 命名不对称、
    cask.cpp 3442 行 TU 拆分（依赖方向不变的 watch item）——均按 S9 restraint
    保留。

> **S20 批次收官（2026-07-04）**：全库重构八子任务落地。**冗余收敛**
> （R1 finish_single/R3 apply_docmap_delta_section/R4 SectionWriter/R6
> component_ckpt/R7 byte helper/R2 walk_chain/R8 remove_chain_files）删净
> ~5 处 ckpt 骨架 + 链走读 ×5 + delta 解析 ×2 拷贝；**接口质量**（FlushResult
> 多态回执消下探、manifest RAII、Searcher run_query/run_batch）；**内聚耦合**
> （config 头拆分——配置层与插件实现层解耦）；**注释/文档换代**（~17 处现状
> 式误导注释 + 7 个 reference/design 文档）。中风险 R8-save/R9-flush 骨架经
> 保守评估弃做（S18-3 脆弱区）。全程行为零变化：Debug 545/545、TSan 544/544
> （排除 1 先例不兼容）、C ABI 逐符号一致、磁盘格式逐字节不变。遗留：深层
> 设计文档的历史 file:line 指针（S20-7 已记）、R8/R9 骨架（P6+ 若重构 flush
> 决策再议）。

- [x] **S20-7 补：深层设计文档历史 search_layer 引用清理** — 已完成（2026-07-04）
  - S20-7 记为残留的深层设计/分析文档 file:line 指针，逐处处理：
    - **换代到现位（含现行行号）**：hnsw-lifecycle-zh（rebuild_hnsw →
      VectorPlugin::rebuild@vector_plugin.cpp:240、on_vector → VectorPlugin、
      save/load_search_ckpt → legacy_ckpt/per-component、CkptLoadResult →
      ckpt::LoadResult）、merge-policy-zh（on_delete → text_plugin.cpp:243、
      live callback → vector_plugin.cpp:216、rebuild → :240）、hnsw-design-zh
      （图内过滤 → vector_plugin.cpp:216、RRF → HybridSearcher）、put-flow-zh
      （on_write → reducer 扇出 TextPlugin::apply_job + VectorPlugin::insert）、
      ord-recycling-design-zh（RRF ord 去重 → hybrid_searcher.cpp:45、
      rebuild_index → text_plugin.cpp:295）。
    - **顶部换代注记**（历史评审/设计稿，正文行号保留为当时快照）：
      async-index-pipeline.md（S18-4：搜索域迁 text_plugin/vector_plugin）、
      s13-review-2026-07-02.md（S18-3：hnsw_ → vector_plugin.hpp:173）。
    - **保留（设计本意）**：plugin-arch-split-design-zh.md 的 search_layer.cpp
      引用——该稿正是「拆分 search_layer 为插件」的设计本体，其 before-state
      引用是叙事主体，不可改写；recovery-unified 已有 S17 顶部注记覆盖。
  - 旁证：plugin-arch-split-design-zh.md 状态栏独立记载「TSan 523/523，唯一
    失败为**既知预存项 ThreadCountIndependent**」——印证 S20-8 对该 TSan 失败
    「先例、非本批回归」的判定。
  - 文档-only，不涉编译/ABI。

- [x] **S20-2 补：walk_chain `chain_seq==0` 语义修正（对抗性审查发现的真 bug）** — 已完成（2026-07-04）
  - **背景**：S20 收官后对「行为零变化」做对抗性等价审查（我 + 2 个独立
    审查代理逐处比对 pre-S20 基线 5fca39a），在最高风险的 R2 walk_chain 发现
    一处**真实等价性破坏**。其余全部重构（walk_chain 另 5 核对点、FlushResult
    多态回执含 dim==0/空日志/失败边界、R1/R4/R6/R7/B-B1/B-C1）确认逐字节等价。
  - **bug**：`walk_chain` 用 `bounded = chain_seq != 0` 从计数推断有界性——
    `chain_seq==0` 被当作**无界扫盘**。但三个有界调用点（text/vector/docmap
    load_component）改前是 `for s=1; s<=chain_seq`，`chain_seq==0` 时**零迭代**；
    而 base-only 组件（0 已提交 delta）与新库的 committed chain_seq 恒为 0。
  - **危害**：delta `.d<seq>` 先写盘、manifest 最后提交（唯一 commit 点）。
    崩溃于「写完 .d1、提交 manifest 之前」的窗口，或 write_manifest 失败
    （ENOSPC）→ 盘上残留 manifest 未记录的 orphan .d1。恢复时改前零迭代
    正确忽略，改后无界扫盘 → orphan .d1 kDeltaInfo 恰过校验 → **重放未提交
    delta**，覆盖水位越过 committed：插件端 → 健康检查失败触发无谓全量
    rebase；docmap 端 → orphan 行/删除经 keydir hook 灌入活 keydir（幽灵条目）。
  - **545 未捕获原因**：正常 base-only 盘上无 .d 文件（remove_chain_files
    已清），走不到分歧；bug 仅在 crash 窗口的特定盘上残留态现形——无既有
    测试构造该态。
  - **修复**：walk_chain 加显式 `bool unbounded` 参数（不再从 chain_seq==0
    推断）。text/vector/docmap 传 `false`（有界，chain_seq==0 → 零迭代），
    legacy/shim 传 `true`（无界）。
  - **回归测试**：`TextPlugin.OrphanDeltaNotReplayedWhenChainSeqZero`——构造
    base(wm=1, committed chain_seq=0) + orphan .d1(wm=2)，断言载入覆盖水位停
    在 1、orphan 文档不进索引。**临时还原 buggy 逻辑验证：bug 版失败、修复
    版通过**。
  - 验收：Debug **546/546**（545 + 新回归）、TSan checkpoint/recovery/plugin
    **122/122**；不改磁盘格式与 C ABI。

---

## S21 批次：结构 / 存储 / 内存三轴优化（2026-07-06）

> 来源：用户指令「继续优化：①结构不合理处 ②存储结构 ③内存使用效率（不引入
> 额外内存分配器）」。两个独立审计代理全库扫描（内存效率 12 靶点、存储/结构
> A×7+B×7 靶点），已对照历史批次去重；按 ROI 择优落地如下。

- [x] **S21-1a Index 常驻内存收缩（DocSlot 40→24B + meta_blobs_ 惰性化）** — 已完成
  - `DocLoc` 重排 `{offset,file_id,total_sz}` 消 8B padding（24→16B）；`DocSlot`
    去掉 ord 死重字段（原注释自证「仅 get() 返回时填充」），`get()` 改返回
    `DocHit : DocSlot`（聚合继承，`slot->ord/loc/tstamp` 消费点全部无感）。
    每 ord 常驻 40→24B（slots 数组 −40%，每 chunk 2.5MB→1.5MB）。
  - 全部 `DocLoc{}` 构造点改 **designated initializer**——位置式聚合初始化在
    字段重排下会静默错位（`DocLoc{1,100,50}` 三个字面量可无警告编译成错值）。
  - `meta_blobs_` 惰性化：首个非空 `set_meta` 前不随 ord 增长——无 meta 部署
    零常驻（改前每 ord 白付 24B 空 vector 头，千万 ord = 240MB）。读路径经
    `ord >= size()` 门禁天然空 blob 语义，启用后与 live_ 同步跟平。
  - `static_assert(sizeof)` 锁定布局；serialize/deserialize_docmap 逐字段
    读写，磁盘格式不变。
- [x] **S21-1b VectorPlugin delta 插入日志平行数组化** — 已完成
  - `delta_vecs_`（`vector<pair<u64,vector<float>>>`）拆 `delta_ords_` +
    `delta_data_`（dim 步长紧凑拼接）。每向量写入 −1 次堆分配，窗口常驻
    −(24B 头 + malloc 圆整)/条。kHnswDelta 段序列化字节不变。
- [x] **S21-1c 缓存失效路径 term 零拷贝** — 已完成
  - `invalidate_terms` 收 `span<const string_view>`（借用 caller 的 term 存储），
    `term_index_` 换 StringHash 透明查找。三个调用点（apply_text/apply_job/
    on_delete）原每写/删一篇文档把全部词深拷成 owning string 只为一次交集
    判定。on_delete 的 `tf` 提出块外保证 view 生命周期覆盖 invalidate 调用。
- [x] **S21-1d 注释漂移订正**：data record header「14 字节」→ kHeaderSize=23（3 处）。
- [x] **S21-2 A4 checkpoint 落盘补 fdatasync** — 已完成
  - `SearchCheckpoint::write` 与 `KeyDir::save_snapshot` rename 前 fdatasync
    （对齐 write_manifest）。manifest 是唯一 commit 点且自带目录 fsync，但组件
    数据页不落盘的话，断电后 manifest 已提交而组件 CRC 坏 → 整组件退全量
    fold——checkpoint 的启动加速在断电场景整体失效。flush 低频，成本可忽略。
- [x] **S21-2 A3 keydir 快照 v2（entries 块 vbyte）** — 已完成
  - kSnapVersion 1→2：klen/file_id/total_sz/offset/epoch/ord 换 vbyte，tstamp
    保持定宽 4B（unix 秒级时间戳 vbyte 需 5B 反而更大）。~38B/条 → 典型
    15-20B/条，GB 级快照读写 I/O 近半。读端 v1/v2 双分支；快照可重建，零迁移。
  - 回归：`KeyDirSnapshot.V2RoundTrip` + `V1CompatRead`（写端已恒写 v2，v1 读
    分支无自然覆盖 → 手工构造 v1 字节流防回归）。
- [x] **S21-2 A2 docmap 行编码 v2（gap+vbyte）** — 已完成
  - base：BCIS sidecar 版本 1→2，行 `[gap vbyte ord][vbyte klen][ext][vbyte
    file_id/offset/total_sz][u32 tstamp][vbyte doc_len]`，34B 定宽 → 典型
    12-15B/行。delta：新段型 `kDocmapDeltaV2`(=13) 同款编码，30B → ~10-12B/行。
    gap 用 u64 二补数回绕（`prev + (v−prev) ≡ v`），正确性不依赖升序。
  - **降级安全论证**：旧二进制对未知**段型**是「静默忽略」而非链断——若只加
    段型不升文件版本，降级后 v2 delta 的行被丢弃且水位照常推进（数据洞）。
    故 v2 delta 文件头写 **file version 2**（`SearchCheckpoint::write` 加
    version 参数；读端 1/2 双收），旧读端整文件拒收 → 链断退 fold（安全慢）。
    base 侧 BCIS version 不符本就拒收退 fold，无需升文件版本。
  - 回归：`Index.SidecarV2RoundTrip` + `SidecarV1CompatRead`（手工 v1 字节流）；
    `cask_docvalue_test` 段型断言 kDocD 10→13。
- [x] **S21-3 B3 delta 段集应用骨架去重** — 已完成
  - docmap_ckpt / legacy_ckpt 两处 `apply_delta_file` 的「全段 CRC 预检 +
    rows/rems/meta 收集 + hook 收尾一次性透传」逐字节同构（S20-2 收敛解析后
    残留），抽 `index::apply_delta_sections` 模板骨架，两侧只留段分发 switch。
    不变量（坏段整 delta 拒绝、hook 恰好一次）收敛单点。行为零变化。
  - 顺带：search_checkpoint.hpp 补缺失的 `<memory>`（曾靠传递包含掩盖）。
- [x] **S21-3 B1 cask.cpp 物理拆分** — 已完成
  - 3441 行按功能域**纯平移**拆四份：cask.cpp 2300（open/close/写路径/get/
    checkpoint/merge/backup 留守）+ cask_iter.cpp 240（CaskIter 全族）+
    cask_search.cpp 250（search 门面）+ cask_recovery.cpp 656（upgrade/
    replay_delta_to_keydir/migrate_legacy_search_ckpt/load_keydir_from_disk/
    load_recovery_snapshots）。类与 API 不变（≠ 已搁置的 P2-a 拆类）。
  - 跨 TU 共用助手抽 src/cask/cask_internal.hpp（内部头，不进 include/）：
    5 个 ckpt 文件名常量 + io_fault/err/now_sec_default/bytes_to_view/
    component_of_plugin（anon/static → inline，函数体不动）。单 TU 助手
    （writer lock 族、DocmapRelocator）留守 cask.cpp 匿名 namespace。
  - **纯度核验**：脚本比对 git HEAD 版 cask.cpp——每个搬移段在新 TU 逐字节
    原样出现，唯一非原文行是 cask.cpp 的 `#include "cask_internal.hpp"` 与
    内部头的 `inline` 前缀。
  - 收益：最大 TU 3441→2300 行，增量编译与导航双收；恢复域/查询门面各自
    独立演进。

### S21 挂账（审计已确认、本批未做）

- [x] **S21-M6 PostingList AoS→SoA** — **已完成（S22，2026-07-06）**，见下方 S22 段。
- [x] **S21-M3 BM25 查询期 per-term 快照 scratch 复用** — **已完成（S23）**，见下方 S23 段。
- [x] **S21-M4 索引写路径 move 化** — **已完成（S23，positions 半边被 M6 SoA 吸收）**，见下方 S23 段。
- [x] **S21-M7 fold/scan key 快照扁平化** — **已完成（S24）**，见下方 S24 段。
- [x] **S21-M9 ensure_vocab 增量化** — **已完成（S24，两层视图半）**；view 化
  消双份常驻（TBB 节点稳定性验证）仍挂账，见下方 S24 段。
- [ ] **S21-M10 ord_field_lens_ 单字段文档跳过登记**（~80-100B/文档，删除
  统计语义需对拍测试）。
- [x] **S21-A1 hint record 格式 vbyte 化** — **已完成（S23，实现为 gap 差分而非纯推导）**，见下方 S23 段。
- [ ] **S21-A6 sealed-mmap 可信读跳 CRC**（opt-in，先 bench 定量）。
- [x] **S21-B2 inverted_wal 退役** — **用户拍板删除，已完成（S22，2026-07-06）**，见下方 S22 段。
- [~] **S21-B4 tests/support shim 渐进收缩** — **首批已完成（S24）**：
  wildcard/fuzzy/highlighter 15 例迁 TextPlugin 直连，锁定 9→6；余量
  inverted/synonym/searcher/checkpoint_recovery/cask_docvalue/
  search_layer_test，见下方 S24 段。
- [ ] **S21-B6/B7 低优先**：cask.hpp 前置声明化（收益仅 C API TU）；sidecar
  I/O 统一走 io 层 whole-file helper。
- **P3 维持暂留**（nfkc_fold view 化，lifetime 复杂收益边际）；**S13-M4 维持
  信息级**（fstats_ 收缩需扩展 RCU 发布协议，增长极慢不值当）。


---

## S22 批次：M6 PostingList SoA + B2 WAL 退役（2026-07-06）

- [x] **S22-B2 inverted_wal 正式退役（用户拍板删除）**
  - `enable_wal` 生产零调用（仅 tests/bench 直用）；bm25 增量持久化自 S14-4
    起由 delta 链承担。删 inverted_wal.{cpp,hpp}（465 行）+ 13 例测试；剥
    InvertedIndex 的 wal_ 成员/五方法/两写入钩子；删 TextPlugin::truncate_wal
    包装、shim 调用点、bench BM_Wal_AppendOnly。若需断电增量能力，以 git
    历史 inverted_wal.* 为底重新接线（search_config.hpp 注释已记）。
  - 验收：Debug 537/537（550−13 WAL 例）、Release(LTO+bench) 构建绿。
- [x] **S22-M6 PostingList AoS→SoA（倒排最大常驻结构）**
  - 前置：测绘代理全库扫描确认——无 memcpy/指针算术/span<Posting> 连续布局
    假设；v6 落盘本就列式（ord FOR 块/tf 组/dl 组分开），SoA 是其天然内存
    镜像，**字节零变化**；唯一深度 AoS 依赖是 phrase 路径持
    `vector<u32>*`（改 span，CoW 冻结语义保生命周期）。
  - 布局：`ords/tfs/dls` 平行数组 + positions 扁平化（pos_data + pos_off
    u64 哨兵尾，惰性物化——index_positions=false 恒空）。40B/条 + 每条独立
    positions 堆块 → **16B/条（无位置，−60%）/ 24B+紧凑数据（有位置）**；
    千万 posting 级库常驻省数百 MB；CoW 克隆 N+1 次堆分配 → 6 次。
  - 关键点：`append()` 唯一追加入口防漏列错位；`compact_flags` 原地双指针
    压实（每轮先读 pos_off[i]/[i+1] 再写 [w]，w≤i 无冲突）；find/
    serialize_delta 改对 ords 列直接二分；snapshot_flat 退化整列 assign；
    serialize 免 ords_view 物化；bm25_kernels/intersect_u64/FlatPostings
    消费面零改动。
  - 验收：Debug **537/537**、TSan 并发子集（CoW/phrase/并发查询）**91/91**
    零 race、Release bench 全跑通。落盘格式与 C ABI 不变。
  - 后续演进方向（挂账）：snapshot_flat 的整列拷贝可再演进为 CoW 零拷贝共享
    （FlatPostings 持 shared_ptr 列引用）；S21-M3 的 per-term 快照 scratch
    复用与此正交，仍在挂账单。


---

## S23 批次：M3 查询 scratch 池 + M4 写路径 move + A1 hint v3（2026-07-06）

- [x] **S23-M3 BM25 查询期 per-term 快照 scratch thread_local 池化**
  - search 标量/WAND/bool_search 三路径的 per-term 快照容器（3-7 个内层
    vector/term）改 thread_local 池按下标复用；score_bow_topk 改收 span +
    内部 hits/live/dls/contrib 同池化。稳态查询零堆分配。
  - **安全性论证**：三路径全程串行（BOW 经 T6 决策、WAND/BMW 无 TBB
    spawn）→ 无 work-stealing 重入窗口；批量查询按 TBB worker 线程隔离。
    复用槽重置清单：block_upper_bounds（push_back 增长）必须 clear；
    live/dls/dls_filled 由 resize+fill/assign 整段覆盖；must_not 槽的陈旧
    dls 永不被读。WAND 经 order 索引数组排序不搬元素、bool BMW 指针在池
    停增后取得——池槽地址稳定。
  - **实测**（Release 同机对比）：QueryThroughputBOW 1 线程 −24%
    （9.2→7.0µs）、**16 线程 −81%**（95→18µs，分配器争用即多线程瓶颈）；
    SearchHotTerm −15%；BoolMustHot −23%。
- [x] **S23-M4 apply_job 双入口（doc_text move 进原文 LRU）**
  - 生产 on_put（持可变 TextPrepared::job）走 `apply_job(ReduceJob&)`
    move doc_text；shim/降级走 const 版（仅 doc_text 一次拷贝）。共享
    apply_job_impl。每文档省一次全文深拷。原审计的 positions move 半边
    已被 S22-M6 SoA 吸收（span 拷入扁平数组，无 per-term malloc）。
- [x] **S23-A1 hint 文件格式 v3（18B 定宽/条 → 典型 8-9B，−50%+）**
  - 布局（format.hpp）：magic "BCH3" + `[vbyte gap][vbyte total_sz]
    [vbyte keysz<<1|tomb][tstamp u32][key]` + 8B trailer（"BCHE"+CRC 覆盖
    [0,size-8)）。**offset 存 gap 差分**而非纯前缀和推导——连续追加恒
    0（1B），u64 二补数回绕保证正确性不依赖连续性假设。tstamp 保持定宽
    （unix 秒级 vbyte 5B 反而更大）。
  - 兼容：写端恒 v3（cask/merge/migrate 三写点全经 HintFile）；读端三入口
    按文件头 magic 双分派；v2 与 magic 撞值概率 ~2^-32 且后果仅 CRC 失败
    退 fold(data)。hint 可重建零迁移，旧文件 merge/roll 后自然消亡。
  - 测试：v3 golden 字节锁定（43B vs v2 同载荷 79B）+ gap≠0 + u64 回绕 +
    逐字节截断 + v2 兼容读三入口 + 未封口拒收；writer golden 换代 v3。
  - 收益：keydir 启动重建 hint I/O 近半。
- 验收：Debug **544/544**（537+7 新测试）、TSan 子集 **251/251** 零 race、
  Release(LTO+bench) 绿且 bench 实测收益如上。


---

## S24 批次：M7 key 快照扁平化 + M9 vocab 增量化 + B4 shim 收缩首批（2026-07-06）

- [x] **S24-M7 fold/scan key 快照扁平化**
  - keydir `keys_snapshot_`（vector<string>）→ `keys_buf_`（单一拼接）+
    `key_offs_`（N+1 哨兵）；两遍构建（先精确字节和 reserve 再拼接），
    `next()` 按 string_view 切片消费（entries 透明查找）；release
    shrink_to_fit 不留常驻。`CaskIter::drain_live_keys` 同款 →
    `FlatKeys{buf, offs}`，parallel_scan 经 `operator[]` span 消费。
  - 收益：每快照分配 N+1 → 2、峰值内存约减半（每 key 省 32B 头 + malloc）；
    fold/merge/backup/parallel_scan 都在此路径上。
- [x] **S24-M9 ensure_vocab 增量化（两层视图）**
  - `VocabView{base, extra}`：base 大基线重建不拷；extra 排序增量层，重建
    只重排旧 extra ∪ raw delta（O(extra+delta)，阈值封顶）；extra 超
    max(1024, base/8) 才付一次 O(V) `std::merge` 并入基线（摊还 O(V/阈值)）。
    唯一性由 is_new_term（map 首插恰一次）保证 delta ∩ (base∪extra)=∅。
  - 消费方：wildcard 两层各自独立二分区间、fuzzy 两层线性扫——层间无序
    不影响结果（集合语义 + BM25 按 term 求和）。发布协议（vocab_dirty_
    acquire/release + shared_ptr 换指针）不变。
  - **仍挂账（M9 后半）**：词典字符串双份常驻（vocab_ 与 TBB map key）的
    view 化——需先验证 oneTBB concurrent_hash_map rehash 的节点地址稳定性。
- [x] **S24-B4 shim 收缩首批（15 例迁 TextPlugin 直连）**
  - wildcard 4 + fuzzy 3 + highlighter 8，断言集逐条不变；驱动层
    on_write → put_doc+apply_text（host_put_row 同构），双参 on_delete →
    host_delete 逐行复刻；analyzer 配置逐一核对等价；CMake 三目标
    shim → bitcask_text_plugin。**shim 锁定 9→6**。
  - **余量核实（S24 续）**：审计所称 9 个锁定中，inverted/searcher/
    checkpoint_recovery/cask_docvalue 实为只用公开 SearchLayerConfig
    （S19 已解耦）——非真实锁定。synonym_test 已迁（5 例 TextHost 直连）。
    真实锁定 9 → **1**：仅剩 search_layer_test（shim 自测）。
  - **留守判定**：shim + search_layer_test 保留至 legacy_ckpt 退役（P6+）
    ——它是 legacy 统一 search.ckpt 的唯一写端夹具，并以 legacy 输入覆盖
    生产共享解码（apply_docmap_delta_section/walk_chain）；届时 shim
    1038 行 + 测试 922 行一并删除。B4 至此**实质收官**。
- 验收：Debug **544/544**（组合树两次独立全量）+ TSan 子集 **93/93**
  零 race + Release(LTO+bench) 构建绿。

### S24 尾：三 sanitizer 全量正确性验证（2026-07-06）

- **Debug 全量 544/544**；**TSan 全量 543/544**——唯一失败
  `IndexPoolMultiLib.ThreadCountIndependentOfLibCount` 为既知预存项
  （S15 era 已记录「git stash 后未改动 HEAD 同样失败」，S20-8 复核同判）。
- **ASan+UBSan 全量 543/543**（本机**首次**跑该组合；新建 build-asan 树，
  address,undefined）。首跑揪出 6 个失败，全部 blame 定性为**预存 UB**
  （非 S21-24 引入）并已修复：
  - 生产：GetResultView 向量字节 misaligned float* 解引（加对齐守卫，
    未对齐拷入拥有缓冲）；
  - 测试：sv_bytes(临时 string) 存 span 悬垂 ×2 类（helper 改
    string_view + 具名化 3 处）。
- 已知豁免：c_api_test 在 ASan 树链接失败（C 驱动缺 UBSan C++ runtime，
  临时树基建项，CI 矩阵/Debug 树覆盖该测试）；TSan 预存项如上。
- **结论：S21-S24 全部 26 个 commit 在三 sanitizer 下零回归。**


---

## 待办：第十五梯队（S25 内存/线程/格式三维度安全审计 — 2026-07-08）

> 来源：2026-07-08 全代码库安全审计。3 个并行 explore agent（内存安全 / 线程管理 /
> 存储文件格式）+ 直接精读 codec/posix_file/data_file/hint_file/meta_file/keydir/
> cask/merger/hnsw/thread_pool/C API 全核心文件。
>
> **总体结论**：代码库质量高——S11-S24 已系统性修复了大部分并发/格式/生命周期
> 问题（S13-F1~F8 并发 bug 全修、S12-3/3b field.schema/meta CRC 已加、S13-M2 C API
> malloc 检查已加、S13-F2 ord 泄漏已修）。本轮发现的**真实新缺口**集中在：
> ① 几处整数溢出 UB（理论性，FFI 恶意输入可触发）；② parallel_scan 的 RAII 遗漏
> （cask_recovery 已修同型但 parallel_scan 漏了）；③ C API slice 校验缺口；
> ④ close() 等待无超时兜底。

### P0 内存安全（立即可修，投入小）

- [x] **S25-M1 `parallel_scan` 线程创建缺少 RAII join 保护** — 已完成（2026-07-08）· `src/cask/cask.cpp:1187`
  - 加 `JoiningPool` RAII 结构（照搬 `cask_recovery.cpp:500` 同模式），`emplace_back`
    抛异常时已创建的线程在栈展开时自动 join。显式 join 保留（错误在 return 前传播）。
  - 验证：全量 544/544 ctest 通过。

- [x] **S25-M2 C API slice 未校验 `nullptr + size>0`** — 已完成（2026-07-08）· `c_api/internal.h` + `c_api/bitcask_kv.cpp`
  - 新增 `slice_valid()` helper（`internal.h`）。`bitcask_get`/`put`/`put_ex`/`delete`/
    `put_doc`/`put_batch` 六个入口全部加校验：`data==nullptr && size>0` → `BITCASK_ERR_INVALID_OPTION`。

### P1 内存安全（整数溢出 UB，理论性但 FFI 场景不可忽视）

- [x] **S25-M3 `read_bytes_section` + 向量 dim 整数溢出** — 已完成（2026-07-08）· `src/fileops/codec.cpp`
  - 三处修复：① `read_bytes_section` 的 `pos+len` 改 `len > buf.size()-pos`（减法不溢出）；
    ② 量化向量段的 `need = 1+sizeof(float)+dim` 改拆步减法检查；③ f32 向量段的
    `dim*sizeof(float)` 乘法改 `dim > (remaining)/sizeof(float)` 除法检查。
  - 验证：全量 544/544。

- [x] **S25-M4 `parse_leading_pid` 有符号整数溢出 UB** — 已完成（2026-07-08）· `src/cask/cask.cpp:75`
  - 溢出检查从乘法**之后**移到**之前**（`if (pid > (1<<30)) return -1;` 先判再乘）。

- [x] **S25-M5 HNSW buffer reserve 整数溢出** — 已完成（2026-07-08）· `src/vector/hnsw.cpp:1822`
  - `(1 + cfg_.M * 2) * 4 * 8` 从 int 域改全程 `static_cast<std::size_t>` 运算。

### P1 线程管理

- [x] **S25-T1 `close()` 等待 `writes_in_flight_` 无超时** — 已完成（2026-07-08）· `src/cask/cask.cpp:594`
  - 加 30s `wait_for` 超时兜底。超时后 `log_error` 并强制继续释放资源（接受潜在
    UAF 风险优于进程永久挂死）。`writes_in_flight_.wait(n, seq_cst)` 在计数变化时
    唤醒，外层 `for` 检查 `elapsed >= 30s` 则 break。

### 已核实为安全（无需修复，避免重复审计）

- **`data_file.cpp:110` mmap span 悬垂**（代理报告 CRITICAL）—— **误报**。
  `cask.cpp:1094` 的 `GetResultView(std::move(df), ...)` 持有 `shared_ptr<DataFile>`
  锚定 mmap 生命周期，view 存活期间映射不撤。安全。
- **`cask.cpp:1268` GetResultView 移动构造读 moved-from**（代理报告 CRITICAL）—— **误报**。
  mmap 路径下地址稳定（MAP_SHARED 不因 shared_ptr 移动而变），注释已正确解释。安全。
- **field.schema 无 CRC**（代理报告 HIGH）—— **S12-3 已修**（2026-07-01）。
- **bitcask.meta 无 CRC**（代理报告 HIGH）—— **S12-3b 已修**（2026-07-01，v3 加 CRC）。
- **hint v3 trailer CRC** —— **S23-A1 已实现**（2026-07-06）。
- **data record CRC 覆盖** —— 正确（覆盖 Type..Value，标准做法）。
- **torn-write 截断恢复** —— 正确（`fold` + `truncate_to(last_valid_end)`）。
- **vbyte_read shift>=64 防御** —— 正确。
- **IndexPool 生命周期 / HNSW seqlock / KeyDir 屏障协议** —— 均已 TSan 验证正确。

---

## 待办：第十六梯队（S26 索引写入吞吐优化 — 2026-07-09）

> **背景**：wiser-cpp（Wikipedia 全文索引，jieba 分词）实测「索引吞吐 ≪ KV 写吞吐，
> 且随文档正文 V 增大急剧恶化」。根因分析（本会话）：
>
> `put_doc` 相较纯 KV `put` 多背一条异步索引流水线（thread_pool.hpp：queue → N 个
> map worker 并行分词 → reorder buffer → **单 reducer 串行插倒排**）。写路径经背压
> （submit_index_task 阻塞，cask.cpp:730）被这条流水线的吞吐钳住。倒排本体已是
> `tbb::concurrent_hash_map`（桶级锁，inverted.hpp:25）→ **串行不是数据结构限制，是
> 「单 reducer」的刻意设计**。
>
> **优化梯队**（按性价比排序，本批先做 ②③——库内、低风险、见效快）：
>   - ① 并行化 reduce：term-hash 分片多 reducer（最大杠杆，抬天花板；工程量大）——**待评估**
>   - ② catch-all 可配置开关（多字段场景倒排量直接减半）——**本批**
>   - ③ 削减每文档分配抖动（大 V 尤甚）——**本批**
>   - ④ reducer 内批量/排序插入（吞吐 + 局部性）——待办
>   - ⑤ position delta-varint 编码（省内存/ckpt/IO）——待办
>   - ⑥ checkpoint 序列化并行/流式（缓解 1.7GB flush 停顿）——待办

### P1 ② catch-all 可配置（多字段倒排减半）

- [x] **S26-2 `index_catch_all` 配置开关** — 已完成（2026-07-09，`text_plugin_config.hpp:31` +
  CatchAllDisabled 测试，见下方验证节）· `text_plugin_config.hpp` + `search_config.hpp` + `src/search/text_plugin.cpp`
  - 现状：`map_analyze`（text_plugin.cpp:145-181）**无条件**把非默认字段词项合并进
    默认字段（catch-all），使 `search_text/phrase/near`（只查默认字段）也能命中多字段
    文档。代价：每个词 `add_doc` 两遍 → reducer 工作量 ×2、posting 内存 ×2、`bm25.ckpt` ×2。
  - 改动：`TextPluginConfig` / `SearchLayerConfig` 新增 `bool index_catch_all = true`（默认
    保持既有行为），经 `text_config()` 透传。`map_analyze` 的 catch-all 累积分支加
    `config_.index_catch_all &&` 守卫；关闭时 `ca_data` 恒空 → `apply_job_impl`（:228）的
    默认字段合并自动跳过，无需二改。
  - 语义：关闭后 `search_text` 对多字段文档不再命中（只走 `search_fields` 字段限定）——
    调用方按查询形态自选。纯默认字段写入（field 名为空）不受影响（走 `wrote_default`）。

### P1 ③ 削减每文档分配抖动

- [x] **S26-3a `map_analyze` 高亮正文深拷按需化** — 已完成（2026-07-09，`text_plugin.cpp:223`）· `src/search/text_plugin.cpp`
  - 现状：`job.doc_text = std::string(fields.front().second)`（:183）**无条件深拷整段正文**
    （O(V)/doc），即便高亮 LRU 关闭（`doc_text_cache_max==0`，`apply_job_impl` put 恒被丢弃）。
  - 改动：`doc_text_cache_max==0` 时跳过该拷贝（置空）。config 语义本就是「0 → 高亮降级
    无片段」，行为不变；大 V 下省一次整文档 memcpy + 分配。
- [x] **S26-3b `job.fields.reserve`** — 已完成（2026-07-09，`text_plugin.cpp:179`）· `src/search/text_plugin.cpp`
  - `map_analyze` 循环前 `job.fields.reserve(fields.size())`，免 vector 逐字段扩容。
- [ ] **S26-3c（待办，未纳本批）`ord_field_lens_` 结构** · `text_plugin.hpp:368`
  - 按 ord 单调递增的 `unordered_map<uint64_t, vector<...>>`，随 live doc 线性堆积（on_delete
    字段精确扣减依赖）。改扁平结构/分段是内存优化，但触删除记账，风险较高，另批处理。

### 验证
- [x] `search_layer_test` 新增 `CatchAllDisabled`：`index_catch_all=false` + 多字段写入 →
  `search_text` 不命中、`search_fields` 命中；对照默认（catch-all 开）`search_text` 命中。—— 通过。
- [x] 全量 ctest 回归（build-rel + build-clang 双树，见 [三构建树分工]）。—— build-clang 545/545、
  build-rel 构建通过。

### P0 ②③ 落地时发现的既存缺陷（本批顺带修复）

- [x] **S26-B `apply_delta` DEBUG 跳段断言恒误报** — 已完成（2026-07-09）· `src/bm25/inverted.cpp:2561`
    + `include/bitcask/inverted.hpp:426`
  - 现象：`bitcask_cask_docvalue_test` 4 个 checkpoint 用例（CheckpointVecPayloadAppends /
    DeltaChainSelectiveSections / DeltaChainDeletesAndOverwrites / DeltaChainLengthBound）在
    Debug 树 `apply_delta` 断言 abort。git stash 核实 clean tree 同样失败——**非 S26-2/3 引入**，
    是上一提交 `f2f56d3`（名为 docs，实加了 DEBUG 断言）。
  - 根因：断言 `from_ord ≤ max_indexed_ord_ + 1` 把**全局链 coverage 水位**（from_ord）与
    **每字段 posting 最大 ord**（max_indexed_ord_）当成差 ≤1。二者差着「不产生本字段 posting
    的 ord」数——checkpoint/skip 的 RunFn ord、删除墓碑、向量-only 文档、稀疏命名字段，正常
    负载必然发散。实测 `from_ord=61 vs wm=59`（ord60 被 checkpoint RunFn 吃掉）。
  - 修复：移除该断言（+ 随之无用的 `#include <cassert>`），保留契约文档并改写描述断言的段落
    ——链完整性本就由调用方 `walk_chain`（kDeltaInfo 三元组）独家保证，本层无法本地断言。恢复
    正确性由用例自身断言（reopen 后 search 命中数）验证。
  - 验证：4 用例转绿，全量 545/545。

---

## 待办：第十七梯队（S27 分段索引 — LSN/docid 解耦 + 并行构建 — 2026-07-09）

> 设计文档：[`doc/segment-index-design-zh.md`](doc/segment-index-design-zh.md)（本梯队的完整设计）。
>
> **背景**：S26 会话把「索引吞吐 ≪ KV 写」的优化路径推进到架构层。经与 Lucene/ES 对照，
> 确认正解不是 S26 梯队的 ① term-sharded 多 reducer（踩 per-field 幂等水位 / 跨 shard 原子性 /
> 全局统计竞争），而是**分段（segment）模型**：多个不可变段 + 后台 merge。段模型一次性收敛
> 三个已知痛点——ord 死内存回收（[ord-recycling] §5 方案 A 未做）、索引并行吞吐、delta 链复杂度
> （S26-B 误报即其副作用）。**S27 取代 S26 ①。**
>
> 核心决定：identity(key) / LSN(ord，不变) / docid(段内本地，新增) 三分；docid 随段 drop 自然回收；
> 段替换 delta 链。

### Stage 2 设计基线已闭合（详见设计文档 §3.4/§3.5/§4）

- [x] **BM25 跨段统计** = **G-on-the-fly**（对标 ES 单节点/段级：查询时段本地 df 求和算一次全局 idf，
  零漂移、不维护全局表）。设计文档 §4。
- [x] **段本地 doc_store** = 封口段平坦定长（chunk 退役）承载 R2/R3/R4；全局 resolver `key→(seg,docid)`
  （B1）担 R1；保留 `lsn[docid]` 供 RRF。设计文档 §3.4。
- [x] **多段查询归并** = 串行逐段 + 全局第 k 名阈值传播 + 大小 k 的 min-heap 并集；查询独立于
  IndexPool；作 `search_fields` 内层。设计文档 §3.5。

### 分阶段实现（详见设计文档 §5/§7）

- [x] **S27-1 LSN/docid 概念拆分** — 已完成（2026-07-09）· 新增 `include/bitcask/index_ids.hpp`
  + 接缝注解 `keydir.hpp`/`index.hpp`/`inverted.hpp`/`search_types.hpp`
  - 采**方案 A（接缝处窄拆分）**：中央头定义 `Lsn`（全局单调；恢复/MVCC/幂等水位；`alloc_ord`
    产出）与 `DocId`（倒排存储值/数组下标；将来段内本地、merge 可重编）**弱别名**（同型 uint64、
    数值相等、零行为/性能变更）。只在关键接缝标注角色：keydir `alloc_ord/advance_ord/peek_next_ord`
    → Lsn；docmap `Index` 写/读接口（`put_doc`/`set_doc_len`/`is_live`/`ord_to_ext`/…）→ DocId、
    `remove` tomb_ord → Lsn；`InvertedIndex::add_doc` → DocId、`max_indexed_ord` → Lsn 水位；
    `SearchHit/ReduceJob.ord` → Lsn。深层内部（posting 编码/SIMD/HNSW/format）暂留生 uint64，
    强类型强制留待 Stage 3 docid 真正发散时按段代码局部上。
  - 验证：build-clang 545/545、build-rel 构建通过（双树）。零行为变更（别名同型，全调用点无改动即编译）。
- [x] **S27-2 段抽象 + 多段读** — 已完成（2026-07-09，全 4 slice 见下）：复用 InvertedIndex base
  当段格式；实装 §3.4 doc_store + §3.5 查询归并。分 4 slice：
  - [x] **Slice 1 外部统计注入（G-on-the-fly enabling primitive）** — 已完成（2026-07-09）·
    `inverted.hpp`/`inverted.cpp` + `inverted_test.cpp`
    - `InvertedIndex` 新增 `ExtStats{N, sum_dl, term→全局 df 的 map}` + `doc_freq(term)` 访问器；
      `search`/`search_wand` 加 `const ExtStats* ext=nullptr`（默认 nullptr=本地统计，**零行为变更**）。
      ext 非空时 idf 用全局 df（回退本地 live_df）、avgdl 用全局 N/sum_dl；idf 一致用于块上界与
      打分 → WAND 剪枝仍正确。`score_bow_topk` 加 `global_df` 参数。
    - 测试：`ExtStatsSelfEquivalenceScalar`/`...Wand`（self-stats 注入与本地打分逐位一致，覆盖标量+WAND）
      + `ExtStatsInjectionAffectsScore`（只注入更小 df → idf 抬升→分更高，证明注入被采纳）。
    - 验证：build-clang 548/548（既有 545 + 新 3，证零回归）、build-rel 构建通过。
  - [x] **Slice 2 多段查询归并（§3.5，G-on-the-fly）** — 已完成（2026-07-09）·
    新增 `include/bitcask/segment_query.hpp`（header-only）+ `inverted_test.cpp`
    - `SegmentView{inv, live, key_of, lsn_of}`（段最小只读查询视图，本地 docid）+
      `multi_segment_search()`：① 跨段聚合全局 N/sum_dl/df（G-on-the-fly）② 串行逐段用同一 idf
      打分（`search(...,&ext)`）③ 大小 k 并集归并（一 doc 只在一段，不求和）。阈值传播（WAND floor）
      属纯剪枝优化、不改结果，留待后续 slice。
    - 测试 `SegmentMergeEquivalence`：8 文档「单索引 whole」vs「均分 2 段」→ multi_segment_search
      与 whole.search **逐位同 key 同分**（含 lsn_of 还原全局 LSN）；反证段本地统计（ext=nullptr）
      idf 与单索引不同→分数不一致（证 G-on-the-fly 必要性）。
    - 验证：build-clang 549/549、build-rel 构建通过。
  - [x] **Slice 3 SealedSegment 落盘 round-trip** — 已完成（2026-07-09）·
    新增 `include/bitcask/segment.hpp`（header-only）+ `search_checkpoint.hpp`（+kSegDocStore=14）+ `inverted_test.cpp`
    - `SealedSegment`（IS-A LiveChecker）= 段内 `InvertedIndex`（默认字段）+ **平坦定长 doc_store**
      （docid→{key, lsn, DocSlot, live}，§3.4 chunk 退役）。`add()` 构建、`view()` 出 SegmentView、
      `save()`/`load()` 复用 SearchCheckpoint 段级 CRC 容器（section kBm25Default=InvertedIndex::serialize、
      kSegDocStore=平坦 doc_store 编码）。多字段/向量段化后续。
    - 测试 `SealedSegmentRoundTrip`（内存段 → save → load → `multi_segment_search` 结果逐位一致，
      含 lsn 还原）+ `SealedSegmentCrcReject`（篡改字节 → 段级 CRC 失败 → load 返回 nullptr）。
    - 验证：build-clang 551/551、build-rel 构建通过。
  - [x] **Slice 4 SegmentSet 段管理器 / 活跃段清单** — 已完成（2026-07-09）·
    新增 `include/bitcask/segment_set.hpp`（header-only）+ `search_checkpoint.hpp`（+kSegManifest=15）+ `inverted_test.cpp`
    - `SegmentSet`：管理目录下一批活跃 `SealedSegment` + `segments.manifest`（唯一原子提交点，复用
      SearchCheckpoint tmp+rename + 段级 CRC）。`open`（读清单→载各段，缺清单=空集，段损坏→nullptr）/
      `add`（落盘段文件+提交清单，提交失败回滚）/ `search`（跨段 G-on-the-fly 归并）/ `drop`（删文件+提交）/
      `next_seg_id` 单调。
    - 测试 `SegmentSetAddQueryReopen`（add 2 段→查询→**重开逐位一致**）+ `SegmentSetDrop`（drop 后只剩
      存留段、重开仍生效、next_seg_id 不回退）。
    - 验证：build-clang 553/553、build-rel 构建通过。
    - 注：仍为**隔离基础设施**（未接入 Cask/IndexPool/plugin）；live 路径接线 = S27-3。

  **S27-2 收官**：段已是能查、能落盘、能校验恢复、能管理生命周期的持久化实体；多段查询与单索引
  逐位等价（G-on-the-fly）。全 4 slice header-only、零回归（553/553 双树）、**未触碰任何现有 live 路径**。

- [ ] **S27-3 段累积替换 delta 链**（**部分完成** 2026-07-09）：checkpoint flush 新段 + 后台 merge。
  删 delta 链 + 死内存回收。仍单写者。
  - [x] Slice A/B1/B2a/C 已落地（`ea7794f`/`6148e04`/`3be3f6c`）：SealedSegment 多字段扩展
    （fields_ map + 多字段 save/load + multi_field_segment_search + mark_dead）、Building 段镜像 +
    查询 9/9 走 [SegmentSet+Building] 多段归并、delta 链退役（CheckpointDeltaChain×3 测试删除）。
  - [ ] **剩余 B2b/D/E**：删 fields_ + flush 走段集 + recovery 重写 + legacy 迁移 + 段级 merge。
    设计已闭合（双 manifest 冲突分析 + 方案 A + 5 步实现计划）：
    [`docs/design/s27-3-b2b-recovery-design.md`](docs/design/s27-3-b2b-recovery-design.md)。
- [ ] **S27-4 并行 builder（DWPT）**：多段内单线程 builder，文档分派。**吞吐红利落地。**依赖 S27-3 收官。

### 关联既有设计（勿重复造轮子）
- [`ord-recycling-design-zh.md`](doc/ord-recycling-design-zh.md)：ord 三角色 / per-write 硬约束 /
  方案 A（seq/ord 解耦，段模型让其免费发生）。
- [`recovery-unified-checkpoint-design-zh.md`](doc/recovery-unified-checkpoint-design-zh.md)：
  当前 base+delta 链（S27-3 替换目标）。
- [`merge-policy-zh.md`](doc/merge-policy-zh.md)：merge 触发/执行（S27-3 段级 merge 复用基础）。

---

## 待办：S28 doc.text 多字段模式下未索引为默认字段（Bug — 2026-07-09）

> 来源：wiser-cpp 移植实测——`put_doc` 同时设置 `text`（正文）与 `fields`（命名字段）后，
> `search_text` 无法命中正文。详见 [`docs/design/bug-doctext-ignored-multifield.md`](docs/design/bug-doctext-ignored-multifield.md)。
>
> **根因三层**：① `prepare()` 多字段路径只传 `doc.fields`，丢弃 `doc.text`；
> ② `InvertedIndex::add_doc` 水位幂等保护阻止对同一 (field, ord) 调两次；
> ③ `wrote_default` 标志使 catch-all 合并被跳过 → kDefaultField 只有部分词元。
>
> `DocInput.text` 注释（`cask.hpp:233`）写"作默认字段"，实现未兑现 → **API 契约违背**。

- [x] **S28-1 `doc.text` 在多字段模式下索引为 kDefaultField** — 已完成（2026-07-09，`95631e5`，
  测试 `cask_docvalue_test.cpp` S28-1 用例 + 全量回归绿）· `src/search/text_plugin.cpp` + `include/bitcask/text_plugin.hpp`
  - **prepare()**：多字段路径中 `doc.text` 非空时前置 `{kDefaultField, doc.text}` 到传入 `map_analyze` 的 fields 列表。
  - **map_analyze()**：`index_catch_all` 开启时 kDefaultField 词项也纳入 `ca_data`（不设 `wrote_default`），
    使 `apply_job_impl` 单次 `add_doc(kDefaultField, ca_data)` 写入 doc.text + 命名字段的全部词元；
    `index_catch_all` 关闭时 kDefaultField 走直接写入（保持现有 `wrote_default` 语义）。
  - **on_put()**：单文本兜底分支同步——fields 非空且 doc.text 非空时不只调 `apply_text`（会撞水位）。
  - **约束**：kDefaultField 的所有词元必须在**单次 `add_doc**` 中写入（水位幂等保护硬约束）。
  - **关联**：S26-2 `index_catch_all` 配置开关——关闭时 doc.text 仍需能进 kDefaultField（走直接写入）。
  - **测试**：多字段 + doc.text → `search_text` 同时命中 title（经 catch-all）和 body（doc.text）；
    反向验证 `index_catch_all=false` 时 body 命中、title 不进默认字段（走 `search_fields`）；
    checkpoint round-trip 后行为不变。

---

## 待办：S29 批次（全量 bench 复测 + 四路热点分析 — 2026-07-10）

> **来源**：build-rel 全量 131 项 benchmark 复测（S28 代码，6 核 @3.0GHz，无 AVX-512，
> 运行时派发 avx2+fma / avxvnni）+ 四路并行代码分析（Cask 写路径 / 查询扩展性 / HNSW 插入 /
> 通用热路径）。结果 JSON 已存 scratchpad。
>
> **基线关键数字**（回归对照用）：
> - `BM_Cask_Put_Concurrent` 1→8 线程 902k→48k ops/s（**负扩展 18×**）
> - `BM_Inverted_QueryThroughputBOW` 1→4 线程 7.22→7.47us（**零扩展**，6 核机）
> - `BM_KeyDir_Get_MultiThreaded` 1→8 线程 CPU 37.8→63.7ns（共享读退化）
> - `BM_Hnsw_Insert/100000` 82s（1.2k/s；10k 时 3.0k/s → 超线性变慢）
> - `BM_Text_AnalyzeNgramCjk` 40 MiB/s；`BM_Crc32_Hw_Streaming` 500 MiB/s（非流式 32 GiB/s）
> - `BM_IndexPool_MapSpeedup` 4 线程即平台化（484k/s）
>
> **三大病根**：① 逻辑只读路径对共享 cacheline 做原子 RMW（TBB 桶锁 / KeyDir 独占 mutex）
> → cacheline 弹跳；② 短临界区 mutex collapse（`write_mu_` 包住 encode+pwrite+keydir）；
> ③ HNSW f32 导航工作集 153MB 打穿 L3（cache-miss bound）。

### P0 快赢（小 diff，现有 bench 直接验收）—— 已完成（2026-07-10）

> **落地验收**：build-rel 全量构建通过（bench 树抓 API 破坏）+ build-clang ctest
> **555/555 全绿**。bench 复跑：`BM_Hnsw_Insert/10000` 3375→3030ms（**-10%**，两次
> 复跑一致）；`BM_P12_MetaBlobAccess`/`QueryThroughputBOW` 波动在噪声带内（±10-15%）
> ——BOW 扩展性未解（预期内：剩余的每词 1 次 find 仍弹 cacheline，根治在 S29-6
> epoch/RCU），本批收益主体是**结构性消分配**（S27 分段聚合路径的 string/map 节点
> 分配全清零）。

- [x] **S29-1 `InvertedIndex::search` 两趟 find 合并为一趟** —— 已完成 · `src/bm25/inverted.cpp:445`
  - 单趟 find 时在 accessor 下拷出 `shared_ptr<const PostingList>`（P2-min CoW 协议：
    读者持引用 → 写者 use_count>1 时克隆替换，与 phrase/near 读者同模式），暂存
    thread_local 数组；WAND 路由读 size()、标量/WAND 快照均复用指针 → 每词桶锁
    RMW 2→1 次。`search_wand` 加 `pls` 参数（私有、唯一调用方是 search），其内部
    find 趟一并消除。ReleaseRefs guard 兜住所有出口释放引用（防旧版本钉住 + 写者恒克隆）。
- [x] **S29-2 `meta_lookup` 去每调用堆分配 + 提前退出** —— 已完成 · `include/bitcask/meta_codec.hpp`
  - 删 offsets vector 与二分（二分建立在强制 O(n) 线性预扫上，纯额外开销），单趟
    扫描边解析边比较：命中就地 decode 返回；key 升序（encode_meta assert 的格式
    不变式）→ `ek > key` 提前 break。零分配 + 平均半程退出。
- [x] **S29-3 HNSW `select_neighbors`/`_int8` 内部 vector 改 thread_local** —— 已完成 · `src/vector/hnsw.cpp`
  - `picked`/`picked_vecs` 池化（对齐 t_visited/pool/tl_cands_buf 既有模式），尾部
    `std::swap` 替代 move 让两缓冲轮换复用。单写者协议保证无重入。实测建图 -10%。
- [x] **S29-4 `doc_freq`/`df`/`df_live` 去 `std::string(term)` 构造** —— 已完成 · `src/bm25/inverted.cpp`
  - 新增 `tls_term_key(string_view)`：thread_local string 复用容量，稳态零分配
    （不依赖 TBB 异构查找支持，零版本风险）。
- [x] **S29-5 S27 分段查询层容器复用（范围调整）** —— 已完成 · `include/bitcask/segment_query.hpp`
    + `include/bitcask/inverted.hpp` + `include/bitcask/segment.hpp`
  - **做了**：`ExtStats::df` 类型从 `unordered_map*` 改扁平
    `vector<pair<string,uint64_t>>*`——查询词个位数：消费侧（score_bow_topk /
    search_wand）线性扫描优于 hash find；生产侧 `multi_segment_search` 的 global_df
    thread_local 槽位复用（resize 截断残留槽 + string assign 复用容量，稳态零分配），
    `multi_field_segment_search` 的 per_field_df 同步扁平化。附带修正：原 map 版对
    重复 term 会重复累加 df（emplace 去重 + `[t]+=` 撞同一 entry），扁平版每槽独立、
    消费取首匹配，语义更正。测试 3 处适配。
  - **评估后不做**：① 双重排序——`text_plugin.cpp:619` 的重排**语义必需**（把
    multi_segment_search 的 key-asc 并列序对齐单索引路径 score_bow_topk 堆序的
    ord-desc + 缓存语义），≤k_req 个元素成本可忽略，已加注释防误删；② views 池化
    ——`std::function` 单指针捕获走 SSO 不分配，实际仅省每查询 1 次 vector 分配，
    不值得引入「返回 thread_local 引用」的重入隐患。
  - **遗留（并入 S29-6 或独立）**：阶段 1 doc_freq 与阶段 2 search 对同段同词各
    find 一次——合并需 search-with-pls 公开变体，超快赢范围。

### P1 结构性（量级改变，需设计，各自独立可做）

- [ ] **S29-6 读路径「零共享写」改造（epoch-RCU + 乐观读）**（**设计已闭合** 2026-07-10）·
    `src/keydir/keydir.cpp` + `include/bitcask/keydir.hpp` + `include/bitcask/inverted.hpp:486`
  - 病根：读路径每次对共享锁字原子 RMW——`KeyDir::get` 拿 shard 独占 `std::mutex`（S5 实验
    遗留）；TBB `const_accessor` 读也写桶锁字。换 shared_mutex 无效（reader 计数同样写
    cacheline）。8 线程退化 = 每 acquire 一次锁字 RFO 缓存缺失（~26ns），非阻塞争用。
  - ⚠️ **实现前审计（2026-07-10）推翻原 sketch**：naive「每 shard seqlock + POD 乐观拷贝」
    **不成立**——seqlock 事后校验兜不住 UAF/段错误。三个致命场景：① ankerl map grow/rehash
    立即 free 旧数组（读者正遍历）；② `remove` 无 fold 分支热路径物理 erase（`keydir.cpp:699`,
    free key string 缓冲，读者 find 正比较）；③ variant Single→Multi 升级（可判别回退，唯一
    能安全处理的一类）。
  - **修正设计**（四件套缺一不可，P1-P3 必须整批上线）：P1 热路径 erase → tombstone-in-place
    （物理回收挪 quiescent 点 + 写者 sweep）；P2 epoch 读者注册表 + per-shard limbo 延迟回收
    + ankerl 自定义 allocator；P3 get 乐观快路径（per-shard seq 校验 + fold/Multi 回退 +
    重试上限 + 运行期回退开关）；P4 TSan/ASan 定向压力 + bench 验收。
    详见 [`docs/design/s29-6-keydir-lockfree-read.md`](docs/design/s29-6-keydir-lockfree-read.md)
    （**§5 评审决议已确认**：TSan 豁免选 b、KeyDir 先行、增量 sweep、运行期开关）。
  - [x] **P1 已完成（2026-07-10）**：`remove` 无 fold 分支 erase → sibling sentinel 墓碑覆写
    （`entry_at_epoch` 的 Single 分支上报 is_tombstone——原「Single 从不表示墓碑」不变量
    解除）；`put_insert` 原「理论不可达」的 Single 覆写分支变为复活路径（epoch 守卫使 fold
    迭代器语义不变）；写者增量 sweep（阈值 1/8 表长、每写扫 8 槽、cursor 续扫）+
    `collapse_multi_entries_barrier` quiescent 点全量清；snapshot 跳墓碑（count 与写出严格
    一致）+ histogram/apply_pending 计数口径同步。Shard 新增 `tombstones`/`sweep_cursor`
    （仅锁下读写）。
    - 验收：clang 全量 **558/558**（含新增 `KeyDirTombstone` 定向 ×3：删除不可见/复活、
      delete-heavy 2000 键 sweep 回归、快照跳墓碑往返）；TSan keydir/iter/merge/fold 58/58；
      ASan keydir/cask 132/132；build-rel 构建通过。
  - [x] **P2 已完成（2026-07-10）：epoch 读者注册表 + limbo + ankerl allocator**。
    - 新增 `include/bitcask/epoch_reclaim.hpp`（通用,倒排二期复用）：进程级
      `epoch::Registry`——64 个 cacheline 对齐读者槽（进入 seq_cst store 自线程槽、
      退出 store 0,零共享 RMW）、`advance()`/`min_active()`（seq_cst 交错论证见文件头）、
      thread_local 槽位 RAII（耗尽 → nullptr → 回退加锁,恒正确）。
    - `Shard` 新增 `Limbo`（raw 数组 / key 遗骸 / Entry 遗骸三池,仅锁下访问,声明先于
      entries 保证析构序）；map 换 `LimboAllocator`（deallocate → retire,覆盖 rehash/grow/
      析构的旧数组,设计 §1.1）；**erase 零 free 化**：sweep/collapse/apply_pending 三处
      erase 前把 key/Entry **move 进 limbo**——erase 只析构 moved-from 空壳（免改 map key
      类型即覆盖设计 §1.2）。回收：写者攒批（≥8 项,put/remove 尾部）+ collapse quiescent
      点全量,stamp 前缀释放（advance 全局单调）。
    - P2 期读者未注册 → min_active 恒 max → 达阈值即全清,行为等价即时 free,零语义变化。
    - 验收：clang 558/558；ASan keydir/cask/snapshot 137/137；TSan 135/135；
      `BM_KeyDir_*` 全部在基线噪声带内（Get 32ns / Put 61ns,零回归）。
  - [x] **P3+P4 已完成（2026-07-10,评审选 A：自建表）**。新增
    `include/bitcask/seq_shard_table.hpp`——seqlock 原生开放寻址表替换 Shard 的
    ankerl map（§6.3 阻断:ankerl 桶内部 private 且是 submodule）：self-describing
    桶块（块头带 mask,指针/界恒一致,deref 恒 in-bounds）、稠密 pair 数组、
    backward-shift 删除、表内置 limbo（erase 恒零 free）、深度感知 WriteSection、
    `try_get_optimistic` 逐跳 copy→seq 验证→使用配方；`KeyDir::get` 快路径
    （开关/fold 检查 → epoch 槽注册 → 乐观读,Single 判别采纳,Multi/重试 ×3 回退加锁）。
  - **实测（BM_KeyDir_Get_MultiThreaded CPU）**：1→8 线程 43.9→44.6ns **完全平坦**
    （基线 37.8→63.7ns 退化 +69%）；4 线程吞吐 +30%、8 线程 **+43%**。单线程 +16%
    （37.6→43.7ns,槽注册 seq_cst + 拷贝校验固定成本;2 线程起反超,有运行期开关）。
  - **验收**：clang 全量 560/560（含新 `KeyDirOptimisticRead` 压力 ×2:4 读者 vs
    put/remove/grow 写者,撕裂检测不变量 offset==ord）；TSan 123/123（零 race）；
    ASan 125/125；GCC -O2 独立压力 55M gets 零 miss 零 torn。
  - **实现期抓获并修复的 5 个缺陷**（各有防回归注释/断言,教训已写入表头注释）：
    ① 嵌套 WriteSection 使 seq 变偶(内层 +1+1)→ 深度感知只最外层 bump;
    ② SSO 陷阱:key 比较 mismatch 分支未验 seq → 撕裂误判「不同 key」伪 miss;
    ③ **严格别名违规**:uint64_t* 直读 Bucket/string,GCC -O2 TBAA 读陈旧零值
    （单线程 100% 伪 miss,clang 宽容差点漏网）→ `__atomic_load_n` 豁免;
    ④ TSan libc 拦截器:no_sanitize 压不住 memcpy/memcmp 真实调用 → builtin 定长;
    ⑤ LSan:析构序——values 终末数组 retire 晚于手动 drain → RawLimbo 自析构。
  - ⚠️ P1 单独上线的既知代价：delete-heavy 且长期无写/无 fold 的库,墓碑驻留至下次写触发
    sweep——内存有界（≤1/8 表长 + sweep 滞后量），语义无损。
  - 二期：倒排桶锁复用同一套 epoch 注册表（TBB 桶锁不可下探 → 换表或 thread_local
    term→snapshot 缓存）。
- [ ] **S29-7 Put 写路径 group commit（单 handle 多写者扩展）** · `src/cask/cask.cpp:1319-1378`
    + `include/bitcask/data_file.hpp`
  - 病根：`write_mu_`（`cask.hpp:785`）包住 encode + pwrite + hint + keydir 全序列，短临界区
    高争用 = 经典 mutex collapse（1→2 线程即 902k→175k）。fsync 不在 bench 路径内
    （`o_sync=0`），纯锁交接开销。
  - 方向：leader-follower 批量 pwrite——并发 put 编码后入队，leader 一次 pwrite 覆盖多条
    连续 record，完成后逐条 apply keydir + 唤醒 waiter。**字节仍在返回前落盘，不违反
    [WAL data file = immediate durable] 语义**（只合并 syscall，非延迟持久化）。`put_batch`
    （`cask.cpp:1386`）已实现同思路可参考。
  - [x] **铺垫已完成（2026-07-10）：encode 移出锁**。审计发现硬不变量：**文件序 ==
    ord 序**（恢复按 fold 序回放 + `add_doc`/HNSW ord 水位自门 ⇒ ord 必须锁内分配，
    record 不能带真 ord 预编码）。方案：record 锁外用**占位 ord=0** 完整编码
    （O(V) memcpy 出锁），锁内 `codec::patch_data_record_ord`（补 8 字节 + 一次
    CRC 扫描）+ 新 API `DataFile::write_encoded`（纯 pwrite + 偏移推进）。
    - 改动：`put`/`put_doc` 的校验/tstamp/向量归一化/字段 intern（自带
      shared_mutex）/DocValue 编码/record 预编码全部前置锁外；`write_and_keydir`
      改收可变 record span（重试路径 patch ord2 复用同 buffer）；put_batch
      merge-race 重试点就地编码适配。附带修正：put_doc 的 roll `about` 从低估
      （漏 vector/fields 段）变为 record 精确长度。
    - 验收：双树 + ctest 555/555 + TSan cask/put/merge/crash 子集 117/117。
      `BM_Cask_Put_*`（128B 值）持平——预期内，小值时编码仅占临界区 ~2%,
      collapse 主因是锁交接；铺垫收益随值增大（O(V) 出锁），且
      `write_encoded`/`patch_data_record_ord` 即 group-commit leader 所需原语。
  - [x] **主体已完成（2026-07-10）：flat-combining 组提交**。`GcRequest` 队列 +
    `submit_group_commit`（入队 → try_lock 成 leader / 自旋 4096 pause →
    cv 100µs 兜底退化）+ `process_gc_batch_locked`（leader **循环 re-drain 至
    队列空**——批随争用自然增长）：按队列序 alloc_ord + patch + 拼接,每段
    **一次 pwrite**,再逐条 hint/keydir/组提交计数;跨 max_file_size 分段
    flush+roll;失败请求补 Skip;merge-race 单条走 write_and_keydir 重写;
    fsync 失败沿旧语义(post_err:Add 照提 error 照返)。
    「入队序 == ord 序 == 文件序」由 leader 独占 write_mu_ + 队列序分配保证。
  - **实测（BM_Cask_Put_Concurrent,128B tmpfs）**：2 线程 175k→358k(**2.0×**)、
    4 线程 107k→244k(**2.3×**,聚合 428k→976k/s,已贴近 leader 单流处理上限)、
    8 线程 48.6k→71.6k(1.5×,6 核超订阅自旋有代价);单线程 863k(-1.3%,噪声级)。
  - **调优教训（两轮实测迭代）**：① leader 必须循环 re-drain——只 drain 一次则
    处理期间到达的请求全部错过,批恒 ≈1,组提交名存实亡(首版仅 +10%)；
    ② follower 必须先自旋再睡——cv futex 睡/醒一轮 ~10-20µs,直接睡比基线还慢
    3×(次版实测)。done 为原子发布点,自旋免锁读。
  - **验收**：clang 560/560；TSan cask/put/merge/close/keydir 130/130；
    **ASan 全量 560/560**；build-rel 构建通过。
  - **禁区**：任何把 put 的 data pwrite 延迟到返回之后的方案（破坏 WAL 语义）；
    `write_buffered`/`batch_buf_` 明确禁用于单条 put（`data_file.hpp` ⑪ 否决记录）。
  - 备选零代码方案：按 key 分片多 Cask 实例（设计文档既有结论，`cask.hpp:13-14`），
    若业务可接受则优先。
- [x] **S29-8 CJK 分词双重解码融合 + tf-only 免 positions** —— 已完成（2026-07-10）·
    `include/bitcask/text_utils.hpp` + `src/text/analyzer.cpp` + `src/text/jieba_analyzer.cpp`
    + `include/bitcask/ngram_analyzer.hpp`
  - **做了**：① 慢路径抽为 `nfkc_map_slow`；新增融合入口 `nfkc_fold_codepoints[_reuse]`
    ——fold 快路径校验趟直接产出 `CpInfo`（ASCII 记 tolower 后码点，与重解 out 逐位
    一致），命中即免第二遍 `to_codepoints` 全量重解。接入 Ngram/Whitespace 全部 4 个
    analyze 入口 + jieba 全文与逐词 2 处。② Ngram 分词主循环抽为共享 `ngram_tokenize`
    模板，`NgramAnalyzer::analyze` 覆写 tf-only 版（不构造 per-term positions vector；
    term 集与 tf 与派生版逐位一致，min_token_length/停用词语义共享）。
  - **实测**：`BM_Text_AnalyzeNgramCjk` 25.1→22.5µs（**-10%**，3 次重复 mean）；
    CjkHalfwidth -4%；`BM_P7_AnalyzeWithOffsets_Mixed1K` 10.5→9.55µs（-9%）；Mixed 持平。
  - **修正原估**：`BM_Text_ToCodepointsCjk`=0.86µs vs `NfkcFoldCjk`=5.9µs（同 1K 文本）
    ——被消的第二遍解码本身只占 analyze 总成本 ~3%，fold 主成本在 **inert 表查询**，
    analyze 大头在 tokenize+hash 聚合。原分析「双重解码=主瓶颈」高估；-10% 已含
    tf-only 的分配削减。CJK 分词若要再上量级需动 inert 查询（bitmap/SIMD 化）或
    n-gram 聚合结构，另立任务评估。
- [x] **S29-9 IndexPool reorder buffer：map → 环形缓冲 + 分配出锁** —— 已完成（2026-07-10）·
    `include/bitcask/thread_pool.hpp`
  - **做了**：`IndexLane::pending`（std::map，每任务锁内 rb-tree 节点分配）→ 环形滑动
    窗口 `vector<unique_ptr<ReorderEntry>>`：容量 2 的幂 → slot = `ord & (cap-1)`（窗口
    ≤ 容量 ⇒ 唯一，免 head 指针）；entry 由 map worker **锁外** `make_unique`，锁内 O(1)
    挂指针；reducer `ring_take_front` 单次槽位取出（原 count+extract 两次查找），锁下
    推进 ring_base 与 reducer 私有 next_apply_ord 配对。ord < base / 重复 ord 守卫：
    debug 断言 + release 丢弃并补偿 in_flight（防 flush 悬挂）。窗口上界 ≈ queue 容量 +
    reorder 在途上限（背压封顶）。
  - **实测**：`BM_IndexPool_MapSpeedup/8` 41.2→31.6ms（485k→632k/s，**+30%，4 线程
    平台突破**）；`MultiLibThroughput/1..8` +14~28%；SubmitDrain 116→110ms。
  - **验收**：build-rel + build-clang 双树 555/555；TSan 树相关并发子集 65/65。
  - **二期仍开**：per-lane 锁或 SPSC 回填队列（reorder_mu_ 仍全 lane 共享）。
- [ ] **S29-T（新发现，与本批无关）TSan 树既存失败**：`IndexPoolMultiLib.
    ThreadCountIndependentOfLibCount` 在 build-tsan 稳定失败（`after_first-before`=3≠2，
    /proc/self/task 线程计数在 TSan 运行时下多 1）——**git stash 后同样失败，非 S29 引入**。
    build-clang/build-rel 均绿。疑 TSan 运行时线程计入。低优先：加 TSan 下跳过或放宽断言。
- [x] **S29-10 CRC32 流式小块：16-63B CLMUL 内核** —— 已完成（2026-07-10）·
    `include/bitcask/hw_crc32.hpp` + `bench/crc32_bench.cpp`
  - 病根两层：① PCLMUL 大内核 `len >= 64` 下限（Step 1 无条件读 4×16B）；② 头对齐步骤
    会把 16-63B 小块拆碎到全 bytewise（而 `_mm_loadu_si128` 本不要求对齐）。
    16B 增量流（hint/WAL 帧）恒落字节表 → 500 MiB/s。
  - **做了**：新增 `crc32_pclmul_small`（16..63B，loadu 免对齐；首块 XOR seed +
    逐块 (k3,k4) 单折叠 + 与大内核完全一致的 Step 5-7 收尾）；`crc32_update`
    的 <64 分支与「对齐消耗后 48..63B body」分支接入。探测缓存原本已是 static（免改）。
  - **实测**：`BM_Crc32_Hw_Streaming/256` 450→71.8ns（544 MiB/s → **3.33 GiB/s，6.3×**）；
    /4096、/65536 各 **4.9×**（→2.39 GiB/s）；附带 `BM_Crc32_Hw/23` 3×（16B 块过内核）。
    大块无回归（4096/65536 持平）。剩余与一次性 32 GiB/s 的差距 = 每调用固定开销 +
    16B 粒度无跨块 ILP（增量语义固有）。
  - **验证**：独立对拍（长度 0-300 × 偏移 0-16 × 3 seeds = 15300 点 + 200 随机流式
    trial）全过；bench 自测补「16..63 × 非零 seed × 非对齐」定向覆盖（原自测只有
    seed=0 + 对齐起点）；双树构建 + ctest 555/555。
- [ ] **S29-11（待评估）HNSW 深层优化** · `src/vector/hnsw.cpp` + `include/bitcask/hnsw.hpp:69-70`
  - 超线性根因：f32 导航工作集 100k×384d×4B ≈ 153MB 打穿 L3（本机无 VNNI，
    `pick_int8_dot_kernel()` 返 nullptr → 建图全程 f32）。
  - 梯队：① ef_construction 200→128 / M 16→12（配置级，建图 1.5-2×，召回略降，需召回
    评估）；② 建图导航 int8 粗筛（153MB→38MB，需 AVX2 int8 内核，L0 精算保精度）；
    ③ 并行插入（6 核全闲置，收益最大但要打破 V3.3 单写者协议 `writer_active_`
    （`hnsw.cpp:845-854`），参考 hnswlib per-node lock，工程量最大）；④ devirtualize
    `dist_` 函数指针（几 %）。
  - 已到位勿动：visited list 版本化复用、邻接 bump-arena、优先队列缓冲池、导航 prefetch。

### 已核实「无需改动」（本轮四路分析结论,避免重复审计）

- `bm25_kernels.hpp`：AVX-512/AVX2/scalar 三层派发 + FMA 链，已优。
- `intersect.cpp`：预 resize + 裸指针游标 + Inoue 块过滤 + SIMD，已优。
- `score_bow_topk`/`search_wand`（inverted.cpp:92,553）：thread_local 工作数组池、块上界
  预算、惰性 dls——**稳态零分配**，BOW 不扩展与分配无关（纯桶锁争用）。
- `snapshot_flat`（inverted.cpp:253）：SoA 后已是 memcpy 式 assign + 容量复用。
- 生产查询已用批量 `fill_is_live/fill_doc_lens`（每列 2 次锁）；
  `BM_Inverted_SearchLockedScalar` 3132us vs Batch 169us（18.5×）是护栏基准，
  量化「per-element 锁 ≈ 15ns/次」——任何新代码禁止逐元素加锁。
- keydir 分片锁、`alloc_ord` relaxed 原子：非写路径瓶颈（瓶颈在 `write_mu_`）。
- `vbyte.hpp`：编码无分配；`gap_decode` 在载入期非热路径。

### 建议执行顺序

1. ~~S29-1..5（快赢批）~~ **已完成 2026-07-10**（555/555 全绿，HNSW 建图 -10%）
2. ~~S29-9 + S29-8~~ **已完成 2026-07-10**（MapSpeedup/8 +30% 平台突破、MultiLib +14~28%、
   AnalyzeNgramCjk -10%；TSan 并发子集 65/65。注意 S29-8 修正原估：双重解码非主瓶颈，
   CJK 再提速需动 inert 查询/聚合结构。第七梯队 P3 的 `nfkc_fold` API 关联仍在）
3. ~~S29-6 全四相~~ **已完成 2026-07-10**（SeqShardTable + epoch-RCU；Get 8 线程 +43% 平坦扩展）
4. ~~S29-7 铺垫+主体~~ **已完成 2026-07-10**（flat-combining 组提交；Put 并发 2-2.3×,聚合 976k/s）
5. ~~S29-10~~ **已完成**；S29-11 按需（HNSW 召回预算）
6. **下一步候选**：倒排桶锁二期（复用 SeqShardTable + epoch 注册表,BOW 查询扩展性)、
   S27-3 B2b/D/E、S27-4 DWPT、S29-11、S29-T（TSan 既存失败,低优先）

**与 S27 的关系**：S29 结构性各项与 S27-3 剩余（B2b/D/E recovery 重写）独立可并行；
S27-4（DWPT 并行 builder）与 S29-9（reorder 环形缓冲/分片锁）同属索引吞吐轴,
建议 S29-9 先行（小)、S27-4 收大头。
