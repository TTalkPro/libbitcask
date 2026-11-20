# 检索库经 merge_only 旁车合并永久挂住：`Cask::merge` 的 `flush_index()` 等一个永远到不了的 LSN —— 纯 KV 库正常

**报的人**：bitcask（Erlang 封装，修 `merge/2,3` 的 `dict_path` 时撞到；libbitcask 11.1.0 `967b5f7`，11.0.0 `862d735` 同样复现；Linux x64）。
**严重度**：🔴 调用线程永久阻塞，不报错、不超时。S57 之前 `needs_merge` 在重开过的库上常返回 false，`merge/2` 多半根本走不到合并；
S57（11.1.0）之后它会如实返回 true，**`bitcask:merge/2` 对检索库从「什么都不做」变成「挂死」**。

## 症状

```text
needs=1 files=1
HANG in merge            ← 60 s 后仍未返回；同样的写入模式、不开检索（纯 KV）2 ms 合完
```

Erlang 侧：`bitcask:merge(Dir, [{analyzer, whitespace}])`（jieba 同）不返回，调用进程卡在 `bitcask_cpp_nifs:cask_merge/2`。

## 位置

- `src/cask/cask.cpp:3599`：`Cask::merge` 的 `if (text_)` 收尾段先 `flush_index()`（S45-3 LSN 前缀读屏障）。
- `include/bitcask/cask.hpp:1021`：`flush_index()` = `index_pool_->wait_lsn(index_lane_, authority_->peek_next_ord())`，**无超时**。
- gdb 栈：合并线程停在 `Cask::merge` 内的 `condition_variable` 等待；索引池分片 / 串行线程全部空闲（`wait_bounded_queue_monitor`）——
  没有任何在途任务，覆盖水位却够不到 `peek_next_ord()`。
- 车道在 `src/cask/cask.cpp:342` 以打开时的 `peek_next_ord()` 为起点注册。推测：merge_only 句柄在注册之后有路径推进了
  `next_ord`（例如 `Cask::merge` 开头的 `ensure_oki_for_write()` → `run_controlled_rebuild` 的 merge_only 分支
  `rebuild_memory_from_data`，或合并本身），而这些 ord 从未经车道 `complete`。**未核实**，只排除了「有在途任务」。

## 怎么撞上的

```cpp
// 1) 普通可写句柄建检索库（whitespace），30 个 key 各覆盖写 10 遍，close_write_file，close
// 2) 同目录 merge_only 打开（同样的 search_config）
auto o = search_options(); o.merge_only = true;
auto c = Cask::open(dir, o, &reg);
auto nm = (*c)->needs_merge();          // needs=1 files=1
(*c)->merge(nm.files);                  // 永不返回
```

完整探针：`/tmp/mo_probe.cpp`（.13），按 `ninja -t commands tests/bitcask_s57_feedback_test` 的编译 / 链接参数单独编。

## 影响面

- 所有经 merge_only 旁车合并检索库的下游：bitcask `merge/2,3`、`bitcask_merge_worker`、C API 同路径。纯 KV 库不受影响。
- 11.1.0 起更容易撞上（`needs_merge` 不再漏报）。
- 不报错、不超时：merge 调度进程 / 脏调度器线程永久占住，`merge.lock` 也不释放，后续合并全部 `merge_locked`。
- bitcask 侧暂时只能不对检索库做旁车合并（或在持有写句柄的进程里直接 `Cask::merge`）。

## 建议（下游不替 libbitcask 拍板）

1. merge_only 句柄不接受写（put / remove 都是 `kReadOnly`），车道上不会有它自己提交的任务——`flush_index()` 对 merge_only 应当直接跳过，
   或在注册车道 / 受控重建之后把车道起点对齐到当时的 `peek_next_ord()`。
2. 同时核对收尾段 `index_coverage()` 作为 search.ckpt 覆盖水位在 merge_only 下是否可信（覆盖水位落后于 `next_ord` 时存出去的 ckpt
   会让下次打开从错误的位置重放）；以及 merge_only 旁车在写者仍开着时写 `search.ckpt` 是否安全。
3. `Cask::merge` 里的读屏障给有界超时，超时报错而不是无限等。
4. 回归：检索库（whitespace / jieba）merge_only 打开 → `needs_merge` → `merge` 在有限时间内返回，之后用写句柄重开检索结果不变。

## 回没回

已修（**11.1.1 已发版**，S58）。推测方向对：插桩实测旁车车道以 `next_ord = 0` 注册，`Cask::merge` 开头的受控重建（merge_only 分支
`rebuild_memory_from_data`）把 `next_ord` 推到 300，收尾 `flush_index()` 等 `wait_lsn(lane, 300)`、覆盖水位停在 0。
同进程旁车（共享 registry）另有一条：旁车打开后写者新写的 ord 不在旁车车道上，同样挂。

建议 2 的担心也属实：只去掉等待的话，收尾会用旁车的空索引改写写者的 `bm25.ckpt` / `docmap.ckpt` / `index.manifest`
（实测）。所以按建议 1 再进一步：**merge_only 旁车在检索库上按纯 KV 合并**——不带检索插件、跳过检索侧收尾（读屏障 + 成对保存点），
与 `checkpoint()` / close 本就拒绝 merge_only 写检索 ckpt 一致；`flush_index()` 对 merge_only 直接返回，旁车上调检索也不挂。
搬迁的检索侧由写者归正：同进程共享权威，写者读写照常；下次打开按 key+LSN 重放合并输出。

回归 `tests/s58_merge_only_search_test.cpp`：写者已关 / 写者开着且同进程（旁车打开后写者继续写）两种，合并有界返回、
检索与值不变、写者检索持久化逐字节不变（11.1.0 上两条都挂）。

附带一条与本反馈无关、早已存在的问题（未修）：**跨进程**旁车在写者开着时合并，写者之后读被搬走的 key 报
`kIo "open file_id=N"`，重开才好；同进程（bitcask 的用法）不受影响。
