# 更新日志（Changelog）

本文件记录 libbitcask 的所有重要变更。

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)；
版本遵循语义化版本（库 `SOVERSION=1`，盘上 meta 格式版本 `v2`）。

---

## [Unreleased]

### 新增（Added）
- **S11 线程安全化（通用 C++ 库定位）**——同一个 `Cask` handle 可被多线程安全共享，
  对标 RocksDB / LMDB 常规契约。设计稿 [`docs/design/thread-safety.md`](docs/design/thread-safety.md)。
  - **W1 写路径内部串行化**：`put` / `remove` / `put_doc` / `sync` / `close_write_file`
    由内部 `std::mutex write_mu_` 串行化——把「调用方串行化」内化为「库内互斥」，
    同一 handle 多线程并发写安全（写在文件层本就串行 → 锁不损吞吐；更高写并发
    → 按目录分片多实例）。读路径不取 `write_mu_`，吞吐不变。
  - **W2 读/搜索并发确认**：搜索方法（text / phrase / bool / fields / near / fuzzy /
    wildcard）全部确认为并发读安全，订正历史「否（保守）」注释。
  - **W3 `close()` fail-fast 生命周期硬化**：加 `std::atomic<bool> closed_`，
    `close()` 后**新发起**的公共调用返回 `kInvalidOption` 而非解引用已释放状态（UB）；
    `close()` 幂等（二次 no-op）。best-effort 防误用，非完整 rundown。
  - **W4 `parallel_scan` 并行全表扫描 API**：单次快照 live key → 分 N 段 → 并发 `get`
    读值 + 回调；用于 analytics / export / reindex。被并行化的是读值的 pread + decode。
- **S7 批量检索 API**：`search_text_batch` / `search_vector_batch` / `search_hybrid_batch`
  ——多条独立查询在进程级共享 `search_arena`（TBB `task_arena`）上 inter-query 并行，
  保序返回各结果；单查询内部仍串行（WAND 顺序依赖、HNSW 图遍历）。
- **S6 异步索引 MapReduce 流水线**：`put_doc` 入队有界 `IndexPool`（满则背压）
  → N 个 map worker 并行分词（`hardware_concurrency` 真数据并行）→ per-lane reorder
  buffer（按 ord 排序）→ 单 reducer 串行 apply。池由 `KeyDirRegistry` 共享，线程数
  = N+1 与库数无关——多 `Cask` 实例共享同一组索引线程。

### 变更（Changed）
- **性能优化（多梯队，均经实测验证）**：
  - **搜索缓存前置检查**（S10-A1）：命中即跳过 ~2µs NLP analyze。
  - **put_doc 字段打包进单 buffer**（S10-A5）：alloc/put −6.5，heap bytes −13%。
  - **ord_field_lens_ 字段名 intern 化**（S10-A4）：内存 −40%，吞吐 <1% 影响。
  - **短语并行 + HNSW int8 精排并行**（S7-5/6）：3.5–4.65× / 1.12×。
  - **SynonymMap `shared_ptr`**（C-tier）：热路径 45×；highlighter 二分搜索。
  - **HNSW output reserve + madvise RANDOM + select_neighbors 缓存**（D-tier）。
  - **merger 批量 pwrite**（S2）；**checkpoint/keydir 序列化精确 reserve**（S4）；
    **recovery 批量并行 analyze**（S3）；**多文件并行 fold**（R3）。
  - **HintFile::fold chunked pread**：syscall ↓4348×；**hint flush 1MiB**（P2）。
  - **NgramAnalyzer 内部 `string_view` 去重**：alloc O(N)→O(U)。
- **重构（行为零变更）**：
  - **S9**：RAII fd 管理 + `kDefaultField` 透明查找 + `byte_order` 提取（P0）；
    C API `unique_ptr` + vbyte 模板化 + `ThreadLocalBuffer`（P1）；
    `SearchError` 强类型枚举 + deserialize 哨兵具名（P2）。
  - **S8**：批量 / 单条 search 方法去重（R1/R3）+ `thread_pool.hpp` 注释收尾（R2/R5）
    + 池魔法数字 → 具名常量（R4）。
  - **D2**：`search_*` 物化抽 `materialize_hits` + 词序还原抽 `ordered_query_terms`（8 处去重）。

### 修复（Fixed）
- **X1**：`CaskIter` pin `KeyDir` 的 `shared_ptr`（`keydir_pin_`），防 `close()` 后
  iterator 解引用已释放 keydir 的 use-after-free。

---

## [1.1.0] - 2026-06-22

### 新增（Added）
- **HNSW 向量外存化（V7 / BVH2 v2）**：全精度 f32 向量改存独立的 `search.vec`
  文件（`BCVP`，只读 mmap + 每 4KB 页 CRC32）；`search.ckpt` 的 hnsw 段
  （magic `BVH2`，version 2）内嵌 int8 量化码字，省去开库时的重量化 pass。
- **统一分段搜索 checkpoint `search.ckpt`（`BCSC` 容器）**：docmap / bm25.default /
  bm25.fields / hnsw 各为一段、**逐段独立 CRC** + 页脚目录 + `search.ckpt.prev`
  代际回退；取代旧的多文件方案（`search.docmap.ckpt` / `search.vec.ckpt` /
  `search.bm25.*`）。恢复改为单 watermark 自门 + 全段 CRC。
- **倒排盘上格式 v6（`InvVersion=6`）**：ord 改用 FOR（Frame-of-Reference）块压缩
  （128/块），tf / dl 改用 VByte varint 整组编码（不再支持 v1–v5 载入）。
- **CI**：GitHub Actions matrix（Release + ASan/UBSan/TSan）。
- **崩溃恢复回归测试**：`fork + SIGKILL` 写入中崩溃恢复；`MergeFailurePreservesKeyDirVisibility`
  合并失败时 keydir 可见性测试。

### 变更（Changed）
- **性能优化（三梯队，均经实测验证的安全微优化）**：
  - 第一梯队：HNSW rerank、WAND 结果排序、qcodes 条件分配、FStats 缓存行对齐。
  - 第二梯队：KeyDir 换 `ankerl::unordered_dense` 稠密扁平表、HNSW 邻接 bump-slab arena。
  - 第三梯队：`thread_local` scratch/encode 缓冲复用、serialize 缓冲复用、
    hint `pread_into`、向量软件预取、`-march=native` 开关。
- **KeyDir**：分片数演进至 256，分片锁由 `shared_mutex` 改为 `std::mutex`（消写者偏好停车）；
  fstats 改无锁发布路径。
- **文档**：全面对齐 C API 与小端盘上格式，移除遗留 Erlang/NIF 引用；
  与代码现状逐项核对（格式 / 并发锁序 / 恢复 / merge / HNSW / 倒排算法）。

### 修复（Fixed）
- **生产正确性（C1–C5）**：
  - **C1**：merge 失败时 keydir **完全未动**（延后 apply）→ 失败后数据立即可见、无需重启恢复。
  - **C2**：merger 全 9 条错误路径补 cleanup（部分输出文件不残留）。
  - **C3**：IndexPool worker 整体 `try/catch` 吞异常（best-effort 丢弃 + `index_errors` 计数），
    异常不再杀 worker、`pending_` 必递减 → `flush()` 不挂、索引不静默漂移。
  - **C4**：IndexPool 析构 UB 修复——`start()` 从未调用时 `joinable()` guard 跳过 join，
    `stop()` 幂等（CAS 短路）。
  - **C5**：`Cask::close()`（`noexcept`）整体包 `try/catch`，所有可抛操作
    （save_search_ckpt / write_keydir_snapshot / 分配 / 取锁）纳入兜底；
    catch 后的资源释放中唯一可抛的 `registry release` 也单独包 try → 彻底消除
    `noexcept` 函数抛出导致 `std::terminate` 的风险。
  - merge 输出无条件 fsync（成功返回 = 新文件已落盘）。

---

## [1.0.0] - 2026-06-19

首个发布版本——嵌入式存储引擎：在 Bitcask 追加日志 KV 之上集成 **BM25 全文检索**、
**HNSW 向量检索**与 **RRF 混合检索**，通过跨语言稳定 C ABI 暴露。

### 新增（Added）
- **KV 核心**：append-only data/hint 文件、内存分片 keydir、O(1) `get`/`put`/`remove`、
  单次 `pread` 读值、并发 merge（不阻塞 writer）、MVCC 迭代器（兄弟链 + pending 哈希快照）。
- **全文检索（BM25）**：按字段隔离的倒排索引、WAND / BlockMax-WAND top-k 动态剪枝、
  短语 / 近邻（`search_phrase` / `search_near`）、布尔检索（AND/OR/NOT）、
  多字段（`field:term^boost`）、模糊（Levenshtein / Myers 位并行）、通配符（`*?`）、
  同义词展开、命中高亮。
- **向量检索（HNSW）**：cosine（写入端归一化）/ dot / L2 度量、单写者 + 多读者无锁
  发布协议（`atomic<NodeChunk*>` + per-node 自旋锁）、可选 int8 量化。
- **混合检索**：BM25 + 向量 RRF 融合（`k=60`）。
- **分析器**：Ngram、Whitespace、Jieba（中文分词）、Porter 词干、NFKC 归一化。
- **C API**：不透明句柄、显式 `*_free` 配对、错误码 + `bitcask_fault_t` 详情、
  二进制安全切片；稳定 ABI（`SOVERSION=1`）。
- **盘上格式**：小端 only（meta `v2`）、DocValue v3 打包值、字段名↔id 注册表；
  `migrate_le` 旧大端目录 → 小端离线迁移工具。
- **构建**：C++23，无 Boost / abseil 依赖；第三方库以 git submodule vendored
  在 `third_party/`（构建无需联网）；CMake + sanitizer 支持。

### 说明（Notes）
- **字节序 flag-day**：旧大端目录（meta `v1`）在 open 时被干净拒绝，需用
  `migrate_le` 迁移或从源头重灌数据。
- 协议：[Apache License 2.0](LICENSE)。

[Unreleased]: https://github.com/davidalphafox/libbitcask/compare/v1.1.0...HEAD
[1.1.0]: https://github.com/davidalphafox/libbitcask/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/davidalphafox/libbitcask/releases/tag/v1.0.0
