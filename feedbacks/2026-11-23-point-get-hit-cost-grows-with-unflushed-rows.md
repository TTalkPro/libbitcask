# 点查命中一次的耗时随未落盘行数涨（1 千行 1.3 µs → 40 万行 2.0 µs）；热点是 memdelta 点查哈希的缓存未命中和活跃文件每次一个 pread

**报的人**：bitcask（Erlang 封装；图层优化到头之后量下来，各条主路径一半以上的时间在引擎点查上。libbitcask 12.0.2 `ab9b51f`，Linux x64，GCC Release）。
**严重度**：🟡 性能，不报错，结果正确。不是回退——是现在最显眼的一块固定成本。

## 症状

不报错。同样 128 字节的值、页缓存全热、全部数据在活跃文件与 memdelta 里（没 checkpoint 过），命中 / 未命中一次的耗时：

| 库里的 key 数（= 未落盘行数） | 命中（经 NIF） | 未命中（经 NIF） |
|---|---|---|
| 1 024 | 1.29 µs | 0.27 µs |
| 100 000 | 1.84 µs | 0.34 µs |
| 400 000 | 2.00 µs | 0.45 µs |

每格 10 万次取平均。同一份 1024 key 的负载在上游自己的基准里（`BM_Cask_Get_Hot`，纯 C++）是 **848 ns**，
所以 NIF 这层约 0.4 µs，其余都在引擎里；命中比未命中贵 1.0～1.5 µs，并且随行数涨。

`perf record` 两个点：

**1024 key（`BM_Cask_Get_Hot`）**

| 占比 | 符号 |
|---|---|
| 38.4% | `__internal_syscall_cancel`（`pread`） |
| 5.2% | `hw::detail::crc32_bytewise` |
| 约 8% | `cfree` / `malloc` / `operator delete` |
| 4.5% | `OkiState::locate` |
| 约 5.5% | `std::_Hashtable<std::string, …>::_M_find_*`（`delta_idx_`） |
| 4.5% | `codec::decode_doc_value` |
| 2.2% | `hw::detail::crc32_pclmul` |

**40 万 key（经 NIF；40 万次 put 建库 + 400 万次命中的 get 合在一起采的样）**

| 占比 | 符号 |
|---|---|
| 24.4% | `std::__detail::_Node_const_iterator<pair<const std::string, unsigned long>>`（`delta_idx_` 的查找） |
| 10.5% | `__syscall_cancel_arch`（`pread`） |
| 6.3% | `OkiState::locate` |
| 2.1% | `hw::detail::crc32_bytewise` |

（另有 13% 是 BEAM 空闲调度线程的 `scheduler_wait`，与本题无关。）

也就是：行数少时一次 `pread` 占四成；行数多了以后 memdelta 点查哈希的缓存未命中变成最大的一项。

## 位置

- `src/fileops/oki_state.cpp:1166` `OkiState::locate`：`delta_idx_.find(key)`，随后读 `delta_[it->second]`。
  `delta_idx_`（`include/bitcask/oki_state.hpp:496`）是 `std::unordered_map<std::string, std::size_t, StringHash, std::equal_to<>>`
  ——节点式哈希表，每个节点一次堆分配，key 又是一个 `std::string`（长 key 再一次堆分配），命中后还要再读一次 `delta_` 里的行。
  一次点查在这条链上是 3～4 次互不相邻的内存访问；表一大就都是缓存未命中。
  顺带：每个 key 的字节在内存里存了两份（`DeltaRow::key` 和 `delta_idx_` 的 key）。
- `src/fileops/data_file.cpp:251` `DataFile::read`：活跃文件走 `pread_into`（`:258`）。P6 给 sealed 文件做了整文件 mmap
  （`:53`），但没 roll 过的库（下游的图库多数时间如此）所有数据都在活跃文件里，每次命中一个系统调用。
- `hw::detail::crc32_bytewise`：一条 150 字节上下的记录，逐字节查表那一段比 pclmul 折叠那一段还贵（5.2% 对 2.2%）——
  看着是折叠不了的头 / 尾零头在逐字节算。
- `Cask::get` / `get_owned` 一路上每次有 malloc + free（约 8%）。

## 怎么撞上的

```erlang
R = bitcask:open(Dir, [read_write]),
V = binary:copy(<<"v">>, 128),
[ok = bitcask:put(R, <<"k", I:64>>, V) || I <- lists:seq(1, 400000)],
%% 命中：约 2.0 µs / 次；把 400000 换成 1024 是 1.29 µs
[bitcask:get_with_ord(R, <<"k", (1 + (I * 7919) rem 400000):64>>) || I <- lists:seq(1, 100000)].
```

脚本在开发机 `/tmp/gprof/r5.escript`（计时）与 `/tmp/gprof/perfget.escript`（给 perf 用），在 bitcask 仓库根目录用 `escript` 跑。
上游侧：`./bench/bitcask_bench --benchmark_filter='BM_Cask_Get_Hot$'`。

## 影响面

- 所有点查。对 bitcask 图层：低度数的一跳读约 3.9 µs，其中一次点查约 2 µs；批量加边每条边摊到约两次头的点查，占一批耗时的一半上下；
  单条写边约 13 µs 里读两个头约 3.6 µs。图层自己的开销已经压到这些数字以下，再往下只能靠这里。
- 「随未落盘行数涨」这一点对长驻写者更明显：`kFlushRowLimit` 是 100 万行，两次落盘之间 `delta_idx_` 可以长到百万项。
  落盘之后行进了 run，走 bloom + 块缓存那条路（本条没量那条路）。
- 不报错，结果正确。

## 建议（下游不替 libbitcask 拍板）

1. **`delta_idx_` 换成开放寻址的平表**，槽里放 `(key 的哈希, delta_ 下标)`，比较时直接比 `delta_[idx].key`：
   省掉节点分配和重复的 key 字符串，一次点查的内存访问从 3～4 次降到 2 次，内存也省一份 key。
   重排 active（`drop_active_through`）本来就整表重建，换结构不影响那条路。
2. **活跃文件的命中读不走系统调用**：例如把已写入并 flush 过的前缀映射进来（按块增量 remap），或点查先看写缓冲 / 一小块最近读缓存。
   1024 key 的基准里这一项占 38%。
3. **CRC 的零头**别逐字节查表（slice-by-8，或把头尾并进折叠）。
4. **get 路径去掉每次的 malloc / free**（`DataFile::read` 已经是线程局部缓冲了，剩下的看着在 `GetResult` / doc 解码一侧）。
5. 回归 / 基准：`BM_Cask_Get_Hot` 加一档大 keyspace（比如 40 万未落盘行），把「命中耗时随行数涨」钉成曲线。

## 回没回

**libbitcask（2026-11-23，S65，12.0.3 已发版）**：已修建议 1、2、5，建议 4 随 2 一并解决，建议 3 没做。

- 建议 1：memdelta 点查索引换成开放寻址的平表（槽里放哈希 tag + 行下标，key 不另存，直接比行里的 key）。
- 建议 2：活跃文件的命中读走只读映射。映射一次建好、预留到 `max_file_size` 之后，文件增长不用重映射；只认已 `pwrite` 出去的字节，界外退回 `pread`。仅 Linux / FreeBSD。
- 建议 4：映射读是零拷贝，view 路径上原来 pread 那一路的拷贝与分配随之没有了。
- 建议 5：`BM_Cask_Get_Hot` / `_View` 加了 10 万 / 40 万未落盘行两档。
- 建议 3（CRC 零头）没做：约占改前一次命中的 5%，动校验和内核的风险与收益不相称。

实测 `BM_Cask_Get_Hot_View` 命中一次：1 千 / 10 万 / 40 万未落盘行 857 / 1540 / 1828 ns → **138 / 435 / 648 ns**。C API、盘上格式、C++ ABI 不变。下游要注意的两点：活跃文件被点查过的页计入 RSS（文件页，可回收）；映射读遇到 I/O 错误是 SIGBUS——都与 sealed 文件既有的映射读相同。回归 `DataFile.GrowingMap*`、`OkiRangeTest.PointGetMatchesModelAcrossIndexGrowthRebuildAndRoll`。落盘之后走 run 的那条读路径没动、没量。
