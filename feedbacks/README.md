# feedbacks —— 下游消费方报上来的账

一条一个文件，`<日期>-<slug>.md`。**只记「libbitcask 这一侧该改」的**：
下游自己绕过去了但绕法很蠢、或者根本绕不过去的那些。下游自己的 bug 不进这里。
格式与判据照下游 keel 的 `feedbacks/` 约定（症状先行，病因其次，建议最后；
报文让人改错方向的，与不报错同罪）。

| 段 | 写什么 |
|---|---|
| **症状** | 下游看到的**那一行**（报错原文 / 或者「一个字都不报」） |
| **位置** | `文件:行`，libbitcask 这一侧的 |
| **怎么撞上的** | 最短复现路径 |
| **影响面** | 谁会撞、撞了会怎样、**报不报错** |
| **建议** | 可选，下游不替 libbitcask 拍板 |

## 现有

| # | 标题 | 报的人 | 严重度 | 回没回 |
|---|---|---|---|---|
| [2026-10-10](2026-10-10-windows-merge-retired-files-not-deleted-on-close.md) | Windows 上 merge 之后 `bitcask_close` 不删退休文件，盘上体积不降：`close()` 的 `drain_retired_files()`（`cask.cpp:775`）排在 `read_files_.clear()`（`:791`）之前，要删的文件仍被映射占着，`DeleteFileW` 失败后放回队列又被析构丢弃；不报错 | 锦书（C#，经 Keelson.Bitcask 1.0.6，Windows x64） | 🟠 结果错，不报错 | 已修（代码侧，**未上 release**）：`close()` 的 `drain_retired_files()` 挪到释放读句柄缓存（`read_files_.clear()`）之后；排水后仍留在队列里的路径记一条 `log_error`（数量 + 原因），不再静默丢。`checkpoint()` 入口与 merge 开头的排水经核对前序已正确（merge 已在 `:3294` 摘掉输入句柄），未动。回归 `CaskDocValueTest.MergeThenCloseAfterSearchRemovesRetiredFiles`（开搜索、4 轮覆盖写、搜索、merge、close，断言退休输入消失）。**本机未复现**：同形态在 Windows 上原代码即绿，故病因（句柄占用）未经实测证实；锦书侧需在 Windows 上复验 `RagIndexTests.Compaction` |
| [2026-10-06](2026-10-06-atomic-batch-doc-meta.md) | 原子批 / `txn_commit` 写不了带 meta 的文档（`BatchOp` 只有 key + value，value 一律当 text）：文档和它的二级索引项进不了同一批，下游只能分三步写，再加 key 锁和读时回表校验兜底 | bitcask（pin 6.6.1 `33f2147`） | 🟡 能力缺口，不报错 | 已修（**6.6.2**）：`Cask::BatchOp` / `TxnOp` 加 `kPutDoc` + `const DocInput* doc`，批内编码与索引登记与 `put_doc` 共用一份（meta / vector / fields / expiry_at 都能进批）；C API 纯加法 `bitcask_txn_op_ex_t`（`op = 2`）+ `bitcask_put_batch_atomic_ex` / `bitcask_txn_commit_ex`。校验失败零副作用（字段 intern 推迟到全批校验后）；顺修 merge race 重写路径按 text 重编码。回归 `AtomicBatchDocTest.*` / C `test_txn` |
| [2026-10-06](2026-10-06-meta-filter-query-gaps.md) | 带 meta filter 的 `search_text` 静默少返回：固定多取 `max(k×4,64)` 个候选再后过滤，不补取（1000 篇文档里满足 filter 的有 100 篇，K=10 只回 6 条），hybrid 的文本路同病；同分命中 top-K 按 key 升序选、按 ord 降序出，大 K 结果不是小 K 的延长（offset 分页按机制会翻出重复页）；fold / range 迭代器解出 meta 后丢弃（下游逐条补 get，text 和 meta 可能不是同一版本）；phrase / fields / near / fuzzy / wildcard / bool 不收 filter；没有只按 meta 筛选的扫描 | bitcask（pin 6.6.0 `1a6d698`） | 🔴 结果错，不报错（第 1、5 条）；其余 🟠/🟡 | 已修五条（**6.6.1**）：① 后过滤命中不足 k 时候选数翻倍补取，直到凑满 k 或候选穷尽（hybrid 文本路经 `search_text` 同修；同一循环兜住判活缺额）；② `CaskIter::Entry` / `CaskRangeIter::Entry` 加 `meta`，`set_want_meta` / `RangeOptions::want_meta` 开启，与 value 同一次读；③ phrase / bool / fields / near / fuzzy / wildcard 末位加 `filter`（Cask / Searcher / TextPlugin 三层）；④ `CaskIter::set_filter` / `RangeOptions::filter`，C++ 侧逐条求值、不通过不拷值；⑤ 平局规则统一为全序（分数降序, 段次序, 段内 docid 升序）——内核堆比较器改「堆顶 = 最差者」（剪枝条件不变、无额外开销），跨段改按分数稳定排序，不再按 key / ord 重排；小 K = 大 K 前缀、offset 分页不重不漏（可见变化：同分先后由 ord 降序变为大体写入序）。C API 同批跟进（6 个 `*_filtered` 检索 + `*_start_ex` / `*_next_ex` 迭代）。回归 `MetaFilterQueryTest.*` / `MaxScore.TopKTiedScoresPrefixStableAllKernels` / `test_meta_filter_query`。附条（`GetResult` 无 fields）未动 |
| [2026-09-23](2026-09-23-search-open-resident-memory-no-c-api-knob.md) | `enable_search` 的库一打开就常驻 ≈ 盘上 1.8 倍（336 MB ⇒ 峰值 610 MB）：BM25 段 `mmap_verify_crc` 缺省开、开库逐节 CRC 整读（104 MB 全进工作集），docmap / keydir ckpt 整份读进缓冲（≈ 67 MB 瞬时），同一批 key 住三份（keydir / `ext2ord_` / `key_to_location_`）；除 `keydir_cache_entries` 外 C API 一格都没有 | keel（转 coxswain 2026-09-22） | 🟠 开库即付的内存，不报错 | 已修前两条（**6.6.0**：段 CRC 改分块定位读、不再扫映射进工作集，另透 `bitcask_open_ex2` + `segment_verify_crc`；ckpt 读峰值 2× → 1×，docmap 行段与 keydir 快照完全流式）；第三条同批修（S40：实为四份，漏计 docmap `ord2ext`；`ord2ext` / `key_to_location_` 改 view，常驻降为两份） |
| [2026-09-23](2026-09-23-c-api-no-search-thread-count-knob.md) | C API 收不了线程数：`enable_search` 首次开库起 `IndexPool(hardware_concurrency)` + 进程级 search `task_arena`（空库实测 +15 条），下游没有口子 | keel（转 coxswain 2026-09-22） | 🟡 调不到的旋钮，不报错 | 已修（**6.6.0**：`bitcask_set_thread_limits` 进程级，首个 search 库建池即冻结，之后异值报错；`search_slots` 兼封 TBB worker） |
| [2026-09-18](2026-09-18-oki-unflushed-memdelta-range-query-4000x-slower.md) | OKI memdelta 未 flush 时 range 查询按未 flush 写入量线性变贵（5 万边实测 ~120ms/跳，flush 后 ~0.03ms，4200×）：不报错、装载后开始服务的负载必撞 | bitcask（pin `598d080`） | 🔴 静默 600~4200× 性能悬崖 | ✅ 已修（**6.5.0**：排序视图缓存——`make_read_view` 的排序去重快照以 memdelta 变更代数失效，写后首查重建一次，连续查询降为 shared_ptr 拷贝；`status()` 新增 `oki_delta_rows`/`oki_delta_bytes`；回归测试 `OkiRangeTest.SortedViewCache*`/`UnflushedMemdelta*`） |
| [2026-09-18](2026-09-18-embedder-pool-dispatch-flaky-under-parallel-suite.md) | `pool_dispatch_avoids_busy_worker` 全量套件偶发假失败：`:444` guard 采样到队列 >0，`:445` 断言再采样时 mock 已排空，pick 平局回退 W0 | bitcask（pin `598d080`） | 🟡 CI 假红，间歇、隔离不现形 | ✅ 已修（修在下游 bitcask 仓库的测试侧：`pick_probe/1` 改返回 `{胜者, 邮箱快照}`——判定与依据同刻，排空自动不作数；模块 38/38 绿） |
| [2026-09-18](2026-09-18-ord-recycling-doc-exhaustion-years-off-by-10-7.md) | `ord-recycling-design-zh.md` 溢出年数算错 ~10⁷ 倍（正确 ≈585 万年 @10⁵ ord/s）；同段 32-bit「约 12 天」实为约 11.9 小时 | bitcask（pin `598d080`） | 🟢 文档勘误，结论不受影响 | ✅ 已修（§1.1 与 §12.1 两处改为「约 585 万年」并补秒数换算；32-bit 句改为「约 11.9 小时」） |
| [2026-09-14](2026-09-14-c-api-merge-policy-files-checkpoint-not-reachable.md) | C API 够不到 merge 策略 / 显式文件表 / checkpoint：宿主只能吃 60% 缺省、只能等 `needs_merge` 点头、退休文件只能靠 close 重开回收 | keel（转锦书 2026-09-13） | 🟡 调不到的旋钮，不报错 | ✅ 已修（**6.4.0**：`bitcask_merge_policy_t` + `bitcask_open_ex` / `bitcask_merge_files` / `bitcask_checkpoint`；策略走独立结构体而非 `bitcask_options_t`，ABI 不破） |
| [2026-09-13](2026-09-13-icu-msbuild-v143-needs-vctoolsversion-pinned.md) | vendored ICU 在 VS 18 上编不动：`/p:PlatformToolset=v143` 少了配套的 `VCToolsVersion` ⇒ MSB8052，报文指的两条改法都不通 | keel（porthole 报 · coxswain 复现 + A/B） | 🔴 构建当场停，报文指向与病因无关的方向 | ✅ 已修（`/p:VCToolsVersion` 自动钉 + STATUS 第四格；VS 18 真机待复验） |
