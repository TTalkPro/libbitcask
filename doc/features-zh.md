# 功能一览

> 从 README 拆出。[← 返回 README](../README.md)

| 能力 | 接口（`bitcask::Cask`） | 说明 | 文档 |
|------|------------------------|------|------|
| KV 读写 | `put` / `get` / `remove` / `put_batch` | DocValue v4 编码（u64 tstamp），纯 KV 的 binary 走 text 段 | [`api-cpp.md`](api-cpp.md) |
| **原子批 / 多键事务** | `put_batch_atomic` / `TxnCask` | S35 引擎原子批：崩溃/掉电后整批 all-or-nothing；S63 起提交对并发读者一次性生效（原子可见，不是快照隔离）；支持批内 REMOVE / 条件 / check | [`atomic-batch-design-zh.md`](atomic-batch-design-zh.md) |
| 零拷贝读 | `get`（`GetResultView`）/ `get_owned` | view 借 `pread` 缓冲或 sealed mmap，无堆分配 | [`getresult-view-design-zh.md`](getresult-view-design-zh.md) |
| 结构化文档 | `put_doc`（`DocInput`） | text + 可选 meta + 可选 vector + 多字段 | [`api-cpp.md`](api-cpp.md) |
| 词袋检索 | `search_text` | BM25 + meta 过滤，支持 `offset` 分页 | [`api-cpp.md`](api-cpp.md) |
| 短语 / 近邻 | `search_phrase` / `search_near` | 词位置感知 | [`api-cpp.md`](api-cpp.md) |
| 布尔检索 | `bool_search` | AND / OR / NOT 查询语法 | [`api-cpp.md`](api-cpp.md) |
| 多字段 | `search_fields` | `field:term^boost` 语法 | [`api-cpp.md`](api-cpp.md) |
| 模糊 / 通配符 | `search_fuzzy` / `search_wildcard` | Levenshtein / `*?` 模式 | [`api-cpp.md`](api-cpp.md) |
| 高亮 | `search_text_highlight` | 命中片段截取 | [`api-cpp.md`](api-cpp.md) |
| 批量检索 | `search_text_batch` / `search_vector_batch` / `search_hybrid_batch` | 多条独立查询并发跑共享 Search 池（inter-query 并行），保序返回 | [`api-cpp.md`](api-cpp.md) |
| 向量 ANN | `search_vector` | 三引擎可选：`hnsw`（默认，内存档）/ `ivfrq`（IVF 磁盘段，10M-100M 推荐）/ `diskann`（Vamana 图，实验性）；HNSW 支持 int8 量化与 int8-only 内存 | [`hnsw-overview-zh.md`](hnsw-overview-zh.md) / [`vector-dual-engine-selection-zh.md`](vector-dual-engine-selection-zh.md) |
| 向量引擎迁移 | `vec_engine_migrate`（CLI） | 离线切换向量引擎（只改 meta，首次 open 全量 fold 重建，可回滚） | [`vector-dual-engine-selection-zh.md`](vector-dual-engine-selection-zh.md) |
| 混合检索 | `search_hybrid` | BM25 + 向量 RRF(60) 融合 | [`hybrid_searcher.hpp`](../include/bitcask/hybrid_searcher.hpp) |
| 同义词 | `CaskOptions::synonym_map`（open-time） | 查询时自动展开，不可变、并发安全 | [`synonym_map.hpp`](../include/bitcask/synonym_map.hpp) |
| 迭代 | `make_iter` | 创建时刻的 OKI 组合视图快照（runs + frozen + memdelta） | [`ordered-key-index-design-zh.md`](ordered-key-index-design-zh.md) |
| **有序 range 扫描** | `make_range_iter`（`RangeOptions{lo,hi,prefetch}`） | OKI 有序 key 索引：`[lo,hi)` 字典序遍历，**O(range)** 而非 O(全表)（实测 1/256 选择性 8.0 ms → 0.53 ms）；可选值预取 | [`ordered-key-index-design-zh.md`](ordered-key-index-design-zh.md) |
| **OKI 磁盘驻留 / Level B** | `CaskOptions::keydir_cache_entries`（ABI 字段名保留；`>0` 即声明） | 点查走 active memdelta + frozen 段 + BCOK v3 run 的组合视图（bloom + 块缓存），避免重建全量内存哈希索引 | [`keydir-disk-resident-design-zh.md`](keydir-disk-resident-design-zh.md) |
| **OKI 分级归并** | 默认后台策略（`CompactionMode::kTiered`） | 只归并最新连续后缀，降低写放大并解除索引重写量与总规模近似线性绑定；`kFull` 保留为 A/B / 回退策略 | [`designs/oki-size-tiered-compaction-design-zh.md`](../designs/oki-size-tiered-compaction-design-zh.md) |
| 并行扫描 | `parallel_scan` | 多线程全表扫描（快照 key → 分段并发 get；支持 key 前缀过滤） | [`api-cpp.md`](api-cpp.md) |
| 合并 | `merge` / `needs_merge` | 与读写并发的独立 `merge.lock` 模型 | [`merge-policy-zh.md`](merge-policy-zh.md) |
| 备份 | `backup` / 文件级拷贝 + `flush_index` | 一致切面复制 data 提交集与当前 OKI commit set；搜索/向量辅助缓存可按目录带快照 | [`wal-batch-design-zh.md`](wal-batch-design-zh.md) |
| 升级 | `Cask::upgrade` | 离线把 KV 目录升为索引模式 | [`api-cpp.md`](api-cpp.md) |
| 迁移 | `bitcask_migrate`（CLI） | 统一纪元迁移：`detect` / `be2le`（v1 大端）/ `tstamp64`（u32 → u64，5.0 flag-day）/ `hintord`（hint 补 ord，5.1 flag-day，data 零改动）；非破坏性 | [`migrate-le.md`](migrate-le.md) |
| 状态 | `status` / `read_handle_count` | 内省 key 数 / fd 预算 / 索引错误 / OKI run、归并与背压计数（7.0.0 C++ 布局扩展） | [`api-cpp.md`](api-cpp.md) |
| C ABI | `libbitcask.so` | `extern "C"` 不透明句柄 + slice + fault，跨 ABI 稳定 | [`api-c.md`](api-c.md) |
| **进程级线程数上限** | `bitcask_set_thread_limits`（C）/ `include/bitcask/thread_limits.hpp`（C++） | 封顶索引池 map worker 数与批量查询 arena 槽数（0 = 缺省 `hardware_concurrency`）；进程级共享两池，须在首个 search 库 open 前设置 | [`api-c.md`](api-c.md) |
| **开库调优** | `bitcask_open_ex2`（`bitcask_open_tuning_t`） | 首旋钮 `segment_verify_crc`（缺省 1；0 = BM25 段只验页脚 / 目录，省开库整读段目录的 I/O）；`struct_size` 前向兼容，以后尾部追加字段不开新入口 | [`api-c.md`](api-c.md) |


---

接口细节见 [`api-cpp.md`](api-cpp.md) / [`api-c.md`](api-c.md)；并发语义见 [README「线程安全模型摘要」](../README.md#线程安全模型摘要)。
