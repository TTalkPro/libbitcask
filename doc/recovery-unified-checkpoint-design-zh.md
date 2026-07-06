# 恢复持久化：搜索索引分段 checkpoint + 统一 manifest commit

> 前置阅读：[`format-zh.md`](format-zh.md)（ckpt 容器与段类型字节级格式）、
> [`concurrency-zh.md`](concurrency-zh.md)（合并 / 写者并发契约）、
> [`merge-policy-zh.md`](merge-policy-zh.md)（merge 触发与执行）
> 状态：已实现（S17-S22）

## 1. 设计目标

open 时重建 keydir 与搜索索引（docmap / bm25 / hnsw）需要 fold data 文件
到尾部，纯 fold 路径 O(全库)——大库崩溃后启动慢。Checkpoint 是**纯优化**：
落盘一个能 fold 尾巴就追平的快照，使 open 跳过大段历史字节。

设计要点：

- **单 watermark 自门模型**：仅以各组件链覆盖水位的最小值定 fold 起点；
  缺文件/CRC 坏 → 该组件退水位 0 自门、其余组件不受影响
- **损坏隔离到段**：`SearchCheckpoint` 容器每段独立 CRC，坏段单独重建
- **统一 commit point**：`index.manifest` 是三组件 ckpt 的**唯一**提交点
  （tmp+rename 原子写），crash 在「先写组件后写 manifest」窗口则整组件
  退全量 fold——不出现「manifest 已提交但组件页丢失」的脏状态
- **paired save 语义**：keydir 快照 + 三组件 manifest + 各组件 base/delta
  在同一临界区协调，保证 `keydir_covered ≤ search_covered`
- **legacy fallback**：pre-S17 单文件 `search.ckpt` 一次性迁移到 per-component
  协议，迁移失败 → 全量 fold（fallback 安全网）

## 2. 文件总览

数据目录在索引模式下包含：

| 文件 | 用途 |
|------|------|
| `<tstamp>.bitcask.data` / `.hint` | 唯一 WAL（KV + 索引 payload） |
| `bitcask.meta` | 目录配置 v3（magic + version + mode + 向量配置 + CRC） |
| `field.schema` | 字段名注册表 |
| `kv.keydir.ckpt` | keydir 快照（`SearchCheckpoint` 容器，单段） |
| `docmap.ckpt` / `.prev` / `.d<seq>` | docmap 组件 base 与 delta 链 |
| `bm25.ckpt` / `.prev` / `.d<seq>` | bm25 组件 base 与 delta 链 |
| `vec.ckpt` / `.prev` / `.d<seq>` | hnsw 组件 base 与 delta 链 |
| `index.manifest` | 三组件 ckpt 的统一 commit point（80 字节） |
| `search.ckpt` / `.prev` | legacy 单文件 ckpt（S17-5 后仅作迁移源） |
| `search.vec` / `search.qc8` | hnsw 外部 payload（f32 / int8 量化码字） |

后缀契约：`.ckpt` = 可重建的检查点；`.prev` = 上一代回退目标；`.d<seq>` =
delta 链文件；`search.ckpt` = 旧单文件格式（迁移一次性）。所有派生文件
可删——删后 open 走全量 fold 重建。

## 3. checkpoint 容器与段类型

`SearchCheckpoint`（`include/bitcask/search_checkpoint.hpp`）是所有
`*.ckpt` / `*.d<seq>` 文件的统一容器，自描述、分段、每段独立 CRC，
页脚目录定位：

```
== 头部 (16 B) ==
  [0..3]   magic    "BCSC"  (kCkptMagic)
  [4..7]   version  u32     (kCkptVersion=1 / kCkptVersion2=2)
  [8..15]  watermark u64    本快照覆盖到的 next_ord 上界

== 段载荷区 ==
  各段 payload 顺序拼接(无内联段头;位置/校验由页脚目录给出)

== 页脚 ==
  directory(dirLen 字节):
    sectionCount u32
    每段: type u16 | flags u16 | offset u64 | len u64 | crc32 u32
        (crc 覆盖该段 payload)
  footerCrc u32   CRC 覆盖 directory 字节
  dirLen    u32   directory 字节长度
  trailer   "BCSC" (4 ASCII)
```

`SearchCheckpoint::write` 走 `tmp + fdatasync + rename` 原子写
（`include/bitcask/search_checkpoint.hpp`）；`SearchCheckpoint::read` 整文件
读入后从尾倒走 footer 校验，逐段 CRC 写入 `LoadedSection::crc_ok`。
read_selected（`include/bitcask/search_checkpoint.hpp`）支持按段类型过滤
读取——脏段重序列化时不重读、干净段零拷贝搬入新 ckpt。

### 3.1 段类型

| `CkptSectionType` | 数值 | 用途 |
|-------------------|------|------|
| `kDocmap` | 1 | docmap base 段（v2 gap+vbyte 行编码） |
| `kBm25Default` | 2 | bm25 默认域 `InvertedIndex` |
| `kBm25Fields` | 3 | bm25 多字段 |
| `kHnsw` | 4 | hnsw 段（V7 header，f32 payload 外置） |
| `kMeta` | 5 | 可选加速缓存 |
| `kTerms` | 6 | 可选加速缓存（terms-cache，替代旧 bm25 WAL） |
| `kBm25DefaultDelta` | 7 | bm25 默认域 delta |
| `kBm25FieldsDelta` | 8 | bm25 多字段 delta |
| `kDeltaInfo` | 9 | 链校验三元组（`base_gen u64` / `prev_wm u64` / `seq u32`） |
| `kDocmapDelta` | 10 | docmap delta（v1 定宽；保留兼容读） |
| `kHnswDelta` | 11 | hnsw 插入日志 |
| `kKeydirDelta` | 12 | keydir 元数据（`"BKMD"`：标量/fstats/字节水位） |
| `kDocmapDeltaV2` | 13 | docmap delta v2（gap+vbyte 行编码） |

段类型定义于 `include/bitcask/search_checkpoint.hpp` 的
`CkptSectionType` 枚举。

### 3.2 checkpoint v2 文件版本

`kCkptVersion2 = 2` 仅用于**含 `kDocmapDeltaV2` 段**的文件：

- 写端：含 v2 段型则用 `kCkptVersion2` 写出（`SearchCheckpoint::write`
  的 `version` 入参默认 `kCkptVersion`，caller 显式传 `kCkptVersion2`）
- 读端：`read` / `read_selected` 双收 `kCkptVersion` 与 `kCkptVersion2`
- 旧读端（只认 `kCkptVersion`）整文件拒收 → 链断 → 退 fold——这是
  「降级安全」设计：含 v2 段的文件不会被旧读端静默忽略丢行推进水位

## 4. 组件 ckpt 数据结构

`include/bitcask/component_ckpt.hpp` 收敛三组件（docmap / bm25 / vec）
共同的链状态与载入结果类型。各插件类以 `using` 别名暴露
（`TextPlugin::ChainState` 等既有名字不变），差异化的 setter 语义
（如 `VectorPlugin::set_chain_state` 联动 `delta_window_wm_`）仍留在各类。

```cpp
// 组件链状态：base 世代 | 链覆盖水位 | 下一 delta 序号
struct ChainState {
    std::uint64_t base_gen = 0;
    std::uint64_t chain_wm = 0;
    std::uint32_t next_seq = 1;
};

// delta 写结果
struct DeltaSaveResult {
    bool          wrote = false;
    std::uint32_t new_seq = 0;
};

// 组件载入结果
struct LoadResult {
    bool          loaded = false;
    std::uint64_t watermark = 0;
    bool          from_prev = false;
    bool          all_segments_ok = false;
};
```

链走读由 `bitcask::search::walk_chain`（`include/bitcask/ckpt_chain.hpp`）
统一管理：

```cpp
template <typename Apply>
ChainWalk walk_chain(const std::string& base_path, std::uint64_t base_gen,
                     std::uint64_t base_coverage, std::uint32_t chain_seq,
                     bool unbounded, Apply&& apply);
```

行为：

- 从 `<base>.d1` 逐个递增 `seq`，到 `chain_seq`（有界）或首缺文件（无界）
- 每个 `.d<seq>` 调 `SearchCheckpoint::read`；校验 `kDeltaInfo` 段三元组
  `base_gen / prev_wm / seq` 必须与 `base_gen` / 当前 coverage / 当前 seq
  一致
- 通过则调 caller 的 `apply(LoadedCheckpoint)`；失败（read 坏 / 三元组错 /
  apply 假）→ 断链返回 `ok=false`

`unbounded == true` 时缺文件 = 正常链尾（legacy / shim 无 manifest 链长
提示时用）；`false` 时缺文件 = 链断（manifest 提示链长可信时用）。

`remove_chain_files`（同头）实现 base 落成后的链坍缩：连续 8 个 miss
序号即停（链恒连续 `1..N`，8 空洞 orphan 扫尾足够）。

## 5. manifest 协议

`include/bitcask/index_manifest.hpp` 定义 per-component 协议的**唯一
commit point**：

- 文件大小：80 字节（`kManifestSize = 12 + kComponentCount * 20 + 8`）
- magic：`"BCMF"`（`kManifestMagic`，文件头与文件尾双重）
- version：1（`kManifestVersion`）
- 组件 ID：`kDocmap=0` / `kBm25=1` / `kVec=2`（`ComponentId` 枚举，
  `kComponentCount = 3`）
- 每组件 entry：`(base_watermark u64, chain_seq u32, chain_watermark u64)`
- 尾部：footer CRC32 覆盖 header + body + trailer magic

```
[magic "BCMF"(4)][version u32=1][component_count u32=3]
per component [0=docmap, 1=bm25, 2=vec]:
  base_watermark u64 | chain_seq u32 | chain_watermark u64
[footer_crc32 u32][trailer "BCMF"(4)]
```

`write_manifest`（同头）走 `tmp + fdatasync + rename + 目录 fsync`；损坏
（含 CRC 失败）退全量 fold——80 字节 + CRC + 原子 rename 足够可靠，无
`*.manifest.prev`。

`Manifest::min_chain_watermark()` 返回所有组件 `chain_watermark` 的最小值
（任意一条 0 则返 0）——fold 起点候选。

## 6. paired save 语义

`Cask::save_checkpoint_paired`（`src/cask/cask.cpp`）是所有搜索模式 ckpt
保存的**统一入口**（手动 / 自动 / 收尾 / merge / ①均经此）。`dir` 固定
为 `dirname_`，写序不变量 `keydir_covered ≤ search_covered` 集中在一处
维护。

### 6.1 决策路径

1. 脏掩码组装：`docmap_->dirty()` / `text_->dirty()` / `vec_plugin_->dirty()`
2. 全局判据 `global_base = !any_dirty || ckpt_rebase_needed_`（close /
   compact / rebuild / legacy / merge 走 base；脏但无 rebase 走 delta）
3. 链上限：`docmap_cap = (max_delta_chain > 0 && docmap_chain_.chain_seq
   >= max_delta_chain)`——docmap 走 base，插件自查各自上限
4. 走 base 还是 delta 由每组件自决（S18-6）：docmap 走 `save_docmap_base` /
   `save_docmap_delta`；bm25 / vec 经 `plugin::flush` 自决（rebase 标志 +
   链长上限）

### 6.2 docmap 侧

- `index::save_docmap_base`（`include/bitcask/docmap_ckpt.hpp` /
  `src/keydir/docmap_ckpt.cpp`）：`rename(base, base.prev)` + 写新 base +
  `remove_chain_files` 清链文件 + `Index::begin_delta_window(watermark)` +
  `clear_removals` + `clear_dirty` 收尾
- `index::save_docmap_delta`（同上）：写 `<base>.d<seq>`，含 `kDeltaInfo` +
  `kDocmapDeltaV2` 段（窗口 live 行 + 删除日志按 ord 升序交错）+ 可选
  `kKeydirDelta` 段（keydir 半边元数据内联进 delta 文件，delta 路径不
  写独立 keydir 快照）

### 6.3 插件侧

`plugin::FlushRequest` 经各插件 `flush()`：返回 `FlushResult{ status,
covered_ord, generation, chain_seq, chain_wm }`。宿主把 `chain_seq` /
`chain_wm` / `generation` 写入 `new_manifest.entries[comp]`。

### 6.4 keydir 快照成对

`global_base` 走时（docmap base 已落成）→ 调 `write_keydir_snapshot(*wms)`
写 `kv.keydir.ckpt`。Delta 路径不写 keydir 快照——keydir 元数据已内联
进 docmap delta 的 `kKeydirDelta` 段。

### 6.5 写序不变量

`write_manifest` 是**唯一 commit point**，且走 `fdatasync + 目录 fsync`——
保证组件数据先于 manifest 落盘（manifest 在 `SearchCheckpoint::write`
的同名 fdatasync 屏障后写入）。崩溃窗口处理：

- 已写组件 / 未写 manifest → manifest 仍指旧代，链走读退回 `chain_seq=0`
  起点、缺文件即停
- 已写 manifest / 未写 keydir 快照 → keydir 快照在下一次 close / merge
  末尾 `write_keydir_snapshot()` 兜底（best-effort）

## 7. 恢复时序

`Cask::load_keydir_from_disk`（`src/cask/cask_recovery.cpp`）是 open 期
keydir 与索引统一恢复入口。完整流程：

```
open(dirname, opts, &registry)
  └─→ meta 读取（bitcask.meta v3 → MetaConfig：mode / 向量配置）
  └─→ keydir::open → KeyDir 实例
  └─→ load_recovery_snapshots                       # 见 §7.1
  └─→ load_keydir_from_disk → 调 fold_one(e) × N    # 见 §7.2
       └─→ ① 收尾 save_search_ckpt_paired           # 见 §7.3
```

### 7.1 `load_recovery_snapshots` —— 快照快路径

`src/cask/cask_recovery.cpp::Cask::load_recovery_snapshots` 流程：

1. **keydir 快照先载**：`keydir_->load_snapshot(dirname_/kKeydirSnapName)`。
   返回 `RecoverySnapshots::snap_wms`（per-file 字节水位）与
   `snap_loaded = true`。
2. **legacy 一次性迁移**：`!has_manifest && has_old_ckpt` 时
   `migrate_legacy_search_ckpt()`——读旧 `search.ckpt` 段集、改写为
   per-component 文件族 + `index.manifest` + 删旧文件（`.prev` / `.d<seq>`
   / `.vec` / `.qc8`）。
3. **manifest 读**：`bitcask::read_manifest(mpath)`。读不到 → 全量 fold。
4. **docmap 组件直载**：`index::load_docmap`（`include/bitcask/docmap_ckpt.hpp`）
   以 `entry.base_watermark` 校验 base，失败退 `.prev`；成功后链重放
   （`DocmapReplayHook` 透传 `kKeydirDelta` 段到宿主 keydir LWW put 与
   标量/fstats/字节水位更新）。
5. **插件 open**：经 `plugin::OpenContext` 注入器注入 `dir` / `host` /
   `committed_{base,chain}_watermark` / `committed_chain_seq`；所有路径
   （含 manifest 缺失 / 迁移失败的全量 fold 早退）都必须调用——零提示
   时插件自降级（`watermark 0` + rebase 置位 → 首次 flush 全量 base）。
6. **健康判据**：每组件 `plugin->watermark() == entry.chain_watermark` 即
   健康。所有组件健康 → 清 `ckpt_rebase_needed_`。
7. **快路径门**：`search_ok = all_components_ok && recovery.snap_loaded`。
   任一组件 `.prev` 回退 → 字节水位不可信 → 退全量 fold。

### 7.2 `load_keydir_from_disk` —— fold 主体

`src/cask/cask_recovery.cpp::Cask::load_keydir_from_disk` 流程：

1. `fileops::scan_dir(dirname_)` 列出 sealed data 文件（按 tstamp 升序）
2. `load_recovery_snapshots()` 拿 `snap_wms` 与 `snap_loaded` 标志
3. `fold_start(fid) = snap_loaded ? wm_of(fid) : 0`
4. 对每个文件 `fold_one(e)`：
   - `keydir_->increment_file_id_at_least(tstamp)`——保证后续 file_id 不撞
   - 有 hint 且无 search 模式且无 snap_loaded → 走 hint fold（单遍校验
     + fold，trailer CRC 不过时回调零次零污染）
   - 否则走 data fold：`DataFile::fold(..., start_offset = fold_start, ...)`
   - search 模式：每条 record 经 `decode_doc_value` → 攒批到
     `recover_batch`（`kRecoverBatch = 1024`）→ 满批调 `flush_recover`：
     批内 `tbb::parallel_for` 跑 `plugin::prepare`（分析并行），然后
     fold 序串行 apply `docmap_->put_doc` + `plugins_[pi]->on_put`——
     与活写路径 `reduce_index_entry` 同构
   - 墓碑前必 flush 攒批（保「文档↔墓碑」相对序）
   - search 模式墓碑重放：仅宿主 `docmap_->remove`（**不广播
     `on_delete`**——恢复期不扣减倒排统计，统计基线随 ckpt 恢复）
5. 调度：search 模式或单文件 → 串行 fold；纯 KV 库多文件 → 按硬件并发
   数并行 fold（`JoiningPool` RAII 防 `terminate`）
6. ① 收尾（post-recovery ckpt）：search 模式 + `recovered_docs >=
   1000` + 读写模式 → 调 `save_search_ckpt_paired` 把刚 fold 出的成果
   落盘。best-effort，失败仅降级下次启动速度

### 7.3 崩溃恢复时序图

```
open(dirname)
   │
   ├─ read meta (bitcask.meta v3)         ←── §1 MetaConfig
   │
   ├─ keydir_->load_snapshot
   │     └─ BCKS v2 校验失败 → snap_wms = {} (snap_loaded = false)
   │
   ├─ has_manifest ?                      ←── §5 manifest 协议
   │     ├─ yes: read_manifest
   │     │       └─ CRC 失败 → 全量 fold
   │     └─ no + has_old_ckpt: migrate_legacy_search_ckpt
   │             └─ 失败 → open_plugins(空 manifest) + 全量 fold
   │
   ├─ per-component load:
   │     ├─ docmap::load_docmap ──> DocmapReplayHook → keydir 半边
   │     └─ plugin->open (per-component chain state 注入)
   │
   ├─ watermark 对齐：
   │     all_components_ok && snap_loaded → fold_start = keydir_wm(fid)
   │     else                              → fold_start = 0
   │
   ├─ fold_one(e) × N                     ←── §7.2 fold 主体
   │     ├─ data fold (search mode 攒批并行 prepare + 串行 apply)
   │     └─ hint fold (纯 KV 无 search / 无 snapshot 时)
   │
   └─ ① post-recovery paired save        ←── §7.2 step 6
         (search mode + docs ≥ 1000 时立即回存 ckpt)
```

`fold_one` 内的 hint fold 路径仅在「无 search 模式且无快照」时启用
（`cask_recovery.cpp` 的 `fold_one`：e.has_hint && !search_on && !snap_loaded）；
其他情况走 data fold 兜底。torn-write 恢复：`last_valid_end <
actual_size` 且 `read_write && !merge_only` → `truncate_to(last_valid_end)`
（best-effort，best-effort 不影响正确性）。

## 8. legacy ckpt —— backward-compat fallback

`src/cask/legacy_ckpt.{hpp,cpp}`（S19-2）是 pre-S17 统一 `search.ckpt` 的
**load-only 读取器**，唯一生产用途是 `Cask::migrate_legacy_search_ckpt`。
读端用旧路径 `load` 把段集载回，分发到 `Index` / `TextPlugin` /
`VectorPlugin` 原语；链走读经 `sc::walk_chain`（unbounded，因为旧 ckpt
无 manifest 链长提示）。实现从 `SearchLayer::load_search_ckpt` +
`apply_delta_file` 平移，段分发与链重放语义逐字节一致。

写端（`save_search_ckpt` / `save_delta_ckpt`）不在生产侧——随
`SearchLayer` shim 降级为测试夹具（生成旧格式文件喂本读取器的迁移测试）。
旧格式支持整体退役时本模块删除。

`migrate_legacy_search_ckpt` 流程（`src/cask/cask_recovery.cpp`）：

1. `legacy_ckpt::load(old_ckpt, *docmap_, *text_, *vec_plugin_)` 读段集
2. 构造 `Manifest`——每组件 `base_watermark / chain_seq / chain_watermark`
   统一对齐到 `result.watermark`（旧 ckpt 不区分组件 base / 链）
3. 写 per-component 文件：
   - `index::save_docmap_base` 写 `docmap.ckpt` + `docmap_chain_` 镜像
   - `text_->save_component_base` 写 `bm25.ckpt`
   - `vec_plugin_->save_component_base` 写 `vec.ckpt`
4. `write_manifest(mpath, m)` 写 `index.manifest` 提交点
5. 删旧 `search.ckpt` / `.prev` / `.d<seq>` / `.vec` / `.qc8`

任一环节失败 → 返回 false，caller 退全量 fold（迁移失败安全降级）。

## 9. 关键不变量

1. **paired save 写序**：`global_base` 路径下 docmap base 落成后写
   `kv.keydir.ckpt`；delta 路径下 keydir 元数据内联进 `kKeydirDelta` 段。
   两路径均维持 `keydir_covered ≤ search_covered`。
2. **commit point 单向性**：`index.manifest` 是三组件 ckpt 唯一提交点；
   组件数据先于 manifest 落盘（`fdatasync` 屏障）——断电后 manifest 已
   提交但组件页丢失 → CRC 坏 → 整组件退全量 fold，不会出现「manifest
   OK 但组件坏」的混合态。
3. **回退完备性**：`search.ckpt` / 三组件 base 均带 `.prev`；链走读
   `chain_seq == 0` 表示零已提交 delta，孤儿 `.d1`（crash 在「先写 delta
   后提交 manifest」窗口）被有界模式忽略（与 pre-S20 逐字节一致），
   无界模式才会扫盘重放。
4. **fold 起点自门**：`fold_start = snap_loaded ? keydir_wm(fid) : 0`。
   `search_covered ≤ keydir_covered` 不变量保证 `[keydir_covered, end)`
   覆盖搜索所需；搜索各索引按自身 ord 水位自门丢弃重叠区，幂等安全。
5. **链校验三元组**：每个 `.d<seq>` 的 `kDeltaInfo` 段声明
   `(base_gen, prev_wm, seq)`，必须与基准世代 / 当前 coverage / 当前
   seq 一致——保证链是 `1..N` 连续、单调覆盖。
6. **崩溃任意点安全**：checkpoint 偏旧或部分写（tmp 未 rename）→ 对应
   块退回旧态 / 空态，watermark 下移，fold 补齐。无「门失败 → 全量
   fold」悬崖。
7. **墓碑前必 flush**：search 模式恢复 fold 时遇到 `kTombstone` → 先
   flush 攒批（保「文档↔墓碑」相对序），再 `docmap_->remove`。墓碑不
   广播 `on_delete`——历史语义保留（恢复期不扣减倒排统计，基线随
   ckpt 恢复）。

## 10. 关键写入点

- `Cask::close()`：`Cask::write_keydir_snapshot()` + paired save
- `Cask::merge()`：merge 末尾 `compact_chunks` + `force_ckpt_rebase` +
  paired save（`save_search_ckpt_paired(search_ckpt, wm, wms, {})`，
  merge 恒 rebase → 走 base + 全量快照）+ `write_keydir_snapshot` 兜底
- `Cask::checkpoint()`：手动 checkpoint API，paired save
- `Cask::maybe_submit_auto_checkpoint()`：周期自动触发（写者静止时
  RunFn；`auto_checkpoint_min_docs` 阈值），保持 pending + in-flight 互斥
- `Cask::recover` 末尾 ①：post-recovery paired save，把刚 fold 出的成
  果落盘（避免下次崩溃再白付一次 fold）

## 11. 验证矩阵

| 场景 | 行为 |
|------|------|
| clean close → reopen | 走 paired save 路径；fold 从 keydir 字节水位起 |
| kill -9 写入中 → reopen | manifest 仍指上一代；缺文件链走读断链 OK；fold 起点 0 |
| 单段 CRC 坏 | 仅该 type 走重建（损坏隔离）；其余段照常载入 |
| 整文件 footer 坏 | 回退 `.prev`；fold 尾巴追平到当前 |
| manifest CRC 坏 | 全量 fold（manifest 自身是 commit point） |
| legacy `search.ckpt` 单库 | 一次性迁移到 per-component + manifest；删旧文件 |
| 迁移失败 | `migrate_legacy_search_ckpt()` 返回 false → open_plugins 空提示 + 全量 fold |
| `deletion_rate_trigger` 兜底 | trigger 命中但 per-file 阶段无文件 → 全部非活跃文件入选 |
| v1 / v2 ckpt 文件 | read 双收；含 `kDocmapDeltaV2` 段的文件以 v2 写出，旧读端整文件拒收（降级安全） |
| active writer 文件出现在 fstats | `needs_merge` 排除；`fold` 不触碰 |

## 12. 相关文件索引

| 文件 | 内容 |
|------|------|
| `include/bitcask/search_checkpoint.hpp` | `SearchCheckpoint` 容器 + `CkptSectionType` 段类型 + kCkptVersion/kCkptVersion2 |
| `include/bitcask/ckpt_chain.hpp` | `walk_chain` / `remove_chain_files` 链走读与坍缩 |
| `include/bitcask/component_ckpt.hpp` | `ChainState` / `DeltaSaveResult` / `LoadResult` 公共类型 |
| `include/bitcask/index_manifest.hpp` | `Manifest` / `ManifestEntry` / `kManifestName` / `ComponentId` / `kManifestSize` + `write_manifest` / `read_manifest` |
| `include/bitcask/docmap_ckpt.hpp` | `save_docmap_base` / `save_docmap_delta` / `load_docmap` + `DocmapReplayHook` + `apply_delta_sections` 共享骨架 |
| `include/bitcask/keydir.hpp` + `src/keydir/keydir.cpp` | `save_snapshot` / `load_snapshot` / `serialize_meta_delta` / `apply_meta_delta` |
| `src/cask/cask_recovery.cpp` | `Cask::upgrade` / `load_keydir_from_disk` / `load_recovery_snapshots` / `migrate_legacy_search_ckpt` / `replay_delta_to_keydir` |
| `src/cask/legacy_ckpt.{hpp,cpp}` | pre-S17 单文件 ckpt load-only 读取器（迁移 fallback） |
| `src/cask/cask.cpp::save_checkpoint_paired` | paired save 唯一入口（脏掩码 / 决策 / commit） |
| `src/cask/cask.cpp::Cask::merge` | merge 末尾 paired save 触发点（V4 Pipeline Contract） |
| `src/cask/cask.cpp::Cask::write_keydir_snapshot` | keydir 快照落盘入口（成对） |
| `src/cask/cask_internal.hpp` | `kKeydirSnapName` / `kSearchCkptName` / `kDocmapCkptName` / `kBm25CkptName` / `kVecCkptName` 文件名常量 |
| `doc/format-zh.md` | ckpt 容器字节级格式 + 段类型详表 + v1/v2 文件版本 |
| `doc/merge-policy-zh.md` | merge 触发与执行；本设计的 merge 写点 |
