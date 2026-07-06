# 更新日志（Changelog）

本文件记录 libbitcask 的所有重要变更。

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)；
版本遵循语义化版本。**3.0.0 起三套版本号统一**（S12-7 后单一真源 =
`project(libbitcask VERSION ...)`）：CHANGELOG 发布版本 = 库 `VERSION` = C API 产品版本
`bitcask_version_*` = **`3.1.0`**；库 `SOVERSION` = **`3`**（= major，ABI 兼容不变）；
盘上格式版本独立于库版本：`bitcask.meta` = **`v3`**（含 CRC32），`field.schema` = **FSCH v1**。

---

## [Unreleased]

### Changed（S24：key 快照扁平化 + vocab 增量化 + shim 收缩首批，2026-07-06）

- **fold/scan key 快照扁平化**：keydir 迭代快照与 `drain_live_keys` 从
  逐 key 堆分配（百万 key = N 次 malloc + 32B/头）改单缓冲 + 偏移哨兵，
  每快照恒 2 次分配、峰值内存约减半（fold/merge/backup/parallel_scan
  同路径受益）。
- **ensure_vocab 增量化**：两层视图（基线 + 排序增量层），有新词后的
  首查询重建从全量 O(V) string 深拷降为 O(extra+delta)（阈值封顶，超阈
  一次 std::merge 摊还并入）；wildcard/fuzzy 两层独立二分/线性扫，结果
  集合不变。
- **测试 shim 收缩（实质收官）**：wildcard/fuzzy/highlighter/synonym
  20 例迁 TextPlugin 直连（断言逐条不变）；核实其余 4 个"锁定"实为公开
  配置类型误报。shim 锁定 9→**1**（自测 search_layer_test，作为 legacy
  ckpt 写端夹具留守至 legacy_ckpt 退役）。
- 验收：Debug 544/544、TSan 子集 93/93、Release(LTO) 绿。

### Changed（S23：查询 scratch 池 + 写路径 move + hint v3，2026-07-06）

- **BM25 查询稳态零分配**：search 标量/WAND/bool_search 的 per-term 快照
  scratch（3-7 vector/term）thread_local 池化。实测 BOW 吞吐 1 线程
  −24%、**16 线程 −81%**（分配器争用即多线程瓶颈）；热词查询 −15%。
- **写路径 doc_text move**：apply_job 双入口，生产流水线免每文档一次全文
  深拷。
- **hint 文件格式 v3**：18B 定宽/条 → 典型 8-9B（magic 文件头 + vbyte +
  offset gap 差分 + 8B trailer）。keydir 启动重建 hint I/O 近半；读端
  v2/v3 双分派，hint 可重建零迁移。golden 测试换代 + v2 兼容读锁定。
- 验收：Debug 544/544、TSan 251/251、Release bench 实测如上。

### Changed（S22：PostingList SoA + WAL 退役，2026-07-06）

- **PostingList AoS→SoA**（倒排索引最大常驻结构）：`Posting{ord,tf,dl,
  positions}` 40B/条 + 每条独立 positions 堆块 → 平行数组 `ords/tfs/dls`
  + 扁平 `pos_data/pos_off`（惰性物化）。无位置库 **16B/条（−60%）**、
  有位置库 24B/条 + 紧凑数据；千万 posting 级库常驻省数百 MB；CoW 克隆
  N+1 次堆分配 → 6 次。phrase/near 持针改 span。**落盘 v6/delta 格式
  字节零变化**（盘上本就列式）。Debug 537/537 + TSan 91/91 + bench 全绿。
- **Removed：inverted_wal（用户拍板退役）**——`enable_wal` 生产零调用，
  增量持久化由 S14-4 delta 链承担；删模块（465 行）+ 13 例测试 + 全部
  钩子。需要时以 git 历史为底重新接线。

### Changed（S21：结构 / 存储 / 内存三轴优化，2026-07-06）

- **内存（不引入额外分配器）**：
  - `DocSlot` 40→24B（`DocLoc` 重排消 padding 24→16B + 去 ord 死重字段，
    `get()` 改返回 `DocHit : DocSlot` 聚合继承，消费点无感）；每 chunk
    slots 数组 2.5MB→1.5MB。全部 `DocLoc{}` 构造点改 designated
    initializer（防字段重排下位置式初始化静默错位）。
  - `meta_blobs_` 惰性化：无 meta 部署零常驻（改前每 ord 24B 空 vector 头，
    千万 ord = 240MB）。
  - `VectorPlugin` delta 插入日志平行数组化（每向量写入 −1 次堆分配）；
    `SearchCache::invalidate_terms` 收 `span<string_view>`（每写/删一篇文档
    省全词集 owning 深拷）。
- **存储**：
  - `SearchCheckpoint::write` / `KeyDir::save_snapshot` rename 前补
    **fdatasync**——断电后 manifest 已提交而组件页丢失 → CRC 坏 → 退全量
    fold，checkpoint 启动加速失效；现为保证而非运气。
  - keydir 快照 **v2**（entries 块 vbyte，~38B→15-20B/条，快照 I/O 近半）；
    docmap 行编码 **v2**（BCIS v2 + 段型 `kDocmapDeltaV2` gap+vbyte，
    34B→12-15B/行）。读端全部双版本兼容；**v2 delta 文件头版本升 2**——
    旧二进制对未知段型静默忽略会造成数据洞，版本拒收使其安全退 fold。
    回归测试含手工构造的 v1 字节流（v1 读分支无自然覆盖）。
- **结构**：
  - **cask.cpp 物理拆分**（纯平移，类与 API 不变）：3441→2300 行，切出
    cask_iter.cpp（CaskIter）/ cask_search.cpp（搜索门面）/
    cask_recovery.cpp（升级迁移 + keydir/快照恢复）+ cask_internal.hpp
    （跨 TU 共用助手内部头）。逐字节纯度经脚本核验。
  - docmap_ckpt / legacy_ckpt 两处 `apply_delta_file` 的 CRC 预检 + hook
    收尾骨架去重为 `index::apply_delta_sections`（不变量收敛单点）。
  - data record header 注释漂移订正（14B→kHeaderSize=23，3 处）；
    search_checkpoint.hpp 补缺失 `<memory>`。
- 验收：Debug **550/550**（546 + 4 个新格式回归）+ TSan checkpoint/并发
  子集 **131/131** + Release(LTO) 构建绿；磁盘格式变更均为可重建文件
  （快照/checkpoint），零迁移成本。

### Changed（S19：插件化架构 P5 — 门面收编与 shim 退役，2026-07-04）

- **查询门面换代**：新增 `bitcask/searcher.hpp`——`text::Searcher` /
  `vec::Searcher` / `search::CaskHybridSearcher` 类型化查询门面（查询前
  自动 `Cask::drain_plugins()` 读屏障）；`Cask::search_*` 保留为兼容薄
  委托（直调插件内核）。`Cask::search()` 访问器删除（改插件句柄
  `text_plugin()/vector_plugin()/hybrid_searcher()`）。
- **SearchLayer shim 退役**：产品库零 SearchLayer 符号——shim 整体降级为
  测试夹具（tests/support/，include 名不变，测试零 diff）；
  `SearchLayerConfig` 迁 `bitcask/search_config.hpp`（公开配置面不变）；
  pre-S17 统一 search.ckpt 迁移经新 `legacy_ckpt` load-only 模块保活。
- **C API 分文件**：`bitcask_kv.h`/`bitcask_text.h`/`bitcask_vec.h` +
  `bitcask_c.h` 聚合（符号/签名/ABI 逐一致）；实现拆三 TU + internal.h。
- **Removed**：`SearchLayerConfig::wal_batch_size`（dead config，从未接线）
  及其伪基准 BM_Put_WalBatch。
### Changed（S20：全库重构 — 冗余收敛 / 最佳实践 / 注释与文档一致性，2026-07-04）

- **配置头拆分（B-C1）**：抽 `bitcask/bm25_params.hpp`（Bm25Params）、
  `bitcask/text_plugin_config.hpp`（TextPluginConfig）、
  `bitcask/vector_plugin_config.hpp`（VectorPluginConfig）三个轻量配置
  POD 头。配置聚合层（`search_config.hpp`）与插件实现层（`inverted.hpp`
  /`hnsw.hpp`）**解耦**——独立包含的预处理树不再含插件实现头（`-H`
  追踪证实）。C-API 编译时间**未减**（`c_api/internal.h` 经 `cask.hpp`
  仍转译），价值在层次解耦（为 `CaskOptions::plugins` 换代铺路）。
- **ckpt 公共化新头三件套**：新增 `bitcask/component_ckpt.hpp`（`ckpt::`
  命名空间：ChainState/DeltaSaveResult/LoadResult 单一真源；text/vec/
  docmap 各类以嵌套 `using` 别名暴露同名类型）与
  `bitcask/ckpt_chain.hpp`（`bitcask::search` 命名空间；`walk_chain` 模板
  + `remove_chain_files` helper——前者链走读公共化、后者 8-miss 链坍缩
  公共化；仅 .cpp 包含，隔离 `<filesystem>`）。`search_checkpoint.hpp`
  新增 `SectionWriter` 类：delta/base 保存「owned payload 缓冲 + 并行
  登记 CkptSection」公共累加器（依赖 std::vector 移动语义保 span 不悬垂）。
- **C API 收敛（R1）**：`c_api/internal.h::finish_single()` 统一 12 个
  单结果入口的 `expected → C 结果物化 + 错误/OOM 翻译`；`ParsedFilter`
  收敛 filtered 变体的过滤树三态转换（无过滤/成功/非法，storage 随对象
  存活不悬垂）。`bitcask_text.cpp` 8 入口与 `bitcask_vec.cpp` 4 入口
  收敛为「校验 → 委托 → finish_single」三行形态。
- **ckpt 收编（legacy + text/vector/docmap plugin）**：上一批定义的
  三件公共骨架落地——R4 SectionWriter 用于 text/vec/docmap delta 与
  text/docmap base（vec base 的 .vec/.qc8 侧车非此形态，按 R4 边界保留
  专属路径）；R6 ckpt::* 别名替换 text/vec/docmap 本地三胞胎（legacy
  字段序不同且属 P6 待删，未动）；R2 walk_chain 用于 5 处 .d 链走读；
  R3 apply_docmap_delta_section 自 docmap_ckpt 匿名 ns 提升公开
  （legacy 内联解析改调之，逐字节同构）；R7 byte helper 删 text_plugin
  的 put_u*_b 转发层与 shim 的 put/get_u*_byte 重实现，直用
  `sc::detail::*`；R8 remove_chain_files 用于 4 处 8-miss 链坍缩。
- **接口质量三件**：B-B2 `plugin::FlushResult` 加宽 `chain_seq`/`chain_wm`
  ——manifest entry 三元组 `{generation, chain_seq, chain_wm}` 由 flush
  直接多态回传，`save_checkpoint_paired` 删去「按 `*comp` 分支下探
  text_/vec_plugin_ 读 chain_state()」（第三组件零 else）。B-B1
  `index_manifest.hpp` 裸 fopen/fclose → `manifest_io::FilePtr`
  （unique_ptr<FILE,FileCloser>；独立命名空间避与 field_schema.hpp
  撞名）；fdatasync/rename/dirfsync 序不变。R5 `searcher.hpp` 三门面
  单查/批量骨架 → `search::detail::run_query` / `run_batch` 模板
  （读屏障 + 内核调用 + 错误翻译 + TextSearchResult 包装 / 并发保序批量）；
  `text::Searcher` 删私有 run()，`vec::Searcher` / hybrid 各收敛。
  `search_text_highlight` 形态不同，保留独立物化。
- **注释换代（~17 处 S20-6）**：判据「自/原 SearchLayer 平移 = 历史
  归属保留；由 X 调用/现状式描述/引已删符号 = 误导改」。cask.cpp/cask.hpp
  open 阶段 banner（SearchLayer → Text/Vector 插件）；search_vector/
  hybrid、save_checkpoint 读水位、base/delta 决策注释旧成员名；thread_pool
  search_layer.cpp → search_arena.cpp、on_vector → VectorPlugin::on_put；
  query/inverted/hnsw/index 各 1-2 处 SearchLayer 路由 → Text/Vector
  Plugin；plugin_api.hpp `name()` 加注持久化身份 vs API 命名空间双词汇。
- **文档一致性换代**：api-cpp.md include/section/示例换代（新增 §7.5
  Searcher 门面节、删 wal_batch_size/补 max_delta_chain）；concurrency-zh
  §6 重写（SearchLayer → 双插件；「单 worker」失实 → **N map worker +
  1 reducer**）+ thread-safety 锁层级表与全序图 file:line 修正
  （fields_mu_ → TextPlugin@text_plugin.hpp:360、hnsw_ → VectorPlugin@
  vector_plugin.hpp:173）；cpp-arch.md 模块清单（search_layer.hpp/cpp
  已删 → plugin_api/text_plugin/vector_plugin/hybrid_searcher/searcher/
  search_config）+ put_doc 流程 on_write → TextPlugin::on_put + 测试
  二进制数 22 → 32；api-c.md 补 6 个在册函数（put_ex / put_batch /
  status_ex / search_{text,vector,hybrid}_filtered）；format-zh.md
  新增 §10.4 index.manifest（BCMF 80B）+ per-component ckpt + delta
  段型；README 测试二进制数 26 → 32。
- **深层设计文档历史 file:line 清理**：hnsw-lifecycle-zh（rebuild_hnsw
  → VectorPlugin::rebuild@vector_plugin.cpp:240、save/load_search_ckpt
  → legacy_ckpt/per-component、CkptLoadResult → ckpt::LoadResult）、
  merge-policy-zh（on_delete → text_plugin.cpp:243、live callback →
  vector_plugin.cpp:216、rebuild → :240）、hnsw-design-zh（图内过滤
  → vector_plugin.cpp:216、RRF → HybridSearcher）、put-flow-zh
  （on_write → reducer 扇出 TextPlugin::apply_job + VectorPlugin::insert）、
  ord-recycling-design-zh（RRF ord 去重 → hybrid_searcher.cpp:45、
  rebuild_index → text_plugin.cpp:295）换代到现位；async-index-pipeline.md
  / s13-review-2026-07-02.md 顶部换代注记（正文行号保留为当时快照）。
- **保守评估弃做（R8-save / R9-flush 骨架，S20-5）**：收益不抵风险——
  R9 三组件 flush 决策差异正落在 S18-3 脆弱区（vec dim==0 / no-op 条件
  / no-op 时 vec 无条件清 dirty_），共用骨架须把这三处编码成回调/标志
  反而更漏；R8 save 公共壳小、载荷构造发散大，回调化得不偿失。低风险
  部分（remove_chain_files、SectionWriter）已在 S20-1/S20-2 收割。
- 全程行为零变化：Debug 545/545 全绿、TSan 544/544（排除 1 先例
  ThreadCountIndependentOfLibCount 不兼容）、C ABI `nm -D` 与基线
  逐符号一致、磁盘格式逐字节不变。

### Fixed（S20）

- **walk_chain `chain_seq==0` 语义修正（S20-2 补，对抗性等价审查发现）**：
  `walk_chain`（R2 抽出的链走读）用 `bounded = chain_seq != 0` 从计数推断
  有界性，把 `chain_seq==0` 当作**无界扫盘**——与三个有界调用点（text/
  vector/docmap `load_component`）的合法输入冲突：base-only 组件（0 已提交
  delta）与新库的 committed `chain_seq` 恒为 0，改前是零迭代。**危害**：delta
  先写盘、manifest 最后提交；崩溃于「写完 .d1、提交 manifest 之前」窗口或
  `write_manifest` 失败（ENOSPC）时盘上残留 manifest 未记录的 orphan .d1，
  改后无界扫盘会**重放该未提交 delta**（插件端触发无谓全量 rebase，docmap
  端 orphan 行/删除灌入活 keydir）。修复：加显式 `bool unbounded` 参数
  （text/vector/docmap 传 false 有界、legacy/shim 传 true 无界），不再从
  `chain_seq==0` 推断。回归测试 `TextPlugin.OrphanDeltaNotReplayedWhenChainSeqZero`
  （bug 版失败、修复版通过）。**注**：磁盘格式与 C ABI 不变；正常路径
  （base-only 盘上无 .d 文件）本就无分歧，故 545 套件未触发——bug 仅在
  crash 窗口的特定盘上残留态现形。Debug 546/546、TSan 122/122（checkpoint 子集）。



### Changed（S18：插件化架构 P4 — SearchLayer 拆分，2026-07-04）

- **SearchLayer 一分为三**：`text::TextPlugin`（BM25 全家：倒排/analyzer/
  查询缓存/高亮/同义词/bm25 组件 ckpt）、`vec::VectorPlugin`（HNSW/写入端
  归一化/vec 组件 ckpt 含侧车）、`search::HybridSearcher`（RRF 融合器）。
  SearchLayer 退化为兼容 shim（公共 API 面不变，P5 删除）。
- **四条通路全部经 CaskPlugin 广播**：写路径（prepare/on_put/on_delete，
  S15 起）+ checkpoint（flush/open 自治，base/delta 决策下沉每插件——
  rebuild_hnsw 只 rebase vec 链）+ merge（`run_merge` 收
  `span<CaskPlugin*>`，begin/relocate/commit/abort，收尾经
  `host->run_serialized` FIFO）+ 恢复（fold 重放批内并行 prepare +
  `PutEvent::replay`）。
- **docmap 持久化归宿主**：`index::Index` 自记账（脏位/删除日志门限）+
  `docmap_ckpt` 模块（base/delta/load 宿主直驱）；`DocLenWriter`/
  `CompactionStats` 窄接口注入（不给插件 Index&）。
- **CMake 目标终态**：新增 `bitcask_text_plugin` / `bitcask_vector_plugin` /
  `bitcask_hybrid`；`bitcask_index` 改名 `bitcask_docmap`（旧名 ALIAS 兼容
  一个版本期）；`bitcask_merge` 依赖降为 `bitcask_plugin_api`。依赖隔离：
  vector_plugin 不链 jieba/text，text_plugin 不链 HNSW。
- 磁盘格式与 C API 签名全程不变（P4 只移交持久化发起权）。

### Fixed（S18）

- **S17 回归：delta 日志门限失效**——per-component 路径不更新 legacy 链
  水位（门恒 0），fold 重叠区旧墓碑误入删除日志，重放时误杀上一代 base
  里的复活文档（S18-1，回归测试锁定）。
- `SearchLayerConfig::wal_batch_size` 坐实为 dead config（从未接线
  `enable_wal`），迁 `TextPluginConfig` 标 deprecated（P5 决）。

S13 四维审查（内存/并发/性能/功能）首批修复。

### Fixed（并发正确性）

- **【Critical·数据丢失】merge 重定位改条件 CAS（S13-F1）**：merger 曾误传
  `newest_put=true`——merge 期间任何并发 put 触发 roll 后，全部冷 key 重定位被拒
  且输入文件被无条件 unlink → key 指向已删文件、重启后永久丢失。改传 `false`
  （keydir 契约本为 merge 设计的条件 CAS 语义），并加纵深防御：`MergeStats` 新增
  `relocations_stuck`/`stuck_file_ids`，`Cask::merge` 对复查后 keydir 仍引用的输入
  文件跳过 unlink。新增回归测试 `ConcurrentWriterRollDuringMergeNoDataLoss`
  （反向验证：bug 版本下立即失败）。
- **【High·永久挂起】写路径失败泄漏 ord（S13-F2）**：put/remove/put_doc 在
  alloc_ord 之后、真任务提交之前的任何错误 return（含 `write_and_keydir` 重试
  路径的双泄漏）都会在 reorder buffer 留下永久空洞 → 此后 flush/merge/close
  全部永久阻塞（一次 ENOSPC 即卡死句柄）。新增 `OrdSkipGuard` RAII 守卫，
  错误/异常路径自动补 `IndexOp::Skip`。
- **【UB】`CaskIter::pin_files` 无锁读 `active_data_`（S13-F3）**：与并发 roll 的
  shared_ptr reset 构成数据竞争。改为 `read_cache_mu_` 共享锁内拍快照。
- **【UB】`active_file_id_` 改 `std::atomic<uint32_t>`（S13-F4）**：写者持
  `write_mu_`、读者持 `read_cache_mu_` 或无锁，无 happens-before。
- **get 与 merge unlink 窗口的假 kIo（S13-F5）**：读者先查 keydir、后 open 文件，
  merge 恰在其间重定位并 unlink → ENOENT 假失败。`get()` 现对该窗口重查 keydir
  重试一次。
- **文档矛盾订正（S13-F7）**：`cask.hpp` merge 线程安全注释与 thread-safety.md
  §7.6 统一（KV 路径安全；索引模式注明 S13-F6 未修前的并发注意事项）。

- **tbb::concurrent_hash_map 遍历与并发插入的竞态（S13-F6）**：TBB 不支持遍历与
  插入并发（rehash 可致迭代器失效），但 `ensure_vocab` 在查询线程、merge 的
  compact/ckpt 序列化在调用线程遍历，均与 reducer 的 `add_doc` 插入并发。修复：
  ① 新增 `IndexOp::RunFn`——merge 路径的 compact/`compact_index_chunks`/
  `save_search_ckpt`（含 truncate_wal）经 reorder buffer 在 reducer 线程内执行
  （同 RebuildHnsw 先例）；② vocab 侧表改增量维护（`vocab_delta_`，add_doc 仅
  新词付锁记账），重建不再遍历 map。`Cask::merge` 线程安全注释同步更新为
  索引模式亦安全。新增回归测试 `VocabConcurrentNewTermsAndMergeNoRace`。

- **【High·UAF】fstats 无锁读与 deque 扩容的竞态（S13-F8，F1 修复揭出的存量
  问题）**：merge 线程 `update_fstats` 无锁 `fstats_[idx]` 与写者 `emplace_back`
  的 deque 内部块指针表重分配构成 use-after-free（「元素地址稳定」≠
  「operator[] 并发安全」；F1 修复前 merge 的该路径从未真正与写者并发执行，
  故一直潜伏）。修复：旁挂 RCU 指针表 `fstats_ptrs_`（扩容建新表 release 发布、
  旧表退休不释放，内存有界），无锁读点全部改经 `fstats_slot()`。TSan 下
  F1 并发测试 10 连跑 + 全并发批零告警。另：`cmake/tsan.supp` 增补
  `mutable_pl` CoW 协议的 fence 假阳性抑制（协议正确，TSan 不建模 fence）。

### Fixed（内存 / C API 健壮性）

- **C API `bitcask_iter_next_batch` 错误中途泄漏（S13-M1）**：返回 -1 前现已释放
  已填充条目的 key/value malloc 缓冲（契约在头文件注明：错误时调用方无需 free）。
- **extern "C" 边界异常隔离（S13-M2）**：31 个导出函数统一 try/catch——C++ 异常
  穿越 C 栈帧是 UB（通常 terminate），现翻译为 `BITCASK_ERR_IO` + fault 详情
  （bad_alloc→ENOMEM）。所有 malloc/strdup 现已检查返回值，OOM 时清理半成品
  并报错（此前直接对 nullptr memcpy）。
- **5 处 FILE\* 异常路径泄漏（S13-M3）**：keydir 快照 / HNSW / 倒排 / WAL replay /
  migrate 的加载函数改用 `unique_ptr<FILE, FileCloser>` RAII——文件大小来自可能
  损坏的输入，缓冲分配可抛 bad_alloc。

### Added

- **插件回调接口层 `bitcask::plugin`（S15 P1）**：新增自包含头
  `include/bitcask/plugin_api.hpp`（`CaskPlugin`/`PluginHost` + KV 事件类型）与
  `bitcask_plugin_api` INTERFACE 目标。KV 核心的 IndexPool 写路径改经
  `CaskPlugin` 接口分发（SearchLayer 经 `SearchLayerAdapter` 作「唯一插件」接
  入，行为零变化）；`thread_pool.hpp` 不再依赖 search 头（reorder variant 塌缩
  为 Put/Delete/Skip/RunFn 四类通用条目，原 `IndexOp::RebuildHnsw` 并入 RunFn
  通道）。设计见 `doc/plugin-arch-split-design-zh.md`。内部管线类型
  `ReduceEntry/OnWriteEntry/RebuildEntry` 移除（非公开 API，无 ABI 影响）。
- **`Cask::search_text_highlight` 门面方法（S13-D3）**：README 功能表宣称已久但
  实际只在 SearchLayer 上（绕过门面丢失 closed fail-fast 与 flush 可见性契约）。
  现补上门面（返回 `HighlightSearchResult`，命中含高亮片段），README 与代码对齐。
- **前缀扫描（S13-D4）**：`CaskIter::start` / `Cask::parallel_scan` 新增尾置默认
  参数 `key_prefix`（源兼容）——遍历命名空间（如 `"user:"`）无需全表扫 + 自行
  过滤；过滤在 keydir proxy 层，非匹配 key 零 pread 零拷贝。
- **批量写 `put_batch`（S13-D1）**：全批校验（零副作用）→ 聚合 pwrite → 单次
  flush（durability 按既有 sync 策略：o_sync 即时 / sync_every_n>0 整批一次组
  提交 / 否则 caller sync() 控制）→ keydir apply（flush 之后 ⟹ 本进程内
  all-or-nothing 可见）。批内 syscall 从 N 次摊到少数几次；merge race 条目自动
  走单条重写路径。C API 新增 `bitcask_kv_pair_t` + `bitcask_put_batch`。
  不提供跨崩溃原子性（契约注明，与连续单条 put 的崩溃语义一致）。
- **布尔查询语言：括号嵌套 + 引号短语（S13-D9）**：`bool_search` 现支持
  `+(rust go) +web`、`+"exact phrase"`、`-"..."` 及任意嵌套（递归下降 parser +
  集合式树求值，短语按 positions 匹配）。仅含新语法的查询走树路径——既有
  扁平查询行为位级不变。
- **搜索分页 offset（S13-D10）**：`search_text/search_phrase/bool_search` 新增
  尾置 `offset` 参数（overfetch 后截断；total 估计有意不做——剪枝下仅有下界，
  见 TASK.md）。
- **per-key TTL（S13-D5）**：`put(..., expiry_at)` / `DocInput::expiry_at` /
  C `bitcask_put_ex`（绝对 unix 秒，0=永不）。过期后 get/iter 视作不存在；
  merge 时不搬运并 CAS 清 keydir（`MergeStats::records_expired`）。格式：
  DocValue 新 flag 0x20 + 末尾 u32 段——**旧版本库读到带 TTL 的记录会静默
  忽略 TTL（永不过期），不拒绝**。
- **备份/热拷贝 API（S13-D6）**：`Cask::backup(dst_dir)`——封存 active 后
  hardlink（跨设备回退 copy）全部数据与元数据文件，备份目录可独立 open；
  原库不停机（下一次 put 自动重建 writer）。caller 须保证与 merge 不并发。
- **日志回调 hook（S13-D7）**：`CaskOptions::log_fn`（open-time 不可变、可能从
  任意内部线程调用、回调抛出被吞）。库内 6 处 best-effort 静默失败点现可观测
  （keydir/search checkpoint 保存失败、stuck 重定位、索引 worker 异常、close
  兜底等）。C API 追加 `log_fn`/`log_ctx`。
- **统计扩展（S13-D8）**：StatusInfo 加 `hnsw_nodes`/`search_cache_entries`/
  `read_handles`；C API additive `bitcask_status_ex`。total_postings 有意不含
  （遍历 concurrent map 与 reducer 并发不安全，待原子计数器）。
- **HNSW 建图参数透传（S13-D11）**：`SearchLayerConfig::hnsw_m`/
  `hnsw_ef_construction`（0=默认 16/200），构造与 merge 期 rebuild 均生效；
  C options 同步。
- **C API meta 过滤（S13-D2）**：V5 meta 过滤此前 FFI 完全不可用。新增
  `bitcask_meta_filter_t`（条件数组 + And/Or + 嵌套子树）与三个 **additive**
  检索变体 `bitcask_search_text_filtered` / `bitcask_search_vector_filtered` /
  `bitcask_search_hybrid_filtered`（ABI 兼容，既有签名不动）。全部 8 算子
  （Eq/Neq/Gt/Gte/Lt/Lte/In/Exists）+ 5 值类型；非法 filter → INVALID_OPTION。

### Performance

- **HNSW 读路径 seqlock（S13-P7）**：读者 `copy_neighbors` 此前对 per-node
  自旋锁做 exchange（写操作）——hub 节点并发查询缓存行核间乒乓。写者单线程
  ⟹ 改 seqlock（数据字 `atomic_ref` relaxed，TSan 干净），读侧零共享行写。
  新增并发基准 `BM_Hnsw_SearchConcurrent`（1→4 线程延迟持平实证）。
- **结构性两件（S13-P8 收官，14/14）**：merge `pending_` 分批 apply（内存
  峰值 O(全部活 key) → O(单文件批)；失败语义修订为「已 apply 批指向已 fsync
  输出、输出保留」，无中间不可读态）；`rebuild_hnsw` 改 `clone_live` 结构化
  拷贝（零距离计算替代从零重插，100k 节点分钟级 → memcpy 级；死邻一跳路径
  收缩补边；int8-only 免反量化往返）。
- **查询侧三件套（S13-P8，+3 → 12/14）**：`invalidate_terms` 反向索引
  （O(全部条目×词) → O(变更词+受害条目)，写路径独占锁热点）；meta filter
  改 `Index::eval_meta` 锁内求值（免 overfetch 每候选一次 blob 堆拷贝，
  HNSW 图内过滤回调同享）；`search_fields` 删死代码 + 按 boost 分组一次
  多词搜索（内核调用 O(组)；行为改进：跨词组合分高的文档不再被单词 top-k
  截丢）。
- **启动/checkpoint 三件套（S13-P8，+3 → 9/14）**：FOR 解码 64-bit 窗口
  （大索引 ckpt 加载主导项，位级等价）；`save_vec_payload` 流式写（峰值内存
  从整 payload ~1.5GB 降到一页）；hint 恢复单遍 `fold_validated`（原校验+
  fold 各读一遍文件）。
- **搜索杂项批（S13-P8，6/14）**：短语打分 thread_local 复用 + 最稀有词驱动
  （分数逐字节同果）；`search_fuzzy` 并行化（镜像 wildcard）；`on_delete`
  空缓存跳过重分词；C `bitcask_get` 消除双拷贝（直接消费零拷贝 view）；
  vocab 全量重建由 F6 delta 设计覆盖。
- **HNSW int8 热路径零分配 + madvise 批量化（S13-P5）**：`quantize_into` +
  thread_local 复用消除每查询/插入的 codes 堆分配；精排预取按地址区间合并，
  k=256 时 ~768 次 madvise/查询降到个位数。SIMD round 有意不做（舍入模式与
  std::round 在 .5 边界不同，codes 入 checkpoint，违反位级不变约定）。
- **搜索管线改按词选择性失效查询缓存（S13-P1）**：`reduce_apply` 曾无条件清空
  整个查询缓存——所有 put/put_doc 走管线，混合读写负载下命中率归零。现与单文本
  路径 `on_write` 对齐，用文档词集调 `invalidate_terms`。
- **`fdatasync` 替代 `fsync`、`O_DSYNC` 替代 `O_SYNC`（S13-P2）**：追加写下持久性
  语义等价，每次持久化省一笔元数据 journal 提交（ext4/xfs 可观）。WAL 契约不变。
- **`bool_search` 消除 posting 快照二次深拷贝（S13-P3）**：must/should 的
  `TermPostings`（热词可达 MB 级扁平快照）合入评分数组时改 move（原为整体拷贝，
  每个含 SHOULD/MUST_NOT 的查询都付）。
- **`search_wand` doc_len 按块惰性填充（S13-P4）**：DAAT 全量前置
  `fill_doc_lens` 抵消 WAND 块跳跃剪枝——每 term 加 `dls_filled` 位图
  （每 `kBlockSize=128` 一位），pivot 评分点处按需 `ensure_dls`；被跳
  过的块永付 gather 成本。`live` 仍全量（IDF 用 live_df 不可换 raw df）。
  位级行为等价，无新回归。
- **`DataFile::fold` 改 256KiB 分块流式读（S13-P6）**：原每条 record 2 次
  pread（header + body），百万条即两百万 syscalls；照搬 hint_file 的 chunked
  refill 模式后降 3 个数量级。影响 merge 全部输入扫描、搜索模式恢复与纯 KV
  无 hint 回退。

**验证**：Debug（clang）全量 503/503（494 既有 + 本批 9 新回归：put_batch×2
+ ttl/backup/log/status_ex/hnsw_param/c_pagination × 7）；TSan 并发相关全过
（`ThreadCountIndependentOfLibCount` 为 TSan 环境既有失败，干净树同样失败，
与本批无关）；C API 11/11；Release 构建干净。

## [3.1.0] - 2026-07-01

S12 全库审计批次落地：read 句柄默认上限 / reducer 内自动 compaction（opt-in）/
field.schema 加 magic+version+CRC（FSCH v1）/ **bitcask.meta 加 CRC（v3）** / C API
能力扩展（批量检索×3 + parallel_scan + BITCASK_ERR_CLOSED）/ C API 头线程安全注释订正 /
clang 构建 job / -Werror 库构建护栏 / 三套版本号单一真源。

> **版本语义**：本版为向后兼容的**功能新增**（C API 纯增量、格式加校验），故 MINOR +1
> → `3.1.0`；ABI 未破坏（新符号 + 枚举末尾加值），`SOVERSION` 保持 `3`。
>
> ⚠️ **前向不兼容（数据格式）**：本版写出的库**不能被 3.0.0 打开**——`bitcask.meta`
> 升至 v3（3.0.0 读端只认 v2，遇 v3 报 "unsupported meta version"）。**反向兼容**：本版
> **能读**旧库（v2 meta 兼容读、legacy field.schema 自动升级为 FSCH）。升级请单向进行。

### 新增（Added）
- **read 句柄默认上限（防 fd/mmap 无界，S12-1）**：`CaskOptions::max_read_handles = 0`
  从「不限」改为按 `RLIMIT_NOFILE` 软上限自动推导（约一半、下限 64）。新增
  `kUnlimitedReadHandles` 哨兵显式不限。小/中库行为不变、零 churn；大库由 fd 耗尽
  crash 改为 graceful 句柄淘汰（miss 时重开 sealed 文件 ~μs）。
- **reducer 线程内自动 compaction（opt-in，S12-2）**：新增
  `SearchLayerConfig::auto_compact_dead_ratio`（默认 0=关；`(0,1]`=开+per-list 阈值）。
  Index 加 `retired_since_compact_` 计数器；`maybe_auto_compact()` 在 reduce_apply /
  on_write / on_delete 末尾调用，开时累计退休达 `max(1024, live/2)` 才在 reducer
  线程内 compact（与 add_doc/put_doc 同线程、无并发窗口，TSan 三例零 race 实证）。
  默认关零开销；附 `total_postings()` 内省。
- **field.schema 加格式头（FSCH v1，S12-3）**：文件头 8 字节
  `[magic="FSCH":u32][version=1:u32]`（小端）+ 每条 entry 的
  `[NameLen:u16][name][CRC32:u32]`（CRC 覆盖 `[NameLen|name]`）。magic/version 未知或
  entry CRC 不符 → `open()` 返回 false（fail-fast）；torn tail 容忍跳过。兼容旧库：
  peek 前 4 字节，无 magic 按 legacy `[len][name]` 照读并在可写目录**原子升级**
  （temp + fsync + rename，权威数据零丢失窗口）。
- **bitcask.meta 加 CRC（version 2→3，S12-3b）**：保留区偏移 14 放 CRC32（u32 LE，
  覆盖 `[0,14)`）。读端：v1 拒绝（大端）；**v2 向后兼容读**（无 CRC 字段，旧库不破坏）；
  v3 校验 CRC 失配 → fail-fast。写端恒写 v3；`migrate_le` 输出改 v1→v3（含 CRC）。
  补齐审计发现的「meta 有 magic+version 但无 CRC」缺口，使 field.schema 与 meta 都具备
  magic+version+CRC 三件套。
- **C API `BITCASK_ERR_CLOSED = 13`（S12-5）**：C++ `CaskError::kClosed` 末尾追加
  （ABI 增量安全），11 处 `is_closed()` fail-fast 从 `kInvalidOption` 改为 `kClosed`。
  C 枚举加 `BITCASK_ERR_CLOSED` + `to_c_error_kind` 映射。**关键**：纯 C API 下不可达
  ——`bitcask_close` 直接 `adopt+delete` 销毁句柄，close 后再用是 use-after-free
  （caller bug）；kClosed 的实际受益方是 C++ 消费方，C 映射为完整性 / 未来路径保留。
- **CI 矩阵扩容**：
  - **clang Debug 构建 job**（`clang-build-test`，S12-6）：ubuntu-24.04 + Clang +
    Debug，作为 GCC 主构建的**可移植性护栏**，抓 gcc-ism（AVX-512 intrinsic 分支的
    `__GNUC__` 条件、`\x` 转义贪婪等 clang 更严之处）。
  - **`-Werror` 库构建 job**（`werror-lib`，S12-7）：GCC 13 + Release + `BUILD_TESTING=OFF`
    + 只建 `bitcask_static`/`bitcask_shared`，开启 `-Werror` 作 first-party 新告警
    回归护栏。third_party 头标 SYSTEM 不受影响。

### 变更（Changed）
- **bitcask.meta 版本 2→3 + CRC32（S12-3b，field.schema 头+CRC 的姊妹项）**：
  `bitcask.meta` 之前有 magic+version 但**无 CRC**（18 字节里 metric/dim/quant/inmem
  单 bit 翻转检测不出 → 静默以错误配置打开库）。`kMetaVersion` bump 2→3，保留区偏移
  14 放 CRC32（u32 LE，覆盖 `[0,14)`），与 data/hint/field.schema 同多项式
  （`hw::crc32`）。
  - **读端向后兼容**：v1（大端 legacy）仍干净拒绝；v2（无 CRC）向后兼容读（旧库不
    破坏）；v3 校验 CRC 失配 → fail-fast（`bitcask.meta CRC mismatch`）。
  - **写端恒写 v3**：所有 open + 重写路径自动写 v3（带 CRC）。
  - **`migrate_le` 输出 v1→v3**：含 CRC 写入与 `write_meta` 一致。
  - **验证**：新增 `MetaV3CrcRoundTripAndCorruption`（往返 + 篡改覆盖区一字节 →
    CRC 失配拒绝）+ `MetaV2BackwardCompatRead`（v2 无 CRC 兼容读）。migrate
    RoundTrip 断言更新为 v3 + 校验 CRC。**全量 488/488**（486+2），Release +
    `-Werror` 库构建干净。
  - **附带回答**：field.schema legacy 读后**确实原子重写为新升级格式**
    （`upgrade_legacy_to_new_`），仅只读目录升级失败时才回退 legacy 追加。
  - 文档：`doc/format-zh.md` 加版本读端策略表 + CRC 偏移；`doc/migrate-le.md` 同步
    v1→v3 描述。
- **C API 能力缺口全部补齐（S12-5 [中]）**：
  - 头里早已声明 `bitcask_search_text_batch` / `bitcask_search_vector_batch` /
    `bitcask_search_hybrid_batch`（+ `bitcask_iter_next_batch`）但 `.cpp` 未实现——
    本批一并补齐实现。共用 `fill_batch_results` helper + `bitcask_search_result_batch_free`
    释放（先逐个 `result_free` 再 `free` 数组）。`search_hybrid` 额外新增
    `bitcask_hybrid_query_t{text, vector, vector_len}` 结构体。公共模式：
    queries/single-query 为 NULL → `INVALID_OPTION`；`n==0` → `*out_results=NULL + OK`；
    首条失败查询回填 fault + 对应 `out_results[i]=NULL`。
  - 新增 `bitcask_parallel_scan` + `bitcask_scan_fn` 回调 typedef（callback + `ctx`
    用户状态）。透传 C++ W4 的 `parallel_scan`：单次快照所有 live key → 按 `n_threads`
    分段并发 `get` 读值 + 回调（**回调可能多工作线程并发调用**）；`n_threads==0` →
    `hardware_concurrency()`；并发删除致 get not-found → 跳过；IO/CRC 错误 → 停止并
    返回。`key/value` 是零拷贝 view（仅回调内有效）。
- **C API 头线程安全注释订正（S12-5 [高]）**：`bitcask_c.h` 旧「put/delete/search 非
  线程安全，caller 串行化」与 C++ W1/W2 内化线程安全**矛盾**（C API 是 Cask 的透明
  包装、无 C 层共享可变态，完全继承其契约）。重写为「同一 handle 多线程安全」对齐
  `cask.hpp:6-24` / `api-c.md §14`，含读/写/读写并发/merge/iter 各条。**纯注释、
  零行为变更**。
- **三套版本号单一真源（S12-7）**：`project(libbitcask VERSION ...)` 为唯一手写处；
  `configure_file` 从 `PROJECT_VERSION*` 生成 `c_api/bitcask_version.h`，
  `bitcask_c.cpp` 用宏替换原硬编码 `return 3/0/0`（`__has_include` 优雅回退到
  `0.0.0-unknown` 占位）。库 `VERSION/SOVERSION` 改 `${PROJECT_VERSION}/${PROJECT_VERSION_MAJOR}`。
  杜绝 `SOVERSION` / C API / 库 `VERSION` 三处手工同步漂移。
- **C API 测试链接 `bitcask_sanitizers`**：`bitcask_c_api_test` 补 link（与其它测试
  目标一致），sanitize 构建下未插桩的 C 主程序链接已插桩 `.so` 不再 SEGV（KV-only 时
  不触发，search 测试首次暴露）。

### 修复（Fixed）
- **AVX-512 归并可移植性 bug（S12-6）**：`hnsw.cpp:150` `#if` 只判 `__GNUC__>=10` 漏
  了 clang（其 `__GNUC__` 恒为 4，即 GCC 4.2.1 兼容伪装）→ 落入 `#else` 用了 clang
  不认的 `_mm512_extractf64x4_ps`。补 `defined(__clang__)` + 修正死分支 intrinsic。
- **`kDefaultField` 字节可移植性 bug（S12-6）**：`"\x01default"` 的 `\x` 转义贪婪吞
  "defa" → 实际是 `0xFA + "ult"`（GCC 静默、clang 报错）。改 `"\xfa" "ult"` 保留
  完全相同字节（已入 checkpoint，零 on-disk 变化）。
- **`cask.cpp` 忽略 `save_search_ckpt()` 返回值（`-Wunused-result`）**：显式 `(void)`
  + best-effort 注释（checkpoint 失败非致命，下次 fold 重建）。
- **设计文档 4 处状态行订正（S12-4）**：
  - `docs/design/async-index-pipeline.md`「评审中（未实现）」→「**S6 已落地**」+ 唯一
    偏差（单 reducer 线程替代 M 线程池 → 库间 apply 未并发，仅 Map 并行）。
  - `doc/hnsw-design-zh.md`「过滤检索 V3 不做」→「**V5 图内过滤已落地**」。
  - `doc/hnsw-int8-only-design-zh.md`「盘上直存 int8 仍未做」→「**V7 BVH2 v2 段直存
    qcodes+scale+sum 已落地**」+ DocValue int8 落盘。

### 构建 / 工具链
- **`-Werror` 选项**：新增 `option(BITCASK_WERROR OFF)`，开时给 `bitcask_warnings`
  INTERFACE 加 `-Werror`。**默认关**——避免新编译器新告警破坏下游 / 本地构建；
  CI `werror-lib` 开启作护栏。
- **`cppjieba` SYSTEM include**：消除 third_party 头大量告警对 `-Werror` 的干扰。
- **13 处 first-party cosmetic 告警清零**（`-Wshadow`×7 + `-Wsign-conversion`×4 +
  `-Wunused-function`×1 + `-Wunused-parameter`×1）：机械修复、零行为风险（rename
  `max_tf`/`pos`/`k`、删冗余 `using Cand`、删未用 `str_to_bytes`、`[[maybe_unused]]`
  key、3 处 `static_cast<ptrdiff_t>`）。

### 说明（Notes）
- 全量 488/488 零回归（C++，Debug GCC，含 S12-3b 新增 2 例）；C API 7/7 通过（plain +
  ASan(含 leak) + TSan）；TSan 三例零 race（reducer 内 compact 并发护栏）。
- **默认行为变更**：`max_read_handles = 0` 由「不限」变「按 RLIMIT_NOFILE 自动上限」
  ——需显式不限者用 `kUnlimitedReadHandles`。
- **未做（有意）**：
  - `bitcask_close` 仍返 `void`：C++ `close()` 本就 void + noexcept + best-effort；
    改签名是破坏性变更却无实益。若要让 C 侧也能「close 后 fail-fast 不 UAF」，
    需把 close 拆为 `close`(不销毁) + `free`(销毁) ——ownership 模型破坏性变更，
    留待独立评估。
  - 后台线程驱动 compaction：违"无后台维护线程"哲学 + 需 flush stall；用 caller
    驱动 + reducer 内 opt-in 替代。
  - macOS / ARM64 CI job：需对应 runner，本地无法验证——S12 唯一未决项。

---

## [3.0.0] - 2026-06-25

> ⚠️ **破坏性 ABI 变更**（库 `SOVERSION 1 → 3`，C API 产品版本 `2.2.0 → 3.0.0`）。
> 下游 C/C++ 调用方需**重新编译链接**；soname `libbitcask.so.1 → libbitcask.so.3`，
> 链接器会在 ABI 不兼容时明确报错而非静默崩溃。
>
> 本版**统一三套版本号为 `3.0.0`**：此前 CHANGELOG（1.x）、C API（2.x）、库 VERSION
> 各自为政；自此 CHANGELOG 发布版本 = 库 VERSION = C API 版本 = `3.0.0`，SOVERSION = 3。

### 变更（Changed — 破坏性）
- **同义词词典：运行期 setter → open-time 不可变配置**。
  - **移除** `bitcask_set_synonym_map`（C）/ `Cask::set_synonym_map`（C++）。
  - **新增** `bitcask_options_t::synonym_file_path`（C，同义词文件路径）/
    `CaskOptions::synonym_map`（C++，`std::shared_ptr<const text::SynonymMap>`）。
  - 词典 open 时一次性加载、构造后**不可变** → 并发查询天然安全（消除配置项里唯一的
    reader-vs-writer 竞态，把「须先于并发配置」的口头契约升级为结构保证）。运行期更换
    词典需重开库；按请求用不同词典请自行展开查询串。

### 版本（Versioning）
- **三套版本号统一为 `3.0.0`**：C API `bitcask_version_*` `2.2.0` → **`3.0.0`**（删公共函数 =
  major）；库 `VERSION` `1.0.0` → **`3.0.0`**、`SOVERSION` `1` → **`3`**；CHANGELOG 发布版本同步为
  `3.0.0`。

---

## [1.2.0] - 2026-06-25

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

[Unreleased]: https://github.com/davidalphafox/libbitcask/compare/v3.1.0...HEAD
[3.1.0]: https://github.com/davidalphafox/libbitcask/compare/v3.0.0...v3.1.0
[3.0.0]: https://github.com/davidalphafox/libbitcask/compare/v1.2.0...v3.0.0
[1.2.0]: https://github.com/davidalphafox/libbitcask/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/davidalphafox/libbitcask/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/davidalphafox/libbitcask/releases/tag/v1.0.0
