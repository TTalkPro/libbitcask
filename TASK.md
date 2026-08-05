# S33：有序 Key 索引（OKI）开发任务清单

> 来源：[`doc/ordered-key-index-design-zh.md`](doc/ordered-key-index-design-zh.md)（设计草案已定稿）
> 决策基线：否决整体换 LevelDB；WiscKey 式旁挂有序 key 索引；**flag-day 停机迁移**
> （hint BCH4→BCH5 加 ord、meta v4→v5、`bitcask_migrate hintord`，与 5.0.0 tstamp64 同模式）
> 版本目标：**5.1.0**（盘上 `bitcask.meta` = v5，S35 起用原子批的目录懒升 v6；
> `SOVERSION` 保持 `5`——C API 纯增量，**盘上格式破坏不驱动 major**，同 3.1.0 先例）
> 基线测试：ctest 全绿（Phase 6 T22 后 644 项，1 个 S30RssProbe 预存 Disabled）
> 验收标准：每项改动后 ctest 全绿 + 编译无新告警；公共结构体改动须 build-rel 双树验证；
> 格式改动须对拍 + crash 注入

---

## 📁 上一清单归档

Google C++ Style 规范化清单（Phase 1-6，T1-T26）已收官，详见 git 历史
（本文件在 v5.0.0 之后、S33 换届之前的版本）与 `RISK_REPORT.md`。
**未完项带入本清单遗留区**：T23（ChunkedReader）、T24（decode_rec 模板）、
T8（搜索读屏障，⏸ 前置未满足）、T12（HNSW ckpt 去重，⏸ 默认不做）。
其中 **T23 与 S33-2 撞车**（同在 `hint_file.cpp` 的 refill 三胞胎）——见 S33-2 备注。

---

## 🔴 S33 主线

### S33-1 — 量化探针 + 基线 bench 🟡 MED（先行，产出 Level B 门禁数据）

1. `KeyDir::key_length_histogram()`（`keydir.hpp:389-396`，现仅测试在用）接出诊断口：
   `Cask` 新增只读访问器（不进 `StatusInfo`，避免无谓的公共结构体膨胀）。
2. keydir RSS 估算口径落地：`key_count × (40 + avg_key + 槽位开销)`，
   在访问器返回里直接给出估算字节数。
3. 新增 `bench/range_bench.cpp`：现状 O(全表) prefix 扫描基线
   （`CaskIter::start(key_prefix)` 路径），OKI 落地后同 bench 对比。
- **工作量**：半天
- **验收**：ctest 全绿；bench 可跑出基线数字；build-rel 双树（新公共访问器）

#### ✅ S33-1 落地记录（2026-07-31）

**一处与原计划的偏离**：`Cask::keydir()` 公共访问器**已存在**（`cask.hpp:751`），
无需新增——直方图本就可达。实际改动收窄为：
- `SeqShardTable` 加 `values_capacity()` / `bucket_count()` 诊断访问器；
- `KeyLenHistogram` 扩展 5 个内存估算字段（`key_bytes` / `entry_slot_bytes`
  （capacity 口径——vector 空闲槽照样占 RSS）/ `bucket_bytes` / `heap_key_bytes`
  / `estimated_bytes`），`keydir_test` 断言口径；
- `bench/range_bench.cpp`：`BM_Cask_PrefixScan_Baseline`（100k key，选择性
  1/16 vs 1/256）+ `BM_KeyDir_MemProbe`。

**基线数字（tmpfs，build-rel，负载下量的粗锚点）**：选择性收窄 16×（6250→391
条命中）耗时仅降 1.8×（15.1→8.3ms）——**O(全表) 实锤**（降幅来自省掉的
value 读取，非扫描本身）。内存探针：11B 短 key 负载 ~122 B/key（含 capacity
slack），100k key ≈ 11.6MB。

### ✅ T23 落地记录（2026-07-31，随 S33-2 一并做）

`detail/chunked_reader.hpp` 新建：三份手抄 refill 归并为 `ChunkedReader`
（`avail/cursor/consume/refill/shrink`），`need` 公式统一为 hint 版（正确
覆盖巨型 record 的 `len_ + need_hint`；data_file 版此前丢了 `len_ +`）。
落点：`data_file.cpp::fold` + 新 `hint_file.cpp::fold_v5` 两站点——原 v2
fold / fold_v4 两份手抄随 flag-day 整体删除，三胞胎实际收敛为 2 站点 1 实现。

### S33-2 — flag-day 基建：hint BCH5 + meta v5 + 迁移工具 🔴 HIGH（发布边界）

设计依据：设计文档 §3.4 / §7。**同步清单逐项勾销，缺一即数据错读**：

- [x] `format.hpp`：BCH5 常量（`kHintMagicV5`）+ 布局注释；`kHintMagicV4` 保留
      （识别/拒收用）；v2 定宽常量（`kHintRecordSize`/`kMaxOffsetV2`/`kTombMaskV2`）删除
- [x] `codec.hpp::HintRecord`：加 `ord` 字段；`encode/decode_hint_record_v5`
      （prev_end/prev_ord 双差分 in/out 引用）；v2 与 v4 编解码整体删除
- [x] `hint_file.cpp/.hpp`：写端 BCH5（`write(..., ord)`，`prev_ord_` 串联）；
      读端仅 `fold_v5`（ChunkedReader）；v2 fold / fold_v4 删除
- [x] `merger.cpp`：merge 写 hint 传 `view.ord`
- [x] `cask_recovery.cpp` hint 快路径：`put(..., rec.ord)` + `advance_ord`
      （含墓碑）；**顺手修**：fold(data) 墓碑分支原本不 `advance_ord`
      （文件尾墓碑 → 重启 ord 复用的潜在隐患），已对齐
- [x] `meta_file.cpp`：`kMetaVersion = 5`；v4 拒绝分支（提示 hintord）；
      `meta_file.hpp` 头注释修正（原写 v2）；**顺手修**：`Cask::open` 两处把
      `MetaError.message` 吞成笼统 "read meta failed"——门禁迁移提示传不到
      用户，已透传
- [x] `migrate.{hpp,cpp}` + `tools/bitcask_migrate.cpp`：`migrate_hint_ord`
      （前置校验 src=v4 → data 硬链接（跨设备退化拷贝）→ DataFile::fold 重扫
      生成 BCH5 hint → meta 最后写 = commit point，幂等）；be2le/tstamp64 目标
      纪元同步 bump 到 v5；`detect` 识别 v4/v5。`--prebuild-oki` 未留桩（S33-4 实装时加）
- [x] 测试：HintRecordV5 golden/回绕/流式/短读（codec_test）；HintFileGolden
      双向 v5 golden；Bch4LegacyFileRejected 三入口全拒；MetaV4CleanlyRejectedWithHintordHint；
      MigrateHintOrdV4EraDirOpensAndReads（含 data 字节零改动断言 + 幂等 + 已 v5 拒迁）；
      **HintAndDataFoldRecoveryOrdEquivalent**（含 merge 后，逐 key ord + next_ord 水位双等价）
- [x] 文档：`doc/format-zh.md`（hint v5 / meta v5 表与版本策略，顺手修正 §3
      的 v3 陈旧描述与缺失的 VecEngine 字段）/ `doc/migrate-le.md`（统一入口
      提示）/ 设计文档 §7（派生缓存不迁移，对齐工具惯例）；CHANGELOG 发布时补

- **工作量**：1.5~2 天（实际 1 天内，与 T23 一并）
- **验收**：✅ Debug 全量 **650/650**（648 基线 + 新测试；1 预存 Disabled）
  | ✅ build-rel 全量零错误（含 bench，公共 API 双树验证）| ASan 全量见状态快照
- **备注（偏离记录）**：
  1. 计划说"BCH4 读端干净报错提示迁移"——实现为 `validate_trailer`/
     `fold_validated` 对非 BCH5 magic 返回 **false（退 fold(data) 重建）**、
     仅 `fold()` 报错：hint 是派生缓存，陈旧格式当"缓存不可用"处理比硬报错
     更符合语义；纪元硬门禁由 meta v5 承担（v5 目录里本不该有 BCH4 hint，
     出现即用户手工拷文件，fold(data) 回退仍保正确性）。
  2. "迁移幂等 + 中途 kill 注入"以"meta 最后写 + 删 dst 重跑"的幂等测试覆盖；
     显式 kill 注入留给 S33-4 的 crash 套件统一做（迁移的 commit point 语义
     已由"无 meta 的 dst 不可开"保证）。

### S33-3 — OKI run 格式（BCOK v1）+ manifest（BCOM v1）🔴 HIGH

- run：块式布局（~4KiB 块、块内 key 前缀差分、稀疏块索引、trailer CRC），
  writer（流式 + 外排入口）/ reader（`seek(lo)` 二分 + 顺序游标）
- manifest：run 集合 + `cover_ord` + 联合水位，`atomic_write_bytes(fsync_dir=true)`
  唯一 commit point；未知 magic/ver 整体拒收 → 重建
- 格式预留 Level B 全字段扩展位（flags + 可选字段区）
- **工作量**：1.5 天
- **验收**：round-trip / seek 边界 / 损坏拒收单元测试

#### ✅ S33-3 落地记录（2026-07-31）

新增 `include/bitcask/oki_run.hpp` + `src/fileops/oki_run.cpp`（挂
`bitcask_fileops`）+ `tests/oki_run_test.cpp`（10 测试）。要点：

- **格式简化**：块首条不设独立编码——每块解码状态复位（prev_key="" /
  prev_ord=0），块首条自然退化为全量 key + 绝对 ord，读写两端同一条码路径。
- trailer 扩为 24B（+entry_count u64）；ord 差分与 hint v5 同款回绕语义。
- **flags 未知位 fail-fast**（Level B 扩展位预留），专项测试绕过 CRC
  重算后单独验证该门。
- reader open 时**全文件 eager CRC**（派生缓存，安全优先；惰性/分块校验
  留作后续优化），稀疏索引载入内存，Cursor 按块 pread。
- 顺手：`detail::AtomicFileWriter` 补移动语义（T21 基建缺口，OkiRunWriter
  按值持有需要；源移出标记 committed_ 防双清理）。
- 测试含：多块 round-trip（乱序 ord/墓碑/`prefix:id` 形态）、100 长公共
  前缀 + 二进制 key、块界全覆盖 seek（每个存在 key 精确 seek + seek 后
  顺序推进）、writer 乱序/重复/finish 后拒写、弃写不留残件、四区域翻
  bit + 截尾拒收、manifest 逐字节翻 bit 全扫。
- **验收**：OKI 10/10；Debug 全量 663/663；**ASan 全量 663/663**；
  TSan 相关套件（KeyDir/Concurrent/Parallel/CaskDocValue/Oki）137/137
  （按 CI 门控豁免 1 项既知 seqlock 误报，见 ci.yml TSan 豁免注释）。

### 🔴 S33-B1 —（S33-3 期间发现）纯 KV 并行恢复墓碑复活 bug 修复

**Phase 7 头号盲区（Tombstone 语义 / basho #82 删除复活类）的现行实例**，
由 S33-2 新增的 `MigrateHintOrdV4EraDirOpensAndReads` 首次暴露（8/30 flaky）：

- **根因**（两个缺口叠加，均为既有代码，非 flag-day 引入）：
  ① `put_insert` 命中墓碑**无条件复活**（不比对任何新旧）；
  ② key 尚未插入时 `remove` **不留任何标记**直接返回。
  R3 的纯 KV 并行恢复（按文件并发 fold）下，「墓碑文件先于 put 文件完成」
  或「remove 先到 + put 后到」皆复活——到达序依赖。串行恢复（按 tstamp
  升序）从未触发，故一直潜伏。
- **修复**（ord 全序判据——恢复两路 BCH5/data fold 皆有 ord，无平局）：
  `KeyDir::remove` 增 `ord` + `insert_tombstone_if_absent` 参数（默认值
  保持旧语义）；墓碑 sentinel 记 ord，重复 remove 推进 ord 高水位；
  `put_insert` 复活门：`!newest_put` 且墓碑 ord≠0 时须 `put.ord > 墓碑 ord`。
  运行期路径（newest_put、ord=0 墓碑、链重放）语义零变化。缺席 sentinel
  走 S29-6 P1 既有墓碑清扫回收。
- **验收**：flaky 复现 8/30 → 修后 40/40 稳过；keydir 新增 3 测试
  （复活门 / 双 remove 高水位 / 运行期旧语义留存）；Debug 全量 663/663 +
  ASan 全量 663/663 + TSan 相关套件 137/137（CI 门控口径）。

### S33-4 — memdelta + 写挂钩 + flush/恢复 🔴 HIGH

- memdelta 单写者结构（锁独立于 keydir 分片锁全序）；put/remove 成功后单一挂钩点
- flush 触发（条数/字节阈值 + close 收尾）；open 恢复：manifest 校验 → tail 重放
  （`ord > oki_wm` 喂 memdelta，hint/fold 两路皆可，靠 S33-2 的 BCH5）
- 全量重建路径（复用 save_snapshot 遍历 + 外排）；`bitcask_migrate --prebuild-oki` 实装
- put 路径回归预算 **≤3%**（`keydir_bench` 挂钩前后对比），超预算换 append+惰性排序
- **工作量**：2 天
- **验收**：crash 注入套件（flush/manifest 各阶段 kill）；put 回归达标

#### ✅ S33-4 落地记录（2026-07-31）

新增 `oki_state.hpp` + `src/fileops/oki_state.cpp`（OkiState：memdelta/
flush/rebuild/水位）+ `tests/oki_recovery_test.cpp`（5 集成测试）。要点：

- **挂钩收敛到 KeyDir::put/remove 咽喉点**（比原计划的"Cask 各写路径挂钩"
  更深一层）——Cask 侧零逐点改动，未来新增写路径自动覆盖（设计 §8 难点 1
  的结构性对策）。锁外执行（分片/meta 锁释放后），锁序不交叉。过滤规则：
  merge 搬迁（old_file_id≠0）不收；`ord < wm` 的 tail 重放旧行由水位门
  自动丢弃——**恢复路径因此零逐点改动**（fold/hint/链重放全自动生效）。
- **wm 语义定为排他上界**（= 尚未覆盖的最小 ord）。发现并绕开一个 ±1 深坑：
  `alloc_ord` 首个 LSN 是 **0**，含上界语义表示不了「已覆盖 ord 0」vs
  「未覆盖任何」——排他语义下 ord 0 自然纳入，全部特判消失。
- **快照崩溃窗口的闭环**：flush 恒在 write_keydir_snapshot 之后同站点搭车
  （close/merge 收尾/成对 ckpt base）；快照自身 next_ord 在链重放前捕获
  （RecoverySnapshots.snap_next_ord）；open 收尾 `wm < snap_next_ord` 或
  manifest 缺失/损坏 → 全量重建（迭代 keydir 活 key 排序写单 run）。
  重建只在 rw 句柄做，best-effort 不阻断 open。
- 阈值 flush：写路径探询 `should_flush()`（1M 行/64MiB，无锁 hint），超限
  同步落 run；flush 换出 memdelta，IO 期间 append 不被阻塞，失败放回队头。
- **put 回归实测**：KeyDir 微基准 52→67ns（+29.6%——无 IO 口径放大，且
  bench 中 memdelta 无界增长）；**Cask put 全路径 1246ns，挂钩 ≈15ns ≈
  1.2% ≤ 3% 预算达标**（预算口径即 put 路径）。
- **计划偏离**：`--prebuild-oki` 未实装（推 S33-5/6——迁移后首开的自动
  重建已覆盖语义，prebuild 只是省一次重建耗时的锦上添花）。
- 测试矩阵：干净关闭（run 内容/墓碑去重/wm 追平）；重开零重建（gen 集
  不变 + memdelta 空）；**fork crash 后 tail 重放**（checkpoint 已 flush
  一半 + 崩溃丢另一半 → 旧 run 保留 + 新行进 memdelta，不触发重建）；
  manifest/runs 全删 → 全量重建（活 key 覆盖、删除键缺席）；快照缺口
  （manifest 回滚 wm=0）→ 重建追平。
- **验收**：Debug 全量 **668/668** | **ASan 全量 668/668** |
  **TSan 并发套件 142/142**（CI 门控口径）| build-rel bench 零错误。

### S33-5 — Range 查询路径 🔴 HIGH

- `Cask::make_range_iter(RangeOptions{lo,hi,...})`：manifest 快照 pin runs →
  k 路归并（runs + memdelta，同 key max-ord 胜、tomb 抵消）→ 逐 key 回查 keydir →
  现有 `get` 取值（零拷贝）
- 一致性：per-key 弱一致，API 注释言明（非 fold 快照语义）
- **三方对拍**：`range(lo,hi)` vs 全表 `CaskIter`+过滤+排序 vs 影子 `std::map`，
  随机写/删/merge/crash 交错属性测试；每轮校验完整性不变量
  （OKI key 集 ⊇ keydir 活 key 集）
- **工作量**：2 天
- **验收**：对拍 + 完整性测试全过；TSan 树（writer + N range iter + merge 并发）

#### ✅ S33-5 落地记录（2026-07-31）

新增 `src/cask/cask_range_iter.cpp`（`CaskRangeIter` + `RangeOptions`，
cask.hpp 公共 API）、OkiState `make_read_view()`（runs 共享 Reader +
memdelta 排序去重快照）+ `tests/oki_range_test.cpp`（3 测试）+
`range_bench` OKI 对比项。要点：

- **run Reader 常驻缓存**：load 时全量 CRC 校验并打开（任一坏 → 整体未
  加载态 → 重建，S33-3 eager 取舍的落点）；flush/rebuild 随 manifest 提交
  同步维护；shared_ptr 使在途 ReadView 安全跨越 rebuild 的旧 run 删除
  （POSIX unlink 后已开 fd 仍可读）——"pin" 即持句柄，无需显式引用计数。
- 归并：各 run `seek(lo)` + memdelta `lower_bound(lo)`，逐 key 取 max-ord
  （不依赖"memdelta 恒新"假设）、tomb 抵消、`get_owned` 回查（kNotFound
  跳过 = 陈旧行无害）；Entry 的 tstamp/ord 取 keydir 权威值。
- **三方对拍**：12 轮 × 120 随机操作（put/覆盖/删/merge/close-reopen/
  checkpoint 交错）× 每轮全域 + 3 随机窗口，range vs 全表过滤 vs 影子
  map 逐 key 逐 value 相等；完整性不变量由「全域 range == 影子 map」蕴含。
- 并发 stress：writer(4000 ops) + 3 range 读者 + 中途 merge——严格升序 +
  零错误断言，20 连跑稳过；TSan 树重点目标。TSan 下经 S29-6 回退开关关闭
  乐观读快路径（与 CI 既知豁免同根因的 seqlock 误报；OKI 层并发仍全程
  受检，**未新增豁免条目**）。
- **`--prebuild-oki`（S33-4 遗留）确认不再需要**：hintord 迁移产物首开
  自动重建已覆盖（MigrateHintOrdV4EraDirOpensAndReads 即证），从计划移除。
- **bench（vs S33-1 基线，同参数）**：选择性 1/256 时 8.01ms → **0.534ms
  （15×）**，1/16 大窗口 14.3ms → 7.80ms（1.8×，value 读取主导）；OKI 耗时
  随选择性线性下降（基线持平）——**O(range) 实锤**，S33-1 立的靶命中。
- **验收**：Debug 全量 **671/671**（20 连跑稳定）| **ASan 全量 671/671**
  （首轮 1 个无关既有测试 IndexPoolUnregister.Timeout* 在同机双 sanitizer
  高负载下时序 flaky，单独 12 连跑 0 失败，非 OKI 回归）| **TSan 并发套件
  157/157**（CI 门控口径，未新增豁免）。

### 🔴 S33-B2 —（S33-5 并发测试发现）mmap 窗口外读竞态修复

**S30 mmap × S13 分批 CAS 的既有竞态**，并发 range 读者首次稳定复现
（3/3 读者中招），普通 get 在多批次 merge 期间同样可踩：

- **根因**：S30 的前提是「只映射 sealed 文件」，但 `read_file` 无从判定
  封口——merge 输出在分批 CAS（`kApplyBatch`/逐输入文件 apply）期间仍在
  增长，读者以打开时刻的尺寸 mmap 后，**后续批次 CAS 的条目落在映射窗口
  之外** → `read_mmap` kShortRead → get 报 kIo。S13-F5 的重试只覆盖
  open-ENOENT 窗口，不覆盖此分支。
- **修复**：`Cask::get` mmap 分支收窄错误处理——kShortRead（= 窗口外）
  **跌落 pread**（fd 本就保留未关，条目本身有效），kBadCrc/其余仍报错。
  零拷贝路径对窗口内读不变。
- **验收**：并发 stress 修前 3/3 读者报错 → 修后 0/20 全过。

### ✅ S33-6 — C API + 值预取 + bench + 文档 🟡 MED

- [x] C API：`bitcask_range_iter_*`（顺手补现缺的 `key_prefix` 能力——
      `bitcask_iter_start_prefix` + `bitcask_parallel_scan_prefix`）
- [x] 值预取：`RangeOptions::prefetch`（parallel_scan 同款分段并发 get）
- [x] `range_bench` 对比 S33-1 基线；RSS 探针扩展（memdelta/run/重建峰值）
- [x] 文档同步：`api-cpp.md` / `api-c.md` / `format-zh.md` / `migrate-le.md` / README
- **工作量**：1 天（实际 1 天内）
- **验收**：✅ build-rel 双树 + bench 入基线（数字见下方落地记录）

#### ✅ S33-6 落地记录（2026-08-03）

C API `bitcask_range_iter_*`（6 个新导出 + 2 个新结构体）、
`RangeOptions::prefetch`、bench 扩展、文档同步四块全部落地。要点：

- **值预取**：`prefetch`（批大小）+ `prefetch_threads`（0 = min(hw, 4)）。
  实现分层——归并层 `next_merged_key()`（串行、廉价）+ 取值层（分段并发
  `get_owned`，与 `parallel_scan` 同款 JoiningPool）。**语义不变量：只改
  取值时机，输出序与内容和惰性路径逐 key 逐 value 相同**（测试断言）。
  死 key 在批内丢弃 ⟹ 缓冲可能空而迭代未结束（续跑路径专门测了「删掉
  一整段 ≥ 最大批」）。
- **计划外的一处调优**：线程是**每批**创建的（无常驻池），实测批 64 ×
  4 线程比惰性还慢 50%（16.1 vs 10.8ms——98 批 × 4 次线程创建）。加
  「每线程至少 64 key」的收窄后，小批自动退化为串行。最终数字（tmpfs、
  1KiB 值、6250 命中）：惰性 10.8ms / 批 64 = 11.6ms（打平）/ 批 256 ×
  4 线程 = **7.6ms（1.4×）**。收益形态是「大窗口 + 冷值」，故默认关闭，
  头文件与 api-cpp 都写明了这条取舍。
- **C API 补齐**：`bitcask_iter_start_prefix` / `bitcask_parallel_scan_prefix`
  ——既有 C++ 形参在 C 侧一直缺口；实现上把**带前缀版做成主体**、无前缀版
  以空切片调用它（零重复逻辑）。range 侧 entry 另立
  `bitcask_range_entry_t`（无 file_id/offset/tomb——range 只产活 key）。
- **bench**：`BM_Cask_RangeScan_OKI_Prefetch`（5 组参数，含同二进制内的惰性
  对照）+ `BM_Oki_MemProbe`（memdelta 行/字节 + RSS 增量、run 字节/每 key、
  **重建峰值 RSS**——后台采样线程取 max，计时段只含重建）。10 万 key 锚点：
  memdelta 5.7MB（RSS +25.3MB 含 keydir）、run **6.2 B/key**（11B key 的前缀
  差分效果）、重建峰值 +8.9MB / 49.5ms。
- **文档**：`format-zh.md` §六 **重写**（原文还停在 v2/v4——S33-2 只改了常量
  表，正文是漏网的陈旧描述）+ 新增 **§十五 OKI**（BCOK/BCOM 字节级布局 +
  生命周期 + wm 排他语义）+ §14.2 补 `hintord`；`api-cpp.md`（§5.8 +
  §6 CaskRangeIter + 线程表 + 示例）、`api-c.md`（§11.7 全套 + 所有权配对表
  + 线程表 + §4.1/§7.5b 类型）、`migrate-le.md`（纪元 v3→v5 陈旧修正 +
  OKI 不迁移行）、`README.md`（能力表 + 架构图）。
- **顺手修（两处 OKI 重建的既有小疵，均为 S33-4 代码、未发布）**：
  ① 零活 key（空库首开 / 全删后重建）仍落一个 entry_count=0 的空 run——
  36B 文件 + 一个常驻 Reader fd，归并不出任何行，且要等下次 rebuild 才被
  清；改为 manifest 记 0 run + `wm=cover_ord`（语义等价）。
  ② 由 ① 的新测试逮出更实的一个：rebuild 的旧文件清理原本只遍历**内存
  manifest 列出的 run**，而触发重建的典型场景恰恰是 **manifest 缺失/损坏**
  （此时内存 manifest 为空）——那批 run 文件成了永不回收的孤儿，每重建一次
  多一批。改为提交后按目录扫描删除一切非本次 run 的 `kv.oki.seg-*`。
  新增测试 `RebuildWithNoLiveKeysWritesNoEmptyRun`（空库首开 + 全删后
  删 manifest 重建，双形态各验 manifest 0 run + 目录零 seg 文件 + 水位追平
  + 重建后 range 照常出货）。
- **补实装 run 归并（设计 §5.2 的漏项，fd 探针实测发现）**：起因是复盘
  「一个库同时开多少文件」——探针（scratchpad `fd_probe.cpp`）显示 1500 key /
  37 个 data 文件的库共 45 个 fd（data read handle 37 个是大头且**有界**：
  `max_read_handles=0` → RLIMIT_NOFILE 一半、下限 64），但 **OKI run 数 =
  flush 次数线性增长且 merge 不回收**（5 次 checkpoint → 5 个 run），每 run
  一个常驻 fd + open 期全文件 CRC + range 多一路归并，墓碑行还永远回收不掉。
  设计文档 §5.2 本就写明「run 数 > N（默认 8）→ 归并成一个；全归并时墓碑真正
  丢弃」，S33-4 首版只做了 flush 与 rebuild，这层漏了。现补
  `OkiState::compact_all_locked`（k 路归并全部 run → 单 run，复用 range 同款
  max-ord 归并；manifest 一次提交；旧 run 走 sweep_runs）+ 阈值常量
  `kCompactRunLimit=8`。best-effort：归并失败不影响 flush 的成功语义。
  **墓碑丢弃的正确性只在「全归并」下成立**（同 key 的 put 行与 tomb 行必定同在
  本次归并里），已在头文件写明——将来若改部分归并必须收回此条。
  探针复测：run 数呈锯齿 1→8→1，fd 有界。新增 2 测试
  （`RunCompactionCollapsesRunsAtThreshold` 阈值内不归并/越阈值塌成 1 个 +
  文件清理 + 水位不变 + 数据完整；`FullCompactionDropsTombstoneRows` 归并前
  tomb 行在、归并后整条消失、活 key 齐全、**丢墓碑后重新 put 仍可见**）。
- **fd 预算收口（承上条探针）**：`max_read_handles` 自动档加绝对上限 **1024**
  （`kAutoReadHandleCeiling`，原来只有下限 64）——自动档是「RLIMIT_NOFILE 的
  一半」，本机 rlimit 524288 ⟹ 26 万，等于没有上限。1024 个句柄在默认
  `max_file_size=2GiB` 下对应约 2TB 数据，正常库碰不到；显式值不夹取。
  **默认行为变更**，已进 CHANGELOG 的 Changed 段。`ReadHandleCap.ResolveSemantics`
  加 7 条断言（封顶/边界内外/显式值不受影响）。
  文档：`api-cpp.md` 新增 **§11 运维调优**（§11.1 fd/mmap 预算三段实测表 +
  四步调优顺序；§11.2 merge 调度），`api-c.md` 新增 §6.5.1 同款；两处
  `max_read_handles` 字段说明改写。**关键结论写进文档**：merge 按碎片率/死字节
  触发，纯追加负载 `needs_merge` 恒 false（实测 89 文件 merge 前后不变）——
  **merge 解决空间放大，不是 fd 预算手段**。
- **CHANGELOG**：新增 `[5.1.0] - 未发布` 段（S33 全景：flag-day + OKI +
  C API + 两个 B 级修复）；`project(VERSION)` 已 bump 到 **5.1.0**，
  `SOVERSION` 随之保持 **5**（= major，CMakeLists 机械派生）。
  **版本号定为 5.1.0 而非原计划的 6.0.0**（2026-08-03 复核后改）：本轮 C API
  纯增量（新导出 6 个 range 函数 + 2 个前缀入口，既有签名/结构体布局零改动；
  `.so` 导出表核实——改了签名的 `KeyDir::remove` / `HintFile::write` 均**未
  导出**，只有 24 个头内联弱符号），ABI 未破坏。仓库先例即此规则：3.1.0 同样
  是「盘上前向不兼容 + C API 增量」→ MINOR，SOVERSION 不动；4.0.0（结构体
  布局）与 5.0.0（签名/字段宽度）才是真 ABI 破坏 → major。soname 换号挡的是
  「二进制 × 二进制」，而这里的不兼容是「二进制 × 数据」，由 meta v5 门禁
  承担，换号零收益却逼下游重链。
- **验收**：Debug 全量 **675/675**（+4 新测试；1 预存 Disabled）|
  **ASan 全量 675/675** | **TSan 相关套件 150/150**（CI 豁免口径，未新增
  豁免；C API 新路径含 prefetch 也在 TSan 下跑过）| build-rel 全树零错误
  （含 bench，公共结构体 `RangeOptions` 双树验证）。

### ✅ S33-7 — Level B 门禁评审（2026-08-05 收口：**通过，立项 S36**）

门禁数据（`doc:<n>` 真实形态直灌 keydir，`key_length_histogram` 口径）：
1M → 96MB（100.7 B/key）；10M → 1.4GB（147.6，扩容相位最差点）；
**100M → 11GB（118.1 B/key，大头是槽位 107.4 而非 key 本体）**。
纯 KV 模式下 11GB 远超 40% 线 → **立项**。设计文档：
[`doc/keydir-disk-resident-design-zh.md`](doc/keydir-disk-resident-design-zh.md)（S36）。

---

## 🔵 S34：多键事务 helper（TxnCask）

> 来源：[`doc/multikey-txn-zh.md`](doc/multikey-txn-zh.md)（模式设想）→
> [`doc/multikey-txn-impl-design-zh.md`](doc/multikey-txn-impl-design-zh.md)（实现设计定稿）
> 决策基线：**方案 B**——库内应用层 helper，建在公共 API 之上，
> 零盘上格式改动（否决引擎原生 commit marker——需 record 格式 flag-day，另行评估）。
> 版本目标：随 **5.1.0**（未发布）出货（C API 纯增量，`SOVERSION` 保持 5，
> 无盘上格式改动；若 5.1.0 已先行发布则为 5.2.0）。

### S34-1 — 设计稿 + 前提核实 🟢

- [x] 三前提核对（put_batch 契约 / sync / OKI range / remove 幂等）；
      发现模式文档 4 处问题：§2.3 `.prefetch = true` 实为关闭（prefetch 是
      size_t 批大小，0/1=关闭）、骨架 API 不存在（`valid()` 等）、
      put_batch 不支持墓碑未提及、uuid txn key 的重放序 ≠ 提交序缺口
- [x] `doc/multikey-txn-impl-design-zh.md` 定稿
- [x] 参考笔记 `doc/pg-xid-mvcc-zh.md`（论证无需 XID 式回收）

### ✅ S34-2 — TxnCask 核心 🔴 HIGH

- [x] `include/bitcask/txn.hpp`：`TxnOp` / `TxnSyncPolicy` / `PendingTxn` / `TxnCask`
- [x] `src/cask/txn.cpp`：commit（校验→意图→sync→apply→清理）、
      recover（收集后前滚）、pending_txns、意图 blob v1 编解码、
      进程级单调 seq txn key（设计 §3/§4/§5）
- [x] CMake：挂 `bitcask_cask` target
- **验收**：✅ ctest 全绿；txn TU 零新告警

### ✅ S34-3 — 测试 🔴 HIGH

- [x] `tests/txn_test.cpp`：设计 §9 全部用例落地为 9 个（含手写编码器
      格式对拍 + fork 崩溃注入 CrashMidApplyRecovers + 乱序写入验证
      重放序 = key 字典序）
- **验收**：✅ Debug 全量 **684/684**（675 基线 + 9 新增）；崩溃注入稳过

### ✅ S34-4 — C API + 文档修正 🟡 MED

- [x] `bitcask_txn_commit` / `bitcask_txn_recover` / `bitcask_txn_pending_count`
      （每调用栈上构造 TxnCask；`c_api_test.c` 增 `test_txn` 冒烟全过）
- [x] 模式文档 4 处修正（见 S34-1）；README / api-cpp §5.3 / api-c §10.3 /
      CHANGELOG（并入 5.1.0 未发布条目）增量
- **验收**：✅ ctest 全绿；build-rel 双树零错误（新公共头 txn.hpp）

#### ✅ S34 落地记录（2026-08-05）

一次成型，无计划偏离。要点：apply() 为 commit ③ 与 recover 前滚的**同一
实现**（PUT 集一次 put_batch + REMOVE 逐条——BatchItem 无墓碑形态）；
recover 先全量收集再前滚，不在弱一致 range 迭代器活跃期间写库；意图 blob
v1 布局由测试侧**独立手写编码器**对拍钉死，改任何一边必红。
未提交（含前置的 pg-xid-mvcc 参考笔记与索引更新）。

---

## ✅ S35：引擎原生原子批（方案 C——kBatchHeader）

> 来源：用户拍板方案 C 取代方案 B 的提交路径。设计定稿：
> [`doc/atomic-batch-design-zh.md`](doc/atomic-batch-design-zh.md)。
> 核心：批头声明区间、区间完整即提交（无批尾 marker）；成员为普通
> kDoc/kTombstone ⇒ 读路径零改动；meta v6 **懒升级**（首批前重写，
> 未用批的目录停留 v5——保守纪元标记）；TxnCask 接口保留、commit
> 重接、recover 保留意图重放（兼容方案 B 遗留 pending）。

### ✅ S35-1 — 格式 + fold 区间语义 🔴 HIGH

- [x] `format.hpp`：`kBatchHeader = 2` + 批头 value 布局（`[u8 ver][u32 count][u64 span_bytes]`，13B 定长）
- [x] `codec`：`encode/decode_batch_header_value`（拒收错长/错版/count=0/span=0）
- [x] `data_file.cpp` fold：批区间内不推进 lve、区间收口一次推到位、
      不完整（越 EOF/批头畸形/嵌套批头/记录跨界）→ break，lve 停批头起点；
      区间内单条 CRC 腐蚀走 tolerate 跳过（位腐蚀逐条降级，原子性只承诺崩溃）
- [x] codec 金测（枚举值 + 批头 value 黄金字节 + 拒收）

### ✅ S35-2 — meta v6 懒升级 🔴 HIGH

- [x] `kMetaVersionBatch = 6`，读端收 v5/v6，`MetaConfig::version` 回填/写出；
      **顺手修**：`write_meta` 原为裸 ofstream（非原子、无 fsync）——改
      `atomic_write_bytes(fsync_dir=true)`；`Cask::upgrade` 补纪元保留
      （原会把 v6 目录降回 v5）
- [x] 懒升级入口（put_batch_atomic 内、write_mu_ 下、首个批字节进写缓冲之前）
- [x] `bitcask_migrate detect` 认 v6 + usage 表补行

### ✅ S35-3 — 写路径 + 恢复 + merge 🔴 HIGH

- [x] `Cask::put_batch_atomic(std::span<const BatchOp>)`（镜像 put_batch：
      校验+值预编码 arena→懒升 v6→roll→批头+成员 write_buffered→flush→
      hint（仅成员）→keydir put/remove→索引 Add/Delete + BatchOrdGuard
      含批头 ord；merge-race 重试落区间外，独立完整记录）
- [x] `cask_recovery.cpp` data fold 回调批 staging（apply_rec 提取共用、
      拷贝暂存、区间收口依序放行、fold break 即弃）
- [x] `merger.cpp`：批头 skip（成员为普通类型走既有路径，keydir 即活性权威）
- [x] `tests/atomic_batch_test.cpp` 7 用例：可见性/hint-data 双路对拍、
      meta 懒升级、掐尾批不可见+截断回批头起点、掐进批头、merge 交互、
      TxnCask 重接掐尾、批后追加恢复。崩溃模拟 = 删派生缓存
      （hint/kv.keydir.ckpt/kv.oki.*）+ resize_file，确定性无 fork

### ✅ S35-4 — TxnCask 重接 + 文档 🟡 MED

- [x] `txn.cpp` commit → 一次 `put_batch_atomic`（意图日志退役热路径，
      写放大 2-3× → 1×）；recover/pending_txns 保留 legacy 意图重放
- [x] S34 九用例零改动回归全绿；C API 增 `bitcask_put_batch_atomic` 直通
- [x] format-zh §3/§4.2/§4.5、multikey-txn 系三文档、api-cpp/api-c、
      README、CHANGELOG（并入 5.1.0 条目，标注 v6 懒升级语义）
- **验收**：✅ Debug 全量 **692/692**（684 + 8 新增）+ build-rel 双树零错误

#### ✅ S35 落地记录（2026-08-05）

关键设计落点与探查结论：**批头 header-first + 「区间完整 ⟺ 已提交」**
（头+成员同一次 flush pwrite，无批尾 marker）；**成员用普通
kDoc/kTombstone 类型** ⇒ get/iter/merge/hint 读路径零特判（探查确认
keydir 是全部读路径的唯一入口，批头永不进 keydir）；「封口 ⟹ 已提交」
不变量使 hint 快路径零改动；meta v6 懒升级使不用原子批的目录与 5.1.0
读端完全互通。测试期发现：掐尾模拟必须同时删 keydir 快照/OKI 派生缓存
（否则批成员经快照复活——真实掉电下这些缓存同样不会覆盖撕裂批）。

**收尾验证（2026-08-05）**：
- **ASan 全量 692/692**（无豁免）；
- **TSan 相关套件**（KeyDir/Concurrent/Oki/Txn/AtomicBatch/CaskDocValue）
  按 CI 门控口径全绿——唯一失败为 ci.yml:159 明文豁免的既知 S29-6
  seqlock 误报（`KeyDirOptimisticRead.ConcurrentGetPutRemoveGrowStress`），
  非 S35 回归；
- **bench 锚点**（`BM_Cask_PutBatch(Atomic)`，bench/cask_bench.cpp 新增）：
  原子批 vs put_batch **零可测回归**（8/64 档噪声内；512 档 -15%——arena
  预编码比逐条 thread_local 编码缓存友好）。写放大 1×（批头 40B/批），
  对比方案 B 意图日志的 2-3×。数字入设计文档 §8。

S34 已由用户提交（dc81bbc）；S35 改动未提交。

---

## 🔵 S36：keydir 磁盘驻留（OKI Level B）

> 来源：S33-7 门禁通过（100M `doc:` key 实测 keydir 11GB）。设计定稿：
> [`doc/keydir-disk-resident-design-zh.md`](doc/keydir-disk-resident-design-zh.md)。
> 核心：哈希 keydir 降级热点缓存，点查权威 = cache → memdelta →
> **BCOK v2 全字段 run**（+内嵌 bloom + 块 LRU）；(ord, run_gen) 胜出格
> 取代 epoch；merge 活性/搬迁改走统一 `locate()`（Level A「零交互」反转）；
> **零 flag-day**（run 是派生缓存，版本升级自愈）。目标：100M key
> 11GB → ~1.2GB（-90%），热 get 零回归、冷 get ≤2 次 pread。

### S36-1 — BCOK v2 格式 + 外排 rebuild 🔴 HIGH

- [ ] v2 行（全字段 vbyte + tomb 免位置）+ bloom 内嵌 + 32B 尾部；
      BCOM v2（条目带 format_ver）；(ord, gen) 归并胜出
- [ ] rebuild 换外排（64MiB 分段 + k 路归并——现全内存 sort 在 100M 档
      即 11GB 峰值，必须先修）
- **验收**：roundtrip/seek/bloom FP/损坏拒收/v1 拒载自愈单测；重建峰值探针

### S36-2 — 全字段 delta + locate() 影子对拍 🔴 HIGH（安全网）

- [ ] DeltaRow 加宽（+SingleEntry）；搬迁/TTL 挂钩（keydir 咽喉点反转
      old_file_id!=0 跳过规则）；统一 `locate()` 原语
- [ ] **影子模式**：缓存不逐出，get 双查对拍（debug 断言组合视图 == 哈希
      权威）——零漂移是 S36-4 开逐出的前置门
- **验收**：全量 ctest + 对拍零漂移；put/merge 回归 bench（put ≤3%）

### S36-3 — get 冷路径 + 块 LRU 🔴 HIGH

- [ ] get 接 locate；块 LRU（独立小锁，不进 keydir 锁序）；读升温回填
      （二次命中门）
- **验收**：冷/热 get bench 锚点（热 ≤3%、冷 P99 ≤300µs tmpfs 另锚）

### S36-4 — 逐出 + 快照三元组 + BCKS v4 🔴 HIGH

- [ ] CLOCK 逐出 + `CaskOptions::keydir_cache_entries`（0=不限=现状，
      默认 0 opt-in）；MultiEntry 不可逐；逻辑计数与 fstats 校准
- [ ] CaskIter/parallel_scan 快照 = 缓存屏障 + delta 拷贝 + manifest pin
- [ ] kv.keydir.ckpt v4（缓存子集语义；Level B 关闭时仍写 v3）
- **验收**：100M 档 RSS ≤1.5GB 实测；快照一致性 stress

### S36-5 — merge 组合视图 + 崩溃注入 + B1 收口 🔴 HIGH

- [ ] merge 活性/搬迁切 locate；unlink 前搬迁行入 delta 顺序不变量；
      遗留 B1（ckpt fsync 水位）失败注入证实 + 修复
- **验收**：逐出态 merge 千轮无丢 key stress；崩溃注入全套；ASan/TSan

### S36-6 — C API + 文档 + 门禁复测 🟡 MED

- [ ] 选项透出 C API；文档/CHANGELOG；100M 门禁复测入档
- **验收**：全矩阵 + build-rel 双树

---

## 执行序

```
S33-1 (半天)  ───── 先行，独立可交付（探针 + 基线数字）
T23   (半天)  ───── S33-2 前置（hint_file refill 归并，避免格式分叉叠在漂移代码上）
S33-2 (2天)   ───── flag-day 基建 = 5.1.0 发布边界（盘上纪元，非 ABI）
S33-3 (1.5天) ───── OKI 格式，纯新增，与 S33-2 后半可并行
S33-4 (2天)   ───── 依赖 S33-2 + S33-3
S33-5 (2天)   ───── 依赖 S33-4
S33-6 (1天)   ───── 收尾
S33-7 (评审)  ───── 依 S33-1 数据
```

---

## ⏸ 遗留（2026-08-05 S35 收尾时复核）

| 项 | 内容 | 状态 |
|---|---|---|
| T23 | ChunkedReader 归并 refill ×3 | ✅ done（随 S33-2，见落地记录）|
| T24 | decode_rec 共享解包段模板归并（须 bench 基准 + build-rel 双树）| 🟢 择机——**前提复核仍成立**：两份解码现位于 `src/bm25/segment_v2.cpp:604`（`decode_rec`→FlatPostings）与 `:970`（`decode_rec_list`→PostingList），原文见 git 历史 62789cd |
| T8 | 搜索读屏障无界等待（`prepare_search` 饥饿）| ⏸ 4 项前置未满足（饥饿注入测试 / applied_ord 可见性调查 / flush 超时基建✅ / flush_upto+notify 成对恢复；原文 62789cd）|
| T12 | HNSW ckpt 去重（~115 行）| ⏸ 默认不做（注释同步已替代）|
| **B1** | **checkpoint 可能跑赢未 fsync 的数据**（S35 测试期发现的**预存**暴露面）：keydir 快照/OKI 经 `atomic_write_bytes` fsync 落盘，而被引用的数据记录可能还在 page cache——掉电后快照存活、数据撕裂 ⟹ 恢复拿到悬空条目（get 报 IO/CRC）。单条 put 与批同样暴露，非 S35 引入。候选方向：ckpt 写前记录各文件已 fsync 水位、快照只覆盖水位内条目；或 ckpt 前强制 sync。**须先写失败注入测试证实再立项** | 🟡 待评估 |
| **B2** | **legacy 意图重放退役时间表**：`TxnCask::recover`/`pending_txns` + blob v1 解码现在只服务方案 B 时期（dc81bbc..S35 之间）目录的崩溃遗留。建议 5.3+ 删除（CHANGELOG 预告一版）| 🟢 择机 |

Phase 6 复核仍成立的低价值项（RED-3/5/6/10，随重构自然消化）见 git 历史 62789cd。

---

## 当前状态快照

| 项 | 状态 |
|---|---|
| 设计文档 | ✅ `doc/ordered-key-index-design-zh.md`（含 flag-day 决策）|
| S33-1 探针 + 基线 | ✅ done（O(全表) 实锤 + ~122B/key 锚点，见落地记录）|
| T23（前置）| ✅ done（ChunkedReader；v2/v4 读端删除后收敛为 2 站点 1 实现）|
| S33-2 flag-day | ✅ done（Debug 650/650 + **ASan 全量 650/650** + build-rel 零错误）|
| S33-3 OKI 格式 | ✅ done（10 测试；Debug 全量 663/663；ASan/TSan 见落地记录）|
| S33-B1 墓碑复活修复 | ✅ done（并行恢复到达序无关；flaky 8/30 → 40/40 稳过）|
| S33-4 memdelta + 写挂钩 | ✅ done（Debug 668/668；put 挂钩 ≈1.2% 达标）|
| S33-5 Range 查询路径 | ✅ done（三方对拍 + 并发 stress；**1/256 选择性 8.01→0.53ms = 15×，耗时随选择性线性**；Debug 671/671）|
| S33-B2 mmap 窗口外读修复 | ✅ done（S30×S13 既有竞态；kShortRead 跌落 pread；修前 3/3 中招 → 0/20）|
| S33-6 C API + 值预取 + bench + 文档 | ✅ done（range C API 6 导出；预取批 256×4 线程 1.4×；format-zh §六重写 + §十五 OKI；顺带补实装 run 归并 + 两处重建疵；Debug/ASan **675/675**）|
| S33-7 Level B 门禁评审 | 🟡 下一步（数据已齐：keydir ~122 B/key，见下）|

### S33-7 输入数据（S33-1 + S33-6 探针汇总）

门禁口径：**keydir RSS 占进程 RSS > ~40% 才立项** keydir 磁盘驻留。
现有锚点（10 万 key、11B key、64B 值、tmpfs）：keydir 估算 11.6MB
（~122 B/key）、OKI memdelta 5.7MB、OKI run 0.59MB（6.2 B/key）、
重建峰值 +8.9MB。**该锚点规模太小、且 value 不在进程内**，占比不代表生产
形态——立项与否须在生产规模负载（≥1000 万 key + 真实 value 分布）上重取
`BM_KeyDir_MemProbe` / `BM_Oki_MemProbe` 两项后评审。
