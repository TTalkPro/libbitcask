# 全表扫描逐条 `pread` 取值（每行约 1 µs，是有序 range 的 2～3 倍）；range 打开预取反而更慢

**报的人**：keelson（clj-bitcask 量 `scan-entries` / `range-entries` 时发现；libbitcask 12.1.0 `1caee1a`，Linux x64，GCC Release，keelson 的原生构建参数）。
**严重度**：🟢 性能，不报错，结果正确。全表扫描 / 导出 / 统计类负载受影响；点查与 range 不受影响。

## 症状

不报错。20 万个 key、200 B 的值，C API 直接遍历全表（批 256，每条读出值后 `*_entry_ex_free`），页缓存全热，取后两轮：

| 场景 | 全表扫描 `bitcask_iter_*_ex` | range `bitcask_range_iter_*_ex` | range，`prefetch = 256` |
|---|---|---|---|
| 按 key 顺序写入，刚写完 | 955～1042 ns/行 | **332～343 ns/行** | 631～970 ns/行 |
| 按 key 顺序写入，重开后 | 930～944 ns/行 | 561～930 ns/行 | 897～1005 ns/行 |
| 乱序写入，刚写完 | 1301～1323 ns/行 | 653～671 ns/行 | 958～1101 ns/行 |
| 乱序写入，重开后 | 1288～1322 ns/行 | 846～863 ns/行 | 905～995 ns/行 |

- 全表扫描在所有情况下最慢，**与写入顺序无关**（按 key 顺序写入、数据文件是顺序的，照样慢）⇒ 不是读位置乱，是每行的固定开销。
- range 打开预取（`prefetch = 256`，缺省 `prefetch_threads` = min(hw, 4)）在每一格都比不开慢。

perf（同一程序，只换遍历方式、各跑 5 遍；写入阶段相同）：只扫描时系统调用（`__internal_syscall_cancel`）占 **31%**，只 range 时 **5%**；
扫描另有 `malloc` / `free` / `memmove` 各约 5%。预取那一档的 perf 里能看到 `pthread_create` / `clone3`。

## 位置

- 扫描取值：`src/cask/cask_iter.cpp:175` `df->read(e.offset, e.total_sz)`——直接 `DataFile::read`，每条一次 `pread` 加一份拷贝
  （旧称「keydir 扫描」；keydir 内存索引已于 7.0.0 退役，这条路径实际是 OKI 归并视图上的全表快照扫描）；
  点查与 range（不预取）走 `read_doc`，活跃文件经 S65 的只读映射、封口文件经 P6 映射，不发系统调用。
- range 预取：`src/cask/cask_range_iter.cpp:148` `fill_prefetch`——每批把 ≤ prefetch 个 key 交给 JoiningPool 分段并发 get。页缓存热、值小时，
  起线程 / 汇合的固定开销大于并行省下的取值时间。

## 怎么撞上的

keelson 给 clj-bitcask 的常用接口做 JFR 采样（各接口原生帧占比都在 87～99%），注意到 `scan-entries` 每行 1.04 µs、`range-entries` 每行约 0.45 µs，
时间几乎全在原生侧；改用 C API 直接测确认（附程序）。

## 影响面

- 用全表扫描做全表导出 / 统计 / 重建派生数据的上层：比改用 range 慢 2～3 倍（刚写完的库）或 1～1.6 倍（重开后）。
- 文档写着「`prefetch` > 1 时归并一批 key 后并发取值，只改时机」，调用方很自然会以为打开更快；在热缓存、小值的常见情况下它是负收益。

## 复现

完整 C 程序（编译：`cc -O2 -o scan scan.c -I<libbitcask>/c_api -L<dir> -l:libbitcask.so -Wl,-rpath,<dir>`；用法 `./scan <空目录> 200000 200 seq|shuffle [scan|range]`，
第 5 个参数给了就只跑那一种遍历 5 遍，供 perf 用）：

```c
/* keydir 扫描（bitcask_iter_*_ex）vs 有序 range（bitcask_range_iter_*_ex）每行耗时。
 * 用法：./scan_repro <空目录> <key 数> <值字节> <seq|shuffle>   （批 256，与 keelson 缺省相同） */
#include "bitcask_kv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e9 + ts.tv_nsec; }
static bitcask_fault_t fault;
#define B 256

static bitcask_t* open_rw(const char* dir) {
    bitcask_t* c; bitcask_options_t o; bitcask_options_init(&o); o.read_write = 1;
    if (bitcask_open(dir, &o, &c, &fault) != BITCASK_OK) { fprintf(stderr, "open: %s\n", fault.detail); exit(1); }
    return c;
}

static double run_scan(bitcask_t* c, long* rows, size_t* bytes) {
    bitcask_iter_t* it; bitcask_iter_entry_ex_t e[B];
    double t0 = now_ns();
    if (bitcask_iter_start_ex(c, -1, -1, 0, (bitcask_slice_t){NULL, 0}, 0, NULL, &it, &fault) != BITCASK_OK) { fprintf(stderr, "iter: %s\n", fault.detail); exit(1); }
    long n = 0; size_t by = 0; int k;
    while ((k = bitcask_iter_next_batch_ex(it, e, B, &fault)) > 0) {
        for (int i = 0; i < k; i++) { by += e[i].base.value.size; bitcask_iter_entry_ex_free(&e[i]); }
        n += k;
    }
    if (k < 0) { fprintf(stderr, "iter next: %s\n", fault.detail); exit(1); }
    bitcask_iter_release(it);
    *rows = n; *bytes = by;
    return now_ns() - t0;
}

static double run_range(bitcask_t* c, size_t prefetch, long* rows, size_t* bytes) {
    bitcask_range_options_t o; bitcask_range_options_init(&o); o.prefetch = prefetch;
    bitcask_range_iter_t* it; bitcask_range_entry_ex_t e[B];
    double t0 = now_ns();
    if (bitcask_range_iter_start_ex(c, &o, 0, NULL, &it, &fault) != BITCASK_OK) { fprintf(stderr, "range: %s\n", fault.detail); exit(1); }
    long n = 0; size_t by = 0; int k;
    while ((k = bitcask_range_iter_next_batch_ex(it, e, B, &fault)) > 0) {
        for (int i = 0; i < k; i++) { by += e[i].base.value.size; bitcask_range_entry_ex_free(&e[i]); }
        n += k;
    }
    if (k < 0) { fprintf(stderr, "range next: %s\n", fault.detail); exit(1); }
    bitcask_range_iter_release(it);
    *rows = n; *bytes = by;
    return now_ns() - t0;
}

static void measure(bitcask_t* c, const char* state) {
    long rows; size_t by;
    for (int r = 0; r < 3; r++) {                       /* 第 1 轮预热页缓存，报后两轮 */
        double s = run_scan(c, &rows, &by);
        double g = run_range(c, 0, &rows, &by);
        double p = run_range(c, 256, &rows, &by);
        if (r > 0) printf("  %-10s scan %6.0f ns/行   range %6.0f ns/行   range(prefetch 256) %6.0f ns/行   （%ld 行，%zu B）\n",
                          state, s / rows, g / rows, p / rows, rows, by);
    }
}

int main(int argc, char** argv) {
    long n = atol(argv[2]); int vsz = atoi(argv[3]); int shuffle = strcmp(argv[4], "shuffle") == 0;
    char* val = malloc(vsz); memset(val, 'x', vsz);
    long* order = malloc(sizeof(long) * n);
    for (long i = 0; i < n; i++) order[i] = i;
    if (shuffle) { srand(7); for (long i = n - 1; i > 0; i--) { long j = rand() % (i + 1); long t = order[i]; order[i] = order[j]; order[j] = t; } }
    printf("libbitcask %d.%d.%d，%ld 个 key，值 %d B，写入顺序：%s\n", bitcask_version_major(), bitcask_version_minor(), bitcask_version_patch(), n, vsz, shuffle ? "乱序" : "按 key 顺序");
    bitcask_t* c = open_rw(argv[1]);
    for (long i = 0; i < n; i++) {
        char k[32]; int kl = snprintf(k, sizeof k, "key%08ld", order[i]);
        if (bitcask_put(c, (bitcask_slice_t){k, (size_t)kl}, (bitcask_slice_t){val, (size_t)vsz}, 0, &fault) != BITCASK_OK) { fprintf(stderr, "put: %s\n", fault.detail); return 1; }
    }
    if (argc > 5) {                                     /* 只跑一种遍历 5 遍（strace / perf 用） */
        long rows; size_t by;
        bitcask_close(c); c = open_rw(argv[1]);
        for (int r = 0; r < 5; r++) { if (strcmp(argv[5], "scan") == 0) run_scan(c, &rows, &by); else run_range(c, 0, &rows, &by); }
        printf("  only %s: %ld 行 × 5\n", argv[5], rows);
        bitcask_close(c);
        return 0;
    }
    measure(c, "刚写完");
    bitcask_close(c);
    c = open_rw(argv[1]);
    measure(c, "重开后");
    bitcask_close(c);
    return 0;
}
```

写入：`bitcask_put` 20 万次（`key%08ld`，200 B），按 key 顺序或打乱；测「刚写完」与「close 后重开」两种状态，每种 3 轮取后 2 轮。

## 建议（下游不替 libbitcask 拍板）

1. 扫描取值改走和点查同一条读路径（映射读，界外再退回 `pread`），或者在迭代器里持有当前文件的映射。
2. 预取：线程池常驻（或复用 `parallel_scan` 的池），别每批起线程；或者按值大小 / 是否命中页缓存自适应，小值直接串行；至少在文档里写明它只在冷缓存 / 大值时划算。
3. 扫描与 range 每条都 malloc 一份 key 与 value（`*_entry_ex_free` 释放）：批接口可以考虑给一块调用方提供的缓冲 / 视图版本，省掉每条两次分配。

## 回没回

已修前两条（S71，12.2.0 已发版）；第 3 条扫描半边已做（S75，未发版），range 半边待量测门槛。

1. 扫描取值改走与点查同一条读路径（`read_mmap`，界外退回 `pread`）；另外迭代器钉住的封口文件此前是不带映射打开的（`mmap_enabled = false`），
   重开后的扫描照样全走 `pread`——一并改成带映射打开，并恢复缺省预读（扫描大体按文件顺序取值）。附带的程序，热缓存每行 ns：

   | 场景 | 扫描 12.1.0 → 现在 | range（不预取） | range（prefetch 256） |
   |---|---|---|---|
   | 按 key 顺序，刚写完 | 949 → **214** | 350 → 320 | 650 → 350 |
   | 按 key 顺序，重开后 | 960 → **258** | 570 → 570 | 914 → 635 |
   | 乱序，刚写完 | 1230 → **370** | 640 → 635 | 865 → 725 |
   | 乱序，重开后 | 1214～1432 → **436** | 800～850 → 793 | 930～1124 → 865 |

   扫描现在比 range 快（扫描直接用归并视图里的位置；range 每个 key 再做一次点查定位）。
2. 预取：`prefetch_threads = 0`（缺省）时每批先串行取 32 条计时，估算并行省下的时间盖不过起线程的成本就整批串行；显式给线程数照旧并行。
   热缓存下从比不开慢 1.5～1.9 倍降到慢 5～15%（剩下的是攒批本身的开销）。文档（`api-c.md` / `api-cpp.md` / 头文件）已写明。冷缓存没测。
3. 每条两次分配：扫描侧已做（S75，未发版），range 侧待量测。
   （2026-11-23 补记：下游拍板接口形状——缓冲归库所有、借用视图、需保留则外部复制；新入口不动老 ABI；
   设计见 [`../doc/scan-batch-view-design-zh.md`](../doc/scan-batch-view-design-zh.md)，立项 S75。
   评审后改两步：扫描先行，Linux 实测 ≥ 10% 再做 range——理论上限只有约 15%，JVM 绑定端到端估计个位数，
   不值得在量到收益之前把两半都铺开。）
   - **扫描（已做）**：C++ `CaskIter::next_batch_view(max_n)` 交 `span<const EntryView>`，C 侧
     `bitcask_iter_next_batch_view(iter, entries, max_n, fault)`。遍历语义与 `*_next_batch_ex` 相同，`EntryView`
     字段是 `bitcask_iter_entry_t` 全集 + meta，只是 key / value / meta **借库内存储**：映射命中时 value 直达
     文件映射，`pread` 兜底时记录留在迭代器里，整批稳态零堆分配，没有也不需要 free 函数。
     有效期到本迭代器的下一次推进调用为止（`next` / `_ex` / `_batch` / `_batch_view` / `release`）；
     `bitcask_close` 不算推进，当前批读完再 release 即可，但 close 后不可再推进。要保留请当下复制。
   - **range（未做）**：`CaskRangeIter::next_batch_view` / `bitcask_range_iter_next_batch_view` 等 Linux 上
     扫描半边的实测数字（门槛 ≥ 10%）。
   - 顺带修了一个老问题：全表迭代器 `see_tombstones = true` 此前从不交出墓碑（开关没透传到 OKI 归并层，
     墓碑在归并层就被抵消），所以此前该开关下扫描结果与 `false` 完全一样；现在照常交出墓碑行。
   - Windows 上的覆盖：刚写完的库扫描全部走 `pread` + 记录 holder（活跃文件没有 growing map），映射零拷贝
     路径只有 Linux CI 覆盖；Windows 实测数字请在下游量。
