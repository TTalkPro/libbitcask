# S43 OKI-only Rollout 报告（6.9.0 发布前）

> 日期：2026-11-15。结论：**通过 rollout gate**。

## 1. 架构与迁移语义

- `.data` 仍是唯一持久真相；OKI（`kv.oki.manifest` + `kv.oki.seg-*`）
  是唯一查找/提交权威。旧内存 KeyDir 与 `kv.keydir.ckpt` 写路径退役。
- 新库不生成 `.hint`；可写打开会惰性清理历史 hint。旧 hint 只是
  不可信缓存，损坏、孤儿或删除都不改变恢复语义。
- 健康 Level B v4 提交走 data-only streaming replay；坏 manifest、
  坏 run、越界水位或旧 Level A 形态显式 fallback/受控重建。
- 旧库兼容路径：带 hint、v3 manifest/BCKS、无 manifest、manifest
  tmp 残件、run version 99、数据 torn tail 都在 rollout gate 中重开。
  目标目录不回写 hint/BCKS；旧二进制缺少辅助缓存时可退慢速
  data fold，但新格式 manifest 本身保留 v1-v3 reader 兼容面。

## 2. 新增 rollout gate

`tests/s43_rollout_gate_test.cpp` 固化 7 项验收：

1. 480 步随机 reference model：put/overwrite/remove/reinsert、TTL、
   atomic batch、sync/checkpoint/merge、close/reopen 后对拍 get/range；
2. legacy hint、interrupted manifest tmp、corrupt manifest、
   unavailable/corrupt run 与 torn data tail 的迁移/损坏矩阵；
3. K1/K2：data sync 后 OKI 未提交，tail replay 不丢不串；
4. K3/K4：run 先于 manifest commit 失败，旧 commit 保持、pending 可恢复；
5. K10：逐写 flush 边界恢复；
6. K12/K13：merge output 已完整落盘但 manifest 回退，重开按 ord
   幂等收敛，无悬空 loc、无 key 丢失；
7. K14/K15：merge commit 后 close/checkpoint 前崩溃，新旧 loc 语义一致。

K5-K9 由 OKI flush、BCOM codec、checkpoint 与 recovery 专项用例继续
守门；merge/relocation 竞态由 `OkiLevelBTest`、`MergeConcurrentWriterTest`
与相关 recovery 面覆盖。

## 3. Release benchmark

主机：本地 i9-13900H（8C/8T，Linux，约 3.0 GHz）；构建：
`build-gcc-release`，GCC Release、`BITCASK_LTO=ON`、
`BITCASK_NATIVE=OFF`；Google Benchmark `--benchmark_min_time=2s`，
单次连续采集；Zipf 为独立单基准采集。原始 JSON/日志在
`bench-results/s43-10/`。

| 基准 | 结果 | 说明 |
|---|---:|---|
| `BM_Cask_Get_Hot` | **806 ns/op**，1.24M ops/s | 热点查仍为百 ns 量级 |
| `BM_Cask_Get_ColdOki/100000/0` | **4.82 µs**，p50 4.57 µs / p99 7.41 µs | 冷块首次定位的量化上限 |
| `BM_Cask_Get_ColdOki/100000/1` | **3.98 µs**，p50 3.73 µs / p99 6.62 µs | 块缓存后明显收敛 |
| `BM_Cask_Put_Overwrite/1024` | **1.375 µs/op** | 小型覆盖写 |
| `BM_Cask_Put_Overwrite/100000` | **1.713 µs/op** | 更大覆盖写约 +24.5% |
| `BM_Cask_WriteProbe_Uniform/100000` | **2.707 µs/op** | 均匀写相对热覆盖写约 +90.8%，量化可接受 |
| `BM_Cask_Put_Zipfian/100000` | **1.599 µs/op** | bounded Zipf(s=1)；比同 keyspace 均匀端到端覆盖写 -6.7% |
| `BM_Cask_RangeScan_OKI/100000/16` | **23.9 ms** | 6,250/100,000 命中 |
| `BM_Cask_RangeScan_OKI/100000/256` | **1.21 ms** | 更长 range 后单点摊销更低 |
| `BM_Merge_Throughput/20000` | **31.4 ms**，60.7 MiB/s | 636k rows/s |
| `BM_Cask_Open_Snapshot` | **7.10 ms** | 健康 manifest/run 开库 |
| `BM_Cask_Open_FullFold` | **11.7 ms** | 最坏 data rebuild；约为 snapshot 1.64× |
| `BM_Oki_OpenRssProbe/100000` | **10.75 MiB RSS 净增** | 当前健康开库常驻上界，含 allocator 保留页与块缓存 |
| `BM_Oki_OpenRssProbe/1000000` | **108.1 MiB RSS 净增** | 1M 档健康开库上界；不是最坏 rebuild 峰值 |

端到端性能表不是同机旧 KeyDir 的严格 A/B；本报告把 S43 的最终形态作为
rollout 门槛记录。冷点查与均匀写退化已显式量化，满足“不接受未量化
回退”的 S43-0 约束。

### 3.1 RSS 与磁盘核算

RSS A/B 在同机连续采集，基线为 S43 前提交 `cce59d4`（6.8.1，GCC
Release、LTO OFF），新端为当前 GCC Release/LTO；两侧使用同一
`BM_Oki_MemProbe/100000` 工作负载（100k 独立 key、64B value），
各 3 次重复。基线含旧内存 KeyDir；新端无 KeyDir/BCKS 常驻哈希。

| 指标 | 6.8.1 baseline | 6.9.0 | 变化 |
|---|---:|---:|---:|
| memdelta RSS 净增 | **26.37 MiB**（均值） | **7.60 MiB**（均值） | **-18.78 MiB / -71.2%** |
| rebuild 峰值 RSS | **37.14 MiB**（均值） | **17.60 MiB**（均值） | **-19.54 MiB / -52.6%** |

原始 JSON/日志在 `rss-a-b-memprobe-{before,after}.{json,log}`。1M
当前端探针另见 `rss-memprobe-1m.*`；其 201k memdelta 行是自动 flush
后的尾段，不是全库常驻规模。上述 RSS 差值是同构 KV 探针的实测 A/B；
检索文档负载仍含 DocMap/search 内存，不能外推为零开销。

merge 磁盘核算来自新增 `BM_Merge_DiskAccounting/20000`（20k 覆盖写、
100B value、256KiB file roll；`/tmp` tmpfs；CPU 79.8 ms）。merge
只覆盖 18,702 条 eligible sealed 记录，active 尾部不合并：

| 账目 | 结果 |
|---|---:|
| data 总量 before/after | **5.1668 / 5.1668 MiB**；retired sealed **2.5834 MiB** 被 merge 输出等量替换 |
| `MergeStats.bytes_written` | **2,532,362 B** |
| OKI run before/after | **0.2882 / 0.5616 MiB**（增量 **0.2734 MiB**） |
| OKI manifest before/after | **1,706 / 519 B** |
| hint 文件 | **0 / 0**（新库不生成） |
| value 写放大口径 | `(data total delta + OKI delta) / kept value bytes` = **0.1533** |

口径说明：该分母是 `records_kept × 100B`（1,870,200B）的用户 value
字节，不含 key/ord/loc/data record header；因此它衡量 merge 造成的
额外落盘增量，不等同输出物理字节/逻辑 payload。原始账本在
`merge-disk-accounting.*`。

hint 删除收益另做 100k/key20B 迁移 A/B：旧 6.8.1 建库后
`*.hint` 为 **1 文件 / 3,200,012B**；6.9.0 可写打开迁移后为
**0 文件 / 0B**。同目录 OKI run 为 1,319,099B；该 run 在旧库中也已
存在，因此 hint 清理的直接物理回收就是 3.05MiB，不再把它重复抵扣
OKI 成本。原始命令输出在 `hint-disk-a-b.log`。

## 4. 验证账

| Gate | 结果 |
|---|---|
| GCC Release full ctest | **808/808 passed**（810 configured；2 RSS probes disabled） |
| ASan + UBSan full ctest | **808/808 passed**（同 2 disabled probes） |
| TSan full ctest | **807 executed / 0 failed**；`IndexPoolMultiLib.ThreadCountIndependentOfLibCount` 平台 skip，2 RSS probes disabled |
| `-Werror` 全树 | GCC Release build **207/207 targets** 完成，0 warning/error |
| Clang Debug/Release | Clang 21 `-Werror` 均为 **808/808 passed**（同 2 disabled probes；Release 测试构建关 LTO，规避 linker LTO 插件加载 C test object） |
| S43 rollout gate | Release/ASan 均 **7/7 passed** |
| 迁移与损坏 | rollout gate 第 2 项 + migrate/recovery/scanner 专项全绿 |

完整日志保留在 `/tmp/asan-s43-10-final.log`、
`/tmp/tsan-s43-10-final.log`、`/tmp/werror-s43-10-build.log`、
`/tmp/clang-debug-s43-10-final.log`、
`/tmp/clang-release-s43-10-final.log`；GCC 全量日志见
`/tmp/gcc-s43-10-final.log`，RSS/磁盘账补齐后的复测日志见
`/tmp/gcc-s43-10-final-rssdisk.log`，后续可用
`ctest --test-dir build-gcc-release` 复现。

## 5. 发布判断

- 版本取 **MINOR +1：6.9.0**；公共 C++/C API ABI 兼容，
  `SOVERSION` 保持 6。
- 磁盘布局为单向架构收敛：OKI manifest/run 必需，hint/BCKS 辅助面
  可缺失、可清理、不再生成。回退手段不是写回旧缓存，而是保留 v1-v3
  manifest reader 和 data 唯一真相。
- 通过。默认路径可随 6.9.0 rollout；旧库升级前建议备份或使用
  `Cask::backup()`，升级后首次 checkpoint 完成 v4 收敛。
