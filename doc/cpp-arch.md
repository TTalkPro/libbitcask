# C++ 架构

本文档介绍 libbitcask 的 C++ 代码库，并说明各层之间如何协同工作。请结合 [`format-zh.md`](format-zh.md)（磁盘格式规范）一起阅读。

## 模块布局

```
.
├── include/bitcask/         # 公共头文件（API 接口）
│   ├── detail/              # 实现细节（内嵌头）
│   │   ├── file_fault.hpp        # data/hint 文件共用错误类型
│   │   ├── inert_table.hpp       # NFKC 惰性区间表（构建期生成）
│   │   ├── int8_kernels.hpp      # int8 对称量化 + VNNI 距离内核
│   │   ├── scanner.hpp           # scan_dir：bitcask 目录扫描
│   │   ├── stop_words.hpp        # 默认停用词表（中英）
│   │   └── thread_local_buffer.hpp # 热路径读缓冲 thread_local 复用
│   ├── byte_order.hpp        # 小端 load/store 辅助（盘格式统一 LE）
│   ├── codec.hpp             # data/hint 记录编解码 + DocValue v3 + CRC32
│   ├── format.hpp            # 磁盘格式常量（header 布局 / RecordType / DocValue flags）
│   ├── io.hpp                # PosixFile（pread/pwrite/fsync/lseek）+ IoError
│   ├── data_file.hpp         # DataFile：append/kRead/fold/sealed-mmap 零拷贝
│   ├── hint_file.hpp         # HintFile：keydir 重建加速（v3 变长 + trailer CRC）
│   ├── keydir.hpp            # KeyDir：256 分片 + 屏障 v2 + MVCC 迭代器
│   ├── keydir_registry.hpp   # KeyDirRegistry：同目录共享 keydir（refcount）
│   ├── index.hpp             # Index：DocMap（ord↔ext/live/meta）+ DocTable
│   ├── doc_table.hpp         # DocTable：查询面只读身份表接口（插件借用）
│   ├── live_checker.hpp      # LiveChecker：ord 存活/doc_len 接口
│   ├── file_lock.hpp         # FileLock：O_CREAT|O_EXCL 进程间锁
│   ├── file_fault.hpp        # （已迁 detail/file_fault.hpp，保留兼容）
│   ├── meta_file.hpp         # bitcask.meta v2：模式 + 向量配置（18B）
│   ├── meta_codec.hpp        # DocValue meta 段：key-sorted KV 二进制格式
│   ├── meta_filter.hpp       # MetaFilter：AND/OR 复合过滤树
│   ├── field_schema.hpp      # FieldSchema：字段名↔id 注册表（schema interning）
│   ├── inverted.hpp          # InvertedIndex：BM25 倒排 + WAND + postings
│   ├── bm25_params.hpp       # Bm25Params：k1/b/δ 等可调参数
│   ├── bm25_kernels.hpp      # SIMD 向量化 BM25 tf_norm 评分内核
│   ├── intersect.hpp         # k-way posting 交集（galloping / Inoue SIMD）
│   ├── vbyte.hpp             # VByte 变长整数编解码
│   ├── query.hpp             # 查询 AST（MUST/SHOULD/MUST_NOT + 短语叶）
│   ├── plugin_api.hpp        # CaskPlugin 接口 + PluginHost + OpenContext
│   ├── text_plugin.hpp       # TextPlugin：BM25 文本域插件（impl CaskPlugin）
│   ├── text_plugin_config.hpp # TextPluginConfig 配置 POD（轻量头）
│   ├── vector_plugin.hpp     # VectorPlugin：HNSW 向量域插件（impl CaskPlugin）
│   ├── vector_plugin_config.hpp # VectorPluginConfig 配置 POD
│   ├── hybrid_searcher.hpp   # HybridSearcher：BM25 + 向量 RRF 融合
│   ├── searcher.hpp          # text::Searcher / vec::Searcher / CaskHybridSearcher
│   ├── search_config.hpp     # SearchLayerConfig 聚合配置（拆分产出两插件）
│   ├── search_arena.hpp      # 进程级共享 Search 池（inter-query 并发）
│   ├── search_cache.hpp      # 查询结果 LRU（选择性失效）
│   ├── search_checkpoint.hpp # search.ckpt 分段容器（每段 CRC + footer）
│   ├── search_types.hpp      # SearchHit / SearchError / ReduceJob / kDefaultField
│   ├── ckpt_chain.hpp        # 组件 checkpoint .d 链走读/坍缩（模板收敛）
│   ├── component_ckpt.hpp    # 组件 ckpt 共用类型：ChainState / DeltaSaveResult
│   ├── docmap_ckpt.hpp       # docmap 组件 ckpt：宿主侧持久化
│   ├── index_manifest.hpp    # index.manifest：per-component commit 点
│   ├── highlighter.hpp       # 搜索命中 snippet 截取
│   ├── synonym_map.hpp       # 同义词词典（open-time 不可变）
│   ├── fuzzy_matcher.hpp     # Levenshtein 编辑距离
│   ├── wildcard_matcher.hpp  # * / ? 通配符匹配
│   ├── myers.hpp             # Myers 位并行编辑距离（fuzzy 加速）
│   ├── porter_stemmer.hpp    # Porter 词干提取
│   ├── stemming_analyzer.hpp # StemmingAnalyzer wrapper（Porter）
│   ├── analyzer.hpp          # Analyzer 抽象基类 + 工厂 + AnalyzerConfig
│   ├── ngram_analyzer.hpp    # NgramAnalyzer：CJK n-gram + 拉丁空白切分
│   ├── jieba_analyzer.hpp    # JiebaAnalyzer：中文分词（cppjieba）
│   ├── whitespace_analyzer.hpp # WhitespaceAnalyzer：纯空白切分
│   ├── cjk_detect.hpp        # CJK 字符检测（Unicode 15.0 范围）
│   ├── text_utils.hpp        # NFKC 标准化 + 文本工具
│   ├── hnsw.hpp              # HnswIndex：HNSW 图 + int8 量化 + mmap payload
│   ├── thread_pool.hpp       # IndexPool：MapReduce 异步索引流水线
│   ├── hw_crc32.hpp          # CRC32 IEEE 802.3（PCLMULQDQ + zlib fallback）
│   ├── string_hash.hpp       # 透明 hash（string_view 异构查找）
│   ├── cask.hpp              # Cask：KV + 搜索门面 + 生命周期
│   ├── merger.hpp            # Merger：合并 + CAS 重定位 + 插件广播
│   ├── merge_policy.hpp      # 纯函数策略：decide/summarize/per_file_reasons
│   └── migrate.hpp           # bitcask::migrate::migrate_be_to_le 大小端迁移
├── c_api/                   # libbitcask.so 的 C ABI（extern "C"）
│   ├── bitcask_kv.{h,cpp}    # KV / 迭代 / meta 过滤 / 生命周期 / 配置
│   ├── bitcask_text.{h,cpp}  # BM25 文本检索
│   ├── bitcask_vec.{h,cpp}   # 向量 / RRF 混合检索
│   ├── bitcask_c.h           # 聚合 include（源兼容）
│   ├── bitcask_version.h.in  # 版本头 configure_file 模板
│   └── internal.h            # 三 TU 共享助手（handle / slice↔span / fault）
├── src/                     # 实现（按子目录分库）
│   ├── fileops/             # codec.cpp, data_file.cpp, hint_file.cpp, scanner.cpp, migrate.cpp
│   ├── io/                  # posix_file.cpp
│   ├── lock/                # file_lock.cpp
│   ├── keydir/              # keydir.cpp, keydir_registry.cpp, index.cpp, docmap_ckpt.cpp
│   ├── merge/               # merger.cpp, merge_policy.cpp
│   ├── cask/                # cask.cpp, cask_iter.cpp, cask_search.cpp,
│   │                        # cask_recovery.cpp, meta_file.cpp,
│   │                        # cask_internal.hpp, legacy_ckpt.{hpp,cpp}
│   ├── search/              # text_plugin.cpp, vector_plugin.cpp,
│   │                        # hybrid_searcher.cpp, search_arena.cpp,
│   │                        # search_cache.cpp, highlighter.cpp
│   ├── bm25/                # intersect.cpp, inverted.cpp, query_parser.cpp
│   ├── text/                # analyzer.cpp, jieba_analyzer.cpp
│   └── vector/              # hnsw.cpp, hnsw_kernels.hpp（内部分距离内核声明）
├── tests/                   # GoogleTest 单元 + 集成测试
│   ├── support/             # bitcask/search_layer.{hpp,cpp}（shim 测试夹具）
│   └── 32 个测试二进制（参见 tests/CMakeLists.txt）
├── bench/                   # Google Benchmark（cask/keydir/inverted/hnsw …）
├── tools/                   # migrate_le、gen_inert_table
├── cmake/                   # BitcaskSanitizers 模块 + tsan.supp
├── third_party/             # 第三方依赖（git submodule，vendored）
└── doc/                     # 架构 / 格式 / 设计文档
```

整个代码树使用 C++23，不依赖 Boost / abseil。第三方库（utf8proc / cppjieba / limonp / googletest / google-benchmark / oneTBB / unordered_dense）均以 **git submodule** 形式 vendored 在 `third_party/`，clone 后无需手动安装，构建无需联网。GoogleTest 与 Google Benchmark 仅在 `BUILD_TESTING=ON` / `BITCASK_BUILD_BENCHMARKS=ON` 开启时编译。

## 分层结构与依赖

下图是各层的逻辑分层与编译期依赖关系（PUBLIC 链传播，PRIVATE 不外露）。命名空间在头文件里已显式标注。

```
┌─────────────────────────────────────────────────────────────────────┐
│  C API（c_api/bitcask_kv.{h,cpp} + bitcask_text + bitcask_vec +    │
│        bitcask_c.h 聚合头 + internal.h 共享助手）                  │
│  libbitcask.so：extern "C" 不透明句柄 + slice + fault（PIMPL）      │
└────────────────────────────┬────────────────────────────────────────┘
                               │ PIMPL：持 bitcask::Cask
┌────────────────────────────▼────────────────────────────────────────┐
│  Cask（KV + 搜索门面）                                              │
│  include/bitcask/cask.hpp（Cask / CaskOptions / CaskFault /          │
│  CaskIter / GetResult[View] / DocInput / StatusInfo / …）            │
│  src/cask/{cask,cask_iter,cask_search,cask_recovery}.cpp + meta_file│
│                                                                     │
│  ├─ KeyDir（256 分片 shared_mutex + MVCC 迭代器）                  │
│  │   include/bitcask/keydir.hpp（SingleEntry/MultiEntry/EntryProxy │
│  │   /IterHandle/KeyDirInfo/FStatsEntry）+ keydir_registry.hpp      │
│  │   src/keydir/keydir.cpp + keydir_registry.cpp                   │
│  ├─ DataFile 缓存（pread 句柄 + 近似 LRU 淘汰）                    │
│  │   include/bitcask/data_file.hpp（DataFile/ReadRecord/WriteResult）│
│  │   src/fileops/data_file.cpp                                     │
│  ├─ HintFile（活跃写入器 + v3 trailer CRC + sealed-mmap hint）      │
│  │   include/bitcask/hint_file.hpp                                 │
│  │   src/fileops/hint_file.cpp                                     │
│  ├─ DocMap（Index：ord↔ext/live/meta 宿主服务；查询面 DocTable）   │
│  │   include/bitcask/index.hpp + doc_table.hpp + live_checker.hpp  │
│  │   src/keydir/index.cpp + keydir/docmap_ckpt.cpp（宿主侧持久化） │
│  ├─ 插件分发表 plugins_（CaskPlugin ×：写/恢复/merge/ckpt 广播）   │
│  │   include/bitcask/plugin_api.hpp                                │
│  │   ├─ TextPlugin  "bm25"   src/search/text_plugin.cpp            │
│  │   │   （倒排/Analyzer/缓存/高亮/bm25.ckpt 文件族）             │
│  │   └─ VectorPlugin "hnsw"  src/search/vector_plugin.cpp          │
│  │       （HNSW/归一化/vec.ckpt 族 + .vec/.qc8 侧车）             │
│  ├─ HybridSearcher（RRF 融合器；持两插件引用）                     │
│  │   include/bitcask/hybrid_searcher.hpp + src/search/hybrid_searcher.cpp│
│  ├─ CaskPluginHost（read_at / run_serialized / log 窄反向接口）    │
│  ├─ MetaConfig（bitcask.meta v2 + 向量配置 + CRC32）               │
│  │   include/bitcask/meta_file.hpp + meta_codec.hpp                │
│  │   src/cask/meta_file.cpp                                        │
│  └─ IndexPool（异步索引 MapReduce，借自 KeyDirRegistry）           │
│      include/bitcask/thread_pool.hpp                                │
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
│                                                                     │
│  fileops        include/{codec,data_file,hint_file}.hpp             │
│                 + detail/{file_fault,scanner}.hpp                   │
│                 src/fileops/{codec,data_file,hint_file,             │
│                              scanner,migrate}.cpp                  │
│                 → codec 纯函数 + 文件抽象 + 目录扫描 + 大端迁移     │
│                                                                     │
│  io / lock      include/bitcask/{io,file_lock}.hpp                 │
│                 src/io/posix_file.cpp + src/lock/file_lock.cpp      │
│                 → PosixFile + FileLock（O_EXCL 仲裁）               │
│                                                                     │
│  merge          include/bitcask/{merger,merge_policy}.hpp           │
│                 src/merge/{merger,merge_policy}.cpp                 │
│                 → 纯函数策略 + 执行（CAS 重定位 + 插件广播）        │
│                                                                     │
│  bm25           include/bitcask/{inverted,bm25_params,              │
│                                  bm25_kernels,intersect,            │
│                                  vbyte,query}.hpp                   │
│                 src/bm25/{inverted,intersect,query_parser}.cpp     │
│                 → 倒排 + WAND + k-way 交集 + 查询 AST               │
│                                                                     │
│  text           include/bitcask/{analyzer,ngram_analyzer,           │
│                                  jieba_analyzer,whitespace_analyzer,│
│                                  stemming_analyzer,porter_stemmer,  │
│                                  cjk_detect,text_utils}.hpp        │
│                 src/text/{analyzer,jieba_analyzer}.cpp              │
│                 → Analyzer 实现 + 工厂 + 停用词                     │
│                                                                     │
│  vector         include/bitcask/{hnsw}.hpp + detail/int8_kernels.hpp│
│                 src/vector/{hnsw.cpp, hnsw_kernels.hpp}             │
│                 → HNSW 图 + int8 量化 + mmap payload + 距离内核     │
│                                                                     │
│  工具           include/bitcask/{hw_crc32,string_hash,               │
│                                  highlighter,synonym_map,           │
│                                  fuzzy_matcher,myers,               │
│                                  wildcard_matcher,                  │
│                                  search_arena,search_cache,         │
│                                  search_checkpoint,ckpt_chain,      │
│                                  component_ckpt,index_manifest,     │
│                                  docmap_ckpt,byte_order,migrate}.hpp │
│                 → 校验/缓存/高亮/查询基础设施                       │
└─────────────────────────────────────────────────────────────────────┘
```

### 关键依赖图

```
Cask (cask.hpp)
 ├─ KeyDir/KeyDirRegistry ────────┐
 ├─ DataFile/HintFile ────────────┤── fileops ──┬── codec (纯函数)
 ├─ DocMap (Index/DocTable) ──────┤              ├── PosixFile (io)
 ├─ TextPlugin ───────────────────┤              └── FileLock (lock)
 │    └─ InvertedIndex ───────────┤── bm25
 │    └─ Analyzer ────────────────┤── text (ngram/jieba/whitespace)
 │    └─ SearchCache/Highlighter ─┤── 工具
 ├─ VectorPlugin ─────────────────┤
 │    └─ HnswIndex ───────────────┤── vector
 ├─ HybridSearcher ───────────────┘── 持两插件引用
 ├─ IndexPool (MapReduce) ────────┘── thread_pool
 ├─ Merger ───────────────────────┘── merge
 └─ MetaFile ─────────────────────┘── meta_file/meta_codec
```

- **插件化架构**（`plugin_api.hpp`）：`CaskPlugin` 接口是 KV 存储层与索引层之间的唯一契约。`Cask` 在写/恢复/merge/checkpoint 四条通路上向注册的插件广播事件；插件要变异自身单写者状态时经 `PluginHost::run_serialized` 投递到 reducer 静止点。Text/Vector 是当前两个内建插件；新增插件（TTL、metrics、CDC）只需实现此接口。
- **DocMap 宿主服务**：所有插件都借用 `Cask::docmap()`（实现为 `index::Index`）做身份翻译（ord↔ext、live 过滤、meta 过滤）。reducer 在插件之前先 apply docmap，保证插件收到的事件里 docmap 行已落地。
- **配置拆分**：`CaskOptions::search_config`（`SearchLayerConfig`）由 `text_config()` / `vector_config()` 拆成两插件独立配置，分别喂给 `TextPluginConfig` / `VectorPluginConfig`。配置层只依赖 POD 头（`text_plugin_config.hpp` / `vector_plugin_config.hpp`），不拖入插件实现。

## 插件架构（S18/S19 拆分）

聚合类 `SearchLayer` 已按插件化设计（设计稿 [`plugin-arch-split-design-zh.md`](plugin-arch-split-design-zh.md)）拆分为三个目标：

| 目标 | 头 | 实现 | 职责 |
|---|---|---|---|
| `bitcask_text_plugin` | `text_plugin.hpp` + `text_plugin_config.hpp` | `src/search/text_plugin.cpp` + `search_cache.cpp` + `highlighter.cpp` + `search_arena.cpp` | BM25 全家：per-field 倒排（`InvertedIndex`）+ Analyzer + 查询缓存 + 高亮原文 LRU + 同义词 + `bm25.ckpt` 文件族。`name() = "bm25"` |
| `bitcask_vector_plugin` | `vector_plugin.hpp` + `vector_plugin_config.hpp` | `src/search/vector_plugin.cpp` | HNSW 图 + 写入端归一化 + merge 重建 + `vec.ckpt` 文件族 + `.vec`/`.qc8` 侧车。`name() = "hnsw"` |
| `bitcask_hybrid` | `hybrid_searcher.hpp` | `src/search/hybrid_searcher.cpp` | RRF 融合器（非插件）：持两插件引用，两路各超采 `max(k×4, 64)`，RRF(60) 融合，ord 决胜 |
| `bitcask_search` shim | （已降级为测试夹具 `tests/support/`） | `bitcask_search_shim` 仅 `tests/CMakeLists.txt` 引用 | 不再是生产库目标；保留 shim 仅供 `search_layer_test.cpp` 与 legacy 迁移测试使用 |

Cask 经 `plugin::CaskPlugin` 接口（`plugin_api.hpp`）在写/恢复/merge/checkpoint 四条通路广播事件。插件在 `on_put` / `on_delete` / `on_relocate` / `on_merge_commit` / `flush` 等回调里被 reducer 单写者驱动，事件严格按 ord 升序到达。

## 磁盘文件清单

一个 bitcask 实例是一个扁平目录，包含以下文件。详细的字节级规范请参阅 [`format-zh.md`](format-zh.md)。

```
<dir>/
├── bitcask.meta                # 二进制元数据（模式 + 向量配置，18 B）
├── <tstamp1>.bitcask.data      # 追加数据文件（可以有多个）
├── <tstamp1>.bitcask.hint      # 数据文件的附带给定索引（每个数据文件一个，可选）
├── <tstamp2>.bitcask.data
├── <tstamp2>.bitcask.hint
├── ...
├── field.schema                # 字段名→id 注册表（仅索引模式）
├── bitcask.write.lock          # 由活跃写入器持有（独占）
├── bitcask.merge.lock          # 由活跃合并器持有（独占）
├── kv.keydir.ckpt              # keydir 段快照（A4，可选）
├── docmap.ckpt                 # docmap 组件 checkpoint（S17-2，可选）
├── docmap.ckpt.d<seq>          # docmap delta 链（可选）
├── bm25.ckpt                   # BM25 组件 checkpoint（可选）
├── bm25.ckpt.d<seq>            # BM25 delta 链（可选）
├── vec.ckpt                    # HNSW 组件 checkpoint（可选）
├── vec.ckpt.d<seq>             # HNSW delta 链（可选）
├── <dir>.vec                   # HNSW 向量 payload（V7 BCVS v2，mmap）
├── <dir>.qc8                   # HNSW int8 码字 payload（可选）
├── index.manifest              # per-component commit point（S17-2）
└── search.ckpt                 # 旧统一容器（P14e/P14b，S17-5 一次性迁移后消失）
```

> S17-2 之前是单一 `search.ckpt`；S17-2 拆为 per-component 三件套 + `index.manifest` commit 点；旧库 open 时触发 `Cask::migrate_legacy_search_ckpt` 一次性迁移。详细设计见 [`recovery-unified-checkpoint-design-zh.md`](recovery-unified-checkpoint-design-zh.md)。

### 文件详解

| 文件 | 数量 | 生命周期 | 用途 |
|------|-------|----------|------|
| `bitcask.meta` | 1 | 持久化 | 魔术数 `BCME` + version=2 + 模式（0=KV，1=Index/search）+ 向量配置（VecMetric/VecDim/VecQuant/InmemInt8）。来源：`meta_file.hpp`。18 字节，最后 4 字节是覆盖前 14 字节的 CRC32。 |
| `<tstamp>.bitcask.data` | 多个 | 持久化，旧文件由 merge 删除 | 核心数据。记录序列：`CRC(4)+Type(1)+Tstamp(4)+Ord(8)+KeySz(2)+ValueSz(4)+Key+Value`（23 B 头）。追加方式，无文件级头。`<tstamp>` = 单调递增的 uint32 文件 id，永不重用。 |
| `<tstamp>.bitcask.hint` | 0..N | 持久化，与数据文件 1:1 配对 | 附带给定索引：键 + 偏移 + 总大小（无值）。S23-A1 起写端恒 v3（文件头 magic "BCH3" + 变长 vbyte 记录 + 8 B trailer "BCHE" + running_crc）；读端按文件头 magic 分派 v2/v3。校验不过 → caller 退回 fold(data) 重建。 |
| `field.schema` | 0 或 1 | 持久化 | 仅索引模式。追加式字段名→id 注册表。S12-3 起 8 B 文件头 `[magic:"FSCH" u32 LE][version:1 u32 LE]` + 每条 `[NameLen:u16 LE][name][CRC32:u32 LE]`；CRC 覆盖 `[len\|name]`。来源：`field_schema.hpp`。open 时识别 legacy 无头格式并原子升级到带头格式。 |
| `bitcask.write.lock` | 0 或 1 | 运行时（以 RW 模式打开时创建，关闭时删除） | 通过 `O_CREAT|O_EXCL` 独占写锁。内容：`<pid> <active_data_file_path>\n`。合并器读取此文件以了解活跃写入器的活动文件并将其从合并候选中排除。过时锁通过 `kill(pid, 0)` 探测自动回收。 |
| `bitcask.merge.lock` | 0 或 1 | 运行时（合并期间持有） | 独占合并锁。**有意独立**于 write.lock —— 写入器和合并器并发运行，互不竞争。 |
| `kv.keydir.ckpt` | 0 或 1 | 持久化 | KeyDir 段快照（A4 特性）。通过避免完整数据文件扫描来加速打开。 |
| `docmap.ckpt` / `docmap.ckpt.d<seq>` | 各 0..1 | 持久化 | DocMap 组件 checkpoint 与 delta 链（S17-2 拆分）。宿主侧驱动，写入 docmap + keydir 元数据 delta。 |
| `bm25.ckpt` / `bm25.ckpt.d<seq>` | 各 0..1 | 持久化 | BM25 倒排组件 checkpoint 与 delta 链。 |
| `vec.ckpt` / `vec.ckpt.d<seq>` | 各 0..1 | 持久化 | HNSW 图组件 checkpoint 与 delta 链。 |
| `<base>.vec` / `<base>.qc8` | 各 0..1 | 持久化 | HNSW 向量 f32 payload + int8 码字 payload（V7 BCVS v2）。S14-2/S14-8 引入前缀不变追加契约 + payload 代号校验。 |
| `index.manifest` | 1 | 持久化 | per-component commit 点。magic "BCMF"，记录每个组件的 `{base_watermark, chain_seq, chain_watermark}` + footer CRC；唯一原子提交点（tmp+rename）。 |

### 操作如何接触文件

| 操作 | 接触的文件 |
|-----------|---------------|
| `put(K,V)` | 追加记录到活跃 `.data` + 追加 hint 到活跃 `.hint` + 更新内存 keydir。索引模式：同时异步提交 IndexTask → reducer 单写者广播到 plugins（docmap 行/删 + TextPlugin on_put/VectorPlugin on_put） |
| `get(K)` | 查找内存 keydir → 从一个 `.data` 文件 `pread(file_id, offset)`（sealed-mmap 命中时走 mmap 零拷贝读，否则 pread） |
| `delete(K)` | 追加墓碑记录（`type=kTombstone`）到活跃 `.data` + 墓碑 hint + 索引模式异步 docmap 软删 |
| `open` | 读取 `bitcask.meta` → 扫描所有 `.data` 文件（优先使用 `.hint` 加速，回退到完整数据扫描）→ 重建内存 keydir。索引模式：加载 `index.manifest` → per-component ckpt 链重放 → 从 keydir 水位起 fold(data) 增量补齐 |
| `merge` | 获取 `merge.lock` → 读取 `write.lock` 获取活跃文件 id → 选择高碎片化候选 → 复制活跃记录到新的 `.data`+`.hint` 对 → CAS 更新 keydir → 触发插件 on_merge_commit（VectorPlugin rebuild + TextPlugin compact）→ 删除旧文件 |
| `close` | 释放 `write.lock`（删除）+ 自动 ckpt（触发 flush + 各组件 ckpt 落盘 + index.manifest commit + keydir 快照） |

### 关键设计要点

- **文件 id 永不重用**：`KeyDirRegistry` 在打开/关闭之间持久化 `biggest_file_id + 1`。
- **追加方式**：每个 put/delete 追加一个新记录；旧版本成为死字节。
- **两个独立锁**：写入器持有 `write.lock`，合并器持有 `merge.lock` —— 它们从不互相阻塞。
- **提示是可选/防御性的**：损坏或缺失的提示只会触发较慢的数据文件完整扫描重建。正确性从不依赖于提示。
- **插件 checkpoint 自洽**：每个组件自己管自己的 base + delta 链 + 记账；宿主只驱动 docmap 持久化，bm25/vec 由各自插件在 `flush()` 里落盘 + 回执 `chain_seq/chain_wm` 给宿主写 manifest。
- **payload 前缀不变契约**：`.vec` / `.qc8` 走追加路径时仅写 `[vec_file_.count, count_)` 区间，崩溃残留的尾部垃圾在下次 `save` 时被覆盖。

## 双持久化：数据文件 vs 倒排 WAL

Bitcask 有**两条独立的持久化路径**，服务不同的目的。在接触写入路径之前，理解这种区别至关重要。

### 路径 1：数据文件（追加日志）—— KV 权威

每个 `put(K,V)` 向活跃 `.data` 文件追加一个带类型记录。这就是权威的 KV 存储。在 `open` 时，扫描所有 `.data` 文件（优先通过 `.hint` 附带文件）并重建内存 KeyDir。KV 数据不需要单独的 WAL —— 追加日志本身就是 WAL。

### 路径 2：插件组件 checkpoint（base + delta 链）—— 索引恢复

BM25 倒排索引（倒排列表、词词典、位置）和 HNSW 图是 `TextPlugin` / `VectorPlugin` 内部的复杂内存结构。每次重启从头重建需要重读所有数据文件并重新分析所有文本 —— 大规模下代价高昂（例如 200K 文档约需 2–5 秒）。

为了避免完整重建，索引使用**per-component base + delta 链 + manifest commit**模式：

```
open (索引模式):
  read_meta               → 决定模式 + 向量配置
  read_manifest           → 加载 index.manifest 取得每组件 {base_wm, chain_seq, chain_wm}
  load_component_base     → TextPlugin::load_component() / VectorPlugin::load_component()
                            / docmap_ckpt::load()（base wm 校验，失败退 .prev）
  load_component_delta    → ckpt_chain 走读 .d1..d<chain_seq>（每条 delta 含
                            kDeltaInfo 三元组校验）
  replay 重叠区           → 从 keydir 水位起 fold(data) 增量补齐
  watermark               → 报告给 reducer：≥ watermark 的事件需 fold 重放

运行时（每次 put_doc）:
  put_doc(K, DocInput)
    ├─ DataFile::write(kDoc)         ← 路径 1（KV 权威，追加）
    └─ submit_index_task(Add)        ← 异步到 IndexPool（MapReduce）
         └─ map worker (并行分词)
              └─ TextPlugin::prepare()  → 各插件 produce TextPrepared
         └─ reducer 单写者按 ord 序 apply
              └─ docmap.put_doc / remove（reducer 起点：宿主先 apply）
              └─ TextPlugin::on_put() / VectorPlugin::on_put()

周期性保存:
  flush()                  → 各 plugin::FlushRequest → 落盘 ckpt + 写 manifest commit
                            （keydir 快照仅 base 路径下写）

崩溃恢复:
  read_manifest + 链重放 + keydir 水位后 fold 重叠区 → 索引当前到崩溃点
```

**为什么索引需要单独的 checkpoint？** 数据文件记录包含 DocValue 编码的文本 —— 分析器的原始输入。但倒排索引是一个*派生*结构（已分词、位置索引、词排序）。checkpoint 链捕获*分析结果*（ord + 词位置），以便恢复跳过重新分析所有文本。没有 checkpoint 链，重启要么丢失自上次 base 以来添加的索引条目（搜索结果陈旧），要么需要从数据文件完整重建。

**这反映了标准搜索引擎架构**：Elasticsearch 有 translog，Lucene 有段级 WAL —— 都服务于相同的目的，即在内存索引状态和周期性完整快照之间建立桥梁。

## 并发模型

运行时存在三层锁；在添加代码之前，了解你在哪一层下。

| 层            | 类型                  | 持有者               | 保护对象                        |
|------------------|-----------------------|-----------------------|---------------------------------|
| `bitcask.write.lock`  | flock(2) 文件锁 | 一个写入进程    | 活跃数据文件的尾部     |
| `bitcask.merge.lock`  | flock(2) 文件锁 | 一个合并进程    | 合并输出文件              |
| `KeyDir` 分片锁 + `meta_mu_` + 写者闸门 | 256 把分片 `std::mutex` + 1 把 `meta_mu_`（shared_mutex）+ `barrier_mu_`/`gate_mu_` 屏障 | 热路径每线程 1 把分片锁；fold 走屏障排干写者 | entries 分片、全局标量、fold 协调状态 |

两个 flock 文件是**独立的** —— 持有 `merge.lock` 的合并器不会阻塞持有 `write.lock` 的写入器，反之亦然。这是 M5.1 双锁模型。合并器读取 `write.lock` 的内容以了解活跃写入器正在追加到哪个文件 id，并将其从合并候选中排除。

> **KeyDir 内部锁全序**（M6 屏障 v2）：
> `barrier_mu_ → gate_mu_ → meta_mu_ → 单个 shard（任意时刻 ≤1 把） → fstats_grow_mu_`
> 热路径（无 fold）：get/put/remove 单分片 mutex + relaxed 原子，至多一把锁。
> fold 的 start/release/save_snapshot/load_snapshot 走 `BarrierGuard` 写者闸门：置 `barrier_active_` 后逐分片加锁-放锁排干在途写者；写者拿到分片锁后检查闸门退避，**读者照常并发**。
> 完整锁全序、死锁防护与例外论证（含方向 ① shard→meta 和方向 ② meta→shard 的无环证明）见 [`concurrency-zh.md` 锁全局序图](concurrency-zh.md) 与 [`keydir-sharding-design-zh.md`](keydir-sharding-design-zh.md)。

KeyDir 全局标量（`epoch_` / `key_count_` / `key_bytes_` / `biggest_file_id_` / `next_ord_` / `keyfolders_`）全部 `std::atomic`，fstats 走无锁发布路径；`pending_` 与 iter 协调状态由独立的 `meta_mu_` 保护（仅 fold 期间触碰，冷路径）。

**索引层是异步单写者**：索引模式下 `put/delete` 把任务入队到 `IndexPool`
的有界队列（满则 push 阻塞做背压）。IndexPool 是 **N map worker + 1 reducer**
（S6-P4 并行 map）：map 阶段并发跑各插件 `prepare`（纯函数），reducer 按 ord
严格升序串行扇出所有索引变更（宿主 DocMap 落行/删除 + 插件 `on_put`/
`on_delete`/`set_meta`/`add_doc`）——库内单写者即 reducer。搜索在调用线程上跑，
与 reducer 并发——读路径靠「锁内拷贝、不逃逸指针、安全遍历 tbb 表、跨线程
标量原子、消费者异常兜底」等不变量保证安全，详见
[`concurrency-zh.md` §6](concurrency-zh.md)。

**搜索插件（TextPlugin/VectorPlugin）** 自身非线程安全：写经 IndexPool
reducer 串行，读可与之并发。

**InvertedIndex** 线程安全：内部按词哈希分片锁 + `tbb::concurrent_hash_map`
桶锁 + posting list 的 CoW —— 与 KeyDir 的分片锁是各自独立的体系。

**HnswIndex**（V3.3）单写者 + 多读者：`atomic<NodeChunk*>` 发布协议 + per-node seqlock；rebuild 走 `atomic<shared_ptr<HnswIndex>>` 旁路建图 + 原子换指针，旧图由引用计数续命。

## 迭代器语义（兄弟链 + 待定哈希）

当至少一个 `IterHandle` 正在迭代时（`keyfolders_ > 0`）：

1. 新键进入一个单独的 `pending_` 映射。读取首先查询 `pending_`，然后查询 `entries_`。fold 不会看到 `pending_`，因此快照保持稳定。
2. 覆盖现有键将其条目从 `SingleEntry` 提升为 `MultiEntry` —— 一个最新的兄弟链。`IterHandle::next` 在迭代器的 `iter_epoch_` 处读取，因此它看到的是 fold 开始时当前的版本，而不是后续的覆盖。
3. fold 期间的删除写入一个兄弟墓碑（哨兵值：`file_id == kMaxFileId, total_sz == kMaxSize, offset == kMaxOffset`）。

当最后一个 folder 释放时：

- `pending_` 合并回 `entries_`。
- 所有多版本条目折叠为单个版本。
- `iter_generation_` 递增；`iter_mutation_` 标志清除。

这是 bitcask 相当于内存状态的 MVCC —— 读者看到一致的快照，无需在迭代开始时复制整个映射。

## C API 导出

`c_api/`（分域头 `bitcask_kv.h` / `bitcask_text.h` / `bitcask_vec.h` + 聚合 `bitcask_c.h`，实现 `bitcask_kv.cpp` / `bitcask_text.cpp` / `bitcask_vec.cpp` + 共享 `internal.h`）提供 `extern "C"` ABI，由 `libbitcask.so`（`SOVERSION=3`，版本由根 `CMakeLists.txt` 的 `project(VERSION 3.1.0)` 单一真源生成）导出，供跨语言绑定（Python / Rust / Go / Node …）使用。设计要点：

- **不透明句柄**：`bitcask_t` / `bitcask_iter_t` 是 forward-declared struct，调用方只持有指针。
- **显式内存配对**：每个返回堆分配的函数都有对应的 `*_free`（如 `bitcask_get_result_free`、`bitcask_search_result_free`、`bitcask_iter_entry_free`、`bitcask_needs_merge_free`）。
- **错误码 + out-param**：函数返回 `bitcask_error_t`，详情经 `bitcask_fault_t*` 传出（含 errno + 512 字节 detail 缓冲，栈安全）。
- **二进制安全切片**：`bitcask_slice_t = {data, size}`，不依赖 NUL 结尾。

> 历史：项目早期是 Erlang NIF（`bitcask_cpp.so` + `nif/` 胶水 + `src/bitcask.erl` 门面，28 个 NIF）。Erlang 端已删除，C API 取而代之——同一套 C++ 核心（`bitcask::Cask`）通过 PIMPL 暴露给 C。

### 函数分组

| 组 | 函数 |
|-------|-----------|
| 版本 | `bitcask_version_{major,minor,patch,string}` |
| 生命周期 | `bitcask_open`, `bitcask_close`, `bitcask_options_init` |
| KV | `bitcask_get`, `bitcask_put`, `bitcask_delete`, `bitcask_sync`, `bitcask_close_write_file`, `bitcask_get_result_free` |
| 结构化文档 | `bitcask_put_doc` |
| BM25 搜索 | `bitcask_search_text`, `bitcask_search_phrase`, `bitcask_bool_search`, `bitcask_search_fields`, `bitcask_search_near`, `bitcask_search_fuzzy`, `bitcask_search_wildcard` |
| 向量 / 混合 | `bitcask_search_vector`（HNSW）, `bitcask_search_hybrid`（RRF 融合） |
| 词典 | 同义词经 `options.synonym_file_path`（open-time）；`bitcask_search_result_free` |
| 迭代 | `bitcask_iter_start`, `bitcask_iter_next`, `bitcask_iter_next_batch`, `bitcask_iter_release`, `bitcask_iter_entry_free` |
| 管理 | `bitcask_status`, `bitcask_needs_merge`, `bitcask_needs_merge_free`, `bitcask_merge`, `bitcask_is_empty`, `bitcask_is_frozen`, `bitcask_flush_index` |

### 线程模型（S11：通用 C++ 库，同一 handle 多线程安全）

- **线程安全（读）**：`bitcask_get` / `bitcask_search_*`（text/phrase/bool/fields/near/fuzzy/wildcard/vector/hybrid）/ `bitcask_status` / `bitcask_is_*` / `bitcask_needs_merge` / `bitcask_flush_index`
- **线程安全（写）**：`bitcask_put` / `bitcask_delete` / `bitcask_put_doc` / `bitcask_sync` / `bitcask_close_write_file` / `bitcask_merge`——S11-W1 内部 `write_mu_` 串行化，同一 handle 多线程写安全（写在文件层本就串行 → 锁不损吞吐；更高写并发 → 按目录分片多实例）。读写并发安全（搜索 near-real-time）。
- **例外**：`bitcask_close`（生命周期，close 即 free 句柄，须无在途调用）；同一 `bitcask_iter_t` 不可并发（每线程一个）。（同义词词典已改为 open-time 不可变配置 `options.synonym_file_path`，无并发竞态。）
- 并行全表扫描：C++ `Cask::parallel_scan`（C-only host 可自行多线程 `bitcask_get`）。

完整契约见 `docs/design/thread-safety.md`；原型见 [`api-c.md`](api-c.md) 与 `c_api/bitcask_c.h`。

## CMake target 列表

根 `CMakeLists.txt` 定义的库目标（PUBLIC 链接传播头路径与依赖；PRIVATE 仅内部使用）：

| 目标 | 类型 | 源文件 | 公共依赖 | 用途 |
|---|---|---|---|---|
| `bitcask_format` | STATIC | `src/fileops/codec.cpp` | ZLIB::ZLIB | codec 纯函数（data/hint 编解码、CRC32、DocValue v3） |
| `bitcask_io` | STATIC | `src/io/posix_file.cpp`, `src/lock/file_lock.cpp` | — | PosixFile + FileLock 原语 |
| `bitcask_fileops` | STATIC | `src/fileops/{data_file,hint_file,scanner,migrate}.cpp` | `bitcask_format`, `bitcask_io` | DataFile / HintFile / scanner / 大端迁移 |
| `bitcask_keydir` | STATIC | `src/keydir/{keydir,keydir_registry}.cpp` | `bitcask_fileops`, `bitcask_io`, `unordered_dense` | KeyDir + KeyDirRegistry |
| `bitcask_docmap` | STATIC | `src/keydir/index.cpp`, `src/keydir/docmap_ckpt.cpp` | `bitcask_text`, `bitcask_format` | DocMap 宿主服务 + 组件 ckpt（别名 `bitcask_index` 兼容） |
| `bitcask_plugin_api` | INTERFACE | 纯头 | — | `plugin_api.hpp`（CaskPlugin 接口） |
| `bitcask_bm25` | STATIC | `src/bm25/{intersect,inverted,query_parser}.cpp` | `bitcask_format`, TBB | 倒排索引 + WAND + k-way 交集 + 查询 AST |
| `bitcask_vector` | STATIC | `src/vector/hnsw.cpp` | `bitcask_format`, TBB | HnswIndex（HNSW 图 + int8 + mmap payload） |
| `bitcask_text_plugin` | STATIC | `src/search/{text_plugin,search_arena,search_cache,highlighter}.cpp` | `bitcask_docmap`, `bitcask_bm25`, `bitcask_text`, `bitcask_plugin_api`, TBB | TextPlugin（BM25 域）+ inter-query 搜索池 + 查询缓存 + 高亮 |
| `bitcask_vector_plugin` | STATIC | `src/search/vector_plugin.cpp` | `bitcask_vector`, `bitcask_format`, `bitcask_plugin_api` | VectorPlugin（HNSW 域） |
| `bitcask_hybrid` | STATIC | `src/search/hybrid_searcher.cpp` | `bitcask_text_plugin`, `bitcask_vector_plugin` | HybridSearcher（RRF 融合器） |
| `bitcask_merge` | STATIC | `src/merge/{merger,merge_policy}.cpp` | `bitcask_keydir`, `bitcask_fileops`, `bitcask_io`, `bitcask_format`, `bitcask_plugin_api` | 合并执行 + 纯函数策略 |
| `bitcask_text` | STATIC | `src/text/{analyzer,jieba_analyzer}.cpp` | utf8proc, cppjieba, `generate_inert_table` | Analyzer 抽象基类 + Ngram + Jieba + 工厂 + 停用词 |
| `bitcask_cask` | STATIC | `src/cask/{cask,cask_iter,cask_search,cask_recovery,meta_file,legacy_ckpt}.cpp` | `bitcask_keydir`, `bitcask_fileops`, `bitcask_io`, `bitcask_format`, `bitcask_merge`, `bitcask_hybrid`, TBB | Cask 高层门面（KV + 搜索 + 生命周期 + 元数据） |
| `bitcask_shared` | SHARED | `c_api/{bitcask_kv,bitcask_text,bitcask_vec}.cpp` | `bitcask_cask` | `libbitcask.so`（C ABI，SOVERSION 3） |
| `bitcask_static` | CUSTOM | 合并上述所有 STATIC 为单一 `libbitcask.a` | — | 静态归档 |
| `migrate_le` | EXECUTABLE | `tools/migrate_le.cpp` | `bitcask_fileops`, `bitcask_format`, `bitcask_io` | 大端 → 小端离线迁移工具（详见 `migrate-le.md`） |
| `gen_inert_table` | EXECUTABLE | `tools/gen_inert_table.cpp` | utf8proc | 构建期 NFKC 惰性区间表生成器 |

附加构建选项与目标：

- `bitcask_warnings` (INTERFACE)：所有 first-party 目标的统一告警集合（`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual -fvisibility=hidden`）。
- `bitcask_sanitizers` (INTERFACE)：由 `cmake/BitcaskSanitizers.cmake` 提供；`-DBITCASK_SANITIZE=address,undefined` 或 `=thread` 时注入 ASan/UBSan/TSan 标志（ASan 与 TSan 互斥）。
- `bitcask_search_shim` (STATIC)：仅在 `tests/CMakeLists.txt` 内定义（`tests/support/search_layer.cpp`），作为 shim 测试夹具；不参与生产库链接。
- 预编译头（PCH，默认 `BITCASK_PCH=ON`）：`bitcask_cask` / `bitcask_text_plugin` / `bitcask_keydir` / `bitcask_bm25` / `bitcask_text` 共享同一组 STL 头（`<algorithm> <cstdint> <expected> <memory> <optional> <span> <string> <string_view> <unordered_map> <vector>`）。

## 构建入口

```bash
# Release 构建（含 LTO / -falign-functions=64）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 测试 + 基准
cmake -S . -B build -DBUILD_TESTING=ON -DBITCASK_BUILD_BENCHMARKS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure

# Sanitizers（一次设置一种；ASan 和 TSan 互斥）
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug \
    -DBITCASK_SANITIZE=address,undefined -DBUILD_TESTING=ON
cmake --build build/asan -j

# TSan 构建改用 third_party/oneTBB 源码编译插桩版（系统 libtbb 未插桩会漏报/误报）
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug \
    -DBITCASK_SANITIZE=thread -DBUILD_TESTING=ON
cmake --build build/tsan -j

# -Werror 库构建（first-party 护栏，CI `werror-lib` job 开启）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DBITCASK_WERROR=ON -DBUILD_TESTING=OFF
cmake --build build -j --target bitcask_static bitcask_shared

# 仅构建大小端迁移工具
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j --target migrate_le

# 安装
cmake --install build        # 头文件、libbitcask.{so,a}、bitcask_c.h
```

产物：

- `libbitcask.so` — 共享库，导出 C API（`SOVERSION=3`，版本由 `project(VERSION ...)` 派生）
- `libbitcask.a` — 把全部静态归档合并为单一 `.a`
- `migrate_le` — 旧大端目录 → 小端目录的离线迁移工具（详见 [`migrate-le.md`](migrate-le.md)）
- `gen_inert_table` — NFKC 惰性区间表代码生成器（构建期自动执行）

## 添加新的 C++ 特性

1. 在 `include/bitcask/` 中放置头文件更改。保持公共 API 小 —— 内部辅助函数放在 .cpp 的匿名命名空间中。
2. 在 `src/` 下的匹配 .cpp 中实现。对可能失败的 API 使用 `std::expected`（本代码库中的每个层都这样做）。
3. 在 `tests/` 下添加单元测试（每个区域一个 .cpp）。`tests/CMakeLists.txt` 把每个测试二进制通过 `bitcask_sanitizers` 连接，以便它们在 CI 中在 ASan/UBSan/TSan 下运行。
4. 如果更改涉及 keydir 或 cask 热路径，在 `bench/` 中添加微基准。
5. 如果更改需要暴露给 C / 跨语言绑定：
   - 在 `include/bitcask/cask.hpp`（或对应模块头）增加 C++ 接口；
   - 在对应分域头（`bitcask_kv.h` / `bitcask_text.h` / `bitcask_vec.h`）增加 `extern "C"` 函数原型 + 所需类型（错误码 / 选项字段）；
   - 在对应实现 TU（`bitcask_kv.cpp` / `bitcask_text.cpp` / `bitcask_vec.cpp`）实现句柄解包 / `slice ↔ span` / `fault` 翻译三件套（共享助手在 `internal.h`）；
   - 对返回堆分配的函数补 `*_free` 配对。