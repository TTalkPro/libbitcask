# 更新日志（Changelog）

本文件记录 libbitcask 的所有重要变更。

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)；
版本遵循语义化版本。**3.0.0 起三套版本号统一**（S12-7 后单一真源 =
`project(libbitcask VERSION ...)`）：CHANGELOG 发布版本 = 库 `VERSION` = C API 产品版本
`bitcask_version_*` = **`11.1.0`**；库 `SOVERSION` = **`11`**（= major）；
盘上格式版本独立于库版本：`bitcask.meta` = **`v5`**（基线；使用原子批的目录懒升 **`v6`**），
hint = 历史 BCH4/BCH5（新库停写），OKI = **BCOK v1/v2/v3 / BCOM v1-v4**，
keydir 快照 = 历史 BCKS v3/v4（不再生成），
`field.schema` = **FSCH v1**。
**盘上格式破坏不驱动 major**（3.1.0 / 5.1.0 两次先例）——major 只在 ABI 破坏时 bump
（4.0.0 / 5.0.0 / 6.0.0 / 7.0.0 / 8.0.0 / 9.0.0 皆是）。

---

## [Unreleased]（S58 / S59：merge_only 旁车合并）

> 版本未定。C API、盘上格式、公共结构体零改动。

### Fixed

- **检索库经 merge_only 旁车合并永久挂住**（S58，下游反馈 `feedbacks/2026-11-20-merge-only-search-merge-hangs.md`，bitcask）：
  旁车车道以打开时的 `next_ord`（未折叠数据）注册，合并开头的受控重建把 `next_ord` 推到数据末尾，收尾读屏障
  `flush_index()` 无界等待；同进程旁车则等写者的新写。11.1.0 起 `needs_merge` 不再漏报，`merge` 会真的走到这里。
  现旁车在检索库上按纯 KV 合并（不带检索插件、不做检索侧收尾），`flush_index()` 对 merge_only 直接返回。
- **旁车合并改写写者的检索持久化**（S58）：收尾成对保存点用旁车的空检索索引改写 `bm25.ckpt` / `docmap.ckpt` /
  `index.manifest`，逼写者下次整库重放。现旁车不写检索持久化（与 `checkpoint()` / close 拒绝 merge_only 一致）。
- **跨进程 merge_only 旁车合并后，开着的写者读不到被搬走的 key**（S59）：旁车关库删掉输入文件，写者内存索引仍指旧位置
  ⇒ 点查 `kIo "open file_id=N"`、全表迭代 `fold location disappeared`，重开才好（纯 KV 同样）。现读到「定位所指数据文件
  已不存在」时从 data 受控重建一次再重试，迭代器按重查到的新位置重读；文件还在的读错误照常上报。代价：每次跨进程合并后
  写者第一次碰到被搬走的 key 时整库重建一次。同进程旁车（共享 registry）本不受影响。

---

## [11.1.0] - 2026-11-20（S57：jieba 缺词典 open 报错、`needs_merge` 用实时账本）

> **版本语义（2026-11-20 定版）**：C API、盘上格式、既有结构体布局零改动，C++ 侧仅新增自由函数
> `text::jieba_missing_dict_file` ⇒ ABI 兼容，取 MINOR 11.1.0，库 `SOVERSION` 保持 `11`（`.so.11`），
> C++ 下游无需重新编译链接。行为变化：词典缺失的 jieba 库由「abort 进程」变为 `open` 返回
> `kInvalidOption`；`needs_merge` 结论改与 `status()` 一致（提交点之后的死字节也算）。

### Fixed

- **jieba 词典路径不对时整个宿主进程被 abort**（S57，下游反馈 `feedbacks/2026-11-19-jieba-missing-dict-aborts-process.md`，bosun）：
  工厂不检查词典文件就交给 cppjieba，后者 `XCHECK` 失败即 abort。现在建分词器前先查 5 个词典文件，缺任一 ⇒ `Cask::open`
  返回 `kInvalidOption`，报文带缺失的完整路径（C API 同为 `BITCASK_ERR_INVALID_OPTION`）。不降级：词典缺失的库打不开。
- **`needs_merge` 看不见本次打开之后的死字节**（S57，`feedbacks/2026-11-19-needs-merge-blind-to-dead-bytes-since-commit-point.md`）：
  候选统计优先取上一个提交点的 v4 清单快照，长驻写者重开前永远不合并，与 `status()` 结论相反。现改用实时 per-file 账本
  （`status()` 同源）。

### Added

- `text::jieba_missing_dict_file(dict_dir)`：返回第一个缺失 / 不可读的 jieba 词典文件路径（全在返回空串）。

---

## [11.0.0] - 2026-11-20（S56：`search_fields` 打分口径）

> **版本语义（2026-11-20 定版）**：⚠️ **C++ ABI 破坏**——已安装公共头 `include/bitcask/segment.hpp` 的
> `search::FieldSegmentView`（加 `live`）与 `search::SealedSegment`（加成员）布局变化，按 [10.0.0] / [7.0.0] 先例取
> MAJOR +1 → **`11.0.0`**，`SOVERSION` → **`11`**（`.so.11`），**C++ 下游须重新编译链接**。
> C API 与盘上格式零改动。**打分行为变化**：既有库的 `search_fields` 分数与排序会变；
> `index_catch_all = false` 的库默认字段（`search_text` 等）分数会变。不提供旧口径开关。

### Fixed

- **`search_fields` 分数与 `search_text` 对不上、重开前后排序变化**（S56 ①，下游反馈
  `feedbacks/2026-11-19-search-fields-local-stats-and-total-doc-len.md`，bosun）：`search_fields_attributed` 逐段用本段本地的
  N / df / sum_dl 打分，S45-4 后新文档散在各分片 building 段里各算各的 idf ⇒ 刚写入与落盘后分数不同（`title:w` 差 2.4 倍）、
  默认字段查询与 `search_text` 不一致。现每字段跨段聚合统计（同 `search_text`），`search_fields "w"` 与 `search_text "w"` 任何时刻
  逐位一致。
- **命名字段打分的文档长度取了整篇总词数**（S56 ②）：BM25 的 dl 取段级整篇总长、avgdl 却是该字段的 ⇒ 短字段（title / labels）
  被按「整篇长度 ÷ 字段平均长度」重罚，`field:term^boost` 的加权随正文长度漂移。现 dl 取该字段自身词数（posting dls 列 / 写入时
  记录；v2 段首次查询该字段时从 posting 惰性推出，盘上格式不变）。`index_catch_all = false` 时默认字段只含正文，此前同样被
  命名字段长度压分，一并归正；catch-all 开（缺省）时默认字段分数逐位不变。

---

## [10.0.0] - 2026-11-19（S55：分词器配置进 meta、按记录打开、ngram 高亮）

> **版本语义（2026-11-19 定版）**：⚠️ **C++ ABI 破坏**——已安装公共头 `include/bitcask/meta_file.hpp` 的
> `meta::MetaConfig`（加 `analyzer`）与 `meta::MetaError`（加 `kind`）、`include/bitcask/search_config.hpp` 的
> `search::SearchLayerConfig`（加 `analyzer_from_meta`，经 `CaskOptions::search_config` 进开库选项）三个结构体
> **加了成员 = 布局变更 = ABI 破坏**，按 [7.0.0] / [6.0.0] 先例取 MAJOR +1 → **`10.0.0`**，`SOVERSION` → **`10`**
> （`.so.10`），**C++ 下游须重新编译链接**。
> C API 侧全是**纯新增**（枚举值 `BITCASK_ANALYZER_STORED`、错误码 `BITCASK_ERR_UNSUPPORTED_FORMAT`、
> 结构体 `bitcask_meta_info_t`、函数 `bitcask_meta_read` / `bitcask_meta_info_free`），既有 C 结构体布局与函数签名零改动
> ⇒ C ABI 兼容，旧 C 二进制可直接换新库。
> 盘上格式：`bitcask.meta` 追加可选 **C2 尾段**（版本仍 v5/v6；C1 读端只读前 27 字节，对其透明）。

### Fixed

- **ngram 分词的库高亮片段恒空**（S55，下游反馈 `feedbacks/2026-11-19-analyzer-not-recorded-ngram-no-highlight.md`，
  keelson 转 bitcask-viewer）：`NgramAnalyzer::analyze_with_offsets` 从 positions 派生、每个 token 记 `{pos, 0, 0}`
  ⇒ 高亮器丢掉全部区间 ⇒ 缺省分词器（ngram）的库热 / 冷都没有片段，且不报错。现给真实字节偏移（归一化文本坐标，同
  whitespace / jieba），与 positions / tf 两版共用 `ngram_collect` + `materialize_and_filter`（term 集与停用词过滤逐位一致）。
- **高亮器漏标多词与重叠 token**（S55）：`highlight` 按 `unordered_map` 遍历序逐词插标签、只收 `start >= pos` 的出现
  ⇒ 位置在前、遍历在后的查询词标不出；ngram 的重叠 token（「着色」「色器」「着色器」）只标出残片。现在片段内先按位置排序、
  合并重叠 / 相接区间再插标签（「quick … fox」两词都标，「着色器」标成一整段）。

### Added

- **`bitcask.meta` 记录分析器配置本身**（S55，C2 尾段）：类型、n、停用词开关与自定义表、token 长度上下限、词干——
  即 C1 指纹覆盖的全部字段；`dict_path` 不记（机器相关路径）。自带 CRC，读端再用 C1 指纹自校验，任一不过当未记录；
  只在 C1 存在时写。建库、KV→检索库升级时记录；**S55 之前的库**可写打开且配置与 C1 指纹相符时顺手补记。
  C++：`meta::MetaConfig::analyzer`（`std::optional<text::AnalyzerConfig>`）。
- **按库里记的分析器打开**：C `analyzer_type = BITCASK_ANALYZER_STORED` / C++ `SearchLayerConfig::analyzer_from_meta`
  ——给不知道建库配置的只读查看 / 诊断工具。`jieba_dict_path` 仍取选项。没有 C2 的旧库：C1 指纹恰是某个缺省配置
  （ngram / whitespace / jieba × 停用词 × 词干）就按它开，否则 `INVALID_OPTION`。新建目录按缺省 ngram 建并记录。
  （修带的一处：`Cask::open` 建搜索插件改用开库后改写过的选项，否则按记录覆盖的分析器到不了插件。）
- **`bitcask_meta_read` / `bitcask_meta_info_free`**：不开库、不拿锁读 meta（模式、向量配置、ICU / Unicode 版本、
  C1 指纹、C2 分析器配置）。失败分类：`NOT_FOUND`（目录不存在 / 不是目录 / 没有 `bitcask.meta`）、`BAD_CRC`（损坏）、
  新增 **`BITCASK_ERR_UNSUPPORTED_FORMAT` = 18**（纪元门禁拒开的旧格式，detail 写明迁移还是重建；仅 C 侧）、`IO`。
  C++：`meta::MetaError::kind`（`kIo` / `kCorrupt` / `kUnsupported`）。
- 回归：`AnalyzerRecordTest.*`（C2 往返 / 坏 C2 不影响 C1 / 与指纹不符不写 / 按记录打开 / 保留选项的词典路径 /
  旧库认缺省 / 旧库非缺省拒开 / 可写补记 / 不符不补记 / ngram 热冷读高亮）、`NgramOffsets.*`、`HighlighterOrder.*`、
  C `test_meta_read_and_stored_analyzer`。

### 已知

- **`HighlightColdTest`（S53）在高并发下偶发失败**：用例靠「写 1500 篇填充让目标落进封好的段 ⇒ 重开即冷」，
  高负载下重开仍从数据重放、把原文填回 LRU ⇒ 前提不成立。改动前的 9.1.0 基线同样如此（16 路并发 64 次：
  基线 29 / 32 次失败，S55 后 21 / 36 次）；串行跑稳定通过。S55 自己的冷路径用例改用 `doc_text_cache_max = 0`
  保证必冷（64 次 0 失败）。S53 用例未改。

---

## [9.1.0] - 2026-11-19（S52 / S53 / S54：下游反馈三条）

> **版本语义**：全部是**纯新增**——C API 新增函数 `bitcask_search_text_highlight_ex` 与宏
> `BITCASK_HIGHLIGHT_LOAD_COLD`；C++ 新增枚举 `search::ColdText`、`Cask::search_text_highlight`
> 四参重载、`TextPlugin::highlight_snippets`（public）与私有 `snippets_for`、`SegmentSet::fresh`
> （头文件内联静态函数）。既有结构体布局、虚函数表、函数签名零改动；盘上格式零改动。
> 按仓库规则不破 ABI ⇒ MINOR，取 **9.1.0**，`SOVERSION` 保持 `9`。

### Fixed

- **打开旧版封段的检索库：新段写进进程当前目录**（S52，下游反馈
  `feedbacks/2026-11-19-old-segments-fallback-writes-cwd.md`，观澜）：旧段集载不进
  （典型：8.1.0 之前封的段默认字段还是旧标识 `\xfa` "ult"，v2 段载入要求 `kDefaultField` ⇒ 拒载，
  这是 8.1.0 定的「不迁移、需重建」；或清单 / 段 CRC 坏）时，`TextPlugin::init_segment_set` 退到
  **默认构造**的段集——`dir_` 为空 ⇒ 全量重放封出的 `seg-N.seg` / `segments.manifest` 是相对路径，
  落进进程当前目录；库目录的段永远是旧的；不报错不记日志。现改为 `SegmentSet::fresh(segs_dir)`：
  新段落在库目录；seg_id 从目录里已有段文件的最大号之后起（避免全活新段与旧 `seg-N.seg.live`
  旁车同名——旧旁车会在下次打开时把活文档标成已删）；目录里原有的段与旁车排进延迟删除，下次
  可写 checkpoint 清掉（只读句柄不动盘）；记一条 warning（`... cannot be loaded ... rebuilding the
  full-text index from data`）。检索结果此前与此后都正确（全量重放本来就在做）。

### Added

- **冷文档也给高亮片段**（S53，下游反馈
  `feedbacks/2026-11-19-highlight-cold-docs-never-get-snippets.md`，keelson 转 bitcask-viewer）：
  `bitcask_search_text_highlight_ex(..., flags, ...)` + `BITCASK_HIGHLIGHT_LOAD_COLD`；C++
  `Cask::search_text_highlight(query, k, opts, ColdText::kLoad)`。片段为空的命中经 `get_doc_view`
  读回正文，再由新的 `TextPlugin::highlight_snippets` 切片段——与原文 LRU 命中同一条路，片段逐字相同。
  正文空取第一个命名字段；读回版本 ≠ 命中版本不补；不回填 LRU；保留 flag 位 ⇒ `INVALID_OPTION`。
  缺省行为不变。

### Docs

- **`bitcask_range_iter_start` 的 `NO_INDEX` 说明勘误**（S54，下游反馈
  `feedbacks/2026-11-19-c-api-range-iter-no-index-comment-stale.md`）：只读打开**不会**得到
  `NO_INDEX`——目录没有 OKI 时 open 期从 data 建一份内存读视图（不写盘），照常迭代；`NO_INDEX`
  只出在 merge-only 句柄（不建读视图）。`INDEX_REBUILD_FAILED` 也覆盖「只读打开建内存读视图失败」。
  改动面：`bitcask_kv.h`（两处）、`cask.hpp`（两处）、`cask_range_iter.cpp` 报文
  （原文把 read-only 写进了 NO_INDEX 的成因）、`doc/api-c.md`、`doc/api-cpp.md`。

### Validation

- 新增 `SegmentFallbackTest.UnreadableSegmentsRebuildInsideTheIndexDir`（9.0.0 实现上红：当前目录被写、
  无 warning、旧段与旁车未清、新段与旧段同号）与 `.FreshLibraryLogsNothing`；
  `HighlightColdTest.{ReopenedLibraryHasNoSnippetsUnlessLoaded, HotSnippetsAreUntouchedByLoad,
  ColdSnippetsMatchHotOnes}`；`OkiRangeTest.ReadOnlyWithoutOkiIteratesMergeOnlyHasNoIndex`；
  C `test_highlight_load_cold`。
- GCC Release 全量 ctest 913/913（2 项 Disabled），编译无新告警（`-Werror`）；相关 61 项在
  ASan / TSan / Clang Debug 全绿。Windows 未实测。

---

## [9.0.0] - 2026-11-19（S50 / S51：merge 后与带过滤的向量检索结果修复）

> **版本语义**：⚠️ **C++ ABI 破坏**——`bm25::DocTable` 新增虚函数
> `fill_eval_meta()`（vtable 变化；带默认实现，外部实现者源码无需改动）。
> `SOVERSION` 升 `9`，取 MAJOR +1（9.0.0），**C++ 下游须重新编译链接**。
> `vec::HnswIndex` 另有纯新增的 `BatchFilter` / `search_filtered()` /
> `kFilterBudgetMul`。C API 签名、C ABI、盘上格式零改动；8.1.0 写出的数据
> 直接可用（下一次 merge 后向量图自动变好）。

### Fixed

- **merge 后向量检索静默返回错结果**（S50，下游反馈
  `feedbacks/2026-11-19-jinshu-merge-corrupts-search-index.md`）：HNSW
  `clone_live` 原来只做死邻过滤，仅「某层邻居全死」才补边。死比一高，0 层
  出度塌缩（死比 75% 时平均出度 26 → 6），极端时图被撕成互不连通的分量——
  merge 之后检索返回错 key、top-K 命中数不足，不报错；f32 与 int8-only 都中。
  现按死比分两路：死比 < 50% 走结构化拷贝 + 全面路径收缩（凡丢了邻居的节点，
  从「活邻 + 穿过死节点链可达的活节点」里按建图同款启发式重新选边）；死比
  ≥ 50% 走全量重连（节点数据与层数保留，邻接按标准插入路径重建）。两路收尾
  都做 0 层从入口的强连通检查，不合格节点补连。
  实测（5000 活节点、dim 32、recall@10、ef=64）：随机删 30% 0.968 → 0.991，
  75% 0.775 → 0.994，90% 0.344 → 0.991；覆盖写 8 轮（每轮向量不同）
  0.168 → 0.994。

- **向量检索带过滤时少返回**（S51）：HNSW 的活过滤 / meta filter 原来在结果侧——
  0 层先取 ef 个候选再滤，死文档占多数或 filter 选择性高时返回数 < k、召回塌陷，
  不报错；默认高维路径（int8 粗筛 + f32 精排）还先截到 3k 再滤，活比 < 1/3 时
  调大 ef 也无效。现改为**遍历期过滤**：被拒节点照常参与导航，但只有通过过滤的
  节点进结果集，凑满 ef 个通过者才收敛。DiskANN 封存段同款处理（过滤在展开时
  求值，凑满 L 个通过者才收敛）。IVF 本就在入堆前过滤，不受影响。
  实测（5000 活节点、dim 64 默认路径、k=10、ef=64）：死比 75% recall 0.615 → 0.954，
  90% 0.300 → 0.956，回不满 k 的查询 237 / 300 条 → 0 条。

### Added

- **`bm25::DocTable::fill_eval_meta()`**（S51-8，⚠️ 新增虚函数）：`eval_meta`
  的批量版，`out[i]` = `ords[i]` 活且通过 filter。`Index` 覆写为一把
  `shared_lock` 判整批；默认实现逐个调 `eval_meta`。带 meta filter 的向量检索
  由此也走批量：2 万节点、dim 64、8 线程，通过率 1/2 时 21.0k → 35.2k QPS，
  通过率 1/8 时 7.3k → 11.0k QPS。
- **`HnswIndex::search_filtered()` 与 `HnswIndex::BatchFilter`**（S51，C++ 头纯新增）：
  批量过滤接口，每展开一个节点把候选攒成一批调一次。遍历期过滤每查询要判几百到
  上千个 ord，逐个 `Index::is_live` 是每次一把 `shared_lock`——8 线程并发查询 QPS
  掉一半以上（136k → 57k）；`VectorPlugin` 与封存段插件的窗口 HNSW 现走
  `fill_is_live` 批量，同条件 130k。原 `search(…, live*)` 签名与语义不变。

### Changed

- **带过滤的向量检索耗时随通过率变化**（S51 的代价）：耗时约 ∝ 1/通过率（dim 32：
  全部通过 32 µs 不变，死比 75% 34 → 136 µs，90% 42 → 380 µs），过滤求值次数约
  3 × ef / 通过率。带过滤时 0 层距离计算有预算上限 ef × 256
  （`HnswIndex::kFilterBudgetMul`；DiskANN 为 L × 256），耗尽即返回已找到的——
  通过率极低时仍可能 < k，但不会把全图扫一遍。
- **高死比 merge 的向量重建变慢**（S50 的代价）：死比 ≥ 50% 时 `clone_live`
  耗时从毫秒级变为「活节点数 × 单点连边耗时」（单线程；dim 32 约 0.1 ms/节点，
  5000 活节点约 0.5 s）。死比 < 50% 时只对丢了邻居的节点算距离（同规模 25～32 ms，
  原 1～3 ms）。

### Validation

- 新增 `HnswCloneConnectivity.RepairAfterHeavyDeadFilter`（下游最小回归）与
  `HnswCloneConnectivity.RecallHoldsAcrossDeadRatios`；两条在 8.1.0 实现上红。
- 新增 `HnswFilteredSearch.FillsKWhenMostNodesRejected`、
  `HnswFilteredSearch.BudgetBoundsWorkWhenNothingPasses`、
  `DiskannSegment.FilterFillsKWhenMostNodesRejected`、
  `DiskannSegment.FilterBudgetBoundsWorkWhenNothingPasses`、
  `HnswFilteredSearch.BatchFilterMatchesPerOrdAndBatches`、
  `Index.FillEvalMetaMatchesEvalMeta`。
- GCC Release 全量 ctest 907/907；向量 / Index / C API 相关 209 项在 ASan / TSan /
  Clang Debug 全绿。Windows 未实测。

---

## [8.1.0] - 2026-11-19（S49：默认字段哨兵改为 `_bitcask:default`）

> **版本语义**：C API 签名、C ABI 与盘上容器格式零改动；
> `SOVERSION` 保持 `8`。取 MINOR +1（8.1.0）。
> ⚠️ **search 数据兼容性破坏**：默认字段哨兵字节序列变化。由 8.0.x
>（或更早）写出的 search checkpoint / 已封存索引在 8.1.0 下不能无损沿用，
> 需重建 search 索引或回用旧版本打开；KV-only 数据不受影响。

### Changed

- **`kDefaultField` 改为 `"\x01" "_bitcask:default"`**：保留不可见首字节
  避免与用户字段名冲突，同时让内部哨兵具备可读前缀。转义拆开书写，
  避免 `\x` 对后续字符的贪婪解析歧义。KV-only 路径与 C API 零改动；
  search 模式下旧库中的默认字段持久化标识不会自动迁移。

### Validation

- `bitcask_text_plugin_test` 24/24、`bitcask_segment_v2_test` 23/23、
  `bitcask_cask_docvalue_test` 109/109 通过（GCC Release）。

---

## [8.0.2] - 2026-11-18（S48：MSVC 异常展开必需项）

> **版本语义**：纯缺陷修复——C API 签名、C ABI、盘上格式零改动；
> `SOVERSION` 保持 `8`。取 PATCH +1（8.0.2）。

### Fixed

- **MSVC 未显式启用异常展开时的 RAII/SeqGate 挂死**（下游反馈
  `feedbacks/2026-11-18-msvc-no-ehsc-raii-not-unwound-seqgate-hangs.md`）：
  MSVC 全局编译项显式加入 `/EHsc`。此前只依赖 CMake 默认 flags；下游嵌入
  或清空 `CMAKE_CXX_FLAGS` 后，异常跨过 `SeqGate::WriteGuard` 时不会析构，
  序号停在奇数，读者会永久自旋。显式 `/EHsc` 保证 RAII 异常安全契约成立，
  并同步保证 oneTBB 的 `_CPPUNWIND` / `TBB_USE_EXCEPTIONS=1` 配置。C API、
  盘上格式与正常路径行为零改动；`SeqGate.WindowReleasedOnException` 为
  必测哨兵。

---

## [8.0.1] - 2026-11-18（S47：Windows TBB finalize 兼容）

> **版本语义**：纯缺陷修复——C API 签名、C ABI、盘上格式零改动；
> `SOVERSION` 保持 `8`。取 PATCH +1（8.0.1）。

### Fixed

- **Windows/MSVC 单参 `tbb::finalize` 编译失败**（下游反馈
  `feedbacks/2026-11-18-windows-tbb-finalize-single-arg-does-not-compile.md`）：
  `TbbLifetime::release()` 改用 `tbb::finalize(*handle_, std::nothrow)`。
  当 MSVC 编译单元未启用异常展开时，oneTBB 会设 `TBB_USE_EXCEPTIONS=0`，
  单参重载不存在；nothrow 双参重载在任何配置下都可用，并与 C API
  shutdown 的既有写法一致。公共 API、行为与盘上格式零改动。

---

## [8.0.0] - 2026-11-18（S46：原子批条件写）

> **版本语义**：C API **纯加法**
>
>（错误码 17 + 新结构 + 新函数），既有 C ABI 零改动；C++ 头面追加成员与带
> 缺省值的形参——源码兼容、C++ ABI 布局变化，下游需重新编译；盘上格式
> 零改动。按发布决定取 MAJOR +1（8.0.0），`SOVERSION=8` 由 major 派生。

### Added

- **原子批前置条件（S46，下游反馈 2026-11-17）**：`Cask::BatchOp` 加
  `expect`（`kNone` / `kAbsent` / `kOrdEquals`）+ `expect_ord`，新 op 型
  `kCheck`（只校验不写）。全部条件在 `write_mu_` 内、任何字节落盘前对照
  批前状态判断；任一不成立 ⇒ 新错误 `CaskError::kConditionFailed`，整批零
  副作用。「不存在」口径与 `get()` 的 kNotFound 一致（含删除、`expiry_secs`
  与 per-key `expiry_at` 过期）。`put_batch_atomic` 新增可空出参
  `BatchOutcome*`：失败时给出首个不成立的 op 下标，成功时给出每个 op 落盘
  记录的 LSN（可直接作为下一轮 `kOrdEquals` 的期望值，不必重读）。
  `TxnOp` / `TxnCask::commit` 同步透传。用途：值为聚合体的读-改-写
  （计数器、邻接表、二级索引）不再需要外部锁——冲突时重读重试即可。
  LSN 契约写进头文件：merge 搬迁原样保留 record 的 ord，重开 / OKI 重建从
  record 读回，key 的 LSN 只在它被写入时改变（回归覆盖 merge 后、重开后、
  删派生缓存全量重建后）。
- **C API（纯加法）**：`BITCASK_ERR_CONDITION_FAILED = 17`；`bitcask_cond_op_t`
  （`op = 3` 为 check；`expect` / `expect_ord`）+ `bitcask_put_batch_atomic_cond` /
  `bitcask_txn_commit_cond`（可空出参 `out_ords` / `out_failed_op`）。既有结构
  与函数零改动，`bitcask_txn_op_ex_t` 仍不认 `op = 3`。条件所需的 ord 由既有
  `bitcask_get_result_t.ord` 提供。
- C++ 头面：`BatchOp` / `TxnOp` 追加成员、`put_batch_atomic` / `TxnCask::commit`
  追加带缺省值的形参——源码兼容，C++ ABI 变化（与 S45 的 C++ 头面变化同批）。
- 回归：`ConditionalBatchTest.*`（10 例，含 8 线程并发计数器自增精确 + 无条件
  对照组确实丢更新）、C `test_conditional_batch`。

---

## [7.0.3] - 2026-11-17（S45：索引管线去全局顺序化 + Windows 文件大小兼容性修复）

> **版本语义**：C API 签名与 C ABI 零改动；盘上格式字节布局零改动（HNSW
> 图文件放宽一条内容校验，见 Changed）；`SOVERSION` 保持 `7`（C ABI 稳定，
> 共享库 soname 不变）。取 PATCH +1（7.0.3）——本仓库的 ABI 基线是 C ABI，
> C++ 头面虽有破坏性变化（`IndexPool` / `IndexTask` / 插件契约 /
> `TextPlugin` / `DataFile::physical_size()` 等），但**下游 C++ 使用者须
> 重新编译**（头文件与库同版本发布），沿用 7.0.1 起的既有口径（patch 版
> 亦含公开头变更）。7.0.3 此前只有提交（`4d3b506`，旧索引 BM25 分数漂移
> 修复）从未发布/打 tag，故那一条一并并入本节定版。设计：
> `designs/index-pipeline-unordered-design-zh.md`；调研：
> `docs/design/s45-1-research-zh.md`；基准：
> `docs/design/s45-6-bench-zh.md`。

### Fixed

- **读旧格式 OKI 索引时 BM25 分数漂移**（下游反馈
  `feedbacks/2026-11-16-legacy-oki-index-score-drift.md`）：恢复期的全量
  data fold 重放会把已封口的旧 SegmentSet 段整体标记为 dead，同时在新的
  building 段重建全部 live 文档。旧段盘上持久化的 BM25 全局统计
  （`N` / `sum_dl` / `df`）仍是封口时快照，查询视图若继续纳入全死段，
  on-the-fly 全局聚合会把 stale 统计注入 BM25，造成 idf 口径错与分数恒比例
  漂移。现在默认字段、多字段与 `SegmentSet::search()` 视图均跳过
  `live_doc_count() == 0` 的段；building 段也改按 live 文档数判断是否参与
  聚合。键、正文、向量与盘上格式不受影响，C API 零改动。回归
  `TextPlugin.RecoveryReplayFullyDeadSegmentKeepsScoresStable`。
- **索引池死锁**（search 模式）：大 `put_batch`（≥ 约 4 万条）与并发 `put`
  交错时，map worker 被 reorder ring 名额挡死、队头 LSN 永远进不了 ring，
  所有写线程永久挂在索引队列上（同 registry 的其它库一起停摆）。在
  S45 之前的基线（`4d3b506`，本版之前标注为 7.0.3）上批 6 万 +
  4 线程 2/2 复现。根因是「乱序提交 + 严格按序应用 + 有上限的
  重排缓冲」三者并存；S45-0 先以饥饿旁路止血，S45-4 起该结构整体退役。
  回归 `IndexLivenessCaskTest.LargePutBatchWithConcurrentPutsCompletes`。
- **`close()` 的 30 秒逃生门不生效**：等待在途写用的是无超时的
  `std::atomic::wait`，写线程被 kill 时 close 永久阻塞。改为有界轮询，
  超时可注入。回归 `CloseWaitTest.*`。
- **merge 搬迁与并发写竞态**：`DocmapRelocator` 的 `get → put_doc` 两步
  之间若应用了同 key 的更新版本，会把 docmap 改回旧版本的定位。新增
  `Index::relocate`（当前版本 == ord 才改定位，原子）。
- **checkpoint 覆盖水位高估**：旧水位取提交时刻的 `peek_next_ord()`，会把
  并发写刚分配、尚未应用的 LSN 计入覆盖。改为执行时刻的 LSN 连续完成前缀。
- **search 模式每次重开都全量重新分词**：恢复重放对同 LSN 文档执行
  「重加 + 标死旧副本」，并在回存 checkpoint 时重写段。现同 LSN 重放幂等
  跳过，且低于各组件覆盖水位的记录直接不重放（20 万文档库重开
  约 1.6 s → 0.6 s）。
- **查询结果缓存陈旧写回**（既有 TOCTOU）：查询在 miss 路径基于旧快照
  计算、写入在计算期间完成失效、随后旧结果才 `put` 进缓存——陈旧结果常驻
  直到下次相关写。真并发应用下实测约 5% 丢 1～2 篇文档
  （`CaskDocValueTest.AutoCompactConcurrentReadersNoRace`）。`SearchCache`
  新增失效代号，计算前取代号、put 时代号已前进即拒收；`on_delete` 的缓存
  失效挪到标死之后。回归 `SearchCacheGeneration.*`。
- 同轮审计次要项随重构消除：`BatchOrdGuard` / `OrdSkipGuard` 析构里提交
  任务可能抛异常（改为直接记 LSN 完成，noexcept）；自动 checkpoint 在
  池已停时任务被丢弃导致 `auto_ckpt_inflight_` 永久卡住（池停时调用线程
  直跑）；`unregister_lib` 超时后析构车道的已知 UAF（改为摘钩入墓地，不再
  回调宿主）。
- **Windows：取不到「本进程正在写的文件」的准确大小**。NTFS 的目录项
  大小在句柄存续期惰性更新，`std::filesystem::file_size()` /
  `directory_iterator::file_size()` 对仍在写入的文件返回陈旧值（实测为 0）。
  受影响的是 `Cask::collect_snapshot_watermarks()`——checkpoint 的持久
  水位被钳到陈旧值（方向安全，但 Windows 上 checkpoint 实际失效、每次
  重开都要全量重放尾部）。新增 `DataFile::physical_size()`（句柄级
  `fstat` / `GetFileSizeEx`，对打开中的文件即时准确），采集点与崩溃注入
  测试缝改用它；Windows 上原先稳定失败的
  `OkiCommitCut.DataWatermarkClampedToAppliedRecords` /
  `IndexCrashRecoveryTest.OverclaimedOkiManifestFallsBackToRebuild`
  随之转绿。另加固 `UnorderedApplyTest.ControlGroupWithoutArbitrationShowsViolations`
  对照组：Windows 默认 timer 粒度约 15 ms 会把 2 ms 提交抖动坍缩到同一
  唤醒 tick（提交序恒等于 LSN 序、对照组采不到违例），按 `SeqGate`
  先例在采不到样本时升级抖动窗口。全量 882 项 Windows 测试转绿。

### Changed

- **索引管线**：reducer、reorder ring、map worker 池、`IndexOp::Skip` /
  `IndexOp::RunFn` 全部移除。新结构：按 key 哈希分发到 N 个分片线程
  （N = `index_workers`，同 key 串行、不同 key 并行），单写者插件走 1 个
  串行道线程，维护操作（checkpoint、压实、HNSW 重建、merge 收尾）在 1 个
  维护线程上按库暂停后独占执行。正确性由「key + LSN 裁决」（OKI 为权威，
  乱序到达的旧版本丢弃）与各结构的 LSN 守卫保证；读屏障与 checkpoint
  水位由新的 `LsnTracker`（LSN 连续完成前缀）给出。
- **插件契约**（`plugin_api.hpp`）：事件**到达序任意**、同 key 串行；新增
  `CaskPlugin::concurrency()`（`kSerial` 默认 / `kSharded`）、
  `accepts_put()` / `accepts_delete()`、`PutEvent::shard` /
  `DeleteEvent::shard` / `PutEvent::replay`（重放亦经执行器）、
  `OpenContext::shards`、`kReplayFloorMargin`。`run_serialized` 改为维护
  线程独占执行。第三方插件默认 `kSerial`，行为最保守。
- **TextPlugin**：BuilderPool 退役，每分片一个 building 段；
  `TextPluginConfig::builder_threads` / `SearchLayerConfig::builder_threads`
  忽略并在 open 时告警一次。
- **向量索引的乱序与重放**：HNSW / IVF / DiskANN 插件改用 key + LSN 定位表裁决
  （该 key 已有 LSN ≥ 本事件即跳过，删除登记墓碑），HNSW 图本身不再判重；串行
  插件一律收到删除事件。定位表随向量检查点持久化（新增段型 `kVecKeyLoc` /
  `kVecKeyLocDelta`），重开时不再依赖 docmap 反查节点 key；旧检查点自动回退。
  修复：docmap 检查点没落成而向量检查点落成时，重开会把当前版本再插一遍。
- **HNSW 图文件**（`vec.ckpt` 的 kHnsw 段）：节点 ord 不再要求随节点 id
  严格递增，头部 max_ord 写真实最大值。字节布局不变；**7.0.x 读端遇到
  S45 写出的乱序图文件会拒收并退回全量重放重建向量索引**（降级、不损坏
  数据）；S45 读端可读旧文件。
- **恢复重放**：在 OKI 收尾之后单独一遍 fold，经分片执行器并行应用；重放同样
  走严格的 key + LSN 裁决（OKI 为终态，只有命中、非墓碑且 LSN 恰好相等的版本
  放行）；墓碑重放改为
  真实删除；从 `replay_floor_of(min(组件覆盖水位))` 起重放（余量 4096 吸收旧版本
  checkpoint 的水位高估）。
- **checkpoint / close 时分片小段先合并再落盘**：各分片的 building 段按文档数
  装箱（每组 ≤ 65 536 篇）合成一个段文件，不再每分片各封一个。实测在线段数
  8 → 1、2 词查询 p50 约 −33%；close / checkpoint 耗时无可测变化。
- **OKI 提交切面高估 data 水位（崩溃后 KV 丢写）**：写路径「追加 data → 应用到
  OKI」之间，OKI flush 的一致切面会看到已追加、未应用的记录，靠「各文件已应用
  落点」钳制水位；但 roll 新建的文件在首条记录应用前没有登记项、不被钳制，
  水位取文件大小，manifest 声明 run 已覆盖其实没进 run 的首批记录——此时崩溃，
  恢复补 tail 据水位跳过，该写丢失（`AutoCheckpointOnRoll` 约 1% 偶发即此）。
  现在新分配的文件登记落点 0；运行期受控重建后以扫描末尾登记（原为清空，同类
  缺口）。
- **打开时校验 OKI manifest 的覆盖声明**：恢复补 tail 时，对每个 data 文件取
  manifest 水位之内的最后一条记录，补完后比对 OKI 与 data 的胜出版本（同一文件
  内记录按追加序应用，末条在 ⟹ 之前都在；每文件一次点查）。不一致即判定
  manifest 高估覆盖，从 data 全量重建 OKI——旧版本写坏的目录打开即自愈。
  诊断接口 `IndexAuthority::last_ledger_view_consistent()` /
  `last_ledger_view_probes()`。
- **OKI 崩溃恢复的墓碑 LSN**：恢复时补 data tail 行，若 OKI 视图里该 key 已是
  更旧的墓碑，tail 中更新的墓碑此前被跳过，`locate` 报偏旧的墓碑 LSN（删除状态
  正确、LSN 错）。现在同样补行，墓碑 LSN 与 data 一致。
- **docmap 检查点**：行与 meta 不再以覆盖水位为上界（写全部已应用行）——修复
  乱序先完成、LSN ≥ 水位的行在重开后丢失 meta。
- **`checkpoint()`**：先有界等待调用时刻之前的 LSN 全部应用，再独占保存；
  执行期间本库暂停应用（写者照常提交，分片队列满后才被背压）。

### Added

- `Cask::verify_index_consistency()`：docmap 逐行对 OKI 当前版本核对
  （诊断用）。`StatusInfo::index_stale_dropped`：被裁决丢弃的过期写任务数。
- `DataFile::physical_size()`：句柄级文件大小（`fstat` / `GetFileSizeEx`），
  对「本进程仍打开写入的文件」即时准确——Windows 上目录元数据（NTFS
  惰性更新）给出陈旧值，不能用于持久水位 floor。

---

## [7.0.2] - 2026-11-16（Windows 平台兼容性修复）

> **版本语义**：纯缺陷修复与构建适配——C API 签名、ABI、盘上格式零改动；
> `SOVERSION` 保持 `7`。取 PATCH +1（7.0.2）。

### Fixed

- **Windows `atomic_rename` 共享冲突**：`MoveFileExW(REPLACE_EXISTING)` 在目标
  文件仍被本库读句柄短暂打开，或目标处于 delete-pending 时返回
  `ERROR_ACCESS_DENIED` / `ERROR_SHARING_VIOLATION`，而 POSIX `rename(2)`
  没有共享冲突语义。现在对这两类瞬时冲突做有界退避重试（约 0.5 s 上限，
  防读者死循环拖死写端）；其他错误仍立即失败。目录项持久性仍由
  `MOVEFILE_WRITE_THROUGH` 承担。

### Changed

- **MSVC 构建适配**：OKI merge cursor 的大端读取从 GCC 内建
  `__builtin_bswap64` 改为标准 C++23 `std::byteswap`，补 `<bit>` 头。
- **测试可移植性**：运行期 `rebuild_from_data` 并发写回归给 Windows
  `std::mutex` 的非公平唤醒留出调度窗口，避免连续 rebuild 把 writer 饿死；
  测试意图与断言不变。

---

## [7.0.1] - 2026-11-16（迭代后写入 flush_index 卡死修复）

> **版本语义**：纯缺陷修复——C API 签名、盘上格式零改动；`SOVERSION` 保持 `7`。
> 取 PATCH +1（7.0.1）。C++ 侧 `IndexAuthority`
> 末尾追加一个成员（`fold_floor_`，既有成员偏移不变）并新增 `fold_epoch()`；
> `advance_fold_epoch()` 由头内联改为库内定义。

### Fixed

- **迭代器用过之后再写一次，`flush_index` 永不返回**（6.9.0 起；下游反馈
  `feedbacks/2026-11-16-flush-index-hangs-after-iter.md`）：`CaskIter::start` 借 ord 序列推进 fold 时代
  （`advance_ord(peek_next_ord())`），每开一次迭代器凭空跳掉一个 ord——既不写 data 也不进索引通道；
  通道的重排缓冲按 ord 严格连续 apply，其后第一次写把 `submitted_ord_hwm` 推过空洞，
  `applied_ord` 再也追不上，`flush_index` 永久阻塞（不报错、不超时）。现在 fold 时代用独立下界：
  `fold_epoch() = max(next_ord, fold_floor_)`，开 fold 只抬下界，**ord 只发给真正的写**。
  下界不落盘，`adopt_manifest` 由 fstats 的 `pending_delete` + `expiration_epoch` 还原。
  `bitcask_iter_start` / `_prefix` / `_ex` 同修；`bitcask_range_iter_start*` 本就不受影响。
  卡过的库无需修复，重开即对齐。回归 `FlushAfterIterTest.*`、C `test_flush_after_iter`。

---

## [7.0.0] - 2026-11-16（S44：OKI 分级归并 + 读视图不可变发布）

> **版本语义（2026-11-16 定版）**：**C++ ABI 破坏 → bump major → `7.0.0`**，
> **`SOVERSION` → `7`**（`.so.7`，下游需重新链接）。
> 破坏点：`StatusInfo` **布局变更**（追加统计字段）、`OkiState` / `OkiState::ReadView` /
> `OkiMergeCursor` **接口与布局变更**、`OkiState::flush()` 去掉 `async_compaction` 参数——
> 均在已安装的公共头 `include/bitcask/*.hpp` 内，按 [6.0.0] 先例
> （结构体布局变更 = ABI 破坏 = bump major）处理。
> C API **函数签名零增删改**，`bitcask_version_*` 随产品版本同步为 7.0.0。
> 盘上格式**零改动**（manifest v4 / run v3），新旧版本可互开同一目录，无 flag-day、无迁移。

### Changed

- **OKI run 归并改为分级后缀归并**（设计稿 `designs/oki-size-tiered-compaction-design-zh.md`）：
  不再「run 数 > 8 就把全部 run 重写」，改为只归并最新的一段连续后缀（几何选段，扇入 K = 1）；
  后缀含最老 run 时才是全归并、才回收墓碑；非全归并保留墓碑，墓碑占比 > 25% 时触发一次全归并。
  底座 < 1M 行的小库仍按旧规则。旧策略保留为 `CompactionMode::kFull`。写放大与耗时对比见
  `docs/design/s44-compaction-baseline-zh.md`。
- **归并全部后台执行**：所有提交点（checkpoint / close / merge 收尾 / 备份 / 阈值 flush）只调度归并；
  两条归并道（大归并在途时，年轻道收拾期间新产生的小 run）；run 数硬上限 24，超限时 flush 背压等待；
  close 给在途归并 500ms 宽限后取消，耗时有上界。`OkiState::flush()` 去掉 `async_compaction` 参数。
- **OKI 读视图整体不可变发布**：run 集合与「已冻结、待落盘」的内存段放进同一个不可变视图整体发布，
  附「到达序号 ≤ frozen_through 的行已全部归入视图」水位；读者先取 active、后取视图并按序号过滤，
  成对一致由构造保证（取代上一版的 `SeqGate` 写窗口）。flush 改为「冻结 → 落盘 → 单次发布」。
- **OKI run 的 bloom 加大**：新写出的 run 由 10 bits / 7 哈希（FP ≈ 0.8%）改为 16 bits / 11 哈希
  （FP ≈ 0.05%）。分级归并下常驻 run 增多，旧参数会让 3%～6% 的点查多读一个块（p99 约 2.3 倍）；改后
  10⁸ 行点查 p99 与全归并持平。参数写在 run 文件里，旧 run / 旧版本 reader 不受影响；bloom 常驻内存 +60%。
- **归并策略细化**：`max_runs` 默认 6（背压阈值另设，默认 2 × max_runs）；硬上限只驱动主道；主道修复
  「新而大」倒挂、被上限扩大后缀后继续几何吸收（修复前两道并发下实测写放大可达 28）。
- **读路径**：归并游标改为堆 + 下沉提前退出 + 前 8 字节整数比较；区间起点定位块走块缓存
  （`OkiRunReader::seek_cached`）；多 run 点查的 bloom 哈希只算一次。读视图携带共享块缓存
  （`ReadView::gens` / `ReadView::cache`）。
- **已知取舍**：10⁸ 行、64K/run 的大库上，分级布局（约 6 个 run）相对单 run（索引驻留页缓存时）：点查 p99
  约 1.3 倍、p50 约 1.4 倍、全量迭代约 0.46 倍、短区间扫描 p99 约 3.7 倍；索引放不进内存时点查尾延迟差距更大
  （未预热实测 p99 约 1.6 倍）。减少 run 数会让写放大急剧上升。见基准报告 §3d–§3f。
- **无快照的 `OkiState::flush(dir, durable_wms)`** 不再「整组持留」引用未持久字节的行，改为与带快照
  路径一致地放弃本轮（生产调用点均带快照；仅影响直调）。

### Added

- `OkiState::compaction_stats()`、`set_compaction_policy()` / `compaction_policy()`、`wait_compaction()`；
  `StatusInfo::oki_runs` / `oki_flushed_rows` / `oki_compactions` / `oki_full_compactions` /
  `oki_young_compactions` / `oki_compaction_rows` / `oki_compaction_tomb_rows` / `oki_compaction_aborts` /
  `oki_compaction_ns` / `oki_backpressure_waits` / `oki_backpressure_ns`。
- `include/bitcask/detail/oki_compaction_policy.hpp`（`choose_compaction` 纯函数）。
- 基准 `bitcask_oki_compaction_bench`；测试 `bitcask_oki_compaction_policy_test`、
  `bitcask_oki_compaction_model_test`（模型对拍、读视图成对对照组、两道归并、取消、背压、并发 load）。

### Fixed

- `OkiState::load()` 读完 manifest 后若 run 文件已被另一进程的归并清掉，重读 manifest 有界重试，
  不再直接退回未加载态（只读句柄 / merge_only 旁车因此不必全量重建内存视图）。

---

## [6.9.1] - 2026-11-16（S43 后续修复：merge-only 可完成 OKI readiness 重建）

> **版本语义**：纯缺陷修复——公共 C++ / C API、盘上格式与 ABI 零改动；
> `SOVERSION` 保持 `6`。取 PATCH +1（6.9.1）。

### Fixed

- **merge-only OKI readiness rebuild**：`ensure_oki_for_write()` 此前把
  `opts_.merge_only` 与「不可写」一并拒绝，导致 merge-only 打开的库在
  OKI 尚未达到写探针 ready 时无法从 data 完成受控重建。现在 merge-only
  也允许重建，并走 `IndexAuthority::rebuild_memory_from_data()` 的轻量
  路径；重建成功后显式启用 point query。健康 data 仍唯一真相，失败时
  继续显式报错，不静默退回旧查找语义。

---

## [6.9.0] - 2026-11-15（S43：OKI-only 权威、data-only recovery 与 KeyDir/hint 退役）

> **版本语义**：内部架构能力收敛与盘上辅助布局变化；公共 C++ / C API
> 既有签名与 ABI 零破坏（`SOVERSION` 保持 `6`）。`data` 仍唯一持久真相，
> OKI 成为查找/提交权威。取 MINOR +1（6.9.0）。

### Added

- **C API 新错误码（纯追加，S43-1）**：`BITCASK_ERR_INDEX_CORRUPT = 16`
  （枚举尾部追加，既有取值不变），对应 C++ `CaskError::kIndexCorrupt`：
  索引定位成功但读回 record 的 key / ord 与请求不一致时显式报错，
  绝不返回别的 key 的 value，也不折叠成 NotFound。
- **Rollout gate 与恢复语义守门（S43-0/10）**：新增
  `S43RolloutGateTest`（随机 reference model、迁移/损坏矩阵、K1-K15
  崩溃窗口）与 `OkiOnlyContractTest`（点查生命周期、data-only reopen、
  atomic batch 语义）。
- **磁盘与内存量化基准（S43-10）**：新增 `BM_Merge_DiskAccounting`
  （data/OKI/hint 物理字节与增量写放大）与 `BM_Oki_OpenRssProbe`
  （健康 manifest 开库 RSS 上界）；`BM_Oki_MemProbe` 增加 1M 档。
  原始 JSON/日志归档在 `bench-results/s43-10/`。

### Changed

- **OKI-only 查找权威（S43）**：`get`、写路径、range/iterator、merge
  relocation、ord/file-id/fstats 都直接走 `OkiState`/`IndexAuthority`；
  内存 KeyDir 与 `KeyDirRegistry` 实现退役，多 Cask 同目录共享、
  write lock 与 multi-lib 线程限制语义保持。坏索引返回显式
  `kIndexCorrupt`，可写读者触发 data-only 受控重建，绝不静默返回
  其他 key 的 value 或折叠成 NotFound。

- **Data-only recovery 与提交点（S43）**：BCOK v3 用 sparse block CRC
  把 100M 元数据冷开从约 2.5 s 降到约 0.414 s；BCOM v4 保存全局
  next_ord/file-id/key 数/fstats 元数据。健康 Level B v4 按 data 字节
  水位 streaming replay；manifest/run 损坏、水位越界或旧 Level A 显式
  fallback/rebuild。K1-K15 写、checkpoint、merge 崩溃窗口由集中
  rollout gate 与专项 recovery/merge 测试守门。

- **迁移与磁盘布局（S43-8/9）**：新库不再写 `.hint` 和
  `kv.keydir.ckpt`；可写打开惰性清理历史 hint。旧库带 hint、v3
  manifest/BCKS 可打开并迁移；目标目录 data-only，旧二进制缺少辅助
  缓存时退慢速 data fold。备份只复制 `.data` + 当前 OKI 提交集。
  回退依赖 data 唯一真相与 v1-v3 manifest reader，而不是重建已退役
  的运行时缓存。

- **Rollout 基线**：本地 i9-13900H Release/LTO 下热点查约
  806 ns/op，冷块 p50/p99 4.57/7.41 µs；100k 均匀写 2.707 µs/op，
  100k Zipf(s=1) 覆盖写 1.599 µs/op；
  snapshot/full-fold 开库 7.10/11.7 ms；20k merge 约 60.7 MiB/s
  （636k rows/s）。冷点查与均匀写退化已量化，接受换取消除
  KeyDir 常驻结构并统一提交集。报告见
  `docs/design/s43-okionly-rollout-report-zh.md`。
- **内存/磁盘账**：100k KV RSS A/B 显示 memdelta 净增从 6.8.1 的
  26.37 MiB 降到 7.60 MiB（-71.2%），rebuild 峰值从 37.14 MiB 降到
  17.60 MiB；20k merge 的 value 增量写放大实测 0.1533，同时历史 hint
  在可写迁移后从 3.05 MiB 清理为 0。

### Removed

- **`.hint` 写路径与历史 hint 辅助缓存（S43-8）**：新库不再生成
  `*.hint`；可写打开惰性清理历史 hint 文件（旧 hint 退化为不可信缓存，
  损坏或缺失都不改变恢复语义）。
- **内存 KeyDir 与 `kv.keydir.ckpt`（S43-9）**：删除 `KeyDir` /
  `KeyDirRegistry` 实现与 checkpoint 写路径；查找、写入、range、
  merge 全部改由 OKI ReadView 裁决，多 Cask 同目录共享改由
  `IndexAuthority` / `IndexRegistry` 承担。

### Validation

- GCC Release 全量 ctest **808/808**；ASan/UBSan 全量 **808/808**；
  TSan 全量 **807 executed / 0 failed**（一项平台 skip；RSS probes
  保持 disabled）；GCC Release `-Werror` 全树干净。
- 新增 `S43RolloutGateTest` 7 项随机模型、迁移/损坏与崩溃窗口验收，
  Release 和 ASan 均 7/7 通过。

---

## [6.8.1] - 2026-11-14（S42：DocMap live-only store，内存随 live docs 而非历史写入增长）

> **版本语义**：纯内部内存结构变更——公共 C++ / C API 与盘上格式零改动；
> docmap checkpoint sidecar 格式逐字节兼容，旧数据目录无需 migration；
> `SearchHit.ord` 仍为全局 LSN；`SOVERSION` 保持 `6`。取 PATCH +1（6.8.1）。

### Changed

- **DocMap Live-Only Store（S42）**：全局 DocMap 从「按历史 ord/LSN 展开的
  chunks + 平坦 live/doc_lens/meta 数组」改为 `ext_id → 最新 ord` 主表加
  「仅当前 live row 的稠密 map」。覆盖写与删除在写路径同步淘汰旧 `ord` 的
  live row、meta 与 delta journal，不再等待 `compact_chunks()`。极端热点 key
  覆盖写的 DocMap 常驻内存从 `O(total writes)` 降到 `O(live docs)`——远端
  1M 次覆盖写、1 个 live doc 的 RSS 净增为 **0.0625 MiB**；100K/1M 低 churn
  live set 分别约 **172.5B**、**169.5B/live doc**。
  - 放弃全局 DocMap 层 SIMD live/doc-len gather：BM25 主评分路径早已使用段内
    dense DocId 与段内 live checker，全局 DocMap 服务 KV 点查、ord/ext 翻译、
    meta 过滤、结果物化与向量 callback。远端 Release 基线：KV 覆盖写
    **578 ns/op**，单线程 BOW 查询 **3.74 µs/query**，快照开库 **2.335 ms**。
  - checkpoint 写入时显式按 `ord` 升序提取；delta checkpoint 增加当前窗口
    live journal，覆盖写淘汰旧 `ord`，热点 key 不会把 journal 写成
    `O(writes)`。恢复重放期间抑制 journal 增长。`compact_chunks()` 成为兼容
    no-op，相关 SIMD kernel 文件删除。
  - 报告与远端基线见 `docs/design/s42-rollout-bench-report-zh.md`；
    功能/内存语义见 `docs/design/s42-docmap-live-store-design-zh.md`。

---

## [6.8.0] - 2026-11-13（字段命中归属 + fields 读回：结构化文档字段不再是纯写侧投影）

> **版本语义**：纯能力新增——C API 纯加法（新结构 + 新函数，既有
> `bitcask_search_fields*` / `bitcask_get` 布局与语义零改动）；C++ 层
> `search_fields` 改为委托 `search_fields_attributed` 后剥离（排序与命中语义不变），
> 读结果末位追加 `fields` 并新增显式文档读入口；盘上格式未动；
> `SOVERSION` 保持 `6`。取 MINOR +1（6.8.0）。

### Added

- **字段命中归属（反馈 2026-11-13 #4）**：`search_fields` 的命中此前统一
  `{key, ord, score}`，回包不说命中的是哪个字段——下游「哪张卡的第几节」只能靠
  派生的节级存储自己回答。补齐：C++ 侧 `SearchFieldHit{key, ord, score, fields}`
  + `TextPlugin::search_fields_attributed` / `Cask::search_fields_attributed`
  （复用 `search_fields` 的逐段逐字段 boost 累加与补取判据；`search_fields` 改为
  委托后剥离，排序语义不变）；`fields` 去重、按字段首次命中序，默认字段
  （text / catch-all，内部哨兵 `kDefaultField`）对外记空串。C API 纯加法：
  `bitcask_search_hit_fields_t` / `bitcask_search_fields_result_t` +
  `bitcask_search_fields_ex` / `bitcask_search_fields_ex_filtered` /
  `bitcask_search_fields_result_free`——既有 `bitcask_search_fields*` 布局与语义
  零改动，SOVERSION 不变。逐字段 snippet 未做（命中归属已可回答「第几节」）。

- **fields 读回（反馈 2026-11-13 #2）**：`put_doc` 的命名字段此前只进 DocValue v2
  与倒排索引，读接口没有任何 get-doc——下游要拿字段原值只能重新解析业务文档。
  补齐：C++ 侧 `DocFieldView` / `DocField`，`GetResultView` / `GetResult` 末位追加
  `fields`，新增 `Cask::get_doc_view()` / `Cask::get_doc_owned()`；`get()` 不解码
  fields、结果恒空，KV 点查热路径零变化。文档读共用定位 / TTL / CRC / merge-retry
  路径，field id 经 `field.schema` 翻译回原名；schema 缺名或 DocValue 解码失败返回
  `kIo`，不静默丢字段。C API 纯加法：`bitcask_doc_field_result_t` /
  `bitcask_get_doc_result_t` + `bitcask_get_doc` /
  `bitcask_get_doc_result_free`——既有 `bitcask_get` 布局与语义零改动，
  SOVERSION 不变。append-only 语义不变：覆盖写仍是全量 fields 替换。

---

## [6.7.1] - 2026-10-12（Windows：merge 后 `bitcask_close` 正确删除退休文件，盘上体积不再滞留）

> **版本语义**：纯缺陷修复——C API 零改动（无新符号、无结构体变化）；C++ 层只动
> `Cask::close()` 内部次序并加一条错误日志；盘上格式未动；`SOVERSION` 保持 `6`。
> 取 PATCH +1（6.7.1）。

### Fixed

- **Windows 上 merge 之后 `bitcask_close` 不删退休数据文件，盘上体积不降**（🟠 结果错，
  不报错）。根因：`close()` 的 `drain_retired_files()` 排在释放读句柄缓存
  （`read_files_.clear()`）之前——要删的文件仍被 mmap 映射占用，Windows 上
  `DeleteFileW` 对被映射文件必然失败，失败路径把文件放回退休队列，析构时被静默丢弃；
  Linux/glibc 上 `unlink` 对映射文件本就成功，故本机测不出（本机同形态原代码即绿，
  病因「句柄占用」未经实测证实）。
  - 修复：排水挪到释放读句柄缓存之后；排水后仍滞留队列的路径记一条 `log_error`
    （数量 + 原因），不再静默丢。`checkpoint()` 入口与 merge 开头的排水经核对前序已
    正确（merge 已先摘掉输入句柄），未动。
  - 回归 `CaskDocValueTest.MergeThenCloseAfterSearchRemovesRetiredFiles`（开搜索、
    4 轮覆盖写、搜索、merge、close，断言退休输入文件消失）。
  - 来源：下游锦书（C#，经 Keelson.Bitcask 1.0.6，Windows x64）
    `feedbacks/2026-10-10-windows-merge-retired-files-not-deleted-on-close.md`；
    锦书侧需在 Windows 上复验 `RagIndexTests.Compaction`。

---

## [6.7.0] - 2026-10-08（`bitcask_shutdown`：拆除进程级后台线程供卸载动态库 · C API 内部助手不再泄进动态符号表）

> **版本语义**：C API **纯加法**（1 个新符号 `bitcask_shutdown` + `bitcask_error_t`
> 末尾加值 `BITCASK_ERR_BUSY = 15`，既有函数签名与结构体布局零改动）；C++ 层只加
> 内部成员函数；盘上格式未动；`SOVERSION` 保持 `6`。按维护者决定取 MINOR +1（6.7.0）。

### Added

- **`bitcask_shutdown`：拆除进程级后台线程，供宿主卸载动态库**。动机：经 FFM / P/Invoke /
  `dlopen` 嵌入的宿主要在进程存活期间卸载本库，而索引池线程、Search 池（`task_arena`）
  与 TBB worker 常驻、C API 原本没有拆除口子（`TbbLifetime` 只给 NIF 的
  `on_load` / `on_unload` 用）。实测 Linux/glibc 上不拆直接 `dlclose`，库驻留不卸
  （其它平台有崩溃风险）；调用后 `dlclose` 真正卸载。
  - 行为：仍有库打开 → `BUSY`、零副作用；否则依次停索引池（join）、`task_arena::terminate`、
    `tbb::finalize`（nothrow；仍有其它 public 引用时 → `BUSY`）。无论成败库仍可用，
    各池按冻结的线程数上限懒重建。
  - 限制：除调用线程外，曾进入过本库 TBB 并行段的线程须已退出（TBB 线程局部状态只在
    线程退出时释放）——宿主线程池里跑过查询的常驻线程会令其返回 `BUSY`；宿主自己也在用
    同一份 TBB 时同样返回 `BUSY`。详见 `doc/api-c.md` §8.3。
  - 内部：`KeyDirRegistry::stop_index_pool_if_idle()`（检查与停池同锁，不与 acquire
    竞态）、`search::release_search_arena()`（池从未建过则不建，免得冻结线程数上限）。
  - `BITCASK_ERR_BUSY` 是首个**仅 C 侧**的错误码（无 `CaskError` 对应）；绑定层的错误码
    枚举需补这一项。
  - 回归 `CApiShutdownTest.*`（独占可执行文件：未初始化即成功 / 有库打开 → BUSY 且库照常
    可用 / 全关后成功且线程数回到基线 / shutdown 后可再开再查 / 另一存活线程持 TBB 状态
    → BUSY、其退出后成功）。

### Fixed

- **4 个 C API 内部助手不再泄进 `libbitcask.so` 动态符号表**：`put_doc_common` /
  `to_cpp_doc` / `to_cpp_batch_ops` / `set_invalid`。它们本已在匿名 namespace 里，
  但处于 `bitcask_kv.cpp` 的 `extern "C" {}` 块内——C 链接的函数即便在匿名 namespace
  中，GCC 仍给外部链接，于是以裸名导出（污染全局符号表，宿主或其它库同名符号可能被
  错绑）。现包一层 `extern "C++"`（同文件 `fill_ex` 一段的既有做法）。`nm -D` 核对：
  非 `bitcask_*` 导出 24 → 20，`bitcask_*` 导出集合不变（另加本版的 `bitcask_shutdown`）。
  这 4 个符号不在任何头文件中，不构成 ABI 承诺，移除不影响 `SOVERSION`。

---

## [6.6.2] - 2026-10-06（原子批 / `txn_commit` 收结构化文档 · 越界 op 值拒收 · merge race 重写路径复用预编码 value）

> **版本语义**：C API **纯加法**（2 个新符号 + 1 个新结构体，`bitcask_txn_op_t` 与
> 既有函数零改动）；C++ 层 `Cask::BatchOp` / `TxnOp` 枚举末尾加值 + 末位加带默认值
> 的成员（同 6.6.1 `Entry::meta` 口径）；盘上格式未动（批成员本就是普通 `kDoc`
> 记录，恢复 / merge / hint 零特判）；`SOVERSION` 保持 `6`。**按维护者决定取
> PATCH +1（6.6.2），而非按 semver 的 MINOR（6.7.0）**——同 6.6.1 的口径，延续
> 同一批下游的 pin 序列；新符号只被下游新代码引用，对既有二进制零风险。此处显式
> 记下这处偏离，不静默改口径。
>
> 来源：下游 bitcask（Erlang 封装）`feedbacks/2026-10-06-atomic-batch-doc-meta.md`。

### Added

- **原子批 / 事务收结构化文档**（🟡 能力缺口）。原先 `BatchOp` 只有 key + value，
  value 一律按 text 编码，带 meta / vector / fields / expiry_at 的文档进不了原子批——
  维护 meta 二级索引的下游无法让「文档 + 新索引项 + 删旧索引项」同批落盘，只能拆三步
  写、加 key 锁、读时回表校验兜底。
  - C++：`Cask::BatchOp::Type::kPutDoc` + `const DocInput* doc`；`TxnOp` 同步加
    `kPutDoc` / `doc`（`TxnCask::commit` 照常做事务级校验）。批内文档的编码、向量
    归一化（cosine 存储即归一化值）、字段 intern、索引登记（text / fields / vector /
    meta）与 `put_doc` 逐项相同，`expiry_at` 同样生效。二者共用新抽出的
    `make_doc_add_task`，不会各自漂移。
  - C：`bitcask_txn_op_ex_t`（多 `const bitcask_doc_input_ex_t* doc`，`op = 2` =
    put_doc）+ `bitcask_put_batch_atomic_ex` / `bitcask_txn_commit_ex`。
  - 校验：`doc == nullptr` / 向量维度错 / 未知 op 类型 → `kInvalidOption`，**零副作用**
    ——向量校验在前，字段 intern（会追加 `field.schema`）推迟到全批校验通过之后，被拒
    批的新字段名不入注册表；meta 不懒升 v6。原先 C++ 直调传入越界 op 值（C 侧已挡）会按墓碑
    落盘、却按 put 进 keydir，前后不一致；现一并拒绝。
  - 顺带修正：merge race 单条重写路径改为复用预编码 value，不再按 text 重新编码
    （否则 kPutDoc 在该罕见路径上会丢 meta / vector / fields）。
  - 回归 `AtomicBatchDocTest.*`（与 put_doc 存储逐字段相同 + 全量 fold 后索引仍在；
    meta 改色同批完成；撕裂批下文档与索引项在 keydir 与检索两侧一起不可见——崩溃盘面
    取自库未关闭时的目录拷贝，变异验证：不掐尾则四项断言全反转；校验零副作用；
    批内 expiry_at 与 `TxnCask` 路径）/ C 侧 `test_txn` 扩展。

---

## [6.6.1] - 2026-10-06（meta filter：补取到 k · 同分 top-K 前缀稳定 · 全部文本检索收 filter · 迭代器交出 meta / 按 meta 筛选 · keydir 乐观读 TSan data race）

> **版本语义**：C API **纯加法**（14 个新符号 + 2 个新结构体，既有函数签名与
> 结构体布局都没动，ABI 向后兼容）；C++ 层只加带默认值的末位形参 / 新成员；盘上
> 格式未动；`SOVERSION` 保持 `6`。**按维护者决定取 PATCH +1（6.6.1），而非按
> semver 的 MINOR（6.7.0）**——本批主体是结果错的修复，Added 里的新 API 只被
> 下游新代码引用，对既有二进制零风险。此处显式记下这处偏离，不静默改口径。
>
> 来源：下游 bitcask（Erlang 封装）`feedbacks/2026-10-06-meta-filter-query-gaps.md`
> 五条账；另销 `TASK.md` S37-4 复验里挂着的 TSan 预存豁免项
> （`KeyDirOptimisticRead.ConcurrentGetPutRemoveGrowStress`）。

### Fixed

- **带 meta filter 的检索不再静默少返回**（反馈第 1 条，🔴）。原先 filter 非空时
  只多取一轮固定 `max(k×4, 64)` 个候选再后过滤，候选不够就直接少返回：1000 篇里
  100 篇满足 filter，`search_text(K=10)` 只回 6 条，K=50 回 20 条。现改为循环补取——
  命中不足 k 时把候选数翻倍重取，直到凑满 k 或候选穷尽（候选数 < 请求数）。
  返回少于 k 条即「满足条件的就这么多」。查询缓存键本就含候选数，各轮各占一条
  缓存，不互相污染。同一循环也兜住了无 filter 时段级判活漏掉宿主墓碑导致的缺额
  （正常情况首轮即满足，不多付成本）。
  影响面：`search_text` / `search_text_batch` / `search_hybrid` 的文本路
  （`HybridSearcher` 经 `search_text` 拿 K' 条通过 filter 的命中）、带 `offset` 的分页。
- **同分命中：小 K 结果不再与大 K 结果矛盾，offset 分页不再翻出重复页**（反馈第 5 条，🔴）。
  原先三处平局规则互不一致：段内内核的 top-k 堆是 `std::greater` 小顶堆——同分时
  「收」按先到先留（严格 `>` 才换），「逐」却按最小 ord 先出；跨段归并按 key 升序截取；
  `TextPlugin` 输出又重排为同分 ord 降序。1000 篇全同文本：K=5 回 d5..d1，K=1000 时 d1
  排最后；offset 分页每页都是同一组。
  现统一为全序 **(分数降序, 段次序, 段内 docid 升序)**（同 Lucene 的 docid 平局）：
  - 内核：堆比较器改为「更优」序，堆顶 = 最差者（最低分、同分 docid 最大）
    （`detail::TopKHeap` / `topk_offer`，7 处堆统一）。全部调用点按 ord 严格升序
    递入候选（DAAT / 升序候选集），同分后到者在此全序下必败，故**准入仍是「严格
    高分才换」、只有逐出侧换比较器**；剪枝 `上界 <= θ` 跳过的也只会是 docid 更大、
    同分必败的文档——**剪枝条件不变**。准入若改用全序比较，大量同分的穷举路径
    实测 +5~15%，故不用。
  - 跨段：`multi_segment_search` / `multi_field_segment_search` / `TextPlugin` 逐段并集
    改为按分数**稳定**排序（段内已是全序、按视图次序拼接），不再按 key / ord 重排；
    `search_fields` 的累加结果带 (段次序, docid) 平局键。
  - 未选 LSN 作平局键：段内 docid 与 LSN 不单调（DWPT 多 building 段交错、段合并按
    输入序拼接、fold 重放按 keydir 序），用它须放弃 `<= θ` 剪枝。
  - 评估过并否决「TextPlugin 层边界同分补取到整组取全」：结果同样精确，但无 filter
    查询实测慢 1.8×~4.9×（单词查询同分组上千篇）。
  - 性能（同机 A/B 对 HEAD，5 次中位数）：BOW / WAND / MaxScore / 短语 / 布尔 / mmap 段 /
    hybrid / 高亮共 19 项内核 bench 在 −6%~+1% 内；20 万文档端到端 `search_text`
    无 filter 832→820 µs、短标题语料 148→155 µs（噪声内）。
  **可见变化**：同分命中的先后由「ord 降序」变为「段次序、段内写入序」（大体是先写
  入者在前）；同分序在两次封口 / 段合并之间稳定（段与 docid 随之重排）。多字段 /
  多 boost 组的 `search_fields` 是逐字段 top-k 后求和的近似，前缀性不作保证（既有近似，
  与平局无关）；hybrid 的 RRF 融合平局规则（ord 小者在前）未动。
  回归：`MaxScore.TopKTiedScoresPrefixStableAllKernels`（BOW / WAND / MaxScore / 短语 /
  扁平与树形布尔，同分前缀性）、`MetaFilterQueryTest.TiedScoresPrefixStableAndPageable`
  （反馈原场景，单段 / 多段）；三方穷举对拍的参照实现改为「全量打分 + 全序排序取前 k」
  的规范定义（不再模拟堆语义），WAND / MaxScore 与之逐位一致。

- **keydir 乐观读在 TSan 下报 data race**（`KeyDirOptimisticRead.ConcurrentGetPutRemoveGrowStress`
  稳定复现，每轮三四十条）。两处成因，均在 `SeqShardTable::try_get_optimistic`：
  - 读 `values_.data()` / `values_.size()`——对 `std::vector` 内部指针的非原子并发读
    （严格说是 UB），且 libstdc++ 成员函数非内联时被插桩、不继承读函数的
    `no_sanitize`。改为写者在最外层 `WriteSection` 收尾时把 (data, size) 发布到两个
    原子（`pub_vdata_` / `pub_vsize_`），乐观读者只读原子副本，由 seq 校验背书。
  - `opt_copy_bytes` 的 `__atomic_load_n(relaxed)`：原子操作恒经 TSan 运行时、不受函数级
    豁免，与写者的普通写（桶 memcpy、string / variant 移动赋值）配成「原子读 × 非原子写」
    必报。写侧是标准库类型整体移动，无法逐字原子化；故仅 TSan 构建下改用
    `may_alias` + `volatile` 普通读（豁免函数内不插桩），普通构建照旧原子读。
  修后 TSan 全量 810/810、该用例连跑零告警；keydir 读 / 写 / 混合 bench 同机 A/B 在 ±1.5% 内。

### Added

- **`search_phrase` / `bool_search` / `search_fields` / `search_near` /
  `search_fuzzy` / `search_wildcard` 收 `const meta::MetaFilter* filter = nullptr`**
  （反馈第 3 条）：`Cask`、`text::Searcher`、`TextPlugin` 三层都加在末位，语义与补取
  策略同 `search_text`。`search_fields` 原先先截到 k 再判活，判活丢掉的也会缺额，
  一并纳入补取。
- **迭代器可交出 meta**（反馈第 2 条）：`CaskIter::Entry` / `CaskRangeIter::Entry`
  新增 `meta`（原样 blob，不解码）。`CaskIter::set_want_meta(true)` /
  `RangeOptions::want_meta = true` 时才填，默认不付拷贝。meta 与 value 出自同一次
  读——下游不必再逐行补 `get`，也不会拿到 A 版本 text 配 B 版本 meta。
- **迭代器按 meta 筛选**（反馈第 4 条）：`CaskIter::set_filter(&f)` /
  `RangeOptions::filter = &f`，C++ 侧逐条 `MetaFilter::evaluate`，不通过的连 value
  都不拷。无 meta 的不通过（与检索侧同语义）；`see_tombstones=true` 时墓碑照常交出。
  合用即「只按 meta 筛选的有序扫描」：`make_range_iter({.lo, .hi, .want_meta = true,
  .filter = &f})`；无 OKI 的句柄用 `CaskIter` 全表版。
  `CaskRangeIter` 取值改走零拷贝 `get` 视图（原 `get_owned` 先整份拷出），filter 在
  视图上求值。
- **C API 跟进 filter 与 want_meta**（纯加法）：
  - 检索：`bitcask_search_phrase_filtered` / `bitcask_bool_search_filtered`（兼收 `offset`）、
    `bitcask_search_fields_filtered` / `_near_filtered` / `_fuzzy_filtered` / `_wildcard_filtered`。
  - 迭代：`bitcask_iter_start_ex` / `bitcask_range_iter_start_ex` 收 `want_meta` + `filter`
    （过滤树由迭代器句柄持有）；新结构体 `bitcask_iter_entry_ex_t` / `bitcask_range_entry_ex_t`
    （`base` 内嵌既有条目 + `meta` 切片）配 `*_next_ex` / `*_next_batch_ex` / `*_entry_ex_free`。
    既有条目结构由调用方分配、布局不能改，故走新类型。
  - 既有 `bitcask_iter_start_prefix` / `bitcask_range_iter_start` 改为转调 `_ex`（行为不变）。

---

## [6.6.0] - 2026-09-23（search 库开库内存 + 进程级线程数上限）

> **版本语义**：C API **纯加法**（4 个新符号 + 1 个新结构体，既有函数签名与
> `bitcask_options_t` 布局都没动）；盘上格式只有一处**向后兼容的追加**：`bitcask.meta`
> 可选尾段（analyzer 指纹，旧二进制透明，见下）。C++ 层 `index::Index::for_each_live*`
> 回调的 ext 参数改为 `std::string_view`（见下）。MINOR +1，**`SOVERSION` 保持 `6`**。
>
> 来源：下游 keel 转来 coxswain 的两条账（2026-09-22）：
> `feedbacks/2026-09-23-search-open-resident-memory-no-c-api-knob.md`（`enable_search`
> 库一打开就常驻 ≈ 盘上 1.8 倍，336 MB ⇒ 峰值 610 MB）与
> `feedbacks/2026-09-23-c-api-no-search-thread-count-knob.md`（C API 收不了线程数）。

### Changed

- **BM25 v2 段开库校验不再把整个段扫进工作集**。`MmapSegment::open` 的逐节 CRC
  原先直接扫 mmap 映射——映射页一经触碰就算进本进程工作集，校验一遍 = 整个
  `bm25_segments/` 常驻（报账的库 104 MB）。现改为分块定位读（1 MiB 缓冲，
  数据走 OS 文件缓存），校验强度不变；映射只留给查询路径按需触页。
- **checkpoint 读取不再整份读进缓冲**。`SearchCheckpoint::read` 原先「整文件读进
  一个 buffer，再把每段 assign 出一份 owned payload」——瞬时峰值其实是
  **2 ×** 文件大小（报账估的是 1 ×）。现拆出 `SearchCheckpoint::open`（只解析页脚
  目录、句柄留着），`read` / `read_selected` 改为按目录逐段定位读，峰值降为 1 ×，
  惠及全部组件 ckpt（text / vector / docmap / 段清单）。
- **docmap 行段与 keydir 快照改为流式载入**。`load_docmap` 走 `open` + 新重载
  `Index::deserialize_docmap(fd, off, len)`（分块校验段 CRC 与内层 CRC，再分块
  解析）；`KeyDir::load_snapshot` 同样分块校验 + 分块解析。两处开库时堆上只剩
  一块 1 MiB 缓冲（报账的库此前瞬时 ≈ 60 MB + 7 MB，按上条实为约两倍）。
  解析逻辑不变：docmap 行解析对游标模板化，span 版（legacy ckpt）与流式版共用；
  keydir 的 `SnapCursor` 换成同形接口的 `detail::StreamCursor`。
  docmap ckpt 里不被消费的扩展段，其 CRC 仍照旧校验（坏了照旧退 fold）。

- **检索 key 常驻由四份降为两份（S40，`docs/design/s40-key-single-instance.md`）**。
  enable_search 库里同一批 key 原本住四份（报账记三份，漏了 docmap 的 ord→ext
  反查表）：keydir、docmap `ext2ord_`、docmap `ord2ext`、TextPlugin
  `key_to_location_`。现后两份改为 `string_view`——`ord2ext` 指向 `ext2ord_`
  的节点键，`key_to_location_` 指向段内 key 存储（mmap 段直指映射区、内存段指
  `keys_`，由条目持有的段 shared_ptr 钉住），墓碑键单独存一份并在键重新写入时回收。
  实测（1M 文档开库，堆 + mmap 大块）：20 字节 key 开库堆 565.8 → 455.3 MB
  （**−110.5 MB / −19.5%**），12 字节 key 418.8 → 384.6 MB（**−34.2 MB / −8.2%**），
  RSS 同步下降。开库重建 `key_to_location_` 与写 / 删热路径不再构造 key 字符串。
  写入 / 开库 / 检索 bench 中位数与改前差异在 ±1% 内（多线程 builder 写入项受
  机器噪声影响约 ±15%，两轮方向相反，未见可检测回退）。
- **`Index::ord_to_ext` 对已删 / 被覆盖的 ord 返回 `nullopt`**，兑现 `DocTable`
  契约（此前返回残留的旧 key）。可见影响仅在「检索判活 → 物化 key」之间被并发
  删除 / 覆盖的命中（高亮检索、向量检索）：此前返回过期 key，现丢弃该命中。
- **核心路径文件句柄一律 `O_CLOEXEC`（行为变化）**：data / hint / `write.lock` /
  OKI run / keydir 快照 / `field.schema` 追加句柄，以及经 `fopen_utf8` 打开的
  ckpt、原子写临时文件、目录 fsync 句柄。此前宿主 fork+exec 子进程时，子进程会
  继承这些 fd：拖住已删除数据文件的磁盘空间、能读到库内容、延长锁文件句柄的
  生命期。bm25 段与向量插件此前已带。若宿主有意让子进程继承库 fd（不应如此），
  现在继承不到了。Windows 句柄本就默认不继承，无变化。
- **`Index::remove` 删除日志不再拷贝 key**：命中时从 `ext2ord_` 节点直接 move 出来。
- **HNSW `search_layer` / `greedy_closest` 的 f32 与 int8 两份手写实现合并为一个
  模板主循环**（距离函数与预取目标作参数，内联后同码）。`BM_Hnsw_RecallQps` 4 档新旧
  交替：召回逐位相同，查询延迟中位数 −5.0%～0%，建图吞吐 +1～2%。
- **C++：`index::Index::for_each_live` / `for_each_live_in` 回调第二参数由
  `const std::string&` 改为 `std::string_view`**（仅在回调内有效）。写成
  `const std::string&` 的回调改为 `std::string_view` 或 `const auto&` 即可。
  C API 不受影响。

### Added

- **analyzer 配置指纹告警**：建索引时把影响切词的 analyzer 配置（分词器类型、
  n-gram 范围、停用词、token 长度上下限、词干化）的指纹写进 `bitcask.meta`
  可选尾段；之后以不同配置重开时 `kWarn` 告警（仍然开库）。失败形态与 ICU 版本
  告警相同：换了分词配置，新写入的文档和查询就与老文档切出不同的 term，
  互相搜不到，且没有任何报错。尾段自带 CRC，旧二进制读写都不受影响（旧二进制
  重写 meta 会丢掉尾段，退化为「未记录」，不再告警）；C1 之前建的目录未记录，
  不告警，也不补记。布局见 `doc/format-zh.md` §3.1b。
- **`bitcask_set_thread_limits(index_workers, search_slots, fault)`**：进程级线程数
  上限（两处线程池本来就是进程共享的）。`index_workers` = 索引池 map worker 数；
  `search_slots` = 批量查询 `task_arena` 槽数，非 0 时顺带用 `tbb::global_control`
  把本库经 TBB 跑的并行段（恢复期批内 prepare、查询内 parallel_for）封到
  `search_slots - 1` 条 worker。0 = 缺省（`hardware_concurrency`，即旧行为）。
  须在首个 search 库 open 之前调；之后值不同 → `BITCASK_ERR_INVALID_OPTION`
  （detail 注明生效值），值相同 → OK。C++ 侧为 `bitcask/thread_limits.hpp`。
- **`bitcask_open_tuning_t` + `bitcask_open_tuning_init` + `bitcask_open_ex2`**：
  开库调优，首格 `segment_verify_crc`（缺省 1；0 = BM25 段只验页脚 / 目录，信任盘上
  节内容——省掉开库整读段目录的 I/O）。结构体带 `struct_size`，以后尾部追加字段
  不再开新入口；未经 init（`struct_size` 不够）→ `INVALID_OPTION`，不碰盘。
  `bitcask_open_ex` 改为 `bitcask_open_ex2(…, NULL, …)`，行为逐字节不变。
- 测试：`ThreadLimitsTest.SetBeforeFirstSearchOpenCapsIndexPoolThenFreezes`
  （独占可执行文件：冻结是进程级不可逆的；含开库线程增量断言）；
  `c_api_test.c` 新用例 `test_thread_limits_and_tuning`（排在所有 search 用例之前，
  其后全部 C API 用例都在 (2, 2) 的小池下跑）；S40：`Index.OrdToExtDeadOrdReturnsNullopt`、
  `TextPlugin.S40TombKeyLifecycle`、`TextPlugin.S40RekeyAcrossSealMergeNoDangling`；
  `MetaAnalyzerFpTest.*`（8 项：记录 / 不记录保持 18 字节 / 同配置静默 / 异配置告警
  且仍开库 / 无关字段不改指纹 / 尾段截断与损坏降级 / 重写 meta 保留指纹）；
  `ProcessIsolationTest.*`（exec-self 真实子进程：cloexec 探针自检、开库状态下子进程
  继承不到任何库 fd、第二个进程争写锁得 `kWriteLocked`、持锁进程崩溃后 stale 锁被
  接管）；`Index.RemoveLogsKeyOnHitAndMiss`；手动探针
  `CheckpointRecoveryTest.DISABLED_S40KeyRssProbe`（两进程协议测开库常驻）。
- CI：`build-test` 追加 `BITCASK_SIMD_MAX=scalar/sse42/avx2` 强制降档全量；新增
  `avx512-sde`（Intel SDE 仿真 Sapphire Rapids，首次执行 AVX-512 / AVX512-VNNI 内核）
  与 `windows-msvc`（MSVC Release 全量），后两者首次跑绿前为非门控。

## [6.5.0] - 2026-09-18（OKI：memdelta 排序视图缓存——拔掉「装载后 range 静默变慢 4200×」的性能悬崖）

> **版本语义**：C API **一个字节都没动**；盘上格式零改动；C++ 层
> `StatusInfo` 尾部追加两个字段（`oki_delta_rows` / `oki_delta_bytes`，
> 纯加法）。MINOR +1，**`SOVERSION` 保持 `6`**。
>
> 来源：下游 bitcask 的反馈账（`feedbacks/2026-09-18-oki-unflushed-memdelta-range-query-4000x-slower.md`）：
> 批量装载后开始服务的负载，每次
> `make_range_iter` 固定付出与未 flush 写入量成正比的时间——5 万行实测
> ~120ms/次（flush 后 ~0.03ms，4200×），不报任何错。

### Fixed

- **OKI range 视图构建不再逐查询重排 memdelta**。`OkiState::make_read_view`
  原先每次调用都对 memdelta 全量深拷贝（每行一次 string 堆分配，持 `mu_`
  阻塞 append）+ `stable_sort` + 同 key 去重，成本与查询次数相乘；现排序
  去重快照缓存在 `OkiState` 内，以 memdelta 变更代数（`delta_version_`）
  失效——**写后首查重建一次，无写时的连续查询降为一次 `shared_ptr` 拷贝**。
  发布时若期间有写则弃存（持旧快照照常返回），per-key 弱一致语义不变。
  锁序恒为 `flush_mu_ → mu_ → view_mu_`；`ReadView.delta` 随之改为
  `shared_ptr<const>`（命中路径零深拷贝）。
- 文档勘误（`feedbacks/2026-09-18-ord-recycling-doc-exhaustion-years-off-by-10-7.md`）：
  `ord-recycling-design-zh.md` §1.1/§12.1 的 ord 耗尽年数 `5.8 × 10¹³ 年`
  实为 `5.8 × 10⁶ 年（约 585 万年）`（秒数误作年）；同段 32-bit「约 12 天」
  实为约 11.9 小时（@10⁵ ord/s）。两处已改并补换算。

### Added

- `StatusInfo.oki_delta_rows` / `StatusInfo.oki_delta_bytes`（`Cask::status()`）：
  OKI memdelta 未 flush 的行数/字节。range 首查视图构建的最坏成本上界——
  装载后 range 变慢时先看这里（`doc/api-cpp.md` §4.6 已补用法说明）。
- 回归测试：`OkiRangeTest.SortedViewCacheInvalidatesOnWriteAndStaysFast`
  （写后失效 × 影子三方对拍）与
  `OkiRangeTest.UnflushedMemdeltaRepeatedRangeNotLinearPerQuery`
  （5 万行未 flush × 1000 次窄窗口 range，悬崖探针）。

## [6.4.0] - 2026-09-14（C API：merge 策略 / 显式文件表 / 手动 checkpoint 三个口子）

> **版本语义**：C API **纯加法**（4 个新符号 + 1 个新结构体，既有函数一个签名都
> 没动，`bitcask_options_t` 布局不动）；盘上格式零改动；C++ 层 `Cask::merge(files)`
> 对**显式**文件表多两道拒绝（此前静默跳过 / 静默并 active 文件，见下）。
> 纯加法 → MINOR +1，**`SOVERSION` 保持 `6`**。
>
> 来源：下游 keel 转来的锦书账（keel `feedbacks/2026-09-13-jinshu-bitcask-merge-policy.md`）：
> 「不是 bug，是调不到的旋钮」——一本书一个 bitcask、索引几 MB 到几十 MB，
> 缺省 2 GiB `max_file_size` 下一辈子住在一个 active 文件里，每次压缩都得先
> `close_write_file`；缺省 60% 碎片才触发 ⇒ 盘上占用上界 ≈ 2.5 × 活数据，
> 桌面写作软件想收到 30–40%；`needs_merge` 说不需要就什么都劝不动。
> （`max_file_size` C API 本来就有，是 keel 那一侧没接；其余三格是这里补的。）

### Added

- **`bitcask_merge_policy_t` + `bitcask_merge_policy_init` + `bitcask_open_ex`**：
  `bitcask::merge::PolicyOptions` 逐字段映射进 C API（9 格），`init` 填的缺省
  **从 C++ 结构体抄**、不手写数字。`policy == NULL` 与 `bitcask_open` 逐字节等价
  ——`bitcask_open` 本身改成了 `bitcask_open_ex(…, NULL, …)`。百分比三格越界
  `[0, 100]` → `BITCASK_ERR_INVALID_OPTION`，`detail` 点名哪格，不碰盘。
  ⚠️ 为什么不追加进 `bitcask_options_t`：改布局 = ABI 破坏 = major bump
  （6.0.0 `keydir_cache_entries` 先例）；独立结构体是纯加法。
- **`bitcask_merge_files(cask, files, count, fault)`**：在调用方指定的 data 文件上
  跑 merge（C++ `Cask::merge(files)` 那一支，此前 C API 只有空表）。
  ⚠️ `files == NULL` / `count == 0` **等价于 `bitcask_merge`**（上游语义：空表 =
  自动），不是「并零个」。`files[i]` 为 NULL / 空串 → `INVALID_OPTION`。
- **`bitcask_checkpoint(cask, fault)`**：C++ `Cask::checkpoint()` 接出来——keydir
  快照 + `search.ckpt` 主动落盘，⭐ 顺带把上一轮 merge 退休的输入文件此刻删掉
  （此前 C API 只能靠 `bitcask_close` 或下一次 merge 回收）。只读 / merge-only →
  `BITCASK_ERR_READ_ONLY`。
- `tests/c_api_test.c` 新用例 `test_merge_policy_files_checkpoint`（10 步）。
  ⚠️ 顺带发现：本文件既有用例把**有副作用的调用写在 `assert(...)` 里**
  （`assert(bitcask_put(...) == BITCASK_OK)`），Release 构建 `NDEBUG` 下那些 put
  **根本没执行**——新用例自带 `MP_CHECK` 宏与构建类型无关；旧用例没动，记在这儿。

### Changed

- **`Cask::merge(files)` 对非空 `files` 多两道闸**（`kInvalidOption`，不碰盘）：
  ① 名字 parse 不出 data 文件 tstamp——此前 `MergeRunner` 对这种是 `continue`
  **静默跳过**，打错字什么都不报；② 是当前 active 写文件——并它 = 收尾时把 writer
  正在追加的文件 unlink 掉（POSIX 上此后写的全丢，Windows 上 unlink 直接失败）；
  `needs_merge` 自动挑的从来排除它，显式表同一条纪律（merge_only 旁车排除的是
  open 时从 write.lock 抠出的 live writer id，与 `needs_merge` 同源）。
  ⚠️ 这是 C++ 层唯一的行为变化，只影响**自己传文件表**的调用方；空表那一支不动。
  ⭐ 这道闸当场抓到两条自家判据：`oki_levelb_test` / `oki_locate_test` 里
  「收集 sealed 文件 → 字典序 sort → `pop_back()` 当 active」——`9.bitcask.data`
  在字典序里排在 `10.bitcask.data` 之后，超过 9 个文件时弹掉的不是 active，
  **active 一直在被并**，此前静默通过。改成按 `parse_data_tstamp` 数值排。

## [6.3.3] - 2026-09-14（修复：Windows vendored ICU 在 VS 18 上撞 MSB8052）

> **版本语义**：C API 零改动，盘上格式零改动，只动 Windows 构建脚本
> `cmake/BitcaskICU.cmake`。纯构建修复 → PATCH +1，**`SOVERSION` 保持 `6`**。

### Fixed

- **vendored ICU 在 VS 2026（VS 18）上无法构建**：MSBuild 会把 `VCToolsVersion`
  自选成最新的 14.5x，与 `/p:PlatformToolset=v143` 冲突报 `MSB8052`，而报文指的
  「改工具集 / 修 VS 安装」两条都不通（14.5x 的 bin 里没有 cl.exe；安装没坏）。
  现在从 `CMAKE_CXX_COMPILER` 路径切出 `…/VC/Tools/MSVC/<ver>/bin/` 的版本号，经
  `/p:VCToolsVersion=<ver>` 全局属性钉给 MSBuild（`VCToolsVersion` 环境变量只作
  兜底；都拿不到不钉，保持旧行为）。configure 输出 STATUS 第四格显示钉的是哪版：
  `[Release | x64 | v143 | VCToolsVersion 14.44.35207]`。VS 18 真机 A/B：不钉复现
  MSB8052，钉上后 vendored ICU 与整库构建、链接通过。

### Added

- `feedbacks/`：下游反馈账（一条一文件，只记 libbitcask 这一侧该改的）；首条即本次
  ICU 条目。

## [6.3.2] - 2026-09-11（修复：V5 结构化 meta 重开即丢——带 meta filter 的检索恒空）

> **版本语义**：C API 零改动；盘上格式**仅追加段型、文件版本不动**——docmap
> ckpt 新增 `kDocmapMeta`（20）/ `kDocmapMetaDelta`（21）两个段，旧读端静默
> 忽略 → 行/水位完整，只丢 meta，与此前「meta 从不落盘」等价，不构成数据洞。
> 纯缺陷修复 → PATCH +1，**`SOVERSION` 保持 `6`**。无 meta 部署不写段，零字节
> 开销。

### Fixed

- **V5 结构化 meta 只活在内存，重开即丢**：docmap ckpt（base 全量与 delta 窗口）
  与 fold 重放都不带 meta，重开后 `eval_meta` 对所有文档恒 false——带 filter
  的检索一律空集，无 filter 检索正常。三条恢复路径本次全部补齐：
  - **docmap base ckpt**：新增 `kDocmapMeta` 段（全部 live 且 meta 非空的 ord，
    ord-gap + 长度 + blob 原样），读端在 `kDocmap` 行段应用完后逐条
    `Index::set_meta`。
  - **docmap delta 链**：新增 `kDocmapMetaDelta` 段（窗口 `[from, watermark)`
    内同上），**必须排在行段之后**——读端按文件序应用，行先 `put_doc`、meta
    后补，与活写路径 `put_doc→set_meta` 同序。
  - **fold 尾部重放**：恢复重放（`load_keydir_from_disk`）的 `ReplayDoc` 补带
    meta，与 ckpt 同序回填；仅带 meta 的文档（text/向量/命名字段全空）此前
    连 docmap 都不登记，被 live 过滤当死文档——现在与纯命名字段文档同理恢复。
  - 段畸形（长度不足/尾部残留）→ 整组件退 fold 重建，fold 重放同样回填 meta，
    不丢数据。`Index` 新增 `for_each_meta_in(from, to, fn)` 遍历原语（语义/
    锁与 `for_each_live_in` 一致）。
  - 回归测试 `V5MetaFilterSurvivesReopen` 三路覆盖：close 全量 base、崩溃镜像
    （base + delta 链）、崩溃镜像（ckpt + fold 尾部），并验证恢复后再
    checkpoint 的增量 meta 落盘。格式规约同步见 [`format-zh.md`](doc/format-zh.md)
    §10.1.1。

---

## [6.3.1] - 2026-09-08（构建修复：多配置生成器下的 exe 输出目录）

> **版本语义**：只动构建脚本——C API 的函数、枚举、结构体布局零改动，
> 库代码一行未变，盘上格式零变化 → PATCH +1，**`SOVERSION` 保持 `6`**。

### Fixed

- **多配置生成器（Visual Studio / Xcode）下 exe 与 DLL 被拆到两个目录**。
  `CMAKE_RUNTIME_OUTPUT_DIRECTORY` 那条只设了无后缀的变量，而多配置生成器只认
  per-config 的四个变体，于是 exe 落进 `bin/<Config>/`、DLL 仍在 `bin/`——
  「全部收进同一个 `bin/`」这条就地失效。现在 `_DEBUG` / `_RELEASE` /
  `_RELWITHDEBINFO` / `_MINSIZEREL` 一并指向 `bin/`，与下面 oneTBB 那段逐目标
  拉回 `bin/` 的既有做法一致。
  - 失败点在**构建期**而非运行期：`gen_inert_table` 是链 ICU 的代码生成器，
    构建中途要被执行，`icuuc*.dll` 不在它旁边就是 `0xC0000135`（MSB8066，
    退出码 -1073741515），而那条报错里一个字都不提 DLL。VS 生成器 + vendored
    ICU 的发行构建必现。
  - 代价：Debug 与 Release 在同一棵构建树里会互相覆盖 `bin/`——与既有取舍一致。

---

## [6.3.0] - 2026-09-08（S38：Unicode 层由 utf8proc 迁至 ICU · 新增编码转换；S39：C API 补齐——多字段写入 · meta 编解码 · 检索分页 · 高亮）

> **版本语义**：C API 纯增量——S38 自 6.2.2 起 `c_api/` 下一个字节都没动
> （`git diff 6.2.2..HEAD -- c_api/` 为空），S39 再新增 11 个函数 + 9 个类型；
> 既有函数签名、枚举值、结构体布局零改动 → MINOR +1，**`SOVERSION` 保持
> `6`**（`.so.6` 不换号，下游无需重新链接）。盘上格式**不 bump**、零变化
> （无 flag-day、无迁移）：`bitcask.meta` 仍是 `v5`（基线；使用原子批的目录
> 懒升 `v6`）；S38 的 Unicode 版本记录占用的是保留区里两个**原本就空闲**的
> 字节（偏移 12-13，落在既有 CRC 覆盖区 `[0,14)` 内）——旧目录读出来是
> `0` = 未记录，新旧双向兼容。

### Added

- **ICU 取代 utf8proc 作为 Unicode 层（S38）**。`third_party/utf8proc` 换成
  `third_party/icu`（submodule，钉 `release-78.3`；标了 `update = none`，
  `--recursive` 不拉它）。
  - NFKC_Casefold 归一化改走 `icu::Normalizer2::normalizeUTF8`——直接吃 UTF-8，
    省掉 utf8proc 路径上的 UTF-16 往返。接入层集中在
    `include/bitcask/detail/icu_util.hpp`，全库唯一直接摸 ICU C++ API 的地方。
  - 字符属性 `utf8proc_category` → `u_charType`（两者同为 Unicode 通用类别，
    逐码点等价）。
- **文本编码转换（`src/text/text_encoding.cpp`）**：走 ICU 的 `ucnv_*`，把
  GB18030 / GBK / Big5 / Shift_JIS 等转成 UTF-8 再入索引。编码名走 ICU 的
  转换器名与别名；转换器出厂回调是 SUBSTITUTE，本层显式改成 STOP——非法输入
  报错而不是静默变 U+FFFD。
- **Unicode / ICU 版本记入索引元数据**：建索引时把两个 major 写进
  `bitcask.meta` 的保留字节，重开时不一致则**告警**。刻意不做 fail-fast——
  这是诊断信息、不是纪元门禁，未知版本号不该让目录打不开。
- **`cmake/BitcaskICU.cmake`**：ICU 来源三态可控 `BITCASK_ICU_PROVIDER`
  = `auto`（默认，系统优先、回落 vendored）/ `system`（找不到就配置期报错）/
  `vendored`（只用子模块）。只用 `uc` + `data` 两个组件，不链 `i18n`。
- **Windows/MSVC 的 vendored ICU 支持**（ICU 官方在 Windows 只提供 MSBuild
  解决方案，无 CMake）：MSBuild 经 `CMAKE_VS_MSBUILD_COMMAND` → `PATH` →
  `vswhere` 三级探测；显式指定 `PlatformToolset`（ICU 自带的 props 只认到
  `v143`，更新的 VS 会落空报 `Platform Toolset = ''`）；ICU 配置跟随
  `CMAKE_BUILD_TYPE` 以对齐 CRT；两个 ICU DLL 自动进 `<build>/bin/` 与
  `install` 的 `bin/`。新增旋钮 `BITCASK_ICU_MSBUILD` /
  `BITCASK_ICU_MSVC_TOOLSET` / `BITCASK_ICU_MSVC_CONFIG`。

- **多字段文档写入**：`bitcask_doc_field_t` / `bitcask_doc_input_ex_t` /
  `bitcask_put_doc_ex`。此前 C 侧只能写 text/meta/vector，命名字段（fields）
  无从表达，`bitcask_search_fields` 的 `field:term` 在纯 C 建的库上永远搜不到
  东西——本组补齐该缺口。`fields_count==0` 时与 `bitcask_put_doc` 逐字节同义
  （实现收敛于 `put_doc_common`）。fields 与 meta 正交：fields 是**进倒排索引
  的命名文本**（分词、参与 BM25），meta 是**不分词的结构化属性**（仅布尔过滤）。
- **Meta 编解码（纯函数，不碰句柄，线程安全可重入）**：
  - `bitcask_meta_encode` / `bitcask_meta_blob_free`——编码
    `bitcask_doc_input_t.meta` 那块 V5 结构化 KV blob。entries **无需预排序**
    （内部按 key 升序排），重复 key 直接判非法——把「升序无重复」的格式
    不变式封在库内，不外泄给绑定作者。
  - `bitcask_meta_lookup`（单 key 零拷贝查询，零分配，引擎过滤热路径同款
    剪枝）与 `bitcask_meta_iter_begin` / `bitcask_meta_iter_next`（零拷贝
    游标遍历）+ `bitcask_meta_value_view_t` 零拷贝值视图（STRING 借 blob
    内存，不保证 NUL 结尾）。损坏 blob 一律按「未命中 / 遍历结束」处理，
    不报错——与 C++ 侧容错一致。
- **检索分页**：`bitcask_search_text_ex` / `bitcask_search_phrase_ex` /
  `bitcask_bool_search_ex`。C++ 侧三个入口一直有 `offset` 参数（S13-D10），
  C 侧此前全无分页能力。offset 经 overfetch `k+offset` 后截断——深分页成本
  线性增长；不提供总命中数（WAND/BMW 剪枝下 total 只能给下界，与 C++ 侧
  同样不给）。
- **高亮检索**：`bitcask_search_text_highlight` + `bitcask_highlight_options_t`
  （全零取 C++ 默认：`<em>`/`</em>`、片段 120B、每命中 3 段）+ 结果族
  `bitcask_search_result_ex_t` / `bitcask_search_hit_ex_t` / `bitcask_snippet_t` /
  `bitcask_search_result_ex_free`。补齐 C++ `Cask::search_text_highlight`
  （S13-D3）的 C 表示；原文 LRU 未命中的冷文档降级为 `highlights_count==0`
  的 hit，不整条丢弃。
- 测试：`tests/c_api_test.c` +620 行覆盖四组新 API；`doc/api-c.md` 符号表同步。

### Changed

- **`BITCASK_ICU_TRIM_DATA` 默认由 `ON` 改为 `OFF`**，即默认用全量 ICU 数据
  （约 31 MB，裁剪版约 5.5 MB）。理由是风险不对等：裁多了**不会让构建失败**，
  只会让归一化或某个编码在**运行期**静默失效；而
  `cmake/icu-data-filter.json` 里写的是 ICU 的**内部 category 名**，跨大版本
  会改（ICU 78 已对其中的 `brkitr_treedict` / `coll_tree` 报“category 不存在”，
  且只是 warning、不中断）。为省 25 MB 背这个不划算。需要时显式
  `-DBITCASK_ICU_TRIM_DATA=ON`。

### Fixed

- **`inert_table.hpp` 跨 ICU 版本的构建安全**。这份 NFKC inert 区间表是**生成
  物却放在源码树里**，被所有构建树共享，而它的内容取决于生成器链接的那个 ICU
  的版本（system 与 vendored 可以不同版本）。原先的
  `add_custom_command(OUTPUT ...)` 只按“生成器是否比产物新”判定，另一棵树刚写
  过表之后本树就会跳过重新生成，**拿着别人 ICU 版本的表编译自己的 TU**。改为
  `add_custom_target(... ALL)` 每次构建都跑，由生成器自己“内容不变就不落盘”
  来避免无谓的下游重编。
  - `tests/analyzer_test.cpp` 直接 include 这份生成的头，补上对
    `generate_inert_table` 的显式依赖——换成 custom target 后，消费者不再能经
    **文件**拿到隐式依赖，缺这条边时 ninja 可以在同一次构建里先编测试、后生成表。
  - `NfkcInert.TableOracleExhaustive` 改为**双向**校验（表说 inert 的码点确实
    inert，且表中每个成员确实 NFKC-casefold 不变），版本错配当场变红而不是靠
    一张陈旧的表静默通过。

---

## [6.2.0] - 2026-08-08（S37：Windows 移植——MSVC 原生 · x64；P3/P4 I/O 稳健性收口）

> **版本语义**：C API 零变化——函数签名、枚举值、结构体布局全部零改动 →
> MINOR +1，**`SOVERSION` 保持 `6`**（`.so.6` 不换号，下游无需重新链接）。
> 盘上格式零变化（无 flag-day、无迁移）。

### Added

- **Windows 平台支持（S37，MSVC 原生 ABI · x64）**，子任务全数交付：
  - **S37-1 io seam**——全库裸 POSIX 调用（`::open` / `::fstat` /
    `::fdatasync` / `::rename` / `::mmap` / `::sysconf` …）收进 `bitcask::io`，
    Linux 行为零变化；换平台 = 换一个后端文件。seam 补齐
    `io::atomic_rename`（Windows 上 `std::rename` 目标存在即失败，9 个
    原子写站点全员改道）、`PosixFile::size() / truncate() / identity()`、
    `io::sync_data / sync_directory()`、`MappedFile::prefetch`、`io::page_size`。
  - **S37-3 `bitcask_simd`**——运行时 CPUID/XGETBV 探测 + `BITCASK_SIMD_MAX`
    档位钳制（替代 MSVC 不支持的 `__builtin_cpu_supports`）；SIMD 内核按
    ISA 分 TU（S37-3.b），`-m` / `/arch:` 一一对应，规避 PCH 选项不一致。
  - **S37-4 构建适配 MSVC 工具链**——`/utf-8`、`/bigobj`、`/permissive-`、
    `/Zc:preprocessor`、`/Zc:__cplusplus`、`NOMINMAX` / `WIN32_LEAN_AND_MEAN`
    全局开关；CRT 模型检查（/MT 跨模块跨堆释放告警）；合并静态库改原生
    `$<TARGET_OBJECTS:>`（顺带补回此前漏掉的 bitcask_simd 对象），Windows
    下改名 `bitcask_static.lib` 避免与导入库撞名；`CMakePresets.json` 覆盖
    Windows（debug/release/asan）与 Linux 全部常用配置。
  - **S37-5 Windows I/O 后端**——`src/io/win32_file.cpp` 落地，io seam 在
    Windows 侧有实现，链接期目标不再缺符号。
  - **S37-6 文件删除/替换**——Windows 上打开中的文件不再阻塞重命名/删除
    （file_lock、`.prev` 轮转、merge 迁移全部改经 seam）。
  - **zlib 子模块化（v1.3.1）+ 移除 vcpkg**——Windows 构建不再需要
    vcpkg / `VCPKG_ROOT` / `x64-windows` 三元组，依赖来源与其余平台统一、
    版本钉在提交里；Linux / BSD / macOS 维持系统 `find_package(ZLIB)`。
- **解除 Windows 单文件 2 GiB 上限（P2）**：写路径偏移全 64 位，与 Linux 对齐。

### Changed

- 库边界不再跨模块传递 `FILE*`（W5）：C I/O 两路只交换内核句柄，`FILE*`
  的生成与拆解在调用方模块内完成——最后一处长驻 `FILE*` 退役。
- `.prev` 轮转与 vec 追加目标身份（dev/ino）校验改走 io seam（P1）。
- CI 工具链 GCC 13 → **14**（Release 全量），配套清掉 GCC 14 新增告警
  （`-Wmissing-field-initializers` 聚合默认初始化、数据面/OKI/搜索/插件/
  bench 各套件）；`BITCASK_WERROR` 库构建在 GCC 14 下保持全绿。

### Fixed

- **撕裂尾部（torn tail）覆盖（P3）**：写入偏移锚定到最后一个完整 record，
  崩溃 / 掉电后尾部半条 record 不再被当作有效数据；「丢写导致字段 id
  漂移」一并修复。
- **Win32 错误码泄漏（P4）**：`GetLastError` 不再污染 C API 的 `errno`
  字段，调用方拿到的始终是 POSIX 语义错误码。
- **窄路径 UTF-8 双端对齐（P0，关 W4）**：`std::filesystem` 窄字符路径在
  输入 / 输出两侧统一按 UTF-8 处理（Windows 内部 UTF-16），并扩充测试覆盖。

---

## [6.1.0] - 2026-08-06（OKI 不可用拆码：kNoIndex / kIndexRebuildFailed）

> **版本语义**：C API 纯增量——`bitcask_error_t` 尾部追加一个枚举值，
> 既有值与函数签名、结构体布局零改动 → MINOR +1，**`SOVERSION` 保持
> `6`**（`.so.6` 不换号，下游无需重新链接）。盘上格式全部不动。

### Added

- **错误码 `CaskError::kIndexRebuildFailed`** / C
  `BITCASK_ERR_INDEX_REBUILD_FAILED`（枚举尾部追加，既有值不动）：
  `make_range_iter` / `bitcask_range_iter_start` 的「OKI 不可用」按成因
  拆码——RO/merge_only 打开无 OKI 的目录（本就不建，重开读写即建）仍报
  `kNoIndex`；可写 open 重建失败（IO/环境问题）改报新码。旧行为两者压成
  `kNoIndex`，调用方无从区分「重开读写即好」与「环境有问题」。

---

## [6.0.0] - 2026-08-06（S36：keydir 磁盘驻留 Level B；backlog 收口 T24 / B1-B4）

> **版本语义（2026-08-06 定版）**：`CaskOptions`（C++）与
> `bitcask_options_t`（C）各新增 `keydir_cache_entries` 字段——**结构体
> 布局较 5.1.0 变更**，按 [4.0.0] 先例（布局变更 = ABI 破坏）bump major
> → `6.0.0`，**`SOVERSION` → `6`**（`.so.6`，下游需重新链接）。C API
> **函数签名零增删改**；除 TxnCask `recover`/`pending_txns` 语义收敛
>（见 Removed）与 merge 空间回收时序（见 Changed）外行为向后兼容。
>
> 盘上格式：**meta 纪元不动**（v5/v6 同 5.1.0，无 flag-day、无迁移）。
> 新增格式皆**派生缓存演进**：OKI run **BCOK v2**、manifest
> **BCOM v2/v3**、keydir 快照 **BCKS v4**——5.1.0 读端遇之拒收 → 全量
> 重建自愈；5.1.0 写出的目录本版直接打开。双向互开、零停机。

### Added（S36：keydir 磁盘驻留——Level B，opt-in）

- `CaskOptions::keydir_cache_entries` / C `bitcask_options_t.keydir_cache_entries`
  （`0`=不限=现状，默认）：keydir 降级热点缓存（超预算分片内采样逐出），
  点查权威 = 缓存 → memdelta → **BCOK v2 run**（全字段行 + 内嵌 bloom +
  块 LRU），冷 get ≤2 次 pread。**1 亿 `doc:` key 实测：常驻 11GB →
  加载峰值 1.14GB / 重开 0.80GB（-90%）**；热 get/put/merge 零回归。
- 盘上（全部派生缓存演进，零 flag-day）：**BCOK v2**（全字段 vbyte 行 +
  墓碑免位置 + 10bits/key 内嵌 bloom + 32B trailer）；**BCOM v2**
  （run 条目带 format_ver，惰性版本选择）与 **BCOM v3**（Level B 模式戳
  ——未带戳开启即全量重建，Level A 写者 open 清戳）；**BCKS v4**
  （缓存子集快照 + 逻辑计数，信任链 = 模式戳 + wm 覆盖）；OKI 重建改
  **外排**（`kv.oki.spill-*` 分段 + k 路归并，500 万行峰值 RSS 83MB）。
- fold/`parallel_scan`/range 在逐出态经组合视图完整枚举；merge 活性/搬迁/
  TTL 对被逐 key 经组合视图裁决（免 CAS 的 (ord, gen) 胜出格）；merge
  收尾「搬迁行固化先于输入 unlink」（崩溃任意点数据可读）。
- **merge_only 旁车对 Level B 目录 open 拒绝**（旁车无挂钩搬迁会腐蚀
  组合视图位置权威）。

### Changed（backlog B3/B4：mmap 归并 + 延迟删除队列）

- **B4：merge 的输入文件改「退休 + 延迟删除」**，不再当场 unlink——文件
  留在原路径直到下一个落点（下次 merge 开始 / `checkpoint()` 入口 /
  `close()`）才删除。持旧 keydir 快照的在途读者惰性重开不再有 ENOENT
  假失败窗口（S13-F5 重试兜的那类）；merge 收尾临界区不再做文件系统
  操作。**可见变化：merge 释放的磁盘空间延后一拍**（到下一个落点）；
  崩溃残留的退休文件是普通 data 文件，恢复与后续 merge 自愈收编。
  OKI run 清扫同步改「尝试删除 + 失败滞留重试」。
- **B3（内部）**：7 处手写 mmap 生命周期归并为 `io::MappedFile` RAII
  （data 文件 sealed 映射 / BM25 段 / HNSW-IVF-DiskANN payload）——
  行为零变化，纯维护面收敛。

### Removed（backlog B2：legacy 意图重放）

- **TxnCask 方案 B 意图重放删除**（意图 blob v1 解码 + `recover` 前滚 +
  `pending_txns` 枚举的实现体）：5.1.0 发布版的 `commit` 已是引擎原子批
  （S35 在发布前替换了方案 B），意图 blob 只可能出自 dc81bbc..S35 之间的
  **未发布开发构建**——发布用户的目录不存在重放对象，兼容代码没有服务
  对象。接口
  `recover`/`pending_txns`（含 C API `bitcask_txn_recover`/
  `bitcask_txn_pending_count`）**签名保留**，恒返回 0/空；开发期残留的
  `_txn:` 前缀 key 不被触碰，可经普通 KV API 手工清理。崩溃原子性全部
  由引擎原子批承载（S35，`put_batch_atomic`）。

### Fixed（S36 期间）

- **B1（checkpoint 跑赢未 fsync 数据，S35 备案的预存暴露面）收口**：
  持久水位由 DataFile 自维护（sync/o_sync 写路径/封口推进——roll/close
  起「封口即持久」不变量），checkpoint 采集点做线程安全的 fd 级
  fdatasync，keydir 快照 / ckpt 链字节水位 / OKI run 一律「引用 ≤ 持久」
  过滤。掉电后快照存活而数据蒸发的悬空条目（get 报 IO/CRC）就此消除。
  注入测试证实并回归钉死。
- 顺手修（S29-7 遗留）：组提交成为 put 主路径后 memdelta **阈值 flush
  失联**——纯 KV 长写负载（无 checkpoint 搭车点）memdelta 无界增长
  （实测 1000 万 put 时 790MB、0 run）。组提交 leader 批间与各批量写路径
  尾部补探询；顺带修 `close()` 在 `sync_every_n=0` 下全程零 fsync 的
  既有暴露。
- 顺手修（S36-2）：`KeyDir::conditional_remove`（TTL 路径）的文档化
  TOCTOU（「可能误删并发新写」）改锁内精确 CAS，从容忍变为不可能。

---

## [5.1.0] - 2026-08-05（S33：有序 key 索引 OKI + hint ord flag-day；S34/S35：多键事务与引擎原子批）

> **版本语义**：C API 为**纯增量**（新导出 6 个 range 函数 + 2 个前缀入口；
> 既有函数签名与结构体布局零改动），故 MINOR +1 → `5.1.0`，
> **`SOVERSION` 保持 `5`**（`.so.5` 不换号，下游无需重新链接）——与
> [3.1.0] 同款处置：**盘上格式破坏不驱动 major/SOVERSION，ABI 破坏才驱动**
> （4.0.0 = 结构体布局变更、5.0.0 = 签名与字段宽度变更，那两次才必须 bump）。
>
> 盘上格式版本：`bitcask.meta` = **v5**（基线；首次 `put_batch_atomic`
> 懒升 **v6**，见 S35 条目），hint = **BCH5**，
> OKI run = **BCOK v1** / manifest = **BCOM v1**。

### ⚠️ 前向不兼容（盘上格式 flag-day；ABI 未破坏）

**hint 内嵌 ord flag-day**——hint 记录新增 `ord` 字段（vbyte 差分），
magic `BCH4` → `BCH5`；`bitcask.meta` v4 → **v5** 作为唯一纪元门禁。

- **本版写出的库不能被 5.0.0 打开**（5.0.0 读端只认 meta v4）。
- **本版也不直接打开 v4 纪元目录**：open 时**干净拒开**并提示迁移命令，
  绝不按新语义把旧字节静默读坏。
- **但不必重建**：`bitcask_migrate hintord <src> <dst>` 是**非破坏性 +
  data 字节零改动**的迁移路径（data 硬链接，只重生成 hint + meta；
  meta 最后写 = commit point，幂等可重跑）。
- **二进制层面无隔离需求**：不兼容发生在「二进制 × 数据」而非
  「二进制 × 二进制」——旧调用方链接 `.so.5` 打开 v5 目录会拿到带迁移提示
  的干净错误，故不换 soname。

动机：v4 时代 hint 不存 ord，hint 快路径恢复的条目 ord 恒 0，与 fold(data)
不等价；v5 之后两条恢复路径逐 key 等价，这也是 OKI tail 重放的前提。

### Added（OKI：有序 key 索引 / range 查询）

- **`Cask::make_range_iter(RangeOptions{lo, hi, ...})` + `CaskRangeIter`**：
  按 key 字典序遍历 `[lo, hi)`，**O(range)** 取代 O(全表) 过滤。实测
  （10 万 key，选择性 1/256）：`CaskIter::start(key_prefix)` 8.0 ms →
  **0.53 ms（15×）**，且耗时随选择性线性下降。一致性为 per-key 弱一致
  （与 `parallel_scan` 同档，非 fold 快照）。
- **`RangeOptions::prefetch` / `prefetch_threads`**：批量并发预取值——只
  改取值时机，输出序与内容不变。大窗口 + 冷值形态收益明显
  （1 KiB 值 / 6250 命中：10.8 ms → 7.6 ms）。
- **OKI 盘上结构**（派生缓存，校验不过即整体弃用重建）：
  `kv.oki.seg-<gen>`（BCOK v1：~4 KiB 块 + 块内 key 前缀差分 + 稀疏块索引
  + trailer CRC）与 `kv.oki.manifest`（BCOM v1：run 集合 + 覆盖水位，
  `atomic_write_bytes(fsync_dir=true)` 唯一 commit point）。
- **写挂钩与恢复**：挂钩收敛在 `KeyDir::put/remove` 咽喉点（Cask 各写路径零
  改动）；flush 恒在 keydir 快照之后搭车；open 时 `wm < 快照 next_ord` 或
  manifest 缺失/损坏 → 全量重建兜底。put 路径实测回归 ≈1.2%（预算 ≤3%）。
- **run 归并**：run 数 > 8 时全部归并成一个（同 key 取 max-ord），使常驻
  fd 数与 range 的归并路数有界；**全归并时墓碑行真正丢弃**（其余时候保留
  tomb 行以抵消旧 put 行）。
- **C API（新增导出，ABI 纯增量）**：
  `bitcask_range_iter_start` / `_next` / `_next_batch` / `_release` /
  `bitcask_range_entry_free` / `bitcask_range_options_init` +
  `bitcask_range_options_t` / `bitcask_range_entry_t`；
  另补齐既有能力的 C 侧入口：`bitcask_iter_start_prefix`、
  `bitcask_parallel_scan_prefix`（空切片时与无前缀版完全等价）。
- **迁移工具**：`bitcask_migrate hintord`（v4 → v5）+ `detect` 识别 v4/v5。

### Added（S34/S35：多键事务 + 引擎原子批）

- **S35 `Cask::put_batch_atomic`（引擎原生跨崩溃原子批）**：盘上
  `kBatchHeader`（RecordType=2）声明成员区间，批头+成员一次 flush 落盘；
  恢复时**区间完整且逐条 CRC 有效 ⟺ 已提交**，否则 `last_valid_end` 停在
  批头起点、整批截断——崩溃/掉电后 all-or-nothing。成员是普通
  kDoc/kTombstone 记录 ⇒ get/iter/merge/hint 读路径零特判；支持批内
  REMOVE；批内 op 依序 apply（同 key = 批内 LWW）。设计：
  `doc/atomic-batch-design-zh.md`。
- **meta v6 纪元（懒升级）**：首次 `put_batch_atomic` 前把 `bitcask.meta`
  重写为 v6（原子写：tmp+rename+fsync，顺手修复 `write_meta` 裸 ofstream
  非原子的问题）。**从不使用原子批的目录停留 v5**（保守纪元标记——v5/v6 均为本版
  5.1.0 引入并可读；旧于本版的读端对两者都干净拒开，unsupported meta
  version）。
  `bitcask_migrate detect` 认识 v6。
- **`bitcask::TxnCask`**（`include/bitcask/txn.hpp`，S34 立项、S35 重接）：
  多键事务门面——`commit` = 一次引擎原子批（+ 按 `TxnSyncPolicy` fsync），
  无意图日志写放大；`recover`/`pending_txns` 保留为 legacy（重放方案 B
  意图日志时代目录遗留的 `_txn:` pending，新目录恒 0/空）。提供崩溃
  原子性（A）+ 持久性（D）；**不提供**隔离性（I）与 CAS。
- **C API（纯增量）**：`bitcask_put_batch_atomic` + `bitcask_txn_commit` /
  `bitcask_txn_recover` / `bitcask_txn_pending_count` + `bitcask_txn_op_t`。
- 模式文档 `doc/multikey-txn-zh.md` 勘误（S34-1）：`RangeOptions::prefetch`
  是批大小（`true` 隐转 1 = 关闭）、迭代器真实接口、`put_batch` 无墓碑
  形态、txn key 必须单调（uuid 重放序 ≠ 提交序）。

### Fixed

- **并行恢复下的墓碑复活**（S33-B1，既有 bug）：纯 KV 并行恢复（按文件并发
  fold）中「墓碑文件先完成」或「remove 先到 + put 后到」会让已删 key 复活
  ——到达序依赖。修复引入 ord 全序判据：`KeyDir::remove` 支持记录缺席墓碑
  sentinel（带 ord 高水位），`put_insert` 复活须 `put.ord > 墓碑 ord`。
  运行期路径语义零变化。复现率 8/30 → 修后 40/40 稳过。
- **mmap 窗口外读竞态**（S33-B2，S30 × S13 既有 bug）：merge 输出在分批 CAS
  期间仍在增长，读者以打开时刻尺寸 mmap 后，后续批次 CAS 的条目落在映射窗口
  外 → `get` 误报 `kIo`。修复：mmap 分支的 `kShortRead` 跌落 pread
  （kBadCrc 等仍报错）。并发 stress 修前 3/3 读者中招 → 修后 0/20。
- **meta 错误信息被吞**：`Cask::open` 两处把 `MetaError.message` 吞成笼统
  "read meta failed"，导致纪元迁移提示传不到用户——已透传。
- **文件尾墓碑不推进 ord 水位**：`fold(data)` 墓碑分支原本不 `advance_ord`
  （重启后 ord 复用的潜在隐患），已与 hint 路径对齐。

### Changed

- hint 读端**只认 BCH5**；BCH4 及更早在 `validate_trailer` / `fold_validated`
  处按「缓存不可用」返回 false → 退 `fold(data)` 重建（纪元硬门禁由 meta v5
  承担）。v2 定宽常量与 v2/v4 编解码整体删除。
- **`max_read_handles` 自动档加绝对上限 1024**（原来只有下限 64）：自动档取
  `RLIMIT_NOFILE` 的一半，而容器 / systemd 环境下 rlimit 常见 5×10⁵ 甚至
  10⁶——"一半"等于没有上限，实测 89 个 data 文件的库把 89 个 fd + 88 个 mmap
  全留着不淘汰。**这是默认行为变更**：data 文件数 > 1024 的库会开始淘汰空闲
  读句柄（淘汰后再读需重开 fd + 重建 mmap）。显式给 N 或
  `kUnlimitedReadHandles` 者不受影响（不夹取）。调优见 `api-cpp.md` §11.1。
- `be2le` / `tstamp64` 的目标纪元同步 bump 到 meta v5。
- 三处手抄的 hint/data refill 循环归并为 `detail::ChunkedReader`（T23），
  顺带修正 data_file 版丢失的 `len_ +` 项（巨型 record 的 need 公式）。

### Docs

- `doc/format-zh.md`：§六 重写为 hint v5（BCH5），新增 §十五 OKI
  （BCOK/BCOM 字节级布局 + 生命周期）；§14.2 迁移子命令补 `hintord`。
- `doc/api-cpp.md` / `doc/api-c.md`：range 迭代器与预取契约、所有权配对表、
  线程安全表同步；`doc/migrate-le.md` / `README.md` 同步纪元与新能力。
- 新设计文档 `doc/ordered-key-index-design-zh.md`。

---

## [5.0.0] - 2026-07-17

### ⚠️ ABI / 盘上格式破坏（major bump 的原因）

**64 位时间戳 flag-day**——`tstamp` / `expiry_at` 全链路 `uint32_t` → `uint64_t`，
Y2038 前瞻；与 4.0.0 同款处置：SONAME `libbitcask.so.4` → **`.so.5`**，
链接器层隔离旧二进制。**源码级不向后兼容**（u32 调用方重编即正确），
**盘上格式不向后兼容**（`bitcask.meta` v4 门禁干净拒开 u32 纪元库——但**不必
重建**，见下方 Added 的 `bitcask_migrate tstamp64` 非破坏性迁移路径）。

- **C ABI 签名变更**（5 个导出函数）：`bitcask_put` / `bitcask_put_ex` /
  `bitcask_delete` / `bitcask_put_doc` / `bitcask_put_batch` 的 `tstamp` /
  `expiry_at` 形参 u32 → u64。
- **C ABI 结构体字段宽度变更**：`bitcask_get_result_t` / `bitcask_kv_pair_t` /
  `bitcask_doc_input_t` 的 `tstamp` / `expiry_at` 字段 u32 → u64（旧编译
  二进制 + 新库 = 字段错位读，必须 bump SOVERSION 隔离）。
- **C++ API 签名变更**：`bitcask::Cask` 全系写方法（`put` / `put_batch` /
  `remove` / `put_doc` / `needs_merge` / `merge` / `start`）、
  `keydir::KeyDir::put/remove/conditional_remove/start`、
  `merge::run_merge`、`merge_policy::decide/per_file_reasons` 等同款 u32→u64。
- **盘上格式整体 break**（meta v4 门禁）：详见下方 Changed。旧 u32 纪元
  库（meta v2/v3）open 时干净拒开，error message 提示
  `64-bit tstamp flag-day requires rebuild — re-ingest data`——与 4.0.0
  大端 flag-day 同策略，**绝不按新偏移把旧字节静默读坏**。

### Added（Y2038 readiness）

- **64 位时间戳**：`tstamp` / `expiry_at` 全链路 `uint64_t`——可表达
  unix 秒至远未来（u32 上限 2106-02-07）。C/C++ API + 盘上 record header
  + DocValue + hint + keydir 快照 + docmap sidecar + IndexPool/ReduceJob
  等所有 tstamp 载体同步扩宽。
- **`now_sec_default()` 返回 u64**：`clock_gettime(CLOCK_REALTIME_COARSE)`
  的 `tv_sec` 不再 `static_cast<u32>` 截断。
- **过期判定 u64 域算术**：`tstamp + expiry_secs` 与 `expiry_at` vs `now`
  的比较全部在 u64 域，杜绝 u32 wrap（见 Fixed）。
- **新段型 `kDocmapDeltaV3`**（`search_checkpoint.hpp`）：与 V2 行布局
  相同，仅 tstamp 定宽 4B→8B；含本段的 ckpt 文件以 `kCkptVersion3`
  写出（旧读端整文件拒收 → 链断退 fold，降级安全）。

### Added（迁移工具链——u32→u64 非破坏性路径）

5.0 引入**统一迁移入口** `bitcask_migrate`（`tools/bitcask_migrate.cpp`），
覆盖两次 flag-day 造就的全部三个纪元；旧库无须重建，离线迁移即可：

| 子命令 | 入参纪元 | 出参纪元 | 对应库 API |
|--------|---------|---------|-----------|
| `detect <dir>` | 任意 | —（只报告 + 建议） | — |
| `be2le <src> <dst>` | v1 大端（meta v1） | 当前（meta v4） | `migrate::migrate_be_to_le` |
| `tstamp64 <src> <dst>` | u32 小端（meta v2/v3） | 当前（meta v4） | `migrate::migrate_u32_to_u64`（**新**） |

- **`bitcask::migrate::migrate_u32_to_u64()`**（`include/bitcask/migrate.hpp`）：
  u32 纪元（meta v2/v3）→ 当前纪元（meta v4）。非破坏性（只读 src、只写 dst）：
  - meta v2/v3 → v4：除 version 字节与 CRC 外**逐字节照搬**（mode / 向量配置
    两纪元布局未变）；v3 入口校验 CRC，v2 无 CRC 字段跳过校验。
  - data record header 23B → 27B：解旧小端头 → 用当前 codec 重编码（CRC 重算），
    tstamp u32 → u64 **零扩展**（值域不变，仅位宽升级）。
  - DocValue v3 → v4 转码：仅改 Ver 字节 `0x03`→`0x04` + 末尾 expiry 段 u32→u64
    零扩展（`transcode_doc_value_v3_to_v4`）；中间段（vector/text/meta/fields）
    布局未变，原样保留。墓碑 value（空 / 4B 小端 shadow file_id）不是 DocValue，
    原样照搬。
  - `field.schema` 格式在本次 flag-day 未变，原样拷贝。
  - hint 由迁移后的 data **重生成**；ckpt/seg/wal/旧 hint/锁不迁移（新库首开
    自动 fold 重建）。
  - `MigrateStats` 加 `skipped_bad_docvalue`：kDoc 的 value 段不是合法 DocValue v3
    （Ver 字节不符 / 短于头部）的 record 数——这类 record 旧读端同样解不动。
- **`migrate_be_to_le()`** 输出 meta 由 v3 → **v4**（输入仍是 v1 大端；
  v1 record 解码改用工具内 `kLegacy*Offset` 钉死常量，不再借 `format::`——
  因 `format::kHeaderSize` 等已随 u64 flag-day 漂移到 27B）。
- **`migrate_le`**（旧入口）保留兼容，usage 提示指向统一入口 `bitcask_migrate`。

### Added（回归测试 ×2，`tests/cask_docvalue_test.cpp`）

- `HugeExpirySecsNoWrap`：`expiry_secs = 0xFFFFFFFF`（u32 max ≈ 136 年）
  下 get/iter 不得因 u32 求和 wrap 误判过期。
- `Post2106TimestampRoundTrip`：`tstamp = 5'000'000'000`（> 2^32，2128 年）
  + `expiry_at` 同纪元，data record / hint / 恢复路径全链路落盘读回不截断。

### Changed（盘上格式 flag-day）

| 组件 | 旧（u32 纪元） | 新（u64 纪元） | 备注 |
|------|---------------|---------------|------|
| `bitcask.meta` | v3 | **v4** | 门禁：v1/v2/v3 全部拒开提示重建 |
| data record header | 23B（Tstamp u32） | **27B（Tstamp u64）** | CRC 覆盖范围不变（Type..Value） |
| DocValue | v3 | **v4** | ExpiryAt 段 u32 → u64；Ver 字节 `0x03` → `0x04` |
| hint 文件 | v3（magic `BCH3`） | **v4（magic `BCH4`）** | trailer magic `BCHE` 不变；tstamp u32 → u64 |
| keydir 快照（`kv.keydir.ckpt`） | BCKS v2 | **BCKS v3** | tstamp 定宽 4B→8B；v1/v2 拒收退 fold |
| keydir `meta_delta`（docmap 段内） | v1 | **v2** | fstats `oldest/newest_tstamp` u32 → u64 |
| docmap sidecar（`index::Index`） | v2 | **v3** | 行内 tstamp 定宽 4B→8B；v1 读分支删除 |
| docmap delta 段型 | `kDocmapDeltaV2` | **`kDocmapDeltaV3`** | 旧 V2 段型解析仍收（tstamp64=false） |
| ckpt 文件 version | `kCkptVersion2` | **`kCkptVersion3`** | 仅含 V3 段的文件用 3；旧读端整文件拒收 |
| `segment_v2` DocRow | v1 | **v2** | 字段重排吃掉 pad，仍 48B；布局二进制不兼容 |
| `DocStore`（`segment.hpp`） | v1 | **v2** | 行内 tstamp 4B → 8B |

**`DocSlot` 内存布局**（`index.hpp`）：S21-1 曾借 DocLoc 重排收到 24B；
tstamp u32→u64 后回到 32B（尾部 4B padding），每 chunk 槽数组 1.5MB→2MB。
`serialize_docmap` / `deserialize_docmap` 逐字段读写，不受内存布局影响。

**测试 golden 字节换代**：

- `DataRecord.GoldenLayout` / `GoldenHex`：tstamp 4B → 8B（`78563412` →
  `7856341200000000`）；`Layout.ConstantsLocked` 锁定 `kHeaderSize=27`、
  `kOrdOffset=13`、`kKeySzOffset=21`、`kValueSzOffset=23`。
- `DocValue.VectorSegmentGoldenHex` / `GoldenHex`：Ver 字节 `0x03` → `0x04`。
- `HintRecordV3.GoldenLayoutContiguous`（测试名保留）：tstamp 4B → 8B；
  v4 完整文件 `HintFileGolden.EncodingMatchesGoldenByteForByte` 55B
  （v3 同载荷 43B；v2 同载荷 79B）。
- `MigrateBEtoLE.RoundTrip`：dst meta 版本 3 → 4。

**旧版本读端从「兼容读」翻为「干净拒收」**（与 4.0.0 大端 flag-day 同策略；
meta v4 是统一门禁，组件层不再各自维护 u32 兼容分支）：

- `Index.SidecarV1CompatRead`：v1 sidecar 必须返回 `nullopt` 退 fold
  （改前断言「必须可读」+ 6 项字段值校验）。
- `KeyDirSnapshot.V1CompatRead`：v1 快照必须返回 `nullopt` 退 fold
  （改前断言「必须可读」+ 6 项字段值校验）。
- `CaskDocValueTest.MetaV2BackwardCompatRead`：meta v2 必须被门禁拒开
  并在 error message 中含 `rebuild`（改前断言「必须向后兼容读取」）。

### Fixed

- **u32 wrap 致全库 key 误判过期**：旧路径 `entry->tstamp + opts_.expiry_secs`
  在 u32 域求和，`expiry_secs` 极大（接近 `0xFFFFFFFF`，约 136 年）时和值
  mod 2^32 wrap 成小值，立即 ≤ `now`，**所有 key 被静默判为过期**（get 返回
  `kNotFound`、iter 跳过）。修复：求和显式提升到 u64 域
  （`static_cast<u64>(entry->tstamp) + expiry_secs <= now`）。
  三处同款防御一并推进：
  - `cask.cpp` `get()` 过期判定
  - `cask_iter.cpp` 迭代器过期跳过
  - `merge_policy.cpp` `expiry_cutoffs`（原代码本就借 u64 中间变量计算
    `trigger_cutoff`，但 `static_cast<u32>(...)` 截断回写——已去截断）
- **`Post2106TimestampRoundTrip` 揪出的潜在 u32 截断面**：恢复路径
  （`cask_recovery.cpp`）、`ReduceJob.tstamp`（`search_types.hpp`）、
  `IndexTask.tstamp`（`thread_pool.hpp`）、`PutEvent.tstamp`
  （`plugin_api.hpp`）等全树 tstamp 载体一并 u32 → u64，杜绝任一环节把
  2128 年时间戳静默截断。

---

## [4.0.0] - 2026-07-13

### ⚠️ ABI 破坏（major bump 的原因）

- **`bitcask_options_t` 布局/大小变更 ×2**（S32 引擎字段 + S29-11-②
  导航开关）——旧编译二进制与新库二进制不兼容（旧结构体传新库 = 越界
  读）。SONAME `libbitcask.so.3` → **`.so.4`**，链接器层面隔离旧二进制。
  **源码级完全向后兼容**：新字段全部尾部追加、`bitcask_options_init()`
  全量初始化，旧代码重编即正确。

### Added（S32 向量双引擎 + S29-11-②④ + M5）

- **向量双引擎**：`vector_engine`（C/C++ API）建库时选定并持久化进
  `bitcask.meta`——`hnsw`（默认，内存档）/ `ivfrq`（IVF 磁盘段，磁盘档
  推荐，10M-100M）/ `diskann`（Vamana 图，**实验性**，真实语料验证前不
  建议生产）。引擎不符重开 → `MODE_MISMATCH`；离线切换工具
  `vec_engine_migrate`（只改 meta，首次 open 全量 fold 重建，可回滚）。
- **IVF 引擎**（BIV v2）：k-means 分簇 + int8 posting 顺序扫 + 1-bit
  RaBitQ-lite 两段粗筛 + 两级质心索引——100k/384d 查询 36.5µs、
  召回损失 ≤0.08pt（三级优化累计 5.6×）。
- **DiskANN 引擎**（BDA1）：Vamana 单层图 + 盘上节点块共置 + beam
  search；实验性定级（极端聚簇合成语料为单层图病态形态，连续分布对照
  达标；解除标注前置 = 真实语料验证）。
- **AVX2 int8 内核**（S29-11-②）：sign 技巧防 vpmaddubsw 饱和；分发链
  VNNI512→VNNI256→AVX2→标量——非 VNNI 机器（Haswell+）全 int8 路径
  激活。HNSW 混合精度建图导航：插入 +29%~+75%（100k 82s→48s），
  **recall@10 零损失**（`hnsw_build_nav_int8=0` 回退闸）。
- 召回评估基建（`bench/ann_recall_harness.hpp`：盘上真值缓存 + f32/int8
  双真值 + 三元组 bench）；`vec::DeltaLog` 插入日志单一真源。

### Changed

- **向量 ckpt 崩溃恢复有界**（S32-M1）：base rebase 双门槛
  （`vector_rebase_min_docs`，默认 256K）——恢复链重放从最坏 ~4.2M 条
  （小时级）收到 ≤320K 条（分钟级）。
- **HNSW 内存**（S32-M2）：`.qc8` 码字 mmap 化（堆 ~165B/节点，高维
  int8 码字出堆）；`clone_live` payload 外溢——merge 重建堆峰值不再
  翻倍。

### Fixed（审计 2026-07-13）

- 磁盘段结构边界校验误挂 `verify_crc` 门（可信盘模式下损坏文件 →
  mmap OOB 读 UB）：IVF cidx 无条件校验、DiskANN 查询侧 use-site guard。
- IO 循环 EINTR 重试；`parallel_for` 异常安全（捕获重抛替代
  `std::terminate`）+ build 的 fd/tmp RAII 清理。

---

## [4.1.0] - 2026-07-15

### Fixed（Phase 5/6 深度审计：资源泄漏 / 进程挂死 / 持久性，2026-07-15）

三路深读 agent + 对抗复核（推翻 1 项、细化 2 项触发窗口、亲验
fdatasync / reinterpret_cast 关键断言）。基线 641/641 ctest，落地后
ASan **644/644** + TSan 全量零告警。

- **P6-MEM-1 + P6-DL-1（进程级永久挂死，两条独立进入路径同一点）**：
  `IndexPool::submit` 先 `in_flight.fetch_add` 再 `queue_.push`（TBB
  有界队列分配可抛 bad_alloc），抛出即泄漏计数 → `flush()` 谓词永假。
  加上 `close()` 的 30s 逃生门后紧接 `unregister_lib` 无超时 `flush()`，
  逃生门被 25 行后的等待抵消。**修复**：submit 的 push 套 try/catch、
  catch 内 `dec_in_flight(lane)` 后重抛（照抄既有补偿）；`flush` 加
  `optional<ms>` 超时参数，拆卸路径 `unregister_lib` 传 30s（搜索读
  屏障语义不变——超时只上在拆卸路径，不做全局）；超时路径归还 ring
  占用的全局名额防池损坏。
- **P6-MEM-2**：`RowChunks::ensure_slot` 用 `unique_ptr` 暂存再
  `release`，防 `push_back` 抛出时已分配槽泄漏。
- **P6-MEM-3**：`MmapSegment::open` 的 `new` 提至 `::open` **之前**
  （非 RISK_REPORT 建议的 mmap 之前——fd 已在手、close 在 mmap 之后，
  提到 mmap 前仍漏 fd；提到 open 前才无窗口，new 是本函数唯一抛出点）。
- **P6-DUR-1（持久性）**：`hnsw.cpp` 三处 FILE* 原子写
  （`save` / `save_vec_payload` / `write_bcq8`）rename 前补
  `fflush` + `::fdatasync(::fileno(f))` 且**两个返回值都检查**
  （disk-full 下 fflush 失败而 fdatasync 对已落盘部分成功 → 静默
  rename 出半截文件）。此前全库 9 站点中 6 个遵守 sync 纪律、唯独
  hnsw 这 3 处无任何 sync——崩溃后旧好文件已被 rename 覆盖。
- **P5-MEM-1**：`OrdSkipGuard` 析构 try-catch 防 `std::terminate`
  （析构期间 predicate 抛出）。
- **P5-MEM-2**：`last_ckpt_ord_` 三处漏更新（checkpoint 水位推进
  不一致）。
- **P5-DL-3**：删除死代码 `flush_upto`（无引用）+ reducer 每任务
  通知块（`flush_cv_` 仅剩 `dec_in_flight` 1→0 单点 notify，恢复
  方法体谓词也无人唤醒）。

### Changed（Phase 5/6 深度审计：冗余收敛 / 死代码，2026-07-15）

- **file_util.hpp 归并（T21，P6-RED-1/2）**：新增 `read_file_bytes`
  + `atomic_write_bytes` + `AtomicFileWriter` RAII（header-only
  inline，33 → 175 行）。整读 ×6 站点 + 原子写 ×9 站点归并；**fsync
  纪律从 4 套收敛为 1 套**（fflush 与 fdatasync 两个返回值都检查；
  field_schema 的 `::fsync` → `fdatasync`）。刻意不做目录 fsync
  全面铺开——保持纯重构，留 Phase 7 专项。~100 行回收。
- **Analyzer 双出口归并（T22，P6-RED-4）**：抽 `ngram_collect`
  （含全部过滤语义）+ `materialize_and_filter` + `whitespace_tokenize`，
  把 S29-8 注释断言「term 集与 tf 值逐位一致」变成 **+3 对拍测试**
  （覆盖 CJK/拉丁/混排/标点 + 停用词 + min/max 参数矩阵），**经变异
  测试验证有效**（三种单边分叉全抓）。明确否决 Jieba 先例的物化 token
  向量——会抵消 S29-8 的全部收益。
- **本地别名收口（T17）**：`field_schema` + `hnsw` 消除本地 `FilePtr`
  / `pwrite_all` 别名（T10 RED-2 真正收尾）。
- **死代码清理（T25，P6-RED-6）**：删 `ends_with_vowel` /
  `newest_folder_epoch_` / `kValueSizeOverflow`（全树仅定义零读）。

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

[Unreleased]: https://github.com/davidalphafox/libbitcask/compare/11.1.0...HEAD
[11.1.0]: https://github.com/davidalphafox/libbitcask/compare/11.0.0...11.1.0
[11.0.0]: https://github.com/davidalphafox/libbitcask/compare/10.0.0...11.0.0
[10.0.0]: https://github.com/davidalphafox/libbitcask/compare/9.1.0...10.0.0
[9.1.0]: https://github.com/davidalphafox/libbitcask/compare/9.0.0...9.1.0
[9.0.0]: https://github.com/davidalphafox/libbitcask/compare/8.1.0...9.0.0
[8.1.0]: https://github.com/davidalphafox/libbitcask/compare/8.0.2...8.1.0
[8.0.2]: https://github.com/davidalphafox/libbitcask/compare/8.0.1...8.0.2
[8.0.1]: https://github.com/davidalphafox/libbitcask/compare/8.0.0...8.0.1
[8.0.0]: https://github.com/davidalphafox/libbitcask/compare/7.0.3...8.0.0
[3.1.0]: https://github.com/davidalphafox/libbitcask/compare/v3.0.0...v3.1.0
[3.0.0]: https://github.com/davidalphafox/libbitcask/compare/v1.2.0...v3.0.0
[1.2.0]: https://github.com/davidalphafox/libbitcask/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/davidalphafox/libbitcask/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/davidalphafox/libbitcask/releases/tag/v1.0.0
