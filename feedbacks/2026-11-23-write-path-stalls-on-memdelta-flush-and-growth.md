# 写路径的长尾：memdelta 到阈值时在写者线程同步落段（每次 170～200 ms），memdelta 翻倍扩容时前台搬迁 / 重算哈希（52 万行时 10～13 ms）；合计约占连续写入时间的 10%

**报的人**：keelson（clj-bitcask 图层升 12.0.3 后量单条写边时撞上；libbitcask 12.0.3 `d995557`，对照 12.0.2 `ab9b51f`，Linux x64，GCC Release，keelson 的原生构建参数）。
**严重度**：🟡 性能，不报错，结果正确。均值被拉高约 10%，p99 / 最大延迟受影响最大（单次 170～200 ms）。两个版本都有，**不是 12.0.3 的回退**。

## 症状

不报错。同一个进程里连续做单条写（`bitcask_put_batch_atomic_cond`，1 op、无条件，300 B 的值，key 空间 10 万，全部在活跃文件 / memdelta 里），
逐次计时，超过 1 ms 的调用全部列出（下面是 12.0.3 的一次；12.0.2 同形）：

| 第几次写 | 耗时 | 推断 |
|---|---|---|
| 16 384 / 32 768 | 1.5 / 2.5 ms | 平表索引 `FlatIdx` 翻倍（12.0.3 新增） |
| 65 536 / 131 072 / 262 144 / 524 288 | 4.6 / 3.2 / 5.9 / 11.7 ms | memdelta 行数组翻倍搬迁 + 平表翻倍（12.0.2 也有：1.5～1.7 / 2.8～4.6 / 5.4～5.8 / 10.4～13.2 ms） |
| **646 025** | **168～204 ms** | memdelta 字节数到 64 MiB，写者线程内同步 flush |
| 678 794 / 711 562 | 1.1～1.4 / 2.2～2.4 ms | flush 后平表重建再翻倍（12.0.3） |
| **1 291 944** | **168～180 ms** | 第二次同步 flush |

150 万次写合计 3.6～4.4 s，其中 > 1 ms 的调用 9～15 次、共 0.37～0.41 s，**占 9～11%**（12.0.2 两次 10.0% / 9.0%，12.0.3 两次 9.9% / 10.3%）。
同一台机器、同一份程序、只换动态库，交替各跑两次。

对中位数没有影响：同一负载下 1 op 写 p50 约 1.8 µs、p99 4.5～5.7 µs、最大值落在上面那两次 flush 上（最大 155～165 ms）。

## 位置

- 同步 flush：`OkiState::kFlushRowLimit = 1u << 20` / `kFlushByteLimit = 64u << 20`（`include/bitcask/oki_state.hpp:387`），
  `should_flush()` 由写路径探询；命中后 `Cask::maybe_flush_oki`（`src/cask/cask.cpp:1045`）在写者线程里 `commit_oki_cut()`
  （先 fsync 活跃文件、再写 run 与清单），锁外站点经 `maybe_flush_oki_unlocked` 先拿 `write_mu_`（`:1062`）。注释写的是「罕见且有界」——
  300 B 值的单条写约每 64.6 万次一次，每次挡住全部写者 170～200 ms。
- 翻倍扩容：memdelta 行数组（`std::vector<DeltaRow>`）在 append 时翻倍搬迁（每行带 `std::string` key）；12.0.3 的 `FlatIdx::reserve_one`
  （`src/fileops/oki_state.cpp:232`）装填率到 1/2 时整表按行重算哈希重排。两者都在写路径上、持锁执行，耗时随行数线性涨。

## 怎么撞上的

keelson 升 12.0.3 后量图层单条写边（每次写边 = 两端邻接表头带 `[:ord n]` 条件的 2 op 原子批）。一开始看到「带 `ord` 条件的批」均值比无条件的
批多 4～6 µs，以为是条件检查慢；逐次计时后发现**条件本身几乎不花钱**（同一状态下交替跑两轮：1 op 写 p50 1929 / 3284 vs 1799 / 1819 ns，2 op 写 p50 4136 / 2667 vs 3083 / 2344 ns——第二轮 1 op 那格的 p50 也偏高，说明连中位数都受前后停顿与缓存状态影响，但没有一致的方向；
只含 check 的批 ord 条件约 0.25 µs、absent 约 0.2 µs），均值的差全来自哪一段测量碰上了上面这几次停顿。这里把这个误判也写出来，免得上游再走一遍。

## 影响面

- 连续写入的服务（图装载、批量导入、写多的 agent 记忆）：约每 64 MiB memdelta 一次 170～200 ms 的全写者停顿，p99.9 / 最大延迟直接由它决定；
  用条件写做乐观并发的上层（keelson / bitcask 图层），停顿期间别的写者排队，冲突重试的退避也会被拉长。
- 翻倍扩容的停顿随 memdelta 规模涨：52 万行时 10～13 ms，按线性外推到 1M 行阈值前一次约 20 ms（未实测）。
- 只读负载、低写入率的库不受影响。

## 复现

```c
/* cc -O2 -o stall stall.c -I<libbitcask>/c_api -L<dir> -l:libbitcask.so -Wl,-rpath,<dir>
 * ./stall <空目录> 1500000 300 100000 */
#include "bitcask_kv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e9 + ts.tv_nsec; }
int main(int argc, char** argv) {
    long n = atol(argv[2]); int vsz = atoi(argv[3]); long space = atol(argv[4]);
    char* val = malloc(vsz); memset(val, 'x', vsz);
    bitcask_fault_t fault; bitcask_t* cask; bitcask_options_t opts;
    bitcask_options_init(&opts); opts.read_write = 1;
    if (bitcask_open(argv[1], &opts, &cask, &fault) != BITCASK_OK) return 1;
    double t0 = now_ns(), stall = 0; int stalls = 0;
    for (long i = 0; i < n; i++) {
        char k[32]; int kl = snprintf(k, sizeof k, "key%ld", i % space);
        bitcask_cond_op_t op = {0, 0, 0, {k, (size_t)kl}, {val, (size_t)vsz}, NULL};
        double a = now_ns();
        if (bitcask_put_batch_atomic_cond(cask, &op, 1, NULL, NULL, &fault) != BITCASK_OK) return 1;
        double d = now_ns() - a;
        if (d > 1e6) { stalls++; stall += d; printf("stall #%d: write %ld, %.1f ms\n", stalls, i, d / 1e6); }
    }
    double total = now_ns() - t0;
    printf("total %.2f s, %d stalls > 1 ms, %.2f s (%.1f%%)\n", total / 1e9, stalls, stall / 1e9, 100 * stall / total);
    bitcask_close(cask);
    return 0;
}
```

## 建议（下游不替 libbitcask 拍板）

1. flush 不挡写者：到阈值时把当前 memdelta 冻结成只读段（读路径已经要合并多段），写者立刻转向新的 memdelta，落段交给后台线程
   （LSM 的 immutable memtable 做法）；只有冻结段积压超过上限才回到现在的同步背压。持久性约束（「固化的 run 不得引用未持久字节」）可以在后台线程里先 fsync 再落段。
2. 行数组不整体搬迁：memdelta 的行改成分段存储（固定大小的块链 / `std::deque` 式），append 不再触发 O(行数) 的移动；行下标仍可由「块号 + 块内偏移」得到，平表里存的「行下标 + 1」不用变。
3. 平表渐进式扩容：翻倍时新旧两张表并存，每次插入顺带搬迁一小段旧槽（查询两张表都查），把一次 O(行数) 的重排摊开；或者按阈值（1M 行）一次预留到位，代价是空库多占 16 MiB。
4. 至少给个口子：C API 能调 flush 阈值、或能在低峰期显式 `checkpoint` 提前落段（`bitcask_checkpoint` 已有），让上层自己选停顿发生的时机——文档里说明「每 64 MiB memdelta 一次同步落段」。

## 回没回

待处理。
