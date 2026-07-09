# S27-3 B2b/D/E 设计：段集融入 Cask checkpoint 协议 + recovery 重写

> 状态：**设计草案（待实现）**。S27-3 Slice A/B1/B2a/C 已落地（554/554 全绿），
> 本文档覆盖剩余 B2b/D/E（删 fields_ + flush 走段集 + recovery 重写 + legacy 迁移 + 段级 merge）。
>
> 前置阅读：
> - `doc/segment-index-design-zh.md`（段模型总体设计）
> - `doc/recovery-unified-checkpoint-design-zh.md`（当前 base+delta 链持久化）
> - `doc/plugin-arch-split-design-zh.md`（插件化架构 §5 checkpoint 拆分）

## 1. 问题分析：为何 B2b 不能增量推进

### 1.1 双 manifest 冲突（B2b 步骤 1 实测失败根因）

当前架构有两个独立的原子提交点：

| manifest | 路径 | 提交者 | 内容 |
|---|---|---|---|
| `index.manifest` | `dirname_/index.manifest` | `Cask::save_checkpoint_paired` | 3 组件 entry（docmap/bm25/vec），per-component `{base_wm, chain_seq, chain_wm}` |
| `segments.manifest` | `dirname_/bm25_segments/segments.manifest` | `SegmentSet::add` / `SegmentSet::drop` | 活跃段清单 `{seg_id, filename, hi_lsn, doc_count}[]` |

**冲突场景**：TextPlugin::flush 调 `flush_building()` → `SegmentSet::add` 写段文件 + 提交 `segments.manifest`。随后 Cask::save_checkpoint_paired 提交 `index.manifest`。两个 commit point 之间崩溃 → `segments.manifest` 已提交但 `index.manifest` 未提交 → recovery 时 bm25 组件的 chain_wm 与 segments.manifest 的 hi_lsn 不一致 → 查询退化逻辑误判（fields_.live vs seg_total）。

**实测**：`SearchSnapshotCorruptSidecarFallsBack` + `S17ManifestCorruptionFallsBackToFullFold` 在 flush_building 注入后失败。

### 1.2 SegmentSet 独立持久化与 Cask 协议不兼容

SegmentSet::add 是**自治持久化**（自己 tmp+rename+dirfsync），不经 Cask 的 save_checkpoint_paired 编排。这违反 S17 manifest commit 协议的「单一 commit point」原则。

## 2. 设计方案：SegmentSet 持久化经 Cask 统一编排

### 2.1 核心思路：segments 清单合并进 index.manifest

**方案 A（推荐）**：把 bm25 组件的 manifest entry 从 `{base_wm, chain_seq, chain_wm}` 改为 `{base_wm, seg_count, max_seg_hi_lsn}`。段清单（filename + hi_lsn + doc_count）存入 `bm25.ckpt` base 段（作为 kSegManifest section，S27-2 Slice 4 已定义）。

- **Commit 协议**：
  1. SegmentSet::add 改为**只写段文件**（tmp+rename），不提交 segments.manifest
  2. TextPlugin::flush 把段清单写入 bm25.ckpt base（新 section kSegManifest）
  3. Cask::save_checkpoint_paired 提交 index.manifest（唯一 commit point）
  4. 段清单随之原子提交（它在 bm25.ckpt base 内，base 先于 manifest rename）

- **Recovery**：
  1. 读 index.manifest → bm25 entry 的 base_wm
  2. 读 bm25.ckpt base → kSegManifest section → 活跃段清单
  3. 按清单 load 各段文件
  4. fold 重放补齐窗口

- **优点**：单一 commit point（index.manifest），无双 manifest 冲突
- **代价**：bm25.ckpt base 含段清单（~100B/段），每次 flush 写全量

### 2.2 备选方案 B：segments.manifest 保留但纳入 Cask 编排

SegmentSet 不自治提交，而是返回「待提交的段清单」。Cask::save_checkpoint_paired 在 manifest 提交后调 SegmentSet::commit_pending()。

- 优点：SegmentSet 保留独立格式
- 缺点：Cask 需要知道 SegmentSet 的 commit 语义（跨层耦合）

**选方案 A**（更简、单一 commit point）。

## 3. 实现计划（B2b/D/E 拆为 5 步）

### 步骤 1：SegmentSet 持久化解耦（零行为变化）

**改动**：
- `SegmentSet::add` 拆为 `add_pending`（只写段文件，不提交 manifest）+ `commit`（写 segments.manifest）
- `SegmentSet::drop` 同理拆为 `drop_pending` + `commit`
- TextPlugin::flush_building 调 `add_pending`（不自治提交）
- TextPlugin::flush 在 save_component_base 时把段清单写入 bm25.ckpt（kSegManifest section）
- Cask::save_checkpoint_paired 不变（index.manifest 仍是唯一 commit point）

**验证**：554/554 全绿（SegmentSet::open 仍读 segments.manifest 作过渡兼容）。

### 步骤 2：explain 多段化已完成（B2a 9/9）。跳过。

### 步骤 3：删 fields_ map + apply/on_delete 改造

**改动**：
- TextPlugin 删 `fields_` / `field_index` / `ord_field_lens_` / `dirty_default_` / `dirty_fields_`
- `apply_text` / `apply_job_impl` 只写 building_（删 fields_ 写入）
- `on_delete` 只走段级 mark_dead（删 fields_ remove_doc + ord_field_lens_ 扣减）
- `compact` 退役（段级 merge 替代，步骤 5）
- `materialize_hits` 的退化逻辑（fields_ 虚拟段）删除（段集已是唯一源）
- `collect_default_segment_views` 简化（不再有退化路径）

**风险**：recovery 后段集只有 fold 数据（ckpt 数据在段清单里但段文件可能未封口）。需要步骤 4 先就位。

### 步骤 4：recovery 重写

**改动**：
- `TextPlugin::open` / `load_component` 重写：
  - 读 bm25.ckpt base → kSegManifest section → 活跃段清单
  - 按清单 load 各段文件到 SegmentSet
  - 不再反序列化 kBm25Default/kBm25Fields 到 fields_（fields_ 已删）
- `Cask::load_recovery_snapshots` 的 bm25 组件恢复路径调整：
  - open_plugins 时 OpenContext 注入段清单提示
  - watermark = max(段 hi_lsn)
- `Cask::migrate_legacy_search_ckpt`（legacy 迁移）：
  - 读旧 bm25.ckpt（kBm25Default/kBm25Fields）→ 重建为一个初始 SealedSegment → 写 bm25.ckpt base（kSegManifest + kBm25Default + kSegDocStore）

### 步骤 5：段级 merge（Slice D）

**改动**：
- `TextPlugin::on_merge_commit` 从 `compact(0.2)` 改为段级 merge
- 段级 merge：遍历活跃段，drop 死 docid 占比高的段，合并存活文档到新段
- 新段 add_pending + 旧段 drop_pending + TextPlugin::flush 时 commit

## 4. 不变量与验证策略

### 4.1 不变量

1. **单一 commit point**：index.manifest 是唯一原子提交点；段清单在 bm25.ckpt base 内（先于 manifest rename 写入）
2. **段文件先于清单**：SegmentSet::add_pending 写段文件 → TextPlugin::flush 写 bm25.ckpt base（含清单）→ Cask 提交 index.manifest
3. **Recovery 幂等**：段文件 load 失败 → 退全量 fold（安全慢）
4. **覆盖写 mark_dead**：apply_* 写入新 docid 前 mark_dead 旧（B1 已实现）

### 4.2 验证

- 每步全量 ctest + TSan 子集
- 关键测试：crash_recovery 套件（7 例）+ checkpoint_recovery 套件（5 例）+ merge_concurrent（3 例）
- 新增测试：段集 round-trip recovery（crash 镜像重开后段数据完整）

## 5. 风险与缓解

| 风险 | 缓解 |
|---|---|
| Recovery 重写引入数据丢失 | 分步验证：步骤 1（解耦）零行为变化 → 步骤 4（recovery）双路径并行（读 fields_ + 读段集对比）→ 步骤 3（删 fields_） |
| 段级 merge 回归（召回/排序变化） | merge 前后查询结果逐位对比测试 |
| Legacy 迁移失败 | 失败退全量 fold（安全慢），不破坏老库 |
| flush 性能下降（全量 base 替代 delta） | 段集 base 不含 InvertedIndex 序列化（段文件自含）；bm25.ckpt base 只含段清单（~100B/段），写入极小 |

## 6. 工程量估计

| 步骤 | 改动量 | 风险 |
|---|---|---|
| 1 SegmentSet 解耦 | ~200 行 | 低（零行为变化） |
| 3 删 fields_ | ~300 行删除 + ~100 行改 | 中-高（核心数据结构） |
| 4 Recovery 重写 | ~400 行 | 高（recovery 路径） |
| 5 段级 merge | ~300 行 | 中（merge 路径） |
| legacy 迁移 | ~150 行 | 中（格式转换） |
| 测试 | ~300 行 | — |
| **总计** | **~1750 行** | — |

## 7. 建议执行顺序

1. **步骤 1**（SegmentSet 解耦）→ 验证 554/554
2. **步骤 4**（Recovery 重写，双路径并行）→ 验证 crash/checkpoint 套件
3. **步骤 3**（删 fields_）→ 验证全量 + TSan
4. **步骤 5**（段级 merge）→ 验证 merge 套件
5. **Legacy 迁移**→ 验证老库升级

每步独立可交付、可回退。步骤 1 是零风险前置（解锁后续所有步骤）。
