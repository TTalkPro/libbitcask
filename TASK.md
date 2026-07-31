# S33：有序 Key 索引（OKI）开发任务清单

> 来源：[`doc/ordered-key-index-design-zh.md`](doc/ordered-key-index-design-zh.md)（设计草案已定稿）
> 决策基线：否决整体换 LevelDB；WiscKey 式旁挂有序 key 索引；**flag-day 停机迁移**
> （hint BCH4→BCH5 加 ord、meta v4→v5、`bitcask_migrate hintord`，与 5.0.0 tstamp64 同模式）
> 版本目标：**6.0.0**（盘上 `bitcask.meta` = v5）
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

### S33-4 — memdelta + 写挂钩 + flush/恢复 🔴 HIGH

- memdelta 单写者结构（锁独立于 keydir 分片锁全序）；put/remove 成功后单一挂钩点
- flush 触发（条数/字节阈值 + close 收尾）；open 恢复：manifest 校验 → tail 重放
  （`ord > oki_wm` 喂 memdelta，hint/fold 两路皆可，靠 S33-2 的 BCH5）
- 全量重建路径（复用 save_snapshot 遍历 + 外排）；`bitcask_migrate --prebuild-oki` 实装
- put 路径回归预算 **≤3%**（`keydir_bench` 挂钩前后对比），超预算换 append+惰性排序
- **工作量**：2 天
- **验收**：crash 注入套件（flush/manifest 各阶段 kill）；put 回归达标

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

### S33-6 — C API + 值预取 + bench + 文档 🟡 MED

- C API：`bitcask_range_iter_*`（顺手补现缺的 `key_prefix` 能力）
- 值预取：`RangeOptions::prefetch`（parallel_scan 线程池基建复用）
- `range_bench` 对比 S33-1 基线；RSS 探针启用扩展（memdelta/重建峰值）
- 文档同步：`api-cpp.md` / `api-c.md` / `format-zh.md` / `migrate-le.md` / README
- **工作量**：1 天
- **验收**：build-rel 双树 + bench 入基线

### S33-7 — Level B 门禁评审 🟢 LOW（依 S33-1 数据）

keydir RSS 占进程 RSS > ~40% 才立项 keydir 磁盘驻留；立项则另立设计文档。

---

## 执行序

```
S33-1 (半天)  ───── 先行，独立可交付（探针 + 基线数字）
T23   (半天)  ───── S33-2 前置（hint_file refill 归并，避免格式分叉叠在漂移代码上）
S33-2 (2天)   ───── flag-day 基建 = 6.0.0 发布边界
S33-3 (1.5天) ───── OKI 格式，纯新增，与 S33-2 后半可并行
S33-4 (2天)   ───── 依赖 S33-2 + S33-3
S33-5 (2天)   ───── 依赖 S33-4
S33-6 (1天)   ───── 收尾
S33-7 (评审)  ───── 依 S33-1 数据
```

---

## ⏸ 遗留（自 Phase 6 带入，原文见 git 历史）

| 项 | 内容 | 状态 |
|---|---|---|
| T23 | ChunkedReader 归并 refill ×3（`hint_file.cpp` ×2 + `data_file.cpp`）| 🟡 **升级为 S33-2 前置** |
| T24 | decode_rec 共享解包段模板归并（须 bench 基准 + build-rel 双树）| 🟢 择机 |
| T8 | 搜索读屏障无界等待 | ⏸ 4 项前置未满足（见 git 历史 Phase 6 版）|
| T12 | HNSW ckpt 去重 | ⏸ 默认不做（注释同步已替代）|

---

## 当前状态快照

| 项 | 状态 |
|---|---|
| 设计文档 | ✅ `doc/ordered-key-index-design-zh.md`（含 flag-day 决策）|
| S33-1 探针 + 基线 | ✅ done（O(全表) 实锤 + ~122B/key 锚点，见落地记录）|
| T23（前置）| ✅ done（ChunkedReader；v2/v4 读端删除后收敛为 2 站点 1 实现）|
| S33-2 flag-day | ✅ done（Debug 650/650 + **ASan 全量 650/650** + build-rel 零错误）|
| S33-3 OKI 格式 | 🔴 下一步 |
| S33-4..7 | ⬜ 未开始 |
