# P9 — read_files_ fd 预算 LRU 设计

> 状态：**已落地**（P9a/P9b 均已实现，见 §3）。路线图见 [`../ROADMAP.md`](../ROADMAP.md)；
> 与 P6（sealed mmap）互补。

## 1. 背景

`Cask::read_file`（`cask.cpp:1044`）按 file_id 懒打开 DataFile 并缓进
`read_files_`（`cask.hpp:506`，现为 `unordered_map<file_id, ReadHandle>`，`ReadHandle`
持 `shared_ptr<DataFile>` + atime）。merge unlink 时 erase（`cask.cpp:1802`）、close 时
clear（`:678`）。

P9 前**无 LRU 淘汰**——每个被读过的文件常驻一个 fd，大库（数据量大 → 文件数 =
data/2GiB，见 [`concurrency-zh.md`]）读过几十~上百文件 → fd 数线性增长、**可能撞
`ulimit -n`**。P9（本设计）加了 `max_read_handles` 上限 + 近似 LRU 淘汰空闲句柄解决此问题。

## 2. 方案：按上限淘汰只读句柄

- `read_files_` 加 LRU（list + map，front=最近）+ **数量上限** `max_read_handles`
  （可配，默认如 256/1024）。命中 → 提到 front；插入超上限 → 淘汰尾部。
- **shared_ptr 安全**：淘汰只从 map/list 去引用；**在途读者仍持 `shared_ptr<DataFile>`
  → fd 由引用计数续命**，读者用完才真正 close（与现有 O10/merge-unlink 续命同模式）。
- 与 **P6 互补**：sealed 文件 mmap 后 close fd（不占 fd）；未 mmap / pread 路径 / P6
  之前由本 LRU 控 fd。

## 3. 子任务
- **P9a（已落地）**：`read_files_` 值改为 `ReadHandle{shared_ptr<DataFile>, atime}`，在
  现有 `read_cache_mu_` 下维护。命中走**共享锁**置 atime（`read_clock_` 单调计数，§4(b)
  的 clock 近似 LRU，避免读热路径抢独占锁）；插入后在独占锁下 `evict_read_handles_locked()`
  （`cask.cpp:1094`）淘汰 atime 最旧的**空闲**句柄。
- **P9b（已落地）**：`max_read_handles` 选项（`cask.hpp:57`，0 = 不限 = 现状）；
  内省 `read_handle_count()`（`cask.cpp:1085`）。

## 4. 并发 / 边界
- `read_cache_mu_` 护 `read_files_`。**命中需更新 LRU 序**——实现选了方案 **(b)**：
  不维护真正的 list，而是命中时在**共享锁**下把 `ReadHandle.atime` 置为
  `read_clock_` 自增值（atomic，近似 LRU），淘汰时在独占锁下扫描 atime 最旧者；
  读热路径不抢独占锁。淘汰仅针对 **use_count==1 的空闲句柄**（在途读者持引用即跳过）。
  （方案 (a) 命中走独占锁未采用。）
- `active_data_` 不在 read_files_（独立成员）——不受淘汰影响。
- fold-pin 句柄（S13，`CaskIter` 自持）独立于 read_files_——淘汰不影响在跑的 fold。
- merge unlink 的 erase 与 LRU 淘汰并存（都去引用，shared_ptr 续命）。

## 5. 风险
- 淘汰刚要复用的句柄 → 重开 open 代价；cap 需按工作集调。
- 命中提升的锁开销（见 §4，选 clock 近似避免读路径抢独占锁）。

## 6. 测试
- 打开 > cap 个文件并各读一次 → 断言常驻句柄数 ≤ cap（或 fd 数有界）。
- 淘汰后再读该文件 → 正确（重开）。
- 淘汰中并发读：一个读者持 shared_ptr，另一线程触发淘汰该 file_id → 读者照常读、无 UAF。
- cap=0 → 行为同现状（不限）。
