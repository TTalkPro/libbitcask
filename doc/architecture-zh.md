# 架构概览

> 从 README 拆出。[← 返回 README](../README.md)


本文是分层与关键设计点的总览；逐模块的文件清单、依赖图、并发模型与 CMake target 表见 [`cpp-arch.md`](cpp-arch.md)。

```
┌─────────────────────────────────────────────────────────────────────┐
│  C API（c_api/bitcask_kv.{h,cpp} + bitcask_text + bitcask_vec +     │
│        bitcask_c.h 聚合头 + internal.h 共享助手）                  │
│  libbitcask.so：extern "C" 不透明句柄 + slice + fault（PIMPL）      │
└────────────────────────────┬────────────────────────────────────────┘
                               │ PIMPL：持 bitcask::Cask
┌────────────────────────────▼────────────────────────────────────────┐
│  Cask（KV + 搜索门面）                                              │
│  include/bitcask/cask.hpp（Cask / CaskOptions / CaskFault /          │
│  CaskIter / GetResult[View] / DocInput / StatusInfo / …）           │
│  src/cask/{cask,cask_iter,cask_search,cask_recovery}.cpp + meta_file│
│                                                                     │
│  ├─ IndexAuthority（ord/file-id/fstats 账本 + 写序闸门）            │
│  ├─ DataFile 缓存（pread 句柄 + 近似 LRU 淘汰）                    │
│  ├─ OkiState（有序 key 索引：memdelta + BCOK run + BCOM manifest）  │
│  ├─ DocMap（Index：ord↔ext/live/meta 宿主服务；查询面 DocTable）   │
│  ├─ TextPlugin "bm25"（倒排/Analyzer/缓存/高亮/bm25.ckpt 文件族）  │
│  ├─ VectorPlugin "hnsw"（VectorEnginePlugin 契约：HNSW/IVF-RaBitQ/DiskANN │
│  │   三引擎按 meta.vector_engine 选定；归一化/vec.ckpt 族 + .vec/.qc8 侧车）│
│  ├─ HybridSearcher（RRF 融合器；持两插件引用）                     │
│  ├─ CaskPluginHost（read_at / run_serialized / log 窄反向接口）    │
│  ├─ MetaConfig（bitcask.meta v5：magic + version + CRC32 + 纪元门禁）│
│  └─ IndexPool（异步索引 MapReduce，借自 IndexRegistry）             │
└────────────────────────────┬────────────────────────────────────────┘
                               │
┌────────────────────────────▼────────────────────────────────────────┐
│  查询门面（推荐新代码使用）                                        │
│  include/bitcask/searcher.hpp                                      │
│  text::Searcher / vec::Searcher / search::CaskHybridSearcher       │
│  （每次查询先 drain_plugins() 读屏障 → 直调插件内核）              │
└────────────────────────────┬────────────────────────────────────────┘
                               │
┌────────────────────────────▼────────────────────────────────────────┐
│  基础实现层                                                         │
│  fileops        codec / data_file / scanner / migrate               │
│  io / lock      PosixFile / FileLock（O_EXCL 仲裁）                 │
│  merge          纯函数策略 + Merger（CAS 重定位 + 插件广播）       │
│  bm25           InvertedIndex + WAND + k-way 交集 + 查询 AST        │
│  text           Analyzer（Ngram / Jieba / Whitespace / Stemming）   │
│  vector         HnswIndex（HNSW 图 + int8 量化 + mmap payload）    │
│  util           CRC32 / 高亮 / 同义词 / 模糊匹配 / Myers / 缓存    │
└─────────────────────────────────────────────────────────────────────┘
```

关键设计要点：

- **持久真相与派生权威**：data 文件是唯一持久真相；健康目录下 OKI 是查找/提交权威。BM25/HNSW 仍为 per-component base + delta ckpt + manifest 的派生缓存，校验失败回退全量 fold。详见 [`recovery-unified-checkpoint-design-zh.md`](recovery-unified-checkpoint-design-zh.md)。
- **双锁模型**：`bitcask.write.lock`（writer）与 `bitcask.merge.lock`（merger）独立，周期 merge 与 live writer 并行不互斥。merger 通过读取 `write.lock` 内容排除 live writer 的活动文件。
- **异步索引 MapReduce**：`put_doc` 入队有界 `IndexPool`（满则 push 阻塞做背压）→ N 个 map worker 并行分词（`hardware_concurrency` 真数据并行）→ per-lane reorder buffer（按 ord 排序）→ 单 reducer 串行 apply（库内单写者）。池由 `IndexRegistry` 共享，线程数 = N+1 与库数无关；6.6.0 起可经 `bitcask_set_thread_limits`（进程级，须在首个 search 库 open 前）同时封顶索引 worker 数与批量查询 arena 槽数。详见 [`docs/design/async-index-pipeline.md`](../docs/design/async-index-pipeline.md)。
- **查询并发**：批量查询接口（`search_*_batch`）在进程级共享 `search_arena`（TBB `task_arena`）上 inter-query 并行；单查询内部仍串行（WAND 顺序依赖、HNSW 图遍历）。
- **向量双引擎**（S32）：`vector_engine` 建库时一次性选定并持久化进 `bitcask.meta`——`hnsw`（内存图，≤数 M 向量）/ `ivfrq`（IVF-RaBitQ 磁盘段，10M-100M 推荐）/ `diskann`（Vamana 图，实验性）。引擎不符重开 → `kModeMismatch`；离线切换用 `vec_engine_migrate`（只改 meta，首次 open 全量 fold 重建，可回滚）。详见 [`vector-dual-engine-selection-zh.md`](vector-dual-engine-selection-zh.md)。
- **小端 only**：所有多字节整数小端（LE 主机原生零转换）；不再与 legacy 大端 Erlang 字节互通，迁移用 `bitcask_migrate be2le`（旧名 `migrate_le`，见 [`migrate-le.md`](migrate-le.md)）。
- **64 位时间戳（5.0 flag-day）**：`tstamp` / `expiry_at` 全链路扩为 `uint64_t`（C/C++ API + 盘上 record header 23B→27B + DocValue v4 + hint v4 + keydir 快照 v3 + docmap sidecar v3）。`bitcask.meta` v4 门禁**干净拒开** u32 纪元旧库（v1/v2/v3），提示 `rebuild — re-ingest data`；u32 纪元库可经 `bitcask_migrate tstamp64` **非破坏性离线迁移**到当前纪元（无需 re-ingest）。过期判定（`tstamp + expiry_secs`、`expiry_at` 与 `now` 比较）一律 u64 域算术，杜绝 u32 wrap 误判（极端 `expiry_secs ≈ 2^32` 此前会把全部 key 误判为已过期）。SONAME `libbitcask.so.4 → .so.5`，旧二进制由链接器层隔离。
- **插件化**：`CaskPlugin` 接口（`plugin_api.hpp`）是 KV 存储层与索引层的唯一契约，Cask 在写/恢复/merge/checkpoint 四条通路上向注册的插件广播事件。Text/Vector 是当前两个内建插件；新增插件（TTL、metrics、CDC）只需实现此接口。详见 [`plugin-arch-split-design-zh.md`](plugin-arch-split-design-zh.md)。


## 目录结构

```
.
├── include/bitcask/   # 公共头文件（API 接口）
├── c_api/             # libbitcask.so 的 C ABI（bitcask_kv / text / vec + 聚合 bitcask_c.h）
├── src/               # 实现：fileops / io / lock / cask / index / merge /
│                      #       cask / search / bm25 / text / vector
├── tests/             # GoogleTest 单元 + 集成测试（77 个测试二进制，含 C API smoke）
├── bench/             # Google Benchmark（keydir / cask / inverted / hnsw …）
├── tools/             # bitcask_migrate（统一）、migrate_le（旧）、vec_engine_migrate、gen_inert_table
├── cmake/             # BitcaskSanitizers 模块 + tsan.supp
├── designs/           # 设计稿（OKI 分级归并、原子批读可见性、meta 多值、掉电故障注入、索引管线去顺序化）
├── third_party/       # 第三方依赖（git submodule，见 build-zh.md）
├── doc/               # 架构 / 格式 / API 参考 / 设计文档
└── docs/design/       # 设计稿（线程安全审计、异步索引流水线）
```

