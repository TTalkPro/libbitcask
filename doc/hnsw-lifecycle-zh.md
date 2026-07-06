# HNSW 图生命周期：构建、持久化与恢复

> 前置阅读：`hnsw-design-zh.md`（V3 HNSW 基础设计）、`hnsw-graph-construction-zh.md`（建图算法）、`hnsw-int8-only-design-zh.md`（int8-only 模式）、`int8-vnni-v4-zh.md`（V4.2 量化检索）
> 持有方：`bitcask::vec::VectorPlugin`（`include/bitcask/vector_plugin.hpp`，实现 `src/search/vector_plugin.cpp`）；底层图对象为 `bitcask::vec::HnswIndex`（`include/bitcask/hnsw.hpp`，实现 `src/vector/hnsw.cpp`）
> 状态：已实现。

## 1. 概述

HNSW 多层图在 `VectorPlugin` 这一层经历三个生命周期阶段，对应三个核心状态：

```
        未初始化              live                sealed               rebuild
          │                   │                    │                    │
   构造后未 open       接受 insert + search   close()/rebase 后      merge 收尾
   hnsw_ = nullptr    增量构建，链表账 hnsw_  原子换指针（只读挂起）    旁路重建
          │                   │                    │                    │
          └──── open() ──────►├──── flush(rebase) ─►├──── on_merge_commit ─►
                              │     (save_component_delta 或 base)  │     (run_serialized 投递 reducer)
                              └◄─────────── atomic<shared_ptr> 换指针 ────┘
```

具体而言：

1. **live（增量构建）**：`on_put` → `HnswIndex::insert` 逐点加入；`search` 多读者并发。
2. **sealed（持久化收尾）**：`close` 或 `flush` 触发 `save_component_base` 或 `save_component_delta` 落盘；此期间图对象本身仍可读，挂起新一轮写入或交付 `merge`。
3. **rebuild（全量重建）**：`on_merge_commit` 触发 `run_serialized` 投递 `VectorPlugin::rebuild` → `HnswIndex::clone_live` 做结构化活子图拷贝，原子换指针。

持久化与恢复走 `vec.ckpt`（头段嵌入 BVH2 v3）+ `.vec` payload（f32 字节流）+ `.qc8` payload（int8 码字）三件套；增量更新经 `.d<seq>` 链文件累积。

## 2. 状态机

### 2.1 未初始化 → live（`VectorPlugin` 构造）

`VectorPlugin::VectorPlugin` 构造函数（`src/search/vector_plugin.cpp`）：

```cpp
VectorPlugin::VectorPlugin(const VectorPluginConfig& config,
                           const bm25::DocTable& docs)
    : config_(config), docs_(docs) {
    if (config_.dim > 0) {
        HnswConfig hc;
        hc.dim = config_.dim;
        hc.metric = (config_.metric == meta::VectorMetric::kL2)
                        ? HnswMetric::kL2 : HnswMetric::kDot;
        hc.inmem_int8 = config_.inmem_int8;          // P5b 透传
        if (config_.hnsw_m > 0) hc.M = config_.hnsw_m;
        if (config_.hnsw_ef_construction > 0) {
            hc.ef_construction = config_.hnsw_ef_construction;
        }
        hnsw_.store(std::make_shared<HnswIndex>(hc),
                    std::memory_order_release);
    }
}
```

`hnsw_` 是 `std::atomic<std::shared_ptr<HnswIndex>>`（`vector_plugin.hpp` 的 `VectorPlugin` 私有区）——`live` 阶段初始 graph 句柄。dim=0 时不创建图（无向量配置的集合）。

### 2.2 live → live（增量构建）

`put_doc(vec)` 落盘后，事件 `on_put`（`VectorPlugin::on_put`，`vector_plugin.hpp`）进入 reducer 路径（`IndexPool` 单写者）：

```cpp
void VectorPlugin::on_put(const plugin::PutEvent& e, plugin::PreparedPtr) {
    if (e.doc && !e.doc->vec.empty()) {
        insert(e.ord, e.doc->vec);
    }
}
```

`VectorPlugin::insert`（`vector_plugin.cpp`）：

```cpp
void VectorPlugin::insert(std::uint64_t ord, std::span<const float> v) {
    auto hnsw = hnsw_.load(std::memory_order_acquire);
    if (!hnsw || v.size() != config_.dim) return;
    dirty_.store(true, std::memory_order_relaxed);
    // S14-4:窗口插入日志（fold 重叠区幂等：只在 ord ≥ delta_window_wm_ 时入账）
    if (ord >= delta_window_wm_) {
        delta_ords_.push_back(ord);
        delta_data_.insert(delta_data_.end(), v.begin(), v.end());
    }
    hnsw->insert(ord, v);                            // 单写者调用 HnswIndex::insert
}
```

**S14-4 窗口入账**：写入端归一化后，ord 全局单调。`delta_window_wm_` 由 `save_component_base`/`save_component_delta`/`load_component` 维护，重叠区已持久化则不再入账——fold 重放时 `HnswIndex::insert` 自带 `max_inserted_ord_` 水位幂等门（`hnsw.cpp` 内 `insert`），双层防护。

`on_delete` 是空实现——HNSW 软删经 `DocTable::is_live` 过滤（`search` 入口的 live callback），物理清理延迟到 merge。

### 2.3 live → sealed（持久化收尾）

两条触发路径：

1. **正常关闭**（`Cask::close` → `VectorPlugin::close`）：仅返回 `kOk`，实际落盘由前序 `flush` 完成。
2. **水位触发 flush**（`Cask::flush` → `VectorPlugin::flush`，`vector_plugin.cpp`）：

```cpp
plugin::FlushResult VectorPlugin::flush(const plugin::FlushRequest& req) {
    const bool cap_hit = config_.max_delta_chain > 0 &&
                         chain_.next_seq > config_.max_delta_chain;
    const bool want_base = req.force_rebase ||
                           rebase_needed_.load(std::memory_order_relaxed) ||
                           cap_hit;
    if (want_base) {
        save_component_base(dir_, req.watermark);
        ...
    } else if (!dirty() || delta_ords_.empty()) {
        dirty_.store(false, std::memory_order_relaxed);   // 空日志：no-op
        ...
    } else {
        save_component_delta(dir_, req.watermark);
        ...
    }
}
```

- **base 路径**：`save_component_base`（`vector_plugin.cpp`）写 `vec.ckpt` + `.vec` + `.qc8`，链坍缩（`remove_chain_files`），`delta_window_wm_` 推到当前水位。
- **delta 路径**：`save_component_delta`（`vector_plugin.cpp`）写 `.d<seq>` 链文件，含 `kDeltaInfo`（三元组 base_gen/chain_wm/seq）+ `kHnswDelta`（插入日志），成功才清 `delta_ords_`/`delta_data_`。
- **rebase**：`rebase_needed_` 在 `rebuild` 后置位，强制下一次 flush 走 base——重建后旧 delta 链语义不再成立。

`sealed` 实际含义：flush 返回后 `chain_` 已更新到新水位、`dirty_` 已清；图对象本身继续可读，写入由上层调度决定是否暂停（典型实现：close 后调用方 fail-fast 拒新写）。

### 2.4 sealed → rebuild（merge 物理清死）

`on_merge_commit`（`vector_plugin.cpp`）：

```cpp
void VectorPlugin::on_merge_commit(const plugin::MergeCommitEvent&) {
    if (!enabled()) return;
    if (host_) {
        host_->run_serialized([this] { rebuild(); });  // reducer 静止点
    } else {
        rebuild();
    }
}
```

`run_serialized` 把 lambda 投递到 `IndexPool` reducer 静止点执行，维持单写者约束。

`VectorPlugin::rebuild`（`vector_plugin.cpp`）：

```cpp
void VectorPlugin::rebuild() {
    auto old = hnsw_.load(std::memory_order_acquire);
    if (!old) return;
    dirty_.store(true, std::memory_order_relaxed);
    rebase_needed_.store(true, std::memory_order_relaxed);    // S18-6：自置 rebase
    auto fresh = old->clone_live(
        [this](std::uint64_t ord) { return docs_.is_live(ord); });
    hnsw_.store(std::move(fresh), std::memory_order_release);
}
```

**关键**：`clone_live` 是结构化活子图拷贝（详见 §3.3），不是从零重插。原子 `release` 换指针后旧图由在途读者 `shared_ptr` 续命，无锁回收。返回后查询即走新图（live 节点无死）。

### 2.5 状态汇总

| 状态 | `hnsw_` 句柄 | 写入 | 查询 | 落盘时机 |
|---|---|---|---|---|
| 未初始化 | `nullptr`（`dim=0` 时构造） | ❌ | ❌ | n/a |
| live | `shared_ptr<HnswIndex>` 指向当前图 | `on_put` 触发 insert | `search` 走图 | 增量为 delta；水位达上限或 rebase 触发 base |
| sealed | 同上，挂起新一轮写 | ❌（上层调度暂停） | ✅（旧图仍可读） | flush 已落 base/delta |
| rebuild | 旧图指针持有中 + 新图构造中 | ❌（reducer 单写者） | ✅（旧图） | rebuild 内部不写盘；结束后下次 flush 走 base |

## 3. 重建路径：`clone_live` 的 COW 语义

### 3.1 为什么不用「从零重插」

旧实现是遍历 `old` 图每个节点 → `fresh->insert(ord, old->node_vec(id))`。每点要做一次 `ef_construction` 宽的束搜索（默认 200）→ 1M 节点分钟级、阻塞 merge。

`HnswIndex::clone_live`（`hnsw.hpp` 声明，`hnsw.cpp` 实现）改为结构化拷贝：

- **保留原图拓扑**：层数与邻接结构原样搬过来，仅做 id 重映射 + 死邻过滤。
- **O(节点 + 边)** 的 memcpy 级拷贝，**无距离计算**。
- **int8-only 直接拷 qcodes/scale/sum**，消掉旧重插路径的反量化→再量化往返。

### 3.2 三遍 pass

**Pass 0 — id 重映射**：活节点按 id 序紧凑编号 = ord 序保持。

**Pass 1 — 节点数据 + 邻接块分配**：

```cpp
for (old_id = 0..n-1) {
    new_id = remap[old_id];
    if (new_id == kDead) continue;
    // 拷 vec（int8-only 跳过）→ qcodes 直拷 → ords/levels 拷 → 邻接块从 arena alloc
    ...
}
```

**Pass 2 — 邻接重映射 + 死邻过滤 + 一跳路径收缩**：

```cpp
for (each layer l of each node) {
    merged.clear();
    for (n in 旧邻居) {
        if (n >= n) continue;                            // 越界：跳过
        r = remap[n];
        if (r != kDead && r != new_id) merged.push_back(r);
    }
    if (merged.empty() && 旧邻居数 > 0) {
        // 一跳路径收缩：借道死邻的活邻居补边，去重、限 cap
        for (dead_n in 旧邻居, remap[dead_n] == kDead) {
            for (nn in dead_n 的同层邻居) {
                r2 = remap[nn];
                if (r2 != kDead && r2 != new_id && 不在 merged) merged.push_back(r2);
            }
        }
    }
    dst[0] = merged.size();
    for (i) dst[1+i] = merged[i];
}
```

**最终发布**（pass 2 完成后，函数返回前）：

```cpp
fresh->entry_meta_.store(
    (static_cast<std::uint64_t>(best_level + 1) << 32) | best_new_id,
    std::memory_order_release);
fresh->count_.store(nn, std::memory_order_release);
return fresh;                                          // 调用方原子换 hnsw_
```

**约束**（实现注释 `hnsw.cpp` 内 `clone_live`）：

- 调用方须为单写者（reducer）。旧图并发读者只读不冲突。
- 死邻过滤的极端情况下（某层邻居全死且一跳无补），个别节点该层出边为空——下一轮 merge 或重插自愈。
- int8-only 模式下 `qcodes/qscales/qsums` 直拷（无损），旧路径的反量化→再量化往返完全消失。

### 3.3 与原子换指针的关系

`VectorPlugin::rebuild` 持 `old` 的 `shared_ptr` 局部副本——保证 `clone_live` 期间旧图不被析构。`hnsw_.store(fresh, release)` 后，新查询 load 拿到 `fresh`，旧查询持有的 `old` 副本继续使用直至退出，无锁回收（控制块引用计数 atomic 增减）。

## 4. 持久化格式与链

### 4.1 三件套：`.ckpt` + `.vec` + `.qc8`

V7 起 HNSW 持久化为**三件套**（完整字节布局见 `format-zh.md`）：

| 文件 | 内容 | 写入入口 | 备注 |
|---|---|---|---|
| `vec.ckpt` | BVH2 v3 头段（含 `dim/metric/M/efc/seed/count/entry_meta/max_ord/payload_gen`） + 每节点 `ord/level/邻接`，末尾段内 crc32 | `HnswIndex::serialize`（`hnsw.cpp`） | 嵌入 `vec.ckpt` 的 `kHnsw` 段 |
| `vec.ckpt.vec` | BCVP f32 字节流（64B 头 + 页 CRC 表 v1 或 v2 追加） | `HnswIndex::save_vec_payload`（`hnsw.cpp`） | `inmem_int8` 模式不产生（无常驻 f32） |
| `vec.ckpt.qc8` | BCQ8 int8 码字（64B 头 + 记录区 `[qcodes int8[dim] \| qscale f32 \| qsum i32]`） | `HnswIndex::save_qc_payload`（`hnsw.cpp`） | v3 起码字外置；`needs_qcodes_` 为假时无 qc8 |

`save_component_base` 顺序（`vector_plugin.cpp`）：

1. `save_vec_payload(.vec)`：优先追加（S14-2，前缀不变契约），失败退全量重写。
2. `save_qc_payload(.qc8)`：同上结构。
3. `serialize` → `kHnsw` 段拼入 `vec.ckpt`。
4. `SearchCheckpoint::write(vec.ckpt, watermark, sections)`：`tmp + rename` 原子。
5. `remove_chain_files(vec.ckpt)`：链坍缩。
6. `chain_` 推进、`delta_window_wm_ = watermark`、`clear_delta_log()`、`dirty_ = false`。

### 4.2 链文件 `.d<seq>`

`save_component_delta` 写 `.d<seq>`，内容：

- `kDeltaInfo` 段：三元组 `base_gen | chain_wm | seq`，加载时校验链连续性。
- `kHnswDelta` 段：`count u64 | dim u16 | 每条 ord u64 + f32[dim]`，按插入序紧凑。

`VectorPlugin::apply_delta_log`（`vector_plugin.cpp`）：

```cpp
for (每条 (ord, vec)) {
    if (hnsw) hnsw->insert(ord, vec);    // insert 自带水位幂等（不写 delta 链）
}
```

**约束**：直插而非再入账——链内容本就已持久化，重放时不标脏、不入 delta 日志。

### 4.3 写入顺序契约（崩溃安全）

V7 写入顺序（`save_component_base`）：

1. **`.vec` 先**：通过 `pwrite + fdatasync` 落盘数据 → header 原地重写 → fdatasync。
2. **`.qc8` 同上结构**。
3. **`.ckpt` 后**：`tmp + rename` 原子发布。

**S14-2 前缀不变契约**：`.vec`/`.qc8` 追加只写 `offset ≥ 旧 count` 的尾区——文件里声称的 `[0, n)` 前缀在 torn append 下保持完好。配合「先 payload 后 ckpt 原子发布」顺序，崩溃后要么用旧 n（尾部垃圾被忽略）要么用新 n（数据已 fdatasync），方向恒安全。

**S14-8 payload 代号**：`BVH2 v3` 段头与 `.vec`/`.qc8` 头共同携带 `payload_gen`——rebuild 全量重写 payload 后若走 `.prev` 回退，旧图配新 payload（node id 已重映射）会被「前缀 `count ≥ n`」误收。代号不匹配即拒载（`gen == 0` 的 legacy 文件跳过校验）。

## 5. crash 后 `load_component` 恢复路径

`VectorPlugin::load_component`（`vector_plugin.cpp`）：

```cpp
LoadResult VectorPlugin::load_component(std::string_view dir,
                                       std::uint64_t expected_base_wm,
                                       std::uint32_t chain_seq) {
    auto lc = sc::SearchCheckpoint::read(fp);
    bool from_prev = false;
    if (lc && lc->watermark != expected_base_wm) lc.reset();
    if (!lc) {
        lc = sc::SearchCheckpoint::read(prev_path);
        if (lc && lc->watermark != expected_base_wm) lc.reset();
        if (lc) from_prev = true;
    }
    if (!lc) return fail();

    // kHnsw 段应用
    for (const auto& ls : lc->sections) {
        if (ls.type == kHnsw) load_graph_section(ls.payload, vec_path, qc_path);
    }

    // 链重放（.prev 回退 = 链不可信，跳过）
    if (segments_ok && !from_prev) {
        walk_chain(fp, base_gen=coverage, base_coverage=coverage,
                   chain_seq, unbounded=false, callback);
    }
    ...
}
```

**核心契约**：

1. **base 校验**：`vec.ckpt` 段头 `watermark` 必须等于 `expected_base_wm`（来自 host 的 keydir 快照）；不符读 `.prev`；两者都不符 → 整图拒绝。
2. **段级隔离**：单 `kHnsw` 段坏只重建向量索引，其余段照常载入；容器层 `all_segments_ok` 决策 fold 起始点（见 `recovery-unified-checkpoint-design-zh.md`）。
3. **链回放**：`from_prev` 表示回了 `.prev`，链不可信，跳过；否则按 `base_gen/chain_wm/seq` 三元组逐 `.d<seq>` 走 `walk_chain`，`kHnswDelta` 段经 `apply_delta_log` 直插（`HnswIndex::insert` 水位幂等）。
4. **payload 加载**：`load_graph_section` 调 `HnswIndex::deserialize`（段头） → `load_vec_payload(.vec)` → `load_qc_payload(.qc8)`。前两步失败则图整体拒载回退 fold；第三步在 `qc_pending_` 为真时执行（v3 自门）。

### 5.1 三种 fallback

| 情形 | 行为 | 后果 |
|---|---|---|
| `vec.ckpt` 健康 + 段 CRC 通过 + `.vec`/`.qc8` 前缀满足 + `payload_gen` 配对 | 快照加载（秒级） | 走 snapshot 路径 |
| `vec.ckpt` 缺失 / watermark 不符 / 段 CRC 失败 / payload 不匹配 | 拒载 + `rebase_needed_=true` | fold 起点水位由 host 决策（典型：本插件返回 0，让 BM25/DocMap 的 fold 起点同样回退到 0）；下一次写触发 rebase |
| `.prev` 回退 | 链不重放 | fold 起点同上；下一次 write 触发 base |

### 5.2 watermark 自门

V7 不再用 per-index 的 `hnsw_covers_next_ord()` 成对门，改为 `vec.ckpt` 组件段头部单个 **watermark**（= 保存时 `next_ord`）+ **全段 CRC 通过**（`ckpt::LoadResult.all_segments_ok`，定义于 `component_ckpt.hpp`）共同判定：

- 各索引（hnsw `insert` 丢 `ord ≤ max_inserted_ord_`、bm25 `add_doc` 丢 `ord ≤ 水位`）重放幂等。
- 详见 `recovery-unified-checkpoint-design-zh.md`。

## 6. 完整生命周期 ASCII 图

```
                       live（接受 insert + search）
                              ▲
                              │ HnswIndex::insert
                              │ (single writer, atomic publish)
  put_doc ──► on_put ──┐    [VectorPlugin::insert]    [delta_ords_ / delta_data_]
                       ├──────────────────────────────►
                       │
                       │ dirty_=true, rebase_needed_ 走攒账
                       ▼
              flush: save_component_delta  ◄──── 增量累积
                       │ (写 .d<seq> 链文件)
                       │ chain_ 推进, delta_window_wm_ 推进
                       │
                       │ rebase_needed_ / cap_hit / force_rebase
                       ▼
              flush: save_component_base  ◄──── sealed
                       │ (写 vec.ckpt + .vec + .qc8, 链坍缩)
                       │ chain_ = {wm, wm, 1}
                       ▼
   close() / fork-fork-process exit ──────────► [进程退出]

                       重新 open()  ──► load_component
                       │  base 校验 → 段应用 → payload 装载 → 链重放
                       │
                       │ 健康 → 快照加载（live 重启）
                       │ 失败 → fold 起点回 0（触发下一次 rebase）

   merge 收尾 ─► on_merge_commit ──► run_serialized ──► VectorPlugin::rebuild
                       │
                       ▼
              clone_live(is_live) ──► atomic<shared_ptr> 换指针
                       │
                       ▼
              rebuild 后脏位置位 → 下次 flush 走 base（rebase_needed_）
```

## 7. 关键符号索引

| 阶段 | 函数 / 字段 | 文件 |
|---|---|---|
| 图句柄 | `VectorPlugin::hnsw_`（`atomic<shared_ptr<HnswIndex>>`） | `include/bitcask/vector_plugin.hpp` 的 `VectorPlugin` 私有区 |
| 构造 + 初始图 | `VectorPlugin::VectorPlugin` | `src/search/vector_plugin.cpp` |
| 增量插入 | `VectorPlugin::on_put` / `VectorPlugin::insert` | `src/search/vector_plugin.cpp` |
| 增量查询 | `VectorPlugin::search` | `src/search/vector_plugin.cpp` |
| Flush 决策 | `VectorPlugin::flush` | `src/search/vector_plugin.cpp` |
| Base 落盘 | `VectorPlugin::save_component_base` | `src/search/vector_plugin.cpp` |
| Delta 落盘 | `VectorPlugin::save_component_delta` | `src/search/vector_plugin.cpp` |
| 序列化段 | `HnswIndex::serialize` / `HnswIndex::deserialize` | `src/vector/hnsw.cpp` |
| f32 payload | `HnswIndex::save_vec_payload` / `HnswIndex::load_vec_payload` | `src/vector/hnsw.cpp` |
| int8 payload | `HnswIndex::save_qc_payload` / `HnswIndex::load_qc_payload` | `src/vector/hnsw.cpp` |
| Delta 日志序列化 | `VectorPlugin::serialize_delta_log` / `VectorPlugin::apply_delta_log` | `src/search/vector_plugin.cpp` |
| 加载 + 链重放 | `VectorPlugin::load_component` | `src/search/vector_plugin.cpp` |
| Merge 重建 | `VectorPlugin::on_merge_commit` / `VectorPlugin::rebuild` | `src/search/vector_plugin.cpp` |
| 结构化活子图拷贝 | `HnswIndex::clone_live` | `src/vector/hnsw.cpp` |
| 链文件清理 | `sc::remove_chain_files` / `sc::walk_chain` | `src/search`（S20-2 R8/R2） |
| 链状态 | `VectorPlugin::chain_`（`ChainState`，定义于 `component_ckpt.hpp`） | `include/bitcask/vector_plugin.hpp` |
| 水位幂等 | `HnswIndex::insert` 内 `max_inserted_ord_` | `src/vector/hnsw.cpp` |
| 写者原子发布 | `HnswIndex::count_` / `HnswIndex::entry_meta_` | `include/bitcask/hnsw.hpp` |