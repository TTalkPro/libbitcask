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
| [2026-09-23](2026-09-23-search-open-resident-memory-no-c-api-knob.md) | `enable_search` 的库一打开就常驻 ≈ 盘上 1.8 倍（336 MB ⇒ 峰值 610 MB）：BM25 段 `mmap_verify_crc` 缺省开、开库逐节 CRC 整读（104 MB 全进工作集），docmap / keydir ckpt 整份读进缓冲（≈ 67 MB 瞬时），同一批 key 住三份（keydir / `ext2ord_` / `key_to_location_`）；除 `keydir_cache_entries` 外 C API 一格都没有 | keel（转 coxswain 2026-09-22） | 🟠 开库即付的内存，不报错 | 已修前两条（**6.6.0**：段 CRC 改分块定位读、不再扫映射进工作集，另透 `bitcask_open_ex2` + `segment_verify_crc`；ckpt 读峰值 2× → 1×，docmap 行段与 keydir 快照完全流式）；第三条（key 住三份）结构性，未动 |
| [2026-09-23](2026-09-23-c-api-no-search-thread-count-knob.md) | C API 收不了线程数：`enable_search` 首次开库起 `IndexPool(hardware_concurrency)` + 进程级 search `task_arena`（空库实测 +15 条），下游没有口子 | keel（转 coxswain 2026-09-22） | 🟡 调不到的旋钮，不报错 | 已修（**6.6.0**：`bitcask_set_thread_limits` 进程级，首个 search 库建池即冻结，之后异值报错；`search_slots` 兼封 TBB worker） |
| [2026-09-18](2026-09-18-oki-unflushed-memdelta-range-query-4000x-slower.md) | OKI memdelta 未 flush 时 range 查询按未 flush 写入量线性变贵（5 万边实测 ~120ms/跳，flush 后 ~0.03ms，4200×）：不报错、装载后开始服务的负载必撞 | bitcask（pin `598d080`） | 🔴 静默 600~4200× 性能悬崖 | ✅ 已修（**6.5.0**：排序视图缓存——`make_read_view` 的排序去重快照以 memdelta 变更代数失效，写后首查重建一次，连续查询降为 shared_ptr 拷贝；`status()` 新增 `oki_delta_rows`/`oki_delta_bytes`；回归测试 `OkiRangeTest.SortedViewCache*`/`UnflushedMemdelta*`） |
| [2026-09-18](2026-09-18-embedder-pool-dispatch-flaky-under-parallel-suite.md) | `pool_dispatch_avoids_busy_worker` 全量套件偶发假失败：`:444` guard 采样到队列 >0，`:445` 断言再采样时 mock 已排空，pick 平局回退 W0 | bitcask（pin `598d080`） | 🟡 CI 假红，间歇、隔离不现形 | ✅ 已修（修在下游 bitcask 仓库的测试侧：`pick_probe/1` 改返回 `{胜者, 邮箱快照}`——判定与依据同刻，排空自动不作数；模块 38/38 绿） |
| [2026-09-18](2026-09-18-ord-recycling-doc-exhaustion-years-off-by-10-7.md) | `ord-recycling-design-zh.md` 溢出年数算错 ~10⁷ 倍（正确 ≈585 万年 @10⁵ ord/s）；同段 32-bit「约 12 天」实为约 11.9 小时 | bitcask（pin `598d080`） | 🟢 文档勘误，结论不受影响 | ✅ 已修（§1.1 与 §12.1 两处改为「约 585 万年」并补秒数换算；32-bit 句改为「约 11.9 小时」） |
| [2026-09-14](2026-09-14-c-api-merge-policy-files-checkpoint-not-reachable.md) | C API 够不到 merge 策略 / 显式文件表 / checkpoint：宿主只能吃 60% 缺省、只能等 `needs_merge` 点头、退休文件只能靠 close 重开回收 | keel（转锦书 2026-09-13） | 🟡 调不到的旋钮，不报错 | ✅ 已修（**6.4.0**：`bitcask_merge_policy_t` + `bitcask_open_ex` / `bitcask_merge_files` / `bitcask_checkpoint`；策略走独立结构体而非 `bitcask_options_t`，ABI 不破） |
| [2026-09-13](2026-09-13-icu-msbuild-v143-needs-vctoolsversion-pinned.md) | vendored ICU 在 VS 18 上编不动：`/p:PlatformToolset=v143` 少了配套的 `VCToolsVersion` ⇒ MSB8052，报文指的两条改法都不通 | keel（porthole 报 · coxswain 复现 + A/B） | 🔴 构建当场停，报文指向与病因无关的方向 | ✅ 已修（`/p:VCToolsVersion` 自动钉 + STATUS 第四格；VS 18 真机待复验） |
