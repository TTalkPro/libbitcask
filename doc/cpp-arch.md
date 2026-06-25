# C++ 架构

本文档介绍 libbitcask 的 C++ 代码库，并说明各层之间如何协同工作。请结合 [`format-zh.md`](format-zh.md)（磁盘格式规范）一起阅读。

## 模块布局

```
.
├── include/bitcask/         # 公共头文件（API 接口）
│   ├── format.hpp           # 磁盘格式常量（带类型记录：kDoc/kTombstone + ord）
│   ├── codec.hpp            # 数据/提示记录编解码（23B 数据头，18B 提示）
│   ├── io.hpp               # PosixFile + IoError
│   ├── data_file.hpp        # DataFile：追加/读取/折叠带类型记录
│   ├── hint_file.hpp        # HintFile：写入/验证提示记录
│   ├── scanner.hpp          # scan_dir：列出 .bitcask.data 文件
│   ├── keydir.hpp           # KeyDir：内存键目录 + 迭代器（MVCC 兄弟链，256 分片）
│   ├── keydir_registry.hpp  # KeyDirRegistry：命名 keydir 缓存（引用计数共享）
│   ├── merge_policy.hpp     # decide() 规则 + 每文件阈值
│   ├── merger.hpp           # Merger：合并执行（重写活跃记录）
│   ├── cask.hpp             # Cask：终端用户 KV + 搜索门面（open/get/put/remove/put_doc/search*/merge/make_iter）
│   ├── meta_file.hpp        # bitcask.meta：模式 + 向量配置持久化（18 B）
│   ├── meta_codec.hpp       # DocValue v3 编解码（text/meta/vector/fields 段）
│   ├── meta_filter.hpp      # meta 字段过滤表达式（搜索后过滤）
│   ├── field_schema.hpp     # FieldSchema：字段名↔id 追加注册表（DocValue v3 字段）
│   ├── index.hpp            # Index：内存文档侧表（ext2ord/slots/ord2ext/live）
│   ├── live_checker.hpp     # live 集合批量维护（HNSW / 搜索死文档过滤）
│   ├── inverted.hpp         # InvertedIndex：BM25 倒排索引（按字段隔离 + 分片锁）
│   ├── inverted_wal.hpp     # InvertedWal：倒排索引 WAL + 批量刷新
│   ├── bm25_kernels.hpp     # DAAT BM25 评分内核
│   ├── intersect.hpp        # 倒排链交集（k-way / BlockMax 等）
│   ├── vbyte.hpp            # varint / vbyte 编码
│   ├── query.hpp            # 查询 AST（bool / phrase / near / fuzzy / wildcard）
│   ├── search_layer.hpp     # SearchLayer：Index + InvertedIndex + HnswIndex + Analyzer
│   ├── search_cache.hpp     # 查询侧缓存（分析结果 / term ord）
│   ├── search_checkpoint.hpp # search.ckpt 分段快照（恢复用）
│   ├── highlighter.hpp      # 搜索命中高亮
│   ├── synonym_map.hpp      # 同义词词典（查询时展开）
│   ├── fuzzy_matcher.hpp    # Levenshtein 编辑距离匹配
│   ├── wildcard_matcher.hpp # `*?` 通配符匹配（基于 trie）
│   ├── myers.hpp            # Myers 差分算法（fuzzy 用）
│   ├── porter_stemmer.hpp   # Porter 词干提取
│   ├── stemming_analyzer.hpp # 词干分析器 wrapper
│   ├── analyzer.hpp         # text::Analyzer 抽象基类 + 工厂 + AnalyzerConfig
│   ├── ngram_analyzer.hpp   # NgramAnalyzer：CJK 二/三元词 + Latin 空白分词
│   ├── jieba_analyzer.hpp   # JiebaAnalyzer：中文分词（cppjieba）
│   ├── whitespace_analyzer.hpp # WhitespaceAnalyzer：纯空白分词
│   ├── cjk_detect.hpp       # CJK 字符检测工具
│   ├── text_utils.hpp       # NFKC 标准化 + 文本工具
│   ├── hnsw.hpp             # HnswIndex：HNSW 向量索引（单写者 + 多读者无锁发布）
│   ├── thread_pool.hpp      # IndexPool：异步索引有界队列（背压）
│   ├── hw_crc32.hpp         # 硬件加速 CRC32（zlib + SIMD fallback）
│   ├── string_hash.hpp      # 字符串 hash 工具
│   ├── migrate.hpp          # 大端 → 小端离线迁移（migrate_le）
│   └── file_lock.hpp        # FileLock：独占锁（O_CREAT|O_EXCL）
├── c_api/                   # libbitcask.so 的 C ABI（extern "C"，跨语言绑定）
│   ├── bitcask_c.h          # 不透明句柄 / 错误码 / 切片 / 选项 / 函数原型
│   └── bitcask_c.cpp        # C → Cask 转换层（句柄包装 / slice ↔ span / fault 翻译）
├── src/                     # 实现（按子目录分库）
│   ├── fileops/             # codec.cpp, data_file.cpp, hint_file.cpp, scanner.cpp, migrate.cpp
│   ├── io/                  # posix_file.cpp
│   ├── lock/                # file_lock.cpp
│   ├── keydir/              # keydir.cpp, keydir_registry.cpp, index.cpp
│   ├── merge/               # merger.cpp, merge_policy.cpp
│   ├── cask/                # cask.cpp, meta_file.cpp
│   ├── search/              # search_layer.cpp, search_cache.cpp, highlighter.cpp
│   ├── bm25/                # inverted.cpp, inverted_wal.cpp, intersect.cpp, query_parser.cpp
│   ├── vector/              # hnsw.cpp
│   └── text/                # analyzer.cpp, jieba_analyzer.cpp
├── tests/                   # GoogleTest 单元 + 集成测试（22 个测试二进制）
├── bench/                   # Google Benchmark（cask / keydir / inverted / hnsw / gate /
│                            #               intersect_u64_proto / crc32 / analyzer + bench_main）
├── tools/                   # migrate_le（大端→小端）、gen_inert_table（NFKC 惰性区间表生成）
├── cmake/                   # BitcaskSanitizers 模块 + tsan.supp
├── third_party/             # 第三方依赖（git submodule，vendored，构建无需联网）
└── doc/                     # 架构 / 格式 / 设计文档
```

## 分层结构

```
┌────────────────────────────────────────────────────────────┐
│  C API（c_api/bitcask_c.{h,cpp}）                         │
│  libbitcask.so：extern "C" 不透明句柄 + slice + fault      │
└────────────────────────────┬───────────────────────────────┘
                               │ PIMPL
┌────────────────────────────▼───────────────────────────────┐
│  Cask (KV + 搜索门面)                                     │
│  ├─ KeyDir（256 分片 shared_mutex + MVCC 迭代器）          │
│  ├─ DataFile 缓存（pread 句柄 + 近似 LRU 淘汰）            │
│  ├─ HintFile（活跃写入器；含整文件 CRC trailer）           │
│  ├─ SearchLayer（仅索引模式）                              │
│  │   ├─ Index（ext2ord/slots/ord2ext/live 侧表）          │
│  │   ├─ InvertedIndex（按字段隔离的 BM25 倒排 + WAL）      │
│  │   ├─ HnswIndex（单写者 + 多读者无锁发布协议）           │
│  │   └─ Analyzer（ngram / whitespace / jieba / stemming） │
│  └─ MetaConfig（bitcask.meta 模式 + 向量配置持久化）       │
└────────────────────────────┬───────────────────────────────┘
                              │
┌────────────────────────────▼───────────────────────────────┐
│  fileops (codec, data_file, hint_file, scanner, migrate)   │
│  io (PosixFile, FileLock)                                  │
│  merge (Merger, PolicyOptions)                             │
│  bm25 (InvertedIndex, InvertedWal, intersect, query_parser)│
│  vector (HnswIndex)                                        │
│  text (Analyzer, NgramAnalyzer, JiebaAnalyzer)             │
└────────────────────────────────────────────────────────────┘
```

整个代码树使用 C++23，不依赖 Boost / abseil。第三方库（utf8proc / cppjieba / limonp / googletest / google-benchmark / oneTBB）均以 **git submodule** 形式 vendored 在 `third_party/`，clone 后无需手动安装，构建无需联网。GoogleTest 与 Google Benchmark 仅在 `BUILD_TESTING=ON` / `BITCASK_BUILD_BENCHMARKS=ON` 开启时编译。

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
├── bitcask.keydir.snap         # keydir 段快照（A4，可选）
├── search.ckpt                 # 索引层分段 checkpoint（P14e/P14b，可选）
├── <base>.f<N>.inv.snap        # 每字段倒排索引快照（索引模式，可选）
└── <base>.f<N>.inv.wal         # 每字段倒排索引 WAL（索引模式，可选）
```

### 文件详解

| 文件 | 数量 | 生命周期 | 用途 |
|------|-------|----------|---------|
| `bitcask.meta` | 1 | 持久化 | 魔术数 `BCME` + 版本 + 模式（0=KV，1=Index/search）+ 向量配置（VecMetric/VecDim/VecQuant）。来源：`meta_file.hpp`。18 字节。 |
| `<tstamp>.bitcask.data` | 多个 | 持久化，旧文件由 merge 删除 | 核心数据。记录序列：`CRC(4)+Type(1)+Tstamp(4)+Ord(8)+KeySz(2)+ValueSz(4)+Key+Value`（23 B 头）。追加方式，无文件级头。`<tstamp>` = 单调递增的 uint32 文件 id，永不重用。 |
| `<tstamp>.bitcask.hint` | 0..N | 持久化，与数据文件 1:1 配对 | 附带给定索引：键 + 偏移 + 总大小（无值）。加速打开时的 keydir 重建 —— 只读取键，不读取值。以 18 B 哨兵结束，其 `TotalSz` 字段携带整个文件的 CRC32。如果 CRC 校验失败，则忽略 hint 并从数据文件重建 keydir。 |
| `field.schema` | 0 或 1 | 持久化 | 仅索引模式。追加式字段名→id 注册表。每个条目：`[NameLen:u16 LE][name]`，id = 出现顺序（从 0 开始）。DocValue v3 存储字段 id 而不是内联名称。 |
| `bitcask.write.lock` | 0 或 1 | 运行时（以 RW 模式打开时创建，关闭时删除） | 通过 `O_CREAT|O_EXCL` 独占写锁。内容：`<pid> <active_data_file_path>\n`。合并器读取此文件以了解活跃写入器的活动文件并将其从合并候选中排除。过时锁通过 `kill(pid, 0)` 探测自动回收。 |
| `bitcask.merge.lock` | 0 或 1 | 运行时（合并期间持有） | 独占合并锁。**有意独立**于 write.lock —— 写入器和合并器并发运行，互不竞争。 |
| `bitcask.keydir.snap` | 0 或 1 | 持久化 | KeyDir 段快照（A4 特性）。通过避免完整数据文件扫描来加速打开。 |
| `<dir>/search.ckpt` | 0 或 1 | 持久化 | 索引层分段 checkpoint（keydir 水位 + Index 侧表 + 各字段倒排）。详见 [`recovery-unified-checkpoint-design-zh.md`](recovery-unified-checkpoint-design-zh.md)。 |
| `<base>.f<N>.inv.snap` | 0..N | 持久化 | 每字段倒排索引完整快照（仅索引模式）。`<N>` = 字段 id（0=default）。由 `InvertedIndex::save()` 写入，打开时加载。 |
| `<base>.f<N>.inv.wal` | 0..N | 持久化 | 每字段倒排索引 WAL（仅索引模式）。自上次快照以来的 add_doc/remove_doc 追加日志。快照保存后截断。 |

### 操作如何接触文件

| 操作 | 接触的文件 |
|-----------|---------------|
| `put(K,V)` | 追加记录到活跃 `.data` + 追加 hint 到活跃 `.hint` + 更新内存 keydir。索引模式：同时异步提交 IndexTask → `add_doc` → `.inv.wal` 追加 |
| `get(K)` | 查找内存 keydir → 从一个 `.data` 文件 `pread(file_id, offset)` |
| `delete(K)` | 追加墓碑记录（`type=kTombstone`）到活跃 `.data` + 墓碑 hint |
| `open` | 读取 `bitcask.meta` → 扫描所有 `.data` 文件（优先使用 `.hint` 加速，回退到完整数据扫描）→ 重建内存 keydir。索引模式：加载 `.inv.snap` + 回放 `.inv.wal` |
| `merge` | 获取 `merge.lock` → 读取 `write.lock` 获取活跃文件 id → 选择高碎片化候选 → 复制活跃记录到新的 `.data`+`.hint` 对 → CAS 更新 keydir → 删除旧文件 |
| `close` | 释放 `write.lock`（删除） |

### 关键设计要点

- **文件 id 永不重用**：`KeyDirRegistry` 在打开/关闭之间持久化 `biggest_file_id + 1`。
- **追加方式**：每个 put/delete 追加一个新记录；旧版本成为死字节。
- **两个独立锁**：写入器持有 `write.lock`，合并器持有 `merge.lock` —— 它们从不互相阻塞。
- **提示是可选/防御性的**：损坏或缺失的提示只会触发较慢的数据文件完整扫描重建。正确性从不依赖于提示。

## 双持久化：数据文件 vs 倒排 WAL

Bitcask 有**两条独立的持久化路径**，服务不同的目的。在接触写入路径之前，理解这种区别至关重要。

### 路径 1：数据文件（追加日志）—— KV 权威

每个 `put(K,V)` 向活跃 `.data` 文件追加一个带类型记录。这就是权威的 KV 存储。在 `open` 时，扫描所有 `.data` 文件（优先通过 `.hint` 附带文件）并重建内存 KeyDir。KV 数据不需要单独的 WAL —— 追加日志本身就是 WAL。

### 路径 2：倒排 WAL + 快照 —— BM25 索引恢复

BM25 倒排索引（倒排列表、词词典、位置）是 `InvertedIndex` 内部的复杂内存结构。每次重启从头重建需要重读所有数据文件并重新分析所有文本 —— 大规模下代价高昂（例如 200K 文档约需 2–5 秒）。

为了避免完整重建，索引使用**快照 + WAL** 模式：

```
open:
  load_snapshot()    → InvertedIndex::load(.inv.snap)
  enable_wal()       → 打开 .inv.wal 用于追加
  replay_wal()       → 回放自快照以来的增量 add_doc/remove_doc
                      → 成功回放后截断 WAL

运行时（每次 put）:
  put_doc(K, V)
    ├─ DataFile::write(kDoc)         ← 路径 1（KV 权威，追加）
    └─ submit_index_task(Add)        ← 异步到 IndexPool worker
         └─ SearchLayer::on_write()
              └─ InvertedIndex::add_doc(ord, terms)
                   └─ wal_->append_add_doc()  ← 路径 2（索引 WAL）

周期性保存:
  InvertedIndex::save(.inv.snap)    ← 完整状态到磁盘
  truncate_wal()                     ← 快照是权威的，WAL 清空

崩溃恢复:
  load_snapshot() + replay_wal()    → 索引当前到崩溃点
```

**为什么索引需要单独的 WAL？** 数据文件记录包含 DocValue 编码的文本 —— 分析器的原始输入。但倒排索引是一个*派生*结构（已分词、位置索引、词排序）。WAL 捕获*分析结果*（ord + 词位置），以便恢复跳过重新分析所有文本。没有 WAL，重启要么丢失自上次快照以来添加的索引条目（搜索结果陈旧），要么需要从数据文件完整重建。

**这反映了标准搜索引擎架构**：Elasticsearch 有 translog，Lucene 有段级 WAL —— 都服务于相同的目的，即在内存索引状态和周期性完整快照之间建立桥梁。

### WAL 批量刷新（V6.2）

`InvertedWal` 支持可配置的 `batch_size`（默认=1）：

- **batch_size=1**（默认）：每个 `append_add_doc` 立即执行 `fwrite + fflush`。最大持久性，最大 `fflush` 开销。
- **batch_size>1**：条目在内存中缓冲；达到阈值时单个 `fwrite + fflush` 刷新整个批次。析构函数刷新任何剩余缓冲区。

崩溃语义：未刷新的缓冲条目会丢失。这是安全的，因为数据文件（路径 1）是 KV 权威 —— 缺失的 WAL 条目意味着索引在下次快照保存之前不会有该文档，这与不运行 WAL 时的陈旧窗口相同。

WAL 帧格式：`[4B payload_len][payload][4B CRC32]`。CRC 仅覆盖 payload。`replay()` 验证每个条目并自动截断损坏的尾部（崩溃残留的半写入最后条目）。

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
的有界队列（满则 push 阻塞做背压），由**单一 worker 线程**串行执行所有索引
变更（`on_write`/`on_delete`/`on_vector`/`set_meta`/`add_doc`）。搜索在调用
线程上跑，与 worker 并发——读路径靠「锁内拷贝、不逃逸指针、安全遍历 tbb 表、
跨线程标量原子、消费者异常兜底」等不变量保证安全，详见
[`concurrency-zh.md` §6](concurrency-zh.md)。

**SearchLayer** 自身非线程安全：写经 IndexPool 单 worker 串行，读可与之并发。

**InvertedIndex** 线程安全：内部按词哈希分片锁 + `tbb::concurrent_hash_map`
桶锁 + posting list 的 CoW —— 与 KeyDir 的分片锁是各自独立的体系。

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

`c_api/bitcask_c.{h,cpp}` 提供 `extern "C"` ABI，由 `libbitcask.so`（`SOVERSION=1`）导出，供跨语言绑定（Python / Rust / Go / Node …）使用。设计要点：

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
| 词典 | `bitcask_set_synonym_map`, `bitcask_search_result_free` |
| 迭代 | `bitcask_iter_start`, `bitcask_iter_next`, `bitcask_iter_next_batch`, `bitcask_iter_release`, `bitcask_iter_entry_free` |
| 管理 | `bitcask_status`, `bitcask_needs_merge`, `bitcask_needs_merge_free`, `bitcask_merge`, `bitcask_is_empty`, `bitcask_is_frozen`, `bitcask_flush_index` |

### 线程模型（S11：通用 C++ 库，同一 handle 多线程安全）

- **线程安全（读）**：`bitcask_get` / `bitcask_search_*`（text/phrase/bool/fields/near/fuzzy/wildcard/vector/hybrid）/ `bitcask_status` / `bitcask_is_*` / `bitcask_needs_merge` / `bitcask_flush_index`
- **线程安全（写）**：`bitcask_put` / `bitcask_delete` / `bitcask_put_doc` / `bitcask_sync` / `bitcask_close_write_file` / `bitcask_merge`——S11-W1 内部 `write_mu_` 串行化，同一 handle 多线程写安全（写在文件层本就串行 → 锁不损吞吐；更高写并发 → 按目录分片多实例）。读写并发安全（搜索 near-real-time）。
- **例外**：`bitcask_set_synonym_map`（配置类，须先于并发查询配置）；`bitcask_close`（生命周期，close 即 free 句柄，须无在途调用）；同一 `bitcask_iter_t` 不可并发（每线程一个）。
- 并行全表扫描：C++ `Cask::parallel_scan`（C-only host 可自行多线程 `bitcask_get`）。

完整契约见 [`design/thread-safety.md`](design/thread-safety.md)；原型见 [`api-c.md`](api-c.md) 与 `c_api/bitcask_c.h`。

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

# 安装
cmake --install build        # 头文件、libbitcask.{so,a}、bitcask_c.h
```

产物：

- `libbitcask.so` — 共享库，导出 C API（`SOVERSION=1`）
- `libbitcask.a` — 把全部静态归档合并为单一 `.a`
- `migrate_le` — 旧大端目录 → 小端目录的离线迁移工具
- `gen_inert_table` — NFKC 惰性区间表代码生成器（构建期自动执行）

## 添加新的 C++ 特性

1. 在 `include/bitcask/` 中放置头文件更改。保持公共 API 小 —— 内部辅助函数放在 .cpp 的匿名命名空间中。
2. 在 `src/` 下的匹配 .cpp 中实现。对可能失败的 API 使用 `std::expected`（本代码库中的每个层都这样做）。
3. 在 `tests/` 下添加单元测试（每个区域一个 .cpp）。`tests/CMakeLists.txt` 把每个测试二进制通过 `bitcask_sanitizers` 连接，以便它们在 CI 中在 ASan/UBSan/TSan 下运行。
4. 如果更改涉及 keydir 或 cask 热路径，在 `bench/` 中添加微基准。
5. 如果更改需要暴露给 C / 跨语言绑定：
   - 在 `include/bitcask/cask.hpp`（或对应模块头）增加 C++ 接口；
   - 在 `c_api/bitcask_c.h` 增加 `extern "C"` 函数原型 + 所需类型（错误码 / 选项字段）；
   - 在 `c_api/bitcask_c.cpp` 实现句柄解包 / `slice ↔ span` / `fault` 翻译三件套；
   - 对返回堆分配的函数补 `*_free` 配对。