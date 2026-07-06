# Merge 触发策略

> 前置阅读：[`hnsw-lifecycle-zh.md`](hnsw-lifecycle-zh.md)（HNSW 图生命周期）、
> [`format-zh.md`](format-zh.md)（磁盘格式与 ckpt 容器）
> 状态：已实现

## 1. 概述

bitcask 的 merge **不会自动触发**——没有后台定时器，没有写入量阈值自动启动。
调用方必须显式调用 `Cask::needs_merge()` 检查，再调 `Cask::merge()` 执行。
策略模块本身是**纯函数**：根据 `KeyDir::info()` 的 fstats 快照与可调参数
算出一个 `Decision`，由 caller 喂给 `Merger` 真正干活。

merge 是物理回收 HNSW 死节点（已 delete 文档向量）的唯一时机。delete 时
向量节点留在 HNSW 图中，对应 ord 在 `Index` 里被标记为死，搜索通过
`is_live` 软过滤；merge 时 `VectorPlugin::rebuild` 重建无死节点图。

策略实现是 legacy `bitcask:run_merge_triggers/2` + `summarize/2` 的纯函数
移植。两段式决策（先 trigger 后选文件）与 legacy 一致——Riak 等下游有
运维脚本依赖具体触发行为，**不要破坏**。

## 2. 两段式决策

策略由 `bitcask::merge::decide` 一次性算出 `Decision{ needs_merge, files,
expired_files }`：

- **第一阶段（trigger）**：任一文件命中任一 trigger → 整次 merge 触发。
  都不命中 → `needs_merge = false`，连候选都不扫。
- **第二阶段（per-file threshold）**：trigger 命中后扫所有文件，任一 per-file
  阈值满足则该文件入选。
- **容量上限（`cap_size`）**：对入选文件按 on-disk 大小累加，超过
  `max_merge_size` 立即停（legacy「下一个文件会撑爆就不要它」语义）。

`Reason` 列表是给日志 / 测试用的诊断信息，不影响决策——per-file 阶段只看
命中了几条，不看哪条。

## 3. 触发阈值（trigger）

下列字段定义在 `PolicyOptions`（`include/bitcask/merge_policy.hpp`），任一
命中即触发整次 merge：

| 字段 | 类型 | 默认值 | 含义 |
|------|------|--------|------|
| `frag_merge_trigger` | `int` | `60` | 某文件碎片率 ≥ 60% |
| `dead_bytes_merge_trigger` | `std::uint64_t` | `512ULL * 1024ULL * 1024ULL`（512 MB） | 某文件死字节 ≥ 512 MB |
| `deletion_rate_trigger` | `int` | `0`（禁用） | 全局删除率 ≥ N% |
| 过期 | `expiry_secs` | `0`（禁用） | 文件全部 entry 过期 |

定义位置（`include/bitcask/merge_policy.hpp`）：

- `PolicyOptions::frag_merge_trigger` 默认值 `60`
- `PolicyOptions::dead_bytes_merge_trigger` 默认值
  `512ULL * 1024ULL * 1024ULL`
- `PolicyOptions::deletion_rate_trigger` 默认值 `0`（V4 新增，0 = 禁用）
- `PolicyOptions::expiry_secs` 默认值 `0`（0 = 禁用过期判断）

判定逻辑（`src/merge/merge_policy.cpp` 的 `any_trigger_fires`）：

- `f.fragmented >= opts.frag_merge_trigger` 触发
- `f.dead_bytes >= opts.dead_bytes_merge_trigger` 触发
- 过期触发：`trigger_cutoff > 0 && f.oldest_tstamp > 0 &&
  f.newest_tstamp < trigger_cutoff`
  - `trigger_cutoff = now_sec - expiry_secs - expiry_grace_time`（legacy 区分
    threshold 与 trigger：多减一个 grace 期，防止刚到 expiry 边界就频繁
    触发整次 merge）

碎片率 = `(1 - live_keys / total_keys) * 100`，整数算术：
`100 - live_keys * 100 / total_keys`（legacy 公式，避免浮点误差）。
死字节 = `total_bytes - live_bytes`（保底 0，避免下溢）。

`deletion_rate_trigger` 是 V4 新增的全局信号（不是 per-file）：
`dead_doc_rate >= deletion_rate_trigger` 即触发。`dead_doc_rate` 由
`Cask::needs_merge` 从 `Index` 算出
`(total_ords - live_docs) * 100 / total_ords`；策略函数本身不依赖
`Index` / `KeyDir`，信号由 caller 算好后作为 int 入参传入。

`deletion_rate_trigger == 0` 时此参数被忽略——旧行为完全保留。

## 4. Per-file 阈值

下列字段同样在 `PolicyOptions`，任一命中即把对应文件加入候选列表：

| 字段 | 类型 | 默认值 | 含义 |
|------|------|--------|------|
| `frag_threshold` | `int` | `40` | 碎片率 ≥ 40% |
| `dead_bytes_threshold` | `std::uint64_t` | `128ULL * 1024ULL * 1024ULL`（128 MB） | 死字节 ≥ 128 MB |
| `small_file_threshold` | `std::uint64_t` | `10ULL * 1024ULL * 1024ULL`（10 MB） | 文件 < 10 MB |
| 过期 | `expiry_secs` | `0` | 文件全部 entry 过期 |

定义位置（`include/bitcask/merge_policy.hpp`）：

- `PolicyOptions::frag_threshold` 默认值 `40`
- `PolicyOptions::dead_bytes_threshold` 默认值 `128ULL * 1024ULL * 1024ULL`
- `PolicyOptions::small_file_threshold` 默认值 `10ULL * 1024ULL * 1024ULL`
  （`0` 表示禁用该规则，legacy 用 atom `disabled` 表示禁用）

判定逻辑（`src/merge/merge_policy.cpp` 的 `per_file_reasons`）：

- `f.fragmented >= opts.frag_threshold`
- `f.dead_bytes >= opts.dead_bytes_threshold`
- `opts.small_file_threshold > 0 && f.total_bytes < opts.small_file_threshold`
- 过期：`threshold_cutoff > 0 && f.newest_tstamp < threshold_cutoff`
  - `threshold_cutoff = now_sec - expiry_secs`

## 5. `Reason` 类型

`Reason` 是 per-file 触发原因，给日志 / 测试用，定义于
`include/bitcask/merge_policy.hpp` 的 `Reason` 结构：

| `Kind` 枚举 | 触发条件 | `value` 单位 | `cutoff` |
|------------|----------|--------------|----------|
| `Reason::Kind::kFragmented` | 碎片率命中 `frag_threshold` | 百分比 | `0` |
| `Reason::Kind::kDeadBytes` | 死字节命中 `dead_bytes_threshold` | 字节 | `0` |
| `Reason::Kind::kSmallFile` | 文件小于 `small_file_threshold` | 字节 | `0` |
| `Reason::Kind::kDataExpired` | `newest_tstamp < threshold_cutoff` | tstamp（秒） | `threshold_cutoff` |

`decide` 内部用 `per_file_reasons` 给每个候选文件算 reason 列表。任一原因
命中就入选；若 `kDataExpired` 在内，额外压入 `Decision::expired_files`
（expired 是 `files` 的子集）。

## 6. `deletion_rate_trigger` 的兜底入选

V4 全局触发信号成立但 per-file 阶段没有文件通过阈值时（如所有文件都较新），
策略把所有非活跃文件（summary 全部）全部入选——否则触发信号成立了但
没文件可并，等于空转。代码路径在 `decide`：

```
if (d.files.empty() && opts.deletion_rate_trigger > 0 &&
    dead_doc_rate >= opts.deletion_rate_trigger) {
    d.files = summary;
    d.needs_merge = true;
}
```

`deletion_rate_trigger == 0` 时此分支永不入。

## 7. `cap_size` 容量上限

`cap_size`（`src/merge/merge_policy.cpp`）对 `decide` 选出的候选列表
应用 `max_merge_size` 上限：

- `max_merge_size == 0` → 不限，原样返回
- 按 on-disk 大小（`sizes[i]`）累加，超过上限的当前文件**不包含**（严格
  包含会令 cap 形同虚设）——legacy 语义
- 实际实现是严格 break：`if (acc + sizes[i] > max_merge_size) break;`
  （首个文件单独就超 cap 时同样 break、不入选）
- `files.size() != sizes.size()` → caller 传错，跳过 cap（不静默截断）

`max_merge_size` 在 `PolicyOptions` 默认 `0`（无上限）。定义位置
`include/bitcask/merge_policy.hpp` 的 `PolicyOptions::max_merge_size`。
`caller` 责任：通过 `stat` 拿到每个候选文件的 on-disk 大小，平行传入。

## 8. 决策流程

```
Cask::needs_merge(now_sec)
  └─→ keydir_->info()                        # 拿 fstats 快照
  └─→ 排除 active writer 文件 / merge_only 时排除 ≥ exclude_id
  └─→ merge::summarize(dirname, f) × N        # fstats → FileStatus
  └─→ docmap_->info() 算 dead_doc_rate        # V4 全局信号
  └─→ merge::decide(summary, opts, now_sec, dead_doc_rate)
        1. 算 expiry_cutoffs（threshold + trigger）
        2. any_trigger_fires 扫一遍 → 任一文件命中 trigger
           或 deletion_rate_trigger 命中（V4）？
             ── No ──→ { needs_merge = false, files = {} }    # 早退
        3. per_file_reasons 扫所有文件 → 非空 reason 列表即入选
        4. expired_files 收集含 kDataExpired 的子集
        5. deletion_rate_trigger 兜底：files 空则全入选
        6. 返回 Decision
  └─→ merge::cap_size(files, sizes, max_merge_size)   # caller 再套 cap
       → NeedsMerge { needs, files, expired_files }
```

ASCII 数据流：

```
fstats ──summarize──> FileStatus[]
                        │
                        ├──decide──> Decision
                        │              │
                        │              ├── cap_size ──> final files
                        │              │
                        │              └── expired_files
                        │
                        └── per_file_reasons (诊断 / 日志)
```

## 9. 线程模型

策略模块所有函数都是**纯函数**（无内部状态，无 I/O）：

- 可重入 / 线程安全：是。多线程可并发对同一/不同输入并发调用。
- 锁要求：无。
- 注意：输入的 `FileStatus` 列表通常来自 `KeyDir::info()`，由 caller 取到
  快照后再传入；策略模块不保证对「正在被并发修改的容器」的访问安全——
  caller 责任是先拿快照再算策略。

## 10. 数据结构

### 10.1 `PolicyOptions`

`include/bitcask/merge_policy.hpp` 完整字段：

```cpp
struct PolicyOptions {
    // ---- trigger（任一满足整体就要 merge）----
    int        frag_merge_trigger          = 60;
    std::uint64_t dead_bytes_merge_trigger = 512ULL * 1024ULL * 1024ULL;
    // V4 全局信号（0 = 禁用）
    int        deletion_rate_trigger       = 0;

    // ---- per-file 阈值（任一满足该文件入选）----
    int        frag_threshold              = 40;
    std::uint64_t dead_bytes_threshold     = 128ULL * 1024ULL * 1024ULL;
    std::uint64_t small_file_threshold     = 10ULL * 1024ULL * 1024ULL;

    // ---- 过期（0 禁用）----
    std::uint32_t expiry_secs              = 0;
    std::uint32_t expiry_grace_time        = 0;

    // ---- 输出体积上限（0 = 无上限）----
    std::uint64_t max_merge_size           = 0;
};
```

### 10.2 `FileStatus`

`src/merge/merge_policy.cpp::summarize` 从 `keydir::FStatsEntry` 算出。
等价 legacy `#file_status{}` 记录：

```cpp
struct FileStatus {
    std::uint32_t file_id;
    std::string   filename;
    int           fragmented;        // 0..100 百分比
    std::uint64_t dead_bytes;        // total_bytes - live_bytes
    std::uint64_t total_bytes;
    std::uint32_t oldest_tstamp;
    std::uint32_t newest_tstamp;
    std::uint64_t expiration_epoch;
};
```

`total_bytes == 0 && total_keys == 0`（空文件统计）legacy 隐式过滤掉——
`caller` 用 `present` 槽过滤。

### 10.3 `Decision`

```cpp
struct Decision {
    bool needs_merge = false;
    std::vector<FileStatus> files;          // 要并的候选，保持输入顺序
    std::vector<FileStatus> expired_files;  // 子集：因过期而入选
};
```

### 10.4 `Reason`

```cpp
struct Reason {
    enum class Kind {
        kFragmented,
        kDeadBytes,
        kSmallFile,
        kDataExpired,
    };
    Kind kind;
    std::uint64_t value = 0;          // kFragmented 是百分比，其它是字节
    std::uint64_t cutoff = 0;         // 仅 kDataExpired 用
};
```

## 11. caller 责任

策略模块不知道也不管下面这些事——由 `Cask::needs_merge` 实现：

- **拿 fstats 快照**：`keydir_->info()` 取出后做防御性拷贝
- **排除 active writer**：纯 KV 库 `active_file_id_`；`merge_only` 模式排除
  所有 `file_id >= merger_writer_active_id_`（防 writer 后面追加新文件）
- **算 `dead_doc_rate`**（V4）：`docmap_->info()` 得 `total_ords` 与
  `live_docs`，公式 `(total_ords - live_docs) * 100 / total_ords`（`total_ords == 0`
  时跳过，删率无意义）
- **统计 on-disk 大小**（cap 阶段）：`stat()` 候选文件路径，平行传入 `sizes`
- **调 cap_size**：`decide` 不内嵌 cap，由 caller 在 `decide` 之后
  显式调 `merge::cap_size`（也支持 caller 拿原始 `files` 干别的事）

## 12. 默认值变更建议

如需调整默认阈值，改 `PolicyOptions` 内联初始化即可——`opts_.policy`
（CaskOptions 内含字段）会带出，应用层只消费传进来的值。默认值历史上
与 `priv/bitcask.app.src` 对齐，保持同步是应用层的事。

## 13. 相关文件索引

| 文件 | 内容 |
|------|------|
| `include/bitcask/merge_policy.hpp` | `PolicyOptions` 默认值 + `Decision` / `FileStatus` / `Reason` 类型 |
| `src/merge/merge_policy.cpp` | `summarize` / `decide` / `per_file_reasons` / `cap_size` 实现 |
| `include/bitcask/merger.hpp` + `src/merge/merger.cpp` | `Merger` / `MergeRunner`（真正 fold + 重写） |
| `src/cask/cask.cpp` | `Cask::needs_merge`（caller 责任实现）+ `Cask::merge`（V4 Pipeline Contract） |
| `include/bitcask/keydir.hpp` | `KeyDir::info()` 拿 fstats 快照 |
| `doc/hnsw-lifecycle-zh.md` | HNSW 图完整生命周期（merge 中 `VectorPlugin::rebuild` 物理清死） |
| `doc/recovery-unified-checkpoint-design-zh.md` | 恢复 / checkpoint 设计（merge 末尾 paired save 流程） |
