# S42 Rollout 基准报告（6.8.1 发布前）

> 日期：2026-11-14。被测提交：`2347cde`（S42 DocMap Live-Only Store）。
> 结论：**通过 rollout gate**。热点覆盖写内存已经与历史写入量解耦；低 churn
> live set 每文档约 162–170B，符合设计预期数量级；本次远端 Release 基线未见
> 端到端热路径异常退化。

## 1. 测试环境

| 项 | 值 |
|---|---|
| 主机 | `david@192.168.2.4:3022`，`/mnt/storage/workspace/libbitcask` |
| CPU | AMD Ryzen 7 9700X，8C/16T |
| 内存 | 91GiB |
| 系统 | Debian 13，Linux 6.12.115+deb13.1-amd64 |
| 编译器 | g++ 14.2.0 |
| 构建 | CMake 3.31.6，Release，`-O3 -DNDEBUG`，`BITCASK_LTO=ON`，`BITCASK_NATIVE=OFF` |
| CPU governor | performance |
| 构建开关 | `BUILD_TESTING=ON`，`BITCASK_BUILD_BENCHMARKS=ON` |

## 2. 功能回归

远端完整 `ctest` 结果：

```text
100% tests passed, 0 tests failed out of 821
Total Test time (real) = 5.91 sec
```

CMake 注册了 823 项；实际执行 821 项。两项 RSS probe
（`CheckpointRecoveryTest.S30RssProbe`、`CheckpointRecoveryTest.S40KeyRssProbe`）
默认 disabled，未计入执行结果。这与本地 Release、ASan、TSan 各 **821/821**
的收口结果一致。

## 3. S42 内存探针

内存探针只取单次执行的 RSS 净增量；`Time/CPU/Iterations` 列不是本表判断对象。
热点覆盖写旧行为按旧结构推算约为 `53B/历史 ord`，即 1M 次覆盖写会留下约
**50MiB+** 的 DocMap 常驻结构。

| 基准 | live docs | RSS 净增 | 每单元 | 结论 |
|---|---:|---:|---:|---|
| `BM_Docmap_HotOverwrite_MemProbe/1000000` | 1 | **0.0625 MiB** | 0.0655 B/write | 通过；从设计推算的 50MiB+ 降到 RSS 页粒度以下 |
| `BM_Docmap_LiveSet_MemProbe/100000` | 100k | 16.449 MiB | **172.5 B/live doc** | 通过；与设计预期同数量级 |
| `BM_Docmap_LiveSet_MemProbe/1000000` | 1M | 161.691 MiB | **169.5 B/live doc** | 通过；100K/1M 两档稳定 |

说明：探针口径包含 `ext2ord_`、live row、hash 表空槽、allocator 管理费和
RSS 页粒度；key 形态为 `live-key-<number>`。因此数值高于纯 `LiveRow` 字段和。
设计文档中的 `100–160B` 是量级预估，不代表逐字节上限。1M 档稳定在 169.5B，
可接受；若后续要把低 churn 全量集再压低，方向是 key arena 与 row pool，而不是
回退到按历史 LSN 展开的平坦数组。

## 4. Release 性能基线

端到端基准使用 Google Benchmark `--benchmark_min_time=2s`。本次远端没有编译
6.8.0 旧实现作同机 A/B，因此本节是 **6.8.1/S42 远端新基线**，不声明旧版本
同机百分比收益。

| 基准 | 结果 | 备注 |
|---|---:|---|
| `BM_Cask_Put_Overwrite` | **578 ns/op**，1.730M ops/s，211.2 MiB/s | KV 覆盖写热路径 |
| `BM_Inverted_QueryThroughputBOW/threads:1` | **3.74 µs/query**，267.7k iter/s | 每线程每迭代口径；不是聚合 QPS |
| `BM_Cask_Open_Snapshot` | **2.335 ms** | 20k key 快照开库 |
| `BM_Cask_Open_FullFold` | **3.441 ms** | 快照删除后的全量 fold 对照 |
| `BM_Checkpoint_KeydirSave/100000` | **0.141 ms**，708.9M items/s | KeyDir 快照保存 |
| `BM_Checkpoint_KeydirLoad/100000` | **0.016 ms**，6.301G items/s | KeyDir 快照载入 |

快照开库比全量 fold 快约 32%；`DocMap` live-only 化没有把 Cask 覆盖写、单线程
BOW 查询或开库路径推入异常区间。上述数据作为后续同机对照基线。

## 5. Gate 判断

| Gate | 判断 | 依据 |
|---|---|---|
| 功能正确性 | **通过** | 本地 Release/ASan/TSan 821/821；远端 Release 821/821 |
| 热点覆盖写内存 | **通过** | 1M 写入、1 live doc，RSS 净增 0.0625MiB |
| 低 churn live set | **通过** | 100K/1M 分别约 172.5B、169.5B/live doc |
| 热路径性能 | **通过** | KV 覆盖写 578ns/op；单线程 BOW 查询 3.74µs/query |
| 恢复/开库 | **通过** | Snapshot 2.335ms、FullFold 3.441ms；恢复测试全绿 |
| 盘上兼容性 | **通过** | checkpoint/docmap 格式零改动；旧目录无 migration |

## 6. 运维语义

- 库版本取 **PATCH +1（6.8.1）**；`SOVERSION` 保持 `6`。
- 盘上格式不变：旧数据目录可直接打开，不需要 migration。
- DocMap checkpoint sidecar 仍逐字节兼容；行为变化只在内存表示。
- `SearchHit.ord` 仍为全局 LSN，不是数组下标。

## 7. 原始结果位置

远端保留：

```text
/mnt/storage/workspace/libbitcask/bench-results/6.8.1/perf.log
/mnt/storage/workspace/libbitcask/bench-results/6.8.1/perf.json
/mnt/storage/workspace/libbitcask/bench-results/6.8.1/docmap-probes.log
/mnt/storage/workspace/libbitcask/bench-results/6.8.1/docmap-probes.json
```
