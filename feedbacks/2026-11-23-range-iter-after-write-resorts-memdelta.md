# 每次写之后的第一次 range / 迭代要把整个未落盘的 memdelta 拷一遍再排一遍：耗时随未落盘行数线性涨，40 万行时一次 range 80 ms

**报的人**：bitcask（Erlang 封装，排查图层 hub 读变慢时量出来的；libbitcask 12.0.1 `c364cd3`，Linux x64，GCC Release）。
**严重度**：🟠 读写交替的负载下 range 慢 3～4 个数量级，不报错；拷贝在 `OkiState::mu_` 内，期间点查和写也被挡住。

## 症状

不报错。同一个只返回 1 行的 range（`[a, b)`，库里只有一个 key 落在里面），唯一的变量是「上一次 range 之后有没有写过」
和「库里有多少行还没落盘」：

| 未落盘行数 | range（期间无写） | 一次 put | put 之后的 range | put 之后的点查 |
|---|---|---|---|---|
| 1 000 | 8.7 µs | 1.7 µs | 142 µs | 3.1 µs |
| 10 000 | 5.1 µs | 1.5 µs | 1.44 ms | 3.5 µs |
| 100 000 | 5.2 µs | 2.1 µs | 14.9 ms | 3.4 µs |
| 400 000 | 5.0 µs | 2.2 µs | **80.1 ms** | 3.8 µs |

每行各取 200 次的平均。写的 key 与被扫的区间无关（`x…`，区间是 `[a, b)`），照样触发。约 0.15～0.2 µs / 未落盘行 / 次。
`kFlushRowLimit` 是 100 万行，所以长驻写者的库在两次落盘之间最坏约 200 ms 一次 range（外推，未实测）。

## 位置

- `src/fileops/oki_state.cpp:1147` `OkiState::make_read_view`：排序视图缓存以 `delta_version_` 为失效判据，
  任何一次 append 都 bump 版本（`append_update`、`apply_batch`）。失配时：
  - `:1168` 在 **`mu_` 内**把整个 `delta_` 拷一份（`std::vector<DeltaRow>`，每行一个 `std::string` key）；
  - `:1179` 锁外 `sort_dedup` 全量排序去重；
  - `:1181` 再取 `mu_` 回填缓存——期间只要又有一次写，这份缓存立刻作废。
- 调用方：`src/cask/cask_range_iter.cpp:27`（`make_range_iter`）、`src/cask/cask_iter.cpp:47`（`make_iter`）。
  每开一个迭代器调一次。
- 这是 [`2026-09-18-oki-unflushed-memdelta-range-query-4000x-slower.md`](2026-09-18-oki-unflushed-memdelta-range-query-4000x-slower.md)
  的后一半：那次加的缓存修好了「无写时连续查询」，注释里写明「写后首查重建一次」是保留行为。问题是服务型负载里
  写和 range 是交替的，「写后首查」就是常态，每次都重建。

## 怎么撞上的

```erlang
R = bitcask:open(Dir, [read_write]),
[ok = bitcask:put(R, <<"k", I:64>>, <<"v">>) || I <- lists:seq(1, 400000)],
ok = bitcask:put(R, <<"a1">>, <<"x">>),
F = fun(_K, _V, _T, _O, A) -> A + 1 end,
%% 交替：写一条无关的 key，再扫 [a, b)
[begin ok = bitcask:put(R, <<"x", I:64>>, <<"x">>),
       1 = bitcask:range_fold(R, {<<"a">>, <<"b">>}, [], F, 0)
 end || I <- lists:seq(1, 200)].
%% 每轮约 80 ms；去掉 put 则每轮 5 µs
```

复现脚本在开发机 `/tmp/gprof/rng.escript`（在 bitcask 仓库根目录用 `escript` 跑）。

下游的真实场景：bitcask 图层读分块 hub 的邻接表原先走前缀 range，库里有约 7 万个未落盘 key 时，每次「写一条边 → 读 hub 的
前 10 条边」是 9 ms，而同样内容用点查是 11 µs。图层已改成按块目录点查绕开（bitcask TASK G10），但图层的全库扫描
（`materialize`、`edges_by_type`、`check_invariants`）和别的下游的 range / 前缀查询绕不开。

## 影响面

- 所有「写和 range / 迭代交替」的下游：每次迭代器创建付 O(未落盘行数) 的拷贝 + 排序，与扫描区间多大无关——扫 1 行也一样。
  bosun 的前缀查询、bitcask_query 的 range、bitcask_txn 的 `prefix_range`、图层的全库扫描都在这条路上。
- **并发副作用**：`:1168` 的拷贝在 `mu_` 内。`locate`（所有点查）和 `append_update` / `apply_batch`（所有写）都要拿 `mu_`，
  所以一个线程开迭代器时，别的线程的点查和写会被挡住一次拷贝的时间（40 万行估计十毫秒级，未单独测）。
  读线程多时每个线程各自重建一份（缓存回填时版本多半已经又变了），互相不复用。
- 不报错。表现为 range 的延迟随「距上次落盘写了多少」慢慢变差，checkpoint / flush 之后突然变好。

## 建议（下游不替 libbitcask 拍板）

1. **增量维护有序视图**，不要每次写后全量重建。例如：排好序的基底（上次重建的结果，不可变）+ 一段小的未排序尾巴
   （基底之后追加的行）；迭代器创建时只排尾巴，归并游标多归并一路。尾巴超过阈值（比如基底的 1/8 或固定行数）再合并进基底。
   这样写后首查的代价是 O(尾巴)，不是 O(全部)。
2. 或者 active 段直接用有序结构（跳表 / B 树），迭代器在上面按 seq 上界读，不拷贝。改动面大。
3. 至少把 `:1168` 的全量拷贝挪出 `mu_`（例如行存 `shared_ptr` 的不可变块，锁内只拷指针），别让开迭代器挡住点查和写。
4. 回归：N 行未落盘、交替「写 1 条 + range 1 行」，断言单次 range 的耗时不随 N 线性涨（或直接断言重建只碰新增的行）。

## 回没回

**libbitcask（2026-11-23，S64，12.0.2 已发版）**：已修，取建议 1 + 3。`make_read_view` 的排序视图缓存改成增量的——若干不可变有序段 + 一小段有序尾巴，写后首查只拷贝并整理新增的行；尾巴满 128 行升段，相邻段行数相近时线性归并。锁内拷贝一次最多 4096 行。flush / 重建重排 active 时缓存整体作废，之后第一次读重建一次（首建仍是一次全量排序）。实测「写 1 条无关 key + 扫 1 行」：5 万 / 10 万 / 40 万未落盘行 4 / 4 / 5 µs（改前 10 万行 15 ms、40 万行 80 ms）。C API、盘上格式、C++ ABI 不变。回归 `OkiRangeTest.RangeAfterWriteDoesNotResortWholeMemdelta` / `IncrementalSortedView*`（钉的是视图整理处理的行数，不是墙钟）。建议 2（active 直接用有序结构）没做。

**bitcask**：bitcask 图层已自行绕开分块组的读（改点查）；其余 range 路径等上游。
