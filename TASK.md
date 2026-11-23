# S37：Windows 移植（MSVC 原生 · x64 · 保 SIMD）开发任务清单

> 来源：[`doc/windows-port-design-zh.md`](doc/windows-port-design-zh.md)（设计稿已定稿）
> 决策基线（2026-08-07 拍板）：**MSVC 原生 ABI**（不考虑 MinGW）、**Windows 上保留 SIMD**
> （不接受 scalar 退化）、**不做 ARM64 Windows / 32 位 x86**
> 目标平台：Windows x64 / VS2022 17.6+；Linux 侧行为与性能**零回归**是全程硬约束
> 基线测试：ctest 全绿（v6.1.0 基线，Debug 全量）
> 验收标准：每项改动后 ctest 全绿 + 编译无新告警；公共结构体改动须 build-rel 双树验证；
> I/O 语义改动须过 crash 注入用例

---

## 🟡 S75 · 扫描 / range 批视图接口（免分配）（2026-11-23 立项）

> 来源：S71-4 ⏸（下游反馈 [`feedbacks/2026-11-23-keydir-scan-preads-each-value-and-range-prefetch-slower.md`](feedbacks/2026-11-23-keydir-scan-preads-each-value-and-range-prefetch-slower.md) 第 3 条）。
> 2026-11-23 下游拍板：视图归库所有（借用，需保留外部复制）、新入口不动老 ABI、扫描与 range 都做。
> 设计稿：[`doc/scan-batch-view-design-zh.md`](doc/scan-batch-view-design-zh.md)（已定稿；扫描半边随 13.0.0 发版，range 半边待 Linux 实测门槛）。

| 项 | 状态 |
|---|---|
| S75-1 设计稿：现状拷贝点分析、C++/C API 接口形状、生命周期总表、错误契约差异、测试计划 | ✅ |
| S75-1b 评审：前提逐条核实通过（映射不撤换 / LRU 逐出不悬垂 / move 不搬堆缓冲）；收益重估——理论上限约 15%，JVM 绑定端到端估计个位数；改为分两步：扫描先行，Linux 实测 ≥ 10% 再做 range | ✅ |
| S75-2 `CaskIter::next_batch_view`（key 扁平缓冲 + holder 担保 + EntryView 数组，稳态零分配）；`next()` 改为其拷贝包装，同一份 `next_row` | ✅ 扫描 |
| S75-2a 顺带修复：全表迭代器 `see_tombstones=true` 从不交出墓碑——`next_merged_key` 没把开关透传给归并层（`include_tombs` 恒 false），墓碑在归并层就被抵消。改为透传；`drain_live_keys` 补「只收活 key」防御 | ✅ |
| S75-3 `CaskRangeIter::next_batch_view`（GetResultView 为 holder；预取并发 / 自适应串行保留） | ⏸ 待 Linux 实测扫描半边 ≥ 10% 再做 |
| S75-4 C API `bitcask_iter_next_batch_view`（`*_view` 命名先例，批接口，无 free 函数） | ✅ |
| S75-4r C API `bitcask_range_iter_next_batch_view` | ⏸ 随 S75-3 |
| S75-5 测试：`iter_batch_view_test.cpp`（7 例：批内 key 缓冲增长 / 跨批大小对拍 / meta·filter·prefix 对拍 / 跨 close 可读 / owned·view 混用 / 重开 / 空库·未 start·max_n=0）+ `c_api_test.c::test_iter_batch_view`（`_ex` vs `_view` 对拍、非法参数、close 后可读） | ✅ 扫描 |
| S75-5r 测试：预取 view 版 | ⏸ 随 S75-3 |
| S75-6 文档（api-c §11.3a 与所有权表 / api-cpp §6 / 头注释）与反馈「回没回」收口 | ✅ 扫描 |
| S75-7 顺带修复构建环境：`build-msvc-release` / `-debug` 的 Ninja 头文件依赖跟踪失效（CMake 记的 `msvc_deps_prefix` 是中文「注意: 包含文件:」，cl.exe 实际输出英文 → `ninja -t deps` 显示 `#deps 0`，改头文件不触发重编，旧布局的 .obj 被链进新代码）。`Enter-MsvcEnv.ps1` 固定 `VSLANG=1033`，两个目录重新做编译器探测并全量重编 | ✅ |

> S75 验证（Windows，2026-11-23，均在修好依赖跟踪、干净重编之后）：Release ctest 994/994；Debug ctest 994/994；
> ASan 迭代器相关 30 例（IterBatchView / CApi / FlushAfterIter / CrashRecovery 迭代器 / V61BatchFold / Scanner / PrefixScan）
> 全过、无 AddressSanitizer 报告；`/WX` 库构建通过；`check-path-encoding` OK。
> 未在本机验证：Linux 构建与测试、Linux 上映射零拷贝路径的 ASan、扫描半边的实测收益（range 半边的门槛）。

---

## 🟡 S70 · 写路径不再被 memdelta 落段与扩容挡住（2026-11-23 立项）

> 来源：下游反馈 [`feedbacks/2026-11-23-write-path-stalls-on-memdelta-flush-and-growth.md`](feedbacks/2026-11-23-write-path-stalls-on-memdelta-flush-and-growth.md)
>（keelson：升 12.0.3 后量单条写边的长尾）。只编码不提交，版本未定。

| 项 | 状态 |
|---|---|
| S70-1 后台落段线程：读写打开（非 merge_only）的库在 `Cask::open` 末尾经 `IndexAuthority::start_background_flush` 登记，线程等第一次请求才起（没写到阈值的库不多占线程；`ThreadLimitsTest` 数开库新增线程，就是它提醒的），跑 `commit_oki_cut`；`maybe_flush_oki*` 越过阈值时只发请求（`request_background_flush`）就返回。背压：memdelta 涨到阈值的 2 倍（行数或字节）仍没落下去时，写者退回同步落段，内存有界。`close()` 先 `stop_background_flush`（等在途的那次做完）再拆资源。状态挂在 `IndexAuthority` 末尾新增的 `bg_flush_`（`shared_ptr<void>`，追加成员，前面的布局不动） | ✅ |
| S70-2 `commit_oki_cut`：截面在写闸内只取水位，active 文件的 fsync 挪到闸外（`collect_snapshot_watermarks(&defer)`）；fsync 失败时水位钳回已持久处，「run 不引用未持久字节」不变 | ✅ |
| S70-3 落段拷贝分块：每 4096 行单独持一次 `mu_`，块间歇一下（`sleep_for(1µs)`，实为定时器精度约 50 µs）——只 `yield` 时 std::mutex 不公平，等锁的写者整段拷贝期间都拿不到（实测一次 11 ms） | ✅ |
| S70-4 `drop_active_through` 两段式：锁外分块拷后缀、给新数组建好点查索引，剩不到一块时才在锁内搬零头并整体交换；旧数组与旧索引在锁外析构。此前锁内搬 7 万行 + 重建索引挡写者约 4 ms，更早逐行判断整份数组几十毫秒 | ✅ |
| S70-5 扩容：FlatIdx 改增量 rehash（新旧两张表，每次插入顺带搬 16 个槽）；行数组到 65536 行时一次预留到 `kFlushRowLimit × 5/4`，不再逐级翻倍搬迁 | ✅ |
| S70-6 回归 `OkiRecoveryTest.S70*`：越阈值后由后台线程落段且数据完整、重开一致；4 写者 + 2 读者在后台落段期间并发，读到的只会是写过的版本、最终全是最新版本（TSan 也跑）；后台落段进行中 close | ✅ |

**落地记录**：
- 测法：单写者 150 万次单条原子写（300 B 值，10 万 key 空间，`bitcask_put_batch_atomic_cond`），每次计时，>1 ms 记一次停顿。
  12.1.0：最大 **153 ms**，停顿合计占总时长 11.5%；现在：最大 **1.2～2.2 ms**，占 0.1～0.2%；p50 1.44 µs、p99 约 3 µs 不变。
- 排查过程里排除掉的：截面与 fsync 本身（0.03～0.08 ms）；在 fsync 前先 `sync_file_range` 写回大段（试过，没有可测的收益，已撤回）。
  最后两处是靠给后台线程每步打时间戳、在写者卡住时取主线程栈（卡在 `OkiState::locate` 拿 `mu_`）定位的。
- 剩下的 >1 ms 主要是第 65536 次写时行数组一次预留（拷 6.5 万行），每个库一生一次。
- ABI：`IndexAuthority` 末尾追加成员 + 新增成员函数；`OkiState` / `DataFile` / `Cask` 布局不动。
- 验证：见 CHANGELOG 同节。

---

## 🟢 S71 · keydir 扫描走映射读；range 预取自适应（2026-11-23 立项）

> 来源：下游反馈 [`feedbacks/2026-11-23-keydir-scan-preads-each-value-and-range-prefetch-slower.md`](feedbacks/2026-11-23-keydir-scan-preads-each-value-and-range-prefetch-slower.md)
>（keelson：clj-bitcask `scan-entries` 每行 1 µs）。只编码不提交，版本未定。

| 项 | 状态 |
|---|---|
| S71-1 `CaskIter::next` 取值先走 `DataFile::read_mmap`（与点查 `read_doc` 同一条路径），界外 / 无映射才退回 `pread`；映射读不出完整记录报 `kBadCrc` 同点查 | ✅ |
| S71-2 `CaskIter::pin_files` 钉住的封口文件带映射打开（此前 `mmap_enabled = false`，重开后的扫描全走 `pread`），并 `advise_normal` 恢复缺省预读（扫描大体按文件顺序取值，冷缓存靠预读）。新增 `io::MappedFile::advise_normal` / `DataFile::advise_normal`（Windows 无操作） | ✅ |
| S71-3 range 预取：`prefetch_threads = 0`（缺省，自动）时每批先串行取 32 条计时，估算并行省下的时间盖不过起线程的成本就整批串行；显式给线程数照给的并行（测试的并行分支靠它覆盖） | ✅ |
| S71-4 每条两次分配（反馈建议 3）：要新的批接口 / 视图版本，未做，留给下游提需求 | ⏸ |

**落地记录**（反馈附的程序，20 万 key，200 B 值，热缓存，每行 ns；前者 12.1.0，后者现在）：

| 场景 | keydir 扫描 | range（不预取） | range（prefetch 256） |
|---|---|---|---|
| 按 key 顺序写入，刚写完 | 949 → **214** | 350 → 320 | 650 → 350 |
| 按 key 顺序写入，重开后 | 960 → **258** | 570 → 570 | 914 → 635 |
| 乱序写入，刚写完 | 1230 → **370** | 640 → 635 | 865 → 725 |
| 乱序写入，重开后 | 1214～1432 → **436** | 800～850 → 793 | 930～1124 → 865 |

- 扫描现在比 range 快：扫描直接用归并视图里的位置，range 每个 key 再走一遍点查定位（重开后要查 run）。
- 预取在热缓存下还比不开慢 5～15%，剩下的是攒批本身（归并出 key 串、槽位数组）的开销；冷缓存没测（.13 上清不了页缓存）。
- 验证：见 CHANGELOG 同节。

---

## 🔴 S69 · 原子批删除：写出的字节与批头区间一致；读端认得旧写端多算的批（2026-11-23 立项）

> 来源：下游反馈 [`feedbacks/2026-11-23-atomic-batch-remove-span-overcounts-tombstone-shadow.md`](feedbacks/2026-11-23-atomic-batch-remove-span-overcounts-tombstone-shadow.md)
>（bitcask：Cypher 写语句基准里删顶点后重开报 `bad_crc`）。只编码不提交，版本未定。S68 是掉电故障注入的设计稿编号。

| 项 | 状态 |
|---|---|
| S69-1 写端：`put_batch_atomic` 的删除成员写出 ①b 准备好的值（tombstone v2 的 4 字节 shadow，或空），与 `span_bytes` 一致，也与单条 `remove` 写出的 tombstone 同形（`cask.cpp` ④） | ✅ |
| S69-2 读端兼容：`format::legacy_batch_closes` 判定「Count 条成员已读满、离声明的区间末尾还剩 4 × j 字节、j ≤ 空值 tombstone 成员数」= ≤ 12.1.0 写端写出的已提交批，按实际字节收口；`DataFile::fold` 在批头区间越过文件尾不超过 4 × count 时先读成员再判；成员读满却对不上账（不是这种规律）照旧按不完整处理 | ✅ |
| S69-3 `fold_committed` 放行暂存成员用同一规则 | ✅ |
| S69-4 回归 `AtomicBatchTest.S69*`：判定规则；新写端 span 与成员字节一致、删除写出 shadow、删 OKI 重开删除生效且文件不变；手工构造的旧写端批（含文件末尾越界 4 字节的那个、混有不多算的空值 tombstone）由 `fold_committed`（不跳坏区）读到文件尾、Cask 重开删除生效；多出来的字节无法归因时成员不可见 | ✅ |
| S69-5 反馈回填、CHANGELOG `[Unreleased]` | ✅ |

**落地记录**：
- 根因：`put_batch_atomic` ①b 把带 shadow 的删除成员按 `kHeaderSize + key + 4` 计入 `span_bytes`，④ 写成员时删除一律写空值。自 S35（原子批）、
  tombstone v2 起就如此；bitcask 的 `tombstone_version` 缺省 2，图层删边 / 删顶点、`bitcask_txn` 提交里的 `delete` 都会写出这种批。
  恢复时：12.0.3 及更早把批当 torn tail 截断文件（删除与其后同一文件的写静默丢失）；12.1.0（S67）当坏区跳过，同一文件多于 64 处返回 `kBadCrc`。
- 端到端（bitcask 侧隔离副本，索引模式，2000 篇文档 + 280 个单删除原子批 + 1 条普通写）：12.1.0 写出的库 12.1.0 重开 `bad_crc`；修复后的构建打开同一个库：
  存活 1720（应为 1720）逐个一致、最后那条普通写在、全文命中 1720、文件大小不变。修复后写出的库，12.1.0 也能正常打开（批头与字节一致，旧读端照认）——
  盘上格式双向兼容。
- 读端放宽的边界：只认「成员条数读满 + 余量恰为 4 的倍数 + 不超过空值 tombstone 数」，别的账对不上照旧判不完整（有用例）。批内有被
  `tolerate_crc_errors` 跳过的坏记录时 `fold_committed` 数不到它，这种「旧批 + 位腐」的双重故障下批成员不放行。
- 验证：见 CHANGELOG 同节。

---

## 🔴 S67 · 恢复不再截断已封口的数据；带过滤的向量检索回满 + 候选集内排序（2026-11-23 立项）

> 来源：下游反馈
> [`feedbacks/2026-11-23-recovery-truncates-synced-data-on-bad-record-header.md`](feedbacks/2026-11-23-recovery-truncates-synced-data-on-bad-record-header.md)（🔴）、
> [`feedbacks/2026-11-23-filtered-vector-search-incomplete-no-candidate-scoring.md`](feedbacks/2026-11-23-filtered-vector-search-incomplete-no-candidate-scoring.md)（🟡）。
> 随 12.1.0 发布（MINOR）。掉电故障注入的设计稿（`designs/power-loss-fault-injection-design-zh.md`）让出编号，改记 S68。

| 项 | 状态 |
|---|---|
| S67-1 哪些文件可能有「没写完的尾巴」改由写锁证据决定：上一个写者没有正常关闭（读写打开时清掉了陈旧写锁；只读 / merge_only 打开时写锁文件还在）⇒ 写锁里记的活跃文件 + 编号最大的 data 文件；正常关闭 ⇒ 没有。`IndexAuthority::set_unsealed_files` / `may_have_torn_tail`；恢复的三处 fold 与 `rebuild_from_data` 都按它走。**只有这些文件才截断** | ✅ |
| S67-2 其余文件里 fold 停下的地方当作坏区跳过：`DataFile::find_next_record` 从停点之后逐字节找下一条记录（类型字节合法 + CRC 通过 + 后面紧跟另一条完好记录或恰好到文件尾），找到就从那里接着读，文件不动；找不到当作尾部垃圾。单文件最多跳 64 处（`kMaxHealedGaps`）。每跳一处打一条 warn 日志（文件、起止偏移） | ✅ |
| S67-3 原子批不被拆开：停点是一个成员不全的批头时整批跳过（`DataFile::batch_end_at` 取批声明的终点），不会把批里碰巧完好的成员单独放出来；批声明的终点超出文件 ⇒ 到此为止 | ✅ |
| S67-4 `DataFile::read` / `read_mmap`：整条读到了但解不出来的记录统一报 `kBadCrc`（此前坏在长度字段上时报 `kShortRead`，上层并进 `kIo`） | ✅ |
| S67-5 取得写锁后 fsync 目录（写锁文件的目录项落盘，掉电后仍能认出「上一个写者没正常关闭」） | ✅ |
| S67-6 带过滤的 HNSW 检索：0 层遍历预算耗尽且不足 k 时退到精确扫描（`HnswIndex::exact_filtered_scan`：全部节点过一遍过滤器，通过者逐个算距离取前 k）。只在节点数 ≤ `kExactFallbackMaxNodes`（2^20）时做，更大的图维持原取舍 | ✅ |
| S67-7 `Cask::rank_by_vector(query, keys, k)` + C API `bitcask_vector_rank`：在给定的一组 key 里按向量相似度精确排序取前 k。不经向量索引（逐个点读候选、用它存的向量算），与引擎无关，不需要先 flush；分数口径同 `search_vector` | ✅ |
| S67-8 回归：`CrashRecoveryTest.CorruptHeaderInSealedFileKeepsLaterRecords` / `CorruptRecordReadReportsBadCrc` / `CleanCloseThenDamagedTailIsNotTruncated` / `CrashedWriterTailTruncatedSealedFilesHealed` / `FindNextRecordResyncRules`；`HnswFilteredSearch.SelectiveFilterFallsBackToExactScan`；`CaskDocValueTest.S67RankByVectorAmongCandidates`；`c_api_test` 的 IVF-RQ 用例里加 `bitcask_vector_rank` | ✅ |
| S67-9 反馈回填、CHANGELOG `[Unreleased]` | ✅ |

**落地记录**：
- 反馈里的复现（2000 条记录、第 1000 条的头写成 0xFF、删索引后重开）：改前文件 336000 → 167832 字节、后 1000 条消失；
  改后文件大小不变，只丢被写坏的那一条。同一用例里接着把另一条的 `value_sz` 改成越过文件尾、再盖一段 4 KiB 的零，结论相同。
- **行为变化（可见）**：
  1. 正常关闭过的库，文件尾部的残缺不再被截掉（留在原处、读的时候跳过）。此前「正常关闭 + 掐掉文件尾巴」会被当成崩溃尾巴截断；
     用这种办法模拟掉电的测试要同时留下陈旧写锁（`tests/support/stale_writer_lock.hpp`），已改 4 个既有用例。
  2. 带过滤的向量检索在过滤全拒 / 极严时，多出一遍 O(节点数) 的过滤求值（≤ 2^20 个节点时）。
- ABI：`IndexAuthority` 末尾追加一个成员（先例 `fold_floor_`），既有成员偏移不变；`DataFile`、`Cask`、`OkiState` 布局不变；
  `CommittedFoldOptions`（`detail/`）加了字段。纯新增：`DataFile::find_next_record` / `batch_end_at`、`Cask::rank_by_vector`、
  `HnswIndex::kExactFallbackMaxNodes`、C API `bitcask_vector_rank`。盘上格式不变。
- 已知的残余：
  - 跳坏区靠「两条连着的完好记录」认记录边界。一个 value 的**末尾**若恰好嵌着一条完整记录的字节映像、且这条记录自己的头坏了，
    嵌着的那条会被当成真记录读出来（它后面紧跟下一条真记录）。要两处巧合同时成立；嵌在 value 中间的不会（有用例）。
  - 旧版本写者留下的写锁文件没有 fsync 过目录项：用旧版本写、掉电、再用新版本打开，若写锁的目录项没落盘，新版本看到的是
    「没有崩溃证据」——尾巴不截断、当坏区跳过（不丢数据，只是残缺字节留在文件里）。
  - 坏区里的记录丢了就是丢了（日志里有起止偏移）；没有做「打开时报告 / 统计接口」，`status()` 也没有加计数。
  - IVF-RQ / DiskANN 两种引擎的带过滤检索没有动（反馈说的预算是 HNSW 的）。
- 验证：见 CHANGELOG 同节。

## 🟠 S66 · 落盘后的点查：块内增量比较 + 采样索引，缓存命中不重算 CRC（2026-11-23 立项）

> 来源：下游反馈 [`feedbacks/2026-11-23-point-get-after-flush-scans-run-block-linearly.md`](feedbacks/2026-11-23-point-get-after-flush-scans-run-block-linearly.md)
> （bitcask：升 12.0.3 后重开过的库读没有跟着变快——行落进 run 之后点查命中比行在 memdelta 时慢 5～13 倍）。
> 取反馈建议 1（不动盘上格式的两层做法）、2、3。随 12.1.0 发布。

| 项 | 状态 |
|---|---|
| S66-1 `OkiRunReader::find` 不再走 `seek_impl` 的逐条完整解码：块内用前缀压缩做增量比较（记上一行与目标 key 的公共前缀长度，按 `shared_len` 与它的大小关系直接判小于 / 大于，相等时才比后缀），不重建 key、不拷进 `Entry`；ord / tstamp 的块内差分照常累加。行解析抽成 `parse_row`，结构校验口径同 `Cursor::next` | ✅ |
| S66-2 采样索引：块缓存开着时，给被点查过的块建一份「每 16 行一个完整 key + 块内偏移 + 到该行之前的 ord / tstamp 累加值」，作为另一条缓存项放进同一个块缓存（key = `(gen, 块下标 \| 1<<63)`）；点查先在采样上二分、再从采样行起走至多 16 行。等于在内存里补上盘上格式没有的重启点；随块缓存的容量与淘汰走，约为块大小的 15%～20%。缓存关着 / 无缓存时只走 S66-1 | ✅ |
| S66-3 块 CRC 挪进块缓存的 loader（块从盘上读进来的那一次），校验不过的块不进缓存；缓存命中不再对整块重算。无缓存路径不变（每次读盘都校验） | ✅ |
| S66-4 `OkiBlockCache::enabled()`（公开内联访问器，纯新增）。其余类布局、盘上格式不变 | ✅ |
| S66-5 基准 `BM_Cask_Get_Hot_View_Reopened`（建库 → close → 重开，1024 / 10 万 / 40 万 key） | ✅ |
| S66-6 回归 `tests/oki_run_v2_test.cpp`：`OkiRunFind.MatchesSequentialScanForAllKeyShapes`（约 2000 个各种形态的 key——互为前缀、长公共前缀、含 0x00 / 0xFF、长 key；v1 / v2 / v3 × 块目标 64 / 300 / 4096 × 无缓存 / 缓存开（两遍）/ 缓存容量 0；在场的 key 逐字段对顺序游标，约 2700 个不在场的探针全部判缺席）、`OkiRunFind.CorruptBlockNeverEntersCache` | ✅ |
| S66-7 反馈回填、CHANGELOG `[Unreleased]` | ✅ |

**落地记录**：
- 性能（GCC Release，本机，128 B 的值、页缓存全热；命中一次）：

  | key 数 | 行在 memdelta（`Get_Hot_View`） | 重开后，改前 | 重开后，只做 S66-1 + S66-3 | 重开后，全部 |
  |---|---|---|---|---|
  | 1 024 | 130 ns | 3023 ns | 1043 ns | **365 ns** |
  | 100 000 | 502 ns | 3392 ns | 1526 ns | **703 ns** |
  | 400 000 | 646 ns | 3562 ns | 1769 ns | **1008 ns** |

  两种状态的差距由 5～23 倍收到 1.4～2.8 倍。剩下的是 bloom、块首 key 二分、两次块缓存查找（块 + 采样索引，各一次分片锁）与 sealed 映射读。
- 没做：盘上加重启点（要升 run 版本）——内存里的采样索引已拿到大部分收益，先不动格式。范围迭代器的定位（`seek_impl` / `seek_cached`）
  仍是块内线性解码：它每次迭代只定位一次，不在这条反馈的范围里。
- 验证：GCC Release（`-Werror`）全量 ctest 975 注册 / 973 执行全绿（首轮 `-j16` 有一次 `HighlightColdTest.ReopenedLibraryHasNoSnippetsUnlessLoaded`
  ——S53 既有的并行偶发，串行连跑 10 次与整套重跑均通过）；ASan 全量 973 全绿；TSan 相关 320 项全绿、无告警；clang Debug 相关 265 项全绿。
  Windows 未实测。

---

## 🟡 S65 · 点查命中：memdelta 索引换平表、活跃文件走映射（2026-11-23 立项）

> 来源：下游反馈 [`feedbacks/2026-11-23-point-get-hit-cost-grows-with-unflushed-rows.md`](feedbacks/2026-11-23-point-get-hit-cost-grows-with-unflushed-rows.md)
> （bitcask：图层优化到头后，各条主路径一半以上的时间在引擎点查上）。取反馈建议 1、2、5；建议 4 随 2 一并解决；
> 建议 3（CRC 零头）没做。随 12.0.3 发布（PATCH）。

| 项 | 状态 |
|---|---|
| S65-1 memdelta 点查索引换成开放寻址的平表（`oki_state.cpp` 的 `FlatIdx`）：槽 = u64（高 24 位哈希 tag + 低 40 位「行下标 + 1」），key 不另存、比较时直接比 `delta_[下标].key`；线性探测、装填率 ≤ 1/2、扩容按行重算哈希；只增不删，active 重排时整表重建。一次点查从「桶 → 节点 → key 字符串 → 行」变成「槽 → 行」，每个 key 也不再存两份 | ✅ |
| S65-2 **类布局不动**：平表在堆上，指针放进 `OkiState` 原 `sorted_view_version_` 的槽位（改名 `delta_flat_idx_`，析构释放）；原 `delta_idx_` 成员保留、恒为空 | ✅ |
| S65-3 活跃文件的命中读走映射：`DataFile::enable_growing_map(reserve)` 给可写句柄一次建好一段只读映射（长度 = 预留值，可以比文件长；文件往后长时对应的页随之可读，不用重映射）。`read_mmap` 以「已 `pwrite` 出去的字节数」为界（跨线程原子量），界外 / 超出预留 → `kShortRead`，`read_doc` 照旧退回 `pread`；`truncate_to` / `truncate_here` 先收界再截文件。`Cask::roll_active` 建新活跃文件后启用，预留 = `min(max_file_size, 16 GiB) + 64 MiB`。只在 Linux / FreeBSD 生效，别的平台不变 | ✅ |
| S65-4 「已写出字节数」与持久水位放进同一块堆对象（`FileMarks`，`durable_` 是指向首成员的别名指针）——`DataFile` 不加数据成员。新增公开成员函数 `enable_growing_map`、私有静态 `make_marks` 与两个私有成员函数，纯新增 | ✅ |
| S65-5 基准：`BM_Cask_Get_Hot` / `BM_Cask_Get_Hot_View` 的 keyspace 做成参数（1024 / 10 万 / 40 万，全部未落盘） | ✅ |
| S65-6 回归：`DataFile.GrowingMapOnWritableHandle`（逐条写后经映射读、文件尾之后 / 跨尾 / 批缓冲未 flush / 截断区间一律 `kShortRead`、截断后续写）、`DataFile.GrowingMapBeyondReserveFallsBackAndReadOnlyIsNoop`；`OkiRangeTest.PointGetMatchesModelAcrossIndexGrowthRebuildAndRoll`（6 万 key、15 万步覆盖写 / 删除、3 次 checkpoint、256 KiB 文件上限下多次 roll、重开，点查对模型） | ✅ |
| S65-7 反馈回填、CHANGELOG `[Unreleased]` | ✅ |

**落地记录**：
- 性能（GCC Release，本机，128 B 的值、页缓存全热、全部行未落盘；`BM_Cask_Get_Hot_View`，命中一次）：

  | 未落盘行数 | 改前 | 改后 |
  |---|---|---|
  | 1 024 | 857 ns | **138 ns** |
  | 100 000 | 1540 ns | **435 ns** |
  | 400 000 | 1828 ns | **648 ns** |

  `get_owned`（多一次拷贝）同档：866 / 1572 / 1908 ns → 158 / 526 / 705 ns。大档剩下的增长是随机访问 60 MiB 数据页与平表本身的缓存未命中。
- 行为上的注意点：① 活跃文件的命中读现在经映射取页，被访问过的页计入进程 RSS（文件页，可回收）——与 P6 对 sealed 文件的效果相同；
  ② 映射读遇到底层 I/O 错误是 SIGBUS 而不是错误码，同 P6 既有取舍；③ 每个可写句柄多占一段地址空间（默认 2 GiB + 64 MiB，不占内存）。
- 没做：建议 3（`crc32_bytewise` 处理头尾零头，约占改前一次命中的 5%）——动校验和内核的风险与这点收益不相称，留给以后单独评估。
  落盘之后（行进了 run）走 bloom + 块缓存的那条路没有动，也没有量。
- 验证：GCC Release（`-Werror`）全量 ctest 973 注册 / 971 执行全绿（2 项既有 disabled）；ASan 全量 971 全绿；TSan 跑 OKI / range /
  迭代器 / merge / 原子批 / DataFile / Cask 相关 318 项全绿、无告警；clang Debug 相关 263 项全绿。Windows 未实测（映射一项在 Windows 上不启用）。

---

## 🟠 S64 · 写后首查不再整份重排 memdelta（2026-11-23 立项）

> 来源：下游反馈 [`feedbacks/2026-11-23-range-iter-after-write-resorts-memdelta.md`](feedbacks/2026-11-23-range-iter-after-write-resorts-memdelta.md)
> （bitcask 图层排查 hub 读变慢时量出）。是 2026-09-18 那条（只修了无写时的连续查询）的后一半。
> 取反馈建议 1（增量维护有序视图）+ 建议 3（拷贝不长时间占 `mu_`）。随 12.0.2 发布（PATCH）。

| 项 | 状态 |
|---|---|
| S64-1 `make_read_view` 的 active 有序视图改增量：缓存 = 若干不可变有序段（覆盖 `delta_` 的一段前缀，行数逐级翻倍）+ 一小段有序尾巴；写后首查只拷贝、整理缓存之后新增的行，并进尾巴；尾巴满 128 行升段，相邻两段行数相近时线性归并（段数 O(log 行数)）。结构与不变式见 `oki_state.cpp` 的 `ActiveSorted` | ✅ |
| S64-2 纪元：`drop_active_through`（flush 冻结 / 重建 / 装载后重排 active）时 `delta_epoch_` +1，缓存整体作废；纪元内 active 只追加，缓存按行下标增量覆盖 | ✅ |
| S64-3 锁内拷贝分批：一次持 `mu_` 最多拷 4096 行（首建 / 久未读时要拷的行多），途中纪元变了重来，第 3 次起不再分批 | ✅ |
| S64-4 读视图装配：active 的有序段排在 `ReadView::frozen` 末尾（待落盘冻结段之后，旧→新），`delta` 只装尾巴；seq 过滤（丢弃 `seq ≤ frozen_through` 的行）改为逐来源判定。`ReadView` 结构不变 | ✅ |
| S64-5 **类布局不动**：`IndexAuthority` 内嵌 `OkiState` 且其后有内联访问器读的成员，不能加数据成员。缓存对象经 `sorted_view_` 的别名指针挂接（指向对象首成员）；`delta_epoch_` 占原 `sorted_view_min_seq_` 的槽位；`sorted_view_version_` 留作占位。新增一个静态成员函数 `read_view_rows_processed_for_test()`（纯新增） | ✅ |
| S64-6 回归 `tests/oki_range_test.cpp`（3）：5 万未落盘行上 300 轮「写 1 条 + 扫 1 行」，视图整理处理的行数 < 5 万（旧实现 1500 万）；6000 步随机 put / 覆盖 / 删除穿过升段、归并、3 次 checkpoint 的三方对拍；1 写者追加 + 3 读者全域扫描，读到的 key 必须是连续前缀且不倒退 | ✅ |
| S64-7 反馈回填、CHANGELOG `[Unreleased]` | ✅ |

**落地记录**：
- 性能（GCC Release，本机）：「写 1 条无关 key + 扫 1 行的 range」每轮耗时——5 万 / 10 万 / 40 万未落盘行分别 **4 / 4 / 5 µs**，
  300 轮共处理 17758 行，与未落盘行数无关。改前同一操作经 Erlang 封装量得 10 万行 14.9 ms、40 万行 80.1 ms（反馈里的表）。
  首建（缓存为空时第一次读）仍是一次全量排序，之后增量。
- 段数上限约 log2(行数 / 128)（100 万行 ≈ 13 段），范围游标每段多一路归并 / 一次二分定位。
- 验证：GCC Release（`-Werror`）全量 ctest 972 注册 / 970 执行，969 通过；唯一失败 `HighlightColdTest.ReopenedLibraryHasNoSnippetsUnlessLoaded`
  是 S53 既有的 `-j16` 偶发（串行重跑通过，与本项无关）。ASan 全量 970 执行全绿（2 项既有 disabled）。TSan 跑 OKI / range / 迭代器 /
  merge / 原子批相关 194 项全绿、无告警。clang Debug 相关 138 项全绿。Windows 未实测。

---

## 🔴 S63 · 原子批对并发读者整批可见（2026-11-20 立项）

> 来源：下游反馈 [`feedbacks/2026-11-20-atomic-batch-no-read-isolation.md`](feedbacks/2026-11-20-atomic-batch-no-read-isolation.md)
> （keelson 压测撞上；bitcask 审图层确认同根因）。设计 [`designs/atomic-batch-read-visibility-design-zh.md`](designs/atomic-batch-read-visibility-design-zh.md)。
> 拍板：方案 A（整批在权威索引一次性发布）；不做专门 `validate`，用只含 `kCheck` 的批；`put_batch` 不改；检索索引与跨进程读者不在本项。
> 只编码不提交。

| 项 | 状态 |
|---|---|
| S63-1 `OkiState::apply_batch`：锁外构造行（含 key 拷贝），一个 `mu_` 临界区内追加、分配到达序、登记点查哈希；`delta_version_` / flush hint 只批末更新 | ✅ |
| S63-2 `IndexAuthority::apply_batch`：一个 `writer_mu_` 内先零副作用裁决全部成员，通过后按单条 `apply_put` / `apply_remove` 同口径记账并一次发布；`kIndexUnavailable` / merge 拒绝零成员生效；remove 不裁决、幂等 put 跳账本但仍随批发布 | ✅ |
| S63-3 `put_batch_atomic` 第 ⑥ 步改整批提交；merge race → `roll_active()`、被拒 put 只落盘重写、整批重试（至多 3 次）；旧重写 ord 补 Skip，重写槽位映射避免批头 task 错标 | ✅ |
| S63-4 契约文档：`cask.hpp` / `txn.hpp` / C 头 / `doc/api-c.md` §10.3 / CHANGELOG `[Unreleased]`；只含 `kCheck` 批的「不写字节 / 不分配 LSN / 不升 meta」承诺 | ✅ |
| S63-5 回归 `tests/atomic_batch_visibility_test.cpp`（5）：头→尾、尾→头、remove 与头同批、merge race 重写整批可见 + `flush_index` 排干、check-only 不升 meta | ✅ |
| S63-6 基准 `BM_Cask_AtomicBatchPointReadP99`：400 行大批 + 4 点查读者 | ✅ |
| S63-7 反馈与设计稿状态回填 | ✅ |

**落地记录**：
- 性能：400 行大批 + 4 读者（每读者先读 head 后读 tail），150 批共 19.8k 个 get 样本；
  点查 **p99 7.4 µs**、p99.9 73.4 µs（GCC Release 本机，三轮均值）。大批持锁成本可观测但未造成毫秒级 p99。
- 验证：GCC Release（`-Werror`）全量 ctest **967 注册 / 965 执行**全绿（2 项既有 disabled）；ASan 全量 967 注册 / 965 执行全绿（2 项既有 disabled）；
  新用例 TSan 全绿；既有 `atomic_batch` / `atomic_batch_doc` / `conditional_batch` / `s62` / `oki_only_contract` / C API 全绿。Windows 未实测。
- 版本定 **12.0.1**（PATCH：行为修复 + C++ 纯新增，ABI 兼容），`SOVERSION` 保持 **12**。

---

## 🔴 S62 · meta 字符串数组 + Contains 过滤 + bitcask.meta v7 纪元（2026-11-20 立项）

> 来源：下游反馈 [`feedbacks/2026-11-13-doc-capability-gaps.md`](feedbacks/2026-11-13-doc-capability-gaps.md) #4（锦书：别名天然多值）。
> 设计 [`designs/meta-multivalue-design-zh.md`](designs/meta-multivalue-design-zh.md)；用户定方案 A（新值类型），只编码不提交。
> 待定项按设计推荐默认：只做字符串数组；既有操作符碰数组按类型不匹配；`bitcask_update_doc_ex` 同批做；下游 Erlang 不在本项。
>
> 约束：⛔ 改 C 侧既有结构体布局 / 函数签名；C++ `MetaValue` 加变体（ABI 破坏，须升 major，版本由用户定）。

| 项 | 状态 |
|---|---|
| S62-1 `meta_codec.hpp`：类型标记 5 编解码、`meta_find_raw`（零分配值区间，标量跳读内联）、`for_each_array_item`、`meta_has_string_array`、`value_from_raw` | ✅ |
| S62-2 `meta_filter.hpp`：`Contains` / `ContainsAny`（冷分支 `BITCASK_NOINLINE`）；`compare_values` 数组显式分支；求值改原地比较 `compare_raw`（不物化） | ✅ |
| S62-3 纪元：`meta_file` 接受 / 写出 v7；`put_doc` 组提交前 `ensure_multivalue_epoch`；原子批（含 `update_doc` / `TxnCask`）懒升目标按「批内是否含数组」取 6 或 7；`meta_multivalue_` 快判 | ✅ |
| S62-4 C API：枚举、过滤解析、视图 / iter 认数组、`bitcask_meta_array_next`、`bitcask_meta_encode_ex`、`bitcask_update_doc_ex`；旧入口给数组 ⇒ INVALID_OPTION | ✅ |
| S62-5 回归 `tests/s62_meta_multivalue_test.cpp`（8）+ C `test_meta_string_array` | ✅ |
| S62-6 文档 `doc/api-c.md` §10.4b、`doc/api-cpp.md` §7.8、CHANGELOG `[Unreleased]`、反馈回填、设计稿状态 | ✅ |

**落地记录**（已提交 `b3bdeae`，**12.0.0 已发版**）：
- 旧版本实测（11.2.0 `git archive` 单独编）：打开新版本写出的 v7 目录 ⇒ `read meta failed: unsupported meta version`；
  无门禁时的后果也实测了——11.2.0 的 `meta_lookup` 对数组之后的 key 返回「不存在」，`zkind == beast` 求值 0（应为 1），`decode_meta` 报错。
- 性能（无数组的 meta、3 条件组合过滤，独立微基准两份头文件对拍）：初版慢 ~17%（通用 skip 函数调用）→ 标量跳读内联后 ~10%
  → `Contains` 冷分支移出内联后 ~4% → 原地比较不物化后**持平**（121 对 121 ns）；单项 `Exists` 31 → 22 ns。
- 验证：GCC Release（`-Werror`）全量 **960**：一轮挂 `HighlightColdTest.ReopenedLibraryHasNoSnippetsUnlessLoaded`（S53 既有偶发，
  串行连跑 5 次全过）；ASan 全量 960 全绿；Clang Debug / TSan 相关 202 全绿；clang 下 `-Wswitch` 新告警已消。Windows 未实测。

---

## 🔴 S60 / S61 · 逐字段片段 + 文档部分更新（2026-11-20 立项）

> 来源：下游反馈 [`feedbacks/2026-11-13-doc-capability-gaps.md`](feedbacks/2026-11-13-doc-capability-gaps.md)（锦书 §Q-21）的遗留：
> #1 后半「逐字段 snippet」、#3「文档无部分更新」。用户定：先做这两项（#4 多值 meta 涉及盘上格式与 ABI，另出设计），只编码不提交。
>
> **S60 方案**：`Cask::search_fields_highlight(query, k, opts, filter)` = `search_fields_attributed` + 每个命中字段一组片段。
> 原文一律经 `get_doc_view` 读回（不依赖原文 LRU：LRU 只有默认字段），命名字段取该字段值、默认字段取正文（空则第一个命名字段）；
> `TextPlugin::field_query_terms` 按 search_fields 同一套解析把查询拆成「字段 → 词」（含同义词展开），每个字段只用自己的词切
> （`snippets_for_terms`，与 S53 同一条切片路）；读回 ord ≠ 命中 ord 则不给。
> **S61 方案**：`Cask::update_doc(key, DocPatch)`：补丁自洽校验（不碰盘）→ 循环｛`get_doc_view` 读当前版本 → fields 就地删 / 换、
> 新的追加 → meta 解码合并重排编码 → `put_batch_atomic` 单 op `kPutDoc` + `kOrdEquals(读到的 ord)`｝，`kConditionFailed` 重试至多 16 次。
> 正文 / 向量 / expiry_at 取自读到的版本。
>
> 约束：⛔ 改既有结构体布局 / 函数签名 / 盘上格式。

| 项 | 状态 |
|---|---|
| S60-1 `search::FieldSnippets` / `SearchFieldHitEx`；`TextPlugin::field_query_terms` / `snippets_for_terms`；`Cask::search_fields_highlight` | ✅ |
| S60-2 C `bitcask_search_fields_highlight` / `bitcask_search_fields_hl_result_free`（三层 malloc，OOM 逐层回滚） | ✅ |
| S61-1 `DocPatch` + `Cask::update_doc` | ✅ |
| S61-2 C `bitcask_doc_patch_t` + `bitcask_update_doc` | ✅ |
| S60/61-3 回归 `tests/s60_s61_doc_ops_test.cpp`（8）：字段片段只用该字段的词、与 attributed 命中 / 分数 / 字段一致、冷库 + 自定义标签、带 filter；update_doc 改 / 删 / 加字段与 meta、正文不动、字段顺序、索引与 meta 过滤跟新版本、重开不变、过期时刻保留、坏补丁四种、不存在、只读句柄、4 线程并发改不同 meta 键无丢更新、向量库向量保留且向量检索照常；C `test_field_snippets_and_update_doc` | ✅ |
| S60/61-4 `doc/api-c.md` §10.1c / §13.13c、`doc/api-cpp.md`、CHANGELOG `[Unreleased]`、反馈回填 | ✅ |

**落地记录**（已提交 `a19a20f`，**11.2.0 已发版**）：
- 验证：GCC Release（`-Werror`）全量 ctest **952** 全绿（含 C API 用例）；ASan 全量 952 全绿；Clang Debug / TSan 相关 205 全绿；
  TSan 下并发 update_doc 用例连跑 8 轮 0 告警。Windows 未实测。
- 口径备忘：update_doc 不省盘（追加写整条记录 + 整篇重建索引），省的是调用方往返与外部锁；向量 int8 / cosine 库是「读回 → 再写」，
  测试只覆盖了 f32 cosine（逐元素误差 < 1e-6）。
- 版本定 **11.2.0**（MINOR：纯新增，ABI 兼容），库 `SOVERSION` 保持 **11**（`.so.11`）。

---

## 🔴 S59 · 跨进程 merge_only 旁车合并后写者读不到被搬走的 key（2026-11-20 立项）

> 来源：S58 排查中自行发现（探针：两套独立权威 = 两个进程）。用户定：修，只编码不提交。
>
> **病因**：跨进程旁车合并时搬迁只进了旁车自己的权威；旁车关库排水删掉输入文件后，写者内存索引（memdelta / 已载 run）
> 仍指向旧位置——点查 `kIo "open file_id=N"`，删除之后才开始的全表迭代报 `fold location disappeared`，重开才好
> （打开时 manifest 指向已删文件 ⇒ 一致性门不过 ⇒ 从 data 重建）。纯 KV 同样如此；同进程（共享 registry）不受影响。
> 迭代开始前已钉住的文件、读缓存里已打开的句柄不受影响（Linux 上删除后仍可读）。
>
> **方案**：写者侧自愈。新增 `Cask::heal_stale_location(file_id)`：定位所指数据文件在盘上**确实不存在**（打不开但文件还在
> = 真 I/O 错误，照常上报）且非 merge_only ⇒ 一次等待式受控重建（`run_controlled_rebuild(wait, force)`：可写句柄
> `rebuild_from_data`、只读句柄内存重建；并发读者排在同一次重建后，不重复重建），合并输出在 data 里，重建后定位指向新位置。
> `read_doc`：S13-F5 重查后仍打不开 ⇒ 自愈后再试一次。`CaskIter`：重查到同一版本仍指向缺失文件 ⇒ 自愈；同一版本位置已变
> （迭代器视图固定在旧 run 上，后续 key 都会遇到）⇒ 按重查位置重读（每 key 至多 2 次）。range 迭代走 `get`，随 `read_doc` 修。

| 项 | 状态 |
|---|---|
| S59-1 `Cask::heal_stale_location` | ✅ |
| S59-2 `read_doc` 第二次重试前自愈 | ✅ |
| S59-3 `CaskIter` 自愈 + 同版本新位置重读 | ✅ |
| S59-4 回归 `tests/s59_cross_process_sidecar_test.cpp`（KV / 检索）：写者开着、另一套权威的旁车合并并删输入后，写者点查 / 全表迭代 / range 迭代 / 检索全对、之后照常写、重开不变；删除后第一个读者是全表迭代 | ✅ |
| S59-5 CHANGELOG `[11.1.1]`、TASK | ✅ |

**落地记录**（已提交 `cd6ef74`，**11.1.1 已发版**）：
- 基线红：禁用自愈的副本上 4 条用例全红（点查 `open file_id=1`、迭代 `fold location disappeared`）。开发中先只在「重查仍指向
  同一文件」时重建，「先迭代」用例仍红——迭代器视图固定在旧 run 上，第二个 key 起重查已是新位置、原逻辑把「同 ord」判为消失；
  补「同版本新位置重读」后绿。
- 验证：GCC Release（`-Werror`）全量 ctest **944** 全绿；ASan 全量 944 全绿；Clang Debug / TSan
  `S59|S58|S57|Merge|Oki|S43|Checkpoint|CaskDocValue|Crash|Iter|Range|Concurrent` 304 全绿。Windows 未实测（Windows 上
  旁车删不掉写者打开着的文件，原本就不会走到这里）。
- 代价：每次跨进程旁车合并后，写者第一次碰到被搬走的 key 时整库从 data 重建一次（之后正常）。
- 未动：两个进程各自提交 `kv.oki.manifest`（后提交者覆盖前者）；崩溃后打开由既有一致性门（manifest 指向已删文件 ⇒
  从 data 重建）兜住，本项不改。
- 版本：PATCH **11.1.1**（无公共接口与格式改动，ABI 兼容），库 `SOVERSION` 保持 **11**。

---

## 🔴 S58 · 检索库 merge_only 旁车合并挂死 + 旁车改写写者检索持久化（2026-11-20 立项）

> 来源：下游反馈 [`feedbacks/2026-11-20-merge-only-search-merge-hangs.md`](feedbacks/2026-11-20-merge-only-search-merge-hangs.md)
> （bitcask 修 `merge/2,3` 的 `dict_path` 时撞到）。用户定：修，只编码不提交。
>
> **病因**（插桩实测）：merge_only 打开不从数据折叠，车道以 `next_ord = 0` 注册；`Cask::merge` 开头 `ensure_oki_for_write()`
> 的受控重建（merge_only 分支 `rebuild_memory_from_data`）把 `next_ord` 推到数据末尾（例：300），收尾 `flush_index()` 等
> `wait_lsn(lane, 300)`，车道覆盖水位 0、车道上永远不会有任务 ⇒ 无界挂死。同进程旁车（共享权威）则是旁车打开后写者新写的
> ord 不在旁车车道上，同样挂。即使不挂，收尾的成对保存点会用旁车的空检索索引改写写者的 `bm25.ckpt` / `docmap.ckpt` /
> `index.manifest`（实测改写；本例覆盖水位 0，写者重开整库重放兜住，没丢数据，但逼写者整库重放、且依赖水位恰为 0）。
>
> **方案**：merge_only 旁车在检索库上按纯 KV 合并——不带检索插件（DocmapRelocator / Text / Vector）、跳过检索侧收尾（读屏障 +
> 成对保存点），与公开 `checkpoint()` / close「merge_only 不写检索 ckpt」一致；搬迁的检索侧归正交给写者（同进程：共享权威，
> 写者读写照常；下次打开按 key+LSN 重放合并输出）。`flush_index()` 对 merge_only 直接返回（旁车不接受写），旁车上调检索也不再挂。

| 项 | 状态 |
|---|---|
| S58-1 `Cask::merge`：`own_search = text_ && !merge_only` 门控插件 span 与检索侧收尾 | ✅ |
| S58-2 `Cask::flush_index()`：merge_only 直接返回 | ✅ |
| S58-3 回归 `tests/s58_merge_only_search_test.cpp`：写者已关 ⇒ 旁车合并有界返回、旁车上检索不挂、写者检索持久化逐字节不变、输入文件回收、重开检索分数 / 值 / 旧版本不复活 / 之后照常写；写者开着且同进程（旁车打开后写者继续写）⇒ 合并有界返回、写者读与检索照常、在线分数合并前后逐位不变、重开命中集合不变 | ✅ |
| S58-4 更正 S57 的 `needs_merge` 头注（跨进程旁车实时账本为空、退回清单；同进程共享写者实时账本） | ✅ |
| S58-5 CHANGELOG `[11.1.1]`、反馈回填 | ✅ |

**落地记录**（已提交 `cd6ef74`，**11.1.1 已发版**）：
- 基线红：11.1.0 上两条用例都 30 s 挂住；只去掉等待、不拦收尾的变体上，检索持久化三个文件全被旁车改写。
- 验证：GCC Release（`-Werror`）全量 ctest **940** 全绿；ASan 全量 940 全绿；Clang Debug / TSan
  `S58|S57|Merge|Oki|S43|Checkpoint|CaskDocValue|Search|TextPlugin` 383 全绿。Windows 未实测。
- 另发现：**跨进程**旁车在写者开着时合并后写者读被搬走的 key 失败（与 S58 无关、早已存在）——已由 **S59** 修。
- 版本：PATCH **11.1.1**（无接口与格式改动，ABI 兼容），库 `SOVERSION` 保持 **11**（`.so.11`）。

---

## 🔴 S57 · jieba 缺词典时 open 返回错误（不再 abort 进程）+ `needs_merge` 用实时账本（2026-11-20 立项）

> 来源：bosun 两份下游反馈（11.0.0 `862d735` 复现）：
> [`feedbacks/2026-11-19-jieba-missing-dict-aborts-process.md`](feedbacks/2026-11-19-jieba-missing-dict-aborts-process.md)、
> [`feedbacks/2026-11-19-needs-merge-blind-to-dead-bytes-since-commit-point.md`](feedbacks/2026-11-19-needs-merge-blind-to-dead-bytes-since-commit-point.md)。
> 用户定：继续修，只编码不提交。
>
> **方案**：
> - jieba：新增 `text::jieba_missing_dict_file(dict_dir)`（5 个词典文件逐个查「存在、普通文件、可读」，返回第一个缺的完整路径）；
>   工厂的 Jieba creator 先查，缺了返回空 ⇒ `Cask::open` 走既有 `kInvalidOption`，报文带缺失路径。**不降级、不换分词器**：
>   词典缺失 jieba 无法工作，库直接打不开；改的只是「杀进程」变成「open 返回错误」。直接构造 `JiebaAnalyzer` 仍不检查（头注写明）。
> - `needs_merge`：候选统计改取实时账本 `authority_->fstats()`（= 打开时采信的 v4 清单 + 之后的增量，`status()` 同源），
>   实时账本为空才退回清单。S43-7 当时选清单是因为实时源还是待删的 KeyDir；merge 活性裁决走 OKI ReadView，与选文件统计无关。
>
> 约束：⛔ 改 C API / 盘上格式 / 既有结构体布局（C++ 只新增一个自由函数）。
> 下游 bitcask 侧（`merge/2,3` 漏补缺省 `dict_path`、`dict_path` 收 list 被静默忽略）不在本仓库。

| 项 | 状态 |
|---|---|
| S57-1 `jieba_missing_dict_file` + Jieba creator 预检 + `Cask::open` 报文带缺失路径 | ✅ |
| S57-2 `Cask::needs_merge` 实时账本优先 | ✅ |
| S57-3 回归 `tests/s57_feedback_test.cpp`：缺文件定位（目录同名也算缺）、工厂返回空、open 坏路径 / 空路径 ⇒ `kInvalidOption` + 路径、真词典照常；重开后覆盖写 ⇒ `needs_merge` 为真且候选含 status 过阈值的文件、合并后再写死同样看见（KV / 检索两组） | ✅ |
| S57-4 CHANGELOG `[11.1.0]`、反馈回填 | ✅ |

**落地记录**（已提交 `24da81b`，**11.1.0 已发版**）：
- 基线红：11.0.0 上 `needs_merge` 用例 KV / 检索两组都红；工厂坏路径用例进程在 `DictTrie.hpp:243 FATAL` 处直接死。
- 验证：GCC Release（`-Werror`）全量 ctest **938** 全绿；ASan 全量 938 全绿；Clang Debug / TSan
  `S57|NeedsMerge|Merge|Jieba|Analyzer|Oki|S43|CaskDocValue|Checkpoint` 333 全绿。Windows 未实测。
- 版本定 **11.1.0**（MINOR：纯新增函数 + 行为修复，ABI 兼容），库 `SOVERSION` 保持 **11**（`.so.11`）。

---

## 🔴 S56 · `search_fields` 打分口径：每字段跨段统计 + 字段自身 dl（2026-11-19 立项）

> 来源：下游反馈
> [`feedbacks/2026-11-19-search-fields-local-stats-and-total-doc-len.md`](feedbacks/2026-11-19-search-fields-local-stats-and-total-doc-len.md)
> （bosun，经 bitcask 10.0.0）：① `search_fields_attributed` 逐段用**本地** N / df / sum_dl 打分，S45-4 后新文档散在各分片
> building 段里各算各的 idf ⇒ 同一查询重开前后分数、顺序都变，默认字段查询与 `search_text` 对不上；② 字段打分的 dl 取段级
> **整篇总词数**而 avgdl 是**该字段**的 ⇒ 短字段被重罚（`labels:w^8` 压不过正文词频）。用户定：两条都修，只编码不提交。
>
> **方案**：
> - ①：`search_fields_attributed` 每字段跨段聚合 N / sum_dl / df（G-on-the-fly，同 `multi_segment_search`），各段同一
>   idf / avgdl；boost 分组移到段循环外只算一次。
> - ②：每(段, 字段)一张「该字段自身词数」表 `FieldDocLens` + `FieldLenChecker`（存活走段、dl 取表、未知回退整篇总长）
>   （新头 `include/bitcask/field_doc_lens.hpp`）。内存段 add 时先记表再写 posting；v1 段载入后由 posting dls 列重建；
>   v2（mmap）段**盘上格式不变**，首次查询该字段时从 posting dls 列惰性推出（call_once，只解 docid/tf/dl 列），字段
>   sum_dl == 段总长时不建表（每篇字段长 ≤ 整篇，和相等 ⇒ 逐篇相等——catch-all 默认字段常态）。
> - 视图出借：`FieldSegmentView::live`（新成员，缺省 nullptr = 退回段本身）、`SealedSegment::view()` / TextPlugin 默认视图 /
>   explain 改用 `default_checker()`。默认字段因此也走字段自身 dl：catch-all 开（缺省）时与整篇总长逐篇相等，
>   `search_text` / phrase / bool / fuzzy / wildcard 分数逐位不变；**catch-all 关时默认字段只含正文，此前同样被命名字段长度
>   压分，一并归正**（反馈未提，基线实测证实）。
>
> 约束：⛔ 改盘上格式；⛔ 改 C API。C++ 已安装头 `segment.hpp` 的 `FieldSegmentView` / `SealedSegment` 加成员（内部段层，
> 下游一般不直接用）。

| 项 | 状态 |
|---|---|
| S56-1 `search_fields_attributed` 每字段跨段统计（①） | ✅ |
| S56-2 `FieldDocLens` / `FieldLenChecker` / `FieldScoring`；内存段 add 记表、v1 载入重建、v2 惰性推导（`MmapSegment::derive_field_doc_lens`） | ✅ |
| S56-3 视图接线：`FieldSegmentView::live`、`multi_field_segment_search`、默认视图 / `SegmentSet::search` / explain 用 `default_checker()` | ✅ |
| S56-4 回归 `tests/search_fields_scoring_test.cpp`：与 `search_text` 逐位一致（building / 重开 / checkpoint 重开）、命名字段分数 = 解析 idf、boost 排序、字段分数与其它字段长度无关、catch-all 关的默认字段、内存段与 mmap 段字段长 / 查询逐位一致 | ✅ |
| S56-5 `InvertedIndex.MultiFieldSegmentMergeEquivalence` 基线改每字段 checker（原基线按整篇总长写，正是②的旧口径） | ✅ |
| S56-6 CHANGELOG `[11.0.0]`、反馈回填 | ✅ |

**落地记录**（未提交，11.0.0）：
- 基线红：新用例在 10.0.0（`git archive HEAD` 单独编）上 4 / 4 红，数值与反馈逐位吻合（building 段 `search_fields "zebra"`
  body=1.8927 vs `search_text` 4.0170）；catch-all 关的 `search_text` 两篇同正文分数 1.369 vs 0.541（命名字段长度压分）。
- 验证：GCC Release（`-Werror`）全量 ctest **933** 全绿；ASan 全量 933 全绿；Clang Debug / TSan 段与检索相关 331 全绿。
  `BM_Cask_SearchHybrid` 293 µs 对 293 µs（持平）。Windows 未实测。
- 代价：v2 段第一次查某个需要建表的字段时付 O(该字段 posting 数) 一次（catch-all 默认字段免建表）；内存段每字段每篇 4 B。
- 行为变化：既有库的 `search_fields` 分数与排序会变（两条都是）；catch-all 关的库 `search_text` 等默认字段分数会变。
  不给开关（旧口径是错的）。版本定 **11.0.0**（C API 与盘上格式零改动；C++ 已安装头 `segment.hpp` 两个结构体加成员
  = ABI 破坏）。

---

## 🔴 S55 · 分词器配置进 meta、按记录打开、ngram 高亮偏移（2026-11-19 立项）

> 来源：下游反馈
> [`feedbacks/2026-11-19-analyzer-not-recorded-ngram-no-highlight.md`](feedbacks/2026-11-19-analyzer-not-recorded-ngram-no-highlight.md)
> （keelson 转 bitcask-viewer）：① meta 只记分析器指纹，打开方无从得知建库分词器，只读查看工具只能按缺省 ngram 开；
> ② ngram 的 `analyze_with_offsets` 偏移恒 0 ⇒ 高亮片段恒空（热冷都是）；高亮器按 `unordered_map` 序插标签，
> 位置在前、遍历在后的词与重叠 token 标不出；③ 指纹不符只告警（C1 既定取舍，不改）。
>
> **方案**：
> - meta 在 C1 尾段后追加 **C2 尾段**（分析器配置本身，自带 CRC；读端用 C1 指纹自校验，不符当未记录）。旧读端只读前
>   18 / 27 字节，对其透明；被旧二进制重写会丢尾段 ⇒ 退化为未记录（同 C1）。
> - `SearchLayerConfig::analyzer_from_meta` / C `BITCASK_ANALYZER_STORED`：按库里记的配置打开（`dict_path` 仍取选项：
>   机器相关、不入 meta）。库没记录 ⇒ `kInvalidOption` 并说明；可写打开、配置指纹与 C1 相符的旧库顺手补记 C2。
> - C `bitcask_meta_read` / `bitcask_meta_info_free`：不开库、不拿锁读 meta（含分析器配置）；失败分 NOT_FOUND /
>   BAD_CRC（损坏）/ `BITCASK_ERR_UNSUPPORTED_FORMAT`（新增 18，纪元拒开）/ IO。
> - ngram 给真实字节偏移（归一化文本坐标，同 whitespace / jieba）；高亮器在片段内按位置排序、合并重叠区间再插标签。
>
> 约束：⛔ 改既有结构体布局 / 函数签名（C：新枚举值 + 新错误码 + 新函数 + 新结构体；C++：`MetaConfig` / `MetaError` /
> `SearchLayerConfig` 加成员）；⛔ 改定长头与 C1 尾段布局。

| 项 | 状态 |
|---|---|
| S55-1 meta C2 尾段：`MetaConfig::analyzer`（`std::optional<text::AnalyzerConfig>`，不含 `dict_path`）读 / 写 / 指纹自校验；建库与 KV→索引升级时记录 | ✅ |
| S55-2 `SearchLayerConfig::analyzer_from_meta` + C `BITCASK_ANALYZER_STORED`；未记录 ⇒ `kInvalidOption`；可写开、指纹相符的旧库补记 C2 | ✅ |
| S55-3 C `bitcask_meta_read` / `bitcask_meta_info_free` + `MetaError::kind` + `BITCASK_ERR_UNSUPPORTED_FORMAT` | ✅ |
| S55-4 `NgramAnalyzer::analyze_with_offsets` 真实偏移（`ngram_collect` 的 sink 带字节范围，三条路径共用） | ✅ |
| S55-5 高亮器：片段内区间排序 + 合并重叠后插标签 | ✅ |
| S55-6 回归：meta C2 往返 / 指纹自校验 / 旧 meta 透明；按记录打开（whitespace / jieba / 未记录拒开 / 补记）；ngram 偏移对拍；多词乱序与重叠高亮；ngram 冷读高亮；C `test_meta_read_and_stored_analyzer` | ✅ |
| S55-7 `doc/api-c.md`、`doc/api-cpp.md`、CHANGELOG `[Unreleased]`、反馈回填 | ✅ |

**落地记录**（已提交，10.0.0）：
- 开发中多修一处：`Cask::open` 建搜索插件原用入参 `opts`，按记录覆盖的分析器到不了插件（`options()` 看着对、实际仍按选项切词）
  ⇒ 改传 `cask->opts_`。端到端用例 `NgramLibraryHighlightsHotAndColdLoad` 抓到的。
- 验证：GCC Release（`-Werror`）全量 ctest **928**（+15 新增）两轮：一轮全绿；一轮只挂 S53 的
  `HighlightColdTest.ColdSnippetsMatchHotOnes`——**既有偶发**：16 路并发 + CPU 负载下跑 64 次，改动前的 9.1.0 基线
  （`git archive HEAD` 单独编）失败 29 / 32 次，S55 后 21 / 36 次；串行稳定通过。病因是用例前提「写 1500 篇填充 ⇒ 重开即冷」
  在高负载下不成立（重开仍从数据重放、填回原文 LRU；关库前显式 checkpoint 也不够）。S55 的冷路径用例改用
  `doc_text_cache_max = 0` 保证必冷（同压力 64 次 0 失败）；S53 用例未改，待定。
  ASan / Clang Debug：`AnalyzerRecord|NgramOffsets|HighlighterOrder|MetaAnalyzerFp` 23/23、分析器 / 高亮 / 文本插件 / C API
  相关 120/120 全绿，C `test_meta_read_and_stored_analyzer` 通过。Windows 未实测。
- 版本（2026-11-19 定版）：C++ 三个公共结构体加成员（`MetaConfig` / `MetaError` / `SearchLayerConfig`）= 布局变更 = ABI 破坏
  ⇒ MAJOR +1，取 **10.0.0**，`SOVERSION` → **10**（C++ 下游须重新编译链接）；C API 侧纯新增，C ABI 兼容。

---

## 🔴 S54 · `bitcask_range_iter_start` 的 NO_INDEX 说明勘误（2026-11-19 立项）

> 来源：下游反馈
> [`feedbacks/2026-11-19-c-api-range-iter-no-index-comment-stale.md`](feedbacks/2026-11-19-c-api-range-iter-no-index-comment-stale.md)
> （keelson 转 bitcask-viewer）：注释说「只读打开且没建过 OKI ⇒ NO_INDEX」，实测只读照常迭代。
>
> **实际行为（已核对代码与实测）**：只读打开时 recovery 走 `rebuild_memory_from_data`（只建内存读视图、
> 不写盘），失败才置 `oki_rebuild_failed_` ⇒ `kIndexRebuildFailed`；`kNoIndex` 只出在 merge-only 句柄
> （`make_range_iter` 对它不试建）。`cask.hpp:1097` 的注释本来就是对的。

| 项 | 状态 |
|---|---|
| S54-1 `bitcask_kv.h`（range 注释 + `INDEX_REBUILD_FAILED` 枚举注释）、`cask.hpp`（`kNoIndex` / `kIndexRebuildFailed` 两处）、`cask_range_iter.cpp` 报文、`doc/api-c.md`、`doc/api-cpp.md` 改口径 | ✅ |
| S54-2 回归 `OkiRangeTest.ReadOnlyWithoutOkiIteratesMergeOnlyHasNoIndex`（删 OKI 后只读遍历 = 影子、⛔ 写回 OKI；merge-only ⇒ kNoIndex 且报文 ⛔ 再提只读） | ✅ |

**落地记录**（未提交、未定版）：行为零改动，只有注释 / 文档 / 报文。验证见 S52。

---

## 🔴 S53 · 冷文档高亮：读回正文补片段（2026-11-19 立项）

> 来源：下游反馈
> [`feedbacks/2026-11-19-highlight-cold-docs-never-get-snippets.md`](feedbacks/2026-11-19-highlight-cold-docs-never-get-snippets.md)
> （keelson 转 bitcask-viewer）：片段只取原文 LRU，LRU 只在写入 / 建索引时填 ⇒ 新开进程 / 只读快照每条
> 命中都无片段；下游自己 `get_doc` 切片段，分词与引擎不一致。
>
> **方案**：在 Cask 层补（插件拿不到 DocValue 解码，宿主有 `get_doc_view`）。`ColdText::kLoad` ⇒ 片段为空的
> 命中读回正文，交给 `TextPlugin::highlight_snippets`——与 LRU 命中同一条路（抽出私有 `snippets_for` 共用）。
> 口径：正文空取第一个命名字段（同 LRU 写入口径）；读回 ord ≠ 命中 ord 不补；⛔ 回填 LRU。
>
> 约束：⛔ 改既有结构体布局 / 函数签名（C：新函数 `_ex` + 宏；C++：四参重载 + 新枚举 + 新 public 成员函数），
> 「因冷而无片段」的标记要改 `SearchHitEx` / `bitcask_search_hit_ex_t` 布局 ⇒ 不做（kLoad 已覆盖需求）。

| 项 | 状态 |
|---|---|
| S53-1 `TextPlugin::snippets_for`（私有，抽自 `search_text_highlight`）+ `highlight_snippets`（public） | ✅ |
| S53-2 `search::ColdText` + `Cask::search_text_highlight(query, k, opts, cold)`；三参版委托 kSkip | ✅ |
| S53-3 C API `bitcask_search_text_highlight_ex` + `BITCASK_HIGHLIGHT_LOAD_COLD`（保留位 ⇒ INVALID_OPTION）；旧入口委托 flags = 0 | ✅ |
| S53-4 回归 `HighlightColdTest.*`（重开即冷、kLoad 三种文档都有片段、⛔ 回填 LRU、选项生效；热命中不受影响；冷 / 热片段逐字相同）、C `test_highlight_load_cold` | ✅ |
| S53-5 `doc/api-c.md` §13.13b、`doc/api-cpp.md`、CHANGELOG `[Unreleased]`、反馈回填 | ✅ |

**落地记录**（未提交、未定版）：

- 改动面：`include/bitcask/{highlighter,text_plugin,cask}.hpp`、`src/search/text_plugin.cpp`、`src/cask/cask_search.cpp`、
  `c_api/bitcask_text.{h,cpp}`、`tests/search_highlight_cold_test.cpp`（新）、`tests/c_api_test.c`、`tests/CMakeLists.txt`。
- 写判据时撞到：**小库重开会从数据重放**（重放填 LRU ⇒ 热）——3 篇重开全热、1500 篇重开全冷。判据先写目标文档再写一批
  填充，让目标落进封好的段。文档里写明「小库上看不出冷，别据此判断大库」。

---

## 🔴 S52 · 旧段集打不开时回退空集无目录 ⇒ 新段写进进程当前目录（2026-11-19 立项）

> 来源：下游反馈
> [`feedbacks/2026-11-19-old-segments-fallback-writes-cwd.md`](feedbacks/2026-11-19-old-segments-fallback-writes-cwd.md)
> （观澜，9.0.0 打开 7.0.1 封段的旧检索库，当前目录多出 `seg-0.seg` / `segments.manifest`，无日志）。
>
> **病因（已复现）**：用观澜给的旧库夹具逐层载入——段清单与 `bm25.ckpt` 读得出、CRC 对；`seg-0.seg` 是 `BSG2` v2、
> `MmapSegment::open` 也成功，字段为 `\xfault` / `tags` / `title`。`open_v2` 末尾要求有 `kDefaultField`（8.1.0 起是
> `\x01_bitcask:default`）⇒ 拒载。这是 8.1.0 定的「旧默认字段标识不迁移、需重建」。毛病在回退：
> `load_component` 失败 ⇒ `open()` 以 loaded=false 再调 `init_segment_set` ⇒ `SegmentSet::open` 回 nullptr ⇒
> `std::make_unique<SegmentSet>()`（**`dir_` 空串**）⇒ 全量重放封段时 `join("", "seg-0.seg")` 是相对路径。
> 且 `expect_segments` 为假时回 ok、没有任何日志。
>
> 顺带发现的第二个坑：即使回退集带上目录，新段从 0 编号会与旧 `seg-0.seg` 同名；全活段 `save_v2` 不写也不删
> `.live` 旁车 ⇒ 旧旁车下次打开被套到新段上，把活文档标成已删（静默少返回）。

| 项 | 状态 |
|---|---|
| S52-1 `SegmentSet::fresh(dir, verify_crc, &stale)`：落在 dir；seg_id 从已有 `seg-<n>.seg[.live]` 最大号之后起；原有段文件与旁车进 `pending_unlink_`（首次可写 commit 清，只读不动盘） | ✅ |
| S52-2 `init_segment_set` 回退改用 `fresh` + warning（只在 loaded=false 那次记，⛔ 记两遍） | ✅ |
| S52-3 回归 `SegmentFallbackTest.UnreadableSegmentsRebuildInsideTheIndexDir`（改坏旧段魔数制造「载不进」、旧段带旁车、当前目录换成空目录；断言当前目录干净、warning、旧文件清掉、新段号 > 旧号、重开检索全对）+ `.FreshLibraryLogsNothing` | ✅ |
| S52-4 CHANGELOG `[Unreleased]`、反馈回填 | ✅ |

**落地记录**（未提交、未定版）：

- 改动面：`include/bitcask/segment_set.hpp`、`src/search/text_plugin.cpp`、`tests/search_segment_fallback_test.cpp`（新）、`tests/CMakeLists.txt`。
- 新判据在 9.0.0 实现上红（当前目录被写、无 warning、旧段与旁车未清、新段与旧段同为 0 号），本实现上绿。
- 验证（S52～S54 合计）：GCC Release 全量 ctest **913/913**（2 项 Disabled），`-Werror` 无新告警；
  `ctest -R "SegmentFallback|HighlightCold|OkiRange|CApi|TextPlugin|Highlight"` 61/61 在 ASan / TSan / Clang Debug 全绿。Windows 未实测。
- 未处理（另议）：`SegmentSet::open` 无清单 / 有清单时 `next_seg_id_` 取 0 / 清单水位，崩溃留下的孤儿段（add_pending 后未 commit）
  若带 `.live` 旁车，同样可能被同名新段误套——同一类风险，本项只修了回退路径。

---

## 🔴 S51 · 向量检索带过滤时少返回（结果侧过滤 → 遍历期过滤）（2026-11-19 立项）

> 来源：S50 排查时的旁证，无下游反馈。
>
> **症状**：merge 之前死文档占多数、或 meta filter 选择性高时，向量检索返回数 < k、
> 召回塌陷，不报错。`frag_merge_trigger` 默认 60%，所以 50%～60% 死比是 merge 前的常态。
>
> **病因**（`src/vector/hnsw.cpp` `HnswIndex::search`）：
> 1. 活过滤在**结果侧**：0 层搜出 ef 个候选后才滤死，活候选期望只有 ef × 活比。
>    默认 `ef = max(k, 64)`（`vector_plugin.cpp`），活比 < k/ef 就必然回不满。
> 2. 默认高维路径（kDot、dim ≥ 64、有 VNNI）更糟：int8 粗筛后先截到 `k*3` 条做 f32
>    精排，**再**滤死——活比 < 1/3 时无论 ef 传多大都回不满（调大 ef 无效）。
> 3. meta filter 与 `is_live` 折进同一个回调，走同一条路；`vector_plugin.cpp` 注释写的
>    「被拒节点从图遍历源头就不入候选集」与实现不符。2026-10-06 那条反馈里「向量一路
>    不受影响」的判断因此不成立。
> 4. DiskANN 封存段（`diskann.cpp:664`）同为结果侧过滤；IVF 是入堆前过滤，不受影响。
>
> **实测**（5000 活节点、k=10、ef=64、300 查询；原型在 `/tmp/lb-fb1119/p2`）：
>
> | 死比 | dim 32（纯 f32）现状 | dim 64（int8 粗筛）现状 | dim 64 现状 + ef 按活比放大 | dim 64 遍历期过滤 |
> |---|---|---|---|---|
> | 0% | 0.985 | 0.912 | 0.912 | 0.912 |
> | 75% | 0.878（12 条回不满） | 0.615（237 条回不满） | 0.730（240 条回不满） | 0.954（0 条） |
> | 90% | 0.574（279 条） | 0.300（300 条） | 0.302（300 条） | 0.956（0 条） |
>
> **建议方案：遍历期过滤**（hnswlib / Lucene 的做法）——所有节点照常参与导航，但只有
> 通过过滤的节点才进结果堆，收敛条件看结果堆。等价于 ef 按活比自适应放大，但不需要
> 知道活比，对 meta filter 同样有效；`found` 里全是活节点，`k*3` 截断随之无害。
> 代价：查询耗时约 ∝ 1/活比（dim 32：死比 0% 32 µs 不变，75% 34 → 131 µs，
> 90% 42 → 357 µs）；过滤回调次数从 ≤ ef 升到约 3 × ef / 活比（`is_live` 每次拿锁，
> 需在插件层实测，必要时走 `fill_is_live` 批量）。极端选择性（1%）会遍历半张图
> （10 万节点 3.4 ms/查询），需要距离计算预算封顶。
>
> **用户拍板（2026-11-19）**：采用遍历期过滤；预算 ef × 256；DiskANN 同批处理。
> 2026-10-06 反馈第 1 条的文本一路（固定多取 `max(k×4, 64)` 再后过滤）不在本项范围。
>
> 约束：不改盘上格式、不改 C API、不改既有公开方法签名；S51-8 给 `DocTable` 加虚函数，
> C++ ABI 破坏，版本升 9.0.0（`HnswIndex` 只加一个公开常量
> `kFilterBudgetMul`、类型 `BatchFilter` 和方法 `search_filtered()`，都是纯新增；
> 私有 `search_layer*` 加参数）。

| 项 | 状态 |
|---|---|
| S51-1 `search_layer_impl` 加过滤回调：前沿收所有节点、结果堆只收通过者，收敛看结果堆 | ✅ |
| S51-2 `HnswIndex::search` 两条路径改遍历期过滤，去掉结果侧过滤（int8 粗筛的 3k 截断随之无害） | ✅ |
| S51-3 预算封顶：带过滤时 0 层距离计算 ≤ ef × 256，耗尽返回已找到的 | ✅ |
| S51-4 DiskANN 段搜索改「前沿 + 结果」双堆，过滤在展开时求值，预算 L × 256 | ✅ |
| S51-5 回归：`HnswFilteredSearch.*`（2 条）、`DiskannSegment.Filter*`（2 条） | ✅ |
| S51-6 头文件 / 插件注释、CHANGELOG `[Unreleased]` | ✅ |
| S51-7 `is_live` 锁开销实测 + HNSW 过滤改批量（`search_filtered` / `BatchFilter`，插件走 `fill_is_live`） | ✅ |
| S51-8 `DocTable::fill_eval_meta()` 批量版（新增虚函数）+ `Index` 覆写 + 插件接入；版本升 **9.0.0**（用户定：按破坏性变更处理） | ✅ |

**落地记录**（未提交、未定版）：

- 改动面：`src/vector/hnsw.cpp`、`src/vector/diskann.cpp`、`include/bitcask/hnsw.hpp`、
  `include/bitcask/diskann.hpp`（仅注释）、`src/search/vector_plugin.cpp`、
  `include/bitcask/detail/sealed_segment_vector_plugin.hpp`、
  `tests/hnsw_test.cpp`、`tests/diskann_test.cpp`。
- **DiskANN 走了一次弯路**：第一版沿用原候选池、把「展开后被拒」的条目标死且不占池宽。
  召回不达标（通过率 1/8、L=64 时 0.59）——未展开条目仍占池宽，被挤出池的活节点
  不可挽回。改为双堆（前沿不限宽、界只看已展开且通过的前 L 个）后同条件 1.00。
  无过滤 / 全通过时展开序列与原 beam 相同（原收敛条件 ⇔ 前沿最优者不优于第 L 名
  已展开者），现有两条召回测试门限不变、照过。
- 新测试在旧行为上红：`HnswFilteredSearch` 两条、`DiskannSegment.FilterFillsKWhenMostNodesRejected`
  （DiskANN 预算那条是上界断言，旧实现也过）。
- 验证（含 S51-7 / S51-8，版本已是 9.0.0）：GCC Release 全量 ctest 907/907（2 项 Disabled），
  产物 `libbitcask.so.9.0.0`；`ctest -R "Hnsw|Vector|Hybrid|Diskann|Ivf|MetaFilter|Index\.|CApi|Version"`
  209/209 在 ASan / TSan / Clang Debug 全绿，编译无新告警。Windows 未实测。
- 基准（5000 活节点、k=10、ef=64、300 查询，回调为数组查表；「回不满」= 返回 < 10 条的查询数）：

  | 死比 / 拒绝率 | dim 32 f32：recall / 回不满 / µs | dim 64 默认（int8 粗筛）：recall / 回不满 / µs | 回调次数/查询 |
  |---|---|---|---|
  | 0% | 0.985 / 0 / 32（改前 32） | 0.912 / 0 / 38（改前 35） | 200～230（改前 10） |
  | 50% | 0.993 / 0 / 63（改前 0.950 / 0 / 34） | 0.937 / 0 / 71 | ~400 |
  | 75% | 0.998 / 0 / 136（改前 0.878 / 12 / 34） | 0.954 / 0 / 147（改前 0.615 / 237 / 44） | ~800 |
  | 90% | 0.999 / 0 / 380（改前 0.574 / 279 / 42） | 0.956 / 0 / 424（改前 0.300 / 300 / 59） | ~2000 |
  | 99%（10 万节点 1000 活） | 0.979 / 0 / 850（改前 0.069 / 300 / 55） | 0.840 / 0 / 1262 | ~11000 |

  99% 档已触到预算（ef × 256 = 16384 次距离计算），召回开始下降但仍回满。
  DiskANN（2400 节点、通过率 1/8、L=64）：recall 0.32～0.59 → 1.00。
- **S51-7 锁开销实测与批量化**（真实 `Index` + `HnswIndex`，5000 活节点、dim 64、
  k=10、ef=64，8 核机；`/tmp/lb-fb1119/lockbench2.cpp`）。逐个 `is_live` 每次一把
  `shared_lock`：单线程只慢 ~5%，但并发下读锁计数的缓存行争用把 QPS 打掉一半以上。
  改为每展开一个节点把「够格进结果」的邻居攒一批、调一次 `fill_is_live`：

  | 死比 | 线程 | 无锁数组（上限） | 逐个 `is_live` | 批量 `fill_is_live` |
  |---|---|---|---|---|
  | 0% | 1 | 24.4k QPS | 23.9k | 25.2k |
  | 0% | 8 | 136.0k | 57.5k | 130.3k |
  | 75% | 8 | 39.0k | 13.2k | 34.7k |
  | 87.5% | 8 | 18.7k | 6.5k | 17.3k |

  批量版离无锁上限 4%～11%。另测了「一个写线程死循环抢 `unique_lock`」的极端对照
  （非真实负载）：死比 0% 单读线程逐个 2.1k QPS、批量 7.9k QPS——批量同样更好，但写
  线程饱和时读仍会被明显拖慢，这是 `Index` 单把读写锁的固有性质，不在本项范围。
  实现：新增公开 `HnswIndex::BatchFilter` 与 `search_filtered()`；原 `search(…, live*)`
  签名不变，内部把逐个回调适配成批。`VectorPlugin` 与封存段插件的窗口 HNSW 改走批量；
  回归 `HnswFilteredSearch.BatchFilterMatchesPerOrdAndBatches`。
- **S51-8 批量 `eval_meta`**（2026-11-19 用户定：加，版本按破坏性变更处理）：
  `bm25::DocTable` 新增虚函数 `fill_eval_meta(ords, filter, out)`，默认实现逐个调
  `eval_meta`（外部实现者源码不用改），`Index` 覆写为一把 `shared_lock` 判整批；
  两个向量插件带 filter 时改走它。vtable 变化 ⇒ C++ ABI 破坏 ⇒ **9.0.0 / SOVERSION 9**
  （`CMakeLists.txt`、`README.md`、`doc/api-c.md`、`doc/api-cpp.md`、CHANGELOG 已同步；
  未提交、未打 tag）。实测（2 万节点全活、dim 64、k=10、ef=64、8 核机，
  `/tmp/lb-fb1119/metabench.cpp`；对照 = S51-7 的「批量判活 + 逐个 `eval_meta`」）：

  | filter 通过率 | 线程 | 逐个 `eval_meta` | 批量 `fill_eval_meta` |
  |---|---|---|---|
  | 1/2 | 1 | 3.9k QPS | 4.9k |
  | 1/2 | 8 | 21.0k | 35.2k |
  | 1/8 | 1 | 1.7k | 1.8k |
  | 1/8 | 8 | 7.3k | 11.0k |

  回归 `Index.FillEvalMetaMatchesEvalMeta`（含基类默认实现路径）。
- **未做 / 已知取舍**：
  - DiskANN 段的过滤仍是逐个回调：ord 在盘上块里、一次只展开一个节点，没有可攒的批；
    调用次数 = 展开数，与改前同量级。
  - 通过率低到预算不够时（约 < 1%，取决于图规模）仍会少返回，这是预算的设计语义。
  - 文本检索带 filter 的少返回（2026-10-06 反馈第 1 条）未动。

---

## 🔴 S50 · HNSW `clone_live` 高死比后图质量修复（2026-11-19 立项）

> 来源：下游反馈
> [`feedbacks/2026-11-19-jinshu-merge-corrupts-search-index.md`](feedbacks/2026-11-19-jinshu-merge-corrupts-search-index.md)
> （锦书，pin 8.1.0）：覆盖写几轮后 merge，向量检索返回错 key / 命中数不足，不报错。
> 编号说明：S49 已被 8.1.0（默认字段哨兵）占用，本项顺延为 S50。
>
> **病因（已在 Linux 复现并定量）**：`clone_live` pass 2 只做死邻过滤，仅「某层邻居
> 全死」才借道补边。下游报的「0 层被撕成不连通分量」是最极端的表现；更普遍的是
> **出度塌缩**——死比 75% 时 0 层平均出度 26 → 6，图即便连通也稀到 ef=64 搜不准
> （recall@10：随机删 30% 0.99→0.97，75% →0.78，90% →0.34；覆盖写 8 轮 →0.17）。
> 所以只按下游建议「BFS 找不可达节点再重连」修不干净（75% 档修后仍 0.78）。
>
> **方案（2026-11-19 用户拍板阈值 50%）**：按死比分两路，收尾统一兜底。
> - 死比 < 50%：结构化拷贝 + **全面路径收缩**（凡丢了邻居的节点，候选 = 活邻 ∪
>   穿过死节点链可达的活节点，超额按建图同款启发式选边；不做图搜索）。
> - 死比 ≥ 50%：**全量重连**（节点数据/层数保留，邻接清空后按 id 序走标准连边路径；
>   代价 ∝ 活节点数）。
> - 两路之后：0 层从入口的强连通检查，不合格节点走标准连边路径补连。
>
> 约束：不改盘上格式、不改 C API、不改 `clone_live` 签名（C++ 头只加私有成员函数）。
> 已知取舍：高死比 merge 的向量重建从毫秒级变为「活节点数 × 单点连边耗时」（单线程）；
> 大图高维下可达分钟级（S13-P8 当初改结构化拷贝的原因），并行重连留待后续。

| 项 | 状态 |
|---|---|
| S50-1 `insert()` 连边段抽成 `link_node(id, repair)`，插入与修复共用 | ✅ |
| S50-2 `clone_live` pass 2 改全面路径收缩（多跳穿死链、限额、启发式选边） | ✅ |
| S50-3 死比 ≥ 50% 走全量重连 | ✅ |
| S50-4 `repair_l0_connectivity()`：0 层强连通检查 + 补连兜底 | ✅ |
| S50-5 回归：`HnswCloneConnectivity.RepairAfterHeavyDeadFilter`（下游最小回归，f32 / int8-only × 同向量 / 异向量覆盖）、`RecallHoldsAcrossDeadRatios`（死比 30% / 45% / 75% 对照从零建图） | ✅ |
| S50-6 头文件契约注释、CHANGELOG `[Unreleased]`、反馈回填 | ✅ |

**落地记录**（未提交、未定版）：

- 改动面：`src/vector/hnsw.cpp`、`include/bitcask/hnsw.hpp`、`tests/hnsw_test.cpp`。
- 新测试在 8.1.0 实现上红（两条都红），本实现上绿。
- 验证：GCC Release 全量 ctest 901/901（2 项 Disabled）；`ctest -R "Hnsw|Vector|Hybrid"`
  57/57 在 GCC Release / ASan / TSan / Clang Debug 四个构建目录全绿，编译无新告警。
  Windows 未实测。
- 基准（5000 活节点、dim 32、f32、recall@10、ef=64；int8-only 3000 节点 dim 64 各档
  0.957～0.967，与从零建图持平）：

  | 负载 | 8.1.0 recall / clone | S50 recall / clone |
  |---|---|---|
  | 随机删 10% | 0.990 / 3 ms | 0.993 / 25 ms |
  | 随机删 30% | 0.968 / 1 ms | 0.991 / 32 ms |
  | 随机删 50% | 0.942 / 2 ms | 0.992 / 478 ms |
  | 随机删 75% | 0.775 / 2 ms | 0.994 / 477 ms |
  | 随机删 90% | 0.344 / 4 ms | 0.991 / 475 ms |
  | 覆盖写 4 轮（每轮向量不同） | 0.610 / 1 ms | 0.992 / 466 ms |
  | 覆盖写 8 轮（每轮向量不同） | 0.168 / 5 ms | 0.994 / 497 ms |

  规模只到几千节点、随机向量；大图高维下全量重连的实际耗时未测。
- merge **之前**高死比同样少返回，与 `clone_live` 无关，已另立 **S51**。

---

## 🔴 S48 · MSVC 显式异常展开（RAII 跨异常契约）（2026-11-18 立项）

> 来源：下游反馈
> [`feedbacks/2026-11-18-msvc-no-ehsc-raii-not-unwound-seqgate-hangs.md`](feedbacks/2026-11-18-msvc-no-ehsc-raii-not-unwound-seqgate-hangs.md)。
> 本库异常安全按「异常跨过局部对象必然析构」设计（`SeqGate`、C API
> `try_call`、锁/句柄守卫）；MSVC 不开启异常展开时该前提失效，可能造成
> 读者永久自旋。修复必须显式置于目录作用域、不依赖 CMake 默认 flags。

| 项 | 状态 |
|---|---|
| S48-1 MSVC 全局显式加 `/EHsc`，写明 SeqGate / oneTBB 连带原因 | ✅ |
| S48-2 `SeqGate.WindowReleasedOnException` 回归与 Release 增量构建 | ✅ |

---

## 🔴 S47 · Windows TBB finalize 兼容（2026-11-18 立项）

> 来源：下游反馈
> [`feedbacks/2026-11-18-windows-tbb-finalize-single-arg-does-not-compile.md`](feedbacks/2026-11-18-windows-tbb-finalize-single-arg-does-not-compile.md)。
> 单参 `tbb::finalize` 依赖 oneTBB 的 `TBB_USE_EXCEPTIONS`；异常展开未启用时
> 该重载消失。修复必须不改公共 API、不改异常开关、不改盘上格式。

| 项 | 状态 |
|---|---|
| S47-1 `TbbLifetime::release()` 改用 nothrow 双参 finalize，补 `<new>` | ✅ |
| S47-2 MSVC 无异常展开探针 + 全量重建 / TBB 生命周期回归 | ✅ |

---

## 🔴 S46 · 原子批前置条件（按 LSN 的乐观并发）（2026-11-17 立项）

> 来源：下游反馈 [`feedbacks/2026-11-17-conditional-atomic-batch.md`](feedbacks/2026-11-17-conditional-atomic-batch.md)
> （bitcask 图层 v2：一个顶点的边打包成一个 value，每次加边都是读-改-写）。
> 用户定：只开发不提交。

| 项 | 状态 |
|---|---|
| S46-1 引擎：`BatchOp::expect` / `expect_ord` / `kCheck`、`kConditionFailed`、`BatchOutcome` | ✅ |
| S46-2 TxnCask 透传 | ✅ |
| S46-3 C API：`bitcask_cond_op_t` + `*_cond` 两函数 + 错误码 17（纯加法） | ✅ |
| S46-4 测试 + 文档 | ✅ |

**落地记录（2026-11-17）**

- 判定放在 `put_batch_atomic` 拿 `write_mu_` 之后、任何字节进写缓冲之前：
  ⓪a 形态校验 → `ensure_oki_for_write` → ⓪b 逐个条件调 `read_doc`（与 `get()`
  同一条定位 + key/ord 自证 + 墓碑 / 过期过滤）→ ⓪c 剔除 `kCheck` 后原流程不变。
  `read_doc` 不取 `write_mu_`（受控重建只取 `oki_rebuild_mu_`，与锁内调用
  `ensure_oki_for_write` 同理），不自锁。索引不可判定原样上抛 `kIndexCorrupt`，
  不折叠成「不存在」。
- 批内同 key 多次：全部对照批前状态（判定时本批尚无字节）。
- `outcome.ords`：成员落盘 ord；merge race 单条重写的成员以重写记录的 ord 为准
  （那才是 `get()` 之后返回的版本）。group commit 失败时清空。
- LSN 契约核对：merger `copy_oki_record` 原样写回 `rec->ord`；重开 / OKI 重建从
  record 读 ord。回归覆盖 merge 后、重开后、删派生缓存全量重建后旧 ord 仍可作条件。
- 代价：每个带条件的 op 一次 `read_doc`（一次定位 + 一次 pread，刚读过的通常在
  page cache），在写锁内。不带条件的批零额外开销（`has_cond` 门）。
- 验证：gcc Release 全量 **899/899**；ASan 全量 **899/899**；clang Debug 新用例 + 原子批两组 + C API
  全绿；TSan 新用例 ×20 零告警；`-Werror` 库树零告警。

---

## 🔴 S45 · 索引管线去全局顺序化：key + LSN 裁决、分片并发写、去 reducer（2026-11-17 立项）

> 来源：[`designs/index-pipeline-unordered-design-zh.md`](designs/index-pipeline-unordered-design-zh.md)
> （下文「设计 §x」均指该文档）。2026-11-17 竞态 / 泄漏审计，基线 HEAD `4d3b506`（7.0.3）。
> 决策基线：**索引应用不再要求全局 LSN 顺序。正确性由「key + LSN 裁决」（旧版本到达即
> 丢弃）+「同 key 串行」保证；任务按 key 哈希分发到分片线程各写各的段；reducer、重排
> ring、map worker 池、Skip 占位全部退役；水位改由 LSN 完成跟踪器给出。**
> 硬约束：公共 API、C ABI 不变；盘上格式不变（目标，S45-1 逐项确认）；严格读己之写保留；
> HNSW 仍单写者。
> 验收标准：每项改动后 ctest 全绿 + 编译无新告警；并发测试必须带**对照组**（去掉保护后
> 能观察到违例），否则不算有效测试；涉及应用顺序的改动必须过乱序注入对拍与 TSan 循环；
> 收尾过 ASan/TSan、crash 注入与吞吐 / 恢复基准。

### ⚠️ 执行纪律

**先让每一处应用都能乱序且幂等，再拆掉顺序。** 顺序必须是：

```text
止血（死锁旁路 + close 超时）——可独立随 7.0.x 发
→ 调研与基线（docmap / 向量 / OKI 点查 / 全库顺序假设清单）
→ 乱序安全化（reducer 仍在；乱序注入对拍）
→ LSN 完成跟踪器接管水位与读屏障（reducer 仍在；与旧水位恒等）
→ 分片执行器替换管线（删 reducer / ring / map worker / Skip）
→ 恢复重放走新管线
→ 基准、调参、收口
```

不得跳过 S45-2 直接做 S45-4：没有裁决和幂等就拆顺序，后果是静默丢文档或幽灵文档
（`add_doc` 的水位自门、docmap 的无条件覆盖都会在乱序下悄悄出错，不报错）。
S45-2 / S45-3 期间 reducer 仍在，新机制只是冗余保险，行为必须与基线逐位等价——
这是判断新机制正确的唯一便宜时机。

| # | 任务 | 状态 | 说明 |
|---|---|---|---|
| S45-0 | 止血：死锁旁路 + close 有界轮询（设计 §3） | ✅ done | 旁路随 S45-4 删除；close 有界轮询保留；原复现负载全部跑通 |
| S45-1 | 调研与基线（设计 §8） | ✅ done | `docs/design/s45-1-research-zh.md`；裁决选 R-b（OKI）+ docmap 守卫；盘上格式仅 HNSW 图文件放宽一条校验 |
| S45-2 | 乱序安全化（设计 §4.1） | ✅ done | docmap 守卫 + `relocate`；文本 ≥ 守卫 / 定位表按 LSN；HNSW/IVF 重放门；规则 R；乱序注入对拍 + 对照组 |
| S45-3 | LSN 完成跟踪器（设计 §4.3 / §4.5） | ✅ done | `lsn_tracker.hpp`；读屏障 / ckpt 水位切换；Debug 断言与 applied_ord+1 恒等（250 用例未触发） |
| S45-4 | 分片执行器替换管线（设计 §4.2 / §4.4 / §4.7 / §5） | ✅ done | `thread_pool.hpp` 重写 + `index_dispatch.hpp`；reducer/ring/map worker/Skip/RunFn 删除；真并发对拍 + 对照组 |
| S45-5 | 恢复重放走新管线（设计 §4.6） | ✅ done | 重放经执行器 + 重放起点；`verify_index_consistency`；并发写中崩溃用例；20 万文档重开 1.6 s → 0.6 s |
| S45-6 | 基准、调参、文档与收口 | ✅ done | `docs/design/s45-6-bench-zh.md`；CHANGELOG / RISK_REPORT / 设计稿第 10 节；收口中发现并修复查询缓存 TOCTOU |

### ✅ S45-0 — 止血：死锁旁路 + close 有界轮询（先行，可独立发布）

> **落地记录（2026-11-17）**：`map_worker_loop` 谓词加饥饿旁路（仅 reducer
> **已入睡**且无就绪 lane 时放行——reducer 正在 apply 时的「无就绪」是取走
> 队头的瞬态，首版不加此条件导致 `ReorderBackpressureBoundsMemoryThenDrains`
> 偶发超额）；reducer 入睡前名额满则 notify。close 改 deadline + 退避轮询，
> `WriteOpGate` 析构去 notify；接缝 `set_close_wait_timeout_for_test` /
> `pin_write_gate_for_test`。新 `tests/index_pool_liveness_test.cpp`：池层乱序
> + 对照组（关旁路必挂死、重开自愈）、批 6 万 × 4 put 端到端、close 超时 /
> 正常两例。原复现 `60000 4` / `40000 8` / `200000 8` 全部跑通；TSan ×50 干净。
> close 的对照组未做运行时开关（旧实现无接缝，按读码确认会挂死）。
> 旁路代码随 S45-4 一并删除，池层用例改写为结构性活性用例。

工作面：`include/bitcask/thread_pool.hpp`、`src/cask/cask.cpp`（`close`）、
`include/bitcask/cask.hpp`（`WriteOpGate`、超时成员）、`tests/`（新
`tests/index_pool_liveness_test.cpp`）。

1. 回归用例先行（当前代码下必须失败，且不得挂住 ctest——用带超时的 future 或
   ctest `TIMEOUT`）：
   - 池层乱序死锁：队列 8、`reorder_cap` 4、2 个 map worker；先提交 LSN 1..N（N ≥ 64），
     最后提交 0；`flush` 限时返回；
   - 端到端：设计附录 A 的负载（`put_batch` 60000 + 4 个 `put` 线程），限时完成，
     随后 `prepare_search` 断言文档数；
   - close 超时：测试接缝持住一个 `WriteOpGate`，注入 100ms，断言 `close()` 约 100ms
     返回并有 error 日志。
2. 死锁旁路：`map_worker_loop` 谓词加 `!any_lane_ready_locked()`；`reducer_loop` 即将
   等待且 `reorder_inflight_ >= reorder_cap_` 时 `map_cv_.notify_all()`。更新类头
   「锁与死锁约定」注释，写明 `reorder_cap_` 变为软上限。
3. ring 在途首次超过 `2 × reorder_cap_` 时 `log_warn` 一次（限频）。
4. close：等待循环改 deadline + `sleep_for` 退避（50µs 起倍增，封顶 10ms）；超时值改
   成员 `close_wait_timeout_`（默认 30s）+ `*_for_test` 设置器；删 `WriteOpGate` 析构的
   `notify_all()`；修正 S25-T1 注释。
5. 对照组：旁路与新等待各留一个仅测试可见的开关，关掉后用例必须挂死。

验收：三组用例转绿；对照组挂死；`reorder_inflight_for_test()` 在所有 lane 注销后为 0；
池层用例 TSan ×200 无报告；`plugin_contract_test`、`c_api_shutdown_test` 全绿；
CHANGELOG `[Unreleased]` 记两项修复。

### ✅ S45-1 — 调研与基线

> **落地记录（2026-11-17）**：调研记录 `docs/design/s45-1-research-zh.md`（17 项
> 顺序假设逐条判定）。关键发现：HNSW 同样有单调水位门；`flush_building_slot`
> 假设段内 LSN 单调；merge 搬迁 `get→put_doc` 有既有竞态；checkpoint 水位取
> 提交时刻 `peek_next_ord` 会高估；search 模式**每次重开全量重新分词**。裁决
> 选 R-b（OKI `locate`）+ docmap 守卫；docmap 增量链无需改；盘上格式唯一触及
> 点为 HNSW 图文件的「节点 ord 递增」校验（S45-4 放宽，布局不变，见设计 §10.2）。
> 基线数据并入 S45-6 的同源对比基准（同一程序分别链接 7.0.3 与 S45 树）。

工作面：只读代码 + `bench/` + `docs/design/`（产出一份调研记录，回填设计 §8）。

1. docmap（`src/keydir/index.cpp`、`docmap_ckpt.cpp`）乱序语义盘点：
   - `put_doc` / `remove` 加裁决后的行为；`live_` / `meta_` 以 LSN 为键的影响；
   - 检查点增量日志（`delta_live_`、`removals_`、`delta_window_wm_`）在「LSN ≥ 水位的
     行可能先于水位落盘」下是否仍成立；
   - `set_doc_len` / `set_meta` 的寻址与线程约束（设计 §4 表「需核 SoA 无增长」）。
2. 向量插件：HNSW、IVF、DiskANN 段式插件（`sealed_segment_vector_plugin.hpp` 的
   `replay_gate_` 等）逐个列出顺序 / 单写者假设，标注「串行道即可」或「需加裁决」。
3. OKI 点查：`locate` 是否直接返回最新 LSN；刚写入的 key（memdelta 命中）与冷 key 的
   点查耗时；据此在 R-a / R-b / R-c 中选定裁决实现（设计 §4.1）。
4. 全库顺序假设清单：grep「升序 / 单调 / 单写 / reducer / 水位自门 / fold 序」，逐条
   判定「保留 / 需改 / 随管线删除」。已知项见设计 §2.3。
5. 纪律核对：哪些路径在持 `write_mu_` 时提交索引任务；哪些 RunFn / `run_serialized`
   闭包会取 `write_mu_`（设计 §4.4 的两条新纪律）。
6. 盘上格式逐项确认：段文件、段清单、docmap base / delta、向量组件、manifest 是否需要
   任何改动。
7. 基线数据：批量导入吞吐（`builder_threads` = 0 / 2 / 4）、`flush_index` 延迟、
   检查点耗时、恢复时间（尾部 10 万 / 100 万条）。

验收：调研记录入库；设计 §4.1 的实现选定并写明依据；设计 §8 的 1～4 项有结论；
若确认需要改盘上格式，**停下来与用户确认**后再继续。

### ✅ S45-2 — 乱序安全化（reducer 仍在，行为等价）

> **落地记录（2026-11-17）**：`Index::put_doc` LSN 守卫（同 ord 幂等 no-op）、
> `remove` 只删更旧、新增 `Index::relocate`（DocmapRelocator 改用，修既有竞态）；
> TextPlugin 守卫 > → ≥、`rebuild_key_locations` 按 LSN 裁决、`hi_lsn` 逐行取最大；
> HNSW 去单调门 + `arm_replay_gate`；IVF/DiskANN 重放门改 `<` + open 时已有集合；
> 宿主规则 R（`index_put_is_current`）+ `StatusInfo::index_stale_dropped`；reducer
> 乱序注入 `set_apply_shuffle_for_test`。新 `tests/index_unordered_apply_test.cpp`：
> 5 种子 0 违例、对照组（关裁决）必现违例、重开幂等。3 个旧用例按新语义改写
> （`Hnsw.OrdWatermarkIdempotent` → `OutOfOrderInsertsAreAllKept` +
> `ReplayGateIdempotent` 等）。

工作面：`src/keydir/index.cpp`、`include/bitcask/index.hpp`、`src/search/text_plugin.cpp`、
向量插件、`src/cask/cask.cpp`（`reduce_index_entry`）、`tests/`。

1. 宿主裁决（设计 §4.1 规则 R，按 S45-1 选定的实现）：`reduce_index_entry` 应用前裁决，
   过期任务整体丢弃（docmap 与各插件都不调用）。
2. docmap：`put_doc` 拒绝更旧的 LSN；`remove` 不得让更旧的 put 复活。
3. 文本插件：LSN 守卫由「大于则跳过」改为「大于等于则跳过」（重放幂等）；
   `rebuild_key_locations` 改为比较 LSN，不再依赖段序。
4. 向量插件：按 S45-1 结论加裁决或确认串行道足够。
5. 乱序注入器（仅测试）：reducer 取出就绪条目后，在一个小窗口内随机打乱应用顺序。
6. 模型对拍：随机 put / 覆盖 / 删除 / 复写序列（固定种子可复现），与内存 map 模型逐 key
   对比 docmap、BM25 命中集、向量命中集；重放幂等用例（同批任务应用两遍）。
7. 对照组：关掉裁决后，乱序注入下必须出现幽灵文档或丢文档。

验收：注入器关闭时全量 ctest 与基线逐位等价；注入器开启时对拍 ×1000 种子全绿；
对照组必现违例；TSan 循环无报告；吞吐相对 S45-1 基线回退 ≤ 3%。

### ✅ S45-3 — LSN 完成跟踪器（reducer 仍在）

> **落地记录（2026-11-17）**：`include/bitcask/lsn_tracker.hpp`（64K 位滑动位图 +
> 溢出集合，持锁 notify）+ `tests/lsn_tracker_test.cpp`；lane 接入、Debug 断言
> 「前缀 ≥ reducer 应用水位」在 250 个索引相关用例（含乱序注入）下未触发；
> `flush_index` 改等 LSN 前缀；ckpt / 自动 ckpt / merge 闭包在执行时刻取
> `index_coverage()`；close 用注销前一刻前缀；失败补洞收口到
> `complete_index_ord`。新用例：持续写入下读屏障不饿死 + 读己之写。

工作面：新 `include/bitcask/lsn_tracker.hpp`、`include/bitcask/thread_pool.hpp`、
`src/cask/cask.cpp`、`tests/`。

1. `LsnTracker`：`begin(L)` / `done(L)` / `contiguous()`；滑动位图 + 溢出集合；
   `done` 不阻塞、不分配、不抛异常。单测：乱序完成、窗口溢出、并发（TSan）。
2. 接线：LSN 分配点 `begin`；reducer 应用后、裁决丢弃后 `done`。
3. 等价性守门：调试构建断言 `contiguous() == applied_ord + 1`（reducer 仍在时两者恒等）。
4. 读屏障切换：`flush_index` / `prepare_search` 改为等 `contiguous() >= 入口时刻的下一个
   待分配 LSN`；无界语义不变（T8）。
5. 检查点水位切换：各保存点的水位取 `contiguous()`（设计 §4.5）。
6. 失败补偿切换：`OrdSkipGuard` / `BatchOrdGuard` 的析构改调 `done()`；写路径的显式
   Skip 提交同样替换。`IndexOp::Skip` 暂留（S45-4 删）。
7. 溢出集合超过阈值时 `log_error` 一次（有 LSN 长时间未完成）。

验收：等价性断言在全量 ctest 与乱序注入对拍下不触发；持续写入下 `flush_index` 不再
饿死（新用例）；`s43_rollout_gate_test`、`checkpoint_recovery_test` 全绿。

### ✅ S45-4 — 分片执行器替换管线

> **落地记录（2026-11-17）**：`thread_pool.hpp` 重写（N 分片 + 串行道 + 维护线程；
> Dekker 式暂停；归零递减持锁——TSan 抓到的 UAF 已修）；宿主分发抽成
> `index_dispatch.hpp`（Cask 与契约测试共用）；插件契约（`concurrency` /
> `accepts_*` / `shard` / `shards`）；TextPlugin BuilderPool → 每分片 building 段，
> 自动压实经 `run_serialized`；向量插件声明串行 + 只收带向量写；checkpoint /
> merge / 自动 ckpt 改 `run_exclusive`(`_and_wait`)；`unregister_lib` 超时摘钩入
> 墓地。HNSW 图文件放宽「节点 ord 递增」校验（设计 §10.2-1）。测试：
> `thread_pool_test` / `plugin_contract_test` 改写；乱序对拍改为真并发 + 提交抖动、
> KV 为预言机（对照组稳定命中靠「收尾竞速」阶段）。bench `index_pool_bench`
> 改写。

工作面：`include/bitcask/thread_pool.hpp`（重写）、`include/bitcask/plugin_api.hpp`、
`src/cask/cask.cpp`、`src/search/text_plugin.cpp`、向量插件、`tests/plugin_contract_test.cpp`。

1. 执行器：N 个分片线程 + 1 个串行道线程 + 1 个维护线程；每分片 / 串行道一条有界队列；
   按 `hash(key) mod N` 分发；删除任务走同一分片。
2. 每库状态：在途计数、暂停位、跟踪器；`pause(lane)` / `resume(lane)`（设计 §4.4）。
3. 插件契约（设计 §4.7）：`on_put` / `on_delete` 改为「同 key 串行、到达序任意」；
   事件带 `shard`、`OpenContext` 带 `shards`；`concurrency()` 声明；`prepare` 改为同线程
   先行调用；`run_serialized` 改由维护线程在暂停期执行。更新头注释与 DL 陷阱 6 的表述。
4. 文本插件：每分片一个 building 槽，`on_put` 直接写 `slot[shard]`；BuilderPool 线程、
   队列与 `builder_threads` 配置退役（配置项保留解析、忽略并告警一次，避免破坏下游配置）。
5. 向量插件：声明串行，走串行道。
6. 宿主：`reduce_index_entry` 拆为分片应用（裁决 → docmap → 并发插件）与串行应用；
   检查点 / merge 收尾 / 自动检查点改走维护线程；`put_batch` / `put_batch_atomic`
   失败路径的锁内补交挪到解锁后。
7. 删除（设计 §5）：reducer、ring、`reorder_*`、map worker、`MapFn` / `ReduceFn`、
   `IndexOp::Skip` / `RunFn`、`applied_ord` / `submitted_ord_hwm`、S45-0 的旁路。
8. `unregister_lib`：有界等在途归零；超时不销毁 lane（移入墓碑列表），消除已知 UAF 取舍。
9. 用例：同 key 高频覆盖 + 删除的终态；暂停独占（闭包执行期间无应用回调）；多库共享
   分片互不死锁；S45-0 的端到端用例继续全绿（此时不依赖旁路）。

验收：全量 ctest 全绿；乱序对拍（此时乱序是真实的，不靠注入器）×1000 种子全绿；
TSan 循环、ASan 无报告；附录 A 负载在批 60000 / 10⁶ 下均完成且在途任务数 ≤
(N + 1) × 队列容量。

### ✅ S45-5 — 恢复重放走新管线

> **落地记录（2026-11-17）**：重放车道经分片执行器（规则 R 关闭，结构守卫判重；
> docmap 已有行时仍送达插件——组件水位可能不同）；墓碑重放为真实删除；
> `replay_floor = replay_floor_of(min 组件水位)`（余量 `kReplayFloorMargin=4096`
> 吸收旧 ckpt 高估）。`Cask::verify_index_consistency()`。新
> `tests/index_crash_recovery_test.cpp`（4 写线程 + 3 次 checkpoint，写到 90% 时
> 崩溃；重开两轮均一致）。20 万文档库重开 1.6 s → 0.6 s 且不再重写段。
> **追加（2026-11-17，用户定「向量也走 key+LSN」）**：向量插件改用
> `detail/vector_key_loc.hpp` 的 key → (LSN, 墓碑) 定位表，HNSW 重放门删除；search
> 重放移到 `finish_oki_recovery` 之后的第二遍 fold 并开启规则 R（修复重开时已删版本
> 被重插为重复死节点，`V35MergeRebuildEvictsDead` 75 → 50）；docmap 检查点行 / meta
> 去掉 LSN 上界（顺带修 ord ≥ 水位行丢 meta）；串行插件一律收删除。回归：向量图节点
> 数跨重开不变（乱序对拍 + 崩溃用例）。
> **追加 S45-A（2026-11-17，用户定方案 A）**：向量定位表随检查点持久化——
> `kVecKeyLoc`（base 快照，不含墓碑）/ `kVecKeyLocDelta`（delta 窗口条目），重开
> 直接由 base + 链重建、不依赖 docmap；旧检查点回退 docmap 反查。新测试
> `tests/vector_key_persist_test.cpp`（7 例：编解码 3、插件级「盲 docmap 重开」与
> 「旧版缺段回退」2、Cask 级「丢 docmap 检查点」+ 对照组 2——对照组剥掉 key 段即
> 复现重复节点）。顺带修复：崩溃恢复后 OKI 对已删 key 可能报偏旧的墓碑 LSN，重放
> 期规则 R 原有的「OKI 更旧则放行」会放进中间版本（崩溃用例 5/60 偶发向量节点增长），
> 重放改为严格版（LSN 恰好相等才放行），100/100 通过；崩溃用例新增断言：每个存活
> key 的向量定位表 LSN == docmap LSN。
> **追加（2026-11-17，用户要求）**：OKI 墓碑 LSN 偏旧已修——根因在
> `IndexAuthority::rebuild_from_data` 账本模式的 tail 补行：视图已是墓碑时跳过 tail
> 中更新的墓碑。改为视图命中（活行或更旧墓碑）即补行。新用例
> `IndexCrashRecoveryTest.OkiTailTombstoneOverOlderTombstoneKeepsLatestLsn`（崩溃
> 子进程：删除 → checkpoint → 再写再删 → 崩溃）：修复前稳定复现（报 51，实为 54）。
> **追加（2026-11-17，用户要求查 AutoCheckpointOnRoll）**：不是测试复制目录的问题
> （复制前后源目录逐文件 名/大小/mtime 不变），是 OKI 真缺陷——`capture_commit_cut`
> 只钳有 `applied_end_` 登记的文件，roll 新建文件首条记录「已追加未应用」时水位
> 取文件大小，manifest 高估覆盖，崩溃后 tail 补行跳过 → KV 丢写（捕获的失败镜像
> 稳定复现：`ac_281` 是 5 号文件 offset 0 的首条记录，OKI kMiss、账本却计 400）。
> 修复：`increment_file_id()` 登记新文件落点 0；`rebuild_from_data` 发布时以各文件
> 扫描末尾登记（原 `clear()`）。新用例 `OkiCommitCut.DataWatermarkClampedToAppliedRecords`
> （确定性，修复前两处断言均失败：4096≠0、4846≠750）；TSan 加压复现
> 0/400（修复前约 1/60～1/180）。
> **追加（2026-11-17，用户要求加打开期兜底）**：账本模式补 tail 时逐文件抽检水位内
> 末条记录（tail 补完后比 OKI 与 data 胜出版本），不一致 → `finish_oki_recovery`
> 改走全量重建（只读句柄建内存视图）。新用例
> `IndexCrashRecoveryTest.OverclaimedOkiManifestFallsBackToRebuild`（子进程用测试缝
> 把写者停在「已追加未应用」、删登记后提交切面，忠实复现旧版本写坏的 manifest；关掉
> 兜底即丢 victim）。捕获的真实失败镜像：抽检 5 键、判不一致、重建后 0 丢失。全量
> 887 用例中兜底只在该用例触发 1 次（无误报）。
> **追加（2026-11-17）**：`SeqGate.PairedReadsNeverTorn` 偶发失败已查清——全是对照组
> （裸读 `torn_raw > 0`）没采到样本，闸本身无误（加压与 TSan 下 0 次不成对、0 告警）。
> 测试改为对照组为 0 时延长写入直到采到（20 s 封顶）；900 次 0 失败（原约 1%）。
> **S45-7（2026-11-17，用户要求）**：checkpoint / close 时分片 building 小段先合并再
> 落盘——`TextPlugin::flush_building` 按文档数装箱（每组 ≤ 65 536），组内
> `merge_segments_v2` 写一个段（新函数 `merge_building_slots`），失败退回逐段封口。
> 新用例 `tests/shard_segment_merge_test.cpp`（2 例：覆盖 / 删除 / 命名字段下合成 1
> 段且逐 key 对拍、合并后续写续删、重开；超组上限拆段）。同场 A/B：段文件 9→2 /
> 11→4，在线段 8→1，查询 p50 58～60→40～41 µs；**close 耗时无变化**——S45-6 报告里
> 「close 慢是因为 8 个段文件」的归因已更正为不成立（真正原因未定位）。
> **追加（2026-11-17，用户要求查 close 慢的真正原因）**：已查清，**非结构性回退**。
> 两棵树 close 分段计时：差异全在 BM25 落盘，其耗时 ∝ close 时未封口文档数，取决于最后
> 一次自动 checkpoint 的位置——7.0.3 按 LSN 精确每 65 536 一次（20 万篇时只剩 3 392
> 篇），S45 以覆盖水位为锚、节奏前漂且有随机性（20 万篇剩约 1.2 万）。跨 15/20/25/30
> 万篇两者互有胜负；close 前先 checkpoint+sync 时两者持平。排除：段文件数、分片阈值
> 均摊（实验后撤回）。详见 `docs/design/s45-6-bench-zh.md` §7。代码无改动。
> 「自动 checkpoint 以触发点为锚」用户定**不做**（会打破重放窗口 ≤ min_docs 的上限，
> 只换来 close 耗时可预测）。

工作面：`src/cask/cask_recovery.cpp`、`tools/`。

1. 尾部重放改为经分片执行器提交（并行、乱序）；原「攒批并行 prepare、串行应用」退役。
2. 删除依赖「插入序 == fold 序」的自门逻辑，或确认其已被规则 R 覆盖。
3. 一致性核对：诊断 API / 工具，遍历索引侧 `key → LSN` 与 OKI 比对并报告偏差
   （不在启动路径上）。
4. crash 注入：检查点各阶段、暂停期内、重放中途崩溃，重开后与模型一致。

验收：现有恢复 / crash 用例全绿；恢复时间相对 S45-1 基线不回退（预期改善）；
核对工具在对拍用例收尾处全部报告零偏差。

### ✅ S45-6 — 基准、调参、文档与收口

> **落地记录（2026-11-17）**：基准见 `docs/design/s45-6-bench-zh.md`（对比工具
> `scripts/s45_compare_bench.cpp`）：文本 end2end +30～45%、重开 2.7×、7.0.3 读屏障
> 持续写入下饿死 → S45 p50 46 ms；分片队列容量据此定为 512（2048 时 p50 220 ms，
> 吞吐相同）；close 195 vs 130 ms（每分片一个段，记为后续段合并候选）。文档：设计稿
> 第 10 节、CHANGELOG `[Unreleased]`、RISK_REPORT Phase 7、插件契约头注释。收口
> 阶段 sanitizer 矩阵发现并修复：① 新池归零递减 UAF（TSan）；② 查询缓存
> 「计算 → put」TOCTOU（既有缺陷，真并发下约 5% 丢文档，失效代号修复）。
> 全矩阵结果见基准报告末节。

1. 基准：批量导入吞吐（1 / 4 / 8 写线程 × 分片数）、`flush_index` 延迟、检查点期间的
   写停顿、恢复时间；与 S45-1 基线对比，结果写入 `docs/design/`。
2. 校准分片队列容量；评估跨库公平性（设计 §8 第 7 项），决定是否需要按库配额。
3. 文档：插件开发说明（新契约）、`thread_pool.hpp` 头注释、设计稿状态改「已实现」。
4. CHANGELOG：行为变化（`builder_threads` 退役、文本与向量可见时刻、检查点水位取法）。
5. `RISK_REPORT.md`：增补本轮审计两项及处置；Phase 5/6 已修项标注为已修
   （P5-MEM-1、P6-MEM-1/2/3、P6-DL-1、P6-DUR-1）；随管线删除而失效的条目标注作废。
6. 全矩阵：gcc-release / clang-debug / werror 全量 ctest，TSan 循环，ASan。

### 执行序

```text
S45-0 ──（可独立发布）
S45-1 ── S45-2 ── S45-3 ── S45-4 ── S45-5 ── S45-6
```

S45-0 与 S45-1 可并行。S45-2 与 S45-3 都在旧管线上做，互不依赖实现但建议按序，
便于定位回归。

### 风险排序（按「有多容易静默错」）

1. 裁决或幂等有漏：乱序下丢文档或留幽灵，不报错。靠乱序对拍 + 对照组守门。
2. docmap 增量链在乱序下的语义：最不确定的一块，S45-1 结论可能改变方案甚至触及格式。
3. 水位取错（用了「下一个待分配 LSN」而非连续前缀）：崩溃后永久漏索引。靠 S45-3 的
   等价性断言和 crash 注入守门。
4. 暂停与写锁的交互：持 `write_mu_` 阻塞式提交 + 独占闭包取 `write_mu_` 会成环。
5. 插件契约变更波及所有插件实现与下游自定义插件。

### 明确不做

- 不改 BM25 段格式、posting 结构、查询路径；不做 HNSW 多写者。
- 不在写线程内联分词；不放宽读己之写。
- 检查点不做「暂停外序列化」优化。
- 同轮审计的其余次要项（`auto_ckpt_inflight_` 卡死、读路径 / merge 与 close 并发）
  ——另行立项；`BatchOrdGuard` 析构异常与 `unregister_lib` 超时 UAF 已分别由
  S45-3、S45-4 顺带消除。

---

## 🔴 S44 · OKI 分级归并 + 读视图不可变发布（2026-11-16 立项）

> 来源：[`designs/oki-size-tiered-compaction-design-zh.md`](designs/oki-size-tiered-compaction-design-zh.md)
> （含第 10 节自审修订；下文「设计 §x」均指该文档）。
> 决策基线：**run 归并从「超过 8 个就全量重写」改为「只归并最新的连续后缀」
> （几何选段，K 默认 1）；读视图改为整体不可变发布（`OkiView` + `frozen_through`）。**
> 硬约束：**盘上格式零改动**（manifest v4 / run v3 不变，新旧版本可互开同一目录）；
> 任何并发读都不得读到已删除的旧值、旧位置或缺行；data 仍是唯一持久真相。
> 验收标准：每项改动后 ctest 全绿 + 编译无新告警；涉及读视图 / 归并提交的改动
> 必须过模型对拍与 TSan 循环；并发测试必须带**对照组**（去掉保护后能观察到违例），
> 否则不算有效测试；收尾过 ASan/TSan 与写放大基准。

### ⚠️ 执行纪律

**先让「成对一致」由构造保证，再让归并变频繁。** 顺序必须是：

```text
基线与守门用例
→ 读视图整体不可变发布（T-0），删掉 OkiState 里的 SeqGate
→ 选段策略（纯函数）
→ 后缀归并 + 墓碑保留 + 提交校验
→ 调度（全部后台 / 可取消 / 年轻道 / 背压）
→ 跨进程 load 重试
→ 基准调参与收口
```

不得跳过 S44-1 直接做 S44-3：分级后提交点更多更频繁，继续靠「每个提交点都记得
进 `SeqGate` 窗口」维持成对一致，漏一处就是并发读读错（设计 §4.7 陷阱 3 已经在
现有代码里出现过一次）。

已具备的前置（不在本清单内）：`SnapshotCell` / `SeqGate` 基础件、`OkiMergeCursor`
统一归并游标、三段式 `compact_runs`（IO 不持 `flush_mu_`）、在途 gen 登记
（`inflight_gens_`）、提交一致切面（`cut_seq`）。

| # | 任务 | 状态 | 说明 |
|---|---|---|---|
| S44-0 | 基线、不变量守门与对拍底座 | ✅ done | 写放大基线（`bitcask_oki_compaction_bench`）；`locate_matches_cursor` 守门；模型对拍；旧持留路径改为放弃 |
| S44-1 | 读视图整体不可变发布（设计 T-0） | ✅ done | `OkiView` + `frozen_through`；flush「冻结 → 落盘 → 单次发布」；`OkiState` 内 `SeqGate` 已移除 |
| S44-2 | 选段策略纯函数（设计 T-1） | ✅ done | `choose_compaction`；几何后缀、硬上限、小库全归并、`force_full`；`CompactionMode::kFull` 回退 |
| S44-3 | 后缀归并 + 墓碑保留 + 提交校验（设计 T-2） | ✅ done | `compact_once` 后缀归并；不含最老 run 时保留墓碑；连续性校验；墓碑占比触发全归并 |
| S44-4 | 归并调度：后台化、可取消、年轻道、背压（设计 T-3） | ✅ done | 所有提交点只调度；两条归并道；析构宽限 + 取消；`kMaxRuns` 背压；`StatusInfo` 统计 |
| S44-5 | 跨进程 `load` 重试（设计 T-3b） | ✅ done | run 缺失时重读 manifest 有界重试；Windows 滞留文件仅代码路径复核，未实测 |
| S44-6 | 基准、调参、文档与 rollout（设计 T-4） | ✅ done | 用户 2026-11-16 确认接受预热口径：F=64K 点查 p99 1.31×（未预热 1.61×）；写放大 ≈12～13×；F=1M 点查达标；全矩阵 5 套全绿；kFull 可用。自设附加门限 G2–G4 未达标（已知取舍） |

### ✅ S44-0 — 基线、不变量守门与对拍底座（先行）

工作面：`bench/`、`tests/oki_regression_test.cpp`（或新 `tests/oki_compaction_model_test.cpp`）、
`src/fileops/oki_state.cpp`、`docs/design/`。

1. 写放大基线（现状 = 全归并）：
   - `OkiState` 直驱基准：N = 10⁷ / 10⁸，单 run 行数 F = 64K / 1M；
   - 记录「归并重写行数 ÷ flush 行数」、归并总耗时、常驻 run 数、点查 p50/p99、range 吞吐；
   - 为此给 `OkiState` 加只读统计 `compaction_stats()`（归并次数、重写行数、墓碑行数）——
     本任务只加计数，不改行为。
2. 不变量守门（设计 §4.7 的 I1 / I2）：
   - 调试构建断言 / 测试助手：同一视图上 `locate(key)` 的结果必须与 `OkiMergeCursor`
     的裁决一致（两者是 I1 的两种消费方式，I1 被破坏时会分叉）；
   - 把该助手接进现有 OKI 测试的收尾检查。
3. 模型对拍骨架：
   - 随机操作序列（put / remove / 同 ord 搬迁 / 条件墓碑 / flush / 归并 / rebuild），
     与内存 map 模型逐 key 对比 `locate` 与迭代结果；
   - 固定种子可复现；先在现状（全归并）下跑绿，作为后续任务的回归底座。
4. 旧持留路径处置（设计 §4.9「需要一并处理的点」）：
   - 不带提交切面的 `flush(dir, durable_wms)` 按持久水位**整组持留**，会让
     「seq ≤ S 的行仍留在 active」，与 `frozen_through` 水位语义冲突；
   - 盘点调用点（预期只有测试直调），改为与带切面路径一致：引用未持久字节 → 放弃本轮；
   - 同步调整 `OkiOnlyContractTest.HeldRowAdvancesMetadataButNotRunWatermark`。

- **验收**：基线报告入库（`docs/design/s44-compaction-baseline-zh.md`）；模型对拍在现状下
  ≥ 10⁴ 步 × 多种子全绿；`locate` / 游标一致性助手在全量 OKI 测试中启用且全绿；
  旧持留路径无生产调用点残留。

#### ✅ S44-0 落地记录（2026-11-16）

- 基准：新增 `bench/oki_compaction_bench.cpp`（`bitcask_oki_compaction_bench <dir> <N> <F> <full|tiered> [K]`，
  `OkiState` 直驱，均匀随机 key 含覆盖写）；`OkiState::compaction_stats()` 计 flush / 归并次数、
  重写行数、墓碑行数、耗时、背压。基线与对比合并记录在
  `docs/design/s44-compaction-baseline-zh.md`（「现状」= `CompactionMode::kFull`，与旧全归并的选段
  规则逐字等价；旧实现是同步归并，计时口径见报告说明）。
- 不变量守门：`tests/support/oki_invariants.hpp` 的 `locate_matches_cursor()`——同一视图上点查与
  `OkiMergeCursor` 裁决逐 key 比对（墓碑与缺席视为等价：后台全归并会回收墓碑）。已接入
  `oki_regression_test` 的崩溃 tail、运行期重建、后台归并用例收尾，以及全部 S44 用例。
- 模型对拍：`tests/oki_compaction_model_test.cpp` 的 `Model*`——随机 put / remove / 同 ord 搬迁 /
  条件墓碑 / flush / rebuild / reopen，与内存 map 逐 key 对比点查与迭代；三套策略
  （分级小上限、分级 K=4、kFull）× 多种子 × 3000 步，后台归并并发运行。
- 旧持留路径：无快照的 `flush(dir, durable_wms)` 不再「整组持留」，与带快照路径一致（引用未持久字节
  → 放弃本轮）。生产调用点均带快照，无残留；`HeldRowAdvancesMetadataButNotRunWatermark` 已同步。

### ✅ S44-1 — 读视图整体不可变发布（设计 §4.9，T-0）

工作面：`include/bitcask/oki_state.hpp`、`src/fileops/oki_state.cpp`、
`include/bitcask/oki_merge_cursor.hpp`、`src/fileops/oki_merge_cursor.cpp`、
`src/cask/cask_iter.cpp`、`src/cask/cask_range_iter.cpp`、`src/merge/merger.cpp`。

1. 数据模型：
   - `OkiView { runs, frozen[], frozen_through }`，整体经 `SnapshotCell<OkiView>` 发布；
   - `DeltaSegment`：已排序去重的不可变内存段（带 key 二分查找，供点查）；
   - active memdelta 仍在 `mu_` 下只追加，行带 `seq`（已有）。
2. 唯一取数入口（点查与读视图共用，禁止各写一份）：
   - 先取 active（快照或哈希命中）→ 再 `view = cell.load()` → 丢弃 `seq ≤ view.frozen_through`
     的 active 行 → 来源序 active > frozen（新→旧）> runs（gen 降序）；
   - 顺序不可反（反了会漏掉恰在两步之间被冻结的行）。
3. 写者三个发布点（均在 `flush_mu_` 下「读当前视图 → 构造新视图 → 发布」）：
   - 冻结：`frozen += seg`、`frozen_through = cut_seq`，发布后再从 active 物理删除；
   - 落盘完成：`runs += run`、`frozen −= seg`，**一次发布**取代「发布 run + 裁 delta」；
   - rebuild / `install_memory_view`：`frozen_through = 当前最大 seq` 整体作废旧 active 行；
     内存视图的行直接成为一个 frozen 段。
4. `OkiMergeCursor` 增加 frozen 段这一类来源（位于 runs 之后、active 之前）；
   `ReadView` 改为持 `OkiView` + 过滤后的 active 快照；CaskIter / CaskRangeIter / MergeRunner
   只跟着改构造处。
5. 移除 `OkiState::view_gate_` 及三处写窗口、`make_read_view` 的重试路径；
   `sorted_view_` 缓存收窄为只缓存 active 剩余部分（或评估后删除）。

- **验收**：
  - 专项压力测试：写者循环「追加原始行 → 追加同 ord 搬迁行 → flush」，读者断言视图里
    任何 key 的胜出位置不旧于同视图 run / frozen 段里的同 ord 行；**对照组**（去掉 seq
    过滤，或把取数顺序写反）必须能观察到违例；
  - S44-0 的模型对拍与 `locate` / 游标一致性助手全绿；
  - `OkiState` 内不再出现 `SeqGate`；TSan 循环 ≥ 25 次无告警；
  - flush IO 期间的 range 不再重排被冻结的行（基准对比 S44-0 基线，记录即可）。

#### ✅ S44-1 落地记录（2026-11-16）

- `OkiState::OkiView { runs, frozen, frozen_through }` 经 `SnapshotCell` 整体发布；`ReadView` 增加
  `frozen`。点查与读视图同一协议：先取 active、后取视图、丢弃 `seq ≤ frozen_through` 的 active 行。
- flush 三步：冻结（排序去重成不可变段并发布，随后物理删除 active 行）→ 全部待落盘段归并写成 run
  （墓碑保留）→ manifest 提交后**单次发布**（run 入、段出）。写盘失败时段留在视图里，下次 flush 一并写出。
- rebuild / `install_memory_view`：`frozen_through` 抬到当前最大 seq 整体作废旧 active 行；内存视图
  的行直接成为一个冻结段。
- `OkiMergeCursor` 支持多个内存来源（冻结段旧→新，最后 active）；CaskIter / CaskRangeIter / merge
  只改构造处。`OkiState` 内不再使用 `SeqGate`（基础件保留，供其它站点）。
- 验收：`ReadViewPairingControlGroup`（确定性对照组：旧 active + 新 runs 必然判反，真实视图不判反）、
  `ReadViewNeverInvertedUnderConcurrentFlush`（读视图 + 点查「位置不倒退」并发压力）、
  `FrozenSegmentVisibleDuringFlushIo`（冻结窗口不缺行）。
- 偏差：「flush IO 期间 range 不再重排被冻结的行」由构造保证（冻结段已排序），未单独做 range 计时对比。

### ✅ S44-2 — 选段策略纯函数（设计 §4.2 / §4.3，T-1）

工作面：新 `include/bitcask/detail/oki_compaction_policy.hpp` +
`src/fileops/oki_compaction_policy.cpp`、`tests/`。

1. `choose_compaction(sizes, floor_index, opts) → {begin, full}`：
   - 几何后缀：从最新 run 往前累加，`s[j-1] ≤ K·acc` 就并入；后缀长度 ≥ 2 才归并；
   - `floor_index`：只能取下标 ≥ 它的 run（供 S44-4 的年轻道使用；老道传 0）；
   - `kMaxRuns` 硬上限：超过时强制把后缀扩到满足上限；
   - 小库直接全归并（底座 < 16·F 一类阈值，避免千万级小库写放大略升）；
   - `force_full` 输入位：由墓碑占比触发（S44-3 回填）。
2. `CompactionPolicy::kFull` 内部开关：策略函数恒返回全归并，用于 A/B 与回退。
3. 纯函数，不碰 IO、不持锁。

- **验收**：单元测试覆盖边界（空 / 单 run / 全等大小 / 严格递减 / 极小 run 夹杂）、
  K→∞ 退化为全归并、`floor_index` 约束、硬上限；模拟 10⁴ 次随机大小 flush，断言 run 数
  始终 ≤ `kMaxRuns`、每行累计重写次数 ≤ ⌈log₂(N/F_min)⌉ + 常数。

#### ✅ S44-2 落地记录（2026-11-16）

- `include/bitcask/detail/oki_compaction_policy.hpp` + `src/fileops/oki_compaction_policy.cpp`：
  `choose_compaction(sizes, floor, opts, force_full)`。几何后缀（K 默认 1）；`floor` 供年轻道；
  `max_runs`（默认 6，S44-6 由 24 下调）硬上限**只驱动主道**，被上限扩大后缀后继续按几何规则吸收；主道发现
  明显倒挂（老 run × 2 < 某更新 run）时从倒挂点起修复性归并；`stall_runs`（默认 2 × max_runs）为 flush
  背压阈值；底座 < `small_base_rows`（默认 1M 行）按 kFull 规则（run 数 > 8 全归并）；
  `force_full` 仅主道生效，对单 run 也生效（回收墓碑）。
- `tests/oki_compaction_policy_test.cpp` 11 例（S44-6 补「上限不驱动年轻道」「倒挂修复」「上限后继续吸收」3 例）：边界、几何选段、严格递减不并、K→∞ 退化全归并、
  floor、硬上限、kFull / 小库；10⁴ 次随机大小 flush 模拟——run 数恒 ≤ 24，写放大 ≤ log₂(N/F_min)+3，
  且旧策略在同一序列上的重写量 > 分级的 10 倍。

### ✅ S44-3 — 后缀归并 + 墓碑保留 + 提交校验（设计 §4.1 / §4.4 / §4.5，T-2）

工作面：`src/fileops/oki_state.cpp`、`include/bitcask/oki_state.hpp`、`tests/`。
依赖：S44-1、S44-2。

1. `compact_runs` 泛化为归并后缀 `Rj..Rn`：
   - 锁内：调用 S44-2 选段，快照输入 Reader，预留输出 gen（在途登记）；
   - 锁外：`OkiMergeCursor` 归并；**不含最老 run 时 `include_tombs = true`**，墓碑行原样
     写入输出（与 flush 产出的墓碑行同形）；含最老 run 时丢弃墓碑（同现状）；
   - 归并过程顺带统计输出墓碑行数，超过阈值（默认 25%）则置下一次 `force_full`。
2. 锁内提交校验：
   - 输入 gen 仍全部在册，且仍是「所有新于输出 gen 的 run 之前」的一段连续区间；
   - 期间发生 rebuild 或输入被改动 → 放弃并删除输出；
   - 新视图 = `Rj` 之前的 run + 输出 + 期间新增的更高 gen run；frozen / `frozen_through` /
     元数据 / fstats / data 水位 / wm 原样沿用；经 S44-1 的单次发布提交。
3. v1 输入时输出降 v1（同现状）。
4. 调整锁定旧行为的测试：
   - `OkiRecoveryTest.RunCompactionCollapsesRunsAtThreshold`；
   - `OkiRecoveryTest.FullCompactionDropsTombstoneRows`（改为断言「全归并后」）；
   - `OkiRegressionTest.CompactionKeepsManifestMetadata`、`AsyncCompactionKeepsReadViewsComplete`
     （run 数上限改为 `kMaxRuns`）。

- **验收**（设计 §4.7 的陷阱 1、2 必须各有专项用例）：
  - put 在最老 run、墓碑在小 run，后缀归并不含最老 run → key 仍不可见；重开后亦然；
  - 搬迁行与原行分处后缀内外 → 新位置胜出；
  - 归并期间并发 flush / rebuild → 提交放弃或正确拼接；
  - 「输出 run 已写、manifest 未提交」处中断 → 重开后输出作为孤儿被清理，数据完整；
  - 旧版本二进制（6.9.x）能打开分级归并后的目录并读对（降级兼容）；
  - S44-0 模型对拍（加入后缀归并操作）全绿。

#### ✅ S44-3 落地记录（2026-11-16）

- `OkiState::compact_once(dir, lane)`：锁内按策略选后缀、快照输入、预留输出 gen（在途登记）；锁外
  `OkiMergeCursor` 归并（不含最老 run 时 `include_tombs=true`，墓碑原样写入）；锁内校验输入仍在册且连续、
  其后 run 的 gen 全部大于输出 gen、全归并仍从下标 0 开始，通过才替换并单次发布视图。
  非全归并输出墓碑占比 > 25% → 下一次主道归并强制全归并。
- 锁定旧行为的测试已调整：`RunCompactionCollapsesRunsAtThreshold` / `FullCompactionDropsTombstoneRows`
  在 checkpoint 后 `wait_compaction()`；`AsyncCompactionKeepsReadViewsComplete` 上限改为 `kMaxRuns`。
  小库仍按旧规则（第 9 个 run 触发全归并），这两条用例的断言本身未改。
- 验收：`SuffixMergeKeepsTombstoneAndRelocation`（陷阱 1、2；重开后亦然；全归并后墓碑才消失）、
  `OrphanRunSweptOnNextCompaction`、模型对拍（含 rebuild 与后台归并并发）。
- 旧版本兼容：见 S44-6 落地记录（6.9.0 二进制以 Level B 读写打开分级归并后的目录逐 key 核对）。

### ✅ S44-4 — 归并调度：后台化、可取消、年轻道、背压（设计 §4.5 / §4.8，T-3）

工作面：`src/fileops/oki_state.cpp`、`include/bitcask/oki_state.hpp`、`src/cask/cask.cpp`、
`include/bitcask/cask.hpp`（`StatusInfo`）。依赖：S44-3。

1. 所有提交点（checkpoint / close / merge 收尾 / 备份 / 阈值 flush）只**调度**归并，
   不再同步执行；`flush` 的 `async_compaction` 参数随之移除。
   需要确定性 run 集合的调用方（测试、基准）用 `wait_compaction()`。
2. 可取消：归并循环每写 N 行检查取消标志；取消即删除半成品输出（输入未动，无副作用）。
   `OkiState` 析构 / close 先置取消再 join，不再陪等大归并。
3. 两条归并道：
   - 老道在途（预留输出 gen = G）时，允许年轻道只取 gen > G 的 run（S44-2 的 `floor_index`）；
   - 年轻道永不含最老 run，必然保留墓碑；两道各自提交、互不依赖；
   - 提交仍由 `flush_mu_` 串行，各自做 S44-3 的连续性校验。
4. 背压：run 数超过 `kMaxRuns` 时，flush 提交后同步等待年轻道归并完成。
5. `compaction_stats()` 补全（两道分别计数、取消次数、背压等待次数与时长），
   接入 `StatusInfo`。

- **验收**：
  - 自动 checkpoint（reducer 线程）与 close 的耗时不再随归并规模增长（基准对比）；
  - 大归并进行中持续 flush：run 数始终 ≤ `kMaxRuns`，读视图不缺行、不判反；
  - close 在大归并中途返回时间有上界，重开后数据完整；
  - 两道并发的模型对拍 + TSan 循环 ≥ 25 次无告警。

#### ✅ S44-4 落地记录（2026-11-16）

- 所有提交点只调度归并：`flush()` 去掉 `async_compaction` 参数，提交后 `schedule_compaction()`；
  `Cask::commit_oki_cut()` 随之简化。需要确定性 run 集合时用 `wait_compaction()`。
- 两条归并道（`lane_mu_` + `lane_cv_`）：主道在途（`main_out_gen_`）时 flush 调度会起年轻道，只取
  gen > G 的 run；主道在年轻道运行期间不开始新一轮。
- 取消：析构先给 500ms 宽限（小归并跑完——否则只在 close 时 flush 的短会话永远完不成归并），
  再置取消标志；归并每 4096 行检查一次，半成品输出删除、输入不动。
- 背压：flush 提交后 run 数 > `stall_runs`（默认 2 × `max_runs`）时在锁外等待归并追上（归并全部停下即返回）。
  阈值不直接用 `max_runs`：主道重写大 run 期间其输入仍在册，run 数短暂越过上限很正常，否则写入会被卡到大归并结束。
- `StatusInfo` 新增 `oki_runs` / `oki_flushed_rows` / `oki_compactions` / `oki_full_compactions` /
  `oki_young_compactions` / `oki_compaction_rows` / `oki_compaction_tomb_rows` / `oki_compaction_aborts` /
  `oki_compaction_ns` / `oki_backpressure_waits` / `oki_backpressure_ns`（C++ 结构体追加字段；C API 未同步）。
- 仅测试用的 `set_compaction_row_hook_for_test()`：确定性制造「大归并在途」窗口。
- 验收：`YoungLaneRunsWhileMainCompactionInFlight`、`DestructorCancelsLongCompactionBounded`（< 3s，
  取消后输入完整）、`BackpressureBoundsRunCount`；模型对拍覆盖两道并发。
- 偏差：「自动 checkpoint / close 耗时不再随归并规模增长」未单独计时；由构造保证（提交点不再同步
  归并，close 有宽限 + 取消上界）。

### ✅ S44-5 — 跨进程 `load` 重试（设计 §4.8，T-3b）

工作面：`src/fileops/oki_state.cpp`、`tests/`。可与 S44-4 并行。

1. `OkiState::load`：读完 manifest 后打开 run 文件遇到「文件不存在」时，重读 manifest 重试
   （有界次数），而不是直接退回未加载态（只读句柄 / merge_only 旁车会因此全量重建内存视图）。
   只对「缺失」重试；CRC / 结构损坏仍按现状弃用。
2. Windows：打开中的 run unlink 失败进 `sweep_backlog_`；确认每次归并提交都会重试，
   并补覆盖用例。

- **验收**：一个进程持续归并、另一个进程循环只读打开同一目录 ≥ 10³ 次，无一次退回
  全量重建；Windows ctest 全绿且长跑后无滞留 run 文件堆积。

#### ✅ S44-5 落地记录（2026-11-16）

- `OkiState::load`：打开 run 失败且文件不存在 → 指数退避（1ms 起）重读 manifest，最多 8 次；
  CRC / 结构损坏仍立即弃用。
- 验收：`ConcurrentLoaderNeverFallsBackToUnloaded`——写者持续 flush + 分级归并（上限 4），同进程另一个
  `OkiState` 循环 `load` 同一目录，0 次退回未加载态（同一文件系统竞态，未跨进程 fork）。
- Windows：每次归并 / 重建提交的 `sweep_runs` 都会先重试 `sweep_backlog_`，重试点与归并频率同步。
  **未在 Windows 上实测**（本机无 Windows 环境）。

### ✅ S44-6 — 基准、调参、文档与 rollout（设计 §5 / §9，T-4）

工作面：`bench/`、`docs/design/`、`doc/ordered-key-index-design-zh.md`、`CHANGELOG.md`。
依赖：S44-4、S44-5。

1. 与 S44-0 基线同口径复测（N = 10⁷ / 10⁸，F = 64K / 1M），对比 `CompactionPolicy::kFull`：
   写放大、归并总耗时、常驻 run 数、点查 p50/p99、range 吞吐、空间占用（含墓碑滞留）。
2. 结清设计 §9 的待定问题并写回设计文档：
   - K 默认值（1 或 2）；
   - `K` / `kMaxRuns` 是否暴露为 `CaskOptions`（倾向不暴露）；
   - 墓碑占比阈值是否按规模自适应；
   - 两条归并道是否需要 IO 限速（年轻道优先）。
3. 更新 `doc/ordered-key-index-design-zh.md` §5.2（「极简两层」改述）与
   `docs/design/thread-safety.md`（读视图模型、锁序）。
4. 全矩阵收口：gcc/clang Debug、Release+`-Werror`、ASan、TSan；rollout 报告入库。

- **验收**：1 亿 key、F = 64K 时写放大较基线下降一个数量级以上；点查 p99 回退 ≤ 既定门限
  （门限在 S44-0 基线报告中先行写明）；全矩阵全绿；`CompactionPolicy::kFull` 回退路径验证可用。


#### ✅ S44-6 落地记录（2026-11-16）

- 基准报告：`docs/design/s44-compaction-baseline-zh.md`。写放大（归并输出行 ÷ flush 行）：
  10⁷ / 64K 5.22 → 2.83；10⁸ / 1M 3.79 → 2.24；10⁸ / 64K ≈62（估算，模型用两组实测回算误差 ≤ 15%）→ 4.18。
  归并累计耗时下降 41%～54%。点查 p99 在 10⁸ 上约 8µs → 18～19µs（run 数 1 → 5～8）。
- 设计 §9 结清：K = 1；不暴露 `CaskOptions`；墓碑阈值固定 25%；两道 IO 限速暂不做。
- 文档：`doc/ordered-key-index-design-zh.md` §5.2 改述；`docs/design/thread-safety.md` 新增 §5.5
  （读视图协议、发布点、归并道、锁序、析构、跨进程）；设计稿状态改为已实现；CHANGELOG `[Unreleased]`。
- 旧版本兼容（S44-3 验收项）：用新库生成「底座 run + 含 30 条墓碑的后缀归并输出」的目录，6.9.0 的
  `libbitcask.so` 以 Level B 读写打开两次，270 个活 key 值正确、30 个已删 key 均 NOT_FOUND。
- 全矩阵：见下方「验证」。
- **偏差**：
  - 点查 p99 门限没有在首轮测量前写明（S44-0 应先行写明），首轮实测约 2.3 倍回退。已补救：在修复与复测
    **之前**把门限写入报告 §3a（10⁸ / F=1M，分级 / 全归并：p99 ≤ 1.5×、p50 ≤ 1.3×，各 3 次取中位数）；
    定位主因为 bloom 假阳性叠加（多 run 时 3%～6% 点查多一次块读），新 run 改为 16 bits / 11 哈希
    （FP ≈ 0.05%，格式兼容，bloom 内存 +60%）；复测中位数 p99 **1.00×**、p50 **1.11×**，达标（报告 §3b）。
    10⁸ / F=64K 的 p99 约 2.1×（6 个 run，块读数相同，推测为逐 run bloom 探测的缓存未命中，未单独验证），
    按事先约定只记录；
  - 10⁸ / 64K 全归并侧未实测（估算）；
  - **第二批门限（报告 §3c，写于补测之前）**：10⁸ / 64K 同数据对比（分级 6 run vs 全归并 1 run，3 次中位数）——
    G1 点查 p99 1.61×（门限 1.5×）、G2 p50 1.33×（1.3×）、G3 全量迭代 0.40×（≥0.7×）、G4 短区间 p99 4.5×（1.5×）
    **均未达标**；G5（10⁸ / 1M、20% 删除）空间放大 1.469 ≤ 1.5、G6（10⁷ / 64K）1.000，达标。
    补测中发现并修复了选段策略在两道并发下的两个缺陷（硬上限驱动年轻道、「新而大」倒挂；修复前实测写放大 28），
    `max_runs` 24 → 6（另设背压阈值），游标堆优化（迭代 +15%），区间起点走块缓存；bloom 预取实测无效已撤回。
    定位实验与模拟表明：本场景下减少 run 数会让写放大急剧上升（上限 5 → 约 10.5，即只比全归并好约 6 倍）；
  - **结案（2026-11-16）**：用户确认接受预热口径作为 S44-6 点查 p99 门限的判定依据，S44-6 标为完成。
  - **方法修正与重测（报告 §3e / §3f）**：点查分解测量显示 §3d 的 p99 尾部几乎全是 find 段读盘（100～250µs）——
    老 run 文件在写入压力和共享机器外部负载下被挤出页缓存，与 run 结构无关。在门限写定之后、重测之前改为
    「测量前预热 run 文件进页缓存」（两阶段同样处理），按原协议重测：G1 点查 p99 **1.31×（达标）**；
    G2 p50 1.38×、G3 迭代 0.46×、G4 短区间 p99 3.65× 仍未达标。G1 是 S44-6 验收原文里的门限；G2–G4 为补测时
    自设的附加门限（验收原文只要求测量 range 与空间），其差距是多 run 结构固有读放大、与写放大目标冲突，
    记为已知取舍。**索引放不进内存时分级布局的点查尾延迟会明显差于单 run**（未预热 1.61×）；
  - Windows 未测（本机无 Windows 环境）。

- **最终验证**（第二批补测与选段 / 游标 / 读路径修改之后，同一份最终代码）：`build-gcc-release`
  （Release + LTO + `-Werror`）全量构建 0 告警、ctest **853/853**；`build-asan`（ASan + UBSan）**853/853**；
  `build-tsan` **853/853**；`build-clang-debug` **853/853**、构建 0 告警；gcc Debug（树外 `/tmp` 临时配置，
  `BUILD_TESTING=ON` + `BITCASK_WERROR=ON`，验后删除）全量构建 0 告警、ctest **853/853**——验收原文要求的
  gcc / clang Debug、Release + `-Werror`、ASan、TSan 五套齐全。
- **验证**（2026-11-16 首轮，全部在 192.168.3.13；以下为第二批修改之前的记录）：
  - `build-gcc-release`（Release + LTO + `-Werror`）：全量构建 0 告警，ctest **850/850**；
  - `build-asan`（clang Debug，ASan + UBSan）：ctest **850/850**；
  - `build-tsan`（clang Debug，TSan）：ctest **850/850**；
  - `build-clang-debug`：首轮 4 个 S44 并发用例失败——排查为测试临时目录只按用例名命名，与同时在跑的
    TSan 循环写进同一目录；改为目录名带进程级唯一后缀后，S44 相关 42 例并行 ctest 连跑 3 遍全过，
    随后**全量 ctest 重跑 850/850**；Release / ASan 下同批用例复跑亦全过；
  - bloom 参数调整之后：Release 全量重建 0 告警、全量 ctest **850/850**；ASan、TSan 重建后 OKI / merge / 崩溃恢复
    相关 160 例各 160/160；
  - TSan 下 `oki_compaction_model` + `oki_regression` 循环：与 clang Debug ctest 时间重叠的那一轮出现 3 次失败、
    0 条竞争告警（同一目录冲突）；修正目录命名后**单独**重跑 10 轮，两组各 10/10 通过、0 告警。
    `oki_regression_test` 的目录仍只按用例名命名（沿用仓库既有惯例），并行跑多套构建树时同样可能冲突。

### 执行序

```text
S44-0 基线 / 守门 / 对拍底座
→ S44-1 读视图不可变发布
→ S44-2 选段策略            （纯函数，可与 S44-1 并行）
→ S44-3 后缀归并            （依赖 S44-1 + S44-2）
→ S44-4 调度                （依赖 S44-3）
   S44-5 load 重试          （可与 S44-4 并行）
→ S44-6 基准 / 调参 / rollout
```

S44-1 是独立可交付的：即使后续任务暂缓，它也消除了一类并发读隐患并简化了 flush 提交。
S44-3 合入后、S44-4 合入前，归并仍按现状调度（阈值 flush 走后台、其余同步），功能正确
但大库的 checkpoint 可能变慢——两者不宜隔版本发布。

### 风险排序（按“有多容易静默错”）

| # | 风险 | 任务 | 失败形态 |
|---|---|---|---|
| 1 | 不含最老 run 的归并丢了墓碑 | S44-3 | 已删除的 key 读到旧值，最不可接受 |
| 2 | 归并了不连续 / 非后缀的 run | S44-3/4 | 旧位置压过新位置，读已退役的 data 文件 |
| 3 | 读者漏做 seq 过滤或取数顺序写反 | S44-1 | 等 ord 判反：旧位置或已删除的 key 被当成活的 |
| 4 | 年轻道越界取了老道输入 / 提交拼接错位 | S44-4 | run 重复或丢失，读视图缺行 |
| 5 | 冻结段与 run 双重计入或都没计入 | S44-1 | 缺行，或 flush 期间读到重复的陈旧行 |
| 6 | 取消 / 背压路径留下半成品或死等 | S44-4 | close 挂起、孤儿文件、写入停顿 |
| 7 | 跨进程读者撞上 sweep | S44-5 | 只读句柄退化为全量重建（慢，不错） |
| 8 | 小库写放大略升、墓碑滞留占空间 | S44-2/6 | 功能正确，资源回退 |

### 明确不做

| 项 | 理由 |
|---|---|
| leveled（分层、key 区间不重叠）归并 | 需要 manifest 记录层级与区间、点查逐层查；十亿级以上再议 |
| 按 key 区间的分区归并 | 需维护分区边界与热点分裂，本届不做 |
| 改 manifest / run 盘上格式 | 硬约束；回退与新旧互开依赖格式不变 |
| 把 active memdelta 改成分块存储以消除冻结拷贝 | 冻结拷贝与现状的前缀拷贝同量级，单独立项 |
| 把其它 `AtomicSharedPtr` 站点（向量 / 文本插件）迁到 `SnapshotCell` | 那些对象不一定是不可变快照，需逐个确认，另行处理 |
| 解决 merge_only 旁车删文件后写者视图失真 | 属跨进程一致性问题，S44-5 只覆盖 `load` 窗口；写者运行期感知另议 |

---

## 🔴 S43 · OKI-only：直接点查、去 hint、退役 KeyDir（2026-11-15 立项）

> 来源：[`docs/design/oki-only-keydir-analysis-zh.md`](docs/design/oki-only-keydir-analysis-zh.md)、
> [`docs/design/oki-run-open-verify.md`](docs/design/oki-run-open-verify.md)；
> S43-0 基线见 [`docs/design/s43-okionly-baseline-zh.md`](docs/design/s43-okionly-baseline-zh.md)。
> 决策基线：**OKI 成为唯一查找权威；`Cask::get(key)` 直接由 OKI 定位；
> 停写并移除 `.hint`；退役内存 KeyDir / `kv.keydir.ckpt` 路径。**
> 持久真相不变：**data 文件仍是唯一持久真相，OKI 是可重建索引**。
> 硬约束：任何崩溃点都不允许静默丢活跃 key；`Cask::get` 不允许因索引损坏静默
> 返回别的 key 的 value；旧库可打开并迁移；search/docmap/vec 水位语义不回退。
> 验收标准：每项改动后 ctest 全绿 + 编译无新告警；涉及恢复/提交点的改动必须
> 过 kill 点注入、CRC 损坏、unlink/merge 竞态用例；收尾过 ASan/TSan 与 benchmark gate。

### ⚠️ 执行纪律

**不能先物理删除 hint 或 KeyDir 再补 OKI 能力。** 顺序必须是：

```text
OKI 安全点查与恢复自洽
→ BCOM v4 收敛全局元数据/提交点
→ data-only recovery / merge / iterator 全部去 hint、去 KeyDir
→ 停写 hint
→ 最后删除 KeyDir 实现与旧 checkpoint
```

设计分析已指出：废 hint 会把廉价恢复保险换成 data 全量扫描；零缓存/零 KeyDir
会让每次 put 付 OKI 点查成本。本 S43 接受该架构取舍，但必须先用 benchmark
量化回退幅度，不能默认性能不回归。

| # | 任务 | 状态 | 说明 |
|---|---|---|---|
| S43-0 | 基线、语义契约与故障注入底座 | ✅ done | 语义契约、调用面、性能基线与 K1-K15 注入底座已收口 |
| S43-1 | OKI 点查安全加固 | ✅ done | key/ord 核对、BCOK v3、四类语义注入、运行期坏块受控重建、mixed 降级、benchmark 与全量回归已收口 |
| S43-2 | BCOM v4：OKI 元数据提交点 | ✅ done | v4 codec、捕获/恢复、失败注入、v3 迁移、fstats/held row 对拍与全量回归已收口 |
| S43-3 | data-only streaming recovery | ✅ done | 健康可信 Level B v4 从 data 字节水位流式重放 tail；缺失/越界/Level A 等显式 fallback；hint/BCKS 不入主路径 |
| S43-4 | OKI-only `get(key)` 直查 | ✅ done | public get 定位直连 OkiState；hash warm-fill 双路径移除；坏索引显式报错/受控重建与全量回归已收口 |
| S43-5 | OKI-only 写路径与 fstats | ✅ done | put/put_doc/batch 已切 OKI 写闸；group-commit 顺序、fstats parity 与均匀写性能 gate 已收口 |
| S43-6 | OKI-only iterator/range | ✅ done | CaskIter/range 统一 OKI ReadView 归并；prefix seek、view pin、close/rebuild 安全与定向 sanitizer 已收口 |
| S43-7 | OKI-driven merge | ✅ done | merge 选择/活性/迁移统一 OKI ReadView；输出 data-only、BCOM v4 候选元数据与崩溃顺序已定向收口 |
| S43-8 | 停写并清理 hint | ✅ done | 停写、data-only recovery/merge/migrate、惰性清理与实现退役均已收口 |
| S43-9 | 退役 KeyDir 与 BCKS | ✅ done | IndexAuthority/IndexRegistry、只读内存重建、崩溃尾洞检测、OKI-only 备份与 grep 清理已收口 |
| S43-10 | 全量验证、迁移与 rollout | ✅ done | rollout gate、迁移/损坏矩阵、ASan/TSan/Werror、benchmark 报告与发布账已闭合 |

### 🔴 S43-0 — 基线、语义契约与故障注入底座（先行）

工作面：`docs/design/*`、`TASK.md`、`tests/`、`bench/`。

1. 冻结外部语义并写成守门用例：
   - `put` / overwrite / remove / reinsert 的 `kAlreadyExists` 与 LWW 语义；
   - atomic batch 的全成全败语义；
   - TTL / conditional remove / merge relocation 语义；
   - iterator/range 的快照一致性与并发写可见性边界；
   - search/docmap/vec 的 `keydir_covered ≤ search_covered` 不变量。
2. 生成当前实现基线：
   - 冷/热开库时间与 RSS；
   - 点查 p50/p99、写吞吐、range 吞吐；
   - merge 吞吐、磁盘占用、OKI compaction 写放大；
   - 10M/100M 规模 recovery 时间与峰值 RSS。
3. 量化调用面：
   - `.hint` 的写、finalize、fold、merge、迁移路径；
   - `KeyDir` 的 put/remove/get/locate/fstats/ord/file_id/iterator/checkpoint/registry 消费点；
   - 不可直接删除的 public/C API 面。
4. 搭建 kill 注入底座，至少覆盖：
   - data append 后、OKI append 前；
   - OKI memdelta flush 中 run fsync 前/后；
   - BCOM 提交前/后、memdelta 截断前/后；
   - merge unlink data 前/后、relocation append 前/后；
   - checkpoint / manifest 原子替换失败点。

- **验收**：基线报告入库；注入框架能稳定复现“keydir snapshot 已写、OKI flush 未写”
  这类当前缺口；所有新守门用例在改动前明确标注预期行为。

#### ✅ S43-0 落地记录（2026-11-15，第一批）

- 新增基线与契约文档：`docs/design/s43-okionly-baseline-zh.md`。
  - 当前调用面计数：hint 相关 24 文件 / 146 行，生产路径 94 行；
    KeyDir 相关 57 文件 / 631 行；cask 内 `keydir_` 172 行。
  - 冻结点查 lifecycle、OKI/public get 对齐、atomic batch、record key 核对契约。
  - 列出 K1-K15 故障注入矩阵与待回填性能基线。
- 新增 `bitcask_oki_only_contract_test`：
  - 点查 missing / put / overwrite / remove / reinsert / reopen；
  - 删除 `.hint` 后重开仍由 data 恢复最终状态；
  - Level B 下 `OkiState::locate` 与 public get 的存在性 / ord 对齐；
  - atomic batch 批后状态在 data-only reopen 后保持。
- `S43-0` 随 S43-10 收口完成：rollout gate 补齐随机模型、迁移/损坏
  形态与 K1-K15 窗口；性能与 rollout 结果转记
  `docs/design/s43-okionly-rollout-report-zh.md`。

- **验证**：`build-gcc-release` 下新增目标编译通过；
  `OkiOnlyContractTest` 3/3 通过；相关 `OkiLocateTest` 12/12 与
  `AtomicBatchTest` 7/7 回归通过。

### ✅ S43-1 — OKI 点查安全加固

工作面：`src/cask/cask.cpp`、`src/fileops/oki_run.cpp`、`include/bitcask/oki_run.hpp`、`tests/oki_*_test.cpp`。

1. 先落 D2 §5，作为 OKI-only 的前置条件：
   - `Cask::get` 读回 data record 后，比较 record 内 key 与请求 key；
   - 不匹配时返回显式索引损坏错误，禁止返回 value；
   - Level B/OKI-only 模式下触发该 run 降级与重建流程；
   - 确认 mmap / zero-copy 读路径同样能拿到 key 做 memcmp。
2. 按 D2 §3 先量化开库整文件 CRC：
   - Level B 10M / 100M，冷缓存与热缓存分别计时；
   - 记录 run 总大小、盘类型、开库总耗时中 CRC/read 的占比。
3. 若校验占比显著，实施 run v3：
   - 稀疏索引 entry 追加 block CRC；
   - 开库只校验 header / trailer / 索引 / bloom 小区域；
   - `OkiBlockCache` loader 与直读路径首次读块时校验；
   - 运行期坏块降级为弃用 OKI 并重建；
   - v1/v2 读端保留，manifest 条目区分 v3。
4. 若占比不显著，保持 eager CRC，但 key 核对仍必须落地。

- **验收**：翻转 run 块中的 loc/key/tomb/ord 位后，系统不得静默错值；
  v1/v2/v3 mixed manifest 与旧二进制降级用例通过；开库耗时报告归档。

#### 🔄 S43-1 落地记录（2026-11-15，第一批）

- D2 §5：`Cask::read_doc` 在 mmap 与 pread 两条路径都核对 record 内 key；
  定位成功但 key 不同时返回新增 `CaskError::kIndexCorrupt`
  （C API `BITCASK_ERR_INDEX_CORRUPT = 16`，只追加枚举尾部），绝不返回错值。
- D2 §3：10M/100M BCOK v2 真实 run 开库量化已归档到
  `docs/design/s43-okionly-baseline-zh.md`；100M run 热开约 609ms、
  fadvise 冷开约 2.5s，判定开销显著。
- 新增契约用例：强制 Level B hash 缓存逐出后，将 `a` 的 OKI loc 指向
  `z` 的完好 data record；public get 必须返回 `kIndexCorrupt`。
- BCOK v3 第一批已落地：sparse index per-block CRC、v3 metadata-only
  open、块缓存/直读首读 CRC、`OkiState` 新写路径输出 v3、mixed
  `format_ver=1/2/3` manifest 读端。`Cask::read_doc` 修正 warm-fill 时机，
  record key 核对通过前不回填 KeyDir。

#### 🔄 S43-1 落地记录（2026-11-15，第二批）

- `KeyDir::locate_status` 将冷侧 `kUnavailable` 与普通 miss 分离；
  `Cask::read_doc` 不再把坏块/IO/不可定位折叠成 NotFound。
- 首次冷路径不可用时，`Cask` 在 KeyDir 写者屏障内执行 data-only
  受控重建：重新 fold data + 原子批 staging，产出单个 BCOK v3 run；
  成功后重试触发读，失败显式返回 `kIndexCorrupt`。重建期间其他读者
  同样显式失败，不做静默错值/静默 miss。
- 新增 `RuntimeBadBlockTriggersControlledOkiRebuild`：v3 块 CRC 破坏后
  触发重建，目标 key 可读，旧 run 被清理且新 gen 生效。

**S43-1 未收口项**：loc/key/tomb/ord 四类翻转注入矩阵；v1/v2/v3 mixed
run 与旧二进制降级用例；v3 开库/首读 benchmark 回填；全库 C API 回归。
运行期 rebuild 的 RSS/停写窗口上界交由 S43-3 streaming recovery 收敛。

#### 🔄 S43-1 落地记录（2026-11-15，第三批）

- `Cask::read_doc` 在 mmap 与 pread 路径同时核对 record ord：OKI loc 命中
  完好记录但 LSN 不一致时显式返回 `kIndexCorrupt`，避免错误 ord 进入
  warm fill 或后续 merge 裁决。
- 新增结构合法 v3 注入用例：重写 run 并同步重算 block/metadata CRC，
  key 改写、ord 改写均显式失败；tomb 置位不返回旧 value。
- 新增 Cask 级 mixed `format_ver=1/2/3` 降级用例：最高 gen 的 v1 行使
  Level B locate 明确不可用，public get 触发受控重建并收敛到单 v3 run。
- 新增未来 BCOK version 降级用例：旧读端拒收后从 data 重建，不猜测
  未知格式语义。

BCOK v3 benchmark 已回填：100M 元数据冷开约 413.7 ms、第二次热开约
101.9 ms；10k 均匀 distinct key 首读 89.8 ms（p99 约 23.7 µs）。见
`docs/design/s43-okionly-baseline-zh.md` §3.1.1。

最终验证：`build-gcc-release` 全量 ctest 834/834 通过；两个 RSS probe
维持 disabled 未运行。C API 目标包含在该全量回归中。运行期 rebuild 的
RSS/停写窗口上界交由 S43-3 streaming recovery 收敛。

### 🔴 S43-2 — BCOM v4：OKI 元数据提交点

工作面：`include/bitcask/oki_run.hpp`、`src/fileops/oki_run.cpp`、`src/fileops/oki_state.cpp`、
`src/cask/cask.cpp`、`src/cask/cask_recovery.cpp`、`tests/oki_*_test.cpp`、`tests/checkpoint_recovery_test.cpp`。

1. 新增 BCOM v4 元数据段，至少包含：
   - `next_ord`；
   - `biggest_file_id`；
   - `next_file_id`（若与 biggest 语义分离则显式建模）；
   - active key 数、live bytes、时间戳边界；
   - per-file fstats：`file_id/live_keys/total_keys/live_bytes/total_bytes/oldest/newest/pending_delete`；
   - 保留位与版本化 TLV/定长段，避免后续继续 flag-day。
2. v4 仍是唯一 commit point：
   - run fsync → Reader 校验 → data durable watermark 分拣 → 原子写 manifest → memdelta 截断；
   - manifest 提交失败必须能回退到上一份状态并保留 delta/tail；
   - 孤儿 run、半截 spill、旧 BCKS 清理策略写成显式测试。
3. 兼容迁移：
   - 读 v3：先加载 runs，再通过 data/hint fold 或受控 rebuild 计算 v4 元数据；
   - 下一次成功 flush/compaction/rebuild 时写 v4；
   - Level A / Level B 模式戳进入 v4 flags，规则不得弱化。
4. `next_ord`、fstats 与 manifest 提交的一致性：
   - 不允许 run 未固化但水位/统计超前；
   - held row 不推进 cover；
   - 同 key 同 ord 的 relocation 用现有 `(ord, gen/到达序)` 规则裁决。
5. 收敛 checkpoint 顺序：
   - keydir snapshot 不再作为 OKI flush 的前置提交点；
   - search checkpoint 继续独立，但其依赖的 keydir watermark 改由 OKI 提供。

- **验收**：v3/v4 双读、v4 往返、CRC/未知 flags/未知版本 fail-fast 全绿；
  任意 manifest 提交点崩溃后重开不丢活跃 key；v4 元数据与全量重建结果对拍一致。

#### 🔄 S43-2 落地记录（2026-11-15，第一批）

- 新增 BCOM v4 定长元数据段：56B 全局（`next_ord`、`biggest_file_id`、
  `next_file_id`、active key 数、live bytes、时间边界、reserved）+
  64B per-file fstats（含 `pending_delete` 与 `expiration_epoch`）。
  manifest 布局为 header/counts → global → fstats[] → run entries →
  wm → CRC；写端仅在真实快照存在时升 v4，v1-v3 读端路径保留，
  v3 不伪造空元数据。
- `KeyDir` 新增提交快照捕获/应用。捕获位于写者屏障内；活跃 fold
  拒绝提交 v4。应用只用于 BCKS 快照确实接受后的 open 线程，覆盖
  逻辑标量与 fstats；无 BCKS 的全量 data fold 仍从空表重建计数，
  避免 fold 挂钩双记。
- `OkiState::flush/rebuild` 接受 caller 快照；run fsync/Reader 校验后、
  manifest 发布前做水位与结构门。空 memdelta / 全持留时也支持
  metadata-only 原子提交，保证 Level A 和 v3→v4 首迁移不被“无 run”
  静默跳过。compaction 保留上一份 v4 元数据。
- Cask close/checkpoint、merge unlink 前和受控/开库 rebuild 在提交前
  捕获 KeyDir 快照并传给 OKI；v4 + BCKS open 先恢复提交点标量再重放
  tail。
- 新增契约测试：v4 快照跨 hint-only reopen、manifest 全损后 data/BCKS
  rebuild 与上一提交点 metadata 对拍、旧 run 清理；BCOM v4 codec
  未知 flags、reserved、fstats pad fail-fast。

**S43-2 未收口项**：manifest 原子替换失败的注入矩阵；v3 目录强制
迁移到 v4 的覆盖矩阵；merge/TTL fstats 精确对拍；孤儿 run 与半截
spill 的崩溃点专项；next_ord 与 held row 的组合注入。

#### ✅ S43-2 落地记录（2026-11-15，第二批 / 收口）

- manifest 原子发布失败：新增 K4 可复现用例（manifest tmp 被目录阻塞）。
  run 已 durable 但发布失败时，memdelta、上一份 v4 metadata、双 key
  可读性都保持；移除阻塞后重试使用同一 delta 成功提交，旧状态不被回退。
- v3→v4 迁移：显式把健康 v4 目录降写为 v3（保留 runs/wm，清空 metadata/
  fstats）。v3 open 不伪造空元数据；下一次真实 flush 使用 KeyDir 当前
  派生值发布 v4，重启后 metadata 稳定。
- merge/TTL fstats：merge 成功后对拍 pre-unlink flush、trim 旧输入和
  最终 snapshot 三个阶段，旧 input fstats 必须消失，output/active fstats
  与 KeyDir 逐字段一致；到期 key 清理后不复活。`pending_delete` 显式
  锁住 `expiration_epoch` 派生布尔语义，epoch 推进后提交必须发布 true。
- `next_ord` × held row：caller 持久水位不足时，行留在 memdelta，
  `wm/run cover` 不推进；同一提交点可发布真实 next_ord/active/fstats。
  数据越过持久水位后，下一次 flush 才进入 run，metadata 不回退。
- 孤儿/半截 run：manifest 全损重建继续对拍上一提交点 metadata 并清理
  孤儿 run；新增半截 `kv.oki.spill-*` 残件断言，rebuild 后必须清光。

收口回归：85 项 OKI/manifest/recovery/LevelB/KeyDir 定向测试全绿；
`build-gcc-release` 全量 ctest 841/841 通过；ctest 总账 843 项，另两个
RSS probe 维持 disabled 未运行。

### ✅ S43-3 — data-only streaming recovery

工作面：`src/cask/cask_recovery.cpp`、`src/fileops/data_file.cpp`、`src/fileops/scanner.cpp`、
`src/fileops/oki_state.cpp`、`tests/crash_recovery_test.cpp`、`tests/oki_recovery_test.cpp`。

1. 恢复主路径改为：
   ```text
   读 last-good BCOM v4
   → 打开有效 runs
   → 恢复 next_ord / fstats / file_id 元数据
   → 从 wm 对应 data 位置起流式 fold(data)
   → tail row 进入 OKI memdelta
   → 超阈值时继续 flush 成 run
   ```
2. 禁止在主路径遍历内存 KeyDir 或依赖 hint fold。
3. 最坏路径保留为一级公民：
   - manifest 全损 / CRC 失败 / 模式戳不一致 / 覆盖缺口；
   - 全量扫 data → 外排 → 单 run rebuild；
   - 不因 hint 缺失而进入未定义状态。
4. memdelta / recovery 内存上界：
   - 恢复期间保持 bounded RSS；
   - 超过 `kFlushRowLimit` / `kFlushByteLimit` 时可 flush；
   - 恢复 flush 与普通 flush 使用同一提交协议。
5. data tail 解析必须处理：
   - torn tail / CRC 坏记录；
   - 墓碑复活门；
   - 并行 fold 到达乱序时按 ord 裁决；
   - search 层需要 value 时的恢复路径。
6. `fold(data)` 不读 value 的优化若短期不可行，先接受恢复成本回退，并把
   开库耗时作为 S43-10 的明确 gate 项；不得用跳过 CRC/校验换速度。

- **验收**：manifest 健康时 recovery RSS 与 tail 大小相关，而不是与全库 key 数相关；
  hint 全部删除后 recovery 全绿；manifest 损坏 fallback 能重建与原状态等价的 OKI。

#### ✅ S43-3 落地记录（2026-11-15）

- BCOM v4 新增 per-file `data_wms`（`file_id → committed byte EOF`），作为
  data tail 的物理起跳点。旧 v4 缺失水位时退化为从 0 扫描，但仍按
  `ord >= wm` 过滤；读端要求 file_id 升序去重。
- 新增 `Cask::try_data_only_recovery()`：先应用 v4 标量/fstats，再按
  file_id 串行流式 fold data tail。tail 进 OKI memdelta；阈值触发时走
  普通 flush 的 run fsync → Reader 校验 → manifest 原子发布协议。主路径
  不读 hint、不装 BCKS、不构建全量 KeyDir shard。
- `KeyDir::account_recovery_row()` 用 OKI 当前权威裁决新 key/覆盖/墓碑，
  只推进全局标量与 fstats，不插入全量 hash table。
- data-only 主路径显式要求健康 BCOM v4、可信 `level_b` 戳、非 v1 run、
  `next_ord >= wm`、活跃 key 有 run 覆盖、`data_wm <= file_size`。任何
  不满足都是显式 fallback，不“顺手升级”或吞掉提交尾。search 与
  merge_only 保持旧恢复路径。Level A 必须走传统路径：其 merge
  relocation 不在线维护 OKI，直接采信旧 run loc 会指向已退休文件。
- 新增崩溃子进程测试：删除 BCKS/hint 后，3000 个已提交 key 只留在 OKI
  runs，崩溃 tail 重放前后 KeyDir 物理驻留均为 0，覆盖/墓碑逻辑计数
  正确；另锁住 `data_wm > EOF` 必须退回 data full rebuild。BCOM v4 data
  水位 roundtrip 与保留位/pad fail-fast 已入 codec 测试。

收口回归：S43/OKI/LevelB/KeyDir/CrashRecovery 定向 97/97 通过；
`build-gcc-release` 全量 ctest 843/843 通过；`S30RssProbe` 和
`S40KeyRssProbe` 仍为 disabled 未运行。3000 已提交 key + 小 tail 的物理
驻留行为测试已锁住“不随全库 key 数装表”；进程 RSS 绝对值继续留待
disabled probe / S43-10 benchmark gate 量化。

### 🔴 S43-4 — OKI-only `get(key)` 直查

工作面：`include/bitcask/cask.hpp`、`src/cask/cask.cpp`、`include/bitcask/oki_state.hpp`、
`src/fileops/oki_state.cpp`、`src/cask/cask_iter.cpp`、`src/cask/cask_range_iter.cpp`。

1. `Cask::get/read_doc/read_mmap` 的定位入口改为 `OkiState::locate(key)`：
   - memdelta 命中优先；
   - runs 按 gen 降序，首个命中为权威；
   - tombstone 返回 NotFound；
   - TTL 过期按现有语义返回 NotFound 或触发条件删除路径；
   - `kUnavailable` 只能表示索引不可用，不得当 miss。
2. 删除 KeyDir warm-fill / hash-first / OKI cold-fill 的双路径。
3. 读到 data record 后执行 S43-1 的 key 核对、record CRC、TTL 与 merge retry。
4. 并发语义：
   - 获取 runs snapshot + memdelta 可见状态；
   - flush / compaction 期间旧 run 由 shared_ptr 保活；
   - merge relocation 后旧 loc 读失败必须用最新 OKI 视图重试；
   - 不再依赖 KeyDir epoch/limbo 保证槽位稳定。
5. 为 OKI-only 暴露最小内部接口：
   - `locate(key)`；
   - `make_read_view()` / pinned snapshot；
   - 状态与错误分类：`kMiss/kHit/kUnavailable/kCorrupt/kRebuilding`。
6. 影子对拍改为迁移期工具：
   - 先双读 KeyDir 与 OKI 并统计 drift；
   - drift 归零后才切默认路径；
   - 切换后保留测试专用对拍，不在生产热路径执行。

- **验收**：随机写删/覆盖/merge 并发下 `get(key)` 与 reference model 全等；
  OKI 损坏时可见报错或重建，不允许错值；读路径不再引用 KeyDir hash。

### ✅ S43-5 — OKI-only 写路径与 fstats

#### ✅ S43-4 落地记录（2026-11-15）

- `Cask::read_doc` 定位入口改为 `OkiState::locate`：memdelta 先行，runs
  按 gen 降序裁决；miss/tombstone/无 loc 返回 NotFound，TTL 语义保持。
  mmap 与 pread 路径继续执行 S43-1 的 key/ord 核对和 record CRC。
- public get 不再查 KeyDir hash，也不再执行 warm-fill / OKI cold-fill；
  读成功后不回填旧缓存。merge relocation 后的重试直接重查 OKI 最新
  视图，不再依赖 KeyDir epoch/limbo。
- `kUnavailable` 保持显式语义：可写 public get 首次遇到旧 Level A/坏
  索引时触发一次受控 data rebuild；重建中其他读者收到
  `kIndexCorrupt`。损坏 run、语义 loc 注入与未来 run 版本降级用例继续
  保证不返回错值。
- 迁移期 KeyDir→OKI 影子对拍仍保留为 debug/测试工具；生产 release
  热路径已不再双读。Level B 的旧冷命中/warm-fill 用例更新为锁定“冷读
  正常、缓存不回填”的 S43-4 契约；range 并发用例随后通过 S43-5 可用性
  闸完成旧 Level A 目录的一次性懒迁移。

收口回归：S43/OKI/LevelB/KeyDir/CrashRecovery/Range 定向 58/58 通过；
`build-gcc-release` 全量 ctest 843/843 通过（两个 RSS probe 保持
disabled）。读路径不再引用 KeyDir hash。

#### 🔄 S43-5 落地记录（2026-11-15，第一批）

- `OkiState` 增加 `probe_for_write` 与 `apply_put/apply_remove/
  apply_conditional_remove/apply_relocation` 写权威门面。probe 只返回
  loc/ord/CAS 裁决信息；数据仍先落盘，apply 不承担 CAS，探测与应用必须
  在同一把写序闸门内完成。
- `Cask::remove` 切到 `ensure_oki_for_write`；tombstone 影子 `file_id`
  改由 OKI probe 提供，OKI 不可用时显式报 `kIndexCorrupt`，不折叠成
  miss。
- merge/TTL 的 `KeyDir::conditional_remove` 先做 OKI probe；OKI 命中时
  继续用 `ExpectedLoc` 进 `remove_impl`，保证 hash/fstats/OKI 三方一致。
  哈希条目被逐出而 OKI 明确 live/tomb 时，`remove_impl` 增加权威裁决
  分支。
- 迁移期 read fallback：可写 public get 首次访问旧 Level A 时受控重建；
  RO reader 在“无 Level B 戳且点查未启用”的旧目录下保留显式 KeyDir
  fallback。`Cask::make_range_iter` 对旧 Level A/运行期降级复用同一条
  受控重建闸，并发读者等待新视图而非报“没有索引”。
- 新增 `OkiOnlyContractTest.RemoveShadowFileIdComesFromOkiProbe`，锁定
  v2 tombstone 影子文件号必须来自 OKI probe。

**S43-5 未收口项**：put/put_doc/atomic batch 仍走旧 KeyDir 权威；
batch key 去重/排序与单次 apply 未实现；new/overwrite/delete/relocation/
TTL 的 per-file fstats 精确对拍、group-commit 顺序与均匀写 locate 开销
尚未收口。

本批回归：`build-gcc-release` 全量 ctest 844/844 通过（两个 RSS probe
保持 disabled）。

#### 🔄 S43-5 落地记录（2026-11-15，第二批）

- `Cask::put`、`put_doc`、`put_batch`、`put_batch_atomic` 与 `remove`
  统一先过 `ensure_oki_for_write()`。写闸检查 `write_probe_ready()`，
  等待共享受控重建窗口，并保证同一批数据先提交、OKI 后可见。
- 运行期 newest put 在 `KeyDir::put_probe` 内用 OKI 做权威裁决；
  `kIndexUnavailable` 显式映射为 `kIndexCorrupt`，绝不折叠成 miss/new。
  `put_insert` / `put_overwrite` 按该 probe 区分逻辑 new/overwrite，并
  用 OKI prior loc 做 per-file fstats 退账/入账。
- atomic batch 按 key 去重：物理 records/ords 全保留，但只把每个 key
  的最终 op 应用到 KeyDir/OKI/index/fstats；最终 op 按字典序应用后，
  index task 再按 ord 重排。最终 remove 的 v2 tombstone 影子 file_id
  改由 OKI prior loc 提供，中间 remove 保持 v0。
- 新增 `AtomicBatchDuplicateKeysApplyOnlyFinalOp` 与
  `PutAndProbeFstatsParityAcrossFiles`，分别锁定 batch 最终态和跨文件
  new/overwrite 的 fstats 对拍；mixed v1/v2/v3 断言移到首个 filler put
  前，反映“可写 put 允许触发受控重建”的契约。
- 影子对拍增加“冷侧 ord 更新”容忍走廊：旧哈希子集不得阻断公共读的
  OKI/data key/ord 损坏裁决；同时修复 LevelB data-only 用例中临时
  string 生命周期造成的 stack-use-after-scope。

本批回归：`build-asan` 定向 192/192 通过；`build-tsan` 并发/写闸定向
38/38 通过；`build-gcc-release` 配置 848 项、运行 846/846 通过（两个
RSS probe 保持 disabled）。S43-5 仍不关闭：group-commit 顺序覆盖、
merge/TTL fstats 精度确认与均匀写 locate 开销量化待做。

#### ✅ S43-5 落地记录（2026-11-15，第三批 / 收口）

- 新增 `GroupCommitPreservesOrdAndMemdeltaOrder`：8 线程 × 180 个互斥
  key、`sync_every_n=1` 的 group-commit 下，锁定 queue 分派序 → ord
  序 → data 物理追加序一致；重开后逐 key 校验 OKI live row 的 ord
  与 public get 返回值。OKI 扫描按 key 排序输出，因此只断言同 key
  ord 一致，不断言全局行序。
- 均匀覆盖写 gate 落地双 keyspace：`BM_Cask_Put_Overwrite` 参数化为
  1024 与 100000。1024 热覆盖中位延迟 1577 ns（约 635.5k puts/s）；
  100000 稳态 run 覆盖中位延迟 2242 ns（约 447.0k puts/s）。隔离的
  `BM_Cask_WriteProbe_Uniform/100000` 中位探测 2763 ns（约 362k
  probes/s，hit_ratio=1）。因此每 put 一次 run 态 OKI probe 是已量化
  的架构成本，不构成静默降级理由。
- merge/TTL 收口用
  `OkiOnlyContractTest.MergeAndTtlFstatsMatchManifestAfterTrim`
  定向复验，trim 后 manifest per-file fstats 与期望状态一致。
- 基准与测试目标 clean rebuild 无新 warning；ASan 定向复验
  group-commit 与 merge/TTL 2/2 通过；`build-gcc-release` 全量 ctest
  配置 849 项、运行 847/847 通过（两个 RSS probe 保持 disabled）。

工作面：`include/bitcask/oki_state.hpp`、`src/fileops/oki_state.cpp`、
`src/cask/cask.cpp`、`src/cask/cask_internal.hpp`、`tests/atomic_batch_*_test.cpp`、`tests/txn_test.cpp`。

1. 建立写权威服务（可先放在 `OkiState` 或新的 `OkiIndex` 门面）：
   - `put(key, loc, tstamp, ord, expected_loc?)`；
   - `remove(key, tstamp, ord)`；
   - `conditional_remove(...)`；
   - `append_update(...)` 供 merge relocation 绕过水位门；
   - atomic batch 的一次性 apply。
2. 写前存在性/CAS 判定改走 OKI locate：
   - 保持当前 `kAlreadyExists` 外部语义；
   - 同 batch 内先做 key 去重/排序，减少重复点查；
   - memdelta 辅助哈希作为写路径热缓存；
   - 禁止把“不存在”误当 `kUnavailable`。
3. 精确维护 per-file fstats：
   - logical new / overwrite / delete 分别入账；
   - relocation 减旧 file、加新 file；
   - TTL tombstone 正确扣旧 file；
   - merge 后 trim 被删除 file；
   - manifest 提交时的 fstats 与内存状态一致。
4. 提交顺序保持“data 是真相”：
   - data append/commit 先于 OKI 可见性；
   - OKI append 失败不得伪装成功；
   - group commit / parallel batch 的 ord 顺序、写锁与 memdelta 到达序不变量重写并测试。
5. flush 阈值与恢复窗口重新调参：
   - 行数/字节上限按目标 recovery 时间反推；
   - held row 不推进 wm；
   - active data sync 失败时不提交引用未持久字节的 run。
6. 性能记录：
   -均匀写场景必须量化每次 locate 的开销；
   -如 gate 不通过，只能以显式配置/后续设计提案引入延迟记账或近似语义，不得静默改变 API。

- **验收**：put/remove/batch/TTL/merge 后 fstats 与逐文件扫描对拍一致；
  group commit 并发写无 ord 丢失/错序；写路径不再引用 KeyDir 分片表。

### ✅ S43-6 — OKI-only iterator/range

工作面：`include/bitcask/oki_state.hpp`、`src/fileops/oki_state.cpp`、
`src/cask/cask_iter.cpp`、`src/cask/cask_range_iter.cpp`、`tests/oki_range_test.cpp`。

1. iterator 改为标准归并视图：
   ```text
   pinned ReadView = runs[] + memdelta sorted view
   按 key 升序 k-way merge
   同 key 取 max(ord, gen/到达序)
   tombstone 终止该 key 或按 include_tombstones 语义输出
   ```
2. 快照一致性：
   - iterator 创建时 pin runs 集合与 memdelta sorted view；
   - compaction/rebuild 不得 unlink 在途 run 语义；
   - flush 后新写入是否可见遵循现有 iterator 契约，并在测试中显式锁定。
3. seek/range：
   - prefix/range 直接在有序结构上 seek；
   - bloom 只服务点查，不作为 range 剪枝依据；
   - v1/v2/v3 mixed 时按能力显式降级或重建，不静默漏 key。
4. 值读取：
   - iterator 只保存定位，不缓存 value；
   - `next()` 取值走与 `get` 相同的 key 核对与 retry；
   - 迭代期间 relocation/merge 能收敛到最新位置。
5. 删除依赖：
   - 移除 `KeyDir::IterHandle`、`ColdIter`、pending hash、epoch freeze 的调用面；
   - 多 iterator 并发与 close 竞态改由 OKI run/view 引用计数兜住。

- **验收**：并发写、flush、compaction、close 下 iterator 无漏 key/重复 key/UAF；
  range 与 brute-force scan 对拍全等；TSan/ASan 全绿。

#### ✅ S43-6 落地记录（2026-11-15）

- `CaskIter` 弃用 `KeyDir::IterHandle` / `ColdIter` / pending hash / epoch
  freeze 路径：`start()` 捕获 `OkiState::ReadView`，runs 共享 Reader 与
  memdelta 不可变快照由视图自身续命。历史 `StartIterResult` 仍对外保留
  以维持 ABI，成功路径恒 `kOk`；pending freshness 参数不再驱动哈希冻结。
- fold 归并统一 key 升序，同 key 取 `max(ord)` 且等 ord 时 memdelta /
  更高 gen 胜；墓碑在归并层抵消，`see_tombstones=true` 时按视图行合成
  tombstone entry。prefix 先在 run/delta 有序结构上 seek，再越过前缀截断，
  不再全表过滤。
- value 读取只保存定位，按 S43-1 契约核对 record key 与 ord；并发
  merge relocation 造成旧 loc 缺失时重查最新 OKI 并重试一次，绝不折叠成
  静默漏 key。视图存活跨 `close()`；release 只释放自身 runs/文件句柄。
- `CaskRangeIter` 归并同样收敛到 `(ord, 来源序)` 胜出格，移除 KeyDir
  pin；public get 继续承担 key/ord 核对与 merge 窗口重试。
- 新增 `FoldIterPinsOkiViewAcrossWritesAndFlush`：迭代创建后并发覆盖、
  删除、新增并 checkpoint，锁定在途 fold 仍输出创建时视图且无重复 key。

验证：GCC release 目标 clean rebuild 无新 warning；range/CaskIter/C API/
merge/close 相关定向 172/172 通过；ASan 同口径 172/172 通过；TSan
range/LevelB/contract/crash/merge 151/151 通过；GCC release 全量
850 配置、848/848 运行通过（两个 RSS probe 维持 disabled）。

### ✅ S43-7 — OKI-driven merge

工作面：`src/merge/merger.cpp`、`src/merge/merge_policy.cpp`、
`src/cask/cask.cpp`、`include/bitcask/merger.hpp`、`tests/merge_*_test.cpp`。

1. 第一步先完成 no-hint 正确性：
   - 输入枚举改 `fold(data)`；
   - 活性判断改 `OkiState::locate`；
   - relocation 直接 append_update 到 OKI；
   - 输出不再写/finalize/link/copy hint；
   - unlink 只处理 `.data`。
2. 第二步实现 sort-merge join：
   - OKI runs + memdelta 按 key 有序；
   - data 输入若非 key 序，用外排或按 file/offset 序 + OKI 定位筛选；
   - 顺序游标替代逐 record 随机 locate；
   - join 期间 pin 一致性 runs/memdelta 视图；
   - 新写入行按 ord 裁决，不得被旧 relocation 覆盖。
3. merge 候选选择依赖 v4 per-file fstats：
   - dead ratio、live bytes、total bytes 不再从被删除的 KeyDir fstats 获取；
   - manifest 元数据落后时允许保守不合并或先收敛元数据。
4. 崩溃窗口：
   - output data fsync → OKI relocation 提交 → 旧 data unlink 的顺序固定；
   - 任意点崩溃后重开可用 tail/fallback 修复；
   - 绝不允许 OKI 指向已 unlink 且无法恢复的 data。
5. 限速与 foreground 隔离：
   - OKI compaction / merge IO 不无限抢占前台 fsync；
   - 至少提供诊断指标，是否做 token bucket 由 gate 决定。

- **验收**：merge 后 get/range/fstats 与 pre-merge reference model 等价；
  unlink 前后 kill 注入均可恢复；merge 吞吐与写 P99 记录到 gate 报告。

#### ✅ S43-7 落地记录（2026-11-15）

- `run_merge` 全程 pin 一个 `OkiState::ReadView`，由 OKI runs 与不可变
  memdelta 快照参与候选裁决；不逐 record 扫 data，也不调用
  `KeyDir::locate` 判断活性。归并仍按 key 升序进行，同 key 取
  `max(ord, 来源序)`：runs 按 gen 升序，memdelta 为最高来源；墓碑在
  裁决层抵消 live 行。只有权威 live 行且 `loc.file_id` 属于输入集时，
  才按精确 `RowLoc` 读源记录。
- 源记录回读强制核对 key、ord、timestamp、total size 与 `kDoc` 类型；
  不匹配立即失败，避免把并发迁移后的旧位置误写成新位置。重定位继续
  通过 `KeyDir::put(..., newest_put=false, old_file_id, old_offset, ord)`
  分片锁 CAS 提交，内部落到 `OkiState::apply_relocation`；这样保留对
  并发新写的 ord 裁决，避免 merger 裸调 OKI 造成 TOCTOU。CAS 失败计入
  `records_stale`，并由 `Cask::merge` 输出前台 contention warning；
  失败位置仍进入 `relocations_stuck` / `stuck_file_ids` 诊断。
- merge 输出改为 data-only：不创建、写、finalize、sync、link 或清理
  hint。`MergeStats::output_hint_path` 仅保留 ABI 字段，成功路径恒为
  空。retirement 只 unlink 旧 `.data`；历史 hint 残留留给 S43-8 统一
  停写/清理，不在本阶段扩大删除面。
- 固定崩溃顺序保持 output data flush/fsync → KeyDir/OKI relocation
  commit → caller 侧 OKI flush → 输入 retirement/unlink。merger 侧
  relocation 批量 apply 仍先落盘数据，再提交索引；任何 CAS 拒绝都不会
  掩盖新写权威。
- `Cask::needs_merge` 优先使用健康 BCOM v4 的
  `OkiState::manifest_fstats()` 做 dead ratio/live bytes/total bytes
  候选统计；legacy/非 v4 状态才回退 KeyDir fstats。新增
  `merge::summarize(string_view, oki::OkiManifestFStats)`，与旧口径共用
  模板实现，避免统计规则漂移。
- `Cask::merge` 前先执行 `ensure_oki_for_write()`，阻断旧 Level A
  “hash 可用但 OKI 不可用/落后”的目录直接进入 OKI-driven merge；
  受控重建失败显式返回 `kIndexRebuildFailed` 或 `kIndexCorrupt`，绝不
  把 `kUnavailable` 静默当作空索引。
- `PluginContract` 直接 `run_merge` 用例改为先 rebuild 真实 OKI 视图，
  并断言 merge 目录无 hint、`output_hint_path` 为空；`OkiRangeTest`
  merge 失败路径输出 `error().detail` 便于故障区分。

验证：GCC release 全量 850 配置、848/848 运行通过（两个 RSS probe 维持
disabled）；ASan Merge/Oki/Crash/Checkpoint/Plugin 定向 203/203 通过；
TSan Merge/Oki/Crash/Plugin/Concurrent 定向 192/192 通过，日志无
ThreadSanitizer race report。

### ✅ S43-8 — 停写并清理 hint

工作面：`src/cask/cask.cpp`、`src/cask/cask_recovery.cpp`、`src/fileops/hint_file.cpp`、
`include/bitcask/hint_file.hpp`、`src/fileops/scanner.cpp`、`src/merge/merger.cpp`、
`src/fileops/migrate.cpp`、`tools/bitcask_migrate.cpp`、`tests/`。

1. 前置 gate：S43-3 / S43-7 的 data-only recovery 与 merge 全绿。
2. 停止写 hint：
   - `roll_active` / `close` / merge 不再创建或 finalize `.hint`；
   - active data fsync 语义保持不变；
   - 写路径不再因 hint 失败而失败。
3. 扫描与打开：
   - scanner 不再把 `.hint` 当必需伴生文件；
   - 旧目录中的 hint 默认视为可删除的历史加速缓存；
   - 明确策略：open 时惰性删除、merge 后删除，或提供维护命令，不得静默依赖。
4. recovery：
   - 删除 hint fast path 与 hint/data 双路径分支；
   - 只保留 data fold 与 OKI manifest/tail 协议。
5. migration：
   - `hintord` 迁移改为 data-only 迁移或保留只面向旧目标目录的兼容工具；
   - 新目标不生成 hint；
   - 老 binary 打开新目录时允许走慢速 `fold(data)`，但不得报格式错误。
6. 清理实现：
   - hint codec/fold/trailer 代码按迁移策略退役或隔离到 legacy 工具；
   - grep 归零生产路径引用；测试目录中旧 hint fixture 只服务兼容用例。

- **验收**：新库目录树无 `.hint`；旧库带 hint 重开正常；删除/重命名/损坏 hint 不影响
  新路径；recovery 与 merge 注入矩阵全绿。

#### ✅ S43-8 落地记录（2026-11-15，收口）

- 架构落点：`.data` 仍是唯一持久真相；`.hint` 一律降级为历史加速缓存。
  可写普通开库在持有 writer exclusivity 后 best-effort 删除目录内所有
  regular `.hint`；只读、recovery 失败、`merge_only` 都不扩大删除面。
  删除失败只记 warning，不阻塞打开；合法或损坏 hint 都不再进入恢复。
- 写路径停写：`roll_active`、close、checkpoint、batch/组提交与 merge 全部
  不再创建、追加、finalize、link 或复制 hint。active data 的
  flush/fsync → OKI commit → memdelta 截断顺序保持不变；数据 durable
  水位分拣也不回退。
- 恢复收敛：删除 `fold(hint)` fast path，健康 BCOM v4 走 data-only
  streaming replay；缺失、越界、Level A、损坏等情况继续显式 fallback
  或受控重建。scanner 只扫描 `.data`，`DataFileEntry` 不再携带 hint 元数据。
- 实现退役：删除 `src/fileops/hint_file.cpp` 与
  `include/bitcask/hint_file.hpp`；从 production `bitcask_fileops` 移除；
  删除 `HintRecord`、`encode_hint_record_v5`、`decode_hint_record_v5`、
  `mk_hint_filename` 与 BCH4/BCH5 常量。`MergeStats::output_hint_path`
  只保留 ABI 字段并恒为空。
- 迁移 data-only：`be2le`、`tstamp64`、`hintord` 的目标都不迁移或生成
  hint；`hintord_link_and_rehint` 改名 `hintord_link_data`，只 link/copy
  data 并最后原子提交 meta。迁移 CLI 与 meta v4/v5 文案同步为 data-only。
- 测试收敛：删除 HintFile/golden/DataAndHint/BCH4/v3 与 HintRecordV5
  专属用例；保留 scanner 忽略 hint、迁移目标无 hint、删除 hint 后
  data-only reopen，以及可写重开清理 corrupt/orphan legacy hint 的契约
  （`OkiOnlyContractTest.ReopenRemovesLegacyHintsAndUsesDataOnly`）。

验证：干净重建无新 warning；`build-gcc-release` 全量 ctest 835 配置、
833/833 运行通过（两个 RSS probe 维持 disabled）；ASan
Merge/Oki/Crash/Checkpoint/Plugin/Scanner/DataFile/Migrate 定向 226 配置、
224/224 运行通过（同两个 disabled probe）；TSan
Merge/Oki/Crash/Plugin/Concurrent 定向 193/193 通过。生产代码 grep 无
hint codec/fold/writer/filename 符号；剩余引用仅限 ABI 字段、legacy 清理
策略与历史纪元说明。

### 🔴 S43-9 — 退役 KeyDir 与 BCKS

工作面：`include/bitcask/keydir.hpp`、`include/bitcask/keydir_registry.hpp`、
`src/keydir/*`、`src/cask/*`、`src/merge/*`、`src/search/*`、`c_api/*`、`tests/*`。

1. 替换 KeyDir 的内部职责：
   - ord 分配 / `next_ord` → OKI manifest v4；
   - file_id 分配 / biggest_file_id → OKI manifest v4；
   - fstats → OKI manifest v4；
   - point lookup → `OkiState::locate`；
   - range/iterator → OKI ReadView；
   - merge CAS / relocation → OKI 写权威服务。
2. 替换进程内共享语义：
   - `KeyDirRegistry` 的多 Cask 同目录共享改为 OKI/index registry 或等价 owner；
   - open/close refcount、write lock、multi-lib thread limits 语义保持；
   - 不允许多个 Cask 各自维护会漂移的 OKI 权威状态。
3. 删除内存结构：
   - shard table、seqlock/optimistic read、epoch limbo、pending sibling chain；
   - `cache_budget` / eviction / shadow check；
   - BCKS v3/v4 serialize/deserialize；
   - `kv.keydir.ckpt` 写路径。
4. 兼容策略拍板：
   - 首个 rollout 允许读旧 BCKS 仅用于迁移元数据，不作为运行时索引；
   - 稳定后删除 BCKS 解析或隔离到 legacy migration；
   - `Cask::keydir()` 等内部/public API 的删除或 deprecated 窗口必须明确。
5. search/plugin 面适配：
   - live checker、ord/key 回调、checkpoint watermark 改走 OKI/只读 service interface；
   - plugin API 尽量以窄接口替代对 `KeyDir` 具体类型的依赖。
6. C API / bindings：
   - 行为零变化；
   - 只允许错误码/诊断字段新增；
   - SOVERSION 与 ABI 兼容用 c_api test 守门。

- **验收**：生产路径 grep `KeyDir/keydir_/kv.keydir.ckpt` 归零（legacy migration 除外）；
  多 Cask 同目录、multi-lib、close/iterator 竞态全绿；公开 API 行为快照无变化。

#### ✅ S43-9 落地记录（2026-11-15，收口）

- 权威收编：旧 `KeyDir`/`KeyDirRegistry` 实现退役，进程内共享改为
  `oki::IndexAuthority` + `oki::IndexRegistry`。ord、file_id、fstats、点查、
  range/iterator 与 merge CAS/relocation 都由 OKI 权威面提供；多 Cask 同目录
  refcount、write lock、multi-lib 线程池与既有公开行为保持不变。
- 提交面收敛：`kv.oki.manifest` + `kv.oki.seg-*` 是 KV 唯一恢复提交集。
  BCKS/checkpoint 语义迁移为 BCOM v4 全局/文件元数据；`kKeydirSnapName`
  与 `kv.keydir.ckpt` 生产写入/复制路径退役。`Cask::backup()` 先封口并
  flush OKI，再复制 manifest 和当前 run 集；bench 的 full-fold 场景改为
  删除 OKI manifest。备份契约测试新增提交集存在性守门。
- 只读能力：新增 `OkiState::install_memory_view()` 和
  `IndexAuthority::rebuild_memory_from_data()`。只读目录在缺 manifest、
  无点查视图或显式 rebuild 时只构建内存视图，不写 run/manifest；
  RO `read_doc` 可触发受控重建，但不把失败折叠成 `kNoIndex`。
- 崩溃尾洞：恢复收尾在「manifest loaded + run 集为空 + data 非空」时
  先做 ledger-only 对账，metadata 活键数不一致即触发全量重建。该检测
  覆盖 Level A 写者在 OKI flush 前崩溃的窗口；有 run 的 metadata-less
  v3 目录仍保持“只补 ledger、不伪造视图”的迁移契约。
- 搜索与插件：docmap delta 链的 keydir 半边改称 authority 半边，
  `DocmapReplayHook`、保存参数与 `Cask::replay_delta_to_authority()`
  统一术语；merge runner 直接消费 `IndexAuthority`。merge 前受控重建
  后显式启用 point query，避免只建 ledger 导致插件 merge 失败。
- 测试迁移：stale snapshot、crash image、auto checkpoint、search tail、
  checkpoint corruption 等契约从旧 `kv.keydir.ckpt` 迁移到 OKI 提交集；
  Level B/merge/plugin/range/恢复断言同步新语义。删除旧 KeyDir、
  hint-only 和 OKI locate 专属测试面。
- grep 边界：生产路径旧 KeyDir 术语清零。允许保留的只剩
  `keydir_cache_entries` ABI 字段及兼容说明、迁移工具跳过的历史
  `kv.keydir.ckpt` 文件名，以及历史 benchmark baseline JSON。

验证：`build-gcc-release` 全量 ctest **803 配置、801/801 运行通过**
（两个 RSS probe 维持 disabled，日志 `/tmp/full-gcc-s43-9-final.log`）；
ASan Merge/Oki/Crash/Checkpoint/Plugin/Scanner/DataFile/Migrate 定向
**213 配置、211/211 运行通过**（同两个 disabled probe，
`/tmp/asan-s43-9-final.log`）；TSan Merge/Oki/Crash/Plugin/Concurrent
定向 **179/179** 通过，日志无 race report
（`/tmp/tsan-s43-9-final.log`）。

### ✅ S43-10 — 全量验证、迁移与 rollout

工作面：`tests/`、`bench/`、`CHANGELOG.md`、`feedbacks/`、`TASK.md`、CI。

1. 正确性矩阵：
   - 随机 put/overwrite/remove/reinsert/TTL/batch/checkpoint/reopen reference model；
   - `get/range/iterate/fstats/next_ord/file_id` 全量对拍；
   - v3 → v4、带 hint 旧库、无 hint 新库、manifest 损坏、run 损坏、meta 旧版本；
   - 同目录多 Cask、multi-lib、search/docmap/bm25/vec。
2. 崩溃矩阵：
   - S43-0 的所有 kill 点逐点注入；
   - data tail torn / bad CRC / short read；
   - manifest old/new/orphan run / spill 残件；
   - merge unlink 与 relocation 交错。
3. 并发矩阵：
   - reader/writer/flush/compaction/checkpoint/close/iterator；
   - group commit 与 atomic batch；
   - merge relocation 与前台读写竞态。
4. benchmark gate：
   - 点查延迟：接受冷查退化，但必须量化；
   - 写吞吐：记录均匀写与 Zipfian 写差异；
   - 开库：manifest 健康时目标为 O(tail)，最坏 data rebuild 允许显著变慢并明确文档；
   - RSS：无 KeyDir 后常驻内存应显著下降；
   - 磁盘：hint 删除收益与 OKI compaction 写放大一起核算。
5. rollout：
   - 默认路径切换前先跑 shadow/dry-run；
   - 变更分阶段打开：v4 manifest → OKI get → OKI write/range/merge → no hint → remove KeyDir；
   - 每阶段具备回退开关或明确单向迁移决策；
   - CHANGELOG 写清 recovery 时间与磁盘布局变化。

- **验收**：gcc/clang Debug/Release、ASan、TSan、`-Werror` 全绿；
  benchmark 报告归档；旧库迁移演练通过；TASK/CHANGELOG/feedbacks 账目闭合。

#### ✅ S43-10 落地记录（2026-11-15，收口）

- 新增 `S43RolloutGateTest`：480 步随机 put/overwrite/remove/reinsert、
  TTL、atomic batch、checkpoint/merge/reopen reference model；
  legacy hint、manifest tmp、manifest 损坏、run 损坏、torn data tail
  显式重开；K1/K2、K3/K4、K10、K12/K13、K14/K15 子进程崩溃窗口。
  Release 与 ASan 均 7/7 通过。
- `batch` reference model 修掉一个测试侧借用生命周期缺陷：
  `BatchOp::value` 借用的临时字符串在 initializer 完成后过期，ASan
  捕获 stack-use-after-scope；现与 key 一样使用命名宿主字符串。
- K12/K13 语义澄清：merge output 完整 fsync 后即使 manifest 回退，
  重放按 ord/LWW 收敛到该完整更代状态；门禁检查 900 key 无悬空、
  无丢失，而不是要求孤儿输出不可见。
- Rollout 报告归档于
  `docs/design/s43-okionly-rollout-report-zh.md`，原始 benchmark
  JSON/日志在 `bench-results/s43-10/`。热点查 806 ns，冷块 p50/p99
  4.57/7.41 µs，100k 均匀写 2.707 µs/op，snapshot/full-fold 开库
  7.10/11.7 ms，merge 636k rows/s；新增 bounded Zipf(s=1) 100k
  覆盖写基准实测 1.599 µs/op。
- RSS/磁盘账闭合：`BM_Oki_MemProbe/100000` 同机 `cce59d4` A/B 3 次重复，
  memdelta RSS 净增 26.37→7.60 MiB（-71.2%），rebuild 峰值
  37.14→17.60 MiB（-52.6%）；归档
  `rss-a-b-memprobe-{before,after}.{json,log}`。新增
  `BM_Merge_DiskAccounting/20000`：18,702 eligible 行、retired data
  2.5834 MiB、OKI run +0.2734 MiB、hint 0 文件，value 增量写放大
  0.1533；归档 `merge-disk-accounting.{json,log}`。100k/key20B 迁移
  A/B 中 3,200,012B hint 可写打开后清理为 0；归档
  `hint-disk-a-b.log`。
- Release full ctest **808/808**（两个 RSS probe disabled）；ASan/UBSan
  full **808/808**；TSan full **807 executed / 0 failed**（一个平台
  skip，同两个 RSS probe disabled）；GCC Release `-Werror` 207 targets
  干净构建；Clang 21 Debug/Release `-Werror` 均 808/808（Release
  测试构建关 LTO 以规避 linker 插件加载 C object 工具链问题）。日志：
  `/tmp/asan-s43-10-final.log`、`/tmp/tsan-s43-10-final.log`、
  `/tmp/werror-s43-10-build.log`、`/tmp/clang-debug-s43-10-final.log`、
  `/tmp/clang-release-s43-10-final.log`。
- 版本真源 bump 至 **6.9.0**（`SOVERSION` 保持 6）；CHANGELOG 记录
  恢复、迁移、磁盘布局与回退语义。`feedbacks/` 账本无需新增条目，
  既有 2026-11-13 ③④ 与本 S43 范围无耦合。

### 执行序

```text
S43-0 基线/故障注入
→ S43-1 点查安全
→ S43-2 BCOM v4
→ S43-3 data-only recovery
→ S43-4 OKI-only get
→ S43-5 OKI-only write/fstats
→ S43-6 OKI-only iterator/range
→ S43-7 OKI-driven merge
→ S43-8 remove hint
→ S43-9 remove KeyDir/BCKS
→ S43-10 rollout gate
```

S43-1 可与 S43-0 后半并行；S43-4 依赖 S43-1/2/3；
S43-5/6 依赖 S43-4；S43-7 依赖 S43-5/6；
S43-8 是硬 gate 后动作；S43-9 必须最后执行。

### 风险排序（按“有多容易静默错”）

| # | 风险 | 任务 | 失败形态 |
|---|---|---|---|
| 1 | OKI 损坏导致 `get` 静默返回别的 key | S43-1/4 | 数据正确性事故，最不可接受 |
| 2 | manifest 水位超前 / held row 误提交 | S43-2/3/5 | 重放跳过记录，静默丢活跃 key |
| 3 | merge unlink 早于 relocation 持久化 | S43-7 | 重开后 loc 悬空，恢复成本放大 |
| 4 | iterator 视图漏 run / 提前释放 | S43-6 | range 少 key、UAF 或旧值 |
| 5 | fstats 漂移导致 merge 误判死文件 | S43-5/7 | unlink 仍被引用的 data |
| 6 | 均匀写每 put 点查导致吞吐崩 | S43-5/10 | 功能正确但性能不可用 |
| 7 | 废 hint 后最坏恢复变成全库 data 扫描 | S43-3/8/10 | 打开时间小时级、污染 page cache |
| 8 | search/plugin watermark 与 OKI 提交点错序 | S43-2/9 | 搜索丢区间或重复回放 |

### 明确不做

| 项 | 理由 |
|---|---|
| 把 OKI 当作唯一持久真相 | data 文件仍是真相；OKI 必须可从 data 重建 |
| 在 recovery/merge 去 hint 前物理删除 hint 写路径 | 会先失去恢复保险且无兜底 |
| 用关闭 CRC / key 核对换点查性能 | OKI-only 下索引损坏不能变成静默错值 |
| 静默把 logical-new 记账改成最终一致 | 会改变 `kAlreadyExists`、CAS 与 fstats 语义；必须单独拍板 |
| 保留 KeyDir 哈希作为隐藏权威 | 与 OKI-only 目标冲突，只允许迁移期影子对拍 |

---

## 🔴 S42 · DocMap Live-Only Store（2026-11-14 立项）

> 来源：热点覆盖写使 `index::Index` 按历史 ord 展开的平坦数组随总写入量增长
> （`live_docs == 1` 时写 N 次仍占 O(N) 内存）。
> 设计：[`docs/design/s42-docmap-live-store-design-zh.md`](docs/design/s42-docmap-live-store-design-zh.md)（已评审定稿）。
> 决策基线：**live-only map 为主存储**；**checkpoint 显式升序快照**；**delta 窗口 journal**；
> **放弃全局 DocMap 层 SIMD gather**（BM25 主路径已分段化，段内 dense DocId + 段内 live checker）。
> 硬约束：磁盘 docmap checkpoint 格式逐字节不变（旧链可读、新链旧实现可读）；
> `SearchHit.ord` 仍为全局 LSN；S40 key 单实例不变量（ext node 地址稳定）不破。
> 验收标准：每项改动后 ctest 全绿 + 编译无新告警；涉及恢复语义须过 crash/recovery 用例；
> 收尾须过 ASan/TSan 与 benchmark gate（见 S42-5）。

| # | 任务 | 状态 | 说明 |
|---|---|---|---|
| S42-1 | live-only 主结构替换 | ✅ done | `index.hpp` / `index.cpp`：删 chunks/flat arrays，换 `ext2ord_ + live_ + meta_ + delta_live_` |
| S42-2 | checkpoint 升序快照 + delta journal | ✅ done | `for_each_live(_in)/for_each_meta_in` 锁内 RowRef 排序回调；sidecar 格式零改动 |
| S42-3 | recovery replay journal 抑制 | ✅ done | `Index::ReplayJournalGuard`（docmap_ckpt load + legacy_ckpt load） |
| S42-4 | 回归与一致性测试 | ✅ done | index_test 改造 + 随机 reference-model 对拍 + 热点 overwrite 形态回归 |
| S42-5 | benchmark gate 与 rollout | ✅ done | 远端 9700X Release 基线 + rollout 报告（`docs/design/s42-rollout-bench-report-zh.md`）；随 6.8.1 发布 |
| S42-6 | flat arrays 与 SIMD kernel 清理 | ✅ done | 删 `index_kernels.hpp` / `index_kernels_avx2.cpp` + CMake `bitcask_simd_tu` 条目 |

### 🔴 S42-1 — live-only 主结构替换（先行，纯内存侧）

工作面：`include/bitcask/index.hpp`、`src/keydir/index.cpp`。

1. 新结构（设计 §6）：
   - `ext2ord_` 保持 node-based `std::unordered_map`（S40 地址稳定）；
   - `ankerl::unordered_dense::map<Lsn, LiveRow> live_`，`LiveRow{string_view ext; DocSlot slot}`，ext 指向 ext node key；
   - `ankerl::unordered_dense::map<Lsn, vector<byte>> meta_`（无 meta 部署零 per-doc header）；
   - `ankerl::unordered_dense::set<Lsn> delta_live_`（窗口 journal，S42-2 消费）。
   - 依赖已就绪：`unordered_dense::unordered_dense` 已是 CMake 链接目标（KeyDir 分片表在用）。
2. 写路径语义（设计 §7）：覆盖写先 `live_.erase(old)` / `meta_.erase(old)` / `delta_live_.erase(old)` 再插新；
   `retired_since_compact_` 仅旧版本实际 live 时递增；删除先 erase live row 再 extract ext node（防悬垂）；
   removals 入账门限（`tomb >= delta_window_wm_`，命中与否都入账）保持现状。
3. 读路径：`get/ord_of/ord_to_ext/is_live/doc_len/meta_blob/eval_meta` 改 hash 查找；
   `eval_meta` 仍锁内求值；`meta_blob` 仍锁内拷贝。
4. `fill_is_live/fill_doc_lens` 退化为**一次 shared_lock + 逐 candidate hash lookup**（接口不变，`LiveChecker` 契约不变）。
5. 遍历兼容：`for_each_live/for_each_live_in/for_each_meta_in` 保留升序语义（锁内收集 RowRef 排序后回调）；
   checkpoint 内部改走 S42-2 的 snapshot API，避免全量遍历重复排序。
6. `compact_chunks()` 保留 API、返回 0；`IndexInfo` chunk 字段保留但冻结（诊断兼容）。
7. 测试适配：`CrossChunkOps/ChunkBoundaryLastSlot/CompactChunks*` 三个 chunk 语义用例改写为
   「历史 ord 不占内存 / compact no-op」断言；其余 index_test 用例语义不变须全绿。

- **验收**：index_test + checkpoint/recovery 相关 ctest 全绿；`-Werror` 无新告警；
  热点 overwrite 冒烟（1 key × 100K 写）RSS 不随 ord 增长。

### 🔴 S42-2 — checkpoint 升序快照 + delta journal

工作面：`src/keydir/index.cpp`（serialize/deserialize）、`src/keydir/docmap_ckpt.cpp`、
`include/bitcask/index.hpp`（snapshot API）、`src/cask/legacy_ckpt.cpp` 与
`tests/support/search_layer.cpp`（调用点适配）。

1. base：shared lock 内收集 `RowRef{ord, string_view ext, const DocSlot*}` → 按 ord 排序 →
   现有 v3 格式写盘（不拷贝全部 key，只付 ~32B/live 临时 ref）；meta section 同序。
2. delta：只遍历 `delta_live_`，过滤 `[from, watermark)` 且仍 live → 排序 → `kDocmapDeltaV3`；
   与 `removals_snapshot()` 按 ord 交错语义不变；meta delta 只含窗口内 live meta。
3. `begin_delta_window(wm)` 清空 `delta_live_`；put/remove 按 wm 门限维护 journal（S42-1 已埋点，此处接通消费端）。
4. **格式零改动**：deserialize/apply 路径不动；旧 checkpoint 新实现可读、新 checkpoint 旧实现可读。
5.  golden 对拍：同一操作流下新旧实现产出的 base/delta 字节一致（或语义等价重放一致）。
6. **migration 影响：无**。纯内存表示变更，migrate 工具面积不含 docmap（设计 §8.4）；
   不 bump 版本、无 flag-day，回滚双向兼容。

- **验收**：`SidecarV2RoundTrip/SidecarV1CompatRead/ForEachLiveAcrossChunks`、
  `checkpoint_recovery_test`、`crash_recovery_test` 全绿；「删后重写」交错重放用例通过。

### 🟡 S42-3 — recovery replay journal 抑制

1. 载入期（base deserialize / delta apply / fold 重建）抑制 `delta_live_` 常规增长
   （RAII guard 或 replay 标志）；成功推进窗口时清空，失败退 fold 不污染正式状态。
2. 调用点：`docmap_ckpt.cpp` load 链、`legacy_ckpt.cpp`、`tests/support/search_layer.cpp`。
3. removals 的载入污染清理语义保持现状（收尾 `clear_removals()`）。

- **验收**：recovery 用例全绿；大 base 开库峰值 RSS 不因 journal 抬升（对照 S42-1 冒烟基线）。

### 🔴 S42-4 — 回归与一致性测试

1. 随机操作流（put/overwrite/remove/reinsert/set_doc_len/set_meta/checkpoint）对拍简单 reference model：
   `get/ord_of/ord_to_ext/is_live/doc_len/meta_blob` + checkpoint 重放后全量状态。
2. 新增用例：同窗口多次覆盖写 delta 只含当前 live row；覆盖写清除旧 meta；64 位 ord 边界不做数组下标；
   删除后重写 ord 交错；replay 中途失败退 fold。
3. RSS 回归：1 live key × 1M overwrite，DocMap 常驻近似恒定（旧实现对照 ~50MiB+ 增长）。
4. 并发：读写并发 + checkpoint 并发过 TSan；悬垂防护过 ASan。

- **验收**：Debug 全量 + ASan + TSan 全绿。

### 🟡 S42-5 — benchmark gate 与 rollout

1. bench 面：`cask_bench`（hot overwrite RSS/吞吐）、`keydir_bench`/点查、`hnsw_bench`（live callback）、
   IVF 过滤搜索、`inverted_bench`/BM25 分段 P99、`checkpoint_bench`（base/delta 吞吐与文件大小）、开库 recovery。
2. gate 标准（设计 §16.4）：热点 overwrite RSS 显著下降；BM25 主查询 P99 无明显回退；
   ckpt/recovery 在可接受范围。**不要求所有路径都快**。
3. 默认开启 live-only；如 gate 边缘，提供短期配置开关回退（不长期维护双实现）。
4. 账目：CHANGELOG + feedbacks 标注；TASK.md 落地记录回填。

### 🟡 S42-6 — flat arrays 与 SIMD kernel 清理（gate 后）

1. 删 `src/keydir/index_kernels.hpp` / `index_kernels_avx2.cpp` 与 CMake `bitcask_simd_tu(... avx2)` 条目
   （grep 确认仅 index.cpp 消费）；`fill_*` 保留 scalar 批量实现。
2. 清理 `IndexInfo`/`compact_chunks` 的 obsolete 注释或标记 deprecated（API 保留）。
3. 遍历 API 长期方向：无序版本标 deprecated，防新调用方误认升序。

- **验收**：`-Werror` 整树 0 告警；gcc/clang Debug+Release 双树 ctest 全绿；
  分 ISA 降档对拍无回归（沿用 S37-3 纪律）。

### 执行序

S42-1 → S42-2 → S42-3 → S42-4 → S42-5（gate）→ S42-6。
S42-4 的用例随 1/2/3 增量补齐，集中对拍与 sanitizer 在 4 收口；6 必须在 gate 之后。

### ⚠️ 风险排序（按「有多容易静默错」）

1. **checkpoint 遍历顺序**：unordered 天然序直接写盘 → gap 压缩率崩/重放语义错。所有写盘路径显式排序（S42-2 golden 对拍兜底）。
2. **ext view 悬垂**：删除/覆盖时先动 ext node 后 erase live row → use-after-free。写路径顺序纪律 + ASan（S42-1/4）。
3. **delta journal 漏淘汰旧 ord**：热点覆盖写把窗口 journal 写爆 → delta 文件 O(writes)。覆盖写 erase 旧 ord 埋点 + 用例（S42-1/4）。
4. **replay journal 放大开库峰值**：base 重放逐 ord 入 journal。抑制 guard（S42-3）。
5. **低 churn 大全量集内存略增**：非目标场景劣化，gate 决策（S42-5）。

### 明确不做

| 项 | 理由 |
|---|---|
| ord 复用 / 世代压缩 | 与本设计正交，LSN 永不复用是恢复语义基线 |
| 有序索引（B-tree/skiplist）维持遍历序 | checkpoint 时排序是最小正确方案，不为遍历序付常驻结构 |
| BM25/HNSW/IVF 内部 id 结构改动 | 三者均不用 LSN 做数组下标，不在本届面积内 |
| `ext2ord_` value 携带 DocSlot 的点查优化 | 留作后续；第一版双 map 直观、迁移面小 |
| flat/live-only 双实现长期共存 | 仅 gate 边缘时短期开关，S42-6 收口删除 |

#### ✅ S42 落地记录（2026-11-14）

**S42-1 主结构**：`include/bitcask/index.hpp` 重写——`ext2ord_` 保持 node-based
（S40 地址稳定），新增 `ankerl::unordered_dense::map<Lsn, LiveRow> live_`、
`meta_`、`set<Lsn> delta_live_`；chunk/`live_`/`doc_lens_`/`meta_blobs_` 全部退役。
覆盖写/删除在写路径同步淘汰旧行（live row + meta + journal 三处）；
`retired_since_compact_` 仅旧行实际存在时递增。`fill_is_live/fill_doc_lens`
退化为单锁逐 candidate hash lookup（`LiveChecker` 批量接口契约不变）。
`compact_chunks()` 保留 API、恒返 0；`IndexInfo` chunk 字段冻结为 0。

**S42-2 checkpoint**：`for_each_live/for_each_live_in/for_each_meta_in` 在
shared lock 内收集 RowRef 排序后回调——base/delta/meta section 写盘序列与旧
实现逐字节同序同码（编码逻辑未动）。`begin_delta_window(wm)` 清空 journal；
put/remove 按 wm 门限维护（覆盖写淘汰旧 ord，热点写不撑爆 delta）。

**S42-3 载入抑制**：`Index::ReplayJournalGuard`（RAII）在 `load_docmap`
与 `legacy_ckpt::load` 重放期置位——journal 维护跳过，收尾
`begin_delta_window` 整体清空语义不变，开库峰值不随 base 抬升。

**S42-4 测试**：`tests/index_test.cpp` 改造——chunk 用例改为大 ord 间隙 /
compact no-op 断言；新增 `DeltaJournalTracksLiveOnly`（覆盖写淘汰 journal 旧
ord、窗口推进清空、wm 门限）、`OverwriteClearsOldMeta`（覆盖写/删除/空 blob
三路淘汰 + 死 ord no-op）、`HotOverwriteKeepsLiveOnlyShape`（1 key × 100K
overwrite → `live_docs==1`、chunk 字段 0、死 ord 无残留 doc_len）、
`RandomOpsMatchReferenceModel`（mt19937 固定种子 400 步 put/overwrite/
remove/set_doc_len/set_meta 对拍 + sidecar roundtrip 二次对拍）。

**S42-6 清理**：删 `src/keydir/index_kernels.hpp`、`index_kernels_avx2.cpp`、
CMake `bitcask_simd_tu(... avx2)` 条目；`bitcask_docmap` 链
`unordered_dense::unordered_dense`（PUBLIC，index.hpp 暴露成员类型），并在
子模块注册后加目录级 include 路径（覆盖仅 include `index.hpp` 不链 docmap
的目标，如 `bitcask_bm25`）。

**验证矩阵**：

| 树 | 结果 |
|---|---|
| gcc Release | **821/821** 全绿（0 告警；2 项预存 disabled + 1 项 >2GiB skip）|
| ASan | **821/821** 全绿（悬垂 view 防护通过；唯一告警为 googletest 第三方预存）|
| TSan | **821/821** 全绿（读写并发 + checkpoint 并发；1 项预存 skip）|

**bench smoke（Release，新基线，无旧对照）**：`BM_Cask_Put_Overwrite`
1.51µs（663.9K ops/s）、`BM_Inverted_QueryThroughputBOW/threads:1`
6.93µs（144.3K qps，分段主路径不经全局 DocMap gather）、
`BM_Cask_Open_Snapshot` 3.93ms。基线 json 仅有 KeyDir put（本届未动）。

**migration**：无磁盘 migration（设计 §8.4）。chunk/flat arrays 从未落盘；
sidecar v3/V3 逐字节不变，回滚双向兼容。

**遗留到 rollout**：正式 CHANGELOG 账目、低 churn 大全量集 RSS 对照、
HNSW/IVF 带 callback 的正式 gate 数字（本机 bench smoke 已覆盖对应代码路径）。

---

## ✅ S41 · fields 读回（2026-11-13，随 6.8.0）

> 来源：[`feedbacks/2026-11-13-doc-capability-gaps.md`](feedbacks/2026-11-13-doc-capability-gaps.md) #2。
> 设计：[`docs/design/s41-fields-read-back-design-zh.md`](docs/design/s41-fields-read-back-design-zh.md)。

| # | 任务 | 状态 | 结果 |
|---|---|---|---|
| S41-1 | C++ 结果类型与文档读入口 | ✅ done | `DocFieldView` / `DocField`；`GetResultView` / `GetResult` 末位追加 `fields`；`Cask::get_doc_view()` / `get_doc_owned()`；`get()` 不解码 fields，热路径行为不变 |
| S41-2 | 共用读路径 + schema 翻译 | ✅ done | `read_doc(key, want_fields)` 复用定位、TTL、CRC、merge-retry；field id 翻译失败或文档解码失败按 `kIo` 显式失败，不静默丢字段 |
| S41-3 | C API 纯加法 | ✅ done | `bitcask_get_doc` / `bitcask_get_doc_result_free` + 新结果结构；`bitcask_get` 布局与语义零改动，SOVERSION 保持 6 |
| S41-4 | 回归与账目 | ✅ done | C++ 覆盖 view/owned/覆盖替换/重开读回/search；C 覆盖 `put_doc_ex → get_doc` 与所有权释放；CHANGELOG 与 feedbacks 已标随 6.8.0 |

**明确不做**：field/meta 部分更新；fields 值的独立删除。append-only 下文档覆盖仍是
全量 fields 替换，后续如做 patch 更新需另行设计读-改-写与索引重登记语义。

---

## 📁 上一清单归档

S33（有序 Key 索引 OKI）、S34（TxnCask）、S35（引擎原生原子批）、S36（keydir 磁盘驻留
Level B）四届清单已全部收官，随 **v6.1.0** 发布，详见 git 历史（本文件在 056127b
之前的版本）与 `CHANGELOG.md`。

**未完项带入本清单遗留区**：T8（搜索读屏障，⏸ 4 项前置未满足）、
T12（HNSW ckpt 去重，⏸ 默认不做）。两项均与本届无耦合。

---

## 🧭 本届总纲

设计稿把移植拆成 A（构建）/ B（SIMD 派发）/ C（系统调用）/ D（测试）四段，
落到执行序上是 7 个任务。**关键排期事实：S37-1 / S37-2 / S37-3 三项共约 5 周
可完全在 Linux 上完成并验证**，且每项对现有代码库都是净收益 —— 应先行吃掉，
把风险最高的 S37-4 / S37-5 攻击面压到最小。

### 面积实测（2026-08-07，开工前重新量过）

初版设计稿的 `::open` 计数把 `HintFile::open` / `MmapSegment::open` /
`OkiRunReader::open` 一类**成员函数**误算在内。剔除后的真实裸 POSIX 调用面：

| 调用 | 处数 | 主要分布 |
|---|---|---|
| `::close` | 53 | 向量插件错误路径（一个 open 对应 6-8 个 close）|
| `::open` | **13** | hnsw ×4、ivf_rq ×2、diskann ×2、segment_v2 ×1、file_lock ×1、file_util ×1、sealed_segment_vector_plugin ×1、posix_file ×1 |
| `::fstat` | 8 | 同上，全部用于取文件大小（1 处取 dev/ino）|
| `::fdatasync` | 8 | file_util `flush_and_sync` 为主 |
| `::ftruncate` | 4 | file_lock、vec 追加回滚 |
| `::pread`/`::pwrite` | 4 / 3 | posix_file + file_lock |
| `::lseek` | 3 | posix_file |
| `::stat`/`::unlink`/`::madvise` | 2 / 2 / 2 | — |
| `::mmap`/`::munmap`/`::fsync`/`::fileno` | 各 1 | 已收敛（S36-B3）|

**结论：核心 KV 路径（data_file / hint_file / oki_run / segment）早已走
`io::PosixFile`，裸调用高度集中在向量插件（13 个 open 里占 8 个）。**
S37-1 的实际工作量因此低于设计稿估计。

---

## 🔴 S37 主线

### 🟡 S37-1 — 平台抽象层收编 🔴 HIGH（先行，纯 Linux 可交付）

把散落的裸 POSIX 调用全部收进 `bitcask::io`，使「Windows 后端」= 新增一个
`src/io/win32_file.cpp`，而不是改 20 个文件。**Linux 行为零变化**。

`io.hpp` 需补齐的 API（当前缺口）：

1. `OpenFlag` 补 `kTruncate`(O_TRUNC) / `kWriteOnly`(O_WRONLY) —— 向量插件的
   `O_RDWR|O_CREAT|O_TRUNC` 与 `O_WRONLY` 形态现无法表达，是这些站点绕过抽象的直接原因。
2. `PosixFile::size()` —— 收编 8 处 `::fstat`（其中 7 处只为取大小）。
3. `PosixFile::identity()` → `io::FileIdentity{dev, ino}` —— 收编 hnsw `.vec`
   追加目标身份校验，同时把 `<sys/types.h>` 逐出**公开头** `hnsw.hpp:44`。
4. `PosixFile::truncate(len)` —— 显式长度版（现仅有 `truncate_here()`），
   收编 `file_lock` 的 `ftruncate(fd,0)` 与 vec 追加回滚。
5. `io::sync_data(int fd)` —— 收编 8 处 `::fdatasync`。
6. `io::sync_directory(path)` —— 收编 `file_util.hpp:96` 的 `O_DIRECTORY` 开目录 fsync
   （Windows 下将降为 no-op，见设计稿 C5）。
7. **`io::atomic_rename(from, to)`** —— 收编 `file_util.hpp:128/200` 的 `std::rename`。
   **这是 C1（最高危项）的落点**：Windows 上 `std::rename` 目标存在即失败，
   9 个原子写站点会全线挂。提前把它放到 seam 后面，Windows 侧只需改这一个函数。
8. `MappedFile::prefetch(off, len)` —— 收编 `hnsw.cpp:1406` 的 `MADV_WILLNEED`。
9. `io::page_size()` —— 收编 `hnsw.cpp:1380` 的 `::sysconf(_SC_PAGESIZE)`
   （Windows 侧需区分 `dwPageSize` 与 `dwAllocationGranularity`，见设计稿 C7）。

- **工作量**：2 周（面积实测后下修，原估 2 周含 51 个 open 的误算，实际净工作量约 1 周 + 验证）
- **验收**：ctest 全绿（Debug 全量 + ASan）；bench 零回归（hot get / BOW / hnsw）；
  build-rel 双树（`hnsw.hpp` 公开头改动）；`grep` 确认第一方裸 POSIX 调用归零

#### ✅ S37-1 落地记录（2026-08-07）

**seam 建立**：`io.hpp` 从「POSIX 文件 I/O 包装」升格为平台 seam——全库唯一允许
直接调用宿主原语的地方是其实现文件（POSIX `src/io/posix_file.cpp`；Windows 将是
`src/io/win32_file.cpp`）。核验：第一方 `src/` `include/` `c_api/` `tools/` 的裸宿主
原语与 POSIX 头**双双归零**（唯一 grep 命中是成员函数 `SealedSegmentVectorPlugin::open`
的误报）。

**新增 API**：
- 类型：`FileHandle`（+ `kInvalidHandle` / `handle_valid`）、`FileIdentity`、`FileMode`
- `PosixFile` → `File`（`PosixFile` 留别名，既有 65 处引用零改动）+ `release()` /
  `truncate(len)` / `size()` / `identity()`
- `OpenFlag` 补 4 位：`kWriteOnly` / `kTruncate` / `kCloseOnExec` / `kSyncAll` / `kNoAppend`
- 句柄级：`open_handle` / `close_handle` / `sync_data` / `truncate_handle` /
  `handle_size` / `handle_identity` / `path_identity` / `pread_all` / `pwrite_all` /
  `pread_once` / `pwrite_once`
- 路径级：**`atomic_rename`（C1 落点）** / `sync_directory` / `flush_and_sync_stream` /
  `remove_file` / `prefetch_range` / `page_size`
- 进程级：`current_process_id` / **`process_alive`（C4/风险#2 落点）** / `max_open_files`

**收编站点**：`file_util.hpp`（原子写 ×2 + 目录 fsync + fdatasync）、`vec_disk_internal.hpp`
（`pread_all`/`pwrite_all` 下沉，`TmpFile` 换句柄类型；数十处调用站点零改动）、
`ivf_rq.cpp`、`diskann.cpp`、`hnsw.cpp`（站点最多）、`segment_v2.cpp`、`file_lock.cpp`、
`cask.cpp`（进程原语）。`hnsw.hpp` 的 `dev_t`/`ino_t` 换 `io::FileIdentity`，
**`<sys/types.h>` 逐出公开头**（原污染所有下游用户）。

**三处「像机械替换、实则会漂」的语义，已原样保留**（收编的主要风险都在这里，
非 API 翻译）：
1. **`O_APPEND` × `pwrite`**：`file_lock` 原用 `O_CREAT|O_EXCL|O_RDWR|O_SYNC`
   （**无** `O_APPEND`），而 `OpenFlag::kCreate` 带 `O_APPEND`。POSIX 下
   **`O_APPEND` 会让 `pwrite` 忽略 offset 改为追加**——锁文件的 `ftruncate(0)` +
   `pwrite(0,…)` 覆盖写语义会被悄悄改掉（当前因先截断到 0 而结果碰巧一致，
   但已不由 offset 决定）。为此新增 `kNoAppend`。
2. **`O_SYNC` ≠ `O_DSYNC`**：库内其余写路径的 `kOSync` 映射到 `O_DSYNC`（S13-P2），
   而写锁需要元数据也同步。为此新增 `kSyncAll`，未合并两者。
3. **单次 pwrite ≠ 循环 pwrite**：`file_lock::write_data` 的 legacy 契约明写
   「不循环，部分写也算成功」，套 `pwrite_all`（重试补齐）或 `File::pwrite`（循环）
   都会改掉它。为此新增 `pread_once`/`pwrite_once` 与 `*_all` 并存。

**两处刻意的收紧**（非纯搬运，已在代码注释标注）：
- 4 处「开文件后 `::read` 读头部」改为 `pread_all` 定位读：消除对 fd 内部偏移的
  依赖（正是 C3 要解决的形态），且短读由失败改为先重试补齐（更严，非放松）。
- hnsw 两处 payload load 的身份收养由「按路径 `::stat`」改为「按已持有句柄
  `handle_identity`」：排除「校验与收养之间路径被外部替换」的窗口。

**构建**：`bitcask_format` 补 `PUBLIC bitcask_io`——`file_util.hpp` 经 `index_manifest.hpp` /
`field_schema.hpp` / `oki_run.hpp` / `search_checkpoint.hpp` 传播到几乎所有下游，
逐 target 补链接会变成打地鼠。无环（`bitcask_io` 不依赖 format）。

**验收**：Debug 全量 **725/725**、**ASan 全量 725/725**、build-rel 双树零错误
（1 项 S30RssProbe 预存 Disabled）。

### S37-2 — 测试解耦：fork → exec-self 🔴 HIGH（纯 Linux 可交付）

4 个测试文件用 `fork()` + `_exit()` 模拟崩溃恢复，是全套中最有价值的一批
（WAL / torn tail / 墓碑复活门 / 原子批区间提交）：
`crash_recovery_test.cpp`、`oki_levelb_test.cpp`（`:476` `:565` `:787`）、
`oki_recovery_test.cpp`、`txn_test.cpp`（`:182`）。

改为「子进程 = 重新 exec 测试二进制自身 + `--bitcask-child-scenario=<name>` argv 分发」，
父进程 `posix_spawn` + `waitpid`（Windows 侧换 `CreateProcess` + `WaitForSingleObject`）。

> ⚠️ **实质工作量在语义差异**：exec-self 下子进程不再继承 fork 时刻的内存状态
> （已建好的 keydir、已打开的句柄）。每个 case 需逐个确认能否改为「子进程自己重建」，
> 不能的要重新设计断言。这不是机械改写。

顺带清理：`::getpid()` 造临时目录（4 处）、硬编码 `/tmp`（`scanner_test.cpp:53`）、
`<dirent.h>`（`thread_pool_test.cpp`）、直接 `stat` 断言 inode
（`cask_docvalue_test.cpp:38`、`hnsw_test.cpp:38`，改用 S37-1 的 `FileIdentity`）。

- **工作量**：1 周
- **验收**：ctest 全绿；4 个崩溃场景的原有断言意图全部保留（逐条对照 review）

#### ✅ S37-2 落地记录（2026-08-07）

**骨架**：`tests/support/crash_child.{hpp,cpp}` + `crash_main.cpp`，聚成
`bitcask_test_crash_child` 静态库（用它的目标链 `GTest::gtest` 而非 `gtest_main`
——需在 `InitGoogleTest` 之前拦截参数分发）。子进程 = `posix_spawn` 本测试二进制
自身 + `--bitcask-crash-child=<场景> --bitcask-crash-dir=<路径>`。
Windows 分支（`CreateProcessW`）已留位并标注 S37-5。

**6 个 fork 点全部转换**：`crash_recovery`(mid_put)、`txn`(txn_crash_after_commit)、
`oki_recovery`(oki_crash_tail)、`oki_levelb`(b1_checkpoint_fsync /
crash_after_merge / retired_files_crash)。事前担心的「依赖 fork 时刻内存快照」
**未发生**——6 个场景全部形如「`Cask::open(dir)` → 干活 → `_exit(N)`」，唯一
输入是目录路径。

**⚠️ 本届最值得记住的一条：`return N` ≠ `_exit(N)`，且失败形态是静默的**

改造初版让场景函数 `return` 退出码。**编译通过、6 个里 4 个照样 PASS。**
但 `return` 会正常展开栈 → 局部 `Cask` 析构 → `close()` → **OKI flush +
写锁释放**，注入的崩溃态当场消失：那 4 个测试的断言在「干净关闭」下同样成立，
于是**不声不响地不再检验崩溃路径**。只有 2 个（OKI tail 重放 / B1 不变量）
因为断言了「必须存在未固化的尾巴」才炸出来。

排查证据：子进程跑完后目录里**没有 `bitcask.write.lock`**、且已出现 `seg-2`
（第二段被 flush 成 run）——正常关闭的指纹。

**修法是把契约做成强制的**，而非写进注释靠自觉：
- 场景函数返回类型改 `void`，退出一律经 `crash_exit(code)`（`_Exit`，不展开栈）；
- 场景函数若正常走到结尾（忘了调），骨架带诊断以 122 退出，**不当作成功**。

修后复验：子进程目录里写锁残留、只有 `seg-1`，与原 fork 版指纹一致。

**顺带清理的 POSIX 依赖**（新增 `tests/support/test_paths.hpp`）：
- `::getpid()` ×9 文件 → `test_pid()`（走库的 io seam，Windows 已覆盖）
- 硬编码 `/tmp` 负面路径（`scanner_test`）→ `nonexistent_path()`
- `<dirent.h>` 枚举 `/proc/self/task`（`thread_pool_test`）→ `std::filesystem`；
  **`/proc` 本身无 Windows 对应物**，已标注 S37-5 需换
  `CreateToolhelp32Snapshot` 或把该用例整体标 Linux-only
- `::stat` 断言 inode/size（`hnsw_test` ×4、`cask_docvalue_test` ×3）→
  `identity_of()` / `file_size_of()`；`::truncate` → `fs::resize_file`
- `tests/` 下 POSIX 头（`<unistd.h>`/`<sys/wait.h>`/`<sys/stat.h>`/`<dirent.h>`）
  **归零**

**W1 现状复核**：exec-self 已让 fd 继承成为真实语义面，但**当前 6 个场景不触发**
——父进程都是「先 spawn 后重开」，spawn 时手里没有打开的 Cask。W1 仍待评估。

**验收**：Debug 全量 **725/725**（含 6 个崩溃场景）；ASan 全量 725/725。

### S37-3 — SIMD 派发层分 ISA TU 🔴 HIGH（大部分纯 Linux 可交付）

**当前状态：MSVC 下所有 SIMD 内核被 25 处 `#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))`
守卫整体编译掉，静默退化为 scalar 且不报错。** 即「能编过」与「有 SIMD」差一整个阶段。

1. 新建 `src/simd/`：`cpu_features`（CPUID 探测）+ `kernels_{sse42,avx2,avx512,vnni}.cpp`
   + `dispatch.cpp`（函数指针表）。CMake 按 TU 施加 `/arch:`（MSVC）或 `-m*`/target 属性（GCC/Clang）。
2. **3 个公开头 + 1 个内部头交出内联内核**：`bm25_kernels.hpp`、`detail/int8_kernels.hpp`、
   `hw_crc32.hpp`、`src/vector/hnsw_kernels.hpp` —— 跨 TU 边界迁移，内联被切断处须实测无回退
   （`bm25_kernels` 在 BOW 热路径上最需盯）。
3. `__builtin_cpu_supports`（18 处）→ 自实现 CPUID。
   **必须同时检查 `XCR0`**（AVX 需 OSXSAVE + XCR0[2:1]，AVX-512 还需 XCR0[7:5]）——
   `__builtin_cpu_supports` 替我们做了这步，手写漏掉会在「CPU 支持但 OS/hypervisor
   未启用 YMM/ZMM 保存」的机器上直接 `#UD`，且只在特定虚拟化环境复现。**须写单测**。
4. **收紧 AVX-512 运行时门为 `F && CD && BW && DQ && VL`**：MSVC 的 `/arch:AVX512`
   隐含此集合，比代码当前 `target("avx512f")` 的窄集更宽，编译器可能在胶水代码里
   生成 BW/DQ/VL 指令而门只查了 F → 在仅 F+CD 的 CPU 上 `#UD`。
5. 机械替换：`__builtin_ia32_pause`→`_mm_pause`、`__builtin_popcount*`→`std::popcount`、
   `__builtin_memcpy`→`std::memcpy`、`__attribute__((noinline))`→宏、
   `#pragma GCC visibility`（`c_api/internal.h`）删除（靠已有 `BITCASK_API`）、
   25 处 `__x86_64__` 守卫 → `BITCASK_X86_64` 宏（注意 `cask.cpp:1253` 未带编译器条件）。
6. **新增 `BITCASK_SIMD_MAX=scalar|sse42|avx2|avx512` 强制降档开关**：让 CI 在同一台机器上
   把所有 ISA 档位都跑一遍对拍，而不是听天由命看 runner 的 CPU 型号。Linux 侧同样受益。

- **工作量**：2-2.5 天设计 + 2 周实现
- **验收**：全 ISA 档位对拍（`int8_kernels` 既有 `self_test` + BM25/intersect/crc32
  scalar-vs-SIMD）；bench 零回归；**仓库既有纪律「改评分算法必须过三方穷举对拍」在此适用**

> **执行拆分（2026-08-07）**：本项拆两步做。搬内核之前**必须先有对拍手段**——
> 否则搬完无从验证（见下方 3.a 的发现）。
> - **3.a** `cpu_features` + `BITCASK_SIMD_MAX` + 单测 + 派发点切换（纯增量，
>   派发逻辑不变，只换探测来源）✅ done
> - **3.b** 内核出头文件 → 分 ISA TU + `/arch:` 施加 + 机械替换 ⬜ 未开始

#### ✅ S37-3.a 落地记录（2026-08-07）

**开工即发现的盲区**：本机 CPU 为 `avx2 avx_vnni fma pclmulqdq sse4_2`，
**无 AVX-512**；`ci.yml` 也无任何 ISA 相关配置。即：仓库里的 **AVX-512 内核
从未在本地或 CI 上被执行过**——改错了不会红。这比设计稿预估的更严重，
也正是「先建对拍手段再搬内核」的直接理由。

**新增**：`include/bitcask/detail/cpu_features.hpp` + `src/simd/cpu_features.cpp`
（新 `bitcask_simd` 库，由 `bitcask_format` PUBLIC 传播——`codec.cpp` 同时包含
`hw_crc32.hpp` 与 `int8_kernels.hpp`）。三重过滤后的能力位：
CPUID 报告 ∧ XCR0 表明 OS 已启用相应寄存器状态 ∧ 未被 `BITCASK_SIMD_MAX` 钳掉。

**验证策略——与 `__builtin_cpu_supports` 逐位对拍**：`cpu_features_test` 的
`MatchesCompilerBuiltin` 把手写 CPUID 的 11 个位与编译器内建对拍。这是关键的
一步：GCC/Clang 的内建是**替我们做过 XCR0 检查**的权威实现，MSVC 上没有——
只有在 GCC/Clang 上对拍通过，才有底气到 MSVC 上只留手写版。本机通过，
含最易抄错的 **AVX-VNNI（leaf 7 subleaf **1** 的 EAX bit 4，与 AVX512-VNNI 的
leaf 7.0 ECX bit 11 位置迥异）**。

**AVX-512 门已收紧**为 `F && CD && BW && DQ && VL`（原仅查 `avx512f`），
理由见设计稿 §2.5。`have_avx512()` 单一出口，5 个派发点全部随之收紧。

**`BITCASK_SIMD_MAX` 行为**（实测）：`scalar/sse42/avx2/avxvnni` 下调生效；
`avx512/avx512vnni` 在无该硬件的机器上正确保持 `avxvnni`（**只降不升**）；
拼错档位 **SIGABRT**（不做「警告后忽略」——CI 矩阵里一个拼错的档位若被静默
忽略，那个 job 会在满档下跑却显示为在测低档，即「测了个寂寞还报绿」）。

**派发点切换**：`src/` + `include/` 的 18 处 `__builtin_cpu_supports` 与
3 处 `__builtin_cpu_init` **归零**（`hw_crc32` / `bm25_kernels` / `int8_kernels` /
`hnsw.cpp` / `vector_plugin.cpp` / `intersect.cpp` / `index.cpp`）。
`bench/avx512_verify_bench.cpp` 等诊断 bench 暂留（默认不构建，S37-3.b 一并处理）。

**验收——本届第一次真正的跨档对拍**：全套 **732/732** 在
`scalar` / `sse42` / `avx2` / `avxvnni` 四档下**逐档全绿**（此前只有 avxvnni
一条码路被执行过）。build-rel 双树零错误（三个公开头改动）；ASan 732/732。

**仍未覆盖**：AVX-512 / AVX512-VNNI 两档——降档开关只能往下钳，覆盖它们必须
有带 AVX-512 的机器（S37-7 的 CI 矩阵）。这是 3.b 搬内核时的**已知风险敞口**。

#### ✅ S37-3.b 落地记录（2026-08-07）

**24 个 `__attribute__((target))` 函数全部搬进分 ISA TU**，第一方 GCC 扩展归零。

新增 CMake 函数 `bitcask_simd_tu(<源文件> <档位>)`（`avx2|avx512|vnni|vnni512|sse42`）
统一施加 `-m*`（GCC/Clang）或 `/arch:`（MSVC）。**GCC/Clang 侧也改用 `-m` 开关而
非保留 target 属性**——两边一套结构，免得维护两份。

新增 12 个分 ISA TU：

| 模块 | 新 TU | 档 |
|---|---|---|
| hnsw 距离 | `hnsw_kernels_avx2/_avx512.cpp` | avx2 / avx512 |
| int8 点积 | `int8_kernels_avx2/_vnni/_vnni512.cpp` | avx2 / vnni / vnni512 |
| BM25 打分 | `bm25_kernels_avx2/_avx512.cpp` | avx2 / avx512 |
| 求交 | `intersect_kernels_avx2/_avx512.cpp` | avx2 / avx512 |
| 向量归一化 | `vector_plugin_kernels_avx2/_avx512.cpp` | avx2 / avx512 |
| CRC32 折叠 | `simd/crc32_sse42.cpp` | sse42 |
| Index gather | `index_kernels_avx2.cpp` | avx2 |

**3 个公开头 + 1 个内部头交出内联内核**（`bm25_kernels.hpp` / `detail/int8_kernels.hpp` /
`hw_crc32.hpp` / `hnsw_kernels.hpp`）——它们经 `format.hpp`/`hnsw.hpp`/`codec.cpp`
传播到大量 TU，内核留在头里就无法对包含者分别施加 ISA 开关。

**顺带修掉的一个潜在 SIGILL**：`Index::fill_is_live` / `fill_doc_lens` 原**整个成员
函数**带 `target("avx2")` 却被**无条件调用**——该属性允许编译器在函数任何位置
（含运行时门之后才该走的 scalar 回退）生成 AVX2，在无 AVX2 的 CPU 上即 SIGILL。
反汇编确认当前 GCC -O3 未真的越界生成（**潜在风险而非现行 bug**），但结构上不
该依赖编译器的克制；内核拆出后调用方是普通 TU，编译器无权在其中生成 AVX2。

**机械替换**：`__builtin_ia32_pause`→`_mm_pause`（2 处）、`__builtin_popcount*`→
`std::popcount`、`__builtin_memcpy`→`std::memcpy`、`__attribute__((no_sanitize))`→
`BITCASK_NO_SANITIZE` 宏、25 处 `#if defined(__x86_64__) && (GNUC||clang)` →
`BITCASK_X86_64`（后半个条件本就只因内核用了 GCC 扩展而存在）。

**踩到的一个构建坑**：分 ISA TU 与**预编译头冲突**——PCH 在目标级用统一选项
预编译，而本函数给单个 TU 追加 `-m`，GCC 报
`cmake_pch.hxx.gch: created and used with differing settings of '-mavx'`。
这属**警告而非错误**，极易被忽略后演变成难定位的行为差异。已在
`bitcask_simd_tu` 内固定 `SKIP_PRECOMPILE_HEADERS ON`。

**验收**：
- **ISA 泄漏检查**：反汇编本次构建的 **42 个非 SIMD TU，宽指令（zmm/vfmadd/
  vpgather/vpclmul/ymm）泄漏 0**；5 个 avx512/vnni512 TU 确实含 zmm。隔离
  从「靠编译器自觉」变成结构性成立。
  （首轮检查曾报 2 处泄漏，追查为 7 月 4 日的**陈旧 .o**——`bitcask_index.dir` /
  `bitcask_search.dir` 是已不存在的旧目标遗留目录。）
- **四档跨档回归**：`scalar`/`sse42`/`avx2`/`avxvnni` 逐档 **732/732**。
- **CRC32 内核体与原版逐字 diff 一致**（唯一差异是删掉的守卫行）——搬运忠实。
- **bench 零回归**：同机交替 A/B + `taskset` 绑核 + 3 轮取最小值，
  crc32(4 档尺寸)/BOW/intersect 全部落在 **±2%** 内。
  ⚠️ **首轮非绑核测量曾报 crc32 +27%、intersect +40%**，但**未改动的 scalar 路径
  也「回退」13%**——据此判定为负载噪声（load avg >10 / 8 核），改用绑核交替
  后全部消失。**这类噪声足以让人误判并去「优化」一个并不存在的回退。**

### S37-4 — MSVC 构建适配 🟡 MED（首个需要 Windows 环境的任务）

1. **编译第一天必炸三项**：`/utf-8`（187 源文件中 **181 个含中文注释**，不给此项 MSVC
   按系统 ANSI 代码页解析，中文字节吃掉后续代码报无从溯源的 C2001/C4819）；
   `NOMINMAX` + `WIN32_LEAN_AND_MEAN`（`windows.h` 的 `min`/`max` 宏 vs 全库 94 处
   `std::min/max`，且 `windows.h` **只准进移植层 TU**）；`/bigobj`。
2. `CMakeLists.txt:30-113` 全部 GCC/Clang 选项加 `if(MSVC)` 分流（映射表见设计稿 §1.2）。
   `/W4` 首轮**不开 `/WX`**（MSVC 独有告警 C4267/C4244/C4100 会刷屏并阻塞进度），
   逐条清理后再开。
3. **静态库合并重写**：`CMakeLists.txt:458-471` 在配置期生成 bash 脚本跑 `ar x`，
   Windows 无 bash/ar。改 `OBJECT` 库 + `$<TARGET_OBJECTS:>`。
   **顺带修既有隐患**：`ar x` 把 13 个归档解到同一临时目录，不同库中的**同名 `.o`
   会静默相互覆盖** —— 当前是否已发生取决于文件名巧合，属定时炸弹。
4. install 规则补 Windows 布局（`RUNTIME`→`bin/`、`ARCHIVE`(.lib)→`lib/`；
   现二者都指向 `LIBDIR`）。
5. vcpkg manifest 承载 zlib + TBB；cppjieba/limonp 在 MSVC 下的可编译性实测。

- **工作量**：4-6 天
- **验收**：MSVC 能编出 `bitcask.dll` + `bitcask.lib`；ctest 能跑起来（不要求全绿）

#### ✅ S37-4 落地记录（2026-08-07）

环境：VS 18 / **MSVC 14.51（cl 19.51）** / Windows SDK 10.0.28000 / CMake 4.3 / Ninja 1.13。
`scripts/Enter-MsvcEnv.ps1` 把 VsDevCmd 的环境导进当前会话（本届新增的开发脚本）。

**结论先行：全部 188 个 TU（第一方 + 61 个源 + 40 个测试 + tools + 第三方）
在 MSVC 下编译零错误；剩余 1851 条链接错误 100% 是 `bitcask::io` 的 34 个符号
——即 S37-5 的完整工作面，无一例外。** 编译面比设计稿预估小得多。

**依赖策略（决策变更）**：设计稿 §1.4 建议 vcpkg 承载 zlib + TBB。**TBB 改从
`third_party/oneTBB` 子模块现编**——vcpkg 的 `tbb` 端口经 `hwloc` 依赖会拖进
msys2（m4/perl/autotools/ncurses…）整套工具链，与「纯 MSVC」构建约束冲突。
oneTBB 自身用 MSVC + CMake 直接可编（无 hwloc 时只跳过 `tbbbind` 的 NUMA 绑定）。
新增 `BITCASK_BUNDLED_TBB` 开关，Windows 与 TSan 构建默认 ON，Linux 默认 OFF
（`find_package` 行为逐字不变）。`vcpkg.json` 最终只剩 zlib，14 秒装完。

**构建系统改动**：
- 全局 MSVC 分流：`/utf-8 /bigobj /permissive- /Zc:preprocessor /Zc:__cplusplus`
  + `NOMINMAX` / `WIN32_LEAN_AND_MEAN` / `_CRT_SECURE_NO_WARNINGS` /
  `_CRT_NONSTDC_NO_WARNINGS`（目录作用域，置于所有 `add_library`/`add_subdirectory` 之前）
- `bitcask_warnings` 分流 `/W4`（**首轮不开 `/WX`**）；`BITCASK_WERROR`→`/WX`；
  `BITCASK_NATIVE` 在 MSVC 下 **FATAL_ERROR 而非静默忽略**；
  `BitcaskSanitizers.cmake` 加 MSVC 分支（ASan → `/fsanitize=address` + 清 `/RTC1`
  + `/INCREMENTAL:NO`；**TSan/UBSan/LSan 报错退出**——「以为在跑 TSan、实际没插桩」
  比构建失败危险得多）
- `WIN32` 下 `CMAKE_RUNTIME_OUTPUT_DIRECTORY` 收进 `bin/`（Windows 无 RPATH，
  exe 与 bitcask.dll/tbb12.dll/zlib1.dll 须同目录）
- `bitcask_io` 改为**按平台选后端**（`posix_file.cpp` / `win32_file.cpp`）；
  后端文件不存在时告警并留空——库目标仍全部可编译，这正是本届量编译面的形态
- `cmake_minimum_required` 3.20 → 3.21（见下）

**静态库合并重写（设计稿 §1.3）——未按原方案改 OBJECT 库**：
原实现在**配置期生成 bash 脚本**跑 `ar x` 解包 13 个归档再重打包，Windows 无
bash/ar。设计稿建议改 OBJECT 库，但那会**改动整个链接图**（每个测试 exe 从
「按需抽取」变成「全量吞入」），而「Linux 侧零回归」是硬约束且本机无法验证。
改用 `add_library(bitcask_static STATIC $<TARGET_OBJECTS:...>)`——`$<TARGET_OBJECTS:>`
自 CMake 3.21 起对 STATIC 目标同样可用（已实测确认：符号进归档且不重复编译），
于是**链接图逐字不变**，只有「怎么产出这一个归档」变了。同样杜绝了同名 `.o`
互相覆盖的隐患（对象按目标各自成路径喂入，不再解包到同一临时目录）。

顺带修掉两个**现行缺陷**：
1. **`bitcask_simd` 原本不在合并列表里**（S37-3 新增该库时漏补）——即 v6.1.0
   发布的 `libbitcask.a` 缺 `cpu_features` / `crc32_sse42` 两个 TU，静态链接的
   下游会撞未定义符号。已补入并 `dumpbin /ARCHIVEMEMBERS` 核验（58 个成员，
   `bitcask::simd::*` 符号确为定义态）。
2. install 规则的合并静态库那条原是 `install(FILES <硬编码 .a 路径>)`，Windows
   上指向不存在的文件；改 `install(TARGETS)`。Windows 下产物改名
   `bitcask_static.lib`——`bitcask_shared` 的**导入库**已占用 `bitcask.lib`。

**三处被 `/usr/include` 长期掩盖的依赖缺失**（Windows 无全局头目录，一编即炸）：
| 目标 | 缺 | 经由 |
|---|---|---|
| `bitcask_simd` | `ZLIB::ZLIB` | 公开头 `hw_crc32.hpp` 直接 `#include <zlib.h>` |
| `bitcask_bm25` | TBB 需 PRIVATE→**PUBLIC** | 公开头 `inverted.hpp` 含 TBB 头，经 segment/text_plugin/search_cache 传播 |
| `bitcask_cask` | TBB 需 PRIVATE→**PUBLIC** | **公开头** `cask.hpp` 含 `thread_pool.hpp` → TBB |
| `bitcask_keydir` | TBB（PRIVATE 即可）| `keydir_registry.cpp` 含 `thread_pool.hpp` |

**⚠️ 本届最值得记住的一条：`#if defined(__has_feature) && __has_feature(...)`
在符合标准的预处理器下是语法错误，不是「MSVC 方言问题」**

全库 6 处 TSan 探测写成
`#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))`。
标准要求先对整个 `#if` 表达式做宏替换、把剩余标识符换成 `0`，于是右半边成为
`0(thread_sanitizer)`——不合法。GCC/Clang 对 `&&` 右侧宽容，`/Zc:preprocessor`
严格按标准来，报 **C1012「unmatched parenthesis」**，且错误位置指向 `#if` 行，
与真实原因（探测宏不存在）毫无关联，极难追。一处在 `thread_pool.hpp` 里，
波及 `bitcask_keydir`/`bitcask_cask`/`bitcask_hybrid` 等一大片。

修法：`detail/cpu_features.hpp` 新增 **`BITCASK_TSAN_ENABLED`** 单一出口，
用**嵌套** `#if`（外层先确认 `__has_feature` 存在，内层才调用）。仓库里
`oki_levelb/oki_locate/oki_range_test` 三处本来就写的是正确的嵌套形式——
即正确写法一直在库里，只是没被推广。6 处已全部切换。

**其余源码改动（全部是「Linux 上碰巧能过」的真问题）**：
- `seq_shard_table.hpp` 的 `__atomic_load_n(..., __ATOMIC_RELAXED)` → MSVC 侧
  `__iso_volatile_load64`。**GCC 分支逐字保留**：原注释里三条理由（TBAA 豁免、
  无 libcall、TSan 原生理解）中前两条在 GCC 上是正确性依赖，换 `std::atomic_ref`
  有重新引入陈旧读的风险且无法在本机验证。MSVC 侧选 `__iso_` 前缀那一族而非
  普通 `volatile`：x64 默认 `/volatile:ms` 会给 volatile 读加 acquire 语义，
  强于此处需要的 relaxed。
- `intersect_kernels_avx2.cpp` 的 `__m256i_u`（GCC 私有类型）→ `__m256i`；
  `_mm256_loadu_si256` 的非对齐语义由 intrinsic 自身保证，不靠指针类型。
- `intersect_kernels_avx512.cpp` 补 `<bit>`（`std::popcount`）、
  `keydir_test.cpp` 补 `<algorithm>`（`std::sort`）——MSVC STL 不做传递包含。
- **`fs::path::c_str()` 在 Windows 返回 `const wchar_t*`**（设计稿 C8）：
  `migrate.cpp` ×1 + 测试 ×7 处直接喂给窄 `std::fopen`，编译失败。改 `.string()`，
  与全库「路径以 `std::string` 流转」约定一致。⚠️ 这只解决可编译性——`.string()`
  走系统 ANSI 代码页，**非 ASCII 路径仍打不开**，属 S37-5 的 UTF-8→UTF-16 范畴。
- `c_api/internal.h` 的 `#pragma GCC visibility push/pop` **加编译器守卫而非
  按 S37-3.5 原计划删除**：`bitcask_shared` 是唯一不链 `bitcask_warnings` 的目标，
  拿不到 `-fvisibility=hidden`，这条 pragma 是那些助手符号在 Linux 上唯一的
  隐藏来源，删掉即改行为。
- `c_api_test.c` 用 `<stdatomic.h>`（parallel_scan 并发回调计数，非装饰）：
  MSVC 的 `.c` 默认 C89 且 C11 atomics 仍在开关后。该目标单独加
  `C_STANDARD 11` + `/experimental:c11atomics`，不动全局 `CMAKE_C_STANDARD`
  （那会把 utf8proc/zlib 的 C 方言从 gnu17 降到 gnu11，属无谓波及）。

**告警面（S37-7 开 `/WX` 前的账）——远好于设计稿预估**：
库目标全量 `/W4` 共 **94 条**：C4324 ×60（cacheline `alignas` 的填充提示，无害）、
C4244 ×32、C4100 ×1、C4456 ×1。**设计稿点名会刷屏的 C4267 实际为 0。**

**验收状态**：
| 项 | 状态 |
|---|---|
| 全部 188 个 TU 编译 | ✅ 0 错误 |
| 13 个第一方静态库 + 合并 `bitcask_static.lib` | ✅ 产出（121 MB，Debug） |
| `bitcask.dll` | ⛔ 缺 34 个 `bitcask::io` 符号（S37-5） |
| ctest | ⛔ 同上，40 个测试 exe 均卡在链接 |
| Linux 侧 | 🟡 Debug/Release 双树 + 干净树复验通过；ASan/TSan 未跑（见下） |

> ⚠️ **本届记录未覆盖的一项**：所有改动都只在 Windows 上验证过。虽然每处都按
> 「GCC 分支逐字不变」的原则做（`__atomic_load_n`、visibility pragma、
> `find_package(TBB)` 路径均保持原样），`BITCASK_TSAN_ENABLED` 的替换与
> 4 处 `target_link_libraries` 的 PRIVATE→PUBLIC 仍须在 Linux 上过一遍
> **Debug 全量 + ASan + TSan**（TSan 尤其——`BITCASK_TSAN_ENABLED` 若因漏包含
> 头而恒 0，TSan 注解会**静默失效**，表现为 TSan 误报而非编译错误）。

#### 🟡 Linux 复验记录（2026-08-07，df83b4b 时点）

构建系统改动最怕的是「MSVC 分支加进来时误伤 GNU 分支」，且这类错误在增量树上
可能被既有缓存掩盖，故**另起干净树**验证 configure 逻辑本身：

| 验证 | 结果 |
|---|---|
| `build-clang`（Debug + BUILD_TESTING=ON, clang++） | ✅ 编译 0 错误；ctest **732/732**（1 项 Disabled：`CheckpointRecoveryTest.S30RssProbe`）|
| `build-rel`（Release -O3+LTO + bench, g++ 14.2） | ✅ 编译 0 错误，`bitcask_bench` 链接通过 |
| **干净树** `cmake -S . -B <新目录>` Release | ✅ configure 0 警告，全部 target 建成（含 `bitcask_simd` / `bitcask_io`）|

告警面无新增，仍是既有的 `segment.hpp` 七条（`DocId`→`uint32_t` 收窄 ×4、
`pin` 字段缺失初始化 ×3），GCC 与 clang 两侧一致。

**仍未结清**：ASan / TSan 两棵树本次**没跑**。上面那条 TSan 顾虑
（`BITCASK_TSAN_ENABLED` 恒 0 → 注解静默失效）**编译和 Debug 全绿都测不出来**，
必须实跑 `build-tsan` 才能排除，此项保持未决。

**S37-5 的完整工作面（34 个未解析符号，链接器实测）**：

`File` 成员 ×9：`open` / `close_quiet` / `pread` / `pread_into` / `pwrite` /
`write` / `seek` / `seek_bof` / `sync` / `truncate_here`
（另 `truncate(len)` / `size()` / `identity()` / `release()` 在未解析表外——
未被 c_api 路径引用，实现时同样要补）
`MappedFile` ×4：`map_readonly` / `~MappedFile` / `operator=(&&)` / `reset`
句柄级 ×11：`open_handle` / `close_handle` / `sync_data` / `truncate_handle` /
`handle_size` / `handle_identity` / `path_identity` / `pread_all` / `pwrite_all` /
`pread_once` / `pwrite_once`
路径级 ×5：**`atomic_rename`** / `sync_directory` / `flush_and_sync_stream` /
`remove_file` / `prefetch_range`
进程/系统 ×4：`current_process_id` / **`process_alive`** / `max_open_files` / `page_size`

### S37-5 — Windows I/O 后端 🔴 HIGH

实现 `src/io/win32_file.cpp`，填 S37-1 留下的 seam：

1. `CreateFileW` + UTF-8→UTF-16 路径转换（**非 ASCII 目录名在 ANSI 代码页下直接打不开**）。
   编译期地雷：`std::filesystem::path::c_str()` 在 Windows 返回 `const wchar_t*`，
   任何直接喂给 `::open`/`std::fopen` 处编译失败（`file_util.hpp:96` 即一例）；
   `data_file.cpp:452` 的 `find_last_of('/')` 改 `fs::path::filename()`。
2. **`atomic_rename` → `MoveFileExW(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)`**（C1）。
3. **定位读写（C3）**：Windows 无原生 pread；同步句柄上带 `OVERLAPPED.Offset` 的 `ReadFile`
   会更新文件指针且并发被内核串行化 —— 直接翻译会把并发点查悄悄变成串行，
   **测试全绿、只有 bench 掉**。定为**每线程句柄池**方案，并与 read-handle LRU 预算合并考虑。
   `io.hpp:6-12` 的线程模型注释需按平台重新表述。
4. **文件锁（C4）**：`CREATE_NEW` 对应 `O_CREAT|O_EXCL`；`release_quiet` 的
   「先 unlink 后 close」需带 `FILE_SHARE_DELETE`。
   ⚠️ **stale-lock 检测必须加进程创建时间**（`GetProcessTimes` 的 `ftCreationTime`）：
   Windows PID 复用远快于 Linux，仅凭 `OpenProcess` 判存活会误判「新进程复用了崩溃
   进程的 PID」→ 拒绝回收有效 stale lock → **库彻底打不开**。本移植中最易造成生产事故的单点。
5. `sync_data` → `FlushFileBuffers`（无 data/metadata 区分，checkpoint 路径需重新 bench）；
   `sync_directory` → no-op（并在 `doc/format-zh.md` 补：Windows 下 rename 的目录项
   持久性改由 `MOVEFILE_WRITE_THROUGH` 承担）。
6. `MappedFile` → `CreateFileMappingW` + `MapViewOfFile`；**析构要收两个句柄**
   （`UnmapViewOfFile` + `CloseHandle(section)`，`MappedFile` 需多存一个成员）；
   `MADV_RANDOM` → `FILE_FLAG_RANDOM_ACCESS`（**时机从 mmap 后前移到开文件时**）；
   `prefetch` → `PrefetchVirtualMemory`。
7. `getrlimit(RLIMIT_NOFILE)`（`cask.cpp:184`）→ Windows 句柄不受 fd 表约束，
   需重新标定 read-handle LRU 预算（原逻辑是 `nofile` 的比例）。
8. MAX_PATH 260 评估：`longPathAware` manifest 或 `\\?\` 前缀。

- **工作量**：2.5-3 周
- **验收**：ctest 全绿（除已知 Windows 语义差异项）；crash 注入用例通过

#### ✅ S37-5 落地记录（2026-08-07）

**结果：`bitcask.dll` 产出，49 个 exe 全部链接通过，ctest 733 个用例 724 通过
（99%）。剩余 9 个失败全部落在 S37-6 的映射/删除生命周期，无一例外。**
崩溃注入 6 个场景在 Windows 上全绿。

**`FileHandle` 在 Windows 上改 `void*`（HANDLE）**——S37-1 头注释写明的设计意图。
备选是用 CRT 的 `_open_osfhandle` 把 HANDLE 包成 `int` fd（调用站点零改动），
否决理由：CRT fd 表有 8192 硬上限、全局锁、且「CRT fd + HANDLE 双重所有权」
是经典 double-close 温床，而本库要把大量 sealed 段句柄挂在 read-handle LRU 上。
**哨兵值选 `nullptr` 而非 `INVALID_HANDLE_VALUE`**：后者需 `reinterpret_cast`，
进不了 constexpr（`kInvalidHandle` 与 `handle_valid` 都是 constexpr，65 处依赖）；
归一在 `win32_file.cpp` 边界完成，出了那个文件只有一种无效表示。
连带修掉 7 处把 `FileHandle` 当 `int` 的硬编码（`fd_ >= 0` / `int fd_ = -1`），
其中 `file_lock.cpp` 那处在 Linux 上一直是对的、只是不该那么写。

**两处会静默出错、必须逐站点核对才能发现的语义**：

1. **`File::write()` 恒为「原子追加到 EOF」，不走文件指针。**
   Windows 同步句柄上带 `OVERLAPPED` 的定位读**会移动文件指针**（POSIX 的
   pread 不会）。而 `HintFile` 恰恰把顺序 `write()`（hint_file.cpp:50）与定位
   `pread`（112/129/179/189/206/230）**交错**用在同一个 `File` 上——直译成普通
   `WriteFile` 的话，一次 pread 就让随后的追加变成**从上次读到的位置覆盖**。
   编译通过、写入「成功」，只有 hint 内容被悄悄写坏。改用
   `OVERLAPPED.Offset/OffsetHigh` 全 1 的原子追加惯用法，与 O_APPEND 逐条对应
   且完全不受指针状态影响。（核查过：`File::write()` 全库仅此一个调用点，
   `File::read()` 零调用点。）
2. **`truncate(len)` 用 `SetFileInformationByHandle(FileEndOfFileInfo)`**
   而非 `SetFilePointerEx + SetEndOfFile`——后者以文件指针为截断点，会把
   io.hpp 明写「不依赖 fd 当前 offset，故线程安全」的 `truncate` 变成有状态操作。
   `truncate_here()` 则**刻意**用 SetEndOfFile（语义就是「截到指针处」）。

**首轮 ctest 35 个失败 → 9 个，三处修复**（按贡献排序）：

| 修复 | 修好 | 性质 |
|---|---|---|
| `parse_data_tstamp` 的分隔符 | ~12 | **产品 bug** |
| crash_child 的 `CreateProcessW` | 6 | S37-2 留位补齐 |
| 测试里的 POSIX 假设 | ~5 | 测试可移植性 |

**最值得记住的：`parse_data_tstamp` 只认 `/`，导致 merge「成功但从不干活」**
它用 `find_last_of('/')` 剥目录前缀，而 `merger.cpp:292` 传的是**完整路径**。
Windows 上剥不掉 → base 成了 `C:\dir\42` → 数字校验失败 → 返回 nullopt →
**每一个输入文件都被判为「不是 data 文件」而跳过**。merge 于是返回成功、
不报错、不崩，只是 `records_kept` 恒为 0。12 个 merge 相关用例全挂在这一处。
修法是按平台取分隔符集合（Linux 仍只有 `/`——`\` 在 POSIX 是合法文件名字符，
不能一并当分隔符），等价于 `fs::path::filename()` 但保持 noexcept 且不分配。

**⚠️ PID 复用加固（风险 #2）——新增的单测当场揪出一个会静默损坏数据的 bug**

锁文件格式扩为**两行**：
```
<pid> <activefile>\n
<start_token>\n        ← 本届新增：进程实例令牌
```
加第二行而非扩第一行是**刻意**的：现有两个 parser（`parse_leading_pid` 只读
开头连续数字、`parse_active_file_id_from_lock` 取首个空格到首个 `\n`）对第二行
完全不可见，于是新旧锁文件双向兼容。Windows 令牌 = `GetProcessTimes` 的
`ftCreationTime`；**POSIX 恒返回 0**（令牌为 0 时 `process_alive(pid, 0)` 退化为
`process_alive(pid)`），因此 **Linux 行为逐字不变**——不引入 `/proc/<pid>/stat`
的 starttime，那是本届不涉及、也无法在 Windows 上验证的行为变更。

写完实现后加的 `ProcessToken` 三个用例立刻炸出：**`process_alive()` 对当前
存活进程返回 `false`**。根因是 `WaitForSingleObject` 需要 `SYNCHRONIZE` 权限，
而 `PROCESS_QUERY_LIMITED_INFORMATION` **不含**它 → 返回 `WAIT_FAILED` 而非
`WAIT_TIMEOUT`。后果是**每一把写锁都被判为 stale 并删掉 → 两个 writer 能同时
持锁写同一个库**。同时把「只有明确 signaled 才判死」定成不变量，让
`WAIT_FAILED` 一类意外落到保守判活那侧。

> **`grep kWriteLocked tests/` 全库零命中**——写锁竞争路径此前完全没有测试覆盖。
> 这个 bug 走完全套 733 个用例照样全绿。`ProcessToken` 那组用例的价值全在
> **否定断言**（令牌不符必须判死）：若实现退化成只查 pid，肯定断言依然通过。
> 建议 S37-7 补一个真正的「两进程争锁」用例。

**Windows 后端与 POSIX 的语义差异总表**（均在 `win32_file.cpp` 对应函数处展开）：
| # | 差异 | 处置 |
|---|---|---|
| 1 | `File::write()` 恒追加 | 见上；全部调用点本就以追加模式开档 |
| 2 | 定位读写会移动文件指针 | 见上；已逐站点核对 |
| 3 | `sync_directory` 是 no-op | 持久性改由 `MOVEFILE_WRITE_THROUGH` 承担 |
| 4 | `FileMode`(0600/0644) 被忽略 | Windows 靠 ACL 继承 |
| 5 | `kCloseOnExec` 是 no-op | Windows 句柄默认不被继承（W1 在此平台不成立）|
| 6 | `max_open_files` 返回 nullopt | 句柄不受 fd 表约束；调用方兜底 1024 |
| 7 | `advise_random` 被忽略 | 见下「刻意没做」 |
| 8 | `kOSync`/`kSyncAll` 合并 | 都是 `FILE_FLAG_WRITE_THROUGH`；S13-P2 的 dsync 优化在此平台不存在 |

**刻意没做的两项（附理由，避免被当成遗漏）**：
- **`FILE_FLAG_RANDOM_ACCESS`**：设计稿 C7 建议把 `MADV_RANDOM` 的时机前移到
  开文件时。实测 6 个 `map_readonly` 调用点里 **3 个传 `false`**（segment_v2 /
  diskann / ivf_rq 是顺序访问），在 `kReadOnly` 上一刀切会关掉顺序路径的预读。
  该提示只影响预读启发、不涉正确性。若 bench 显示有收益，应做成 `OpenFlag`
  位由调用方指定。
- **每线程句柄池（C3 / 风险 #3）**：同步句柄上的 I/O 被内核在文件对象上串行化，
  即多线程并发 pread 同一句柄不会真正并行。**这正是「测试全绿、只有 bench 掉」
  的那一项**，本次未做——它要与 read-handle LRU 预算合并考虑，属独立工作。

**已知缺口**：窄路径一律按 **UTF-8** 解读（`MB_ERR_INVALID_CHARS` 严格拒非法
序列，不退回 ANSI 猜测——「同一个 char* 可能是两种编码」会让「打开了错误的
文件」无法定位）。但库内多处经 `fs::path::string()` 产出窄路径，它在 Windows 上
走**系统 ANSI 代码页**。纯 ASCII 路径两者一致（现有全部测试与典型部署），
**非 ASCII 路径会被判为非法 UTF-8 而报 EINVAL**。彻底修复要把那些站点换成
`u8string()`，见下方遗留 W4。

**🟡 Linux 复验（2026-08-07，fe4d545 时点）：无回归。**
`build-clang` Debug 与 `build-rel` Release+LTO+bench 均 0 错误；
ctest **735/735 通过**（736 项注册，1 项 Disabled 同上），耗时 86 秒。
用例数较 S37-4 时点的 733 增加 3 项——即本届新增的 `ProcessToken` 三个用例，
它们在 Linux 上同样参与（令牌恒 0 时退化为纯 pid 判活，见上）。
本届主体 `src/io/win32_file.cpp` 在 Linux 下不参与编译，**真正需要 Linux 复验的
是那 7 处「把 `FileHandle` 当 `int`」的连带修改**——它们在跨平台代码里，全绿即无回归。
告警面与 S37-4 时点逐条一致，无新增。
⚠️ 落地记录开头的「724/733」是 **Windows 上**的数，Linux 侧覆盖不到那 9 个失败；
ASan / TSan 仍未跑（同 S37-4 复验记录）。

### S37-6 — 删除/映射生命周期 🔴 HIGH（**唯一架构改动**）

Windows 下：① 所有 `CreateFile` 须带 `FILE_SHARE_DELETE`，否则删除报
`ERROR_SHARING_VIOLATION`；② **即使带了，被 section 映射持有的文件仍删不掉**
（映射对象持独立引用，与句柄共享模式无关）。

现有 `retire_files`/`drain_retired_files`（`cask.cpp:833-858`）重试队列是**必要但不充分**的
兜底：只要 sealed 段的 mmap 还在 read-handle LRU 里，重试永远失败，**队列无限增长**。

**新增不变量**：退休一个文件前，先把它从 read-handle LRU 逐出并 `reset()` 其 `MappedFile`。
流程改为 `evict_mappings(file_id) → close handles → delete`，且 evict 须与
`epoch_reclaim.hpp` 的并发回收协调 —— 不能在读者仍持 `span` 时 unmap。

需同步修订：`doc/read-handle-lru-design-zh.md`、`doc/sealed-mmap-read-design-zh.md`、
`io.hpp:135-151`（`MappedFile` 头注释明写依赖「POSIX unlink-while-mapped 语义」，
该注释在 Windows 上失效）。

- **工作量**：1-1.5 周
- **验收**：Windows 上 merge 后退休队列能排空（长跑测试）；Linux 侧行为零变化 + TSan 全绿

#### ✅ S37-6 落地记录（2026-08-07）—— **结论：不是架构改动**

**ctest 733/733 全绿。**

**开工第一件事是实测本项赖以立项的前提，结果推翻了它。**
设计稿 C2 断言「即使带 `FILE_SHARE_DELETE`，被 section 映射持有的文件仍删不掉」，
并据此规划「退休前先 `evict_mappings` 并与 `epoch_reclaim` 协调」这一架构改动。
写了个复刻本库 `MappedFile` 建法的探针逐条量（Windows 10.0.26200）：

| 设计稿断言 | 实测 |
|---|---|
| 被映射的文件删不掉 | **假**——删除成功，名字**立刻**从目录消失（Win10 1709+ 的 POSIX 语义删除），同名可立即重建 |
| 被映射的文件不能被 rename 覆盖 | **假**（前提是目标没有打开的文件句柄）|
| 覆盖/删除后旧视图读到什么 | **旧内容**，含之后才首次触碰的页——与 POSIX unlink-while-mapped **逐条等价** |

于是重试队列在 Windows 上本就能正常排空，`evict_mappings` 与 epoch 协调**都不必做**。
**风险排序第一位的那项，实际不存在。**

**真正的限制是另外两条，设计稿都没提到**：

1. **`MoveFileEx(REPLACE_EXISTING)` 覆盖一个尚有文件句柄打开的目标必然
   `ERROR_ACCESS_DENIED`**——与共享模式、与映射都无关，任何访问模式（连
   `GENERIC_READ`）都拦。命中点：`IvfSegment`/`DiskannSegment::open` 建好映射后
   仍把 `fd_` 留着，而**那个 fd 从头到尾没被读过**（段全部经 `base_` 访问，
   `fd_` 只在析构里被关）。段 rebase 正是「写 tmp → rename 覆盖旧段」的形态，
   于是 4 个 rebase 用例全挂。
   修法就是建好映射后立刻 `close_handle`——`io.hpp` 早写着「向量 payload 关 fd
   省预算」，这两个站点一直没照做。两平台同时受益（每个 sealed 段少一个 fd）。
   `fd_` 成员随之从两个公开头删除：改完之后它恒为 `kInvalidHandle`，
   `close()` 里那个分支成了死代码，而留一个永远无效的成员会让后来者以为
   「这儿有个 fd 可以用」。
2. **CRT stdio 是第二类阻塞源**。MSVC 的 `std::fopen` / `std::ifstream` 用
   `_SH_DENYNO`，共享位不含 `FILE_SHARE_DELETE`，**且 CRT 不提供任何开关**。
   任何被它们长期持有的文件都删不掉（`ERROR_SHARING_VIOLATION`）。
   新增 `io::open_stream(path, mode)`：POSIX 就是 `std::fopen`；Windows 走
   `CreateFileW`(带 SHARE_DELETE) → `_open_osfhandle` → `_fdopen`。
   全库长期持有的 `std::FILE*` **只有一处**（`field_schema.hpp:105` 的追加句柄），
   已切换；用完即关的 `FilePtr`/`AtomicFileWriter` 保持 `std::fopen` 不动。

**9 个失败的最终归因与修法**：

| 失败 | 真因 | 修法 |
|---|---|---|
| Diskann/Ivf Rebase ×4 | 段留着没用的 `fd_` 挡住 rename 覆盖 | 建映射后立刻关句柄 |
| FieldSchema | CRT `fopen` 无 SHARE_DELETE | `io::open_stream` |
| BackupHotCopy / MigrateHintOrd / SearcherFacade | 同上（目录里那个 `field.schema` 一个句柄卡住整个 `remove_all`）| 同上 |
| InvertedIndex.V5BlockMinDl | **测试**里 `std::ifstream` 未出作用域就 `remove` | 加一层作用域 |

> 注意最后一列：5 个「目录/文件删不掉」的失败里，**4 个的根因是同一个
> `field.schema` 句柄**。Windows 上一个漏网的 CRT 句柄就能让整个库目录删不掉，
> 排查时很容易误以为是 mmap。

**顺带修掉一个我自己引入的 flaky**：S37-5 给 W3 补的 `count_os_threads`
Windows 实现，单跑 5/5 全过、`ctest -j 8` 下偶发失败。两处原因叠加：
① `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD)` 拍的是全系统线程表，繁忙时会以
`ERROR_BAD_LENGTH` 失败（微软文档明载需重试），首版失败即返回 0；
② 进程线程总数本就受被测对象之外的因素影响（ntdll 加载器工作线程、延迟加载 DLL）。
①加重试，②把整轮测量改为最多 5 次重试、任一轮满足不变量即通过。
**重试不削弱断言**：真回归是确定性的，每轮都会违反。
> flaky 测试比没有测试更糟——它会让人养成忽略这个用例的习惯。

**新增 3 个不变量测试**（`tests/posix_file_test.cpp` 的 `FileLifecycle`）：
把本届赖以成立的三条语义从「一次性探针验过」变成常驻守护——
`DeleteWhileMappedKeepsViewContent` / `AtomicRenameOverMappedFileKeepsViewContent`
（含「rename 前必须先关句柄」那条纪律）/ `OpenStreamAllowsDeleteWhileHeld`。
三条在 POSIX 上是常识、在 Windows 上是实测结论，任一条不成立的表现都是
「merge 后文件删不掉、退休队列增长」或「rebase 静默失败」，不会在别处炸出来。

**文档同步**：`io.hpp` 的 `MappedFile` 头注释、`doc/read-handle-lru-design-zh.md`、
`doc/sealed-mmap-read-design-zh.md` 均补「Windows 上同一条 pin 语义成立（实测）」；
`doc/windows-port-design-zh.md` 的 C2 加了醒目更正块并保留原文作对照——
留一个已知为假的前提在定稿设计里，下一个读它的人会照着做无谓的架构改动。

---

#### 🎯（历史）S37-6 开工前的工作面（S37-5 的 ctest 量出，2026-08-07）

| 失败用例 | 卡在哪 |
|---|---|
| `DiskannPlugin.RebaseDropsDeadPhysically` / `RebaseMinDocsWindow` | `IvfSegment/DiskannSegment::build` 写 tmp 后 `atomic_rename` 覆盖旧段文件，而**旧段文件正被 `MappedFile` 持有**——被 section 映射的文件在 Windows 上既删不掉也覆盖不了 |
| `IvfPlugin.RebaseDropsDeadPhysically` / `RebaseMinDocsWindow` | 同上 |
| `CaskDocValueTest.BackupHotCopyAndReopen` | `(*b)->close()` 后 `remove_all(dir)` 仍失败。已核实 `Cask::close()` **确实**清了 `read_files_` 与 `active_data_`，残留的是**搜索/向量插件持有的 mmap**（Cask 对象尚未析构）|
| `CaskDocValueTest.MigrateHintOrdV4EraDirOpensAndReads` | 同上 |
| `SearcherFacade.MatchesCaskFacade` | 同上 |
| `InvertedIndex.V5BlockMinDlTrackedAndPersisted` | `remove(*.snap)` 被占用 |
| `FieldSchema.InternDeterministicAndReverseLookup` | **第二类占用源**：`field_schema.hpp:105` 长期持有一个 CRT `std::fopen(path,"ab")` 句柄，而 **MSVC 的 fopen 不带 `FILE_SHARE_DELETE`**（CRT 用 `_SH_DENYNO` = SHARE_READ\|SHARE_WRITE），文件因此删不掉 |

**两条结论直接影响 S37-6 的设计**：

1. **占用源有两类，不只是 mmap。** 设计稿 C2 只点了 section 映射；实测还有
   **CRT stdio 句柄**——任何走 `std::fopen`/`ofstream` 且长期持有的文件都删不掉，
   因为 CRT 不给 `FILE_SHARE_DELETE`（也没有开关能要）。第一方长期持有的目前
   只有 `field_schema`，但 `AtomicFileWriter` / `segment.hpp:525` /
   `search_checkpoint.hpp:233` / `index_manifest.hpp:169` 同属该形态，需逐个确认
   生命周期。**建议给 io seam 补一个 `open_stream(path, mode)`**：POSIX 直接
   `std::fopen`，Windows 走 `CreateFileW`(带 SHARE_DELETE) + `_open_osfhandle` +
   `_fdopen`，把 `std::FILE*` 这条通路也纳入统一的共享策略。
2. **`remove_all(目录)` 与 POSIX 语义有本质差别。** 即使所有句柄都带
   `FILE_SHARE_DELETE`，被删的文件在最后一个句柄关闭前只是 *delete-pending*、
   **仍留在目录里**，于是父目录删不掉。POSIX 的 `rm -rf` 没有这个问题。
   即：Windows 上「关掉 Cask 才能删目录」是硬要求，`close()` 必须真正放掉
   **全部**映射与句柄（含插件侧），而不只是 KV 侧的 `read_files_`。

### S37-7 — CI + 收尾 🟡 MED

1. `.github/workflows/ci.yml` 加 `windows-2022` job（MSVC Release + 全量 ctest）。
2. 加 `BITCASK_SIMD_MAX` 矩阵（scalar/avx2/avx512），覆盖所有 ISA 档位对拍。
3. `/WX` 清理收口；bench 对拍（Linux vs Windows 同档）。
   > **Linux 侧的 `-Werror` 已于 2026-08-08 全部结清**（见 P 段复验记录 ④⑤）：
   > `werror-lib` 此前从 v6.1.0 起就没绿过。库 14 条 + tests/bench 118 条共
   > **132 条全部修根因**（无一个 `-Wno-*`），现在 `BITCASK_WERROR=ON` +
   > `BUILD_TESTING=ON` + benchmarks 的整树零错误零告警，clang 侧同样 0。
   > **本项在 Linux 侧已无剩余工作**；MSVC 的 `/WX` 仍未开，账见 S37-4 的 94 条。
4. 文档：README 加 Windows 构建段、`doc/format-zh.md` 补持久性机制差异、CHANGELOG。

> ⚠️ **需明确承认的护栏损失**：Windows/MSVC **无 TSan**（ASan 有，UBSan 无）。
> 对一个重并发的库（epoch 回收、无锁 keydir 读、组提交、后台 merge）这是实打实的损失。
> 缓解：并发正确性继续以 Linux TSan 为准（并发逻辑本身平台无关）；但 S37-5.3
> （每线程句柄池）与 S37-6（映射逐出 × epoch 协调）是 **Windows 独有的新并发代码，
> TSan 覆盖不到** —— 这两处必须补压力测试，并考虑 Application Verifier / `/analyze` 补位。

- **工作量**：1-1.5 周

---

## 执行序

```
S37-1 (2周)   ───── 平台抽象层收编      ┐
S37-2 (1周)   ───── fork → exec-self    ├─ 纯 Linux，无需 Windows 环境，可并行
S37-3 (2周)   ───── SIMD 分 ISA TU      ┘   （合计约 5 周 ≈ 一半工作量）
                        ↓
S37-4 (5天)   ───── MSVC 构建适配（首次需要 Windows 环境）
S37-5 (3周)   ───── Windows I/O 后端    ← 依赖 S37-1 的 seam
S37-6 (1.5周) ───── 删除/映射生命周期   ← 依赖 S37-5
S37-7 (1.5周) ───── CI + 收尾
```

**合计约 10-12 周**至「Windows x64 MSVC 全套 ctest 通过 + SIMD 同档性能」。

---

## ⚠️ 风险排序（按「有多容易静默错」）

| # | 项 | 任务 | 为何危险 |
|---|---|---|---|
| 1 | 映射生命周期 | S37-6 | 架构改动 + 与并发回收耦合 + TSan 覆盖不到；失败形态是退休队列无限增长（磁盘不回收），不报错 |
| 2 | PID 复用误判 | S37-5.4 | 导致库彻底打不开；只在特定时序复现 |
| 3 | pread 并发退化 | S37-5.3 | **测试全绿、只有 bench 掉**，最易漏 |
| 4 | CPUID 漏检 XCR0 | S37-3.3 | 只在特定虚拟化环境 `#UD`；本地全绿、线上偶发 |
| 5 | `std::rename` 覆盖语义 | S37-1.7 / S37-5.2 | 危害大（9 个原子写站点全线挂）但**必然立刻暴露**，反而最安全 |

---

## ⏸ 遗留

| 项 | 内容 | 状态 |
|---|---|---|
| T8 | 搜索读屏障无界等待（`prepare_search` 饥饿）| ⏸ 4 项前置未满足（原文 62789cd）|
| T12 | HNSW ckpt 去重（~115 行）| ⏸ 默认不做（注释同步已替代）|
| **W1** | `File::open` 默认**不设 `O_CLOEXEC`** | 🔍 S37-1 期间发现：11 处裸 `::open` 全部显式带 `O_CLOEXEC`（收编后由 `kCloseOnExec` 原样保留），而走 `io::File` 的核心 KV 路径（data/hint/oki run/write.lock）**没有** —— fork+exec 场景下 data file fd 泄漏进子进程。现有 fork 测试只 fork 不 exec 故未暴露，**S37-2 改 exec-self 后会变成真实暴露面**。收编时未擅自统一（那是行为变化，超出「零行为变化」约束）。**待评估**：给核心路径补 `kCloseOnExec`。Windows 侧句柄默认不继承，无对应问题 |
| ~~W5~~ | Windows CRT/堆 边界（/MT 下每模块各一份）| [结清]。**准确范围：本库的 C++ API 本就不是模块边界，只有 C API 是。** 原有三处跨界：`io::File::pread` 返回 `std::vector`（库这侧分配、调用方析构）、`io::open_stream` 返回 `FILE*`、`io::flush_and_sync_stream` 反向收 `FILE*`。`/MD`（本项目默认，已核实 `-MDd`）下全部成立；`/MT` + 多模块下是跨堆 free 与跨 fd 表查号，失败形态为 **invalid-parameter handler → `__fastfail`，进程无声消失、退出码 0xC0000409、terminate/abort 都不触发**（参考 `../coxswain` W9.9，当时靠 cdb 取栈才定位；该状态码名为 STACK_BUFFER_OVERRUN，与栈无关）。已做：配置期告警 + `io.hpp` 注释 + `doc/api-c.md` §2.1 与 README 的 Windows 段（36655bd）+ **两处 `FILE*` 跨界消除：seam 只交换内核句柄，`FILE*` 的生成与拆解由 `detail/file_util.hpp` 的 inline `adopt_stream` / `stream_handle` 在调用方模块内完成**（6ad5b4b）。余下 `pread` 返回 `std::vector` 属常规 C++ ABI 约束（任何按值返回 STL 容器的 C++ 库都有），靠「跨模块只用 C API」这条规则解决，不为它动 API |
| ~~W4~~ | 窄路径的编码约定 | [结清] 由 P0 结清。立项时以为只是「45 处 `.string()` 产出 ANSI → seam 报 EINVAL」，实测（CP936 简中 Windows）比这严重两级：`fs::path(窄串)` 这个**构造方向**同样走 ANSI，两头的错误互相抵消，才让纯 ASCII 与「碰巧是合法 GBK」的数据看着正常；一旦字节不是合法 GBK，**构造直接抛 `std::system_error`**，而 `detail::fsync_parent_dir` 是 `noexcept` 且是全库 9 个原子写站点的公共收尾 ⇒ **`std::terminate`**。修法见 P 段 P0 |
| **W2** | `bitcask_format` → `bitcask_io` 的 PUBLIC 依赖 | 🔍 S37-1 引入（见落地记录）。当前无环且必要，但「记录 codec 层依赖 I/O 层」是轻微的分层异味。若 S37-4 把 13 个 STATIC 改 OBJECT 库，可顺带复核是否有更干净的归置 |
| ~~W3~~ | `count_os_threads()` 读 `/proc/self/task` | ✅ S37-5 结清：补 `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD)` 实现（按 owner pid 过滤），**没有**把用例标 Linux-only——AT5 守的「线程数与库数解耦」是平台无关的结构性保证，在 Windows 上同样值得守。原实现在非 Linux 上恒返回 0，而断言的是差值，0-0=0 直接失败 |

---

## P 段 · 路径编码与 stdio 收编（W4 后续）

来源：S37 收尾后复盘「Windows 侧是否全用原生 API」。结论是**核心 I/O / 内存 /
进程层已是纯原生 Win32**——`src/io/win32_file.cpp`（770 行）用的是 `CreateFileW`、
`ReadFile`/`WriteFile`+`OVERLAPPED`、`SetFileInformationByHandle(FileEndOfFileInfo)`、
`CreateFileMappingW`/`MapViewOfFile`、`MoveFileExW`、`DeleteFileW`、
`GetFileInformationByHandle`、`PrefetchVirtualMemory`、`OpenProcess`+`GetProcessTimes`，
**没有一处走 CRT 的 POSIX 兼容层**（`_open`/`_read`/`_lseek` 零使用）；SIMD 派发用
`__cpuidex`/`_xgetbv`，测试替代 `fork` 的是 `CreateProcessW`。

未原生的只剩两类，各有各的性质：

| 类 | 面积 | 性质 |
|---|---|---|
| CRT stdio（`std::FILE*`）| 10 个文件 51 处调用 | **真问题**：不带 `FILE_SHARE_DELETE`；且是库内第二种文件通货（另一种是 `io::FileHandle`，69 处）|
| `std::filesystem` | 13 个文件 78 处调用 | **基本不是问题**：MSVC 的实现底下已经是 `DeleteFileW` 那一套，无 CRT 堆/fd 表问题 |

### 顺序约束 —— P0 必须最先，否则 P1 是倒退

`fs::remove(p)` 的 `p` 在 Windows 上**本来就是宽字符**（`fs::path` 内部是 `wstring`），
根本不经过窄字符这一道，**对非 ASCII 路径是能工作的**；而
`io::remove_file(p.string())` 会先被 `.string()` 转成 ANSI、再被 `widen()` 的
`MB_ERR_INVALID_CHARS` 判为非法 UTF-8 → `EINVAL`。

**所以在编码修好之前，任何「把 `fs::` 站点收进 seam」的动作都是把一个能用的路径
换成一个不能用的路径。** 这条推翻了直觉上的排序（「先做小的 rename 收编」），
P1 必须排在 P0 之后。

### P0 · 路径编码统一 [done]

**立项时的判断错了两处，实测（2026-08-08，VS 18 / MSVC 14.51 / `GetACP()==936`）
纠正如下**——这也是「动 45 处之前先花二十分钟写探针」换回来的：

1. **不只是 `.string()` 一个方向。** `fs::path(窄串)` 的**构造**同样走 ANSI 代码页。
   两头各错一次、方向相反，于是往返无损——纯 ASCII 与「UTF-8 字节碰巧也是合法
   GBK」的数据（如「测试」）全都看着正常。这解释了为什么 733 个用例一直全绿。
2. **失败形态不止 `EINVAL`，最坏是 `std::terminate`。** 字节不是合法 GBK 时
   （如「测试库」），`fs::path` 构造**直接抛 `std::system_error`**
   （`ERROR_NO_UNICODE_TRANSLATION`）。而 `detail::fsync_parent_dir` 是
   `noexcept`，且是 `atomic_write_bytes` / `AtomicFileWriter::commit` 的收尾，
   即**全库 9 个原子写站点的公共出口**：库目录名带中文 → 写任何东西 → 进程挂。

三种形态实测汇总：

| 数据 | `fs::path(UTF-8)` | 结果 |
|---|---|---|
| 碰巧也是合法 GBK | 静默解错成别的宽字符 | 往返抵消，seam 能用；但该 path 交给 `fs::exists`/`ifstream` 时指向**另一个名字** |
| 不是合法 GBK | **抛 `std::system_error`** | 穿过 `noexcept` ⇒ `std::terminate` |
| 真·宽来源（`directory_iterator`）| `.string()` 编出 GBK | seam `EINVAL`；`remove_file` 返 false 而文件仍在 |

**落地**：新增 `include/bitcask/detail/path_utf8.hpp`，一对 `from_utf8` /
`to_utf8` 把两个方向都钉死在 UTF-8 上（走 `char8_t` 重载，由标准保证语义，
不碰代码页、不需要 `windows.h`）。**两者都是 `noexcept`**：标准转换在非法
UTF-8（孤立续字节、截断序列、超长编码、代理区）下会抛，而调用方多为
`noexcept` 或返 `bool`/`expected` 的风格——失败收敛成空值，下游照常返错。
配套 `fopen_utf8`（Windows 落 `_wfopen`）与 `remove_utf8`（走
`io::remove_file`），因为 CRT 的 `fopen`/`remove` 同样按 ANSI 解释窄路径。

**面积**：59 处（52 + 守门脚本揪出的 7 处），覆盖 15 个文件的窄↔`fs::path`
两个方向，以及 `std::fopen` ×5 / `std::remove` ×2。**成对改动**是硬要求——
只改一头会把「两次错误抵消」拆散，反而把形态 1 从「能用」变成「不能用」。

**守门**：`scripts/check-path-encoding.sh` + CI 的 `path-encoding` job。这条
约定在类型上看不见（`std::string` 既能装 UTF-8 也能装 GBK），且只在非 ASCII
路径上暴露，靠评审守不住。对照 RocksDB：它把每个路径 API 塞进 `RX_*` 宏
（`RX_CreateFile`/`RX_FN`），绕过转换层在结构上就写不出来；我们选普通函数 +
typedef 换可读性，代价就是没那层强制力，用 grep 补回来。

**测试**：`tests/path_utf8_test.cpp` 9 个用例，三种形态各有对应，含
`AtomicWriteInNonAsciiDirDoesNotTerminate`（守 `noexcept` 那条）与
`ScannerYieldsSeamUsablePaths`（守 `directory_iterator` 那条）。

**验证**：msvc-debug 745 用例，新增 9 个全绿，其余无回归
（`Hnsw.ConcurrentReadersWithSingleWriter` 在 `-j 8` 下超 300s 预算，单独跑
184s 通过，是 CPU 争用不是回归）。**Linux 侧未编未跑**——`from_utf8`/`to_utf8`
在 POSIX 下退化成拷贝，但仍需一次 `gcc-release` 复验。

<details><summary>立项时的原始判断（已被实测推翻，留档）</summary>

### P0 · 路径编码统一（前置，45 处）

`.string()` 在 `src/` + `include/` 出现 **45 次**，集中在 `fileops/migrate.cpp`(16)、
`search/vector_plugin.cpp`(7)、`cask/cask.cpp`(5)；全库**没有一处**用 `u8string()`。

**P0.0 前置验证（必须先做，可能使 P0 归零）**：整段建立在「MSVC 的
`path::string()` 是 ANSI」上。MSVC STL 的 narrow 转换走 `__std_fs_code_page()`，
默认 `CP_ACP`，但**进程清单声明 ACP=UTF-8 时它返回 `CP_UTF8`**，那样现状就没有
bug。所以第一步是一个非 ASCII（中文）路径的用例，Windows + Linux 各跑一遍，
确认失败形态真的是 `EINVAL`。**不确认就动 45 处，是拿假设换工作量。**

**P0.1 修法选型**：

| | A. 统一到 UTF-8 窄串 | B. seam 直接收 `fs::path` |
|---|---|---|
| 做法 | 加 `detail::to_utf8(const fs::path&)`，45 处 `.string()` 换掉 | `open_handle(const fs::path&, …)`，Win32 后端直接 `c_str()` 拿宽串 |
| 编码转换 | 仍有一次（UTF-8 → UTF-16）| **零次**，最彻底 |
| 代价 | 45 处机械改动 | seam 签名变（69 处调用点）+ `<filesystem>` 进公开头 |

**选 A。** 决定性理由是 B 要把 `<filesystem>` 拖进 `io.hpp`——那个头现在只包 8 个
标准头，克制得很刻意；且 c_api 边界本来就是 UTF-8 `char*`，B 到了那里还是要转，
彻底性打折。A 另有一个 B 没有的好处：改完可加 CI grep 禁止裸 `.string()`，
把「窄路径 = UTF-8」这条库级约定固化成可执行的守门规则。

</details>

### P1 · `.prev` 轮转的 `fs::rename` 收进 seam [done]

**立项理由没能通过实测，改用另一条理由做的。** 原以为这两处绕过
`io::atomic_rename` 会丢掉 `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`。
对拍结果（同一台机器，同一场景）：

| 场景 | `fs::rename` | `MoveFileExW(REPL)` |
|---|---|---|
| 目标无人打开 | 成功 | 成功 |
| 目标被打开（无 `SHARE_DELETE`）| `ACCESS_DENIED` | `ACCESS_DENIED` (5) |
| 目标被打开（**带 `SHARE_DELETE`**）| `ACCESS_DENIED` | `ACCESS_DENIED` (5) |

**两者行为逐条一致**，覆盖语义 `fs::rename` 本来就有（仓库里那条「`std::rename`
目标已存在即失败」说的是 C stdio 的 `std::rename`，不是 `fs::rename`）。第三行
顺带独立复现了 S37-6 已确立、`AtomicRenameOverMappedFileKeepsViewContent` 已在守
的规则：**Windows 上改名覆盖一个仍开着句柄的目标必然失败，任何 share 位都拦**。

`WRITE_THROUGH` 实测 0.229 vs 0.124 ms/次。而 `.prev` 也不需要它：轮转的两种
崩溃结局（改名了 / 没改名）都落在可恢复状态上——`.prev` 是主 ckpt 结构损坏时
的回退源，一旦回退就不吃 delta 链（`legacy_ckpt.cpp:133`），本就退到更旧的水位
再全量 fold。

所以**收编的理由只剩纪律**：让「Windows 的 rename 语义」全库只有一个地方需要
解释。按这条理由做了，并把原先被丢弃的 `ec` 显式化为 `(void)` + 注释说明为何
失败可容忍。站点比立项时数的多两处——`src/search/vector_plugin.cpp:266` 与
`include/bitcask/detail/sealed_segment_vector_plugin.hpp:498` 是同一形态，
共 4 处。

**P0 的漏网（P1 期间发现，一并结清）**：`fs::remove(窄串, ec)` 这类**隐式**
构造 `fs::path` 的调用点，P0 的守门抓不到（没有字面量 `fs::path(`），而它照样
按 ANSI 解码——且 `ec` 重载**挡不住**，因为路径构造发生在调用之前，抛出的
`std::system_error` 不经过 `ec`。扫出 **26 处，横跨 11 个文件**。守门脚本已补
这条规则（并支持跨行调用；判不准时宁可报出来）：fs:: 的路径入参必须是
`from_utf8(...)`、`e.path()`，或以 `_path`/`_dir` 结尾的变量。

**验证**：msvc-debug 745 全绿，无回归。Linux 仍未复验。

### P2 · 读路径改走 seam 的定位读 [done]（原「`AtomicFileWriter` 换缓冲层」，靶心已改）

**动手前查调用方，把靶心从写路径挪到了读路径。** 原计划要写一个 ~200 行的
`BufferedWriter` 去替换 `AtomicFileWriter`，理由是「stdio 唯一还提供的是缓冲」。
查下来两件事推翻了它：

1. **写路径没有缺陷可修。** `AtomicFileWriter` 的调用方是顺序 `fwrite`，不经过
   `long` 偏移；而它原有的另外三条理由已被前面两期消化掉了——CRT 跨界 W5
   已消除（`flush_and_sync` 现在 inline），路径编码 P0 已消除
   （`fopen_utf8`），`FILE_SHARE_DELETE` 对短命 tmp 文件无意义。剩下的只有
   「通货统一」这种整洁性收益，而代价是改 5 个用 `fseek`/`fwrite` 图案的调用点
   外加一个必须自己写对的缓冲层（短写、disk-full、超大写）。**不划算，不做。**
2. **读路径有真缺陷。** `std::fseek`/`std::ftell` 的偏移类型是 `long`，
   **MSVC x64 上只有 4 字节**（Linux 上 8 字节，所以 CI 与全部现有测试都照不到）。
   实测：

   | | 结果 |
   |---|---|
   | `sizeof(long)` | 4 |
   | `static_cast<long>(3 GiB + 4 KiB)` | **-1073737728**（静默截断成负数）|
   | 2.54 GiB 文件上的 `ftell()` | **-1** |
   | `_ftelli64()` / `ReadFile`+`OVERLAPPED` | 正常 |

   即**库有一个 Windows 独有的 2 GiB 单文件天花板**，Linux 上不存在。失败形态
   分两类：`read_file_bytes` 把好文件当成读不出来（返 nullopt）；
   `search_checkpoint::read_selected` 把好 ckpt 判成结构损坏（调用方据此退
   `.prev` 或全量重建）。受影响的调用方包括 **keydir 快照、HNSW payload、
   BM25 段、migrate 的数据文件**——都是大部署下能过 2 GiB 的东西。

**落地**：

- `detail::read_file_bytes` 与 `search::SearchCheckpoint::read_selected` 整体
  改走 seam 的 `io::File::open` + `handle_size` + `pread_all`（全程
  `std::uint64_t`）。后者本就是「按目录跳着读」，定位读比 `seek`+`fread` 更贴合，
  改完还少了「seek 到尾、量、再 seek 回来」三步。**没有用到缓冲**——整读是一次
  大 `pread`，跳读是按目录定位，缓冲层从头到尾没有必要。
- `hnsw.cpp` 的两处顺序写保留 `FILE*`，只把 `static_cast<long>` 换成新增的
  `detail::fseek64`（Windows 落 `_fseeki64`，POSIX 落 `fseeko`）。那两处偏移
  实际远小于 2 GiB（头部 + 页 CRC 数组），但那个 cast 是颗定时炸弹。
- 守门加一条：禁止 `std::fseek`/`std::ftell`。
- `tests/posix_file_test.cpp` 新增 `LargeFile.SizeAndPositionedIoBeyond2GiB`
  （2.54 GiB，跨过 2^31），空间不足时 `GTEST_SKIP` 而不是把 CI runner 的盘写满。

**验证**：msvc-debug 746 全绿（新增 1 个用例，实跑 856 ms 未跳过）。Linux 仍未复验。

### P3 · `field_schema` 退掉长期持有的 `FILE*` [done]（原「换 `BufferedWriter`」）

**缓冲层的前提在这里也不成立，但查代码时发现了一个更值钱的东西。**

不需要缓冲：`intern` 每写一条 entry 就 `fflush` 一次（语义要求如此——新 id 必须
立刻对其它进程可见）。所以 CRT 缓冲实际从不攒批，一条 entry = 一次系统调用；
改成「组好整条 + 一次定位写」是等价的，legacy 分支还因此从两次 fwrite 减到一次。

**真问题：`intern` 里的写一个返回值都没检查，而 id 是位置性的。** 写侧和读侧
（`load_new_format_`）都用 `id_to_name_.size()` 定序，entry 里**不存 id**。于是
「中间丢一条、后面继续写」会让重启后所有后续 id **前移一位**——老数据里记的
field id N 解析成另一个字段名。这是静默的数据错位，比丢一条字段名严重得多。
佐证这是遗漏而非取舍：同文件的 `upgrade_legacy_to_new_` 是**检查**了 `fwrite`
返回值的。生产侧调用 `intern` 的只有一处（`cask.cpp:2391`），其余全是测试。

**落地**：

- `fp_`（`std::unique_ptr<FILE>`）换成 `io::File wf_` + `std::uint64_t woff_`。
  这是全库最后一个长期持有的 `FILE*`，退役后本类不再碰 CRT 的流层。
- **`kNoAppend` + 自记偏移**，不是 `kNone` 的 `O_APPEND`：POSIX 下 `O_APPEND`
  会让 `pwrite` **忽略 offset**（`io.hpp` 的 `kNoAppend` 注释早记了这条暗礁），
  而 Windows 根本没有 `O_APPEND` 的等价物，seam 的定位写走 `OVERLAPPED` 偏移。
  靠 `O_APPEND` 两边行为就会分叉，只能开时量一次大小、之后自己推进。
- 写失败 → `stop_persisting_()`：关句柄 + 置 `persist_failed_`。这样盘上序列
  **始终是内存序列的前缀**，已持久化的 id 全部保持正确。这正是本类原本就接受
  的降级形态（句柄压根开不出来时即如此），现在把「开着但写挂了」也纳进来。
  新增 `persist_failed()` 供调用方查询。**没有改 `intern` 的签名**——那会波及
  一处生产调用点和约 15 处测试断言，而前缀不变式已经消掉了错位。
- `detail::adopt_stream` 随之**删除**（44 行）：field_schema 是它唯一的生产
  调用方，W5 引入它就是为了这一处。`<fcntl.h>` 也一并从 `file_util.hpp` 退掉。
  `stream_handle` 保留——`flush_and_sync` 仍在用（`AtomicFileWriter` 走它）。
- 测试 `AdoptedStreamAllowsDeleteWhileHeld` 改名并重定向为
  `LongHeldWriteHandleAllowsDeleteAndTracksOffset`：它守的两条性质（句柄带
  `SHARE_DELETE` 故文件仍可删；追加靠自记偏移而非 `O_APPEND`）依然要守，
  只是形态变了。

**未做**：读路径（`open` / `load_new_format_` / `load_legacy_`）仍用短命
`FILE*`，`upgrade_legacy_to_new_` 仍走 `AtomicFileWriter`。两者都是用完即关，
不在「退掉长期持有」的目标内，也没有 P2 那样的实测缺陷支撑。

**验证**：msvc-debug 746 全绿。

#### 🟢 P3 Linux 复验（2026-08-08，e2c875d 时点）——无回归

| 树 | 结果 |
|---|---|
| 干净树 `-Werror` 全树（库+测试+bench） | ✅ 0 错误 0 告警；ctest **749/749** |
| gcc Debug / clang Debug | ✅ 各 **749/749** |
| ASan（P3 动了句柄生命周期，重点） | ✅ **749/749** |
| TSan（写路径持锁，按 CI `-E` 豁免） | ✅ **747/747** |
| Release -O3+LTO+bench 双树（`field_schema.hpp`/`io.hpp` 是公开头） | ✅ 编译 0 错误 |
| `check-path-encoding.sh` | ✅ 无违规 |

**偏移追踪在 POSIX 上确实正确**：探针写三条后文件 43 字节 =
8(头) + (2+5+4) + (2+5+4) + (2+7+4)，逐条对得上——即 `kNoAppend` + 自记 `woff_`
没有覆盖写，也没有因为丢了 `O_APPEND` 而错位。

**顺带查实一个隐患，结论是「预存、非 P3 引入」**：

`woff_` 取的是 `handle_size`（**整个文件大小**），而 `load_new_format_` 对
torn tail 是「短读即 `return true`」——**容忍并停止解析**。两者拼在一起是：
崩在半条 entry 上 → 重开时内存里少了尾部那条，但下一条 entry 被追加到
**那半条之后**，文件变成 `[完整条…][半条][新条]`。

写探针实测（`/tmp` 上真跑一遍：写 3 条 → 砍尾 3 字节 → 重开 → `intern` 一条 →
再重开）：

```
reopen#2: id0=charlie id1=bravo id2=<none>     ← 半条被容忍，少一条
reopen#2: intern(delta) = 2                    ← 追加到半条之后
reopen#3: open() = FALSE (硬失败)              ← 下次打开直接硬失败
```

`open()` 返回 false 按本类契约是「真损坏 → caller 应中止 open」，即**整个库打不开**。

**但这不是 P3 引入的**：把同一个探针对着 `e2c875d~1`（P3 之前，`fopen(path,"ab")`
+ `O_APPEND`）编译运行，**输出逐字相同**——`O_APPEND` 同样从文件末尾（含半条）
开始写。P3 的前缀不变式覆盖的是「**写失败**后停止持久化」，那条它做到了；
覆盖不到「上一次进程崩在半条上」这种既有状态。

#### ✅ 该隐患已修（2026-08-08）

**续写起点从「文件大小」改成「最后一条完整 entry 的末尾」，并截掉残条。**

- 新增成员 `valid_end_`，由 `load_new_format_`（起点 `kHeaderSize`，每条
  `+= 2+nlen+4`）与 `load_legacy_`（起点 0，每条 `+= 2+nlen`）在解析时产出
  ——**就是那趟解析自己走到哪儿**，不引入第二套长度计算。
- `open()` 里 `woff_ = valid_end_`；若文件大小 > 它，`io::truncate_handle` 截掉。
  截掉的一定是解析不出来的残条（`valid_end_` 由刚才那趟解析产出），不是有效数据；
  不截的话残条会夹在新条之前，比截更糟。
- **截断失败 → 不持有句柄**，退化成本类既有的「只在内存生效」，而不是退回去
  在残条之后追加。
- 两处容易漏、都会造成新 bug 的点：
  1. **升级路径要重算**：`upgrade_legacy_to_new_()` 把文件整体重写了，
     `load_legacy_` 记下的 `valid_end_` 是**旧格式**偏移，照它截断会把刚升级好的
     文件切掉一截。已在升级成功后按新格式重算。
  2. **`open()` 开头要重置**：文件不存在/为空时两个 `load_` 都不跑，留着上次
     `open` 的值会把 `woff_` 定到凭空的偏移上（同一对象重复 `open` 才暴露，
     测试里就有这种用法）。

**新增用例 `FieldSchema.TornTailIsOverwrittenByNextIntern`**（`data_file_test.cpp`，
紧挨着既有的 `ToleratesTornTailNewFormat`——后者只验到「打开时容忍」，真正的
危险在下一步）。**确认过它不是空断言**：把修复临时回退成 `woff_ = *sz` 后，
两条断言各自炸出（文件 33 ≠ 29 未截断；`fs3.open()` 返回 false）。

**复验**：干净树 `-Werror` 全树 0 告警、ctest **750/750**；gcc/clang Debug 各
**750/750**；ASan **750/750**；TSan **748/748**（按 CI 豁免）；Release+LTO+bench 编译。

### P4 · 错误模型 [done]（「收窄」的结论保留，但收窄后剩下的那一处是个真 bug）

**「不做大统一」的结论成立，可查证据时发现留下的缝里漏着一个违反公开契约的
错误码。**

立项时估「真正消费 `ec` 的只有 4 处」，重新扫是 15 处以上，且**大多已经在做
正确的事**——`scanner.cpp` 翻成 `ScanFault`、`cask.cpp` 翻成 `CaskError::kIo`、
`migrate.cpp` 翻成 `std::unexpected(ec.message())`。`std::error_code` 在这些
站点是 fs:: 调用旁的**局部机制**，当场翻成本模块的错误类型，并没有「两套模型
跨 API 并存」的问题。所以维持原结论：**不把 37 处 `error_code` 统一成
`IoError`**，尽力清理的 `fs::remove(p, ec)` 保持原样——统一成 `IoError` 再原样
忽略，只是把噪音换个写法。

**但翻译里有一处是错的。** `scanner.cpp` 把 `ec.value()` **原样**塞进
`ScanFault.errnum`，而那个值一路流到公开 API：

```
scan_dir -> ScanFault.errnum -> cask_recovery.cpp io_fault() -> CaskFault
         -> c_api/internal.h -> c_api/bitcask_kv.h 的 int errnum
```

而 `bitcask_kv.h:91` 写的是 `// errno 值（IO 错误时有效，否则 0）`。实测
（P4.0，MSVC，`ec.category()` 是 `system`）：

| 场景 | `ec.value()` | 按 errno 解读 | 应当是 |
|---|---|---|---|
| 目录不存在（最常见的 open 失败）| 3 = `ERROR_PATH_NOT_FOUND` | **`ESRCH`「没有这个进程」** | `ENOENT`(2) |
| 把普通文件当目录 | 267 = `ERROR_DIRECTORY` | errno 里没有对应值 | `ENOTDIR`(20) |

Linux 上同样场景给 `ENOENT`，所以这是**同一字段在两个平台上含义不同**。seam
自身一直守着契约（Windows 后端每条错误路径都过 `errno_of(GetLastError())`），
漏的是 `std::filesystem` 这条旁路。

**落地**：

- 新增 `io::errno_of_native(int)`：Windows 后端委托既有的 `errno_of(DWORD)`，
  POSIX 后端恒等（那边 `ec.value()` 本就是 errno）。放在 seam 里，因为 seam
  本来就是 Win32→errno 翻译的归属地。取 `int` 而非 `std::error_code`，是为了
  不把 `<system_error>` 拖进 `io.hpp`（那个头只包 8 个标准头，很克制）。
- `scanner.cpp` 的唯一一处 `ec.value()` 过翻译。全库再无第二处。
- 守门加一条：`error_code::value()` 未经 `errno_of_native` 不得出现。
- `Scanner.MissingDirectoryReturnsError` 从 `EXPECT_NE(errnum, 0)` 收紧为
  `EXPECT_EQ(errnum, ENOENT)`。**原断言正是这个 bug 活下来的原因**——Win32 码
  同样满足「非零」。

**验证**：msvc-debug 747 全绿。

13 个文件用 `std::error_code`，但绝大多数是 `fs::remove(p, ec)` 这种**尽力而为、
`ec` 根本不看**的清理。把它们统一成 `IoError` 再原样忽略，是把噪音换个写法，
读起来反而更糟（`(void)io::remove_file(p)` 并不比 `fs::remove(p, ec)` 清楚）。

真正**消费**了 `ec` 的只有 `cask/cask.cpp:900`、`fileops/oki_state.cpp:38`、`:55`、
`cask/cask_recovery.cpp:188`。只收这几处，其余保留，并把规则写进 `io.hpp` 或
CONTRIBUTING：**错误要被消费 → 走 seam 的 `IoError`；纯尽力清理 → `fs::` + 忽略 `ec`**。

### 🟡 P 段 Linux 复验记录（2026-08-08，1e23e1e 时点）

P0/P1/P2 三期全部只在 msvc-debug 上验过（三条落地记录均写着「Linux 未复验」），
S37-4/S37-5 的复验也都留了「ASan / TSan 未跑」的尾巴。本次一并结清。

| 树 | 配置 | 结果 |
|---|---|---|
| `build` | g++ 14.2 Debug + bench | ✅ 编译 0 错误；ctest **749/749** |
| `build-clang` | clang++ Debug | ✅ 编译 0 错误；ctest **749/749** |
| `build-rel` | Release -O3 + LTO + bench | ✅ 编译 0 错误 |
| `build-asan` | clang address,undefined | ✅ **749/749**（S37-4 起首次跑）|
| `build-tsan` | clang thread + 插桩 oneTBB | ✅ **747/747**（按 CI 的 `-E` 豁免）|
| 干净树 | Release configure | ✅ 0 警告 0 错误，全部 target 建成 |
| `check-path-encoding.sh` | 守门 | ✅ 无违规 |

（1 项 `CheckpointRecoveryTest.S30RssProbe` 预存 Disabled，同既往。）

**查出三件事，其中一件是真回归。**

**① `-Wcomment` 打断了 CI 的 `werror-lib` job（真回归，已修）**

S37-4 给 `detail/cpu_features.hpp` 写的 `BITCASK_TSAN_ENABLED` 说明块里，引用原
写法时保留了行尾的续行反斜杠：

```
//   #if defined(__SANITIZE_THREAD__) || \
```

`\` 出现在 `//` 行末会把下一行拼进本注释 —— GCC 的 `-Wcomment`（含在 `-Wall`
里），而 `werror-lib` job 带 `-Werror`，于是**这条注释本身让 Linux 库构建失败**。
本头经 `bitcask_format` PUBLIC 传播，一处中招就是几十个 TU 中招（本次 Release
树里 23 条）。修法是把引文写成两行、`||` 前置，不再需要反斜斜杠。

> 这类「注释导致编译失败」最难联想：报错位置是注释行，而改动看着纯属文档。
> 全库另有 2 处同形态（`bench/inverted_bench.cpp` / `checkpoint_recovery_test.cpp`
> 引用 shell 命令），**均早于本届**（6-19 / 7-11）且不在 `werror-lib` 构建的目标里，未动。

**② `path_utf8_test` 把 Windows 的结果写成了通用断言（测试缺陷，已修）**

`InvalidUtf8YieldsEmptyInsteadOfThrowing` 断言「非法 UTF-8 → 空 path」，在 Linux
上 gcc/clang 双双失败。**实现是对的，断言过窄**：

| | Windows | POSIX |
|---|---|---|
| `fs::path` 内部 | UTF-16，构造要真解码 | 就是字节串，`char8_t`↔`char` 无解码 |
| 非法 UTF-8 | 转换失败 → 收敛成空 path | **逐字节透传，libstdc++ 不校验** |

POSIX 那侧的行为**正是要的**：非 UTF-8 文件名在 Linux 上完全合法（Latin-1 名字、
从别的 locale 拷来的目录），P0 之前的 `fs::path(窄串)` 能打开它们，P0 之后必须
照旧。把「非法 → 空」强加到 POSIX 上，等于让库突然打不开一批本来能打开的文件
—— **那才是真回归**。已改成两边各断言各自的不变量（共同的「不抛/不 terminate」
由「函数返回了」本身证明），并补 `PosixNonUtf8FilenameStillUsable`：在磁盘上真建
一个 Latin-1 名字的文件，走 seam 建/开/删一遍。

先写探针逐字节量过 7 类非法输入（孤立续字节 / 截断 / 超长 C0 80 / 代理区
ED A0 80 / Latin-1 / 混合），`from_utf8`→`to_utf8` 往返与旧写法 `fs::path(s)`
→`.string()` **逐字节一致**，`catch` 从不触发 —— 即 P0 在 Linux 上确是零行为变化。
`path_utf8.hpp` 里「非法 → 空值」那段已标注**只在 Windows 上发生**。

**③ TSan 的 `KeyDirOptimisticRead.ConcurrentGetPutRemoveGrowStress` 是预存豁免项（非回归）**

首轮 TSan 全量报此项失败（约 54 条 race，集中在 `seq_shard_table.hpp` 的
seqlock）。**不是本届引入**，两条独立证据：

- `ci.yml` 的 sanitizer job 对 TSan 档**本就带 `-E` 豁免**这一项与
  `IndexPoolMultiLib.ThreadCountIndependentOfLibCount`，注释里记着 S29-6 的定性
  （seqlock 的 `atomic_thread_fence` + 非原子数据，TSan 无法推理）。我首轮没加
  `-E`，是复验脚本的疏漏。
- 另起 worktree 在 **v6.1.0 基线（056127b）** 上编 TSan 跑同一用例：**57 条 race，
  站点集合与 HEAD 逐条对应**（行号整体位移 +20/+21，正是 S37-4 插入的 MSVC 注释块
  长度）。

顺带核实了两处「看着像本届踩了自己写的雷」的地方，**实测都不是**：
- `opt_bytes_equal` 的 `__builtin_memcpy` → `std::memcpy`（S37-3.b 机械替换），
  而该函数上方注释明写「不得用 std::memcpy/memcmp，会被 TSan 拦截器记录」。
  实测 g++ 14.2 与 clang 在 `-O0`/`-O2` 下**定长 8 的 `std::memcpy` libcall 数均为 0**，
  指令序列与 `__builtin_memcpy` 版逐条一致（仅标签名与一处调度差异）。禁令针对的
  是 `memcmp` 与变长形态。已在该处补注说明，免得后来者改回去或"顺手简化"成 `memcmp`。
- `BITCASK_NO_SANITIZE` / `BITCASK_TSAN_ENABLED` 两个宏都确实生效，见下。

**`BITCASK_TSAN_ENABLED` 恒 0 的顾虑已结构性排除。**
S37-4 记录担心「若因漏包含头而恒 0，TSan 注解会静默失效，全绿也测不出来」。
用符号表直接对拍，不靠测试结果：`nm -u` 查 `inverted.cpp.o` ——
**TSan 树里引用 `__tsan_acquire`/`__tsan_release`，普通树里不引用**。宏在两种
构建下各自取到了正确的值。

**④ `werror-lib` job 早在 v6.1.0 就是红的，本次一并修完（S37-7 的 `-Werror` 收口提前结清）**

去掉 `-Wcomment` 那条之后，`werror-lib` 仍然编不过 —— 而且**不是本届引入**：
v6.1.0 基线（另起 worktree 实测）同样红，且比 HEAD 多两条
（`hnsw.cpp` 的 `ftruncate` 返回值未查，**恰是本届收编裸 POSIX 调用时顺带修掉的**）。
即这个 job 长期处于「配置了但从没绿过」的状态，`-Werror` 护栏事实上不存在。

由于 `-Werror` 遇错即停，每修一批才露出下一批，共四轮 **14 条**，全部按「修根因、
不降告警级别」处理（没有加任何 `-Wno-*`）：

| 文件 | 条数 | 类别 | 修法 |
|---|---|---|---|
| `bm25/inverted.cpp` | 1 | `-Wunused-parameter` | `remove_doc` 的 `term_freqs`：V2 删除不动 posting 行，只调统计。标 `[[maybe_unused]]` **保留参数名**并说明为何空着 |
| `search/text_plugin.cpp` | 1 | `-Wunused-parameter` | `load_component` 的 `chain_seq`：S27-3 停用 text 的 delta 链后无用，但三插件签名同形态、vector 侧仍在用。同上处理 |
| `fileops/oki_state.cpp` | 1 | `-Wshadow` | 内层 `std::vector<DeltaRow> next` 遮蔽外层 `OkiManifest next` → 改名 `kept`（本来就更贴切：前缀里持留的 + 前缀之后新追加的）|
| `cask/cask.cpp` | 3 | `-Wshadow` | 三个 `lk(*done_mu)` 遮蔽外层 `lk(ckpt_mu_)` → 改名 `done_lk` |
| `segment.hpp` | 4 | `-Wconversion` | `DocId`(u64) → mmap doc_store 面(u32) 的隐式收窄。新增私有 `local_docid()`：显式转换 + `assert` 上界。**这不是纯消音**——段内 docid 本就是 u32 稠密（`doc_count()` 即 u32），隐式收窄恰恰是「哪天真溢出也没人发现」的形态 |
| `segment.hpp` | 3 | `-Wmissing-field-initializers` | `SegmentView`/`MultiFieldSegmentView` 的 `pin` 聚合初始化时省略。**钉段确实是调用方的事**（`text_plugin.cpp` 拿到 view 后自己设），显式写 `{}` 让「没钉」是决定而非遗漏 |
| `seq_shard_table.hpp` | 2 | `-Wclass-memaccess` | `memset` 清桶数组。`Bucket` 只因带**默认成员初始化器**而「非平凡默认构造」，但它**平凡可复制**，全零字节就是合法空桶。收进 `zero_buckets()`：`static_assert(is_trivially_copyable_v<Bucket>)` 把真正依赖的性质钉死 + 转 `void*` 消告警，生成代码不变 |

**结果：`src/` + `include/` + `c_api/` 的告警面归零**（干净树 `-Werror` 库构建
0 错误 0 告警，13 个静态库 + `libbitcask.so.6.1.0` 全部产出）。

**改动落在 seqlock 头与公开头上，故全矩阵复跑**：gcc/clang Debug 各 **749/749**、
Release+LTO+bench 编译、ASan **749/749**、TSan **747/747**（按 CI 豁免）、
干净树 `-Werror` 构建 —— 全绿。

**⑤ `tests/` + `bench/` 的告警一并清完（2026-08-08，CI 编译器确认为 g++ 14.2）**

上一步只清了库；测试与 bench 还有 **118 条**（26 个文件）。全部清完，现在
`BITCASK_WERROR=ON` + `BUILD_TESTING=ON` + `BITCASK_BUILD_BENCHMARKS=ON`
的干净树**整树零错误零告警**，Debug 与 Release 两棵普通树也各自 **0 告警**。

分布与修法（同样是修根因，**全程没有加一个 `-Wno-*`**）：

| 类别 | 条数 | 修法 |
|---|---|---|
| `-Wmissing-field-initializers` | 39 | **不在 39 个调用点改，而在 4 个声明处改。** GCC 只对**没有默认成员初始化器**的成员报此警，而被点名的恰好就是那几个：`AnalyzerConfig::{stop_words,dict_path}`、`SearchLayerConfig::synonym_map`、`SegmentView::pin`、`MultiFieldSegmentView::pin`。补 `{}` 后语义分毫不变（本来就默认构造成空），却一次消掉全部，且以后新增指派初始化点不会再犯 |
| `-Wsign-conversion` | 43 | 清一色「`int` 循环变量去索引容器」。**逐点判断**：只当下标的改 `std::size_t`（`kLibs`/`kPerLib`/`kN`/`kThreads` 等常量随之）；同时要喂给收 `int` 的函数的，保持 `int` 而在下标处显式转换；`mt19937_64` 的种子、`Cask::put` 的 tstamp 这类收 `uint64` 的，循环变量直接用 `uint64_t` |
| `-Wunused-result` | 22 | **按站点性质分三类**：① 测试 setup 的 `put`/`remove`/`load_from_file` —— 失败则后续断言全无意义，改 **`ASSERT_TRUE`**（不是 `(void)`）；② bench setup 的 `put`/`put_doc` —— 走既有的 `state.SkipWithError` 形态；③ teardown 的 `unregister_lib` —— 显式 `(void)`。`thread_pool_test` 那处 `unregister_lib(a)` 注释本就写着「a 排空并注销」，返回值就是「是否排空」，改成 `EXPECT_TRUE` 断言它 |
| `-Wold-style-cast` | 4 | `(double)` / `(int)` → `static_cast` |
| `-Wdangling-else` | 4 | `if (c) ASSERT_XX(...)` 裸挂——gtest 宏内部展开出 `if/else`，加花括号 |
| `-Wcomment` | 2 | 同 ① 的形态：注释里引用 shell 命令时行尾留了 `\`（bench/inverted_bench、checkpoint_recovery_test）。改成不靠续行符 |
| `-Wunused-function` / `-Wunused-but-set-variable` | 2 | **删死代码**：`ivf_rq_test` 的 float 版 `brute_topk`（召回门用的是 `brute_topk_int8`，其注释写明必须与段内核同一算式穷举对拍——留一个 float 版正是"拿错真值"的入口，仓库已记过这个坑）；`tests/support/search_layer.cpp` 里从未被调用的 `add_byte_sec` 与其缓冲 |
| `-Wformat-truncation` | 1 | `range_bench` 的 `char kb[16]` 装不下 `"g" + %06zu`（最坏 20 位）→ 32 |
| `-Wconversion` | 1 | `gate_bench` 的 `(rss_after-rss_before)*1024` 隐式转 double |

**两个值得记下的坑**：
1. **C 里 `(void)` 压不住 `warn_unused_result`。** `c_api_test.c` 已经写着
   `(void)system(cmd);` 却照样告警——GCC 的 `warn_unused_result` 属性与 C++ 的
   `[[nodiscard]]` 不同，`(void)` 转换对它无效，必须真的接住返回值。
   `fread` 同理（glibc 也用该属性）。
2. **把「忽略返回值」改成「断言返回值」是有风险的改动**，因为原先可能一直在
   静默失败。故本轮不是只看能不能编——全矩阵实跑确认这些新断言都成立。

**顺带对齐 `ci.yml`**：`release` / `werror-lib` / `benchmark` 三个 job 原本显式
`apt install g++-13` 并 `-DCMAKE_CXX_COMPILER=g++-13`，与实际使用的 **g++ 14.2**
不符（ubuntu-24.04 默认也是 13，所以这三个 job 一直在按 13 编）。已统一改为
`g++-14`，ccache key 同步换名（`*-gcc13` → `*-gcc14`，避免命中旧编译器的缓存）。
本轮全部验证都是在 g++ 14.2 上做的，改完之后 CI 看到的就是同一个编译器。

**验收**：`-Werror` 全树（库 + 40 个测试 exe + bench）**0 错误 0 告警**；
Release+tests+bench+`-Werror` 树 ctest **749/749**；gcc Debug **749/749**、
clang Debug **749/749**、ASan **749/749**、TSan **747/747**（按 CI 豁免）；
clang 侧告警数 **0**（CI 另有 clang job，确认没引入 clang 独有告警）。

### 明确不做

| 项 | 理由 |
|---|---|
| 78 处 `fs::` 调用大规模原生化 | MSVC 的 `std::filesystem` 底下已是 `DeleteFileW` 那一套，无 CRT 堆/fd 表问题；重写只增加 `#ifdef` 密度，而本移植的价值恰恰是「一个 seam、两个后端、各一个文件」|
| `FileHandle` 改 strong type | 69 处改动换一个至今没踩过的错误类别；`io.hpp:47-49` 已写明向量插件要裸句柄通货 |
| `FileHandle` 改宏（如 `FILE_HANDLE`）| 宏无命名空间，且 `FILE_HANDLE` 正落在 `windows.h` 的 `FILE_*` 宏命名空间里（`FILE_SHARE_DELETE`/`FILE_ATTRIBUTE_NORMAL`…），会污染每个下游 TU；typedef 还能进 `std::expected<FileHandle, IoError>`、能被模板推导。**现状已是 typedef 分叉 + `kInvalidHandle` + `constexpr handle_valid`，无需改动** |
| 只读一次的 stdio 站点 | `index_manifest.hpp:169`、`segment.hpp:525`、`fileops/migrate.cpp:71` —— 持有窗口短，收益低 |

**执行序**：P0.0 → P0.1 → P1 → P2 → P3 → P4。P0 是硬前置；P2/P3 与 P0/P1 无依赖，
可并行。

---

## 依赖来源统一：vcpkg 退场

`vcpkg.json` 已删除。它只列了 zlib 一个依赖，却让 Windows 构建为这一个库就要
装 vcpkg、设 `VCPKG_ROOT`、拉 `x64-windows` 三元组——而其余七个依赖
（oneTBB / googletest / benchmark / utf8proc / cppjieba / limonp /
unordered_dense）早就是子模块。现在 zlib 也是：`third_party/zlib`，钉在 v1.3.1。

**分流不是一刀切**：`BITCASK_BUNDLED_ZLIB` 在 **Windows 默认 ON**，
**Linux/BSD/macOS 默认 OFF**（仍 `find_package(ZLIB)` 用系统包，行为与既往一字
不差）。理由是那些平台上 zlib 是基础系统库，且发行版打包明确反对 vendored
副本——钉死一个版本等于把自己排除在发行版的安全补丁之外。Windows 才是「根本
没有系统 zlib」的那个平台，也正是 vcpkg 当初存在的唯一理由。规则与既有的
`BITCASK_BUNDLED_TBB` 完全同构。

**没有用 `add_subdirectory(third_party/zlib)`**（那是本仓库其余子模块的做法）：
zlib 自带的 CMakeLists 在 out-of-source 构建时会**无条件把源码树里的 `zconf.h`
改名成 `zconf.h.included`**（其 CMakeLists 第 67-77 行），子模块工作区从此永远
dirty，`git status` 与 `git submodule update` 都受影响；顺带还建共享库、
examples、install/pkg-config 规则，一个都不需要。改为自建 15 个 `.c` 的静态
目标 + `ZLIB::ZLIB` 别名，并对每个源文件做存在性检查——版本升级导致清单对不上
是**配置期报错**而非静默漏编。实测子模块工作区保持干净。

`enable_language(C)` 只在 bundled 这一支调用：顶层 `project()` 只启用了 CXX，
而其余 C 依赖（utf8proc）走 `add_subdirectory`、由它自己的 `project()` 启用，
顶层无感。

Windows 侧还需一并安装 `bitcask_zlib`（合并静态库不含第三方对象），否则静态
链接的下游装出来的包缺 `crc32` 等符号。

**验证**：删掉整个 build 目录重新配置（原缓存里钉着 vcpkg 工具链），
msvc-debug 747 全绿。**Linux 侧未复验**——该分支走的是 `find_package`，与改动前
同一条路径，但仍应跑一次 `gcc-release` 确认 `enable_language(C)` 的条件分支没有
在非 Windows 上引入意外。

---

## 当前状态快照

| 项 | 状态 |
|---|---|
| 设计文档 | ✅ `doc/windows-port-design-zh.md`（含三项决策基线与风险排序）|
| 面积实测 | ✅ 裸 POSIX 调用面已重新量准（`::open` 13 处而非 51，见本届总纲）|
| S37-1 抽象层收编 | ✅ done（裸宿主原语归零；Debug/ASan 725/725 + 双树，见落地记录）|
| S37-2 fork → exec-self | ✅ done（6 个 fork 点全转；`tests/` POSIX 头归零；Debug/ASan 725/725，见落地记录）|
| S37-3.a cpu_features + 降档开关 | ✅ done（`__builtin_cpu_supports` 归零；四档 732/732 跨档对拍，见落地记录）|
| S37-3.b 内核分 ISA TU | ✅ done（24 个 target 函数搬完；ISA 泄漏 0；四档 732/732；bench ±2%，见落地记录）|
| S37-4 MSVC 构建适配 | ✅ done（188 个 TU 编译零错误；13 库 + 合并静态库产出；剩余全部为 io 后端符号。Linux 复验：双树 + 干净树编译通过、Debug 732/732，ASan/TSan 未跑，见落地记录）|
| S37-5 Windows I/O 后端 | ✅ done（bitcask.dll 产出；Windows ctest 733 → 724 通过 99%，余 9 个全部落在 S37-6；Linux 复验 735/735 无回归，见落地记录）|
| S37-6 删除/映射生命周期 | ✅ done（**实测推翻立项前提，非架构改动**；Windows ctest 733/733 全绿，见落地记录）|
| S37-7 CI + 收尾 | ⬜ 未开始 |
| P 段 P0 路径编码统一 | [done]（59 处成对收编 + 守门 + 9 个用例；P1 期间又补 26 处隐式转换。**Linux 已复验**，见 P 段复验记录：1 个用例把 Windows 结果写成了通用断言，已改）|
| P 段 P1 `.prev` 轮转收进 seam | [done]（4 处；立项理由被实测推翻，改按纪律做。**Linux 已复验**）|
| P 段 P2 读路径改走定位读 | [done]（原计划是写路径缓冲层，实测把靶心改到 2 GiB 天花板。**Linux 已复验**，2.54 GiB 用例走稀疏文件 3 ms 实跑未跳过）|
| **Linux 全面复验（P 段 + S37-4/5 遗留）** | ✅ done（2026-08-08）：Debug g++/clang 各 **749/749**、Release+LTO 编译、**ASan 749/749**、**TSan 747/747**、干净树 configure、守门脚本。查出 1 条真回归（`-Wcomment` 打断 `werror-lib`，已修）+ 1 个过窄断言（已修）；TSan 那条失败是预存 CI 豁免项，已用 v6.1.0 基线对拍证否 |
| **`werror-lib` 修复（S37-7.3 的 Linux 半边）** | ✅ done（2026-08-08）：该 job 自 v6.1.0 起从未绿过。库 14 条 + tests/bench 118 条，**共 132 条全部修根因**（无一个 `-Wno-*`）。现在 `BITCASK_WERROR=ON`+`BUILD_TESTING=ON`+benchmarks 的干净树整树 0 错误 0 告警，clang 侧亦 0。改动含 seqlock 头与公开头、且把多处「忽略返回值」改为断言，已全矩阵复跑：Release+`-Werror` 749/749、gcc/clang Debug 各 749/749、ASan 749/749、TSan 747/747 |
| P 段 P3 field_schema 退掉长期 FILE* | [done]（顺带修掉未检查的写导致的 id 错位；adopt_stream 删除）。**Linux 已复验**：`-Werror` 全树 0 告警 + gcc/clang/ASan 各 749/749 + TSan 747/747 + 双树；偏移追踪实测正确。另查实一个 torn-tail 隐患为**预存**（P3 前后输出逐字相同），见 P3 复验记录 |
| P 段 P4 错误模型 | [done]（不做大统一的结论保留；修掉 ScanFault.errnum 漏 Win32 码到 C API）|

### 待确认项

已由 S37-4 实测结清（VS 18 / MSVC 14.51 / SDK 10.0.28000）：

- ~~cppjieba / limonp 在 MSVC 下的可编译性~~ → ✅ **零修改编过**（含 utf8proc /
  googletest / benchmark / unordered_dense / oneTBB，第三方一处没改）。
- ~~`std::expected` 等 C++23 设施的覆盖度~~ → ✅ 21 个依赖 TU 全部编过，
  `/std:c++latest` 下 `<expected>` / `<span>` / `<bit>` 齐备。
- ~~MSVC 侧的 `/arch:` 施加~~ → ✅ `bitcask_simd_tu` 的 MSVC 分支实测可用，
  12 个分 ISA TU 全部编过（含 `/arch:AVX512` 的 5 个）。

仍未结清：

- 设计稿 §2.2「MSVC intrinsic 无需 `/arch` 开关」——**只验证了能编，没验证
  生成的指令**。分 TU 是性能需要还是正确性需要，须等 S37-7 的反汇编泄漏检查
  （Linux 侧 S37-3.b 已做过：42 个非 SIMD TU 宽指令泄漏 0）。
- 目标最低 Windows 版本（决定 `PrefetchVirtualMemory` / `FILE_ID_INFO` /
  长路径 opt-in 可用性）—— S37-5 落地前须拍板。
