# C API 收不了线程数 —— `enable_search` 第一次开库起 `hardware_concurrency` 条索引 / 查询线程，下游没有口子

**报的人**：keel（下游消费者，pin 6.5.0 `f618e82`），转的是 coxswain（keel 的宿主，代码编辑器）2026-09-22 的账：
keel `feedbacks/2026-09-22-bitcask-index-resident-cost.md` ②（前一份 `2026-09-22-embed-ready-loads-all-ggml-backends.md` 里顺带问过一次）。
**严重度**：🟡 不报错，资源占用上的；桌面宿主「开着备用、多数时候没人查」的库也付这批线程。

## 症状

Windows 11，空库实测：`bitcask_open` 带 `enable_search = 0` 线程数不变；换成 `enable_search = 1` ⇒ **+15 条**
（那台机器 `hardware_concurrency` 的量级）。

## 位置

- `src/keydir/keydir_registry.cpp:16-24`：`IndexPool(map_workers = hardware_concurrency)`，首个 `register_lib` 时真起线程；
  外加 dispatcher / reducer。
- `src/search/search_arena.cpp:17-24`：进程级 `static tbb::task_arena(hardware_concurrency)`。
- `c_api/internal.h:58-62`：C API 的 registry 是进程级单例 ⇒ ⭐ 这批线程是**每进程付一次**，不是每库一次
  （keel 按代码读的，没量第二个库 —— 如果不对，请更正）。
- 恢复期另有 TBB 全局 worker（`cask_recovery.cpp:319`）与纯 KV 恢复的临时线程（`:584-649`）—— 后者是瞬时的，不算这里的账。
- `bitcask_options_t` / `bitcask_merge_policy_t` 里没有任何线程数格；仅有的两个是**单次调用级**的（`prefetch_threads`、并行扫描的 `n_threads`）。
  环境变量只有 `BITCASK_SIMD_MAX`。

## 怎么撞上的

任何走 C API 开 `enable_search` 的宿主；核越多越显眼。

## 影响面

- 宿主想把后台索引压到 2–4 条线程让出 CPU / 减少线程数账面，**没有办法**。
- 报不报错：**不报**。

## 建议（下游不替 libbitcask 拍板）

一格**进程级**的设置比每库一格更贴实际形状（池本来就是进程级共享的）：

- `bitcask_set_thread_limits(size_t index_workers, size_t search_slots)`，**首次开 search 库之前**调才生效
  （之后调回错误码，别静默忽略）；`0` = 今天的行为（`hardware_concurrency`）。
- 或者放进 `bitcask_open_ex` 的独立结构体，但「第一个开库的人定终身」的语义要写明。

keel 那侧的形状已经对宿主给了口径：`(bitcask-open p 'search? #t 'search-threads 4)`，`'auto` = 今天的行为；
libbitcask 开格之后 keel shim 转一手。

## 回

**6.6.0 照建议的第一种形状收**：`bitcask_set_thread_limits(index_workers, search_slots, fault)`，
进程级。keel 按代码读的判断是对的：索引池挂在进程级 registry 上，每进程付一次。

- 首个 search 库 open 之前调才生效；之后值不同 → `BITCASK_ERR_INVALID_OPTION`，detail
  注明生效值（照建议没有静默忽略）；值相同 → OK。`0` = 今天的行为。
- 与建议不同的一处：`search_slots` 非 0 时顺带装了 `tbb::global_control`，把本库经 TBB
  跑的并行段（恢复期批内 prepare、查询内 parallel_for）的 worker 也封到
  `search_slots - 1`。不装的话，宿主压到 4 条，首次恢复或首条大查询之后照样会冒出
  十几条 TBB worker。代价：宿主若与本库共用同一份 TBB 动态库，它自己的 TBB 并行也受限
  （文档已写明）。
- 实测（14 核 Windows，`(2, 2)`）：开 search 库线程增量在断言上界 5 以内，缺省约 15。
  回归测试 `ThreadLimitsTest.*`；`c_api_test.c` 的其余 search 用例现在全在 `(2, 2)` 下跑。
  C API 文档 §8.1d。

