# 文档索引

> 从 README 拆出。[← 返回 README](../README.md)

文档分三处放：`doc/`（参考与设计稿）、`designs/`（较新的专题设计稿）、`docs/design/`（早期设计稿与审查纪要）。本页三处都列了。

## 总览与构建

| 文档 | 说明 |
|------|------|
| [`doc/features-zh.md`](features-zh.md) | 逐项能力与接口对照（`bitcask::Cask` 方法 → 说明 → 深入文档） |
| [`doc/architecture-zh.md`](architecture-zh.md) | 分层总览、关键设计要点、目录结构 |
| [`doc/build-zh.md`](build-zh.md) | 依赖、Windows / MSVC、sanitizer、ICU 来源、CMake 选项、产物与安装 |
| [`doc/text-encoding-zh.md`](text-encoding-zh.md) | 文本编码（内部只吃 UTF-8）、索引与 ICU / Unicode 版本的绑定 |

## 用户向参考

| 文档 | 说明 |
|------|------|
| [`doc/api-cpp.md`](api-cpp.md) | C++ API 参考（`bitcask::Cask` / `CaskOptions` / 全部方法） |
| [`doc/api-c.md`](api-c.md) | C API 参考 / FFI 绑定（不透明句柄 + slice + fault） |
| [`doc/cpp-arch.md`](cpp-arch.md) | C++ 代码库架构与 CMake target 表 |
| [`doc/format-zh.md`](format-zh.md) | 字节级磁盘格式真源（record / hint / DocValue / ckpt / meta） |
| [`doc/concurrency-zh.md`](concurrency-zh.md) | 并发契约用户向说明（锁层、不变量、可见性） |
| [`doc/multikey-txn-zh.md`](multikey-txn-zh.md) | **多键事务模式原理**（意图日志 + 前滚重放，补齐 A/D；不提供 I 与 CAS）——库内 `TxnCask` / `bitcask_txn_*` 落地 |
| [`doc/atomic-batch-design-zh.md`](atomic-batch-design-zh.md) | **S35 引擎原子批设计定稿**（kBatchHeader 区间提交判定、meta v6 懒升级、`put_batch_atomic`）——TxnCask 现行提交路径 |
| [`doc/multikey-txn-impl-design-zh.md`](multikey-txn-impl-design-zh.md) | S34 方案 B 设计存档（意图 blob 编码、单调 txn key——提交路径已被 S35 取代，recover 兼容仍在用） |
| [`doc/pg-xid-mvcc-zh.md`](pg-xid-mvcc-zh.md) | 参考笔记：PostgreSQL xmin/xmax 与 XID 回收——论证本库事务方案无需 XID 式回收 |
| [`doc/keydir-disk-resident-design-zh.md`](keydir-disk-resident-design-zh.md) | **OKI Level B 磁盘驻留设计稿**（S36 起点用 BCOK v2 表述；当前格式已演进到 BCOK v3 / BCOM v4） |
| [`designs/oki-size-tiered-compaction-design-zh.md`](../designs/oki-size-tiered-compaction-design-zh.md) | **S44 OKI 分级后缀归并设计**（后台归并、读视图不可变发布、盘上格式零改动） |
| [`docs/design/s44-compaction-baseline-zh.md`](../docs/design/s44-compaction-baseline-zh.md) | S44 写放大 / 读路径基准与验收报告 |
| [`doc/migrate-le.md`](migrate-le.md) | 大端 → 小端目录离线迁移工具（`migrate_le`） |

## 设计文档

| 文档 | 说明 |
|------|------|
| [`docs/design/thread-safety.md`](../docs/design/thread-safety.md) | 内部线程安全审计（as-built 状态记录） |
| [`docs/design/async-index-pipeline.md`](../docs/design/async-index-pipeline.md) | 异步索引 MapReduce 流水线设计稿（S6） |
| [`docs/design/s13-review-2026-07-02.md`](../docs/design/s13-review-2026-07-02.md) | S12/S13 批次审查纪要 |
| [`docs/design/s27-3-b2b-recovery-design.md`](../docs/design/s27-3-b2b-recovery-design.md) | S27-3 B2B 恢复设计 |
| [`docs/design/s27-4-dwpt-design.md`](../docs/design/s27-4-dwpt-design.md) | S27-4 DWPT 并行 builder 设计 |
| [`docs/design/s29-6-keydir-lockfree-read.md`](../docs/design/s29-6-keydir-lockfree-read.md) | S29-6 KeyDir 无锁读设计 |
| [`docs/design/s29-6b-inverted-term-cache.md`](../docs/design/s29-6b-inverted-term-cache.md) | S29-6B 倒排 term 缓存设计 |
| [`docs/design/s30-mmap-segments.md`](../docs/design/s30-mmap-segments.md) | S30 mmap 段设计 |
| [`docs/design/s40-key-single-instance.md`](../docs/design/s40-key-single-instance.md) | 检索 key 内存单实例化（四份驻留 → 两份）+ 开库去 key 字符串分配 |
| [`docs/design/s41-fields-read-back-design-zh.md`](../docs/design/s41-fields-read-back-design-zh.md) | fields 读回设计 |
| [`docs/design/s42-docmap-live-store-design-zh.md`](../docs/design/s42-docmap-live-store-design-zh.md) | DocMap Live-Only Store 设计 |
| [`docs/design/s42-rollout-bench-report-zh.md`](../docs/design/s42-rollout-bench-report-zh.md) | S42 Rollout 基准报告 |
| [`docs/design/s43-okionly-baseline-zh.md`](../docs/design/s43-okionly-baseline-zh.md) | S43-0 基线、语义契约与故障注入底座 |
| [`docs/design/s43-okionly-rollout-report-zh.md`](../docs/design/s43-okionly-rollout-report-zh.md) | S43 OKI-only Rollout 报告 |
| [`docs/design/s45-1-research-zh.md`](../docs/design/s45-1-research-zh.md) | 调研记录：索引管线全局顺序假设盘点 |
| [`docs/design/s45-6-bench-zh.md`](../docs/design/s45-6-bench-zh.md) | 基准报告：索引管线去全局顺序化 |
| [`docs/design/analyzer-fingerprint-meta-tail.md`](../docs/design/analyzer-fingerprint-meta-tail.md) | analyzer 配置指纹（`bitcask.meta` 可选尾段）设计 |
| [`docs/design/bug-doctext-ignored-multifield.md`](../docs/design/bug-doctext-ignored-multifield.md) | Bug 记录：多字段模式下 `doc.text` 未被索引为默认字段 |
| [`designs/index-pipeline-unordered-design-zh.md`](../designs/index-pipeline-unordered-design-zh.md) | 索引管线去全局顺序化（key + LSN 裁决、分片并发写、去 reducer）设计 |
| [`designs/atomic-batch-read-visibility-design-zh.md`](../designs/atomic-batch-read-visibility-design-zh.md) | 原子批对并发读者整批可见的设计 |
| [`designs/meta-multivalue-design-zh.md`](../designs/meta-multivalue-design-zh.md) | meta 多值（字符串数组）设计 |
| [`designs/power-loss-fault-injection-design-zh.md`](../designs/power-loss-fault-injection-design-zh.md) | 掉电模型的故障注入测试设计 |
| [`doc/windows-port-design-zh.md`](windows-port-design-zh.md) | Windows 移植设计（MSVC 原生 · x64 · 保 SIMD） |

## HNSW / 向量

| 文档 | 说明 |
|------|------|
| [`doc/vector-db-design-zh.md`](vector-db-design-zh.md) | 向量库设计：在 Bitcask 上原生扩展 |
| [`doc/vector-db-ann-landscape-zh.md`](vector-db-ann-landscape-zh.md) | ANN 算法全景与 HNSW 定位 |
| [`doc/vector-dual-engine-selection-zh.md`](vector-dual-engine-selection-zh.md) | S32 向量双引擎选择（HNSW / IVF-RaBitQ / DiskANN 定位与切换） |
| [`doc/hnsw-overview-zh.md`](hnsw-overview-zh.md) | HNSW 算法全景与本实现优化点 |
| [`doc/hnsw-design-zh.md`](hnsw-design-zh.md) | HNSW V3 设计定稿（并发协议、持久化） |
| [`doc/hnsw-graph-theory-zh.md`](hnsw-graph-theory-zh.md) | HNSW 多层图原理与量化必要性 |
| [`doc/hnsw-graph-construction-zh.md`](hnsw-graph-construction-zh.md) | 邻接表怎么"算"出来的（建图算法） |
| [`doc/hnsw-lifecycle-zh.md`](hnsw-lifecycle-zh.md) | HNSW 图生命周期：构建、持久化、恢复 |
| [`doc/hnsw-memory-footprint-zh.md`](hnsw-memory-footprint-zh.md) | HNSW 内存占用分析（按 V7 重新推导） |
| [`doc/hnsw-int8-only-design-zh.md`](hnsw-int8-only-design-zh.md) | P5 int8-only 内存模式设计 |
| [`doc/int8-vnni-v4-zh.md`](int8-vnni-v4-zh.md) | int8 量化 + AVX-VNNI 距离内核 |
| [`doc/simd-vnni-internals-zh.md`](simd-vnni-internals-zh.md) | VNNI dpbusd 与偏置补偿指令内幕 |
| [`doc/vector-ondisk-quant-design-zh.md`](vector-ondisk-quant-design-zh.md) | 向量落盘 int8 量化设计 |
| [`doc/s29-11-hnsw-deep-opt-design-zh.md`](s29-11-hnsw-deep-opt-design-zh.md) | S29-11 HNSW 深度优化（AVX2 int8 内核 + 混合精度建图导航） |
| [`doc/rabitq-theory-zh.md`](rabitq-theory-zh.md) | RaBitQ 量化理论（无偏估计、误差界、lite→full 升级路径） |

## BM25 / 倒排

| 文档 | 说明 |
|------|------|
| [`doc/posting-zero-copy-design-zh.md`](posting-zero-copy-design-zh.md) | PostingList 数据布局与零拷贝 |
| [`doc/inoue-simd-intersection-zh.md`](inoue-simd-intersection-zh.md) | Inoue 块过滤 + SIMD 精确匹配 |
| [`doc/intersect-kernel-internals-zh.md`](intersect-kernel-internals-zh.md) | 旋转法交集内核内幕与输出段微优化 |
| [`doc/kway-blockmax-bmw-zh.md`](kway-blockmax-bmw-zh.md) | k-way 交集 + 块级元数据 + BMW |
| [`doc/wand-blockmax-zh.md`](wand-blockmax-zh.md) | WAND / BlockMax-WAND top-k 动态剪枝 |
| [`doc/segment-index-design-zh.md`](segment-index-design-zh.md) | 封口段格式设计（v2 mmap 零驻留 / v1 全量回退） |
| [`doc/roaring-meta-bitmap-design-zh.md`](roaring-meta-bitmap-design-zh.md) | Roaring bitmap 加速 meta 过滤设计 |

## Index / 恢复 / Merge / 插件

| 文档 | 说明 |
|------|------|
| [`doc/keydir-sharding-design-zh.md`](keydir-sharding-design-zh.md) | 6.x KeyDir 分片与 MVCC fold 设计存档（S43 已由 OKI-only 权威取代） |
| [`doc/ordered-key-index-design-zh.md`](ordered-key-index-design-zh.md) | 有序 Key 索引（OKI）：WiscKey 式 range 扫描 + keydir 内存天花板路线 |
| [`docs/design/oki-only-keydir-analysis-zh.md`](../docs/design/oki-only-keydir-analysis-zh.md) | OKI-only keydir 架构改造深度分析 |
| [`docs/design/oki-run-open-verify.md`](../docs/design/oki-run-open-verify.md) | 决策稿：OKI run 开库校验（惰性 / 分块） |
| [`doc/scan-batch-view-design-zh.md`](scan-batch-view-design-zh.md) | 扫描 / range 批视图接口（免分配）设计（扫描半边 13.0.0 已发；range 半边未发版） |
| [`doc/getresult-view-design-zh.md`](getresult-view-design-zh.md) | `GetResultView` 零拷贝设计 |
| [`doc/recovery-unified-checkpoint-design-zh.md`](recovery-unified-checkpoint-design-zh.md) | 恢复持久化：分段 ckpt + manifest commit |
| [`doc/merge-policy-zh.md`](merge-policy-zh.md) | Merge 触发策略（碎片率 / 死字节 / 过期阈值） |
| [`doc/ord-recycling-design-zh.md`](ord-recycling-design-zh.md) | ord 回收复用可行性与方案 |
| [`doc/plugin-arch-split-design-zh.md`](plugin-arch-split-design-zh.md) | 插件化架构（SearchLayer 拆分后） |
| [`doc/read-handle-lru-design-zh.md`](read-handle-lru-design-zh.md) | ReadHandle LRU 缓存设计 |
| [`doc/sealed-mmap-read-design-zh.md`](sealed-mmap-read-design-zh.md) | Sealed DataFile mmap 只读路径 |
| [`doc/put-flow-zh.md`](put-flow-zh.md) | `put(K, V)` 完整调用链（C API → 字节落定 → 索引入队） |
| [`doc/wal-batch-design-zh.md`](wal-batch-design-zh.md) | 批量 flush 设计（当前代码版） |

